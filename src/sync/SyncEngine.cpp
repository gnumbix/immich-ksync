#include "sync/SyncEngine.h"

#include "core/Logging.h"
#include "core/TaskPool.h"
#include "filesystem/FileHasher.h"
#include "filesystem/FileScanner.h"
#include "filesystem/RootFolderAccess.h"

#include <QAtomicInt>
#include <QDir>
#include <QElapsedTimer>
#include <QtCore/qscopeguard.h>

#include <algorithm>

namespace immichksync {

namespace {

/// FolderWatcher already coalesces; this waits for the user to actually finish copying.
constexpr int kFolderChangeDebounceMs = 10000;
/// Identifies which account and server the stored baselines belong to.
constexpr const char *kAccountIdentityKey = "accountIdentity";

} // namespace

QString describe(SyncTrigger trigger)
{
    switch (trigger) {
    case SyncTrigger::Scheduled: return QStringLiteral("scheduled");
    case SyncTrigger::FolderChanged: return QStringLiteral("folder change");
    case SyncTrigger::Manual: return QStringLiteral("manual");
    case SyncTrigger::ConfigurationChanged: return QStringLiteral("configuration change");
    case SyncTrigger::WokeFromSleep: return QStringLiteral("wake from sleep");
    }
    return {};
}

SyncEngine::SyncEngine(SyncStore *store,
                       Transport *transport,
                       UserNotifier *notifier,
                       std::shared_ptr<DateProvider> dateProvider,
                       QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_transport(transport)
    , m_notifier(notifier)
    , m_dateProvider(std::move(dateProvider))
    , m_planner(m_dateProvider)
    , m_intervalTimer(new QTimer(this))
    , m_debounceTimer(new QTimer(this))
    , m_watcher(new FolderWatcher(this))
{
    qRegisterMetaType<SyncTrigger>("immichksync::SyncTrigger");

    m_intervalTimer->setSingleShot(false);
    connect(m_intervalTimer, &QTimer::timeout, this, &SyncEngine::handleScheduledTick);

    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(kFolderChangeDebounceMs);
    connect(m_debounceTimer, &QTimer::timeout, this, &SyncEngine::flushFolderChanges);

    connect(m_watcher, &FolderWatcher::changed, this, &SyncEngine::handleFolderChange);
}

SyncEngine::~SyncEngine() = default;

// MARK: - Lifecycle

void SyncEngine::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_intervalTimer->start(m_settings.syncIntervalSeconds * 1000);
    m_watcher->start(m_settings.rootFolder);
    log::sync.info(QStringLiteral("Sync engine started"));
    // Run once at startup so a fresh launch reconciles immediately.
    trigger(SyncTrigger::Scheduled);
}

void SyncEngine::requestStop()
{
    m_stopRequested.storeRelease(1);
}

bool SyncEngine::isStopRequested() const
{
    return m_stopRequested.loadAcquire() != 0;
}

void SyncEngine::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_intervalTimer->stop();
    m_debounceTimer->stop();
    m_watcher->stop();
    m_pendingFolderChanges.clear();
    log::sync.info(QStringLiteral("Sync engine stopped"));
}

void SyncEngine::applyConfiguration(const SyncSettings &settings,
                                    const std::optional<ImmichCredentials> &credentials)
{
    const bool rootChanged = m_settings.rootFolder != settings.rootFolder;
    const bool serverChanged =
        m_settings.apiBaseUrl != settings.apiBaseUrl || m_credentials != credentials;

    m_settings = settings;
    m_credentials = credentials;
    if (serverChanged) {
        // A different server or account means everything the profile recorded is stale.
        m_profile.reset();
        Q_EMIT serverProfileChanged(std::nullopt);
    }

    if (m_running) {
        m_intervalTimer->start(m_settings.syncIntervalSeconds * 1000);
        if (rootChanged) {
            // The watcher is bound to a specific root, so re-point it rather than
            // leaving it watching a folder the user has moved away from.
            m_watcher->start(m_settings.rootFolder);
        }
    }

    updateStatistics();
    trigger(SyncTrigger::ConfigurationChanged);
}

