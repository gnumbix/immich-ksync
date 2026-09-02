#include "immich/ServerDiscovery.h"

#include "core/Logging.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace immichksync {

namespace ServerDiscovery {

namespace {

/// Asks `{origin}/.well-known/immich` where the API lives.
std::optional<QUrl> wellKnownEndpoint(const QUrl &origin, Transport *transport)
{
    if (!transport) {
        return std::nullopt;
    }

    // `.well-known` lives at the site root, so any path the user included is stripped.
    QUrl url = origin;
    url.setPath(QStringLiteral("/.well-known/immich"));
    url.setQuery(QString());

    HttpRequest request;
    request.url = url;
    request.transferTimeoutSeconds = 10;

    const HttpResponse response = transport->send(request);
    if (!response.isSuccess()) {
        return std::nullopt;
    }

    const QJsonDocument document = QJsonDocument::fromJson(response.body);
    const auto wellKnown = WellKnownResponse::fromJson(document.object());
    if (!wellKnown) {
        return std::nullopt;
    }

    // `endpoint` is normally the relative "/api" but may be absolute.
    const QUrl absolute(wellKnown->endpoint);
    if (!absolute.scheme().isEmpty()) {
        return absolute;
    }

    QUrl resolved = origin;
    resolved.setPath(wellKnown->endpoint.startsWith(QLatin1Char('/'))
                         ? wellKnown->endpoint
                         : QLatin1Char('/') + wellKnown->endpoint);
    return resolved;
}

} // namespace

std::optional<QUrl> normalizedOrigin(const QString &input)
{
    QString text = input.trimmed();
    if (text.isEmpty()) {
        return std::nullopt;
    }
    if (!text.contains(QStringLiteral("://"))) {
        // Defaulting to https rather than http: a typed bare hostname should not
        // silently downgrade a server that supports TLS.
        text.prepend(QStringLiteral("https://"));
    }
    while (text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }

    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid()) {
        return std::nullopt;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        return std::nullopt;
    }
    if (url.host().isEmpty()) {
        return std::nullopt;
    }
    return url;
}

std::optional<QUrl> resolveApiBaseUrl(const QString &userInput, Transport *transport)
{
    const auto origin = normalizedOrigin(userInput);
    if (!origin) {
        return std::nullopt;
    }

    if (const auto discovered = wellKnownEndpoint(*origin, transport)) {
        log::api.info(QStringLiteral("Discovered Immich API at %1").arg(discovered->toString()));
        return discovered;
    }

    // No `.well-known` (an older server, or a proxy that hides it): fall back to the
    // conventional layout, respecting an explicit `/api` the user already typed.
    QString path = origin->path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path.endsWith(QStringLiteral("api"))) {
        return origin;
    }

    QUrl resolved = *origin;
    resolved.setPath(path + QStringLiteral("/api"));
    return resolved;
}

Result<ServerProfile> probe(const QUrl &apiBaseUrl,
                            const ImmichCredentials &credentials,
                            Transport *transport,
                            const RetryPolicy &retryPolicy)
{
    Result<ServerProfile> result;
    ImmichClient client(apiBaseUrl, credentials, transport);
    client.setRetryPolicy(retryPolicy);

    // Unauthenticated first, so a bad address is reported as a bad address rather than
    // as an authentication failure.
    const auto ping = client.ping();
    if (!ping.succeeded()) {
        result.error = ping.error;
        return result;
    }

    const auto version = client.serverVersion();
    if (!version.succeeded()) {
        result.error = version.error;
        return result;
    }

    const auto user = client.currentUser();
    if (!user.succeeded()) {
        result.error = user.error;
        return result;
    }

    ServerProfile profile;
    profile.apiBaseUrl = apiBaseUrl;
    profile.version = *version;
    profile.user = *user;

    const auto mediaTypes = client.supportedMediaTypes();
    if (mediaTypes.succeeded()) {
        profile.mediaTypes = MediaTypeCatalog::fromResponse(*mediaTypes);
    } else {
        log::api.warning(QStringLiteral("Could not read /server/media-types, using the built-in "
                                        "list: %1")
                             .arg(mediaTypes.error.message()));
        profile.mediaTypes = MediaTypeCatalog::fallback();
    }

    if (credentials.mode() == ImmichAuthMode::ApiKey) {
        const auto key = client.currentApiKey();
        if (key.succeeded()) {
            profile.missingPermissions = missingPermissions(key->permissions);
            profile.hasPermissionInformation = true;
        } else {
            // Not fatal: the key still works, we just cannot pre-flight it.
            log::api.warning(QStringLiteral("Could not read API key permissions: %1")
                                 .arg(key.error.message()));
        }
    }

    result.value = profile;
    return result;
}

} // namespace ServerDiscovery

} // namespace immichksync
