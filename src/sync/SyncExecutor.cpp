#include "sync/SyncExecutor.h"

#include "core/Logging.h"
#include "core/TaskPool.h"
#include "filesystem/AtomicFileWriter.h"
#include "filesystem/FileHasher.h"
#include "filesystem/FileIdentity.h"
#include "filesystem/LocalTrash.h"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace immichksync {

namespace {

/// Outcome of one transfer, so the concurrent map can report without sharing state.
struct TransferOutcome {
    bool succeeded = false;
    QString failure;
    /// Set for a successful upload: what the server called the asset.
    QString assetId;
    LocalAsset asset;
};

/// Hands out monotonically increasing indices to concurrent workers for progress
/// reporting, without any of them sharing mutable state.
class ProgressCounter {
public:
    int next() { return m_value.fetchAndAddOrdered(1); }

private:
    QAtomicInt m_value = 0;
};

} // namespace

SyncExecutor::SyncExecutor(ImmichClient *client,
                           SyncStore *store,
                           QString rootPath,
                           SyncSettings settings,
                           std::shared_ptr<DateProvider> dateProvider,
                           ProgressCallback reportProgress)
    : m_client(client)
    , m_store(store)
    , m_rootPath(std::move(rootPath))
    , m_settings(std::move(settings))
    , m_dateProvider(std::move(dateProvider))
    , m_reportProgress(std::move(reportProgress))
{
}

QString SyncExecutor::stagingDirectory() const
{
    return QDir(m_rootPath).filePath(QString::fromLatin1(AlbumFolderLayout::kStagingFolderName));
}

AlbumExecutionResult SyncExecutor::execute(const AlbumPlan &plan)
{
    AlbumExecutionResult result;
    result.albumId = plan.albumId;
    result.albumName = plan.albumName;
    result.folderName = plan.folderName;

    QString structureError;
    if (!applyStructure(plan, result, &structureError)) {
        result.failures.append(
            QStringLiteral("Album “%1”: %2").arg(plan.albumName, structureError));
        log::sync.error(QStringLiteral("Album structure update failed for “%1”: %2")
                            .arg(plan.albumName, structureError));
        return result;
    }
    if (result.trashedFolder) {
        return result;
    }
    if (result.albumId.isEmpty()) {
        return result;
    }

    const QString albumId = result.albumId;

    // Every baseline row is a foreign key onto `album`, so the album must be on record
    // before the first transfer commits — including on the very first cycle, when it
    // was only just discovered or created.
    persisting(QStringLiteral("album “%1”").arg(result.albumName), [&](QString *error) {
        AlbumRecord record;
        record.albumId = albumId;
        record.albumName = result.albumName;
        record.folderName = result.folderName;
        return m_store->upsert(record, error);
    });

    // Baselines for assets that were already in agreement cost nothing to record and
    // make the next cycle's diff smaller.
    for (const AssetBaseline &adoption : plan.baselineAdoptions) {
        persisting(QStringLiteral("baseline for %1").arg(adoption.originalFileName),
                   [&](QString *error) {
                       AssetBaseline copy = adoption;
                       // A plan built before the album existed carries no ID.
                       copy.albumId = albumId;
                       return m_store->upsert(copy, error);
                   });
    }
    for (const Sha1Checksum &checksum : plan.baselineDrops) {
        persisting(QStringLiteral("baseline removal"), [&](QString *error) {
            return m_store->deleteBaseline(albumId, checksum, error);
        });
    }

    runDownloads(plan.downloads, albumId, result);
    runUploads(plan.uploads, albumId, result);
    runAlbumRemovals(plan.albumRemovals, albumId, result);
    runLocalTrashings(plan.localTrashings, result.folderName, result);

    for (const QString &path : plan.duplicateLocalPaths) {
        log::sync.info(
            QStringLiteral("Skipping %1: an identical file is already in this album.").arg(path));
    }
    for (const QString &nested : plan.unsupportedNestedFolders) {
        log::sync.notice(QStringLiteral("Ignoring %1/%2/ — Immich albums are flat, so nested "
                                        "folders are not synced.")
                             .arg(result.folderName, nested));
    }

    return result;
}