void SyncEngine::trigger(SyncTrigger reason)
{
    if (m_inCycle) {
        // One cycle at a time: remember that another is wanted and run it after.
        m_pending = true;
        m_pendingReason = reason;
        return;
    }
    runOnce(reason);
}

void SyncEngine::handleScheduledTick()
{
    trigger(SyncTrigger::Scheduled);
}

void SyncEngine::handleFolderChange(const FolderChange &change)
{
    m_pendingFolderChanges.insert(change.kind == FolderChange::Kind::AlbumFolder
                                      ? change.folderName
                                      : QString());
    m_debounceTimer->start();
}

void SyncEngine::flushFolderChanges()
{
    if (m_pendingFolderChanges.isEmpty()) {
        return;
    }
    const int count = static_cast<int>(m_pendingFolderChanges.size());
    m_pendingFolderChanges.clear();
    log::sync.debug(QStringLiteral("%1 folder change(s) settled").arg(count));
    trigger(SyncTrigger::FolderChanged);
}

void SyncEngine::applyHeldRemovals(const QString &albumId)
{
    m_safetyOverrides.insert(albumId);
    m_store->clearHeldRemovals(albumId);
    m_store->setSafetyHold(false, albumId);
    refreshSafetyHolds();
    trigger(SyncTrigger::Manual);
}

void SyncEngine::restoreHeldRemovals(const QString &albumId)
{
    int restored = 0;
    for (const HeldRemoval &removal : m_store->heldRemovals()) {
        if (removal.albumId != albumId
            || removal.direction != HeldRemoval::Direction::RemoveFromAlbum) {
            continue;
        }
        // Dropping the baseline turns "missing locally" into "new on the server", which
        // the next cycle resolves by downloading it again.
        m_store->deleteBaseline(albumId, removal.checksum);
        ++restored;
    }
    m_store->clearHeldRemovals(albumId);
    m_store->setSafetyHold(false, albumId);
    refreshSafetyHolds();
    log::sync.notice(
        QStringLiteral("Restoring %1 file(s) in album %2 from Immich").arg(restored).arg(albumId));
    trigger(SyncTrigger::Manual);
}

// MARK: - One cycle

QString SyncEngine::missingConfigurationReason() const
{
    if (m_settings.apiBaseUrl.isEmpty()) {
        return QStringLiteral("Enter your Immich server address in Settings.");
    }
    if (!m_credentials) {
        return QStringLiteral("Sign in to your Immich server in Settings.");
    }
    if (m_settings.rootFolder.isEmpty()) {
        return QStringLiteral("Choose a sync folder in Settings.");
    }
    const auto validation = RootFolderAccess::validate(m_settings.rootFolder);
    if (!RootFolderAccess::isUsable(validation)) {
        return RootFolderAccess::message(validation);
    }
    return QStringLiteral("Finish setup in Settings.");
}

void SyncEngine::runOnce(SyncTrigger reason)
{
    if (m_inCycle) {
        // One cycle at a time: remember that another is wanted and run it after.
        m_pending = true;
        m_pendingReason = reason;
        return;
    }

    if (isStopRequested()) {
        return;
    }
    m_inCycle = true;
    const auto finish = qScopeGuard([this]() { m_inCycle = false; });

    // Drained in a loop rather than by recursing: a large copy into the sync folder
    // fires a trigger during every cycle it causes, and recursion would add a stack
    // frame per cycle for as long as that kept up.
    SyncTrigger current = reason;
    while (true) {
        runCycle(current);
        if (!m_pending || isStopRequested()) {
            return;
        }
        m_pending = false;
        current = m_pendingReason;
    }
}

