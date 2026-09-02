#pragma once

#include <QObject>
#include <QSet>
#include <QString>

#include <memory>

class KDirWatch;

namespace immichksync {

/// What changed under the sync root, at album granularity.
struct FolderChange {
    enum class Kind {
        /// A specific album folder was touched.
        AlbumFolder,
        /// Something at the root itself changed, or the exact set is unknown — the next
        /// cycle must look at everything.
        RootStructure,
    };

    Kind kind = Kind::RootStructure;
    QString folderName;

    static FolderChange rootStructure() { return {Kind::RootStructure, {}}; }
    static FolderChange albumFolder(QString name)
    {
        return {Kind::AlbumFolder, std::move(name)};
    }

    bool operator==(const FolderChange &other) const
    {
        return kind == other.kind && folderName == other.folderName;
    }
};

size_t qHash(const FolderChange &change, size_t seed = 0);

/// Watches the sync root and its album folders, publishing coalesced change signals.
///
/// KDirWatch is inotify underneath, which needs one watch descriptor per directory —
/// unlike FSEvents on macOS, which watches a whole tree with one handle. The sync
/// layout is shallow by design (root plus one level of album folders), so the watch
/// count stays proportional to the number of albums rather than to the number of
/// photos. `refresh()` re-registers as albums appear and disappear.
///
/// inotify does not fire on NFS or SMB mounts. That is not fatal — the interval timer
/// is the backstop — but it is worth saying out loud, so it is logged once.
class FolderWatcher : public QObject {
    Q_OBJECT

public:
    explicit FolderWatcher(QObject *parent = nullptr);
    ~FolderWatcher() override;

    /// Starts watching `rootPath`. Replaces any previous root.
    void start(const QString &rootPath);
    void stop();

    /// Re-registers the per-album watches against what is on disk now. Called at the
    /// end of each cycle, when the set of album folders is freshly known.
    void refresh();

    QString rootPath() const { return m_rootPath; }

Q_SIGNALS:
    /// Emitted for each observed change. The engine debounces before acting.
    void changed(const immichksync::FolderChange &change);

private Q_SLOTS:
    void handleDirty(const QString &path);
    void handleCreated(const QString &path);
    void handleDeleted(const QString &path);

private:
    void noteChange(const QString &path);
    /// `/root/Album/IMG.jpg` → album "Album"; `/root/loose.jpg` → root structure.
    std::optional<FolderChange> classify(const QString &path) const;
    static bool isOnANetworkMount(const QString &path);

    std::unique_ptr<KDirWatch> m_watch;
    QString m_rootPath;
    QSet<QString> m_watchedAlbumFolders;
};

} // namespace immichksync

Q_DECLARE_METATYPE(immichksync::FolderChange)
