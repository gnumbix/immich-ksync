#include "settings/LogTailWidget.h"

#include "core/Logging.h"

#include <KLocalizedString>

#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace immichksync {

namespace {
constexpr int kRefreshIntervalMs = 1000;
constexpr int kMaximumBlocks = 500;
} // namespace

LogTailWidget::LogTailWidget(QWidget *parent)
    : QWidget(parent)
    , m_view(new QPlainTextEdit(this))
    , m_timer(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(kMaximumBlocks);
    m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_view->setPlaceholderText(i18n("Log lines appear here as the app works."));
    m_view->setMinimumHeight(140);
    layout->addWidget(m_view);

    m_timer->setInterval(kRefreshIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &LogTailWidget::refresh);
}

void LogTailWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
    m_timer->start();
}

void LogTailWidget::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    QWidget::hideEvent(event);
}

void LogTailWidget::refresh()
{
    const QVector<LogEntry> entries = LogSink::instance().entries();
    if (entries.isEmpty()) {
        return;
    }

    // On first show, jump straight to the tail rather than replaying the whole ring.
    if (!m_primed) {
        m_primed = true;
        const int start = std::max(0, static_cast<int>(entries.size()) - kMaximumBlocks);
        for (int i = start; i < entries.size(); ++i) {
            const LogEntry &entry = entries.at(i);
            m_view->appendPlainText(QStringLiteral("%1  [%2] %3")
                                        .arg(entry.date.toString(QStringLiteral("HH:mm:ss")),
                                             rawName(entry.category),
                                             entry.message));
            m_lastSeenId = entry.id;
        }
    } else {
        for (const LogEntry &entry : entries) {
            if (entry.id <= m_lastSeenId) {
                continue;
            }
            m_view->appendPlainText(QStringLiteral("%1  [%2] %3")
                                        .arg(entry.date.toString(QStringLiteral("HH:mm:ss")),
                                             rawName(entry.category),
                                             entry.message));
            m_lastSeenId = entry.id;
        }
    }

    m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
}

} // namespace immichksync
