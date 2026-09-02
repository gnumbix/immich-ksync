#include "Fixtures.h"

#include "sync/SyncPlanner.h"

#include <QTest>

using namespace immichksync;

namespace {

SyncPlanner makePlanner()
{
    return SyncPlanner(std::make_shared<FixedDateProvider>());
}

AlbumStructureAction createLocalFolder(const QString &name)
{
    AlbumStructureAction action;
    action.kind = AlbumStructureAction::Kind::CreateLocalFolder;
    action.name = name;
    return action;
}

AlbumStructureAction createRemoteAlbum(const QString &name)
{
    AlbumStructureAction action;
    action.kind = AlbumStructureAction::Kind::CreateRemoteAlbum;
    action.name = name;
    return action;
}

AlbumStructureAction trashLocalFolder(const QString &name)
{
    AlbumStructureAction action;
    action.kind = AlbumStructureAction::Kind::TrashLocalFolder;
    action.name = name;
    return action;
}

AlbumStructureAction renameLocalFolder(const QString &from, const QString &to)
{
    AlbumStructureAction action;
    action.kind = AlbumStructureAction::Kind::RenameLocalFolder;
    action.fromName = from;
    action.name = to;
    return action;
}

AlbumStructureAction renameRemoteAlbum(const QString &albumId, const QString &to)
{
    AlbumStructureAction action;
    action.kind = AlbumStructureAction::Kind::RenameRemoteAlbum;
    action.albumId = albumId;
    action.name = to;
    return action;
}

} // namespace

