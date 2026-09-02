#pragma once

#include "credentials/ImmichCredentials.h"
#include "immich/ImmichClient.h"
#include "immich/MediaTypeCatalog.h"

#include <QUrl>

#include <optional>

namespace immichksync {

/// Everything the app learns about a server in one round of probing.
struct ServerProfile {
    QUrl apiBaseUrl;
    ServerVersion version;
    UserResponse user;
    MediaTypeCatalog mediaTypes;
    /// Meaningful only in API-key mode; `hasPermissionInformation` is false when the
    /// check does not apply or could not be made.
    QList<ImmichPermission> missingPermissions;
    bool hasPermissionInformation = false;

    /// Cursor pagination on `/search/metadata` landed in 3.2.0; older servers must use
    /// the deprecated flat `page` shape instead.
    bool supportsCursorPagination() const { return version >= ServerVersion{3, 2, 0}; }

    bool isUsable() const { return !hasPermissionInformation || missingPermissions.isEmpty(); }
};

namespace ServerDiscovery {

/// Turns whatever the user typed into the API base URL.
///
/// Immich publishes `GET {origin}/.well-known/immich` → `{"api":{"endpoint":"/api"}}`
/// precisely so a client can accept the web address. The official CLI does the same
/// lookup before its first request.
std::optional<QUrl> resolveApiBaseUrl(const QString &userInput, Transport *transport);

/// Normalises user input to an absolute http(s) URL with no trailing slash.
std::optional<QUrl> normalizedOrigin(const QString &input);

/// Validates credentials and collects the facts the sync engine needs.
Result<ServerProfile> probe(const QUrl &apiBaseUrl,
                            const ImmichCredentials &credentials,
                            Transport *transport,
                            const RetryPolicy &retryPolicy = RetryPolicy::standard());

} // namespace ServerDiscovery

} // namespace immichksync
