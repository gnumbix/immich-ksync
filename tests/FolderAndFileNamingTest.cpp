#include "Fixtures.h"

#include "filesystem/AlbumFolderLayout.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace immichksync;
using namespace immichksync::AlbumFolderLayout;

/// Naming is the cross-platform compatibility contract: the same album must produce
/// the same folder name here as it does on macOS, or a sync folder moved between the
/// two would look like every album had been deleted and replaced.
class FolderAndFileNamingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // MARK: - Sanitising album names

    void keepsOrdinaryNamesUnchanged()
    {
        QCOMPARE(sanitize(QStringLiteral("Holiday 2024")), QStringLiteral("Holiday 2024"));
    }

    void replacesPathSeparators()
    {
        QCOMPARE(sanitize(QStringLiteral("Trip/Rome")), QStringLiteral("Trip-Rome"));
    }

    /// Legal on Linux, stripped anyway: macOS must remove it because Finder still
    /// renders it as `/`, and the two builds have to agree.
    void replacesColonsForMacCompatibility()
    {
        QCOMPARE(sanitize(QStringLiteral("Trip: Rome")), QStringLiteral("Trip- Rome"));
    }

    void stripsControlCharacters()
    {
        QCOMPARE(sanitize(QStringLiteral("Tri\tp\x01")), QStringLiteral("Trip"));
    }

    void stripsLeadingDotsSoTheFolderIsNotHidden()
    {
        QCOMPARE(sanitize(QStringLiteral(".hidden")), QStringLiteral("hidden"));
        QCOMPARE(sanitize(QStringLiteral("...deep")), QStringLiteral("deep"));
    }

    void stripsTrailingDotsAndSpaces()
    {
        QCOMPARE(sanitize(QStringLiteral("Trip. ")), QStringLiteral("Trip"));
        QCOMPARE(sanitize(QStringLiteral("Trip...")), QStringLiteral("Trip"));
    }

    void fallsBackWhenNothingSurvives()
    {
        QCOMPARE(sanitize(QStringLiteral("...")), QStringLiteral("Untitled Album"));
        QCOMPARE(sanitize(QString()), QStringLiteral("Untitled Album"));
    }

    void truncatesToTheComponentLimit()
    {
        const QString name = sanitize(QString(400, QLatin1Char('a')));
        QVERIFY(name.toUtf8().size() <= 255);
    }

    /// A multi-byte name must never be cut mid-character, which would produce a name
    /// the filesystem rejects.
    void truncatesOnCharacterBoundaries()
    {
        const QString name = sanitize(QString(200, QChar(0x00E9))); // é, two bytes each
        QVERIFY(name.toUtf8().size() <= 255);
        QCOMPARE(QString::fromUtf8(name.toUtf8()), name);
    }

    // MARK: - Folder names

    void usesTheAlbumNameWhenFree()
    {
        QCOMPARE(folderName(QStringLiteral("Trip"), QStringLiteral("abc"), {}),
                 QStringLiteral("Trip"));
    }

    void addsAStableIdSuffixOnCollision()
    {
        const QString name = folderName(QStringLiteral("Trip"),
                                        QStringLiteral("11111111-2222-4000-8000-000000000000"),
                                        {QStringLiteral("Trip")});
        QVERIFY(name != QStringLiteral("Trip"));
        QVERIFY(name.startsWith(QStringLiteral("Trip (")));
        QVERIFY(name.contains(QStringLiteral("11111111")));
    }

    /// A counter would reshuffle every folder whenever an album was deleted; the ID
    /// slice does not.
    void collisionSuffixIsStable()
    {
        const QString first = folderName(QStringLiteral("Trip"),
                                         QStringLiteral("abc-def"),
                                         {QStringLiteral("Trip")});
        const QString second = folderName(QStringLiteral("Trip"),
                                          QStringLiteral("abc-def"),
                                          {QStringLiteral("Trip")});
        QCOMPARE(first, second);
    }

    void comparesTakenNamesCaseInsensitively()
    {
        const QString name = folderName(QStringLiteral("Trip"),
                                        QStringLiteral("abcdef12"),
                                        {QStringLiteral("trip")});
        QVERIFY(name.compare(QStringLiteral("Trip"), Qt::CaseInsensitive) != 0);
    }

    void fallsBackToTheFullIdWhenBothAreTaken()
    {
        const QString albumId = QStringLiteral("11111111-2222-4000-8000-000000000000");
        const QString suffixed = folderName(QStringLiteral("Trip"), albumId, {QStringLiteral("Trip")});
        const QString name =
            folderName(QStringLiteral("Trip"), albumId, {QStringLiteral("Trip"), suffixed});
        QVERIFY(name != suffixed);
        QVERIFY(name.contains(albumId));
    }

    void neverCollidesWithTheReservedInternalFolders()
    {
        const QString trash = QString::fromLatin1(kTrashFolderName);
        // A leading dot is stripped by sanitize, so an album literally named
        // ".immich-trash" cannot reach the reserved set — but one named after the
        // undotted form must still not claim it.
        const QString name = folderName(trash, QStringLiteral("abcdef12"), {});
        QVERIFY(name != trash);
    }

    void truncatesTheSuffixedNameToTheLimit()
    {
        const QString base = QString(300, QLatin1Char('a'));
        const QString name = folderName(base, QStringLiteral("11111111-2222"), {sanitize(base)});
        QVERIFY(name.toUtf8().size() <= 255);
    }

    // MARK: - Asset filenames

    void keepsTheOriginalFilenameWhenFree()
    {
        QCOMPARE(localFilename(QStringLiteral("IMG_0001.HEIC"), Fixture::checksum(1), {}),
                 QStringLiteral("IMG_0001.HEIC"));
    }

    void disambiguatesWithTheChecksumNotACounter()
    {
        const auto checksum = Fixture::checksum(7);
        const QString name = localFilename(QStringLiteral("IMG_0001.HEIC"),
                                           checksum,
                                           {QStringLiteral("IMG_0001.HEIC")});
        QCOMPARE(name, QStringLiteral("IMG_0001~%1.HEIC").arg(checksum.shortHex()));
    }

    void keepsTheExtensionWhenDisambiguating()
    {
        const QString name = localFilename(QStringLiteral("photo.jpeg"),
                                           Fixture::checksum(2),
                                           {QStringLiteral("photo.jpeg")});
        QVERIFY(name.endsWith(QStringLiteral(".jpeg")));
    }

    void handlesFilenamesWithNoExtension()
    {
        const auto checksum = Fixture::checksum(3);
        const QString name =
            localFilename(QStringLiteral("photo"), checksum, {QStringLiteral("photo")});
        QCOMPARE(name, QStringLiteral("photo~%1").arg(checksum.shortHex()));
    }

    void sanitisesAssetFilenames()
    {
        QCOMPARE(sanitizeFilename(QStringLiteral("a/b:c.jpg")), QStringLiteral("a-b-c.jpg"));
        QCOMPARE(sanitizeFilename(QStringLiteral(".hidden.jpg")), QStringLiteral("hidden.jpg"));
        QCOMPARE(sanitizeFilename(QString()), QStringLiteral("asset"));
    }

    /// Unlike an album name, a filename keeps a trailing dot — `sanitizeFilename` has
    /// no reason to trim one, and the macOS build does not either.
    void filenameSanitisationKeepsTrailingDots()
    {
        QCOMPARE(sanitizeFilename(QStringLiteral("photo.")), QStringLiteral("photo."));
    }

    // MARK: - Internal names

    void recognisesInternalNames()
    {
        QVERIFY(isInternalName(QString::fromLatin1(kTrashFolderName)));
        QVERIFY(isInternalName(QString::fromLatin1(kStagingFolderName)));
        QVERIFY(isInternalName(QString::fromLatin1(kMarkerFilename)));
        QVERIFY(isInternalName(QStringLiteral(".anything")));
        QVERIFY(!isInternalName(QStringLiteral("Holiday 2024")));
    }

    // MARK: - Marker round trip

    void writesAndReadsTheMarker()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        AlbumMarker marker;
        marker.albumId = QStringLiteral("album-1");
        marker.albumName = QStringLiteral("Holiday 2024");
        marker.folderName = QStringLiteral("Holiday 2024");

        QString error;
        QVERIFY2(writeMarker(marker, directory.path(), &error), qUtf8Printable(error));

        const auto read = readMarker(directory.path());
        QVERIFY(read.has_value());
        QCOMPARE(*read, marker);
    }

    /// The marker is what lets the whole database be rebuilt from disk, so its key
    /// names are part of the on-disk contract with the macOS build.
    void markerUsesTheAgreedKeyNames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        AlbumMarker marker;
        marker.albumId = QStringLiteral("album-1");
        marker.albumName = QStringLiteral("Trip");
        marker.folderName = QStringLiteral("Trip");
        QVERIFY(writeMarker(marker, directory.path(), nullptr));

        QFile file(markerPath(directory.path()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString contents = QString::fromUtf8(file.readAll());

        QVERIFY(contents.contains(QStringLiteral("\"albumID\"")));
        QVERIFY(contents.contains(QStringLiteral("\"albumName\"")));
        QVERIFY(contents.contains(QStringLiteral("\"folderName\"")));
        QVERIFY(contents.contains(QStringLiteral("\"schema\"")));
    }

    void rejectsAMarkerFromANewerSchema()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QFile file(markerPath(directory.path()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"albumID":"a","albumName":"n","folderName":"f","schema":99})");
        file.close();

        QVERIFY(!readMarker(directory.path()).has_value());
    }

    void rejectsAMarkerWithNoAlbumId()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QFile file(markerPath(directory.path()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"albumID":"","albumName":"n","folderName":"f","schema":1})");
        file.close();

        QVERIFY(!readMarker(directory.path()).has_value());
    }

    void rejectsUnparseableMarkers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QFile file(markerPath(directory.path()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not json at all");
        file.close();

        QVERIFY(!readMarker(directory.path()).has_value());
    }

    void reportsNoMarkerWhenTheFileIsAbsent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(!readMarker(directory.path()).has_value());
    }
};

QTEST_APPLESS_MAIN(FolderAndFileNamingTest)
#include "FolderAndFileNamingTest.moc"
