#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QSpinBox;
class QVBoxLayout;

namespace immichksync {

class AppEnvironment;
class InlineResult;

/// Intervals, concurrency, the safety threshold, open-at-login, and a live log.
class AdvancedSettingsTab : public QWidget {
    Q_OBJECT

public:
    explicit AdvancedSettingsTab(AppEnvironment *environment, QWidget *parent = nullptr);

private Q_SLOTS:
    void confirmReset();
    void refreshSummary();

private:
    void buildScheduleSection(QVBoxLayout *layout);
    void buildTransferSection(QVBoxLayout *layout);
    void buildSafetySection(QVBoxLayout *layout);
    void buildStartupSection(QVBoxLayout *layout);
    void buildDiagnosticsSection(QVBoxLayout *layout);
    void buildAboutSection(QVBoxLayout *layout);
    void buildResetSection(QVBoxLayout *layout);

    /// An interval picker whose options are the handful of values that make sense,
    /// rather than a free-text number of seconds.
    QComboBox *makeIntervalPicker(const QList<int> &optionsSeconds, int current);
    static QString describeInterval(int seconds);

    AppEnvironment *m_environment;
    QCheckBox *m_launchAtLogin = nullptr;
    InlineResult *m_launchAtLoginError = nullptr;
    QLabel *m_ratioLabel = nullptr;
    QLabel *m_lastSync = nullptr;
};

} // namespace immichksync
