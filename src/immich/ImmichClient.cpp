#include "immich/ImmichClient.h"

#include "core/Clock.h"
#include "core/Logging.h"
#include "core/TaskPool.h"
#include "immich/MultipartBody.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QUuid>

namespace immichksync {

// MARK: - Endpoint

Endpoint Endpoint::get(const QString &path, const QUrlQuery &query)
{
    Endpoint endpoint;
    endpoint.method = QStringLiteral("GET");
    endpoint.path = path;
    endpoint.query = query;
    return endpoint;
}

Endpoint Endpoint::json(const QString &method, const QString &path, const QJsonObject &body)
{
    Endpoint endpoint;
    endpoint.method = method;
    endpoint.path = path;
    endpoint.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    endpoint.contentType = QStringLiteral("application/json");
    return endpoint;
}

Endpoint Endpoint::json(const QString &method, const QString &path, const QJsonArray &body)
{
    Endpoint endpoint;
    endpoint.method = method;
    endpoint.path = path;
    endpoint.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    endpoint.contentType = QStringLiteral("application/json");
    return endpoint;
}

// MARK: - Construction

ImmichClient::ImmichClient(QUrl apiBaseUrl,
                           std::optional<ImmichCredentials> credentials,
                           Transport *transport)
    : m_apiBaseUrl(std::move(apiBaseUrl))
    , m_credentials(std::move(credentials))
    , m_transport(transport)
{
}

// MARK: - Transport

HttpRequest ImmichClient::makeRequest(const Endpoint &endpoint) const
{
    HttpRequest request;
    request.method = endpoint.method;

    QString path = m_apiBaseUrl.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    QString suffix = endpoint.path;
    if (!suffix.startsWith(QLatin1Char('/'))) {
        suffix.prepend(QLatin1Char('/'));
    }

    QUrl url = m_apiBaseUrl;
    url.setPath(path + suffix);
    if (!endpoint.query.isEmpty()) {
        url.setQuery(endpoint.query);
    }
    request.url = url;

    if (!endpoint.contentType.isEmpty()) {
        request.headers.insert(QStringLiteral("Content-Type"), endpoint.contentType);
    }
    if (m_credentials) {
        request.headers.insert(m_credentials->headerField(), m_credentials->headerValue());
    }
    for (auto it = endpoint.extraHeaders.cbegin(); it != endpoint.extraHeaders.cend(); ++it) {
        request.headers.insert(it.key(), it.value());
    }
    request.body = endpoint.body;
    request.followRedirects = endpoint.followRedirects;
    return request;
}

QString ImmichClient::serverMessage(const QByteArray &body)
{
    if (body.isEmpty()) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (document.isObject()) {
        const QJsonObject object = document.object();
        const QJsonValue message = object.value(QStringLiteral("message"));
        if (message.isString()) {
            return message.toString();
        }
        if (message.isArray()) {
            QStringList parts;
            for (const QJsonValue &entry : message.toArray()) {
                parts.append(entry.toString());
            }
            return parts.join(QStringLiteral("; "));
        }
        const QJsonValue error = object.value(QStringLiteral("error"));
        if (error.isString()) {
            return error.toString();
        }
    }
    return QString::fromUtf8(body.left(512)).trimmed();
}

ImmichError ImmichClient::errorFrom(const HttpResponse &response, const QString &label)
{
    if (!response.error.isNull()) {
        return response.error;
    }
    if (response.statusCode == 0) {
        return ImmichError::notAnImmichServer(label);
    }
    if (response.isSuccess()) {
        return {};
    }
    return ImmichError::http(response.statusCode,
                             label,
                             serverMessage(response.body),
                             parseRetryAfter(response.header(QStringLiteral("Retry-After"))));
}

HttpResponse ImmichClient::perform(const Endpoint &endpoint) const
{
    if (!m_transport) {
        HttpResponse response;
        response.error = ImmichError::local(QStringLiteral("No transport is configured."));
        return response;
    }
    return m_transport->send(makeRequest(endpoint));
}

HttpResponse ImmichClient::performRetrying(const Endpoint &endpoint) const
{
    HttpResponse response;
    Retry::run(m_retryPolicy, endpoint.label(), [&]() {
        response = perform(endpoint);
        const ImmichError error = errorFrom(response, endpoint.label());
        RetryDecision decision;
        decision.succeeded = error.isNull();
        decision.isRetryable = error.isRetryable();
        decision.retryAfter = error.retryAfter();
        decision.errorMessage = error.message();
        return decision;
    });
    return response;
}

namespace {

/// Shared tail of every JSON call: check the status, parse, and report which of the
/// two went wrong. A body that is not JSON at all usually means something other than
/// Immich answered — an SSO portal or a proxy error page.
template<typename T, typename Parse>
Result<T> decode(const HttpResponse &response, const QString &label, ImmichError error, Parse parse)
{
    Result<T> result;
    if (!error.isNull()) {
        result.error = error;
        return result;
    }
    const QJsonDocument document = QJsonDocument::fromJson(response.body);
    if (document.isNull()) {
        result.error = ImmichError::notAnImmichServer(label);
        return result;
    }
    auto parsed = parse(document);
    if (!parsed) {
        result.error = ImmichError::decoding(label, QStringLiteral("unexpected response shape"));
        return result;
    }
    result.value = std::move(*parsed);
    return result;
}

} // namespace

// MARK: - Server information

Result<QJsonObject> ImmichClient::ping()
{
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/server/ping"));
    const HttpResponse response = performRetrying(endpoint);
    return decode<QJsonObject>(response,
                               endpoint.label(),
                               errorFrom(response, endpoint.label()),
                               [](const QJsonDocument &document) -> std::optional<QJsonObject> {
                                   if (!document.isObject()) {
                                       return std::nullopt;
                                   }
                                   return document.object();
                               });
}

Result<ServerVersion> ImmichClient::serverVersion()
{
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/server/version"));
    const HttpResponse response = performRetrying(endpoint);
    return decode<ServerVersion>(response,
                                 endpoint.label(),
                                 errorFrom(response, endpoint.label()),
                                 [](const QJsonDocument &document) {
                                     return ServerVersion::fromJson(document.object());
                                 });
}

Result<ServerMediaTypesResponse> ImmichClient::supportedMediaTypes()
{
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/server/media-types"));
    const HttpResponse response = performRetrying(endpoint);
    return decode<ServerMediaTypesResponse>(response,
                                            endpoint.label(),
                                            errorFrom(response, endpoint.label()),
                                            [](const QJsonDocument &document) {
                                                return ServerMediaTypesResponse::fromJson(
                                                    document.object());
                                            });
}

// MARK: - Identity

Result<UserResponse> ImmichClient::currentUser()
{
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/users/me"));
    const HttpResponse response = performRetrying(endpoint);
    return decode<UserResponse>(response,
                                endpoint.label(),
                                errorFrom(response, endpoint.label()),
                                [](const QJsonDocument &document) {
                                    return UserResponse::fromJson(document.object());
                                });
}

Result<ApiKeyResponse> ImmichClient::currentApiKey()
{
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/api-keys/me"));
    const HttpResponse response = performRetrying(endpoint);
    return decode<ApiKeyResponse>(response,
                                  endpoint.label(),
                                  errorFrom(response, endpoint.label()),
                                  [](const QJsonDocument &document) {
                                      return ApiKeyResponse::fromJson(document.object());
                                  });
}

Result<LoginResponse> ImmichClient::login(const QString &email, const QString &password)
{
    QJsonObject body;
    body.insert(QStringLiteral("email"), email);
    body.insert(QStringLiteral("password"), password);
    const Endpoint endpoint =
        Endpoint::json(QStringLiteral("POST"), QStringLiteral("/auth/login"), body);

    // Never retried: a wrong password is not a transient failure, and repeated attempts
    // trip server-side rate limiting.
    const HttpResponse response = perform(endpoint);
    return decode<LoginResponse>(response,
                                 endpoint.label(),
                                 errorFrom(response, endpoint.label()),
                                 [](const QJsonDocument &document) {
                                     return LoginResponse::fromJson(document.object());
                                 });
}

// MARK: - Albums

Result<QList<AlbumResponse>> ImmichClient::ownedAlbums()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("isOwned"), QStringLiteral("true"));
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/albums"), query);
    const HttpResponse response = performRetrying(endpoint);

    return decode<QList<AlbumResponse>>(
        response,
        endpoint.label(),
        errorFrom(response, endpoint.label()),
        [](const QJsonDocument &document) -> std::optional<QList<AlbumResponse>> {
            if (!document.isArray()) {
                return std::nullopt;
            }
            QList<AlbumResponse> albums;
            for (const QJsonValue &entry : document.array()) {
                if (auto album = AlbumResponse::fromJson(entry.toObject())) {
                    albums.append(*album);
                }
            }
            return albums;
        });
}

