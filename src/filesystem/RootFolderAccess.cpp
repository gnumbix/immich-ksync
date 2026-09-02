#include "filesystem/RootFolderAccess.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <sys/statfs.h>
#include <sys/vfs.h>

namespace immichksync {

namespace RootFolderAccess {

namespace {

constexpr unsigned long kNfsSuperMagic = 0x6969;
constexpr unsigned long kSmbSuperMagic = 0x517B;
constexpr unsigned long kCifsMagic = 0xFF534D42;
constexpr unsigned long kFuseSuperMagic = 0x65735546;
constexpr unsigned long kOverlayfsMagic = 0x794C7630;

bool isUnder(const QString &path, const QString &candidateParent)
{
    if (candidateParent.isEmpty()) {
        return false;
    }
    const QString parent = QDir::cleanPath(candidateParent) + QLatin1Char('/');
    return QDir::cleanPath(path).startsWith(parent);
}

} // namespace

Validation validate(const QString &path)
{
    if (path.isEmpty()) {
        return Validation::Missing;
    }
    const QFileInfo info(path);
    if (!info.exists()) {
        return Validation::Missing;
    }
    if (!info.isDir()) {
        return Validation::NotADirectory;
    }
    if (!info.isWritable()) {
        return Validation::NotWritable;
    }
    return Validation::Usable;
}

bool isUsable(Validation validation)
{
    return validation == Validation::Usable;
}

QString message(Validation validation)
{
    switch (validation) {
    case Validation::Usable:
        return {};
    case Validation::Missing:
        return QStringLiteral("The sync folder is not available. Is the drive connected?");
    case Validation::NotADirectory:
        return QStringLiteral("The sync folder path is not a folder.");
    case Validation::NotWritable:
        return QStringLiteral("The sync folder is read-only.");
    }
    return {};
}

QString locationWarning(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }

    const QString clean = QDir::cleanPath(path);

    // A folder inside the user's trash will be emptied out from under the app.
    const QString home = QDir::homePath();
    if (isUnder(clean, QDir(home).filePath(QStringLiteral(".local/share/Trash")))) {
        return QStringLiteral("This folder is inside your Trash, which the system may empty at "
                              "any time. Choose a folder somewhere else.");
    }

    // The app's own config and state live here; syncing photos on top of them is a
    // mistake worth catching before the first cycle rather than after it.
    for (const auto location : {QStandardPaths::GenericConfigLocation,
                                QStandardPaths::GenericCacheLocation,
                                QStandardPaths::GenericStateLocation}) {
        if (isUnder(clean, QStandardPaths::writableLocation(location))) {
            return QStringLiteral("This folder is inside a system configuration or cache "
                                  "directory. Choose somewhere in your home folder instead.");
        }
    }

    struct statfs info{};
    if (statfs(clean.toUtf8().constData(), &info) == 0) {
        const auto type = static_cast<unsigned long>(info.f_type);
        if (type == kNfsSuperMagic || type == kSmbSuperMagic || type == kCifsMagic) {
            return QStringLiteral("This folder is on a network share. Changes there are not "
                                  "reported to the app, so it will rely on the timer, and file "
                                  "identity may not be stable across remounts.");
        }
        if (type == kFuseSuperMagic) {
            // Covers gvfs, sshfs, rclone, and every cloud-drive client on Linux — the
            // direct analogue of the macOS build's iCloud warning. A file evicted to
            // the cloud looks deleted to any sync tool.
            return QStringLiteral("This folder is on a FUSE mount such as a cloud drive. Files "
                                  "that get evicted to the cloud look deleted to any sync tool — "
                                  "choose a local folder instead.");
        }
        if (type == kOverlayfsMagic) {
            return QStringLiteral("This folder is on an overlay filesystem, where file identity "
                                  "is not stable. Choose a folder on ordinary storage instead.");
        }
    }

    return {};
}

} // namespace RootFolderAccess

} // namespace immichksync
