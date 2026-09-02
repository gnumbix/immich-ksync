#include "sync/SyncPlan.h"

#include <QStringList>

namespace immichksync {

bool RemoteAsset::operator==(const RemoteAsset &other) const
{
    return assetId == other.assetId && checksum == other.checksum
        && originalFileName == other.originalFileName && fileCreatedAt == other.fileCreatedAt
        && fileModifiedAt == other.fileModifiedAt;
}

bool LocalAsset::operator==(const LocalAsset &other) const
{
    return relativePath == other.relativePath && filename == other.filename && path == other.path
        && checksum == other.checksum && size == other.size && createdAt == other.createdAt
        && modifiedAt == other.modifiedAt && sidecarPath == other.sidecarPath;
}

bool AlbumStructureAction::operator==(const AlbumStructureAction &other) const
{
    return kind == other.kind && name == other.name && fromName == other.fromName
        && albumId == other.albumId;
}

bool AlbumPlan::isEmpty() const
{
    return structureActions.isEmpty() && downloads.isEmpty() && uploads.isEmpty()
        && albumRemovals.isEmpty() && localTrashings.isEmpty() && baselineAdoptions.isEmpty()
        && baselineDrops.isEmpty() && !writesMarker;
}

int SyncPlan::downloadCount() const
{
    int total = 0;
    for (const AlbumPlan &album : albums) {
        total += album.downloads.size();
    }
    return total;
}

int SyncPlan::uploadCount() const
{
    int total = 0;
    for (const AlbumPlan &album : albums) {
        total += album.uploads.size();
    }
    return total;
}

int SyncPlan::removalCount() const
{
    int total = 0;
    for (const AlbumPlan &album : albums) {
        total += album.removalCount();
    }
    return total;
}

bool SyncPlan::isEmpty() const
{
    for (const AlbumPlan &album : albums) {
        if (!album.isEmpty()) {
            return false;
        }
    }
    return true;
}

QString SyncPlan::summary() const
{
    QStringList parts;
    if (downloadCount() > 0) {
        parts << QStringLiteral("%1 to download").arg(downloadCount());
    }
    if (uploadCount() > 0) {
        parts << QStringLiteral("%1 to upload").arg(uploadCount());
    }
    if (removalCount() > 0) {
        parts << QStringLiteral("%1 to remove").arg(removalCount());
    }
    return parts.isEmpty() ? QStringLiteral("nothing to do") : parts.join(QStringLiteral(", "));
}

} // namespace immichksync
