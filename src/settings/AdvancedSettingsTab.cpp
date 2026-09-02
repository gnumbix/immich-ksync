#include "settings/AdvancedSettingsTab.h"

#include "app/AppEnvironment.h"
#include "app/AppInfo.h"
#include "settings/LogTailWidget.h"
#include "settings/SettingsWidgets.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace immichksync {

AdvancedSettingsTab::AdvancedSettingsTab(AppEnvironment *environment, QWidget *parent)
    : QWidget(parent)
    , m_environment(environment)
{
    auto *layout = new QVBoxLayout(this);
    buildScheduleSection(layout);
    buildTransferSection(layout);
    buildSafetySection(layout);
    buildStartupSection(layout);
    buildDiagnosticsSection(layout);
    buildAboutSection(layout);
    buildResetSection(layout);
    layout->addStretch();

    connect(m_environment->status(),
            &SyncStatusModel::changed,
            this,
            &AdvancedSettingsTab::refreshSummary);
    refreshSummary();
}

QString AdvancedSettingsTab::describeInterval(int seconds)
{
    if (seconds < 3600) {
        return i18np("%1 minute", "%1 minutes", seconds / 60);
    }
    if (seconds < 86400) {
        return i18np("%1 hour", "%1 hours", seconds / 3600);
    }
    return i18np("%1 day", "%1 days", seconds / 86400);
}

QComboBox *AdvancedSettingsTab::makeIntervalPicker(const QList<int> &optionsSeconds, int current)
{
    auto *picker = new QComboBox(this);
    for (const int seconds : optionsSeconds) {
        picker->addItem(describeInterval(seconds), seconds);
    }
    const int index = picker->findData(current);
    if (index >= 0) {
        picker->setCurrentIndex(index);
    } else {
        // A value set by hand in the config file must still be shown rather than
        // silently replaced by the nearest option.
        picker->addItem(describeInterval(current), current);
        picker->setCurrentIndex(picker->count() - 1);
    }
    return picker;
}

