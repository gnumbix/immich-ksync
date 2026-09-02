#include "credentials/ImmichCredentials.h"

#include <QSet>

namespace immichksync {

QString keyFor(ImmichAuthMode mode)
{
    return mode == ImmichAuthMode::ApiKey ? QStringLiteral("apiKey") : QStringLiteral("password");
}

ImmichAuthMode authModeFromString(const QString &raw)
{
    return raw == QLatin1String("password") ? ImmichAuthMode::Password : ImmichAuthMode::ApiKey;
}

QString displayName(ImmichAuthMode mode)
{
    return mode == ImmichAuthMode::ApiKey ? QStringLiteral("API key")
                                          : QStringLiteral("Email & password");
}

ImmichCredentials ImmichCredentials::apiKey(const QString &key)
{
    ImmichCredentials credentials;
    credentials.m_mode = ImmichAuthMode::ApiKey;
    credentials.m_secret = key;
    return credentials;
}

ImmichCredentials ImmichCredentials::sessionToken(const QString &token)
{
    ImmichCredentials credentials;
    credentials.m_mode = ImmichAuthMode::Password;
    credentials.m_secret = token;
    return credentials;
}

QString ImmichCredentials::headerField() const
{
    return m_mode == ImmichAuthMode::ApiKey ? QStringLiteral("x-api-key")
                                            : QStringLiteral("Authorization");
}

QString ImmichCredentials::headerValue() const
{
    return m_mode == ImmichAuthMode::ApiKey ? m_secret : QStringLiteral("Bearer %1").arg(m_secret);
}

QString ImmichCredentials::redactedDescription() const
{
    if (m_mode == ImmichAuthMode::ApiKey) {
        return QStringLiteral("api key ending …%1").arg(m_secret.right(4));
    }
    return QStringLiteral("session token");
}

QString keyFor(ImmichPermission permission)
{
    switch (permission) {
    case ImmichPermission::AlbumRead: return QStringLiteral("album.read");
    case ImmichPermission::AlbumCreate: return QStringLiteral("album.create");
    case ImmichPermission::AlbumUpdate: return QStringLiteral("album.update");
    case ImmichPermission::AlbumAssetCreate: return QStringLiteral("albumAsset.create");
    case ImmichPermission::AlbumAssetDelete: return QStringLiteral("albumAsset.delete");
    case ImmichPermission::AssetRead: return QStringLiteral("asset.read");
    case ImmichPermission::AssetUpload: return QStringLiteral("asset.upload");
    case ImmichPermission::AssetDownload: return QStringLiteral("asset.download");
    case ImmichPermission::UserRead: return QStringLiteral("user.read");
    }
    return {};
}

QString purpose(ImmichPermission permission)
{
    switch (permission) {
    case ImmichPermission::AlbumRead: return QStringLiteral("List your albums");
    case ImmichPermission::AlbumCreate: return QStringLiteral("Create an album for a new folder");
    case ImmichPermission::AlbumUpdate:
        return QStringLiteral("Rename an album when its folder is renamed");
    case ImmichPermission::AlbumAssetCreate:
        return QStringLiteral("Add uploaded photos to their album");
    case ImmichPermission::AlbumAssetDelete:
        return QStringLiteral("Remove an album entry when the local file is deleted");
    case ImmichPermission::AssetRead: return QStringLiteral("Enumerate the photos inside an album");
    case ImmichPermission::AssetUpload: return QStringLiteral("Upload new local files");
    case ImmichPermission::AssetDownload:
        return QStringLiteral("Download originals into album folders");
    case ImmichPermission::UserRead: return QStringLiteral("Confirm which account is signed in");
    }
    return {};
}

QList<ImmichPermission> allPermissions()
{
    return {ImmichPermission::AlbumRead,
            ImmichPermission::AlbumCreate,
            ImmichPermission::AlbumUpdate,
            ImmichPermission::AlbumAssetCreate,
            ImmichPermission::AlbumAssetDelete,
            ImmichPermission::AssetRead,
            ImmichPermission::AssetUpload,
            ImmichPermission::AssetDownload,
            ImmichPermission::UserRead};
}

QList<ImmichPermission> missingPermissions(const QStringList &granted)
{
    const QSet<QString> grantedSet(granted.begin(), granted.end());
    if (grantedSet.contains(QLatin1String(kAllAccessPermission))) {
        return {};
    }
    QList<ImmichPermission> missing;
    for (const ImmichPermission permission : allPermissions()) {
        if (!grantedSet.contains(keyFor(permission))) {
            missing.append(permission);
        }
    }
    return missing;
}

} // namespace immichksync
