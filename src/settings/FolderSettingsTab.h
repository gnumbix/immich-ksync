#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

namespace immichksync {

class AppEnvironment;
class InlineResult;

/// Where the album folders live, with a plain-language map of what the app writes to
/// disk.
class FolderSettingsTab : public QWidget {
    Q_OBJECT

public:
    explicit FolderSettingsTab(AppEnvironment *environment, QWidget *parent = nullptr);

private Q_SLOTS:
    void chooseFolder();
    void refresh();

private:
    /// A small, literal picture of what the app writes to disk. Cheaper to read than a
    /// paragraph describing the same thing.
    QWidget *buildLayoutDiagram();

    AppEnvironment *m_environment;
    QLabel *m_location = nullptr;
    QLabel *m_capacity = nullptr;
    QPushButton *m_open = nullptr;
    InlineResult *m_warning = nullptr;
};

} // namespace immichksync
