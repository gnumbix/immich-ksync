#pragma once

#include <QString>
#include <QUrl>

namespace immichksync {

namespace AppInfo {

QString displayName();
QString version();
QString versionDescription();

/// The AGPL expects the corresponding source to be reachable by anyone running the
/// program, so these live in the app itself rather than only in the README.
QUrl repositoryUrl();
QUrl licenceUrl();

/// The reverse-DNS identifier used for the desktop file, the D-Bus name and the tray.
QString applicationId();
/// Path of the XDG autostart entry "Open at login" writes.
QString autostartFilePath();

} // namespace AppInfo

} // namespace immichksync