void SyncEngine::runCycle(SyncTrigger reason)
{
    if (m_settings.apiBaseUrl.isEmpty() || !m_credentials || m_settings.rootFolder.isEmpty()) {
        setState(SyncState::notConfigured(missingConfigurationReason()));
        return;
    }

    const auto validation = RootFolderAccess::validate(m_settings.rootFolder);
    if (!RootFolderAccess::isUsable(validation)) {
        // A missing volume must pause the sync, never look like a mass deletion.
        const QString message = RootFolderAccess::message(validation);
        setState(SyncState::failed(message));
        log::sync.warning(QStringLiteral("Skipping cycle: %1").arg(message));
        return;
    }

    setState(SyncState::preparing());
    const QDateTime startedAt = m_dateProvider->now();
    QElapsedTimer elapsed;
    elapsed.start();
    log::sync.info(QStringLiteral("Sync cycle starting (%1)").arg(describe(reason)));

    SyncCycleSummary summary;
    reconcile(m_settings.rootFolder, startedAt, summary);
    summary.finishedAt = m_dateProvider->now();
    summary.durationSeconds = static_cast<double>(elapsed.elapsed()) / 1000.0;

    log::sync.info(QStringLiteral("Sync cycle finished in %1s — %2")
                       .arg(summary.durationSeconds, 0, 'f', 1)
                       .arg(summary.headline()));
    Q_EMIT cycleFinished(summary);
    if (summary.failures == 0) {
        Q_EMIT errorMessageChanged(QString());
    }
    setState(SyncState::idle());
    updateStatistics();
    // The set of album folders may have changed, so the watcher's per-folder watches
    // have to follow.
    m_watcher->refresh();
    Q_EMIT albumsChanged();
}

void SyncEngine::reconcile(const QString &rootPath,
                           const QDateTime &startedAt,
                           SyncCycleSummary &summary)
{
    Q_UNUSED(startedAt)

    const auto profile = ensureProfile();
    if (!profile) {
        // Counted, because the caller clears the last error message when a cycle
        // reports none — which would wipe the explanation ensureProfile just set.
        ++summary.failures;
        return;
    }
    ImmichClient client(m_settings.apiBaseUrl, m_credentials, m_transport);

    // 1. Remote inventory — one cheap call that also tells us which albums moved.
    const auto remoteAlbums = client.ownedAlbums();
    if (!remoteAlbums.succeeded()) {
        const QString message = remoteAlbums.error.isAuthenticationFailure()
            ? QStringLiteral("Sign-in failed. Check your credentials in Settings ▸ Server.")
            : remoteAlbums.error.message();
        if (remoteAlbums.error.isAuthenticationFailure()) {
            // Credentials will not fix themselves; stop retrying against them.
            m_profile.reset();
            if (m_notifier) {
                m_notifier->postAuthenticationFailure();
            }
        }
        log::sync.error(QStringLiteral("Sync cycle failed: %1").arg(message));
        setState(SyncState::failed(message));
        Q_EMIT errorMessageChanged(message);
        ++summary.failures;
        return;
    }

    const QSet<QString> excluded = m_store->excludedAlbumIds();
    QList<AlbumResponse> syncable;
    for (const AlbumResponse &album : *remoteAlbums) {
        if (!excluded.contains(album.id)) {
            syncable.append(album);
        }
    }

    // 2. Local inventory.
    const FileScanner scanner(profile->mediaTypes, m_settings.settleWindowSeconds, m_dateProvider);
    const RootScan scan = scanner.scan(rootPath);
    if (scan.rootUnreadable) {
        setState(SyncState::failed(scan.errorMessage));
        Q_EMIT errorMessageChanged(scan.errorMessage);
        ++summary.failures;
        return;
    }
    if (scan.looseFileCount > 0) {
        log::sync.notice(QStringLiteral("%1 media file(s) sit directly in the sync folder and are "
                                        "not part of any album; move them into an album folder to "
                                        "sync them.")
                             .arg(scan.looseFileCount));
    }

    const QList<AlbumPairing> pairings = pair(syncable, scan);

    // Album names are not unique in Immich, and a folder name has to be. Claims are
    // accumulated as folders are assigned, so two identically named albums cannot both
    // be planned into the same folder within one cycle.
    QSet<QString> claimedFolderNames;
    for (const ScannedAlbumFolder &folder : scan.folders) {
        claimedFolderNames.insert(folder.folderName);
    }
    for (const AlbumRecord &album : m_store->albums()) {
        claimedFolderNames.insert(album.folderName);
    }

    // 3. Reconcile album by album, committing as we go.
    for (const AlbumPairing &pairing : pairings) {
        if (isStopRequested()) {
            log::sync.info(QStringLiteral("Stopping the cycle early: shutdown was requested."));
            return;
        }
        QSet<QString> reserved = claimedFolderNames;
        if (pairing.folder) {
            // A folder never conflicts with itself.
            reserved.remove(pairing.folder->folderName);
        }

        const AlbumExecutionResult result =
            reconcileAlbum(pairing, reserved, rootPath, *profile, client);

        claimedFolderNames.insert(result.folderName);
        summary.uploaded += result.uploaded;
        summary.downloaded += result.downloaded;
        summary.removedFromAlbums += result.removedFromAlbum;
        summary.movedToTrash += result.movedToTrash;
        summary.albumsCreated += result.created ? 1 : 0;
        summary.failures += static_cast<int>(result.failures.size());
        for (const QString &failure : result.failures) {
            log::sync.warning(failure);
        }
    }
}

