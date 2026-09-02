#include "Fixtures.h"

#include "core/Clock.h"
#include "filesystem/AlbumFolderLayout.h"
#include "filesystem/AtomicFileWriter.h"
#include "immich/ImmichClient.h"
#include "immich/ServerDiscovery.h"
#include "notifications/UserNotifier.h"
#include "storage/SyncStore.h"
#include "sync/SyncEngine.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QCryptographicHash>
#include <QUuid>

using namespace immichksync;

namespace {

/// A 1×1 PNG, distinct per seed, so every test asset has its own checksum.
QByteArray tinyPng(int seed)
{
    // A PNG with a tEXt chunk carrying the seed: valid enough for Immich to accept,
    // and unique per seed without needing an image library.
    QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5E"
        "rkJggg==");
    // Append a comment so the bytes differ; trailing data after IEND is tolerated by
    // every decoder and by the server's checksum, which is all this needs.
    png.append(QStringLiteral("\n# immichksync-test-%1").arg(seed).toUtf8());
    return png;
}

} // namespace

/// Walks the whole matrix against a real server: download, upload, local delete,
/// remote removal, both rename directions, album creation, cross-album deduplication,
/// the safety gate holding, and both ways of resolving a hold.
///
/// Opt-in:
///   make test-live IMMICH_TEST_SERVER=http://localhost:2283 IMMICH_TEST_API_KEY=…
///
/// The harness creates its own albums, excludes every other album so a real library is
/// never touched, and deletes what it created.
class LiveServerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        const QByteArray server = qgetenv("IMMICH_TEST_SERVER");
        const QByteArray key = qgetenv("IMMICH_TEST_API_KEY");
        if (server.isEmpty() || key.isEmpty()) {
            QSKIP("Set IMMICH_TEST_SERVER and IMMICH_TEST_API_KEY to run the live suite.");
        }

        m_transport = std::make_unique<NetworkTransport>();
        const auto baseUrl =
            ServerDiscovery::resolveApiBaseUrl(QString::fromLocal8Bit(server), m_transport.get());
        QVERIFY2(baseUrl.has_value(), "IMMICH_TEST_SERVER is not a usable address");
        m_baseUrl = *baseUrl;
        m_credentials = ImmichCredentials::apiKey(QString::fromLocal8Bit(key));

        const auto profile =
            ServerDiscovery::probe(m_baseUrl, m_credentials, m_transport.get());
        QVERIFY2(profile.succeeded(), qUtf8Printable(profile.error.message()));
        QVERIFY2(profile->isUsable(),
                 "the API key is missing permissions the suite needs; Test Connection names them");
        m_profile = *profile;

        // A unique prefix, so a failed run never collides with the next one.
        m_prefix = QStringLiteral("ImmichKSync Test %1")
                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    }

    void init()
    {
        if (!m_transport) {
            QSKIP("live suite not configured");
        }
        m_directory = std::make_unique<QTemporaryDir>();
        QVERIFY(m_directory->isValid());

        m_store = std::make_unique<SyncStore>();
        QVERIFY(m_store->open(m_directory->filePath(QStringLiteral("state.sqlite")), nullptr));

        m_notifier = std::make_unique<UserNotifier>();
        m_notifier->setDryRun(true);

        m_engine = std::make_unique<SyncEngine>(m_store.get(),
                                                m_transport.get(),
                                                m_notifier.get());
        m_engine->applyConfiguration(settings(), m_credentials);

        // Everything not created by this test is excluded, so a real library is never
        // touched however badly a test goes wrong.
        excludeExistingAlbums();
    }

    void cleanup()
    {
        deleteCreatedAlbums();
        m_engine.reset();
        m_store.reset();
        m_notifier.reset();
        m_directory.reset();
    }

    // MARK: - The matrix

    void downloadsAnAssetAddedOnTheServer()
    {
        const QString albumName = uniqueName(QStringLiteral("download"));
        const QString albumId = createAlbum(albumName);
        QVERIFY(!albumId.isEmpty());
        QVERIFY(uploadAndAdd(albumId, QStringLiteral("remote.png"), tinyPng(1)));

        m_engine->runOnce();

        const QString folder = QDir(root()).filePath(albumName);
        QVERIFY2(QDir(folder).exists(), "the album folder should have been created");
        QVERIFY2(QFileInfo::exists(QDir(folder).filePath(QStringLiteral("remote.png"))),
                 "the asset should have been downloaded");
        QVERIFY(AlbumFolderLayout::readMarker(folder).has_value());
    }

    void uploadsAFileDroppedIntoAnAlbumFolder()
    {
        const QString albumName = uniqueName(QStringLiteral("upload"));
        const QString albumId = createAlbum(albumName);
        m_engine->runOnce();

        const QString folder = QDir(root()).filePath(albumName);
        QVERIFY(writeAsset(folder, QStringLiteral("local.png"), tinyPng(2)));
        m_engine->runOnce();

        QCOMPARE(remoteAssetCount(albumId), 1);
    }

    void createsAnAlbumForANewFolder()
    {
        const QString folderName = uniqueName(QStringLiteral("new-folder"));
        const QString folder = QDir(root()).filePath(folderName);
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(writeAsset(folder, QStringLiteral("a.png"), tinyPng(3)));

        m_engine->runOnce();

        const auto marker = AlbumFolderLayout::readMarker(folder);
        QVERIFY2(marker.has_value(), "a new folder should have become an album");
        m_createdAlbums.append(marker->albumId);
        QCOMPARE(remoteAssetCount(marker->albumId), 1);
    }

    void removesFromTheAlbumWhenTheLocalFileIsDeleted()
    {
        const QString albumName = uniqueName(QStringLiteral("local-delete"));
        const QString albumId = createAlbum(albumName);
        QVERIFY(uploadAndAdd(albumId, QStringLiteral("gone.png"), tinyPng(4)));
        m_engine->runOnce();

        const QString path =
            QDir(QDir(root()).filePath(albumName)).filePath(QStringLiteral("gone.png"));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFile::remove(path));

        m_engine->runOnce();

        QCOMPARE(remoteAssetCount(albumId), 0);
        // The asset must remain in the library — only the album entry goes.
        QVERIFY2(assetStillExists(m_lastUploadedAssetId),
                 "removing a local file must never delete the asset from the library");
    }

    void trashesTheLocalFileWhenTheAssetLeavesTheAlbum()
    {
        const QString albumName = uniqueName(QStringLiteral("remote-remove"));
        const QString albumId = createAlbum(albumName);
        QVERIFY(uploadAndAdd(albumId, QStringLiteral("leaving.png"), tinyPng(5)));
        m_engine->runOnce();

        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        QVERIFY(client.removeAssets(albumId, {m_lastUploadedAssetId}).succeeded());

        m_engine->runOnce();

        const QString original =
            QDir(QDir(root()).filePath(albumName)).filePath(QStringLiteral("leaving.png"));
        QVERIFY2(!QFileInfo::exists(original), "the local file should have left the album folder");

        const QString trashed =
            QDir(QDir(QDir(root()).filePath(QString::fromLatin1(
                          AlbumFolderLayout::kTrashFolderName)))
                     .filePath(albumName))
                .filePath(QStringLiteral("leaving.png"));
        QVERIFY2(QFileInfo::exists(trashed), "the local file should be in .immich-trash, not gone");
    }

    void followsAnAlbumRenamedOnTheServer()
    {
        const QString albumName = uniqueName(QStringLiteral("remote-rename"));
        const QString albumId = createAlbum(albumName);
        m_engine->runOnce();
        QVERIFY(QDir(QDir(root()).filePath(albumName)).exists());

        const QString renamed = uniqueName(QStringLiteral("renamed-remotely"));
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        QVERIFY(client.renameAlbum(albumId, renamed).succeeded());

        m_engine->runOnce();

        QVERIFY2(QDir(QDir(root()).filePath(renamed)).exists(),
                 "the folder should have followed the album name");
        QVERIFY(!QDir(QDir(root()).filePath(albumName)).exists());
    }

    void pushesAFolderRenamedLocally()
    {
        const QString albumName = uniqueName(QStringLiteral("local-rename"));
        const QString albumId = createAlbum(albumName);
        m_engine->runOnce();

        const QString renamed = uniqueName(QStringLiteral("renamed-locally"));
        QVERIFY(QFile::rename(QDir(root()).filePath(albumName), QDir(root()).filePath(renamed)));

        m_engine->runOnce();

        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        const auto album = client.album(albumId);
        QVERIFY(album.succeeded());
        QCOMPARE(album->albumName, renamed);
    }

    /// The same bytes in two album folders must upload once and join both albums.
    void deduplicatesAcrossAlbums()
    {
        const QString firstName = uniqueName(QStringLiteral("dedup-a"));
        const QString secondName = uniqueName(QStringLiteral("dedup-b"));
        const QString firstId = createAlbum(firstName);
        const QString secondId = createAlbum(secondName);
        m_engine->runOnce();

        const QByteArray bytes = tinyPng(6);
        QVERIFY(writeAsset(QDir(root()).filePath(firstName), QStringLiteral("shared.png"), bytes));
        QVERIFY(writeAsset(QDir(root()).filePath(secondName), QStringLiteral("shared.png"), bytes));

        m_engine->runOnce();

        QCOMPARE(remoteAssetCount(firstId), 1);
        QCOMPARE(remoteAssetCount(secondId), 1);
        QCOMPARE(assetIdIn(firstId), assetIdIn(secondId));
    }

    void doesNothingOnASecondCycleWithNoChanges()
    {
        const QString albumName = uniqueName(QStringLiteral("steady"));
        const QString albumId = createAlbum(albumName);
        QVERIFY(uploadAndAdd(albumId, QStringLiteral("a.png"), tinyPng(7)));
        m_engine->runOnce();

        SyncCycleSummary second;
        connect(m_engine.get(),
                &SyncEngine::cycleFinished,
                this,
                [&second](const SyncCycleSummary &summary) { second = summary; });
        m_engine->runOnce();

        QCOMPARE(second.downloaded, 0);
        QCOMPARE(second.uploaded, 0);
        QCOMPARE(second.removedFromAlbums, 0);
        QCOMPARE(second.movedToTrash, 0);
    }

    // MARK: - The safety gate

    void holdsAnImplausibleNumberOfRemovals()
    {
        const QString albumName = uniqueName(QStringLiteral("safety"));
        const QString albumId = createAlbum(albumName);
        for (int i = 0; i < 12; ++i) {
            QVERIFY(uploadAndAdd(albumId,
                                 QStringLiteral("file-%1.png").arg(i),
                                 tinyPng(100 + i)));
        }
        m_engine->runOnce();

        const QString folder = QDir(root()).filePath(albumName);
        QCOMPARE(QDir(folder).entryList(QDir::Files).size(), 12);

        // Simulate the drive going away: every file disappears at once.
        for (const QString &name : QDir(folder).entryList(QDir::Files)) {
            QFile::remove(QDir(folder).filePath(name));
        }
        m_engine->runOnce();

        QVERIFY2(!m_store->albumsWithHeldRemovals().isEmpty(), "the gate should have held");
        QCOMPARE(remoteAssetCount(albumId), 12);
    }

    void applyingAHoldPerformsTheRemovals()
    {
        const QString albumName = uniqueName(QStringLiteral("safety-apply"));
        const QString albumId = holdAnAlbum(albumName);

        m_engine->applyHeldRemovals(albumId);
        m_engine->runOnce();

        QCOMPARE(remoteAssetCount(albumId), 0);
        QVERIFY(m_store->albumsWithHeldRemovals().isEmpty());
    }

    void restoringAHoldFetchesTheFilesBack()
    {
        const QString albumName = uniqueName(QStringLiteral("safety-restore"));
        const QString albumId = holdAnAlbum(albumName);

        m_engine->restoreHeldRemovals(albumId);
        m_engine->runOnce();

        QCOMPARE(remoteAssetCount(albumId), 12);
        QCOMPARE(QDir(QDir(root()).filePath(albumName)).entryList(QDir::Files).size(), 12);
        QVERIFY(m_store->albumsWithHeldRemovals().isEmpty());
    }