// MARK: - Album structure

bool SyncExecutor::applyStructure(const AlbumPlan &plan,
                                  AlbumExecutionResult &result,
                                  QString *errorMessage)
{
    const QDir root(m_rootPath);

    for (const AlbumStructureAction &action : plan.structureActions) {
        switch (action.kind) {
        case AlbumStructureAction::Kind::CreateRemoteAlbum: {
            const auto album = m_client->createAlbum(action.name);
            if (!album.succeeded()) {
                *errorMessage = album.error.message();
                return false;
            }
            result.albumId = album->id;
            result.albumName = album->albumName;
            result.created = true;
            result.mutatedRemotely = true;
            log::sync.notice(QStringLiteral("Created album “%1” for folder %2")
                                 .arg(album->albumName, plan.folderName));
            break;
        }

        case AlbumStructureAction::Kind::CreateLocalFolder: {
            if (!AtomicFileWriter::ensureDirectory(root.filePath(action.name),
                                                   /*markAsCache=*/false,
                                                   errorMessage)) {
                return false;
            }
            result.folderName = action.name;
            log::sync.notice(
                QStringLiteral("Created folder %1 for album “%2”").arg(action.name, plan.albumName));
            break;
        }

        case AlbumStructureAction::Kind::RenameLocalFolder: {
            if (!QFile::rename(root.filePath(action.fromName), root.filePath(action.name))) {
                *errorMessage =
                    QStringLiteral("Could not rename %1 to %2").arg(action.fromName, action.name);
                return false;
            }
            result.folderName = action.name;
            log::sync.notice(QStringLiteral("Renamed folder %1 → %2 to follow the album name")
                                 .arg(action.fromName, action.name));
            break;
        }

        case AlbumStructureAction::Kind::RenameRemoteAlbum: {
            const auto album = m_client->renameAlbum(action.albumId, action.name);
            if (!album.succeeded()) {
                *errorMessage = album.error.message();
                return false;
            }
            result.albumName = album->albumName;
            result.mutatedRemotely = true;
            log::sync.notice(QStringLiteral("Renamed album to “%1” to follow the folder name")
                                 .arg(album->albumName));
            break;
        }

        case AlbumStructureAction::Kind::TrashLocalFolder: {
            const QString path = root.filePath(action.name);
            if (QFileInfo::exists(path)) {
                LocalTrash trash(m_rootPath);
                if (trash.moveFolder(path, errorMessage).isEmpty()) {
                    return false;
                }
            }
            const QString albumId = plan.albumId.isEmpty() ? result.albumId : plan.albumId;
            if (!albumId.isEmpty()) {
                m_store->deleteAlbum(albumId);
            }
            result.trashedFolder = true;
            break;
        }
        }
    }

    // Written last, so the marker only ever describes a state that actually exists.
    if (plan.writesMarker && !result.albumId.isEmpty() && !result.trashedFolder) {
        const QString folder = root.filePath(result.folderName);
        if (!AtomicFileWriter::ensureDirectory(folder, /*markAsCache=*/false, errorMessage)) {
            return false;
        }
        AlbumMarker marker;
        marker.albumId = result.albumId;
        marker.albumName = result.albumName;
        marker.folderName = result.folderName;
        if (!AlbumFolderLayout::writeMarker(marker, folder, errorMessage)) {
            return false;
        }
    }
    return true;
}

// MARK: - Downloads

