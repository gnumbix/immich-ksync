#pragma once

#include "core/Checksum.h"

#include <QDateTime>
#include <QString>

namespace immichksync {

/// The app's view of one Immich album and the folder it maps to.
struct AlbumRecord {
    QString albumId;
    QString albumName;
    /// Folder name relative to the sync root — never a full path, so moving the root
    /// does not invalidate the database.
    QString folderName;
    /// `AlbumResponseDto.updatedAt` as last observed. Compared as an opaque string.
    QString remoteUpdatedAt;
    bool hasRemoteUpdatedAt = false;
    int remoteAssetCount = 0;
    QDateTime lastDeepScanAt;
    QDateTime lastSyncedAt;
    bool isExcluded = false;
    /// Set when the safety gate withheld this album's removals.
    bool hasSafetyHold = false;
};

/// One reconciled asset: the "baseline" that lets a later cycle tell a local deletion
/// apart from a remote addition.
struct AssetBaseline {
    QString albumId;
    Sha1Checksum checksum;
    QString assetId;
    QString originalFileName;
    /// Path relative to the sync root, e.g. `Holiday 2024/IMG_0001.HEIC`.
    QString relativePath;
    qint64 size = 0;
    QDateTime syncedAt;

    bool operator==(const AssetBaseline &other) const;
    bool operator!=(const AssetBaseline &other) const { return !(*this == other); }
};

/// Cached hash of a local file, keyed by identity so a rename or a touch invalidates it.
struct LocalFileFingerprint {
    QString relativePath;
    qint64 deviceId = 0;
    qint64 inode = 0;
    qint64 size = 0;
    qint64 modifiedAtNanoseconds = 0;
    Sha1Checksum checksum;

    /// Two fingerprints describe the same bytes when identity and timestamps agree.
    bool matches(const LocalFileFingerprint &other) const;
};

/// A removal the safety gate refused to perform until the user confirms it.
struct HeldRemoval {
    enum class Direction {
        /// The local file is gone; confirming removes the asset from the album.
        RemoveFromAlbum,
        /// The asset left the album; confirming moves the local file to the trash.
        TrashLocalFile,
    };

    QString albumId;
    Sha1Checksum checksum;
    Direction direction = Direction::RemoveFromAlbum;
    QString displayName;
    QDateTime detectedAt;

    QString id() const;
};

/// The string written to the `direction` column.
QString keyFor(HeldRemoval::Direction direction);
HeldRemoval::Direction directionFromString(const QString &raw);

/// Back-off policy for items that keep failing, so one unreadable file or one asset
/// the server refuses does not consume a transfer slot on every cycle forever.
namespace TransferBackoff {

/// 1 min, 4 min, 16 min … capped at 6 hours.
QDateTime nextAttempt(int attempts, const QDateTime &now);

/// Stable keys, so a retry of the same work finds its own history.
QString downloadKey(const QString &albumId, const QString &assetId);
QString uploadKey(const QString &albumId, const QString &relativePath);

} // namespace TransferBackoff

} // namespace immichksync