/// How folders and albums come into existence, get renamed, and are retired. A rename
/// that goes the wrong way looks exactly like a delete-and-recreate to the other side,
/// so each direction is pinned down separately.
class AlbumStructureTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void createsFolderForNewAlbum()
    {
        Fixture::InputOptions options;
        options.remote = {Fixture::remoteAsset(1)};
        options.marker = std::nullopt;
        options.folderName = std::nullopt;
        options.reservedFolderNames = {};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.structureActions,
                 QList<AlbumStructureAction>{createLocalFolder(QStringLiteral("Album"))});
        QVERIFY(plan.writesMarker);
        QCOMPARE(plan.downloads.size(), 1);
    }

    void createsAlbumForNewFolder()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1, QStringLiteral("Trip")),
                         Fixture::localAsset(2, QStringLiteral("Trip"))};
        options.marker = std::nullopt;
        options.album = std::nullopt;
        options.folderName = QStringLiteral("Trip");
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.structureActions,
                 QList<AlbumStructureAction>{createRemoteAlbum(QStringLiteral("Trip"))});
        QCOMPARE(plan.uploads.size(), 2);
        QVERIFY(plan.writesMarker);
    }

    void trashesFolderWhenAlbumDeleted()
    {
        Fixture::InputOptions options;
        options.local = {Fixture::localAsset(1)};
        options.baseline = Fixture::baselineMap({1});
        options.album = std::nullopt;
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.structureActions,
                 QList<AlbumStructureAction>{trashLocalFolder(QStringLiteral("Album"))});
        // A deleted album must not be recreated by re-uploading its former contents.
        QVERIFY(plan.uploads.isEmpty());
        QCOMPARE(plan.baselineDrops.size(), 1);
    }

    void followsRemoteRename()
    {
        Fixture::InputOptions options;
        options.marker = Fixture::marker(QStringLiteral("Album"), QStringLiteral("Album"));
        options.album = Fixture::remoteAlbum(QStringLiteral("album-1"), QStringLiteral("Summer 2026"));
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.structureActions,
                 QList<AlbumStructureAction>{
                     renameLocalFolder(QStringLiteral("Album"), QStringLiteral("Summer 2026"))});
        QCOMPARE(plan.folderName, QStringLiteral("Summer 2026"));
        QVERIFY(plan.writesMarker);
    }

    void pushesLocalRename()
    {
        Fixture::InputOptions options;
        options.marker = Fixture::marker(QStringLiteral("Album"), QStringLiteral("Album"));
        options.folderName = QStringLiteral("Renamed Locally");
        options.reservedFolderNames = {QStringLiteral("Renamed Locally")};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.structureActions,
                 QList<AlbumStructureAction>{
                     renameRemoteAlbum(QStringLiteral("album-1"), QStringLiteral("Renamed Locally"))});
        QCOMPARE(plan.albumName, QStringLiteral("Renamed Locally"));
    }

    void remoteRenameBeatsLocalRename()
    {
        Fixture::InputOptions options;
        options.marker = Fixture::marker(QStringLiteral("Album"), QStringLiteral("Album"));
        options.album = Fixture::remoteAlbum(QStringLiteral("album-1"), QStringLiteral("Server Name"));
        options.folderName = QStringLiteral("Local Name");
        options.reservedFolderNames = {QStringLiteral("Local Name")};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.structureActions,
                 QList<AlbumStructureAction>{
                     renameLocalFolder(QStringLiteral("Local Name"), QStringLiteral("Server Name"))});
        QCOMPARE(plan.albumName, QStringLiteral("Server Name"));
    }

    void leavesSettledAlbumAlone()
    {
        const AlbumPlan plan = makePlanner().plan(Fixture::input());

        QVERIFY(plan.structureActions.isEmpty());
        QVERIFY(!plan.writesMarker);
    }

    void rewritesMissingMarker()
    {
        Fixture::InputOptions options;
        options.marker = std::nullopt;
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QVERIFY(plan.structureActions.isEmpty());
        QVERIFY(plan.writesMarker);
    }

    // MARK: - Duplicate album names
    //
    // Immich allows two albums to share a name; two folders cannot. Planning one album
    // must therefore take the folder the previous one claimed into account, even though
    // neither folder exists on disk yet.

    void assignsDistinctFoldersToSameNamedAlbums()
    {
        QSet<QString> claimed;
        QStringList assigned;

        for (const QString &albumId : {QStringLiteral("11111111-0000-4000-8000-000000000000"),
                                       QStringLiteral("22222222-0000-4000-8000-000000000000")}) {
            Fixture::InputOptions options;
            options.marker = std::nullopt;
            options.album = Fixture::remoteAlbum(albumId, QStringLiteral("Trip"));
            options.folderName = std::nullopt;
            options.reservedFolderNames = claimed;

            const AlbumPlan plan = makePlanner().plan(Fixture::input(options));
            assigned.append(plan.folderName);
            claimed.insert(plan.folderName);
        }

        QCOMPARE(assigned[0], QStringLiteral("Trip"));
        QVERIFY(assigned[1] != assigned[0]);
        QCOMPARE(QSet<QString>(assigned.begin(), assigned.end()).size(), 2);
    }

    void doesNotRenameItselfAwayFromItsOwnFolder()
    {
        Fixture::InputOptions options;
        options.marker = Fixture::marker(QStringLiteral("Album"), QStringLiteral("Album"));
        options.reservedFolderNames = {};
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QCOMPARE(plan.folderName, QStringLiteral("Album"));
        QVERIFY(plan.structureActions.isEmpty());
    }

    /// A remote rename whose sanitised target is already the folder's name must not
    /// emit a no-op rename action — the folder is where it should be.
    void skipsRenameWhenTargetMatchesCurrentFolder()
    {
        Fixture::InputOptions options;
        options.marker = Fixture::marker(QStringLiteral("Old Name"), QStringLiteral("Album"));
        options.album = Fixture::remoteAlbum(QStringLiteral("album-1"), QStringLiteral("Album"));
        const AlbumPlan plan = makePlanner().plan(Fixture::input(options));

        QVERIFY(plan.structureActions.isEmpty());
        QVERIFY(plan.writesMarker);
    }
};

QTEST_APPLESS_MAIN(AlbumStructureTest)
#include "AlbumStructureTest.moc"
