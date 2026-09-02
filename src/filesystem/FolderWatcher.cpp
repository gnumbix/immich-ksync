#include "filesystem/FolderWatcher.h"

#include "core/Logging.h"
#include "filesystem/AlbumFolderLayout.h"

#include <KDirWatch>

#include <QDir>
#include <QFileInfo>

#include <sys/statfs.h>
#include <sys/vfs.h>

namespace immichksync {

namespace {

// Filesystem magic numbers for the mounts inotify cannot watch. From
// <linux/magic.h>, which does not define all of them.
constexpr unsigned long kNfsSuperMagic = 0x6969;
constexpr unsigned long kSmbSuperMagic = 0x517B;
constexpr unsigned long kCifsMagic = 0xFF534D42;
constexpr unsigned long kFuseSuperMagic = 0x65735546;

} // namespace

size_t qHash(const FolderChange &change, size_t seed)
{
    return qHash(change.folderName, seed) ^ static_cast<size_t>(change.kind);
}

FolderWatcher::FolderWatcher(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<FolderChange>("immichksync::FolderChange");
}

FolderWatcher::~FolderWatcher() = default;

bool FolderWatcher::isOnANetworkMount(const QString &path)
{
    struct statfs info{};
    if (statfs(path.toUtf8().constData(), &info) != 0) {
        return false;
    }
    const auto type = static_cast<unsigned long>(info.f_type);
    return type == kNfsSuperMagic || type == kSmbSuperMagic || type == kCifsMagic
        || type == kFuseSuperMagic;
}

void FolderWatcher::start(const QString &rootPath)
{
    stop();
    if (rootPath.isEmpty()) {
        return;
    }
    // Resolve symlinks so the paths KDirWatch reports share a prefix with the root.
    m_rootPath = QFileInfo(rootPath).canonicalFilePath();
    if (m_rootPath.isEmpty()) {
        m_rootPath = rootPath;
    }

    m_watch = std::make_unique<KDirWatch>(this);
    connect(m_watch.get(), &KDirWatch::dirty, this, &FolderWatcher::handleDirty);
    connect(m_watch.get(), &KDirWatch::created, this, &FolderWatcher::handleCreated);
    connect(m_watch.get(), &KDirWatch::deleted, this, &FolderWatcher::handleDeleted);

    // The root is watched for its own entries only: album folders get their own watch
    // in refresh(), and WatchSubDirs over a photo library would be a watch per folder
    // for no benefit.
    m_watch->addDir(m_rootPath, KDirWatch::WatchDirOnly);
    refresh();

    if (isOnANetworkMount(m_rootPath)) {
        log::fileSystem.notice(
            QStringLiteral("%1 is on a network or FUSE mount, where change notifications are not "
                           "delivered. Changes will be picked up by the interval timer instead.")
                .arg(m_rootPath));
    }
    log::fileSystem.info(QStringLiteral("Watching %1 for changes").arg(m_rootPath));
}

void FolderWatcher::stop()
{
    m_watch.reset();
    m_watchedAlbumFolders.clear();
    m_rootPath.clear();
}

void FolderWatcher::refresh()
{
    if (!m_watch || m_rootPath.isEmpty()) {
        return;
    }

    QDir root(m_rootPath);
    const QStringList names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    QSet<QString> current;
    for (const QString &name : names) {
        if (AlbumFolderLayout::isInternalName(name)) {
            continue;
        }
        current.insert(name);
        if (!m_watchedAlbumFolders.contains(name)) {
            // WatchFiles so a photo dropped into an album folder is noticed, not just
            // the folder's own creation and deletion.
            m_watch->addDir(root.filePath(name), KDirWatch::WatchFiles);
        }
    }

    for (const QString &name : std::as_const(m_watchedAlbumFolders)) {
        if (!current.contains(name)) {
            m_watch->removeDir(root.filePath(name));
        }
    }
    m_watchedAlbumFolders = current;
}

void FolderWatcher::handleDirty(const QString &path)
{
    noteChange(path);
}

void FolderWatcher::handleCreated(const QString &path)
{
    noteChange(path);
}

void FolderWatcher::handleDeleted(const QString &path)
{
    noteChange(path);
}

void FolderWatcher::noteChange(const QString &path)
{
    if (const auto change = classify(path)) {
        Q_EMIT changed(*change);
    }
}

std::optional<FolderChange> FolderWatcher::classify(const QString &path) const
{
    if (m_rootPath.isEmpty() || !path.startsWith(m_rootPath)) {
        return std::nullopt;
    }

    QString relative = path.mid(m_rootPath.size());
    while (relative.startsWith(QLatin1Char('/'))) {
        relative.remove(0, 1);
    }
    if (relative.isEmpty()) {
        return FolderChange::rootStructure();
    }

    const QStringList components = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.isEmpty()) {
        return FolderChange::rootStructure();
    }

    const QString first = components.first();
    if (AlbumFolderLayout::isInternalName(first)) {
        // Our own trash and staging folders churn constantly during a cycle; reacting
        // to them would make the app trigger itself forever.
        return std::nullopt;
    }
    // A single component is a file or folder directly at the root: either way the set
    // of album folders may have changed.
    return components.size() == 1 ? FolderChange::rootStructure()
                                  : FolderChange::albumFolder(first);
}

} // namespace immichksync
