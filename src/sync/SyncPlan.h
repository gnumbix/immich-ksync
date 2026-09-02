#pragma once

#include "core/Checksum.h"
#include "filesystem/AlbumFolderLayout.h"
#include "storage/Records.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <variant>

namespace immichksync {

/// An asset as the server describes it, reduced to what reconciliation needs.
struct RemoteAsset {
    QString assetId;
    Sha1Checksum checksum;
    QString originalFileName;
    QDateTime fileCreatedAt;
    QDateTime fileModifiedAt;

    bool operator==(const RemoteAsset &other) const;
};

/// A hashed local file, ready to reconcile.
struct LocalAsset {
    QString relativePath;
    QString filename;
    /// Absolute path on disk.
    QString path;
    Sha1Checksum checksum;
    qint64 size = 0;
    QDateTime createdAt;
    QDateTime modifiedAt;
    /// XMP sidecar sitting next to the asset, if the user keeps one. Empty if none.
    QString sidecarPath;

    bool operator==(const LocalAsset &other) const;
};

/// A file to fetch and where to put it.
struct PlannedDownload {
    RemoteAsset asset;
    /// Relative to the sync root.
    QString relativePath;
    QString filename;
};

/// A file already on disk that the album is missing.
struct PlannedUpload {
    LocalAsset asset;
};

/// An entry to drop from the album because its local file is gone.
struct PlannedAlbumRemoval {
    AssetBaseline baseline;
};

/// A local file to move to the trash because its asset left the album.
struct PlannedLocalTrashing {
    AssetBaseline baseline;
    LocalAsset local;
};

/// Album-level work, evaluated before any file moves.
struct AlbumStructureAction {
    enum class Kind {
        /// A folder exists with no matching album: create one named after the folder.
        CreateRemoteAlbum,
        /// The album exists on the server but has no folder yet.
        CreateLocalFolder,
        /// The album was renamed on the server; bring the folder along.
        RenameLocalFolder,
        /// The folder was renamed locally; push the new name to the server.
        RenameRemoteAlbum,
        /// The album no longer exists on the server; retire the folder to the trash.
        TrashLocalFolder,
    };

    Kind kind = Kind::CreateLocalFolder;
    QString name;
    QString fromName;
    QString albumId;

    bool operator==(const AlbumStructureAction &other) const;
};

/// Everything one album needs this cycle.
struct AlbumPlan {
    /// Empty until the album has been created on the server.
    QString albumId;
    QString albumName;
    QString folderName;

    QList<AlbumStructureAction> structureActions;
    QList<PlannedDownload> downloads;
    QList<PlannedUpload> uploads;
    QList<PlannedAlbumRemoval> albumRemovals;
    QList<PlannedLocalTrashing> localTrashings;
    /// Both sides already agree; record or refresh the baseline without any transfer.
    QList<AssetBaseline> baselineAdoptions;
    /// Gone from both sides; forget it.
    QList<Sha1Checksum> baselineDrops;
    /// The marker file needs to be (re)written.
    bool writesMarker = false;
    /// Number of baseline entries this album had going in — the denominator the
    /// safety gate divides by.
    int baselineCount = 0;
    /// Local files skipped because an identical file is already tracked in this album.
    QStringList duplicateLocalPaths;
    /// Nested folders that cannot be represented, reported once per cycle.
    QStringList unsupportedNestedFolders;

    int removalCount() const { return albumRemovals.size() + localTrashings.size(); }
    int transferCount() const { return downloads.size() + uploads.size(); }
    bool isEmpty() const;
};

/// The whole cycle's work.
struct SyncPlan {
    QList<AlbumPlan> albums;
    /// Media files sitting directly in the sync folder rather than in an album folder.
    int looseFileCount = 0;

    int downloadCount() const;
    int uploadCount() const;
    int removalCount() const;
    int transferCount() const { return downloadCount() + uploadCount(); }
    bool isEmpty() const;

    QString summary() const;
};

} // namespace immichksync