void AdvancedSettingsTab::buildScheduleSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Schedule"), this);
    section->setFooter(i18n("Changes inside the sync folder are noticed immediately; the timer is "
                            "what catches changes made on the server. A full re-read of every "
                            "album runs on the deep-scan interval as a backstop."));

    Preferences *preferences = m_environment->preferences();

    auto *interval = makeIntervalPicker({60, 300, 900, 1800, 3600},
                                        preferences->syncIntervalSeconds());
    connect(interval, &QComboBox::currentIndexChanged, this, [this, interval](int) {
        m_environment->preferences()->setSyncIntervalSeconds(interval->currentData().toInt());
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("Check for changes:"), interval);

    auto *deepScan = makeIntervalPicker({1800, 3600, 21600, 86400},
                                        preferences->deepScanIntervalSeconds());
    connect(deepScan, &QComboBox::currentIndexChanged, this, [this, deepScan](int) {
        m_environment->preferences()->setDeepScanIntervalSeconds(deepScan->currentData().toInt());
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("Full album re-scan:"), deepScan);

    layout->addWidget(section);
}

void AdvancedSettingsTab::buildTransferSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Transfers"), this);
    section->setFooter(i18n("Higher concurrency finishes large batches sooner but puts more load "
                            "on the server and your connection."));

    Preferences *preferences = m_environment->preferences();

    auto *downloads = new QSpinBox(this);
    downloads->setRange(1, 16);
    downloads->setValue(preferences->downloadConcurrency());
    connect(downloads, &QSpinBox::valueChanged, this, [this](int value) {
        m_environment->preferences()->setDownloadConcurrency(value);
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("Simultaneous downloads:"), downloads);

    auto *uploads = new QSpinBox(this);
    uploads->setRange(1, 16);
    uploads->setValue(preferences->uploadConcurrency());
    connect(uploads, &QSpinBox::valueChanged, this, [this](int value) {
        m_environment->preferences()->setUploadConcurrency(value);
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("Simultaneous uploads:"), uploads);

    auto *settle = new QSpinBox(this);
    settle->setRange(1, 120);
    settle->setSuffix(i18n(" seconds"));
    settle->setValue(preferences->settleWindowSeconds());
    connect(settle, &QSpinBox::valueChanged, this, [this](int value) {
        m_environment->preferences()->setSettleWindowSeconds(value);
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("Wait after a file changes:"), settle);

    layout->addWidget(section);
}

void AdvancedSettingsTab::buildSafetySection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Safety"), this);
    section->setFooter(i18n("If a single cycle would remove more than this share of an album, "
                            "nothing is removed and the album waits for your confirmation on the "
                            "Albums tab. This is what protects you from an unmounted drive being "
                            "read as a mass deletion."));

    Preferences *preferences = m_environment->preferences();

    auto *ratioRow = new QWidget(this);
    auto *ratioLayout = new QHBoxLayout(ratioRow);
    ratioLayout->setContentsMargins(0, 0, 0, 0);

    auto *slider = new QSlider(Qt::Horizontal, ratioRow);
    slider->setRange(5, 100);
    slider->setSingleStep(5);
    slider->setPageStep(5);
    slider->setValue(static_cast<int>(preferences->removalRatioThreshold() * 100));
    ratioLayout->addWidget(slider, 1);

    m_ratioLabel = new QLabel(ratioRow);
    m_ratioLabel->setMinimumWidth(48);
    m_ratioLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_ratioLabel->setText(QStringLiteral("%1%").arg(slider->value()));
    ratioLayout->addWidget(m_ratioLabel);

    connect(slider, &QSlider::valueChanged, this, [this](int value) {
        m_ratioLabel->setText(QStringLiteral("%1%").arg(value));
        m_environment->preferences()->setRemovalRatioThreshold(value / 100.0);
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("Hold above:"), ratioRow);

    auto *minimum = new QSpinBox(this);
    minimum->setRange(0, 500);
    minimum->setSingleStep(5);
    minimum->setValue(preferences->minimumRemovalsBeforeGating());
    connect(minimum, &QSpinBox::valueChanged, this, [this](int value) {
        m_environment->preferences()->setMinimumRemovalsBeforeGating(value);
        m_environment->reconfigureEngine();
    });
    section->form()->addRow(i18n("…but never below:"), minimum);

    layout->addWidget(section);
}

void AdvancedSettingsTab::buildStartupSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Startup"), this);

    m_launchAtLogin = new QCheckBox(i18n("Open at login"), this);
    m_launchAtLogin->setChecked(m_environment->launchesAtLogin());
    connect(m_launchAtLogin, &QCheckBox::toggled, this, [this](bool checked) {
        const QString error = m_environment->setLaunchesAtLogin(checked);
        if (error.isEmpty()) {
            m_launchAtLoginError->clear();
        } else {
            m_launchAtLoginError->show(InlineResult::Kind::Failure, error);
        }
        // Report what actually happened, not what was asked for.
        QSignalBlocker blocker(m_launchAtLogin);
        m_launchAtLogin->setChecked(m_environment->launchesAtLogin());
    });
    section->addWidget(m_launchAtLogin);

    m_launchAtLoginError = new InlineResult(this);
    section->addWidget(m_launchAtLoginError);

    layout->addWidget(section);
}

void AdvancedSettingsTab::buildDiagnosticsSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Diagnostics"), this);

    auto *level = new QComboBox(this);
    for (const LogLevel value : {LogLevel::Debug,
                                 LogLevel::Info,
                                 LogLevel::Notice,
                                 LogLevel::Warning,
                                 LogLevel::Error}) {
        level->addItem(displayName(value), static_cast<int>(value));
    }
    level->setCurrentIndex(level->findData(static_cast<int>(m_environment->preferences()->logLevel())));
    connect(level, &QComboBox::currentIndexChanged, this, [this, level](int) {
        m_environment->preferences()->setLogLevel(
            static_cast<LogLevel>(level->currentData().toInt()));
    });
    section->form()->addRow(i18n("Log level:"), level);

    m_lastSync = new QLabel(this);
    m_lastSync->setEnabled(false);
    section->form()->addRow(i18n("Last sync:"), m_lastSync);

    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *openLogs = new QPushButton(i18n("Open Log Folder"), row);
    connect(openLogs, &QPushButton::clicked, this, [this]() { m_environment->openLogFolder(); });
    rowLayout->addWidget(openLogs);
    auto *syncNow = new QPushButton(i18n("Sync Now"), row);
    connect(syncNow, &QPushButton::clicked, this, [this]() { m_environment->syncNow(); });
    rowLayout->addWidget(syncNow);
    rowLayout->addStretch();
    section->addWidget(row);

    section->addWidget(new LogTailWidget(this));
    layout->addWidget(section);
}

void AdvancedSettingsTab::buildAboutSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("About"), this);

    auto *version = new QLabel(AppInfo::versionDescription(), this);
    version->setEnabled(false);
    section->form()->addRow(i18n("Version:"), version);

    auto *licence = new QLabel(QStringLiteral("AGPL-3.0"), this);
    licence->setEnabled(false);
    section->form()->addRow(i18n("Licence:"), licence);

    auto *links = new QLabel(this);
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);
    links->setWordWrap(true);
    // The AGPL expects the corresponding source to be reachable by anyone running the
    // program, so the link lives in the app itself rather than only in the README.
    links->setText(QStringLiteral("<a href='%1'>%2</a> · <a href='%3'>%4</a><br/>"
                                  "<small>%5</small>")
                       .arg(AppInfo::repositoryUrl().toString(),
                            i18n("Source code"),
                            AppInfo::licenceUrl().toString(),
                            i18n("Licence text"),
                            i18n("Not affiliated with the Immich project.")));
    section->addWidget(links);

    layout->addWidget(section);
}

void AdvancedSettingsTab::buildResetSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Reset"), this);
    section->setFooter(i18n("Forgets what has been synced so far and rediscovers everything on "
                            "the next cycle. Your files and your Immich library are not touched."));

    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *reset = new QPushButton(i18n("Reset Local Sync State…"), row);
    connect(reset, &QPushButton::clicked, this, &AdvancedSettingsTab::confirmReset);
    rowLayout->addWidget(reset);
    rowLayout->addStretch();
    section->addWidget(row);

    layout->addWidget(section);
}

void AdvancedSettingsTab::confirmReset()
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(i18n("Reset the local sync state?"));
    box.setText(i18n("Reset the local sync state?"));
    box.setInformativeText(i18n("The next sync will re-examine every album and every file. "
                                "Nothing is uploaded, downloaded or deleted as a result of the "
                                "reset itself."));
    QPushButton *resetButton = box.addButton(i18n("Reset"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == resetButton) {
        m_environment->resetLocalState();
    }
}

void AdvancedSettingsTab::refreshSummary()
{
    const auto summary = m_environment->status()->lastSummary();
    if (!summary || !summary->finishedAt.isValid()) {
        m_lastSync->setText(QStringLiteral("—"));
        return;
    }
    m_lastSync->setText(i18n("%1 in %2s",
                             summary->headline(),
                             QString::number(summary->durationSeconds, 'f', 1)));
}

} // namespace immichksync