// MARK: - Pairing albums to folders

QList<SyncEngine::AlbumPairing> SyncEngine::pair(const QList<AlbumResponse> &remoteAlbums,
                                                 const RootScan &scan)
{
    const QList<AlbumRecord> stored = m_store->albums();
    QHash<QString, AlbumRecord> storedById;
    QHash<QString, AlbumRecord> storedByFolder;
    for (const AlbumRecord &record : stored) {
        storedById.insert(record.albumId, record);
        if (!storedByFolder.contains(record.folderName)) {
            storedByFolder.insert(record.folderName, record);
        }
    }

    QHash<QString, ScannedAlbumFolder> folderByAlbumId;
    QList<ScannedAlbumFolder> unmatchedFolders;

    // Identity comes from the marker first, then from the stored folder name — never
    // from the album name, which the user is free to change on either side.
    for (const ScannedAlbumFolder &folder : scan.folders) {
        QString albumId;
        if (folder.marker) {
            albumId = folder.marker->albumId;
        } else if (const auto it = storedByFolder.constFind(folder.folderName);
                   it != storedByFolder.cend()) {
            albumId = it->albumId;
        }
        if (albumId.isEmpty()) {
            unmatchedFolders.append(folder);
            continue;
        }
        if (const auto existing = folderByAlbumId.constFind(albumId);
            existing != folderByAlbumId.cend()) {
            log::sync.warning(QStringLiteral("Both “%1” and “%2” claim album %3; “%2” will be left "
                                             "alone. Remove its %4 to sync it as a new album.")
                                  .arg(existing->folderName,
                                       folder.folderName,
                                       albumId,
                                       QString::fromLatin1(AlbumFolderLayout::kMarkerFilename)));
            continue;
        }
        folderByAlbumId.insert(albumId, folder);
    }

    QList<AlbumPairing> pairings;
    QSet<QString> remoteIds;

    for (const AlbumResponse &album : remoteAlbums) {
        remoteIds.insert(album.id);
        AlbumPairing pairing;
        pairing.remote = album;
        if (const auto it = folderByAlbumId.constFind(album.id); it != folderByAlbumId.cend()) {
            pairing.folder = *it;
        }
        if (const auto it = storedById.constFind(album.id); it != storedById.cend()) {
            pairing.stored = *it;
        }
        pairings.append(pairing);
    }

    // Folders whose album is gone from the server, plus brand-new folders.
    for (auto it = folderByAlbumId.cbegin(); it != folderByAlbumId.cend(); ++it) {
        if (remoteIds.contains(it.key())) {
            continue;
        }
        AlbumPairing pairing;
        pairing.folder = it.value();
        if (const auto stored = storedById.constFind(it.key()); stored != storedById.cend()) {
            pairing.stored = *stored;
        }
        pairings.append(pairing);
    }
    for (const ScannedAlbumFolder &folder : unmatchedFolders) {
        AlbumPairing pairing;
        pairing.folder = folder;
        pairings.append(pairing);
    }
    return pairings;
}

