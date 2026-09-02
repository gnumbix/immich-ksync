#pragma once

#include <QDateTime>
#include <QString>

namespace immichksync {

/// Places downloaded files into album folders without a partially-written file ever
/// being visible to the scanner (or to Dolphin, digiKam, or a backup tool).
namespace AtomicFileWriter {

/// Moves `source` to `destination` with `rename(2)`, then stamps the modification time
/// the server reported so a later round trip does not look like a local edit.
///
/// Staging happens inside the sync root, so source and destination are always on the
/// same filesystem and the rename really is atomic.
bool install(const QString &sourcePath,
             const QString &destinationPath,
             const QDateTime &modifiedAt,
             bool replaceExisting,
             QString *errorMessage);

/// Best-effort: the timestamp is cosmetic, so a failure here must not fail the sync.
/// Linux has no way to set a birth time, which is why only mtime is stamped.
void applyModificationTime(const QString &path, const QDateTime &modifiedAt);

/// Creates a directory and returns whether it now exists.
///
/// `markAsCache` writes a `CACHEDIR.TAG`, which every well-behaved Linux backup tool
/// honours — the equivalent of excluding the staging area from Time Machine.
bool ensureDirectory(const QString &path, bool markAsCache, QString *errorMessage);

} // namespace AtomicFileWriter

} // namespace immichksync
