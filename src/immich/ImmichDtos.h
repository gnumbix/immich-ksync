#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace immichksync {

/// Server version, compared rather than displayed.
struct ServerVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    static std::optional<ServerVersion> fromJson(const QJsonObject &object);
    QString toString() const;

    bool operator==(const ServerVersion &other) const;
    bool operator<(const ServerVersion &other) const;
    bool operator>=(const ServerVersion &other) const { return !(*this < other); }
};

struct UserResponse {
    QString id;
    QString email;
    QString name;

    static std::optional<UserResponse> fromJson(const QJsonObject &object);
    bool operator==(const UserResponse &other) const { return id == other.id; }
};

struct ApiKeyResponse {
    QStringList permissions;

    static std::optional<ApiKeyResponse> fromJson(const QJsonObject &object);
};

struct LoginResponse {
    QString accessToken;
    QString userEmail;

    static std::optional<LoginResponse> fromJson(const QJsonObject &object);
};

struct AlbumResponse {
    QString id;
    QString albumName;
    QString updatedAt;
    int assetCount = 0;

    static std::optional<AlbumResponse> fromJson(const QJsonObject &object);
};

struct AssetResponse {
    QString id;
    /// Base64 as the server sends it; converted to `Sha1Checksum` by the caller.
    QString checksum;
    QString originalFileName;
    QString fileCreatedAt;
    QString fileModifiedAt;

    static std::optional<AssetResponse> fromJson(const QJsonObject &object);
};

/// One page of `/search/metadata`, in either the 3.2 cursor shape or the older
/// page-number shape.
struct SearchAssetsResponse {
    QList<AssetResponse> items;
    QString nextCursor;
    QString nextPage;

    static std::optional<SearchAssetsResponse> fromJson(const QJsonObject &object);
};

/// Result of `POST /assets` — the created (or matched) asset.
struct AssetMediaResponse {
    QString id;
    QString status;

    static std::optional<AssetMediaResponse> fromJson(const QJsonObject &object);
};

/// One entry in the response to a bulk album mutation.
struct BulkIdResponse {
    QString id;
    bool success = false;
    QString error;

    static std::optional<BulkIdResponse> fromJson(const QJsonObject &object);
};

struct BulkUploadCheckItem {
    /// The relative path, echoed back so results can be matched to requests.
    QString id;
    /// Lowercase hex, which is what this endpoint expects.
    QString checksum;
};

struct BulkUploadCheckResult {
    QString id;
    /// "accept" or "reject" — a reject means the server already has these bytes.
    QString action;
    QString assetId;
    bool isTrashed = false;

    bool isReject() const { return action == QLatin1String("reject"); }

    static std::optional<BulkUploadCheckResult> fromJson(const QJsonObject &object);
};

struct ServerMediaTypesResponse {
    QStringList image;
    QStringList video;
    QStringList sidecar;

    static std::optional<ServerMediaTypesResponse> fromJson(const QJsonObject &object);
};

/// `GET {origin}/.well-known/immich` → `{"api":{"endpoint":"/api"}}`.
struct WellKnownResponse {
    QString endpoint;

    static std::optional<WellKnownResponse> fromJson(const QJsonObject &object);
};

/// Hard limits imposed by the Immich API, kept in one place so they are easy to audit
/// against `server/src/dtos`.
namespace ImmichLimits {
/// `BaseSearchWithResultsSchema.size` is `z.int().min(1).max(1000)`.
inline constexpr int kSearchPageSize = 1000;
/// Bulk endpoints and the official CLI both chunk album IDs at 1000.
inline constexpr int kAlbumAssetChunk = 1000;
/// The official CLI batches `bulk-upload-check` at 5000 checksums.
inline constexpr int kBulkUploadCheckChunk = 5000;
/// Guard against a pathological pagination loop.
inline constexpr int kMaximumAlbumAssets = 500000;
} // namespace ImmichLimits

} // namespace immichksync