// MARK: - Reconciling one album

AlbumExecutionResult SyncEngine::reconcileAlbum(const AlbumPairing &pairing,
                                                const QSet<QString> &reservedFolderNames,
                                                const QString &rootPath,
                                                const ServerProfile &profile,
                                                ImmichClient &client)
{
    const QDateTime now = m_dateProvider->now();
    QString albumId;
    if (pairing.remote) {
        albumId = pairing.remote->id;
    } else if (pairing.stored) {
        albumId = pairing.stored->albumId;
    }

    QHash<Sha1Checksum, AssetBaseline> baseline;
    if (!albumId.isEmpty()) {
        baseline = m_store->baseline(albumId);
    }

    // Remote side: enumerate only when something can have moved.
    RemoteSnapshot remoteSnapshot = RemoteSnapshot::unchanged();
    bool deepScanned = false;
    if (pairing.remote) {
        if (needsDeepScan(*pairing.remote, pairing.stored, now)) {
            SyncState::Progress progress;
            progress.phase = SyncState::Progress::Phase::Scanning;
            progress.currentItem = pairing.remote->albumName;
            setState(SyncState::working(progress));

            const auto assets =
                client.albumAssets(pairing.remote->id, profile.supportsCursorPagination());
            if (assets.succeeded()) {
                QList<RemoteAsset> converted;
                converted.reserve(assets->size());
                for (const AssetResponse &response : *assets) {
                    const auto checksum = Sha1Checksum::fromEncoded(response.checksum);
                    if (!checksum) {
                        // A checksum in neither encoding cannot be matched to anything;
                        // skipping the asset is recoverable, guessing would not be.
                        continue;
                    }
                    RemoteAsset asset;
                    asset.assetId = response.id;
                    asset.checksum = *checksum;
                    asset.originalFileName = response.originalFileName;
                    asset.fileCreatedAt = fromImmichIso8601(response.fileCreatedAt);
                    asset.fileModifiedAt = fromImmichIso8601(response.fileModifiedAt);
                    converted.append(asset);
                }
                remoteSnapshot = RemoteSnapshot::enumerated(converted);
                deepScanned = true;
            } else {
                AlbumExecutionResult failure;
                failure.albumId = albumId;
                failure.albumName = pairing.remote->albumName;
                failure.folderName = pairing.folder ? pairing.folder->folderName : QString();
                failure.failures.append(QStringLiteral("Could not list “%1”: %2")
                                            .arg(pairing.remote->albumName,
                                                 assets.error.message()));
                return failure;
            }
        }
    } else {
        remoteSnapshot = RemoteSnapshot::enumerated({});
    }

    // Local side: hash whatever the cache cannot vouch for.
    QList<LocalAsset> localAssets;
    int hashFailures = 0;
    if (pairing.folder) {
        const HashOutcome hashed = hashFiles(pairing.folder->files, pairing.folder->folderName);
        localAssets = hashed.assets;
        hashFailures = hashed.failures;

        QSet<QString> livePaths;
        for (const ScannedFile &file : pairing.folder->files) {
            livePaths.insert(file.relativePath);
        }
        m_store->pruneFingerprints(pairing.folder->folderName, livePaths);
    }

    AlbumPlanInput input;
    if (pairing.remote) {
        RemoteAlbumSummary summary;
        summary.id = pairing.remote->id;
        summary.name = pairing.remote->albumName;
        summary.updatedAt = pairing.remote->updatedAt;
        summary.assetCount = pairing.remote->assetCount;
        input.remoteAlbum = summary;
    }
    if (pairing.folder) {
        input.folderName = pairing.folder->folderName;
        input.marker = pairing.folder->marker;
        input.occupiedFilenames = pairing.folder->occupiedNames;
        input.nestedFolderNames = pairing.folder->nestedDirectoryNames;
    }
    input.storedRecord = pairing.stored;
    input.remoteAssets = remoteSnapshot;
    input.localAssets = localAssets;
    input.baseline = baseline;
    input.reservedFolderNames = reservedFolderNames;
    // A file that is still copying, or that could not be read, is missing from the local
    // set for reasons that have nothing to do with the user deleting it — so no removals
    // may be inferred from this cycle.
    const int settling = pairing.folder ? pairing.folder->settlingFileCount : 0;
    input.suppressRemovals = settling > 0 || hashFailures > 0;
    if (input.suppressRemovals) {
        log::sync.info(QStringLiteral("Deferring any removals in “%1”: %2 file(s) still settling, "
                                      "%3 unreadable.")
                           .arg(input.folderName.value_or(QStringLiteral("?")))
                           .arg(settling)
                           .arg(hashFailures));
    }

    AlbumPlan plan = m_planner.plan(input);

    // Items that have failed repeatedly are held back until their back-off expires, so
    // one unreadable file cannot occupy a transfer slot on every single cycle.
    if (!albumId.isEmpty()) {
        const QSet<QString> backedOff = m_store->backedOffKeys(now);
        if (!backedOff.isEmpty()) {
            const auto downloadsBefore = plan.downloads.size();
            const auto uploadsBefore = plan.uploads.size();
            plan.downloads.removeIf([&](const PlannedDownload &download) {
                return backedOff.contains(
                    TransferBackoff::downloadKey(albumId, download.asset.assetId));
            });
            plan.uploads.removeIf([&](const PlannedUpload &upload) {
                return backedOff.contains(
                    TransferBackoff::uploadKey(albumId, upload.asset.relativePath));
            });
            const auto deferred = (downloadsBefore - plan.downloads.size())
                + (uploadsBefore - plan.uploads.size());
            if (deferred > 0) {
                log::sync.info(QStringLiteral("Skipping %1 item(s) in “%2” that are waiting out a "
                                              "retry back-off.")
                                   .arg(deferred)
                                   .arg(plan.albumName));
            }
        }
    }

    // Safety gate.
    if (!albumId.isEmpty() && m_safetyOverrides.contains(albumId)) {
        m_safetyOverrides.remove(albumId);
        log::sync.notice(QStringLiteral("Safety hold overridden for “%1” by user confirmation.")
                             .arg(plan.albumName));
    } else {
        const SafetyGate::Outcome outcome = m_settings.safetyGate.apply(plan, now);
        if (outcome.verdict.isHeld && !albumId.isEmpty()) {
            m_store->setSafetyHold(true, albumId);
            m_store->replaceHeldRemovals(albumId, outcome.held);
            if (m_notifier) {
                m_notifier->postSafetyHold(plan.albumName,
                                           outcome.verdict.removals,
                                           outcome.verdict.tracked);
            }
        } else if (!albumId.isEmpty()) {
            m_store->setSafetyHold(false, albumId);
            m_store->clearHeldRemovals(albumId);
        }
    }

    if (plan.isEmpty() && !deepScanned) {
        AlbumExecutionResult result;
        result.albumId = albumId;
        result.albumName = plan.albumName;
        result.folderName = plan.folderName;
        return result;
    }

    SyncExecutor executor(&client,
                          m_store,
                          rootPath,
                          m_settings,
                          m_dateProvider,
                          [this](const SyncState::Progress &progress) {
                              setState(SyncState::working(progress));
                          });
    const AlbumExecutionResult result = executor.execute(plan);

    persistAlbum(result, pairing, deepScanned, client);
    refreshSafetyHolds();
    return result;
}