void SyncExecutor::runDownloads(const QList<PlannedDownload> &downloads,
                                const QString &albumId,
                                AlbumExecutionResult &result)
{
    if (downloads.isEmpty()) {
        return;
    }

    const int total = static_cast<int>(downloads.size());
    ProgressCounter counter;
    const QDir root(m_rootPath);

    const QList<TransferOutcome> outcomes = TaskPool::map<PlannedDownload, TransferOutcome>(
        downloads,
        m_settings.downloadConcurrency,
        [&](const PlannedDownload &download) {
            if (m_reportProgress) {
                SyncState::Progress progress;
                progress.phase = SyncState::Progress::Phase::Downloading;
                progress.completed = counter.next();
                progress.total = total;
                progress.currentItem = download.filename;
                m_reportProgress(progress);
            }

            TransferOutcome outcome;
            const QString failureKey =
                TransferBackoff::downloadKey(albumId, download.asset.assetId);
            const QDateTime now = m_dateProvider->now();

            QString error;
            if (!AtomicFileWriter::ensureDirectory(stagingDirectory(), true, &error)) {
                outcome.failure = QStringLiteral("%1: %2").arg(download.filename, error);
                return outcome;
            }

            const auto fetched =
                m_client->downloadOriginal(download.asset.assetId, stagingDirectory());
            if (!fetched.succeeded()) {
                m_store->recordFailure(failureKey, fetched.error.message(), now);
                log::sync.error(QStringLiteral("Download of %1 failed: %2")
                                    .arg(download.filename, fetched.error.message()));
                outcome.failure =
                    QStringLiteral("%1: %2").arg(download.filename, fetched.error.message());
                return outcome;
            }

            // Verifying costs one read of a file that is still in the page cache, and it
            // both catches truncated downloads and yields the fingerprint for free.
            const auto actual = FileHasher::checksumOf(fetched->path, &error);
            if (!actual || *actual != download.asset.checksum) {
                QFile::remove(fetched->path);
                const QString message =
                    QStringLiteral("%1 downloaded incorrectly (expected %2, got %3); it will be "
                                   "retried.")
                        .arg(download.filename,
                             download.asset.checksum.shortHex(),
                             actual ? actual->shortHex() : QStringLiteral("nothing"));
                m_store->recordFailure(failureKey, message, now);
                outcome.failure = message;
                return outcome;
            }

            const QString destination = root.filePath(download.relativePath);
            if (!AtomicFileWriter::install(fetched->path,
                                           destination,
                                           download.asset.fileModifiedAt,
                                           /*replaceExisting=*/false,
                                           &error)) {
                QFile::remove(fetched->path);
                m_store->recordFailure(failureKey, error, now);
                outcome.failure = QStringLiteral("%1: %2").arg(download.filename, error);
                return outcome;
            }

            if (const auto identity = FileIdentity::of(destination)) {
                LocalFileFingerprint fingerprint;
                fingerprint.relativePath = download.relativePath;
                fingerprint.deviceId = identity->deviceId;
                fingerprint.inode = identity->inode;
                fingerprint.size = identity->size;
                fingerprint.modifiedAtNanoseconds = identity->modifiedAtNanoseconds;
                fingerprint.checksum = *actual;
                m_store->upsert(fingerprint, now);
            }

            AssetBaseline baseline;
            baseline.albumId = albumId;
            baseline.checksum = *actual;
            baseline.assetId = download.asset.assetId;
            baseline.originalFileName = download.asset.originalFileName;
            baseline.relativePath = download.relativePath;
            baseline.size = fetched->byteCount;
            baseline.syncedAt = now;
            m_store->upsert(baseline);
            m_store->clearFailure(failureKey);

            outcome.succeeded = true;
            return outcome;
        });

    for (const TransferOutcome &outcome : outcomes) {
        if (outcome.succeeded) {
            ++result.downloaded;
        } else if (!outcome.failure.isEmpty()) {
            result.failures.append(outcome.failure);
        }
    }
}

// MARK: - Uploads

