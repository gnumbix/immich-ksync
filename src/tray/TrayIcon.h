#pragma once

#include <QObject>

class KStatusNotifierItem;
class QAction;
class QMenu;

namespace immichksync {

class AppEnvironment;

/// The system tray item and its menu — the whole interface, apart from Settings.
///
/// A status line, then the three commands the app is specified to offer:
/// Resume/Pause, Settings and Quit.
class TrayIcon : public QObject {
    Q_OBJECT

public:
    explicit TrayIcon(AppEnvironment *environment, QObject *parent = nullptr);

Q_SIGNALS:
    void settingsRequested();
    void quitRequested();

private Q_SLOTS:
    void refresh();

private:
    /// A second line, only when it adds something the first does not already say.
    QString secondaryStatusLine() const;

    AppEnvironment *m_environment;
    KStatusNotifierItem *m_item;
    QMenu *m_menu;
    QAction *m_statusAction;
    QAction *m_detailAction;
    QAction *m_pauseAction;
};

} // namespace immichksync
