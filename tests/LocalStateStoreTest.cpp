#include "Fixtures.h"

#include "storage/Migrations.h"
#include "storage/SyncStore.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace immichksync;

namespace {

AlbumRecord albumRecord(const QString &id = QStringLiteral("album-1"),
                        const QString &name = QStringLiteral("Album"),
                        const QString &folder = QStringLiteral("Album"))
{
    AlbumRecord record;
    record.albumId = id;
    record.albumName = name;
    record.folderName = folder;
    record.remoteUpdatedAt = QStringLiteral("2024-01-01T00:00:00.000Z");
    record.hasRemoteUpdatedAt = true;
    record.remoteAssetCount = 3;
    record.lastSyncedAt = Fixture::referenceDate();
    return record;
}

LocalFileFingerprint fingerprint(const QString &relativePath, int seed)
{
    LocalFileFingerprint fingerprint;
    fingerprint.relativePath = relativePath;
    fingerprint.deviceId = 1;
    fingerprint.inode = 1000 + seed;
    fingerprint.size = seed * 10;
    fingerprint.modifiedAtNanoseconds = 1'700'000'000'000'000'000LL + seed;
    fingerprint.checksum = Fixture::checksum(seed);
    return fingerprint;
}

} // namespace

/// The database is a cache of reconciled state: losing it costs a rediscovery, never
/// data. These tests pin down that every read returns exactly what was written, since
/// a silently mangled baseline is indistinguishable from a user deletion.
class LocalStateStoreTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_directory = std::make_unique<QTemporaryDir>();
        QVERIFY(m_directory->isValid());
        m_store = std::make_unique<SyncStore>();
        QString error;
        QVERIFY2(m_store->open(m_directory->filePath(QStringLiteral("state.sqlite")), &error),
                 qUtf8Printable(error));
    }

    void cleanup()
    {
        m_store.reset();
        m_directory.reset();
    }

    void appliesTheSchemaOnOpen()
    {
        // Opening an empty file must leave a usable database at the current version.
        QCOMPARE(m_store->albums().size(), 0);
        QCOMPARE(SchemaMigrations::currentVersion(), 1);
    }

    void roundTripsAnAlbum()
    {
        QVERIFY(m_store->upsert(albumRecord()));

        const auto read = m_store->album(QStringLiteral("album-1"));
        QVERIFY(read.has_value());
        QCOMPARE(read->albumName, QStringLiteral("Album"));
        QCOMPARE(read->folderName, QStringLiteral("Album"));
        QCOMPARE(read->remoteAssetCount, 3);
        QCOMPARE(read->remoteUpdatedAt, QStringLiteral("2024-01-01T00:00:00.000Z"));
        QVERIFY(!read->isExcluded);
        QVERIFY(!read->hasSafetyHold);
    }

    void findsAnAlbumByFolderName()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->albumByFolderName(QStringLiteral("Album")).has_value());
        QVERIFY(!m_store->albumByFolderName(QStringLiteral("Nope")).has_value());
    }

    /// An upsert that knows nothing new must not erase what a previous cycle recorded.
    void upsertPreservesTimestampsItDoesNotKnow()
    {
        AlbumRecord record = albumRecord();
        record.lastDeepScanAt = Fixture::referenceDate();
        QVERIFY(m_store->upsert(record));

        AlbumRecord partial = albumRecord();
        partial.lastDeepScanAt = QDateTime();
        partial.lastSyncedAt = QDateTime();
        QVERIFY(m_store->upsert(partial));

        const auto read = m_store->album(QStringLiteral("album-1"));
        QVERIFY(read.has_value());
        QCOMPARE(read->lastDeepScanAt, Fixture::referenceDate());
        QCOMPARE(read->lastSyncedAt, Fixture::referenceDate());
    }

    void togglesExclusionAndSafetyHold()
    {
        QVERIFY(m_store->upsert(albumRecord()));

        QVERIFY(m_store->setExcluded(true, QStringLiteral("album-1")));
        QCOMPARE(m_store->excludedAlbumIds(), QSet<QString>{QStringLiteral("album-1")});
        QVERIFY(m_store->album(QStringLiteral("album-1"))->isExcluded);

        QVERIFY(m_store->setSafetyHold(true, QStringLiteral("album-1")));
        QVERIFY(m_store->album(QStringLiteral("album-1"))->hasSafetyHold);
    }

    void roundTripsBaselines()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->upsert(Fixture::baseline(1)));
        QVERIFY(m_store->upsert(Fixture::baseline(2)));

        const auto baseline = m_store->baseline(QStringLiteral("album-1"));
        QCOMPARE(baseline.size(), 2);
        QVERIFY(baseline.contains(Fixture::checksum(1)));
        QCOMPARE(baseline.value(Fixture::checksum(1)).assetId, QStringLiteral("asset-1"));
        QCOMPARE(baseline.value(Fixture::checksum(1)).relativePath,
                 QStringLiteral("Album/IMG_0001.HEIC"));
        QCOMPARE(baseline.value(Fixture::checksum(2)).size, 2000);
    }

    void deletesASingleBaseline()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->upsert(Fixture::baseline(1)));
        QVERIFY(m_store->upsert(Fixture::baseline(2)));

        QVERIFY(m_store->deleteBaseline(QStringLiteral("album-1"), Fixture::checksum(1)));
        QCOMPARE(m_store->baseline(QStringLiteral("album-1")).size(), 1);
    }

    /// The schema declares ON DELETE CASCADE, which does nothing unless the connection
    /// enables foreign keys — so this is really a test that the pragma was applied.
    void deletingAnAlbumCascadesToItsBaselines()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->upsert(Fixture::baseline(1)));

        QVERIFY(m_store->deleteAlbum(QStringLiteral("album-1")));
        QVERIFY(m_store->baseline(QStringLiteral("album-1")).isEmpty());
    }

    void roundTripsFingerprints()
    {
        QVERIFY(m_store->upsert(fingerprint(QStringLiteral("Album/a.jpg"), 1),
                                Fixture::referenceDate()));

        const auto cached = m_store->fingerprints(QStringLiteral("Album"));
        QCOMPARE(cached.size(), 1);
        QCOMPARE(cached.value(QStringLiteral("Album/a.jpg")).checksum, Fixture::checksum(1));
        QCOMPARE(cached.value(QStringLiteral("Album/a.jpg")).inode, 1001);
    }

    /// An album folder containing `%` or `_` must not match its siblings through the
    /// LIKE prefix used to scope the cache.
    void fingerprintPrefixEscapesLikeWildcards()
    {
        QVERIFY(m_store->upsert(fingerprint(QStringLiteral("100%/a.jpg"), 1), Fixture::referenceDate()));
        QVERIFY(m_store->upsert(fingerprint(QStringLiteral("1000/b.jpg"), 2), Fixture::referenceDate()));

        const auto cached = m_store->fingerprints(QStringLiteral("100%"));
        QCOMPARE(cached.size(), 1);
        QVERIFY(cached.contains(QStringLiteral("100%/a.jpg")));
    }

    void prunesFingerprintsForFilesThatAreGone()
    {
        QVERIFY(m_store->upsert(fingerprint(QStringLiteral("Album/a.jpg"), 1), Fixture::referenceDate()));
        QVERIFY(m_store->upsert(fingerprint(QStringLiteral("Album/b.jpg"), 2), Fixture::referenceDate()));

        QVERIFY(m_store->pruneFingerprints(QStringLiteral("Album"),
                                           {QStringLiteral("Album/a.jpg")}));

        const auto cached = m_store->fingerprints(QStringLiteral("Album"));
        QCOMPARE(cached.size(), 1);
        QVERIFY(cached.contains(QStringLiteral("Album/a.jpg")));
    }

    void roundTripsHeldRemovals()
    {
        QVERIFY(m_store->upsert(albumRecord()));

        HeldRemoval removal;
        removal.albumId = QStringLiteral("album-1");
        removal.checksum = Fixture::checksum(1);
        removal.direction = HeldRemoval::Direction::TrashLocalFile;
        removal.displayName = QStringLiteral("IMG_0001.HEIC");
        removal.detectedAt = Fixture::referenceDate();

        QVERIFY(m_store->replaceHeldRemovals(QStringLiteral("album-1"), {removal}));

        const auto held = m_store->heldRemovals();
        QCOMPARE(held.size(), 1);
        QCOMPARE(held[0].direction, HeldRemoval::Direction::TrashLocalFile);
        QCOMPARE(held[0].checksum, Fixture::checksum(1));
        QCOMPARE(m_store->albumsWithHeldRemovals(), QSet<QString>{QStringLiteral("album-1")});

        QVERIFY(m_store->clearHeldRemovals(QStringLiteral("album-1")));
        QVERIFY(m_store->heldRemovals().isEmpty());
    }

    void replaceHeldRemovalsIsAWholesaleSwap()
    {
        QVERIFY(m_store->upsert(albumRecord()));

        HeldRemoval first;
        first.albumId = QStringLiteral("album-1");
        first.checksum = Fixture::checksum(1);
        first.displayName = QStringLiteral("a");
        first.detectedAt = Fixture::referenceDate();
        QVERIFY(m_store->replaceHeldRemovals(QStringLiteral("album-1"), {first}));

        HeldRemoval second = first;
        second.checksum = Fixture::checksum(2);
        second.displayName = QStringLiteral("b");
        QVERIFY(m_store->replaceHeldRemovals(QStringLiteral("album-1"), {second}));

        const auto held = m_store->heldRemovals();
        QCOMPARE(held.size(), 1);
        QCOMPARE(held[0].displayName, QStringLiteral("b"));
    }

    /// The back-off ladder is what stops one unreadable file from occupying a transfer
    /// slot on every single cycle.
    void backsOffRepeatedFailuresForLonger()
    {
        const QString key = TransferBackoff::uploadKey(QStringLiteral("album-1"),
                                                       QStringLiteral("Album/a.jpg"));
        const QDateTime now = Fixture::referenceDate();

        QVERIFY(m_store->recordFailure(key, QStringLiteral("boom"), now));
        QVERIFY(m_store->backedOffKeys(now).contains(key));
        // One minute later the first back-off has expired.
        QVERIFY(!m_store->backedOffKeys(now.addSecs(61)).contains(key));

        QVERIFY(m_store->recordFailure(key, QStringLiteral("boom"), now));
        // The second attempt waits four minutes, so a minute is no longer enough.
        QVERIFY(m_store->backedOffKeys(now.addSecs(61)).contains(key));

        QCOMPARE(m_store->failureCount(), 1);
        QVERIFY(m_store->clearFailure(key));
        QCOMPARE(m_store->failureCount(), 0);
    }

    void backoffLadderIsCappedAtSixHours()
    {
        const QDateTime now = Fixture::referenceDate();
        QCOMPARE(TransferBackoff::nextAttempt(1, now), now.addSecs(60));
        QCOMPARE(TransferBackoff::nextAttempt(2, now), now.addSecs(240));
        QCOMPARE(TransferBackoff::nextAttempt(99, now), now.addSecs(6 * 60 * 60));
    }

    void roundTripsMetaValues()
    {
        QVERIFY(!m_store->metaValue(QStringLiteral("identity")).has_value());
        QVERIFY(m_store->setMetaValue(QStringLiteral("identity"), QStringLiteral("server#user")));
        QCOMPARE(*m_store->metaValue(QStringLiteral("identity")), QStringLiteral("server#user"));

        QVERIFY(m_store->setMetaValue(QStringLiteral("identity"), QStringLiteral("other")));
        QCOMPARE(*m_store->metaValue(QStringLiteral("identity")), QStringLiteral("other"));

        QVERIFY(m_store->setMetaValue(QStringLiteral("identity"), std::nullopt));
        QVERIFY(!m_store->metaValue(QStringLiteral("identity")).has_value());
    }

    void reportsStatistics()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->upsert(Fixture::baseline(1)));
        QVERIFY(m_store->upsert(Fixture::baseline(2)));

        const SyncStore::Statistics statistics = m_store->statistics();
        QCOMPARE(statistics.albumCount, 1);
        QCOMPARE(statistics.syncedAssetCount, 2);
        QCOMPARE(statistics.syncedByteCount, 3000);
    }

    /// Excluded albums are not counted: the menu line says what is being synced.
    void statisticsIgnoreExcludedAlbums()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->setExcluded(true, QStringLiteral("album-1")));
        QCOMPARE(m_store->statistics().albumCount, 0);
    }

    void resetForgetsEverything()
    {
        QVERIFY(m_store->upsert(albumRecord()));
        QVERIFY(m_store->upsert(Fixture::baseline(1)));
        QVERIFY(m_store->upsert(fingerprint(QStringLiteral("Album/a.jpg"), 1), Fixture::referenceDate()));
        QVERIFY(m_store->setMetaValue(QStringLiteral("identity"), QStringLiteral("x")));

        QVERIFY(m_store->reset());

        QVERIFY(m_store->albums().isEmpty());
        QVERIFY(m_store->baseline(QStringLiteral("album-1")).isEmpty());
        QVERIFY(m_store->fingerprints(QStringLiteral("Album")).isEmpty());
        QVERIFY(!m_store->metaValue(QStringLiteral("identity")).has_value());
    }

    /// Reopening must find the schema already at the current version and leave the
    /// stored rows alone.
    void survivesReopening()
    {
        const QString path = m_directory->filePath(QStringLiteral("reopen.sqlite"));
        {
            SyncStore store;
            QVERIFY(store.open(path, nullptr));
            QVERIFY(store.upsert(albumRecord()));
        }
        SyncStore reopened;
        QVERIFY(reopened.open(path, nullptr));
        QCOMPARE(reopened.albums().size(), 1);
    }

private:
    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<SyncStore> m_store;
};

QTEST_APPLESS_MAIN(LocalStateStoreTest)
#include "LocalStateStoreTest.moc"