void SyncExecutor::runUploads(const QList<PlannedUpload> &uploads,
                              const QString &albumId,
                              AlbumExecutionResult &result)
{
    if (uploads.isEmpty()) {
        return;
    }

    // Ask the server what it already has before moving a single byte. Files it
    // recognises join the album directly — this is what makes the same photo in two
    // album folders upload once and belong to both.
    QList<BulkUploadCheckItem> checkItems;
    checkItems.reserve(uploads.size());
    for (const PlannedUpload &upload : uploads) {
        checkItems.append({upload.asset.relativePath, upload.asset.checksum.hex()});
    }

    QHash<QString, QString> known; // relative path → existing asset ID
    QSet<QString> trashedPaths;
    const auto checked = m_client->bulkUploadCheck(checkItems);
    if (checked.succeeded()) {
        for (const BulkUploadCheckResult &check : *checked) {
            if (!check.isReject()) {
                continue;
            }
            if (check.isTrashed) {
                trashedPaths.insert(check.id);
            }
            if (!check.assetId.isEmpty()) {
                known.insert(check.id, check.assetId);
            }
        }
    } else {
        log::sync.warning(QStringLiteral("Duplicate pre-check failed, uploading everything "
                                         "instead: %1")
                              .arg(checked.error.message()));
    }

    QList<PlannedUpload> pending;
    for (const PlannedUpload &upload : uploads) {
        if (!known.contains(upload.asset.relativePath)) {
            pending.append(upload);
        }
    }

    const int total = static_cast<int>(pending.size());
    ProgressCounter counter;

    const QList<TransferOutcome> outcomes = TaskPool::map<PlannedUpload, TransferOutcome>(
        pending,
        m_settings.uploadConcurrency,
        [&](const PlannedUpload &upload) {
            if (m_reportProgress) {
                SyncState::Progress progress;
                progress.phase = SyncState::Progress::Phase::Uploading;
                progress.completed = counter.next();
                progress.total = total;
                progress.currentItem = upload.asset.filename;
                m_reportProgress(progress);
            }

            TransferOutcome outcome;
            outcome.asset = upload.asset;
            const QString failureKey =
                TransferBackoff::uploadKey(albumId, upload.asset.relativePath);

            ImmichClient::UploadRequest request;
            request.filePath = upload.asset.path;
            request.filename = upload.asset.filename;
            request.sha1Hex = upload.asset.checksum.hex();
            // The server treats `fileCreatedAt` as the fallback capture time when a
            // file carries no EXIF, so the earlier of the two timestamps wins.
            request.fileCreatedAt = std::min(upload.asset.createdAt, upload.asset.modifiedAt);
            request.fileModifiedAt = upload.asset.modifiedAt;
            request.sidecarPath = upload.asset.sidecarPath;

            const auto response = m_client->upload(request, stagingDirectory());
            if (!response.succeeded()) {
                m_store->recordFailure(failureKey,
                                       response.error.message(),
                                       m_dateProvider->now());
                log::sync.error(QStringLiteral("Upload of %1 failed: %2")
                                    .arg(upload.asset.relativePath, response.error.message()));
                outcome.failure = QStringLiteral("%1: %2").arg(upload.asset.filename,
                                                               response.error.message());
                return outcome;
            }

            outcome.succeeded = true;
            outcome.assetId = response->id;
            return outcome;
        });

    // Everything that now exists server-side, whether uploaded just now or already
    // present, has to be attached to this album.
    QList<QPair<QString, LocalAsset>> attachments;
    for (const TransferOutcome &outcome : outcomes) {
        if (outcome.succeeded) {
            ++result.uploaded;
            attachments.append({outcome.assetId, outcome.asset});
        } else if (!outcome.failure.isEmpty()) {
            result.failures.append(outcome.failure);
        }
    }

    for (const PlannedUpload &upload : uploads) {
        const auto it = known.constFind(upload.asset.relativePath);
        if (it == known.cend()) {
            continue;
        }
        if (trashedPaths.contains(upload.asset.relativePath)) {
            // The bytes exist server-side but sit in the Immich trash; adding a trashed
            // asset to an album would fail, and silently skipping it would leave the
            // user wondering why one photo never appears.
            const QString message =
                QStringLiteral("%1 matches an asset in the Immich trash; restore or permanently "
                               "delete it to continue.")
                    .arg(upload.asset.filename);
            log::sync.warning(message);
            result.failures.append(message);
            m_store->recordFailure(TransferBackoff::uploadKey(albumId,
                                                              upload.asset.relativePath),
                                   message,
                                   m_dateProvider->now());
            continue;
        }
        attachments.append({*it, upload.asset});
    }

    if (attachments.isEmpty()) {
        return;
    }
    if (m_reportProgress) {
        SyncState::Progress progress;
        progress.phase = SyncState::Progress::Phase::UpdatingAlbums;
        progress.total = static_cast<int>(attachments.size());
        m_reportProgress(progress);
    }

    QStringList assetIds;
    assetIds.reserve(attachments.size());
    for (const auto &attachment : attachments) {
        assetIds.append(attachment.first);
    }

    const auto added = m_client->addAssets(albumId, assetIds);
    if (!added.succeeded()) {
        result.failures.append(QStringLiteral("Adding assets to “%1” failed: %2")
                                   .arg(result.albumName, added.error.message()));
        log::sync.error(QStringLiteral("addAssets failed for %1: %2")
                            .arg(albumId, added.error.message()));
        return;
    }

    // A `duplicate` error only means the asset was already in the album, which is the
    // state we wanted; anything else is a genuine failure.
    QSet<QString> rejected;
    for (const BulkIdResponse &response : *added) {
        if (!response.success && response.error.toLower() != QLatin1String("duplicate")) {
            rejected.insert(response.id);
        }
    }
    result.mutatedRemotely = true;

    const QDateTime now = m_dateProvider->now();
    for (const auto &attachment : attachments) {
        if (rejected.contains(attachment.first)) {
            continue;
        }
        persisting(QStringLiteral("baseline for %1").arg(attachment.second.filename),
                   [&](QString *error) {
                       AssetBaseline baseline;
                       baseline.albumId = albumId;
                       baseline.checksum = attachment.second.checksum;
                       baseline.assetId = attachment.first;
                       baseline.originalFileName = attachment.second.filename;
                       baseline.relativePath = attachment.second.relativePath;
                       baseline.size = attachment.second.size;
                       baseline.syncedAt = now;
                       return m_store->upsert(baseline, error);
                   });
        m_store->clearFailure(TransferBackoff::uploadKey(albumId, attachment.second.relativePath));
    }

    if (!rejected.isEmpty()) {
        result.failures.append(QStringLiteral("%1 asset(s) could not be added to “%2”.")
                                   .arg(rejected.size())
                                   .arg(result.albumName));
    }
}