void SyncEngine::persistAlbum(const AlbumExecutionResult &result,
                              const AlbumPairing &pairing,
                              bool deepScanned,
                              ImmichClient &client)
{
    if (result.albumId.isEmpty() || result.trashedFolder) {
        return;
    }
    const QDateTime now = m_dateProvider->now();

    // Our own writes bump `updatedAt`, so re-read rather than storing a value that
    // would make the next cycle think a third party changed something.
    QString updatedAt = pairing.remote ? pairing.remote->updatedAt : QString();
    bool hasUpdatedAt = pairing.remote.has_value();
    int assetCount = pairing.remote ? pairing.remote->assetCount : 0;

    if (result.mutatedRemotely || result.created) {
        const auto refreshed = client.album(result.albumId);
        if (refreshed.succeeded()) {
            updatedAt = refreshed->updatedAt;
            hasUpdatedAt = true;
            assetCount = refreshed->assetCount;
        } else {
            // Unknown: force a deep scan next time rather than trusting a stale value.
            hasUpdatedAt = false;
            updatedAt.clear();
        }
    }

    AlbumRecord record;
    record.albumId = result.albumId;
    record.albumName = result.albumName;
    record.folderName = result.folderName;
    record.remoteUpdatedAt = updatedAt;
    record.hasRemoteUpdatedAt = hasUpdatedAt;
    record.remoteAssetCount = assetCount;
    record.lastDeepScanAt = deepScanned ? now
                                        : (pairing.stored ? pairing.stored->lastDeepScanAt
                                                          : QDateTime());
    record.lastSyncedAt = now;
    record.isExcluded = pairing.stored && pairing.stored->isExcluded;
    record.hasSafetyHold = pairing.stored && pairing.stored->hasSafetyHold;
    m_store->upsert(record);
}

