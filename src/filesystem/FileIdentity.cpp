#include "filesystem/FileIdentity.h"

#include <QTimeZone>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace immichksync {

namespace {

QDateTime fromTimestamp(qint64 seconds, quint32 nanoseconds)
{
    return QDateTime::fromMSecsSinceEpoch(seconds * 1000 + nanoseconds / 1000000, QTimeZone::UTC);
}

} // namespace

std::optional<FileIdentity> FileIdentity::of(const QString &path)
{
    struct statx info{};
    const unsigned int mask = STATX_TYPE | STATX_SIZE | STATX_MTIME | STATX_BTIME | STATX_INO;
    if (statx(AT_FDCWD, path.toUtf8().constData(), AT_STATX_SYNC_AS_STAT, mask, &info) != 0) {
        return std::nullopt;
    }
    if (!S_ISREG(info.stx_mode)) {
        return std::nullopt;
    }

    FileIdentity identity;
    identity.size = static_cast<qint64>(info.stx_size);
    identity.modifiedAtNanoseconds =
        static_cast<qint64>(info.stx_mtime.tv_sec) * 1000000000 + info.stx_mtime.tv_nsec;
    identity.modifiedAt = fromTimestamp(info.stx_mtime.tv_sec, info.stx_mtime.tv_nsec);
    identity.inode = static_cast<qint64>(info.stx_ino);
    identity.deviceId = static_cast<qint64>(makedev(info.stx_dev_major, info.stx_dev_minor));

    // Birth time is optional in statx: ext4 and btrfs report it, NFS and several FUSE
    // filesystems do not. Falling back to mtime is behaviour-preserving, because the
    // only consumer is the upload, which already sends min(createdAt, modifiedAt).
    if (info.stx_mask & STATX_BTIME) {
        identity.createdAt = fromTimestamp(info.stx_btime.tv_sec, info.stx_btime.tv_nsec);
        identity.hasBirthTime = true;
    } else {
        identity.createdAt = identity.modifiedAt;
        identity.hasBirthTime = false;
    }
    return identity;
}

} // namespace immichksync
