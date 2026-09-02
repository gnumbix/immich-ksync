#pragma once

#include "core/Checksum.h"
#include "storage/Records.h"
#include "storage/SqliteDatabase.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

namespace immichksync {

/// Reconciliation state: which albums exist, what the last cycle agreed on, and what
/// is currently held back or backing off.
///
/// The database is a cache. Losing it costs a full rediscovery from the album markers
/// on disk, never data — which is what makes every migration safe to run.
class SyncStore {
public:
    struct Statistics {
        int albumCount = 0;
        int syncedAssetCount = 0;
        qint64 syncedByteCount = 0;
        int heldRemovalCount = 0;
        int failureCount = 0;

        bool operator==(const Statistics &other) const;
    };

    SyncStore() = default;
    Q_DISABLE_COPY_MOVE(SyncStore)

    /// `$XDG_DATA_HOME/immichksync/sync-state.sqlite`
    static QString defaultPath();

    bool open(const QString &path, QString *errorMessage);
    bool isOpen() const { return m_database.isOpen(); }

    // Albums
    QList<AlbumRecord> albums();
    std::optional<AlbumRecord> album(const QString &albumId);
    std::optional<AlbumRecord> albumByFolderName(const QString &folderName);
    bool upsert(const AlbumRecord &album, QString *errorMessage = nullptr);
    bool deleteAlbum(const QString &albumId, QString *errorMessage = nullptr);
    bool setExcluded(bool excluded, const QString &albumId, QString *errorMessage = nullptr);
    bool setSafetyHold(bool held, const QString &albumId, QString *errorMessage = nullptr);
    bool markSynced(const QString &albumId, const QDateTime &date, QString *errorMessage = nullptr);
    QSet<QString> excludedAlbumIds();

    // Baselines
    QHash<Sha1Checksum, AssetBaseline> baseline(const QString &albumId);
    bool upsert(const AssetBaseline &baseline, QString *errorMessage = nullptr);
    bool deleteBaseline(const QString &albumId,
                        const Sha1Checksum &checksum,
                        QString *errorMessage = nullptr);
    bool deleteBaselines(const QString &albumId, QString *errorMessage = nullptr);

    // Local file hash cache
    QHash<QString, LocalFileFingerprint> fingerprints(const QString &folderName);
    bool upsert(const LocalFileFingerprint &fingerprint,
                const QDateTime &at,
                QString *errorMessage = nullptr);
    bool deleteFingerprint(const QString &relativePath, QString *errorMessage = nullptr);
    /// Drops cache rows for files that no longer exist, keeping the table proportional
    /// to what is actually on disk.
    bool pruneFingerprints(const QString &folderName,
                           const QSet<QString> &livePaths,
                           QString *errorMessage = nullptr);

    // Safety-held removals
    QList<HeldRemoval> heldRemovals();
    bool replaceHeldRemovals(const QString &albumId,
                             const QList<HeldRemoval> &removals,
                             QString *errorMessage = nullptr);
    bool clearHeldRemovals(const QString &albumId, QString *errorMessage = nullptr);
    QSet<QString> albumsWithHeldRemovals();

    // Transfer back-off
    QSet<QString> backedOffKeys(const QDateTime &now);
    bool recordFailure(const QString &key,
                       const QString &error,
                       const QDateTime &now,
                       QString *errorMessage = nullptr);
    bool clearFailure(const QString &key, QString *errorMessage = nullptr);
    int failureCount();

    // Meta
    std::optional<QString> metaValue(const QString &key);
    bool setMetaValue(const QString &key,
                      const std::optional<QString> &value,
                      QString *errorMessage = nullptr);

    Statistics statistics();

    /// Forgets everything and forces a full rediscovery on the next cycle. The files on
    /// disk and the assets on the server are untouched.
    bool reset(QString *errorMessage = nullptr);

private:
    /// Escapes `%` and `_` so an album folder containing them cannot match siblings.
    static QString likePrefix(const QString &prefix);

    SqliteDatabase m_database;
};

/// Runs a store write that the cycle can survive losing, logging instead of failing.
///
/// A dropped baseline write is not fatal — the next cycle re-derives it — but it must
/// never be silent, because the symptom is the same work being repeated forever.
void persisting(const QString &description, const std::function<bool(QString *)> &body);

} // namespace immichksync