bool SyncEngine::needsDeepScan(const AlbumResponse &remote,
                               const std::optional<AlbumRecord> &stored,
                               const QDateTime &now) const
{
    if (!stored) {
        return true;
    }
    if (!stored->hasRemoteUpdatedAt || stored->remoteUpdatedAt != remote.updatedAt) {
        return true;
    }
    if (stored->remoteAssetCount != remote.assetCount) {
        return true;
    }
    if (!stored->lastDeepScanAt.isValid()) {
        return true;
    }
    return stored->lastDeepScanAt.secsTo(now) >= m_settings.deepScanIntervalSeconds;
}

// MARK: - Hashing

SyncEngine::HashOutcome SyncEngine::hashFiles(const QList<ScannedFile> &files,
                                              const QString &folderName)
{
    HashOutcome outcome;
    if (files.isEmpty()) {
        return outcome;
    }

    const QHash<QString, LocalFileFingerprint> cached = m_store->fingerprints(folderName);
    const QDateTime now = m_dateProvider->now();

    // Files whose identity is unchanged never need re-reading, which is what keeps a
    // large library's cycle proportional to what changed rather than to its size.
    QList<ScannedFile> pending;
    for (const ScannedFile &file : files) {
        const auto entry = cached.constFind(file.relativePath);
        if (entry != cached.cend() && file.matchesIdentity(*entry)) {
            outcome.assets.append(file.localAsset(entry->checksum));
        } else {
            pending.append(file);
        }
    }
    if (pending.isEmpty()) {
        return outcome;
    }

    const int total = static_cast<int>(pending.size());
    QAtomicInt counter = 0;

    struct HashResult {
        bool succeeded = false;
        LocalAsset asset;
    };

    const QList<HashResult> results = TaskPool::map<ScannedFile, HashResult>(
        pending,
        4,
        [&](const ScannedFile &file) {
            SyncState::Progress progress;
            progress.phase = SyncState::Progress::Phase::Hashing;
            progress.completed = counter.fetchAndAddOrdered(1);
            progress.total = total;
            progress.currentItem = file.name;
            setState(SyncState::working(progress));

            QString error;
            const auto checksum = FileHasher::checksumOf(file.path, &error);
            if (!checksum) {
                log::fileSystem.warning(
                    QStringLiteral("Could not hash %1: %2").arg(file.relativePath, error));
                return HashResult{};
            }
            m_store->upsert(file.fingerprint(*checksum), now);
            return HashResult{true, file.localAsset(*checksum)};
        });

    for (const HashResult &result : results) {
        if (result.succeeded) {
            outcome.assets.append(result.asset);
        } else {
            ++outcome.failures;
        }
    }
    return outcome;
}

