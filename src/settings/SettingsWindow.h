#pragma once

#include <QDialog>

class QTabWidget;

namespace immichksync {

class AppEnvironment;

/// Server · Folder · Albums · Advanced. The only window the app has.
class SettingsWindow : public QDialog {
    Q_OBJECT

public:
    explicit SettingsWindow(AppEnvironment *environment, QWidget *parent = nullptr);

    /// Brings the window forward, creating nothing: an agent with no dock icon would
    /// otherwise open it behind whatever the user is looking at.
    void showAndRaise();

    /// Test seam: lets the render tests reach each tab without driving the tab bar.
    QTabWidget *tabs() { return m_tabs; }

private:
    AppEnvironment *m_environment;
    QTabWidget *m_tabs;
};

} // namespace immichksync
