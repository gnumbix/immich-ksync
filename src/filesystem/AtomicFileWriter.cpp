#include "filesystem/AtomicFileWriter.h"

#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

namespace immichksync {

namespace AtomicFileWriter {

bool install(const QString &sourcePath,
             const QString &destinationPath,
             const QDateTime &modifiedAt,
             bool replaceExisting,
             QString *errorMessage)
{
    const QFileInfo destination(destinationPath);
    if (!ensureDirectory(destination.absolutePath(), /*markAsCache=*/false, errorMessage)) {
        return false;
    }

    if (destination.exists()) {
        if (!replaceExisting) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("%1 already exists and was not replaced.")
                                    .arg(destination.fileName());
            }
            return false;
        }
        if (!QFile::remove(destinationPath)) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Could not replace %1.").arg(destination.fileName());
            }
            return false;
        }
    }

    // rename(2) rather than QFile::rename: the latter falls back to a copy across
    // filesystems, which would make a partially written file briefly visible. Staging
    // is inside the sync root precisely so that fallback is never needed.
    if (::rename(sourcePath.toUtf8().constData(), destinationPath.toUtf8().constData()) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not move the download into place at %1: %2")
                                .arg(destinationPath, QString::fromLocal8Bit(std::strerror(errno)));
        }
        return false;
    }

    applyModificationTime(destinationPath, modifiedAt);
    return true;
}

void applyModificationTime(const QString &path, const QDateTime &modifiedAt)
{
    if (!modifiedAt.isValid()) {
        return;
    }
    const qint64 msecs = modifiedAt.toMSecsSinceEpoch();
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT; // Leave atime alone.
    times[1].tv_sec = static_cast<time_t>(msecs / 1000);
    times[1].tv_nsec = static_cast<long>((msecs % 1000) * 1000000);

    if (utimensat(AT_FDCWD, path.toUtf8().constData(), times, 0) != 0) {
        log::fileSystem.debug(QStringLiteral("Could not stamp the modification time on %1: %2")
                                  .arg(QFileInfo(path).fileName(),
                                       QString::fromLocal8Bit(std::strerror(errno))));
    }
}

bool ensureDirectory(const QString &path, bool markAsCache, QString *errorMessage)
{
    if (!QDir().mkpath(path)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not create %1").arg(path);
        }
        return false;
    }

    if (markAsCache) {
        const QString tagPath = QDir(path).filePath(QStringLiteral("CACHEDIR.TAG"));
        if (!QFile::exists(tagPath)) {
            QFile tag(tagPath);
            if (tag.open(QIODevice::WriteOnly)) {
                // The signature line is fixed by the CACHEDIR.TAG specification; the
                // rest is for whoever finds the folder and wonders what it is.
                tag.write("Signature: 8a477f597d28d172789f06886806bc55\n"
                          "# This folder holds ImmichKSync transfers that are still in "
                          "progress.\n"
                          "# It is safe to exclude from backups and safe to delete when the "
                          "app is not running.\n");
            }
        }
    }
    return true;
}

} // namespace AtomicFileWriter

} // namespace immichksync