Result<AlbumResponse> ImmichClient::album(const QString &id)
{
    const Endpoint endpoint = Endpoint::get(QStringLiteral("/albums/%1").arg(id));
    const HttpResponse response = performRetrying(endpoint);
    return decode<AlbumResponse>(response,
                                 endpoint.label(),
                                 errorFrom(response, endpoint.label()),
                                 [](const QJsonDocument &document) {
                                     return AlbumResponse::fromJson(document.object());
                                 });
}

Result<AlbumResponse> ImmichClient::createAlbum(const QString &name)
{
    QJsonObject body;
    body.insert(QStringLiteral("albumName"), name);
    const Endpoint endpoint =
        Endpoint::json(QStringLiteral("POST"), QStringLiteral("/albums"), body);
    const HttpResponse response = performRetrying(endpoint);
    return decode<AlbumResponse>(response,
                                 endpoint.label(),
                                 errorFrom(response, endpoint.label()),
                                 [](const QJsonDocument &document) {
                                     return AlbumResponse::fromJson(document.object());
                                 });
}

Result<AlbumResponse> ImmichClient::renameAlbum(const QString &id, const QString &name)
{
    QJsonObject body;
    body.insert(QStringLiteral("albumName"), name);
    const Endpoint endpoint =
        Endpoint::json(QStringLiteral("PATCH"), QStringLiteral("/albums/%1").arg(id), body);
    const HttpResponse response = performRetrying(endpoint);
    return decode<AlbumResponse>(response,
                                 endpoint.label(),
                                 errorFrom(response, endpoint.label()),
                                 [](const QJsonDocument &document) {
                                     return AlbumResponse::fromJson(document.object());
                                 });
}

