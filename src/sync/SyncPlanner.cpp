#include "sync/SyncPlanner.h"

#include "core/Logging.h"

#include <algorithm>

namespace immichksync {

SyncPlanner::SyncPlanner(std::shared_ptr<DateProvider> dateProvider)
    : m_dateProvider(std::move(dateProvider))
{
}

AlbumPlan SyncPlanner::plan(const AlbumPlanInput &input) const
{
    const QDateTime now = m_dateProvider->now();

    AlbumPlan plan;
    plan.albumId = input.remoteAlbum ? input.remoteAlbum->id : QString();
    plan.albumName = input.remoteAlbum ? input.remoteAlbum->name
                                       : input.folderName.value_or(QString());
    plan.folderName = input.folderName.value_or(QString());
    plan.baselineCount = static_cast<int>(input.baseline.size());
    plan.unsupportedNestedFolders = input.nestedFolderNames;

    if (!input.remoteAlbum && !input.folderName) {
        return plan; // Nothing on either side; nothing to do.
    }

    if (!input.remoteAlbum) {
        return planFolderWithoutAlbum(*input.folderName, input, std::move(plan));
    }

    if (!input.folderName) {
        plan.folderName = AlbumFolderLayout::folderName(input.remoteAlbum->name,
                                                        input.remoteAlbum->id,
                                                        input.reservedFolderNames);
        AlbumStructureAction action;
        action.kind = AlbumStructureAction::Kind::CreateLocalFolder;
        action.name = plan.folderName;
        plan.structureActions.append(action);
        plan.writesMarker = true;
    } else {
        applyRenames(*input.remoteAlbum, *input.folderName, input, plan);
    }

    reconcileAssets(input, now, plan);
    return plan;
}

// MARK: - Album structure

AlbumPlan SyncPlanner::planFolderWithoutAlbum(const QString &folderName,
                                              const AlbumPlanInput &input,
                                              AlbumPlan plan) const
{
    plan.folderName = folderName;

    const bool wasKnown = input.marker.has_value() || input.storedRecord.has_value();
    if (wasKnown) {
        // A folder we put there whose album is gone from the server: retire it. Never
        // delete — the trash folder is the strongest local action this app takes.
        AlbumStructureAction action;
        action.kind = AlbumStructureAction::Kind::TrashLocalFolder;
        action.name = folderName;
        plan.structureActions.append(action);
        plan.baselineDrops = input.baseline.keys();
        return plan;
    }

    // New folder: create an album named after it and upload everything inside.
    plan.albumName = folderName;
    AlbumStructureAction action;
    action.kind = AlbumStructureAction::Kind::CreateRemoteAlbum;
    action.name = folderName;
    plan.structureActions.append(action);
    plan.writesMarker = true;

    QList<LocalAsset> sorted = input.localAssets;
    std::sort(sorted.begin(), sorted.end(), [](const LocalAsset &a, const LocalAsset &b) {
        return a.relativePath < b.relativePath;
    });

    QSet<Sha1Checksum> seen;
    for (const LocalAsset &asset : std::as_const(sorted)) {
        if (!seen.contains(asset.checksum)) {
            seen.insert(asset.checksum);
            plan.uploads.append(PlannedUpload{asset});
        } else {
            plan.duplicateLocalPaths.append(asset.relativePath);
        }
    }
    return plan;
}

void SyncPlanner::applyRenames(const RemoteAlbumSummary &remote,
                               const QString &folderName,
                               const AlbumPlanInput &input,
                               AlbumPlan &plan) const
{
    plan.albumName = remote.name;
    plan.folderName = folderName;

    if (!input.marker) {
        // No marker: either the first time this folder is adopted, or the user deleted
        // it. Either way, write one and change nothing else.
        plan.writesMarker = true;
        return;
    }

    const bool renamedRemotely = input.marker->albumName != remote.name;
    const bool renamedLocally = input.marker->folderName != folderName;

    // Remote renames win: the server is the shared source of truth, and a folder can
    // always be renamed again locally afterwards.
    if (renamedRemotely) {
        if (renamedLocally) {
            log::sync.notice(
                QStringLiteral("Album “%1” was renamed on the server and its folder was renamed "
                               "locally; the server name wins.")
                    .arg(input.marker->albumName));
        }
        QSet<QString> reserved = input.reservedFolderNames;
        reserved.remove(folderName); // A folder never conflicts with itself.
        const QString target = AlbumFolderLayout::folderName(remote.name, remote.id, reserved);
        if (target != folderName) {
            AlbumStructureAction action;
            action.kind = AlbumStructureAction::Kind::RenameLocalFolder;
            action.fromName = folderName;
            action.name = target;
            plan.structureActions.append(action);
            plan.folderName = target;
        }
        plan.writesMarker = true;
    } else if (renamedLocally) {
        AlbumStructureAction action;
        action.kind = AlbumStructureAction::Kind::RenameRemoteAlbum;
        action.albumId = remote.id;
        action.name = folderName;
        plan.structureActions.append(action);
        plan.albumName = folderName;
        plan.writesMarker = true;
    }
}

// MARK: - Asset reconciliation

void SyncPlanner::reconcileAssets(const AlbumPlanInput &input,
                                  const QDateTime &now,
                                  AlbumPlan &plan) const
{
    if (plan.albumId.isEmpty()) {
        return;
    }

    const QHash<Sha1Checksum, RemoteAsset> remote = remoteAssetsByChecksum(input);

    QList<LocalAsset> sortedLocal = input.localAssets;
    std::sort(sortedLocal.begin(), sortedLocal.end(), [](const LocalAsset &a, const LocalAsset &b) {
        return a.relativePath < b.relativePath;
    });

    QHash<Sha1Checksum, LocalAsset> local;
    for (const LocalAsset &asset : std::as_const(sortedLocal)) {
        // Immich stores a given checksum once per album, so a second local copy is
        // redundant. It is reported, never deleted — that is the user's call.
        if (!local.contains(asset.checksum)) {
            local.insert(asset.checksum, asset);
        } else {
            plan.duplicateLocalPaths.append(asset.relativePath);
        }
    }

    const QHash<Sha1Checksum, AssetBaseline> &baseline = input.baseline;

    QSet<Sha1Checksum> union_;
    for (auto it = remote.cbegin(); it != remote.cend(); ++it) {
        union_.insert(it.key());
    }
    for (auto it = local.cbegin(); it != local.cend(); ++it) {
        union_.insert(it.key());
    }
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
        union_.insert(it.key());
    }

