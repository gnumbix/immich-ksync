#pragma once

#include "core/Clock.h"
#include "sync/SyncPlan.h"

#include <QHash>
#include <QSet>

#include <memory>
#include <optional>

namespace immichksync {

/// The remote side of one album, as this cycle knows it.
struct RemoteSnapshot {
    enum class Kind {
        /// The album's assets were enumerated through `/search/metadata`.
        Enumerated,
        /// `updatedAt` and `assetCount` both match what was stored, so membership
        /// cannot have changed: adding assets bumps `updatedAt`, removing them changes
        /// the count. The baseline therefore *is* the remote set, and a page of search
        /// results would only confirm what is already known.
        UnchangedSinceLastScan,
    };

    Kind kind = Kind::UnchangedSinceLastScan;
    QList<RemoteAsset> assets;

    static RemoteSnapshot enumerated(QList<RemoteAsset> assets)
    {
        return {Kind::Enumerated, std::move(assets)};
    }
    static RemoteSnapshot unchanged() { return {Kind::UnchangedSinceLastScan, {}}; }
};

struct RemoteAlbumSummary {
    QString id;
    QString name;
    QString updatedAt;
    int assetCount = 0;
};

/// Everything the planner needs about one album. Assembled by `SyncEngine`; contains
/// no live handles, so planning is a pure function of these values.
struct AlbumPlanInput {
    /// Unset when no album with this identity exists on the server.
    std::optional<RemoteAlbumSummary> remoteAlbum;
    /// Unset when no folder exists on disk yet.
    std::optional<QString> folderName;
    std::optional<AlbumMarker> marker;
    std::optional<AlbumRecord> storedRecord;
    RemoteSnapshot remoteAssets = RemoteSnapshot::unchanged();
    QList<LocalAsset> localAssets;
    QHash<Sha1Checksum, AssetBaseline> baseline;
    /// Folder names already in use by other albums, so a rename target is unique.
    QSet<QString> reservedFolderNames;
    /// Every name inside the album folder, so a download cannot overwrite anything.
    QSet<QString> occupiedFilenames;
    QStringList nestedFolderNames;
    /// Set when some local files could not be examined this cycle — a still-copying
    /// file or one that failed to hash looks exactly like a deleted file, so removals
    /// must wait for a cycle that saw the folder completely.
    bool suppressRemovals = false;
};

/// Turns three snapshots — remote, local, and the last reconciled baseline — into the
/// work for one album.
///
/// Pure by construction: no I/O, no clock beyond the injected one, no globals. The
/// baseline is what makes this a synchroniser rather than a merger; without it,
/// "deleted here" and "added there" are the same observation, and every deletion would
/// resurrect itself on the next cycle.
class SyncPlanner {
public:
    explicit SyncPlanner(std::shared_ptr<DateProvider> dateProvider = systemDateProvider());

    AlbumPlan plan(const AlbumPlanInput &input) const;

private:
    AlbumPlan planFolderWithoutAlbum(const QString &folderName,
                                     const AlbumPlanInput &input,
                                     AlbumPlan plan) const;
    void applyRenames(const RemoteAlbumSummary &remote,
                      const QString &folderName,
                      const AlbumPlanInput &input,
                      AlbumPlan &plan) const;
    void reconcileAssets(const AlbumPlanInput &input, const QDateTime &now, AlbumPlan &plan) const;
    static QHash<Sha1Checksum, RemoteAsset> remoteAssetsByChecksum(const AlbumPlanInput &input);
    static QList<PlannedDownload> assignFilenames(const QList<RemoteAsset> &assets,
                                                  const QString &folderName,
                                                  const AlbumPlanInput &input,
                                                  const QHash<Sha1Checksum, LocalAsset> &local);

    std::shared_ptr<DateProvider> m_dateProvider;
};

} // namespace immichksync
