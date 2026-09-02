#include "Fixtures.h"

#include "filesystem/AtomicFileWriter.h"
#include "filesystem/FileHasher.h"
#include "filesystem/FileIdentity.h"
#include "filesystem/FileScanner.h"
#include "filesystem/LocalTrash.h"
#include "filesystem/RootFolderAccess.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace immichksync;

namespace {

/// Writes `contents` and backdates the file well past any settle window, so the
/// scanner does not defer it as "still being copied".
QString writeFile(const QString &path, const QByteArray &contents = "photo bytes")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(contents);
    file.close();
    AtomicFileWriter::applyModificationTime(path,
                                            QDateTime::currentDateTimeUtc().addSecs(-3600));
    return path;
}

FileScanner makeScanner(const std::shared_ptr<DateProvider> &clock = systemDateProvider())
{
    return FileScanner(MediaTypeCatalog::fallback(), 5, clock);
}

} // namespace

/// Scanning, hashing, atomic installs and the local trash. The invariant every test
/// here defends is that nothing the app writes is ever visible half-finished, and
/// nothing it removes is ever actually deleted.
class FileSystemTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_directory = std::make_unique<QTemporaryDir>();
        QVERIFY(m_directory->isValid());
    }

    void cleanup() { m_directory.reset(); }

    // MARK: - Hashing

    void hashesAFileToTheDigestImmichUses()
    {
        const QString path = writeFile(root(QStringLiteral("a.jpg")), QByteArray());
        const auto checksum = FileHasher::checksumOf(path);
        QVERIFY(checksum.has_value());
        // SHA-1 of the empty string.
        QCOMPARE(checksum->hex(), QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    }

    void reportsAnUnreadableFileRatherThanGuessing()
    {
        QString error;
        QVERIFY(!FileHasher::checksumOf(root(QStringLiteral("missing.jpg")), &error).has_value());
        QVERIFY(!error.isEmpty());
    }

    // MARK: - File identity

    void readsIdentityFromStatx()
    {
        const QString path = writeFile(root(QStringLiteral("a.jpg")), QByteArray("12345"));
        const auto identity = FileIdentity::of(path);
        QVERIFY(identity.has_value());
        QCOMPARE(identity->size, 5);
        QVERIFY(identity->inode != 0);
        QVERIFY(identity->modifiedAtNanoseconds > 0);
        // Whether birth time is available depends on the filesystem, but createdAt must
        // always be populated — it falls back to mtime.
        QVERIFY(identity->createdAt.isValid());
    }

    void ignoresDirectoriesAndMissingPaths()
    {
        QVERIFY(!FileIdentity::of(m_directory->path()).has_value());
        QVERIFY(!FileIdentity::of(root(QStringLiteral("nope.jpg"))).has_value());
    }

    // MARK: - Scanning

    void findsAlbumFoldersAndTheirAssets()
    {
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        writeFile(root(QStringLiteral("Holiday/IMG_0002.jpg")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.folders.size(), 1);
        QCOMPARE(scan.folders[0].folderName, QStringLiteral("Holiday"));
        QCOMPARE(scan.folders[0].files.size(), 2);
    }

    void ignoresNonMediaFiles()
    {
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        writeFile(root(QStringLiteral("Holiday/notes.txt")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.folders[0].files.size(), 1);
        // …but it still records the name, so a download can never clobber it.
        QVERIFY(scan.folders[0].occupiedNames.contains(QStringLiteral("notes.txt")));
    }

    void countsLooseFilesAtTheRootWithoutSyncingThem()
    {
        writeFile(root(QStringLiteral("loose.jpg")));
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.looseFileCount, 1);
        QCOMPARE(scan.folders.size(), 1);
    }

    void skipsInternalFolders()
    {
        writeFile(root(QStringLiteral(".immich-trash/Holiday/old.jpg")));
        writeFile(root(QStringLiteral(".immich-staging/partial.jpg")));
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.folders.size(), 1);
        QCOMPARE(scan.folders[0].folderName, QStringLiteral("Holiday"));
    }

    /// Immich albums are flat. Flattening cannot be undone when syncing back the other
    /// way, so nested folders are reported and left alone.
    void reportsNestedFoldersInsteadOfDescendingIntoThem()
    {
        writeFile(root(QStringLiteral("Holiday/Raw/IMG_0001.dng")));
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.folders[0].nestedDirectoryNames, QStringList{QStringLiteral("Raw")});
        QCOMPARE(scan.folders[0].files.size(), 1);
    }

    /// A half-copied file hashes to the wrong value and would upload as a corrupt
    /// asset, so anything still moving is deferred to the next cycle.
    void defersFilesThatAreStillBeingWritten()
    {
        const QString path = root(QStringLiteral("Holiday/IMG_0001.HEIC"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("just written");
        file.close();

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.folders[0].files.size(), 0);
        QCOMPARE(scan.folders[0].settlingFileCount, 1);
    }

    void readsTheAlbumMarker()
    {
        const QString folder = root(QStringLiteral("Holiday"));
        QDir().mkpath(folder);
        AlbumMarker marker;
        marker.albumId = QStringLiteral("album-1");
        marker.albumName = QStringLiteral("Holiday");
        marker.folderName = QStringLiteral("Holiday");
        QVERIFY(AlbumFolderLayout::writeMarker(marker, folder, nullptr));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QVERIFY(scan.folders[0].marker.has_value());
        QCOMPARE(scan.folders[0].marker->albumId, QStringLiteral("album-1"));
    }

    void reportsAnUnreadableRootRatherThanAnEmptyOne()
    {
        const RootScan scan = makeScanner().scan(root(QStringLiteral("does-not-exist")));
        QVERIFY(scan.rootUnreadable);
        QVERIFY(scan.folders.isEmpty());
    }

    // MARK: - Sidecars

    void pairsASidecarNamedAfterTheWholeFile()
    {
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC.xmp")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QCOMPARE(scan.folders[0].files.size(), 1);
        QVERIFY(scan.folders[0].files[0].sidecarPath.endsWith(QStringLiteral("IMG_0001.HEIC.xmp")));
    }

    void pairsASidecarNamedAfterTheStem()
    {
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        writeFile(root(QStringLiteral("Holiday/IMG_0001.xmp")));

        const RootScan scan = makeScanner().scan(m_directory->path());
        QVERIFY(scan.folders[0].files[0].sidecarPath.endsWith(QStringLiteral("IMG_0001.xmp")));
    }

    void leavesAssetsWithNoSidecarAlone()
    {
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        const RootScan scan = makeScanner().scan(m_directory->path());
        QVERIFY(scan.folders[0].files[0].sidecarPath.isEmpty());
    }

    // MARK: - Atomic installs

    void installsAtomicallyAndStampsTheModificationTime()
    {
        const QString source = writeFile(root(QStringLiteral(".immich-staging/download-1")));
        const QString destination = root(QStringLiteral("Holiday/IMG_0001.HEIC"));
        const QDateTime stamp = QDateTime::fromMSecsSinceEpoch(1'600'000'000'000, QTimeZone::UTC);

        QString error;
        QVERIFY2(AtomicFileWriter::install(source, destination, stamp, false, &error),
                 qUtf8Printable(error));

        QVERIFY(QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(source));
        const auto identity = FileIdentity::of(destination);
        QVERIFY(identity.has_value());
        QCOMPARE(identity->modifiedAt.toSecsSinceEpoch(), stamp.toSecsSinceEpoch());
    }

    void refusesToOverwriteUnlessAskedTo()
    {
        const QString source = writeFile(root(QStringLiteral(".immich-staging/download-1")));
        const QString destination = writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")),
                                              QByteArray("existing"));

        QString error;
        QVERIFY(!AtomicFileWriter::install(source, destination, QDateTime(), false, &error));
        QVERIFY(!error.isEmpty());
        // The original must still be there, untouched.
        QFile file(destination);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("existing"));
    }

    void marksTheStagingFolderAsACache()
    {
        const QString staging = root(QStringLiteral(".immich-staging"));
        QVERIFY(AtomicFileWriter::ensureDirectory(staging, true, nullptr));
        QVERIFY(QFileInfo::exists(QDir(staging).filePath(QStringLiteral("CACHEDIR.TAG"))));
    }

    // MARK: - Local trash

    void movesFilesToTheTrashRatherThanDeletingThem()
    {
        const QString path = writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        LocalTrash trash(m_directory->path());

        QString error;
        const QString landed = trash.moveFile(path, QStringLiteral("Holiday"), &error);
        QVERIFY2(!landed.isEmpty(), qUtf8Printable(error));

        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(landed));
        QVERIFY(landed.contains(QStringLiteral(".immich-trash/Holiday")));
        QCOMPARE(trash.itemCount(), 1);
    }

    void neverOverwritesSomethingAlreadyInTheTrash()
    {
        LocalTrash trash(m_directory->path());
        const QString first = writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")),
                                        QByteArray("first"));
        QVERIFY(!trash.moveFile(first, QStringLiteral("Holiday"), nullptr).isEmpty());

        const QString second = writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")),
                                         QByteArray("second"));
        const QString landed = trash.moveFile(second, QStringLiteral("Holiday"), nullptr);

        QVERIFY(!landed.isEmpty());
        QVERIFY(landed.endsWith(QStringLiteral("IMG_0001 2.HEIC")));
        QCOMPARE(trash.itemCount(), 2);
    }

    void retiresAWholeAlbumFolder()
    {
        writeFile(root(QStringLiteral("Holiday/IMG_0001.HEIC")));
        LocalTrash trash(m_directory->path());

        const QString landed = trash.moveFolder(root(QStringLiteral("Holiday")), nullptr);
        QVERIFY(!landed.isEmpty());
        QVERIFY(!QFileInfo::exists(root(QStringLiteral("Holiday"))));
        QVERIFY(QFileInfo::exists(landed + QStringLiteral("/IMG_0001.HEIC")));
    }

    // MARK: - Root validation

    void validatesTheSyncFolder()
    {
        QCOMPARE(RootFolderAccess::validate(m_directory->path()),
                 RootFolderAccess::Validation::Usable);
        QCOMPARE(RootFolderAccess::validate(root(QStringLiteral("missing"))),
                 RootFolderAccess::Validation::Missing);
        QCOMPARE(RootFolderAccess::validate(QString()), RootFolderAccess::Validation::Missing);

        const QString file = writeFile(root(QStringLiteral("a.jpg")));
        QCOMPARE(RootFolderAccess::validate(file), RootFolderAccess::Validation::NotADirectory);
    }

    /// A missing volume must pause the sync with an explanation, never look like the
    /// user deleted everything.
    void everyUnusableStateHasAnExplanation()
    {
        for (const auto validation : {RootFolderAccess::Validation::Missing,
                                      RootFolderAccess::Validation::NotADirectory,
                                      RootFolderAccess::Validation::NotWritable}) {
            QVERIFY(!RootFolderAccess::isUsable(validation));
            QVERIFY(!RootFolderAccess::message(validation).isEmpty());
        }
        QVERIFY(RootFolderAccess::message(RootFolderAccess::Validation::Usable).isEmpty());
    }

    void warnsAboutFoldersInsideTheTrash()
    {
        const QString trashPath =
            QDir(QDir::homePath()).filePath(QStringLiteral(".local/share/Trash/files/photos"));
        QVERIFY(!RootFolderAccess::locationWarning(trashPath).isEmpty());
    }

    void saysNothingAboutAnOrdinaryFolder()
    {
        QVERIFY(RootFolderAccess::locationWarning(m_directory->path()).isEmpty());
    }

private:
    QString root(const QString &relative) const
    {
        return QDir(m_directory->path()).filePath(relative);
    }

    std::unique_ptr<QTemporaryDir> m_directory;
};

QTEST_APPLESS_MAIN(FileSystemTest)
#include "FileSystemTest.moc"
