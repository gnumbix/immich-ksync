#pragma once

#include "core/Clock.h"
#include "core/Preferences.h"
#include "immich/ImmichClient.h"
#include "storage/SyncStore.h"
#include "sync/SyncPlan.h"
#include "sync/SyncStatus.h"

#include <functional>
#include <memory>

namespace immichksync {

/// What one album's execution actually accomplished.
struct AlbumExecutionResult {
    QString albumId;
    QString albumName;
    QString folderName;
    bool created = false;
    int uploaded = 0;
    int downloaded = 0;
    int removedFromAlbum = 0;
    int movedToTrash = 0;
    QStringList failures;
    /// True when this cycle changed the album server-side, so its `updatedAt` and
    /// `assetCount` must be re-read rather than trusted.
    bool mutatedRemotely = false;
    bool trashedFolder = false;
};

/// Carries out an `AlbumPlan`.
///
/// Every unit of work commits its own baseline row as soon as it succeeds, so an
/// interrupted cycle resumes from where it stopped instead of repeating transfers.
class SyncExecutor {
public:
    using ProgressCallback = std::function<void(const SyncState::Progress &)>;

    SyncExecutor(ImmichClient *client,
                 SyncStore *store,
                 QString rootPath,
                 SyncSettings settings,
                 std::shared_ptr<DateProvider> dateProvider,
                 ProgressCallback reportProgress);

    AlbumExecutionResult execute(const AlbumPlan &plan);

private:
    bool applyStructure(const AlbumPlan &plan, AlbumExecutionResult &result, QString *errorMessage);
    void runDownloads(const QList<PlannedDownload> &downloads,
                      const QString &albumId,
                      AlbumExecutionResult &result);
    void runUploads(const QList<PlannedUpload> &uploads,
                    const QString &albumId,
                    AlbumExecutionResult &result);
    void runAlbumRemovals(const QList<PlannedAlbumRemoval> &removals,
                          const QString &albumId,
                          AlbumExecutionResult &result);
    void runLocalTrashings(const QList<PlannedLocalTrashing> &trashings,
                           const QString &folderName,
                           AlbumExecutionResult &result);

    QString stagingDirectory() const;

    ImmichClient *m_client;
    SyncStore *m_store;
    QString m_rootPath;
    SyncSettings m_settings;
    std::shared_ptr<DateProvider> m_dateProvider;
    ProgressCallback m_reportProgress;
};

} // namespace immichksync
