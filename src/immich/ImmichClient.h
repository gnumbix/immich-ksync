#pragma once

#include "core/Retry.h"
#include "credentials/ImmichCredentials.h"
#include "immich/ImmichDtos.h"
#include "immich/ImmichError.h"
#include "immich/Transport.h"

#include <QDateTime>
#include <QList>
#include <QUrl>
#include <QUrlQuery>

#include <optional>

namespace immichksync {

/// A single Immich API request, independent of transport concerns.
struct Endpoint {
    QString method = QStringLiteral("GET");
    QString path;
    QUrlQuery query;
    QByteArray body;
    QString contentType;
    QMap<QString, QString> extraHeaders;
    bool followRedirects = true;

    QString label() const { return QStringLiteral("%1 %2").arg(method, path); }

    static Endpoint get(const QString &path, const QUrlQuery &query = {});
    static Endpoint json(const QString &method, const QString &path, const QJsonObject &body);
    static Endpoint json(const QString &method, const QString &path, const QJsonArray &body);
};

/// A result that is either a value or an error, so nothing here needs exceptions and
/// no call site can forget to check.
template<typename T>
struct Result {
    std::optional<T> value;
    ImmichError error;

    bool succeeded() const { return value.has_value(); }
    const T &operator*() const { return *value; }
    const T *operator->() const { return &*value; }
};

/// HTTP client for one server and credential pair.
///
/// Immutable: it owns no mutable state, so requests run fully in parallel and
/// reconfiguration is expressed by constructing a new client rather than mutating a
/// shared one mid-flight.
class ImmichClient {
public:
    ImmichClient(QUrl apiBaseUrl, std::optional<ImmichCredentials> credentials, Transport *transport);

    QUrl apiBaseUrl() const { return m_apiBaseUrl; }

    /// Overrides the backoff used for idempotent calls. Production keeps the default;
    /// the hermetic tests set a near-zero policy so asserting on retry behaviour does
    /// not mean actually waiting out the ladder.
    void setRetryPolicy(const RetryPolicy &policy) { m_retryPolicy = policy; }

    // Server information
    Result<QJsonObject> ping();
    Result<ServerVersion> serverVersion();
    Result<ServerMediaTypesResponse> supportedMediaTypes();

    // Identity
    Result<UserResponse> currentUser();
    /// Only meaningful with an API key; a session token has no key behind it.
    Result<ApiKeyResponse> currentApiKey();
    Result<LoginResponse> login(const QString &email, const QString &password);

    // Albums
    /// `isOwned=true` restricts to albums where the caller's role is owner; albums
    /// merely shared with the user are out of scope for this app.
    Result<QList<AlbumResponse>> ownedAlbums();
    Result<AlbumResponse> album(const QString &id);
    Result<AlbumResponse> createAlbum(const QString &name);
    Result<AlbumResponse> renameAlbum(const QString &id, const QString &name);
    /// Only used to tidy up after integration tests; the sync engine never deletes an
    /// album, because a missing folder is not evidence that the album should be gone.
    ImmichError deleteAlbum(const QString &id);

    Result<QList<BulkIdResponse>> addAssets(const QString &albumId, const QStringList &assetIds);
    Result<QList<BulkIdResponse>> removeAssets(const QString &albumId, const QStringList &assetIds);

    /// Enumerates every asset in an album, following whichever pagination shape the
    /// server supports.
    ///
    /// `POST /search/metadata` rejects requests that mix the deprecated flat fields
    /// with the 3.2 `filter`/`cursor` shape, so the two paths never share a request.
    Result<QList<AssetResponse>> albumAssets(const QString &albumId, bool useCursorPagination);

    // Upload
    Result<QList<BulkUploadCheckResult>> bulkUploadCheck(const QList<BulkUploadCheckItem> &items);

    struct UploadRequest {
        QString filePath;
        /// Sent as the `filename` form field, which the server prefers over the
        /// multipart part name when setting `originalFileName`.
        QString filename;
        QString sha1Hex;
        QDateTime fileCreatedAt;
        QDateTime fileModifiedAt;
        bool isFavorite = false;
        /// XMP sidecar discovered next to the asset, if any.
        QString sidecarPath;
    };

    Result<AssetMediaResponse> upload(const UploadRequest &request, const QString &stagingDirectory);

    // Download
    struct DownloadResult {
        /// Temporary location; the caller owns the file and must move or delete it.
        QString path;
        qint64 byteCount = 0;
        QString suggestedFilename;
    };

    Result<DownloadResult> downloadOriginal(const QString &assetId, const QString &directory);

private:
    HttpRequest makeRequest(const Endpoint &endpoint) const;
    HttpResponse perform(const Endpoint &endpoint) const;
    /// Wraps `perform` in the shared retry policy. Used for every idempotent call.
    HttpResponse performRetrying(const Endpoint &endpoint) const;

    /// Turns a response into an error, or a null error when it succeeded.
    static ImmichError errorFrom(const HttpResponse &response, const QString &label);
    /// Immich errors are `{ "message": ..., "statusCode": n }`; falls back to a short
    /// text excerpt so an HTML error page is still legible in the log.
    static QString serverMessage(const QByteArray &body);

    QUrl m_apiBaseUrl;
    std::optional<ImmichCredentials> m_credentials;
    Transport *m_transport;
    RetryPolicy m_retryPolicy = RetryPolicy::standard();
};

} // namespace immichksync
