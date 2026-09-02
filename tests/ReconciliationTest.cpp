#include "Fixtures.h"

#include "sync/SyncPlanner.h"

#include <QTest>

using namespace immichksync;

namespace {

/// Convenience: the planner under test, with a clock that never moves.
SyncPlanner makePlanner()
{
    return SyncPlanner(std::make_shared<FixedDateProvider>());
}

QSet<QString> downloadNames(const AlbumPlan &plan)
{
    QSet<QString> names;
    for (const PlannedDownload &download : plan.downloads) {
        names.insert(download.filename);
    }
    return names;
}

} // namespace

/// The reconciliation table, case by case. This is the behaviour everything else in
/// the app exists to serve, so each row gets its own test.
class ReconciliationTest : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // MARK: R ✓ L ✓ B –

    void adoptsExistingAgreement()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        options.local = {Fixture::localAsset(1)};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QVERIFY(plan.downloads.isEmpty());
        QVERIFY(plan.uploads.isEmpty());
        QCOMPARE(plan.removalCount(), 0);
        QCOMPARE(plan.baselineAdoptions.size(), 1);
        QCOMPARE(plan.baselineAdoptions[0].assetId, QStringLiteral("asset-1"));
        QCOMPARE(plan.baselineAdoptions[0].relativePath, QStringLiteral("Album/IMG_0001.HEIC"));
    }

    // MARK: R ✓ L ✓ B ✓

    void steadyStateIsEmpty()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        options.local = {Fixture::localAsset(1)};
        options.baseline = Fixture::baselineMap({1});

        QVERIFY(makePlanner().plan(Fixture::input(options)).isEmpty());
    }

    void refreshesStaleBaselinePath()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        options.local = {Fixture::localAsset(1, QStringLiteral("Album"), QStringLiteral("Renamed.HEIC"))};
        options.baseline = Fixture::baselineMap({1});
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QVERIFY(plan.downloads.isEmpty());
        QVERIFY(plan.uploads.isEmpty());
        QCOMPARE(plan.baselineAdoptions.size(), 1);
        QCOMPARE(plan.baselineAdoptions[0].relativePath, QStringLiteral("Album/Renamed.HEIC"));
    }

    // MARK: R ✓ L – B –

    void downloadsNewRemoteAsset()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.downloads.size(), 1);
        QCOMPARE(plan.downloads[0].relativePath, QStringLiteral("Album/IMG_0001.HEIC"));
        QVERIFY(plan.uploads.isEmpty());
        QCOMPARE(plan.removalCount(), 0);
    }

    // MARK: R ✓ L – B ✓

    void removesFromAlbumWhenLocalFileDeleted()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        options.baseline = Fixture::baselineMap({1});
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.albumRemovals.size(), 1);
        QCOMPARE(plan.albumRemovals[0].baseline.assetId, QStringLiteral("asset-1"));
        // A tracked deletion must not be re-downloaded, or every deletion would
        // resurrect itself on the next cycle.
        QVERIFY(plan.downloads.isEmpty());
        QVERIFY(plan.localTrashings.isEmpty());
    }

    // MARK: R – L ✓ B –

    void uploadsNewLocalFile()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1)};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.uploads.size(), 1);
        QCOMPARE(plan.uploads[0].asset.relativePath, QStringLiteral("Album/IMG_0001.HEIC"));
        QVERIFY(plan.downloads.isEmpty());
    }

    // MARK: R – L ✓ B ✓

    void trashesLocalFileWhenRemovedRemotely()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1)};
        options.baseline = Fixture::baselineMap({1});
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.localTrashings.size(), 1);
        QCOMPARE(plan.localTrashings[0].local.filename, QStringLiteral("IMG_0001.HEIC"));
        QVERIFY(plan.uploads.isEmpty());
    }

    // MARK: R – L – B ✓

    void dropsBaselineWhenGoneEverywhere()
    {
        Fixture::InputOptions options;
        options.baseline = Fixture::baselineMap({1});
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.baselineDrops, QList<Sha1Checksum>{Fixture::checksum(1)});
        QCOMPARE(plan.transferCount(), 0);
        QCOMPARE(plan.removalCount(), 0);
    }

    // MARK: - Removal suppression

    void suppressesRemovalsWhenScanIncomplete()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        options.local = {Fixture::localAsset(2)};
        options.baseline = Fixture::baselineMap({1, 2});
        options.suppressRemovals = true;
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QVERIFY(plan.albumRemovals.isEmpty());
        QVERIFY(plan.localTrashings.isEmpty());
    }

    // MARK: - Download naming

    void disambiguatesCollidingDownloadNames()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1, QString(), QStringLiteral("IMG_0001.HEIC")),
                          Fixture::remoteAsset(2, QString(), QStringLiteral("IMG_0001.HEIC"))};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        const QSet<QString> names = downloadNames(plan);
        QCOMPARE(names.size(), 2);
        QVERIFY(names.contains(QStringLiteral("IMG_0001.HEIC")));
        bool foundDisambiguated = false;
        for (const QString &name : names) {
            if (name.contains(QLatin1Char('~')) && name.endsWith(QStringLiteral(".HEIC"))) {
                foundDisambiguated = true;
            }
        }
        QVERIFY(foundDisambiguated);
    }

    void avoidsOccupiedFilenames()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1, QString(), QStringLiteral("notes.HEIC"))};
        options.occupiedFilenames = {QStringLiteral("notes.HEIC")};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.downloads.size(), 1);
        QVERIFY(plan.downloads[0].filename != QStringLiteral("notes.HEIC"));
    }

    void isDeterministic()
    {
        Fixture::InputOptions options;
        for (int seed = 1; seed <= 5; ++seed) {
            options.remote.append(Fixture::remoteAsset(seed, QString(), QStringLiteral("same.jpg")));
        }
        const AlbumPlanInput input = Fixture::input(options);

        const AlbumPlan first = makePlanner().plan(input);
        const AlbumPlan second = makePlanner().plan(input);

        QCOMPARE(first.downloads.size(), second.downloads.size());
        for (int i = 0; i < first.downloads.size(); ++i) {
            QCOMPARE(first.downloads[i].filename, second.downloads[i].filename);
            QCOMPARE(first.downloads[i].asset.assetId, second.downloads[i].asset.assetId);
        }
    }

    // MARK: - Duplicate local files

    void ignoresDuplicateLocalCopies()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1, QStringLiteral("Album"), QStringLiteral("a.HEIC")),
                         Fixture::localAsset(1, QStringLiteral("Album"), QStringLiteral("b.HEIC"))};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.uploads.size(), 1);
        QCOMPARE(plan.uploads[0].asset.filename, QStringLiteral("a.HEIC"));
        QCOMPARE(plan.duplicateLocalPaths, QStringList{QStringLiteral("Album/b.HEIC")});
    }

    // MARK: - Unchanged-remote shortcut

    void treatsBaselineAsRemoteWhenUnchanged()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1)};
        options.baseline = Fixture::baselineMap({1});
        options.remoteUnchanged = true;

        // Nothing moved on either side, so nothing should happen.
        QVERIFY(makePlanner().plan(Fixture::input(options)).isEmpty());
    }

    void uploadsAgainstUnchangedRemote()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1), Fixture::localAsset(2)};
        options.baseline = Fixture::baselineMap({1});
        options.remoteUnchanged = true;
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.uploads.size(), 1);
        QCOMPARE(plan.uploads[0].asset.checksum, Fixture::checksum(2));
    }

    // MARK: - Nested folders

    void reportsNestedFoldersRatherThanFlatteningThem()
    {
        Fixture::InputOptions options;
        options.nestedFolderNames = {QStringLiteral("Raw")};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.unsupportedNestedFolders, QStringList{QStringLiteral("Raw")});
    }

    // MARK: - Nothing on either side

    void producesNoWorkWhenNeitherSideExists()
    {
        Fixture::InputOptions options;
        options.album = std::nullopt;
        options.folderName = std::nullopt;
        options.marker = std::nullopt;

        QVERIFY(makePlanner().plan(Fixture::input(options)).isEmpty());
    }
};

QTEST_APPLESS_MAIN(ReconciliationTest)
#include "ReconciliationTest.moc"
