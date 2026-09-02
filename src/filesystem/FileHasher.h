#pragma once

#include "core/Checksum.h"

#include <QString>

#include <optional>

namespace immichksync {

namespace FileHasher {

/// Streams a file through SHA-1 without ever holding it in memory.
///
/// SHA-1 is not a security choice here: it is the digest Immich uses for asset
/// deduplication, and matching it is the only way a local file and a remote asset can
/// be recognised as the same bytes.
std::optional<Sha1Checksum> checksumOf(const QString &path, QString *errorMessage = nullptr);

} // namespace FileHasher

} // namespace immichksync
