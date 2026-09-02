#include "settings/SettingsWindow.h"

#include "app/AppEnvironment.h"
#include "app/AppInfo.h"
#include "settings/AdvancedSettingsTab.h"
#include "settings/AlbumsSettingsTab.h"
#include "settings/FolderSettingsTab.h"
#include "settings/ServerSettingsTab.h"
#include "settings/SettingsWidgets.h"

#include <KLocalizedString>
#include <KWindowSystem>

#include <QIcon>
#include <QTabWidget>
#include <QVBoxLayout>

namespace immichksync {

SettingsWindow::SettingsWindow(AppEnvironment *environment, QWidget *parent)
    : QDialog(parent)
    , m_environment(environment)
    , m_tabs(new QTabWidget(this))
{
    setWindowTitle(i18n("%1 Settings", AppInfo::displayName()));
    setWindowIcon(QIcon::fromTheme(AppInfo::applicationId()));
    resize(680, 560);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs);

    // Every tab scrolls: the Server and Advanced tabs are taller than a small screen,
    // and a control hidden below the fold may as well not exist.
    m_tabs->addTab(makeScrollablePage(new ServerSettingsTab(m_environment)),
                   QIcon::fromTheme(QStringLiteral("network-server")),
                   i18n("Server"));
    m_tabs->addTab(makeScrollablePage(new FolderSettingsTab(m_environment)),
                   QIcon::fromTheme(QStringLiteral("folder")),
                   i18n("Folder"));
    m_tabs->addTab(new AlbumsSettingsTab(m_environment),
                   QIcon::fromTheme(QStringLiteral("view-media-album-cover")),
                   i18n("Albums"));
    m_tabs->addTab(makeScrollablePage(new AdvancedSettingsTab(m_environment)),
                   QIcon::fromTheme(QStringLiteral("configure")),
                   i18n("Advanced"));
}

void SettingsWindow::showAndRaise()
{
    show();
    raise();
    // On Wayland a plain raise() is advisory; KWindowSystem asks the compositor
    // properly, which is what makes the tray's Settings item actually work.
    KWindowSystem::updateStartupId(windowHandle());
    KWindowSystem::activateWindow(windowHandle());
    activateWindow();
}

} // namespace immichksync