// MARK: - Removals

void SyncExecutor::runAlbumRemovals(const QList<PlannedAlbumRemoval> &removals,
                                    const QString &albumId,
                                    AlbumExecutionResult &result)
{
    if (removals.isEmpty()) {
        return;
    }

    QStringList assetIds;
    assetIds.reserve(removals.size());
    for (const PlannedAlbumRemoval &removal : removals) {
        assetIds.append(removal.baseline.assetId);
    }

    const auto response = m_client->removeAssets(albumId, assetIds);
    if (!response.succeeded()) {
        result.failures.append(QStringLiteral("Removing assets from “%1” failed: %2")
                                   .arg(result.albumName, response.error.message()));
        return;
    }

    QSet<QString> failed;
    for (const BulkIdResponse &entry : *response) {
        if (!entry.success) {
            failed.insert(entry.id);
        }
    }
    result.mutatedRemotely = true;

    for (const PlannedAlbumRemoval &removal : removals) {
        if (failed.contains(removal.baseline.assetId)) {
            continue;
        }
        m_store->deleteBaseline(albumId, removal.baseline.checksum);
        m_store->deleteFingerprint(removal.baseline.relativePath);
        ++result.removedFromAlbum;
    }

    log::sync.notice(QStringLiteral("Removed %1 deleted file(s) from album “%2”. The assets "
                                    "remain in your Immich library.")
                         .arg(result.removedFromAlbum)
                         .arg(result.albumName));
}

void SyncExecutor::runLocalTrashings(const QList<PlannedLocalTrashing> &trashings,
                                     const QString &folderName,
                                     AlbumExecutionResult &result)
{
    if (trashings.isEmpty()) {
        return;
    }

    LocalTrash trash(m_rootPath);
    for (const PlannedLocalTrashing &trashing : trashings) {
        QString error;
        if (trash.moveFile(trashing.local.path, folderName, &error).isEmpty()) {
            result.failures.append(QStringLiteral("%1: %2").arg(trashing.local.filename, error));
            continue;
        }
        m_store->deleteBaseline(trashing.baseline.albumId, trashing.baseline.checksum);
        m_store->deleteFingerprint(trashing.local.relativePath);
        ++result.movedToTrash;
    }
}

} // namespace immichksync
