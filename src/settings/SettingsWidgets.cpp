#include "settings/SettingsWidgets.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QScrollArea>
#include <QVBoxLayout>

namespace immichksync {

SettingsSection::SettingsSection(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *box = new QGroupBox(title, this);
    m_layout = new QVBoxLayout(box);
    m_form = new QFormLayout();
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_layout->addLayout(m_form);
    outer->addWidget(box);

    m_footer = new QLabel(this);
    m_footer->setWordWrap(true);
    m_footer->setTextFormat(Qt::PlainText);
    // Smaller and dimmer than the controls: this is explanation, not instruction.
    QFont font = m_footer->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    m_footer->setFont(font);
    m_footer->setEnabled(false);
    m_footer->hide();
    outer->addWidget(m_footer);
}

void SettingsSection::addWidget(QWidget *widget)
{
    m_layout->addWidget(widget);
}

void SettingsSection::setFooter(const QString &text)
{
    m_footer->setText(text);
    m_footer->setVisible(!text.isEmpty());
}

InlineResult::InlineResult(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_icon = new QLabel(this);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(m_icon, 0, Qt::AlignTop);
    layout->addWidget(m_text, 1);
    hide();
}

void InlineResult::show(Kind kind, const QString &message)
{
    QString iconName;
    switch (kind) {
    case Kind::Success: iconName = QStringLiteral("dialog-ok"); break;
    case Kind::Failure: iconName = QStringLiteral("dialog-error"); break;
    case Kind::Info: iconName = QStringLiteral("dialog-information"); break;
    }
    m_icon->setPixmap(QIcon::fromTheme(iconName).pixmap(16, 16));
    m_text->setText(message);
    QWidget::show();
}

void InlineResult::clear()
{
    m_text->clear();
    hide();
}

QWidget *makeScrollablePage(QWidget *content)
{
    auto *area = new QScrollArea();
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}

} // namespace immichksync