private:
    // MARK: - Harness

    SyncSettings settings() const
    {
        SyncSettings settings;
        settings.apiBaseUrl = m_baseUrl;
        settings.authMode = ImmichAuthMode::ApiKey;
        settings.rootFolder = root();
        // No settle window: the test writes files and reconciles immediately.
        settings.settleWindowSeconds = 1;
        return settings;
    }

    QString root() const { return m_directory->path(); }

    QString uniqueName(const QString &suffix) const
    {
        return QStringLiteral("%1 %2").arg(m_prefix, suffix);
    }

    void excludeExistingAlbums()
    {
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        const auto albums = client.ownedAlbums();
        if (!albums.succeeded()) {
            return;
        }
        for (const AlbumResponse &album : *albums) {
            if (album.albumName.startsWith(m_prefix)) {
                continue;
            }
            AlbumRecord record;
            record.albumId = album.id;
            record.albumName = album.albumName;
            record.folderName = album.id; // never used; the album is excluded
            record.isExcluded = true;
            m_store->upsert(record);
        }
    }

    QString createAlbum(const QString &name)
    {
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        const auto album = client.createAlbum(name);
        if (!album.succeeded()) {
            return {};
        }
        m_createdAlbums.append(album->id);
        return album->id;
    }

    void deleteCreatedAlbums()
    {
        if (!m_transport) {
            return;
        }
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        for (const QString &albumId : m_createdAlbums) {
            client.deleteAlbum(albumId);
        }
        m_createdAlbums.clear();
    }

    bool writeAsset(const QString &folder, const QString &name, const QByteArray &bytes)
    {
        QDir().mkpath(folder);
        QFile file(QDir(folder).filePath(name));
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        file.write(bytes);
        file.close();
        // Backdate past the settle window so the very next cycle picks it up.
        AtomicFileWriter::applyModificationTime(file.fileName(),
                                                QDateTime::currentDateTimeUtc().addSecs(-60));
        return true;
    }

    /// Uploads bytes straight to the server and adds them to an album, bypassing the
    /// engine — this is how a test sets up "the server already has this".
    bool uploadAndAdd(const QString &albumId, const QString &name, const QByteArray &bytes)
    {
        const QString staging = QDir(root()).filePath(QStringLiteral(".test-staging"));
        QDir().mkpath(staging);
        const QString path = QDir(staging).filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        file.write(bytes);
        file.close();

        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        ImmichClient::UploadRequest request;
        request.filePath = path;
        request.filename = name;
        request.sha1Hex = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
        request.fileCreatedAt = QDateTime::currentDateTimeUtc();
        request.fileModifiedAt = QDateTime::currentDateTimeUtc();

        const auto uploaded = client.upload(request, staging);
        if (!uploaded.succeeded()) {
            return false;
        }
        m_lastUploadedAssetId = uploaded->id;
        QFile::remove(path);
        return client.addAssets(albumId, {uploaded->id}).succeeded();
    }

    int remoteAssetCount(const QString &albumId)
    {
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        const auto album = client.album(albumId);
        return album.succeeded() ? album->assetCount : -1;
    }

    QString assetIdIn(const QString &albumId)
    {
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        const auto assets =
            client.albumAssets(albumId, m_profile.supportsCursorPagination());
        return assets.succeeded() && !assets->isEmpty() ? assets->first().id : QString();
    }

    bool assetStillExists(const QString &assetId)
    {
        if (assetId.isEmpty()) {
            return false;
        }
        ImmichClient client(m_baseUrl, m_credentials, m_transport.get());
        const QString staging = QDir(root()).filePath(QStringLiteral(".test-staging"));
        QDir().mkpath(staging);
        const auto fetched = client.downloadOriginal(assetId, staging);
        if (fetched.succeeded()) {
            QFile::remove(fetched->path);
            return true;
        }
        return false;
    }

    /// Builds an album with twelve assets and then makes every local file disappear,
    /// leaving the gate holding.
    QString holdAnAlbum(const QString &albumName)
    {
        const QString albumId = createAlbum(albumName);
        for (int i = 0; i < 12; ++i) {
            uploadAndAdd(albumId, QStringLiteral("file-%1.png").arg(i), tinyPng(200 + i));
        }
        m_engine->runOnce();

        const QString folder = QDir(root()).filePath(albumName);
        for (const QString &name : QDir(folder).entryList(QDir::Files)) {
            QFile::remove(QDir(folder).filePath(name));
        }
        m_engine->runOnce();
        return albumId;
    }

    std::unique_ptr<NetworkTransport> m_transport;
    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<SyncStore> m_store;
    std::unique_ptr<UserNotifier> m_notifier;
    std::unique_ptr<SyncEngine> m_engine;

    QUrl m_baseUrl;
    ImmichCredentials m_credentials;
    ServerProfile m_profile;
    QString m_prefix;
    QString m_lastUploadedAssetId;
    QStringList m_createdAlbums;
};

QTEST_MAIN(LiveServerTest)
#include "LiveServerTest.moc"
