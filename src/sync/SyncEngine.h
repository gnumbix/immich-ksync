#pragma once

#include "core/Clock.h"
#include "core/Preferences.h"
#include "credentials/ImmichCredentials.h"
#include "filesystem/FileScanner.h"
#include "filesystem/FolderWatcher.h"
#include "immich/ServerDiscovery.h"
#include "notifications/UserNotifier.h"
#include "storage/SyncStore.h"
#include "sync/SyncExecutor.h"
#include "sync/SyncPlanner.h"
#include "sync/SyncStatus.h"

#include <QAtomicInt>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>
#include <optional>

namespace immichksync {

/// Why a cycle is starting. Kept for the log, and so a manual run can bypass throttles.
enum class SyncTrigger {
    Scheduled,
    FolderChanged,
    Manual,
    ConfigurationChanged,
    WokeFromSleep,
};

QString describe(SyncTrigger trigger);

/// Owns the sync loop: what runs, when, and in what order.
///
/// One cycle at a time, always. Triggers arrive as queued signals and coalesce onto a
/// single pending flag that the engine's own thread drains serially, so a file-system
/// burst, the interval timer and a manual run can never interleave two reconciliations
/// over the same folder.
///
/// Lives on a dedicated thread with its own event loop. Every public slot is safe to
/// invoke from the GUI thread; every signal is delivered back to it.
class SyncEngine : public QObject {
    Q_OBJECT

public:
    SyncEngine(SyncStore *store,
               Transport *transport,
               UserNotifier *notifier,
               std::shared_ptr<DateProvider> dateProvider = systemDateProvider(),
               QObject *parent = nullptr);
    ~SyncEngine() override;

    bool isRunning() const { return m_running; }

    /// Asks the cycle to give up as soon as it reaches a point where it can.
    ///
    /// Unlike `stop()`, this is safe to call from another thread while a cycle is
    /// running — which is the only useful time to call it, because a queued `stop()`
    /// is not looked at until the cycle that is blocking the thread has finished.
    void requestStop();
    bool isStopRequested() const;

public Q_SLOTS:
    void start();
    void stop();
    void applyConfiguration(const immichksync::SyncSettings &settings,
                            const std::optional<immichksync::ImmichCredentials> &credentials);
    void trigger(immichksync::SyncTrigger reason);

    /// Clears a safety hold and lets the withheld removals run on the next cycle.
    void applyHeldRemovals(const QString &albumId);
    /// Treats the missing local files as accidental deletions and fetches them again.
    void restoreHeldRemovals(const QString &albumId);

    /// Runs a reconciliation cycle, then any cycle a trigger asked for while it was
    /// running. The entry point for the tests, and for any caller that needs to await a
    /// cycle rather than schedule one.
    void runOnce(immichksync::SyncTrigger reason = immichksync::SyncTrigger::Manual);

Q_SIGNALS:
    void stateChanged(const immichksync::SyncState &state);
    void cycleFinished(const immichksync::SyncCycleSummary &summary);
    void statisticsChanged(const immichksync::SyncStore::Statistics &statistics);
    void safetyHoldsChanged(const QStringList &albumNames);
    void errorMessageChanged(const QString &message);
    void serverProfileChanged(const std::optional<immichksync::ServerProfile> &profile);
    void albumsChanged();

private Q_SLOTS:
    void handleFolderChange(const immichksync::FolderChange &change);
    void flushFolderChanges();
    void handleScheduledTick();

private:
    /// Exactly one cycle. Only `runOnce` calls this, and only when no other cycle is
    /// in flight.
    void runCycle(SyncTrigger reason);

    struct AlbumPairing {
        std::optional<AlbumResponse> remote;
        std::optional<ScannedAlbumFolder> folder;
        std::optional<AlbumRecord> stored;
    };

    void reconcile(const QString &rootPath, const QDateTime &startedAt, SyncCycleSummary &summary);
    QList<AlbumPairing> pair(const QList<AlbumResponse> &remoteAlbums, const RootScan &scan);
    AlbumExecutionResult reconcileAlbum(const AlbumPairing &pairing,
                                        const QSet<QString> &reservedFolderNames,
                                        const QString &rootPath,
                                        const ServerProfile &profile,
                                        ImmichClient &client);
    void persistAlbum(const AlbumExecutionResult &result,
                      const AlbumPairing &pairing,
                      bool deepScanned,
                      ImmichClient &client);

    /// Adding assets bumps `updatedAt`; removing them changes `assetCount`. When both
    /// match what was stored, membership cannot have changed, and a search would only
    /// re-derive the baseline. The interval backstop covers anything subtler.
    bool needsDeepScan(const AlbumResponse &remote,
                       const std::optional<AlbumRecord> &stored,
                       const QDateTime &now) const;

    struct HashOutcome {
        QList<LocalAsset> assets;
        int failures = 0;
    };
    HashOutcome hashFiles(const QList<ScannedFile> &files, const QString &folderName);

    std::optional<ServerProfile> ensureProfile();
    QString missingConfigurationReason() const;
    void setState(const SyncState &state);
    void updateStatistics();
    void refreshSafetyHolds();

    SyncStore *m_store;
    Transport *m_transport;
    UserNotifier *m_notifier;
    std::shared_ptr<DateProvider> m_dateProvider;
    SyncPlanner m_planner;

    SyncSettings m_settings;
    std::optional<ImmichCredentials> m_credentials;
    std::optional<ServerProfile> m_profile;

    bool m_running = false;
    /// Guards against a nested cycle: a trigger arriving mid-cycle sets `m_pending`
    /// rather than starting a second reconciliation over the same folder.
    bool m_inCycle = false;
    bool m_pending = false;
    SyncTrigger m_pendingReason = SyncTrigger::Scheduled;

    QTimer *m_intervalTimer;
    /// The watcher already coalesces; this waits for the user to actually finish
    /// copying before a burst of events turns into a cycle.
    QTimer *m_debounceTimer;
    FolderWatcher *m_watcher;
    QSet<QString> m_pendingFolderChanges;

    /// Albums whose safety hold the user explicitly cleared, honoured for one cycle.
    QSet<QString> m_safetyOverrides;

    /// Set from another thread on shutdown; checked wherever the cycle can bail out.
    QAtomicInt m_stopRequested = 0;
};

} // namespace immichksync

Q_DECLARE_METATYPE(immichksync::SyncTrigger)
