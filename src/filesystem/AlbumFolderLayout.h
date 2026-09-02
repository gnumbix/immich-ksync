#pragma once

#include "core/Checksum.h"

#include <QSet>
#include <QString>

#include <optional>

namespace immichksync {

/// Written into every album folder as `.immich-album.json`.
///
/// This marker is what makes folder renames and moves unambiguous, and it lets the
/// entire local database be rebuilt from disk if it is ever lost: the folder itself
/// carries the identity of the album it mirrors.
struct AlbumMarker {
    static constexpr int kCurrentSchema = 1;

    int schema = kCurrentSchema;
    QString albumId;
    /// The album's name on the server as of the last successful sync.
    QString albumName;
    /// The folder's own name as of the last successful sync.
    ///
    /// Recording it here is what makes renames unambiguous: comparing the album name
    /// to the current folder name cannot work, because a folder name may carry a
    /// collision-breaking suffix that was never part of the album name.
    QString folderName;

    bool operator==(const AlbumMarker &other) const;
};

/// Maps Immich album names and asset filenames onto names that are legal, stable and
/// collision-free on disk.
///
/// The rules here are deliberately identical to the macOS build's, `:` stripping
/// included, even though Linux would permit a colon. They are the compatibility
/// contract: the same album must produce the same folder name on both platforms, so a
/// sync folder can be moved between them without every album looking new.
namespace AlbumFolderLayout {

inline constexpr const char *kMarkerFilename = ".immich-album.json";
/// Files removed from an album server-side land here instead of being deleted.
inline constexpr const char *kTrashFolderName = ".immich-trash";
/// In-flight downloads and staged upload bodies.
inline constexpr const char *kStagingFolderName = ".immich-staging";

QSet<QString> reservedFolderNames();

/// Makes an album name safe to use as a directory name.
QString sanitize(const QString &albumName);
QString sanitizeFilename(const QString &filename);

/// A deterministic, collision-free folder name for an album.
///
/// Immich permits duplicate album names, so `taken` is compared case-insensitively
/// (some Linux filesystems, and any folder shared with a Mac, fold case) and ties are
/// broken with a stable slice of the album ID rather than a counter — a counter would
/// reshuffle folders whenever an album is deleted.
QString folderName(const QString &albumName, const QString &albumId, const QSet<QString> &taken);

/// Chooses the on-disk name for a downloaded asset.
///
/// Two assets in one album may share `originalFileName`; when that happens the
/// checksum — not a counter — disambiguates, so the same asset always lands on the
/// same path no matter what order a rebuild discovers things in.
QString localFilename(const QString &originalFileName,
                      const Sha1Checksum &checksum,
                      const QSet<QString> &taken);

/// Truncates on a character boundary so a multi-byte name never becomes invalid.
QString truncate(const QString &value, int limitBytes);

QString markerPath(const QString &folderPath);
std::optional<AlbumMarker> readMarker(const QString &folderPath);
bool writeMarker(const AlbumMarker &marker, const QString &folderPath, QString *errorMessage);

/// True for anything the scanner must never treat as album content.
bool isInternalName(const QString &name);

} // namespace AlbumFolderLayout

} // namespace immichksync
