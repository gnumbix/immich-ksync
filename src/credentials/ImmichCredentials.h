#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace immichksync {

/// How the app proves who it is to the Immich server.
///
/// Both modes end up as a single header. `POST /auth/login` returns a session token
/// the server accepts as `Authorization: Bearer`, so password sign-in is converted to
/// a token once and the password is never persisted.
enum class ImmichAuthMode {
    ApiKey,
    Password,
};

/// The string written to the config file.
QString keyFor(ImmichAuthMode mode);
ImmichAuthMode authModeFromString(const QString &raw);
QString displayName(ImmichAuthMode mode);

class ImmichCredentials {
public:
    ImmichCredentials() = default;

    static ImmichCredentials apiKey(const QString &key);
    static ImmichCredentials sessionToken(const QString &token);

    bool isValid() const { return !m_secret.isEmpty(); }

    QString headerField() const;
    QString headerValue() const;
    ImmichAuthMode mode() const { return m_mode; }

    /// Never log the secret itself.
    QString redactedDescription() const;

    bool operator==(const ImmichCredentials &other) const
    {
        return m_mode == other.m_mode && m_secret == other.m_secret;
    }
    bool operator!=(const ImmichCredentials &other) const { return !(*this == other); }

private:
    ImmichAuthMode m_mode = ImmichAuthMode::ApiKey;
    QString m_secret;
};

/// The exact permission set this app needs, taken from `x-immich-permission` on each
/// endpoint in `open-api/immich-openapi-specs.json`. Used to pre-flight an API key so
/// the user is told what to fix before the first sync fails halfway through.
///
/// Note that `asset.delete` is deliberately absent: this app is not capable of
/// deleting an asset from a library.
enum class ImmichPermission {
    AlbumRead,
    AlbumCreate,
    AlbumUpdate,
    AlbumAssetCreate,
    AlbumAssetDelete,
    AssetRead,
    AssetUpload,
    AssetDownload,
    UserRead,
};

/// The permission name as the Immich API spells it.
QString keyFor(ImmichPermission permission);
QString purpose(ImmichPermission permission);
QList<ImmichPermission> allPermissions();

/// Wildcard granted by an "all access" key.
inline constexpr const char *kAllAccessPermission = "all";

/// Returns the permissions missing from `granted`, honouring the `all` wildcard.
QList<ImmichPermission> missingPermissions(const QStringList &granted);

} // namespace immichksync
