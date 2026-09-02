#pragma once

#include "immich/ServerDiscovery.h"
#include "storage/SyncStore.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace immichksync {

/// What the engine is doing right now, in a form the tray can render directly.
struct SyncState {
    enum class Kind {
        /// Missing server, credentials, or sync folder.
        NotConfigured,
        Paused,
        Idle,
        Preparing,
        Working,
        Failed,
    };

    struct Progress {
        enum class Phase {
            Scanning,
            Hashing,
            Downloading,
            Uploading,
            UpdatingAlbums,
        };

        Phase phase = Phase::Scanning;
        int completed = 0;
        int total = 0;
        QString currentItem;

        QString verb() const;
        QString description() const;
        bool operator==(const Progress &other) const;
    };

    Kind kind = Kind::Idle;
    /// Why it is not configured, or what failed.
    QString message;
    Progress progress;

    static SyncState notConfigured(const QString &reason);
    static SyncState paused();
    static SyncState idle();
    static SyncState preparing();
    static SyncState working(const Progress &progress);
    static SyncState failed(const QString &message);

    bool isWorking() const { return kind == Kind::Working; }
    /// Breeze icon name for the tray.
    QString iconName() const;
    bool operator==(const SyncState &other) const;
};

/// The result of one completed cycle, kept for the settings window.
struct SyncCycleSummary {
    QDateTime finishedAt;
    int uploaded = 0;
    int downloaded = 0;
    int removedFromAlbums = 0;
    int movedToTrash = 0;
    int albumsCreated = 0;
    int failures = 0;
    double durationSeconds = 0;

    bool didChangeAnything() const;
    QString headline() const;
};

/// Observable snapshot the tray and settings window read. Lives on the GUI thread; the
/// engine pushes into it with queued signals.
class SyncStatusModel : public QObject {
    Q_OBJECT

public:
    explicit SyncStatusModel(QObject *parent = nullptr);

    SyncState state() const { return m_state; }
    std::optional<SyncCycleSummary> lastSummary() const { return m_lastSummary; }
    QString lastErrorMessage() const { return m_lastErrorMessage; }
    SyncStore::Statistics statistics() const { return m_statistics; }
    QStringList albumsOnSafetyHold() const { return m_albumsOnSafetyHold; }
    std::optional<ServerProfile> serverProfile() const { return m_serverProfile; }

    /// Single line for the tray's status row.
    QString menuStatusLine() const;
    /// Anything needing the user's attention outranks the plain state icon, so a safety
    /// hold is visible without opening the menu.
    bool needsAttention() const;
    QString trayIconName() const;

public Q_SLOTS:
    void setState(const immichksync::SyncState &state);
    void setLastSummary(const immichksync::SyncCycleSummary &summary);
    void setLastErrorMessage(const QString &message);
    void setStatistics(const immichksync::SyncStore::Statistics &statistics);
    void setAlbumsOnSafetyHold(const QStringList &names);
    void setServerProfile(const std::optional<immichksync::ServerProfile> &profile);

Q_SIGNALS:
    void changed();

private:
    SyncState m_state = SyncState::idle();
    std::optional<SyncCycleSummary> m_lastSummary;
    QString m_lastErrorMessage;
    SyncStore::Statistics m_statistics;
    QStringList m_albumsOnSafetyHold;
    std::optional<ServerProfile> m_serverProfile;
};

} // namespace immichksync

Q_DECLARE_METATYPE(immichksync::SyncState)
Q_DECLARE_METATYPE(immichksync::SyncCycleSummary)
Q_DECLARE_METATYPE(immichksync::SyncStore::Statistics)
