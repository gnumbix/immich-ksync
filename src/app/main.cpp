#include "app/AppEnvironment.h"
#include "app/AppInfo.h"
#include "core/Logging.h"
#include "settings/SettingsWindow.h"
#include "tray/TrayIcon.h"

#include <KAboutData>
#include <KDBusService>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

using namespace immichksync;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // A tray agent has no main window: closing Settings must not end the process.
    QApplication::setQuitOnLastWindowClosed(false);

    KLocalizedString::setApplicationDomain(QByteArrayLiteral("immichksync"));

    // KAboutLicense has no AGPL constant; byKeyword resolves the SPDX identifier, and
    // the licence is set explicitly below so the About box names it correctly either
    // way. The licence matters here — it is the one Immich itself uses.
    KAboutData about(QStringLiteral("immichksync"),
                     AppInfo::displayName(),
                     AppInfo::version(),
                     i18n("Keeps a folder and your Immich albums in continuous two-way agreement."),
                     KAboutLicense::Custom,
                     i18n("Copyright © 2026 GnumBix"),
                     QString(),
                     AppInfo::repositoryUrl().toString());
    about.setLicenseText(
        i18n("GNU Affero General Public License, version 3 — the licence Immich itself uses.\n\n"
             "The full text is at https://www.gnu.org/licenses/agpl-3.0.html, and the "
             "corresponding source for this program is at %1.",
             AppInfo::repositoryUrl().toString()));
    about.setDesktopFileName(AppInfo::applicationId());
    KAboutData::setApplicationData(about);
    // KAboutData derives the organisation domain from the homepage URL, which would
    // make KDBusService claim `com.github.immichksync`. The desktop file declares
    // `com.gnumbix.immichksync`, and activation only works if the two agree.
    QCoreApplication::setOrganizationDomain(QStringLiteral("gnumbix.com"));
    QApplication::setWindowIcon(QIcon::fromTheme(AppInfo::applicationId()));

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    const QCommandLineOption settingsOption(QStringLiteral("settings"),
                                            i18n("Open the settings window on launch."));
    parser.addOption(settingsOption);
    parser.process(app);
    about.processCommandLine(&parser);

    // Unique: a second launch — from the menu, or from the autostart entry racing a
    // manual start — raises the existing instance instead of running two sync loops
    // over one folder.
    KDBusService service(KDBusService::Unique);

    AppEnvironment environment;
    SettingsWindow settings(&environment);
    TrayIcon tray(&environment);

    QObject::connect(&tray, &TrayIcon::settingsRequested, &settings, [&settings]() {
        settings.showAndRaise();
    });
    QObject::connect(&tray, &TrayIcon::quitRequested, &app, &QApplication::quit);
    QObject::connect(&service, &KDBusService::activateRequested, &settings, [&settings]() {
        settings.showAndRaise();
    });

    log::app.info(QStringLiteral("%1 launched").arg(AppInfo::versionDescription()));
    environment.start();

    if (parser.isSet(settingsOption) || !environment.fatalStartupError().isEmpty()) {
        // A database that will not open is not something to leave in the tray for
        // someone to notice later.
        settings.showAndRaise();
    }

    return app.exec();
}
