#include "filesystem/FileScanner.h"

#include "core/Logging.h"
#include "filesystem/FileIdentity.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace immichksync {

bool ScannedFile::matchesIdentity(const LocalFileFingerprint &fingerprint) const
{
    return fingerprint.deviceId == deviceId && fingerprint.inode == inode
        && fingerprint.size == size && fingerprint.modifiedAtNanoseconds == modifiedAtNanoseconds;
}

LocalFileFingerprint ScannedFile::fingerprint(const Sha1Checksum &checksum) const
{
    LocalFileFingerprint fingerprint;
    fingerprint.relativePath = relativePath;
    fingerprint.deviceId = deviceId;
    fingerprint.inode = inode;
    fingerprint.size = size;
    fingerprint.modifiedAtNanoseconds = modifiedAtNanoseconds;
    fingerprint.checksum = checksum;
    return fingerprint;
}

LocalAsset ScannedFile::localAsset(const Sha1Checksum &checksum) const
{
    LocalAsset asset;
    asset.relativePath = relativePath;
    asset.filename = name;
    asset.path = path;
    asset.checksum = checksum;
    asset.size = size;
    asset.createdAt = createdAt;
    asset.modifiedAt = modifiedAt;
    asset.sidecarPath = sidecarPath;
    return asset;
}

FileScanner::FileScanner(MediaTypeCatalog mediaTypes,
                         int settleWindowSeconds,
                         std::shared_ptr<DateProvider> dateProvider)
    : m_mediaTypes(std::move(mediaTypes))
    , m_settleWindowSeconds(settleWindowSeconds)
    , m_dateProvider(std::move(dateProvider))
{
}

QString FileScanner::extensionWithDot(const QString &filename)
{
    const QString suffix = QFileInfo(filename).suffix().toLower();
    return suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix;
}

bool FileScanner::isSyncableAsset(const QString &filename) const
{
    return m_mediaTypes.assetExtensions().contains(extensionWithDot(filename));
}

QString FileScanner::sidecarFor(const QString &filename, const QHash<QString, QString> &index)
{
    const QString lower = filename.toLower();
    // `photo.HEIC.xmp` — the sidecar names the whole file.
    const auto full = index.constFind(lower + QStringLiteral(".xmp"));
    if (full != index.cend()) {
        return *full;
    }
    // `photo.xmp` — the sidecar names the stem only.
    const QString stem = QFileInfo(lower).completeBaseName();
    if (!stem.isEmpty()) {
        const auto byStem = index.constFind(stem + QStringLiteral(".xmp"));
        if (byStem != index.cend()) {
            return *byStem;
        }
    }
    return {};
}

RootScan FileScanner::scan(const QString &rootPath) const
{
    RootScan scan;

    QDir root(rootPath);
    if (!root.exists()) {
        scan.rootUnreadable = true;
        scan.errorMessage = QStringLiteral("Could not read the sync folder at %1").arg(rootPath);
        return scan;
    }

    QStringList entries = root.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
                                         | QDir::Hidden);
    // Sorted so the plan a cycle produces does not depend on directory order.
    std::sort(entries.begin(), entries.end());

    for (const QString &name : std::as_const(entries)) {
        if (AlbumFolderLayout::isInternalName(name)) {
            continue;
        }
        const QString path = root.filePath(name);
        const QFileInfo info(path);
        if (!info.isDir()) {
            if (isSyncableAsset(name)) {
                ++scan.looseFileCount;
            }
            continue;
        }
        scan.folders.append(scanAlbumFolder(path));
    }
    return scan;
}

ScannedAlbumFolder FileScanner::scanAlbumFolder(const QString &folderPath) const
{
    QDir folder(folderPath);
    ScannedAlbumFolder result;
    result.folderName = folder.dirName();
    result.path = folderPath;
    result.marker = AlbumFolderLayout::readMarker(folderPath);

    if (!folder.exists() || !folder.isReadable()) {
        log::fileSystem.warning(
            QStringLiteral("Skipping unreadable album folder %1").arg(result.folderName));
        return result;
    }

    QStringList entries =
        folder.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    std::sort(entries.begin(), entries.end());

    for (const QString &name : std::as_const(entries)) {
        result.occupiedNames.insert(name);
    }

    // Index sidecars first so each asset can claim its own in one pass.
    QHash<QString, QString> sidecarsByLowercasedName;
    for (const QString &name : std::as_const(entries)) {
        if (m_mediaTypes.sidecar.contains(extensionWithDot(name))) {
            sidecarsByLowercasedName.insert(name.toLower(), folder.filePath(name));
        }
    }

    const QDateTime now = m_dateProvider->now();

    for (const QString &name : std::as_const(entries)) {
        if (AlbumFolderLayout::isInternalName(name)) {
            continue;
        }
        const QString path = folder.filePath(name);
        if (QFileInfo(path).isDir()) {
            result.nestedDirectoryNames.append(name);
            continue;
        }
        if (!isSyncableAsset(name)) {
            continue;
        }
        const auto identity = FileIdentity::of(path);
        if (!identity) {
            continue;
        }

        // Half-copied files hash to the wrong value and would upload as corrupt assets,
        // so anything still moving is deferred to the next cycle.
        if (identity->modifiedAt.secsTo(now) < m_settleWindowSeconds) {
            ++result.settlingFileCount;
            continue;
        }

        ScannedFile file;
        file.relativePath = QStringLiteral("%1/%2").arg(result.folderName, name);
        file.path = path;
        file.name = name;
        file.size = identity->size;
        file.modifiedAt = identity->modifiedAt;
        file.modifiedAtNanoseconds = identity->modifiedAtNanoseconds;
        file.deviceId = identity->deviceId;
        file.inode = identity->inode;
        file.createdAt = identity->createdAt;
        file.sidecarPath = sidecarFor(name, sidecarsByLowercasedName);
        result.files.append(file);
    }

    return result;
}

} // namespace immichksync
