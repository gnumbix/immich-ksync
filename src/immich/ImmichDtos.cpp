#include "immich/ImmichDtos.h"

#include <QJsonArray>
#include <QJsonValue>

namespace immichksync {

namespace {

QStringList toStringList(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &entry : value.toArray()) {
        if (entry.isString()) {
            result.append(entry.toString());
        }
    }
    return result;
}

/// The server sends `nextPage` as a number on some versions and a string on others.
QString toIdentifierString(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return {};
}

} // namespace

std::optional<ServerVersion> ServerVersion::fromJson(const QJsonObject &object)
{
    if (!object.contains(QStringLiteral("major"))) {
        return std::nullopt;
    }
    ServerVersion version;
    version.major = object.value(QStringLiteral("major")).toInt();
    version.minor = object.value(QStringLiteral("minor")).toInt();
    version.patch = object.value(QStringLiteral("patch")).toInt();
    return version;
}

QString ServerVersion::toString() const
{
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

bool ServerVersion::operator==(const ServerVersion &other) const
{
    return major == other.major && minor == other.minor && patch == other.patch;
}

bool ServerVersion::operator<(const ServerVersion &other) const
{
    if (major != other.major) {
        return major < other.major;
    }
    if (minor != other.minor) {
        return minor < other.minor;
    }
    return patch < other.patch;
}

std::optional<UserResponse> UserResponse::fromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    UserResponse user;
    user.id = id;
    user.email = object.value(QStringLiteral("email")).toString();
    user.name = object.value(QStringLiteral("name")).toString();
    return user;
}

std::optional<ApiKeyResponse> ApiKeyResponse::fromJson(const QJsonObject &object)
{
    ApiKeyResponse response;
    response.permissions = toStringList(object.value(QStringLiteral("permissions")));
    return response;
}

std::optional<LoginResponse> LoginResponse::fromJson(const QJsonObject &object)
{
    const QString token = object.value(QStringLiteral("accessToken")).toString();
    if (token.isEmpty()) {
        return std::nullopt;
    }
    LoginResponse response;
    response.accessToken = token;
    response.userEmail = object.value(QStringLiteral("userEmail")).toString();
    return response;
}

std::optional<AlbumResponse> AlbumResponse::fromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    AlbumResponse album;
    album.id = id;
    album.albumName = object.value(QStringLiteral("albumName")).toString();
    album.updatedAt = object.value(QStringLiteral("updatedAt")).toString();
    album.assetCount = object.value(QStringLiteral("assetCount")).toInt();
    return album;
}

std::optional<AssetResponse> AssetResponse::fromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    AssetResponse asset;
    asset.id = id;
    asset.checksum = object.value(QStringLiteral("checksum")).toString();
    asset.originalFileName = object.value(QStringLiteral("originalFileName")).toString();
    asset.fileCreatedAt = object.value(QStringLiteral("fileCreatedAt")).toString();
    asset.fileModifiedAt = object.value(QStringLiteral("fileModifiedAt")).toString();
    return asset;
}

std::optional<SearchAssetsResponse> SearchAssetsResponse::fromJson(const QJsonObject &object)
{
    const QJsonObject assets = object.value(QStringLiteral("assets")).toObject();
    if (assets.isEmpty() && !object.contains(QStringLiteral("assets"))) {
        return std::nullopt;
    }

    SearchAssetsResponse response;
    for (const QJsonValue &entry : assets.value(QStringLiteral("items")).toArray()) {
        if (auto asset = AssetResponse::fromJson(entry.toObject())) {
            response.items.append(*asset);
        }
    }
    response.nextCursor = assets.value(QStringLiteral("nextCursor")).toString();
    response.nextPage = toIdentifierString(assets.value(QStringLiteral("nextPage")));
    return response;
}

std::optional<AssetMediaResponse> AssetMediaResponse::fromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    AssetMediaResponse response;
    response.id = id;
    response.status = object.value(QStringLiteral("status")).toString();
    return response;
}

std::optional<BulkIdResponse> BulkIdResponse::fromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    BulkIdResponse response;
    response.id = id;
    response.success = object.value(QStringLiteral("success")).toBool();
    response.error = object.value(QStringLiteral("error")).toString();
    return response;
}

std::optional<BulkUploadCheckResult> BulkUploadCheckResult::fromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    BulkUploadCheckResult result;
    result.id = id;
    result.action = object.value(QStringLiteral("action")).toString();
    result.assetId = object.value(QStringLiteral("assetId")).toString();
    result.isTrashed = object.value(QStringLiteral("isTrashed")).toBool();
    return result;
}

std::optional<ServerMediaTypesResponse> ServerMediaTypesResponse::fromJson(const QJsonObject &object)
{
    if (!object.contains(QStringLiteral("image"))) {
        return std::nullopt;
    }
    ServerMediaTypesResponse response;
    response.image = toStringList(object.value(QStringLiteral("image")));
    response.video = toStringList(object.value(QStringLiteral("video")));
    response.sidecar = toStringList(object.value(QStringLiteral("sidecar")));
    return response;
}

std::optional<WellKnownResponse> WellKnownResponse::fromJson(const QJsonObject &object)
{
    const QJsonObject api = object.value(QStringLiteral("api")).toObject();
    const QString endpoint = api.value(QStringLiteral("endpoint")).toString();
    if (endpoint.isEmpty()) {
        return std::nullopt;
    }
    WellKnownResponse response;
    response.endpoint = endpoint;
    return response;
}

} // namespace immichksync
