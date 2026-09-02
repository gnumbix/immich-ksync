#include "app/AppInfo.h"

#include "core/Logging.h"

#include <QDir>
#include <QStandardPaths>

namespace immichksync {

namespace AppInfo {

QString displayName()
{
    return QStringLiteral("ImmichKSync");
}

QString version()
{
    return QLatin1String(IMMICHKSYNC_VERSION);
}

QString versionDescription()
{
    return QStringLiteral("%1 %2").arg(displayName(), version());
}

QUrl repositoryUrl()
{
    return QUrl(QStringLiteral("https://github.com/GnumBix/immich-ksync"));
}

QUrl licenceUrl()
{
    return QUrl(QStringLiteral("https://www.gnu.org/licenses/agpl-3.0.html"));
}

QString applicationId()
{
    return QString::fromLatin1(kAppId);
}

QString autostartFilePath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(base).filePath(QStringLiteral("autostart/%1.desktop").arg(applicationId()));
}

} // namespace AppInfo

} // namespace immichksync
