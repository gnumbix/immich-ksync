#include "filesystem/LocalTrash.h"

#include "core/Logging.h"
#include "filesystem/AlbumFolderLayout.h"
#include "filesystem/AtomicFileWriter.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace immichksync {

LocalTrash::LocalTrash(QString rootPath)
    : m_rootPath(std::move(rootPath))
{
}

QString LocalTrash::directory() const
{
    return QDir(m_rootPath).filePath(QString::fromLatin1(AlbumFolderLayout::kTrashFolderName));
}

QString LocalTrash::moveFile(const QString &path,
                             const QString &albumFolderName,
                             QString *errorMessage)
{
    const QString destinationFolder = QDir(directory()).filePath(albumFolderName);
    if (!AtomicFileWriter::ensureDirectory(destinationFolder, /*markAsCache=*/false, errorMessage)) {
        return {};
    }

    const QString destination = availablePath(destinationFolder, QFileInfo(path).fileName());
    if (!QFile::rename(path, destination)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not move %1 to the local trash.")
                                .arg(QFileInfo(path).fileName());
        }
        return {};
    }
    log::fileSystem.info(QStringLiteral("Moved %1/%2 to the local trash")
                             .arg(albumFolderName, QFileInfo(path).fileName()));
    return destination;
}

QString LocalTrash::moveFolder(const QString &path, QString *errorMessage)
{
    if (!AtomicFileWriter::ensureDirectory(directory(), /*markAsCache=*/false, errorMessage)) {
        return {};
    }

    const QString destination = availablePath(directory(), QFileInfo(path).fileName());
    if (!QFile::rename(path, destination)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not move the folder %1 to the local trash.")
                                .arg(QFileInfo(path).fileName());
        }
        return {};
    }
    log::fileSystem.notice(
        QStringLiteral("Moved album folder %1 to the local trash").arg(QFileInfo(path).fileName()));
    return destination;
}

QString LocalTrash::availablePath(const QString &folder, const QString &preferredName)
{
    const QDir directory(folder);
    if (!QFileInfo::exists(directory.filePath(preferredName))) {
        return directory.filePath(preferredName);
    }

    const QFileInfo info(preferredName);
    const QString extension = info.suffix();
    const QString stem = info.completeBaseName();

    for (int index = 2; index <= 999; ++index) {
        const QString name = extension.isEmpty()
            ? QStringLiteral("%1 %2").arg(stem).arg(index)
            : QStringLiteral("%1 %2.%3").arg(stem).arg(index).arg(extension);
        if (!QFileInfo::exists(directory.filePath(name))) {
            return directory.filePath(name);
        }
    }

    // A thousand collisions on one name means something pathological is happening;
    // a UUID always terminates.
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString name = extension.isEmpty() ? QStringLiteral("%1-%2").arg(stem, unique)
                                             : QStringLiteral("%1-%2.%3").arg(stem, unique, extension);
    return directory.filePath(name);
}

int LocalTrash::itemCount() const
{
    int count = 0;
    QDirIterator iterator(directory(),
                          QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        ++count;
    }
    return count;
}

} // namespace immichksync