ImmichError ImmichClient::deleteAlbum(const QString &id)
{
    Endpoint endpoint;
    endpoint.method = QStringLiteral("DELETE");
    endpoint.path = QStringLiteral("/albums/%1").arg(id);
    const HttpResponse response = perform(endpoint);
    return errorFrom(response, endpoint.label());
}

namespace {

Result<QList<BulkIdResponse>> mutateAlbumAssets(ImmichClient &client,
                                                const std::function<HttpResponse(const QJsonArray &)> &send,
                                                const QStringList &assetIds,
                                                const QString &label)
{
    Q_UNUSED(client)
    Result<QList<BulkIdResponse>> result;
    if (assetIds.isEmpty()) {
        result.value = QList<BulkIdResponse>{};
        return result;
    }

    QList<BulkIdResponse> collected;
    for (const QStringList &chunk : chunked(assetIds, ImmichLimits::kAlbumAssetChunk)) {
        QJsonArray ids;
        for (const QString &id : chunk) {
            ids.append(id);
        }
        const HttpResponse response = send(ids);
        if (!response.isSuccess()) {
            result.error = response.error.isNull()
                ? ImmichError::http(response.statusCode, label, {}, std::nullopt)
                : response.error;
            return result;
        }
        const QJsonDocument document = QJsonDocument::fromJson(response.body);
        if (!document.isArray()) {
            result.error = ImmichError::decoding(label, QStringLiteral("expected an array"));
            return result;
        }
        for (const QJsonValue &entry : document.array()) {
            if (auto parsed = BulkIdResponse::fromJson(entry.toObject())) {
                collected.append(*parsed);
            }
        }
    }
    result.value = collected;
    return result;
}

} // namespace

Result<QList<BulkIdResponse>> ImmichClient::addAssets(const QString &albumId,
                                                      const QStringList &assetIds)
{
    const QString path = QStringLiteral("/albums/%1/assets").arg(albumId);
    return mutateAlbumAssets(
        *this,
        [&](const QJsonArray &ids) {
            QJsonObject body;
            body.insert(QStringLiteral("ids"), ids);
            return performRetrying(Endpoint::json(QStringLiteral("PUT"), path, body));
        },
        assetIds,
        QStringLiteral("PUT %1").arg(path));
}

