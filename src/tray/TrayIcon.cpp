#include "tray/TrayIcon.h"

#include "app/AppEnvironment.h"
#include "app/AppInfo.h"

#include <KLocalizedString>
#include <KStatusNotifierItem>

#include <QAction>
#include <QDateTime>
#include <QMenu>

namespace immichksync {

TrayIcon::TrayIcon(AppEnvironment *environment, QObject *parent)
    : QObject(parent)
    , m_environment(environment)
    , m_item(new KStatusNotifierItem(AppInfo::applicationId(), this))
    , m_menu(new QMenu())
{
    m_item->setCategory(KStatusNotifierItem::ApplicationStatus);
    m_item->setStatus(KStatusNotifierItem::Active);
    m_item->setTitle(AppInfo::displayName());
    // An agent with no main window: activating the tray icon must open Settings rather
    // than try to restore a window that was never there.
    m_item->setStandardActionsEnabled(false);

    m_statusAction = m_menu->addAction(QString());
    m_statusAction->setEnabled(false);
    m_detailAction = m_menu->addAction(QString());
    m_detailAction->setEnabled(false);
    m_menu->addSeparator();

    m_pauseAction = m_menu->addAction(QString());
    connect(m_pauseAction, &QAction::triggered, this, [this]() { m_environment->togglePause(); });

    QAction *syncNow = m_menu->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                         i18n("Sync Now"));
    connect(syncNow, &QAction::triggered, this, [this]() { m_environment->syncNow(); });

    QAction *settings = m_menu->addAction(QIcon::fromTheme(QStringLiteral("configure")),
                                          i18n("Settings…"));
    connect(settings, &QAction::triggered, this, &TrayIcon::settingsRequested);

    m_menu->addSeparator();
    QAction *quit = m_menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                      i18n("Quit ImmichKSync"));
    connect(quit, &QAction::triggered, this, &TrayIcon::quitRequested);

    m_item->setContextMenu(m_menu);
    connect(m_item, &KStatusNotifierItem::activateRequested, this, [this](bool active, const QPoint &) {
        if (active) {
            Q_EMIT settingsRequested();
        }
    });

    connect(m_environment->status(), &SyncStatusModel::changed, this, &TrayIcon::refresh);
    connect(m_environment, &AppEnvironment::albumsChanged, this, &TrayIcon::refresh);
    refresh();
}

void TrayIcon::refresh()
{
    SyncStatusModel *status = m_environment->status();

    m_item->setIconByName(status->trayIconName());
    m_item->setToolTip(status->trayIconName(),
                       AppInfo::displayName(),
                       status->menuStatusLine());
    // NeedsAttention makes the item stand out in the tray, which is exactly what a
    // safety hold or a failed sign-in warrants and nothing else does.
    m_item->setStatus(status->needsAttention() ? KStatusNotifierItem::NeedsAttention
                                               : KStatusNotifierItem::Active);

    m_statusAction->setText(status->menuStatusLine());

    const QString detail = secondaryStatusLine();
    m_detailAction->setText(detail);
    m_detailAction->setVisible(!detail.isEmpty());

    m_pauseAction->setText(m_environment->isPaused() ? i18n("Resume") : i18n("Pause"));
    m_pauseAction->setIcon(QIcon::fromTheme(m_environment->isPaused()
                                                ? QStringLiteral("media-playback-start")
                                                : QStringLiteral("media-playback-pause")));
    m_pauseAction->setEnabled(m_environment->fatalStartupError().isEmpty());
}

QString TrayIcon::secondaryStatusLine() const
{
    if (!m_environment->fatalStartupError().isEmpty()) {
        return i18n("Database unavailable: %1", m_environment->fatalStartupError());
    }

    SyncStatusModel *status = m_environment->status();
    switch (status->state().kind) {
    case SyncState::Kind::Working:
    case SyncState::Kind::Preparing:
    case SyncState::Kind::NotConfigured:
        // The first line already says what is happening.
        return {};
    default:
        break;
    }

    const auto summary = status->lastSummary();
    if (!summary || !summary->finishedAt.isValid()) {
        return {};
    }
    const qint64 secondsAgo = summary->finishedAt.secsTo(QDateTime::currentDateTimeUtc());
    QString relative;
    if (secondsAgo < 60) {
        relative = i18n("just now");
    } else if (secondsAgo < 3600) {
        relative = i18np("%1 minute ago", "%1 minutes ago", secondsAgo / 60);
    } else if (secondsAgo < 86400) {
        relative = i18np("%1 hour ago", "%1 hours ago", secondsAgo / 3600);
    } else {
        relative = i18np("%1 day ago", "%1 days ago", secondsAgo / 86400);
    }
    return i18n("Last sync %1 — %2", relative, summary->headline());
}

} // namespace immichksync
