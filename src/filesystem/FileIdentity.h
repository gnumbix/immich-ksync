#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

namespace immichksync {

/// A single `statx(2)` worth of identity — everything the hash cache keys on.
///
/// `QFileInfo` cannot report the device number or a nanosecond mtime, and would need
/// several calls to gather what one `statx` returns.
struct FileIdentity {
    qint64 size = 0;
    QDateTime modifiedAt;
    qint64 modifiedAtNanoseconds = 0;
    QDateTime createdAt;
    qint64 deviceId = 0;
    qint64 inode = 0;
    /// False when the filesystem could not report a birth time and `createdAt` was
    /// filled in from the mtime instead.
    bool hasBirthTime = false;

    /// Returns nothing for anything that is not an existing regular file.
    static std::optional<FileIdentity> of(const QString &path);
};

} // namespace immichksync