Result<QList<BulkIdResponse>> ImmichClient::removeAssets(const QString &albumId,
                                                         const QStringList &assetIds)
{
    const QString path = QStringLiteral("/albums/%1/assets").arg(albumId);
    return mutateAlbumAssets(
        *this,
        [&](const QJsonArray &ids) {
            QJsonObject body;
            body.insert(QStringLiteral("ids"), ids);
            return performRetrying(Endpoint::json(QStringLiteral("DELETE"), path, body));
        },
        assetIds,
        QStringLiteral("DELETE %1").arg(path));
}

// MARK: - Album contents

Result<QList<AssetResponse>> ImmichClient::albumAssets(const QString &albumId,
                                                       bool useCursorPagination)
{
    Result<QList<AssetResponse>> result;
    QList<AssetResponse> collected;
    QString cursor;
    int page = 1;

    while (true) {
        QJsonObject body;
        if (useCursorPagination) {
            // The 3.2 shape. Mixing it with the flat fields below is rejected outright.
            QJsonArray albumIds;
            albumIds.append(albumId);
            QJsonObject any;
            any.insert(QStringLiteral("any"), albumIds);
            QJsonObject filter;
            filter.insert(QStringLiteral("albumIds"), any);
            body.insert(QStringLiteral("filter"), filter);
            body.insert(QStringLiteral("size"), ImmichLimits::kSearchPageSize);
            if (!cursor.isEmpty()) {
                body.insert(QStringLiteral("cursor"), cursor);
            }
        } else {
            QJsonArray albumIds;
            albumIds.append(albumId);
            body.insert(QStringLiteral("albumIds"), albumIds);
            body.insert(QStringLiteral("page"), page);
            body.insert(QStringLiteral("size"), ImmichLimits::kSearchPageSize);
        }

        const Endpoint endpoint =
            Endpoint::json(QStringLiteral("POST"), QStringLiteral("/search/metadata"), body);
        const HttpResponse response = performRetrying(endpoint);
        const ImmichError error = errorFrom(response, endpoint.label());
        if (!error.isNull()) {
            result.error = error;
            return result;
        }

        const QJsonDocument document = QJsonDocument::fromJson(response.body);
        const auto parsed = SearchAssetsResponse::fromJson(document.object());
        if (!parsed) {
            result.error = ImmichError::decoding(endpoint.label(),
                                                 QStringLiteral("unexpected search response"));
            return result;
        }
        collected += parsed->items;

        if (useCursorPagination) {
            if (parsed->nextCursor.isEmpty()) {
                break;
            }
            cursor = parsed->nextCursor;
        } else {
            bool ok = false;
            const int next = parsed->nextPage.toInt(&ok);
            if (!ok || next <= page) {
                break;
            }
            page = next;
        }

        // Defensive: a server that never stops advancing must not spin forever.
        if (collected.size() > ImmichLimits::kMaximumAlbumAssets) {
            log::api.warning(QStringLiteral("Album %1 returned more than %2 assets; truncating.")
                                 .arg(albumId)
                                 .arg(ImmichLimits::kMaximumAlbumAssets));
            break;
        }
    }

    result.value = collected;
    return result;
}

// MARK: - Upload

Result<QList<BulkUploadCheckResult>>
ImmichClient::bulkUploadCheck(const QList<BulkUploadCheckItem> &items)
{
    Result<QList<BulkUploadCheckResult>> result;
    if (items.isEmpty()) {
        result.value = QList<BulkUploadCheckResult>{};
        return result;
    }

    QList<BulkUploadCheckResult> collected;
    for (const auto &chunk : chunked(items, ImmichLimits::kBulkUploadCheckChunk)) {
        QJsonArray assets;
        for (const BulkUploadCheckItem &item : chunk) {
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), item.id);
            entry.insert(QStringLiteral("checksum"), item.checksum);
            assets.append(entry);
        }
        QJsonObject body;
        body.insert(QStringLiteral("assets"), assets);

        const Endpoint endpoint = Endpoint::json(QStringLiteral("POST"),
                                                 QStringLiteral("/assets/bulk-upload-check"),
                                                 body);
        const HttpResponse response = performRetrying(endpoint);
        const ImmichError error = errorFrom(response, endpoint.label());
        if (!error.isNull()) {
            result.error = error;
            return result;
        }

        const QJsonDocument document = QJsonDocument::fromJson(response.body);
        const QJsonArray results = document.object().value(QStringLiteral("results")).toArray();
        for (const QJsonValue &entry : results) {
            if (auto parsed = BulkUploadCheckResult::fromJson(entry.toObject())) {
                collected.append(*parsed);
            }
        }
    }
    result.value = collected;
    return result;
}

