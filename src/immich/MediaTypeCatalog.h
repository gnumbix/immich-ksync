#pragma once

#include "immich/ImmichDtos.h"

#include <QSet>
#include <QString>

namespace immichksync {

/// File extensions the server will accept, lowercased and dot-prefixed (".jpg").
///
/// Asked of the server rather than hard-coded, because which formats Immich accepts
/// depends on its version and its build.
struct MediaTypeCatalog {
    QSet<QString> image;
    QSet<QString> video;
    QSet<QString> sidecar;

    /// Extensions that represent a syncable asset. Sidecars are excluded: they ride
    /// along with their asset rather than being uploaded in their own right.
    QSet<QString> assetExtensions() const { return image | video; }

    static MediaTypeCatalog fromResponse(const ServerMediaTypesResponse &response);

    /// Used only until the first successful `/server/media-types` call, so a
    /// momentarily unreachable server does not make every local file look unsupported.
    static MediaTypeCatalog fallback();

    bool operator==(const MediaTypeCatalog &other) const;
};

} // namespace immichksync