    // Sorted for a deterministic plan, which makes the tests meaningful and the
    // download-name assignment reproducible.
    QList<Sha1Checksum> checksums(union_.cbegin(), union_.cend());
    std::sort(checksums.begin(), checksums.end());

    QList<RemoteAsset> pendingDownloads;

    for (const Sha1Checksum &checksum : std::as_const(checksums)) {
        const auto remoteIt = remote.constFind(checksum);
        const auto localIt = local.constFind(checksum);
        const auto baselineIt = baseline.constFind(checksum);

        const bool onServer = remoteIt != remote.cend();
        const bool onDisk = localIt != local.cend();
        const bool inBaseline = baselineIt != baseline.cend();

        if (onServer && onDisk) {
            // Both sides have it. Record the baseline if it is missing or stale — a
            // stale entry means the file was renamed or the asset replaced.
            const bool isStale = !inBaseline || baselineIt->assetId != remoteIt->assetId
                || baselineIt->relativePath != localIt->relativePath
                || baselineIt->size != localIt->size;
            if (isStale) {
                AssetBaseline adopted;
                adopted.albumId = plan.albumId;
                adopted.checksum = checksum;
                adopted.assetId = remoteIt->assetId;
                adopted.originalFileName = remoteIt->originalFileName;
                adopted.relativePath = localIt->relativePath;
                adopted.size = localIt->size;
                adopted.syncedAt = now;
                plan.baselineAdoptions.append(adopted);
            }
        } else if (onServer && !onDisk && !inBaseline) {
            pendingDownloads.append(*remoteIt);
        } else if (onServer && !onDisk && inBaseline) {
            // On the server, missing locally, and we put it there: the user deleted it.
            if (!input.suppressRemovals) {
                plan.albumRemovals.append(PlannedAlbumRemoval{*baselineIt});
            }
        } else if (!onServer && onDisk && !inBaseline) {
            plan.uploads.append(PlannedUpload{*localIt});
        } else if (!onServer && onDisk && inBaseline) {
            // We downloaded it, and it has since left the album on the server.
            if (!input.suppressRemovals) {
                plan.localTrashings.append(PlannedLocalTrashing{*baselineIt, *localIt});
            }
        } else if (!onServer && !onDisk && inBaseline) {
            plan.baselineDrops.append(checksum);
        }
        // The remaining combination is unreachable: the checksum came from one of the
        // three maps, so at least one of them holds it.
    }

    plan.downloads = assignFilenames(pendingDownloads, plan.folderName, input, local);
}

QHash<Sha1Checksum, RemoteAsset> SyncPlanner::remoteAssetsByChecksum(const AlbumPlanInput &input)
{
    QList<RemoteAsset> assets;
    if (input.remoteAssets.kind == RemoteSnapshot::Kind::Enumerated) {
        assets = input.remoteAssets.assets;
    } else {
        // Membership cannot have changed, so the baseline *is* the remote set.
        assets.reserve(input.baseline.size());
        for (auto it = input.baseline.cbegin(); it != input.baseline.cend(); ++it) {
            RemoteAsset asset;
            asset.assetId = it->assetId;
            asset.checksum = it->checksum;
            asset.originalFileName = it->originalFileName;
            assets.append(asset);
        }
    }

    QHash<Sha1Checksum, RemoteAsset> byChecksum;
    byChecksum.reserve(assets.size());
    for (const RemoteAsset &asset : std::as_const(assets)) {
        if (!byChecksum.contains(asset.checksum)) {
            byChecksum.insert(asset.checksum, asset);
        }
    }
    return byChecksum;
}

QList<PlannedDownload> SyncPlanner::assignFilenames(const QList<RemoteAsset> &assets,
                                                    const QString &folderName,
                                                    const AlbumPlanInput &input,
                                                    const QHash<Sha1Checksum, LocalAsset> &local)
{
    QSet<QString> taken = input.occupiedFilenames;
    for (auto it = local.cbegin(); it != local.cend(); ++it) {
        taken.insert(it->filename);
    }

    QList<RemoteAsset> sorted = assets;
    std::sort(sorted.begin(), sorted.end(), [](const RemoteAsset &a, const RemoteAsset &b) {
        return a.checksum < b.checksum;
    });

    QList<PlannedDownload> downloads;
    downloads.reserve(sorted.size());
    for (const RemoteAsset &asset : std::as_const(sorted)) {
        const QString filename =
            AlbumFolderLayout::localFilename(asset.originalFileName, asset.checksum, taken);
        taken.insert(filename);
        PlannedDownload download;
        download.asset = asset;
        download.relativePath = QStringLiteral("%1/%2").arg(folderName, filename);
        download.filename = filename;
        downloads.append(download);
    }
    return downloads;
}

} // namespace immichksync