Result<AssetMediaResponse> ImmichClient::upload(const UploadRequest &request,
                                                const QString &stagingDirectory)
{
    Result<AssetMediaResponse> result;

    QList<MultipartFilePart> files;
    files.append({QStringLiteral("assetData"), request.filename, request.filePath});
    if (!request.sidecarPath.isEmpty()) {
        files.append({QStringLiteral("sidecarData"),
                      QFileInfo(request.sidecarPath).fileName(),
                      request.sidecarPath});
    }

    QString stagingError;
    MultipartBody body = MultipartBody::write({{QStringLiteral("fileCreatedAt"),
                                                toImmichIso8601(request.fileCreatedAt)},
                                               {QStringLiteral("fileModifiedAt"),
                                                toImmichIso8601(request.fileModifiedAt)},
                                               {QStringLiteral("filename"), request.filename},
                                               {QStringLiteral("isFavorite"),
                                                request.isFavorite ? QStringLiteral("true")
                                                                   : QStringLiteral("false")}},
                                              files,
                                              stagingDirectory,
                                              &stagingError);
    if (!body.isValid()) {
        result.error = ImmichError::local(stagingError);
        return result;
    }

    Endpoint endpoint;
    endpoint.method = QStringLiteral("POST");
    endpoint.path = QStringLiteral("/assets");
    endpoint.contentType = body.contentType();
    // A 307 would replay the body, and a 30x to an HTML login page would look like a
    // successful upload.
    endpoint.followRedirects = false;
    // Lets the server short-circuit a duplicate before reading the payload.
    endpoint.extraHeaders.insert(QStringLiteral("x-immich-checksum"), request.sha1Hex);
    endpoint.extraHeaders.insert(QStringLiteral("Content-Length"),
                                 QString::number(body.contentLength()));

    HttpRequest httpRequest = makeRequest(endpoint);
    httpRequest.uploadFilePath = body.path();

    const HttpResponse response = m_transport
        ? m_transport->send(httpRequest)
        : HttpResponse{0, {}, {}, 0, ImmichError::local(QStringLiteral("No transport."))};

    return decode<AssetMediaResponse>(response,
                                      endpoint.label(),
                                      errorFrom(response, endpoint.label()),
                                      [](const QJsonDocument &document) {
                                          return AssetMediaResponse::fromJson(document.object());
                                      });
}

// MARK: - Download

Result<ImmichClient::DownloadResult> ImmichClient::downloadOriginal(const QString &assetId,
                                                                    const QString &directory)
{
    Result<DownloadResult> result;

    const Endpoint endpoint = Endpoint::get(QStringLiteral("/assets/%1/original").arg(assetId));
    const QString staged =
        QDir(directory).filePath(QStringLiteral("download-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    Retry::run(m_retryPolicy, endpoint.label(), [&]() {
        HttpRequest request = makeRequest(endpoint);
        request.downloadFilePath = staged;

        const HttpResponse response = m_transport
            ? m_transport->send(request)
            : HttpResponse{0, {}, {}, 0, ImmichError::local(QStringLiteral("No transport."))};

        const ImmichError error = errorFrom(response, endpoint.label());
        if (!error.isNull()) {
            // A partial file from a failed attempt must never be mistaken for a
            // complete download by the retry that follows it.
            QFile::remove(staged);
            RetryDecision decision;
            decision.isRetryable = error.isRetryable();
            decision.retryAfter = error.retryAfter();
            decision.errorMessage = error.message();
            result.error = error;
            return decision;
        }

        DownloadResult download;
        download.path = staged;
        download.byteCount = response.downloadedBytes;
        download.suggestedFilename = response.suggestedFilename();
        result.value = download;
        result.error = ImmichError();

        RetryDecision decision;
        decision.succeeded = true;
        return decision;
    });

    return result;
}

} // namespace immichksync
