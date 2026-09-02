#pragma once

#include "core/Clock.h"
#include "filesystem/AlbumFolderLayout.h"
#include "immich/MediaTypeCatalog.h"
#include "storage/Records.h"
#include "sync/SyncPlan.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

namespace immichksync {

/// One candidate asset file found under the sync root.
struct ScannedFile {
    /// Path relative to the sync root, e.g. `Holiday 2024/IMG_0001.HEIC`.
    QString relativePath;
    QString path;
    QString name;
    qint64 size = 0;
    QDateTime modifiedAt;
    /// Full-precision mtime; part of the hash-cache identity, because a second-level
    /// timestamp cannot distinguish a file rewritten within the same second.
    qint64 modifiedAtNanoseconds = 0;
    qint64 deviceId = 0;
    qint64 inode = 0;
    QDateTime createdAt;
    /// XMP sidecar sitting next to the asset, if the user keeps one.
    QString sidecarPath;

    /// True when a cached fingerprint still describes this exact file, so its stored
    /// checksum can be reused instead of re-reading the whole file.
    bool matchesIdentity(const LocalFileFingerprint &fingerprint) const;

    LocalFileFingerprint fingerprint(const Sha1Checksum &checksum) const;
    LocalAsset localAsset(const Sha1Checksum &checksum) const;
};

/// One immediate child folder of the sync root.
struct ScannedAlbumFolder {
    QString folderName;
    QString path;
    std::optional<AlbumMarker> marker;
    QList<ScannedFile> files;
    /// Files skipped because they were still being written.
    int settlingFileCount = 0;
    /// Nested directories, which Immich albums cannot represent.
    QStringList nestedDirectoryNames;
    /// Every entry name in the folder, including files this app does not sync. Used
    /// when naming a download so it can never clobber something the user put there.
    QSet<QString> occupiedNames;
};

struct RootScan {
    QList<ScannedAlbumFolder> folders;
    /// Media files sitting loose at the root, outside any album folder.
    int looseFileCount = 0;
    /// Set when the root itself could not be read, so no removals may be inferred.
    bool rootUnreadable = false;
    QString errorMessage;
};

/// Walks the sync root. Deliberately shallow: Immich albums are flat, so only the
/// immediate children of the root are albums and only the immediate children of an
/// album folder are its assets. Nested directories are reported rather than flattened,
/// because flattening cannot be undone when syncing back the other way.
class FileScanner {
public:
    FileScanner(MediaTypeCatalog mediaTypes,
                int settleWindowSeconds = 5,
                std::shared_ptr<DateProvider> dateProvider = systemDateProvider());

    RootScan scan(const QString &rootPath) const;
    ScannedAlbumFolder scanAlbumFolder(const QString &folderPath) const;

    bool isSyncableAsset(const QString &filename) const;

    /// XMP sidecars come in two conventions, both of which the official CLI honours:
    /// `photo.xmp` and `photo.ext.xmp`.
    static QString sidecarFor(const QString &filename, const QHash<QString, QString> &index);

    /// Lowercased extension including the leading dot, matching `/server/media-types`.
    static QString extensionWithDot(const QString &filename);

private:
    MediaTypeCatalog m_mediaTypes;
    int m_settleWindowSeconds;
    std::shared_ptr<DateProvider> m_dateProvider;
};

} // namespace immichksync