// MARK: - Server profile

std::optional<ServerProfile> SyncEngine::ensureProfile()
{
    if (m_profile) {
        return m_profile;
    }
    if (m_settings.apiBaseUrl.isEmpty() || !m_credentials) {
        return std::nullopt;
    }

    const auto probed = ServerDiscovery::probe(m_settings.apiBaseUrl, *m_credentials, m_transport);
    if (!probed.succeeded()) {
        const QString message = probed.error.isAuthenticationFailure()
            ? QStringLiteral("Sign-in failed. Check your credentials in Settings ▸ Server.")
            : probed.error.message();
        log::sync.error(QStringLiteral("Sync cycle failed: %1").arg(message));
        setState(SyncState::failed(message));
        Q_EMIT errorMessageChanged(message);
        if (probed.error.isAuthenticationFailure() && m_notifier) {
            m_notifier->postAuthenticationFailure();
        }
        return std::nullopt;
    }

    m_profile = *probed;
    Q_EMIT serverProfileChanged(m_profile);

    // Baselines describe one account's albums on one server. Carrying them across a
    // sign-in as a different user would make another account's assets look like local
    // deletions, so the state is dropped rather than reinterpreted.
    const QString identity =
        QStringLiteral("%1#%2").arg(m_settings.apiBaseUrl.toString(), probed->user.id);
    const auto previous = m_store->metaValue(QLatin1String(kAccountIdentityKey));
    if (previous && *previous != identity) {
        log::sync.notice(QStringLiteral("Signed-in account or server changed; clearing local sync "
                                        "state before continuing."));
        m_store->reset();
    }
    m_store->setMetaValue(QLatin1String(kAccountIdentityKey), identity);

    if (!probed->missingPermissions.isEmpty()) {
        QStringList names;
        for (const ImmichPermission permission : probed->missingPermissions) {
            names.append(keyFor(permission));
        }
        log::api.warning(
            QStringLiteral("API key is missing permissions: %1").arg(names.join(QStringLiteral(", "))));
    }
    log::api.info(QStringLiteral("Connected to Immich %1 as %2 (%3 pagination)")
                      .arg(probed->version.toString(),
                           probed->user.email,
                           probed->supportsCursorPagination() ? QStringLiteral("cursor")
                                                              : QStringLiteral("page")));
    return m_profile;
}

// MARK: - Status plumbing

void SyncEngine::setState(const SyncState &state)
{
    Q_EMIT stateChanged(state);
}

void SyncEngine::updateStatistics()
{
    refreshSafetyHolds();
    Q_EMIT statisticsChanged(m_store->statistics());
}

void SyncEngine::refreshSafetyHolds()
{
    const QSet<QString> heldIds = m_store->albumsWithHeldRemovals();
    QStringList names;
    for (const AlbumRecord &album : m_store->albums()) {
        if (heldIds.contains(album.albumId)) {
            names.append(album.albumName);
        }
    }
    names.sort();
    Q_EMIT safetyHoldsChanged(names);
}

} // namespace immichksync
