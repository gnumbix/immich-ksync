#include "StubTransport.h"

#include "immich/ImmichClient.h"
#include "immich/ServerDiscovery.h"

#include <QTest>

using namespace immichksync;

namespace {

QUrl baseUrl()
{
    return QUrl(QStringLiteral("https://immich.example.com/api"));
}

/// The near-zero policy the client tests use.
RetryPolicy fastRetries()
{
    RetryPolicy policy;
    policy.baseDelay = Milliseconds{1};
    policy.maximumDelay = Milliseconds{1};
    return policy;
}

ImmichClient makeClient(StubTransport &transport,
                        std::optional<ImmichCredentials> credentials
                        = ImmichCredentials::apiKey(QStringLiteral("test-key")))
{
    ImmichClient client(baseUrl(), std::move(credentials), &transport);
    // Retry *behaviour* is what these tests assert on; the waiting is not, so the
    // ladder is collapsed to a millisecond.
    client.setRetryPolicy(fastRetries());
    return client;
}

QJsonObject albumJson(const QString &id, const QString &name, int assetCount = 0)
{
    QJsonObject album;
    album.insert(QStringLiteral("id"), id);
    album.insert(QStringLiteral("albumName"), name);
    album.insert(QStringLiteral("updatedAt"), QStringLiteral("2024-01-01T00:00:00.000Z"));
    album.insert(QStringLiteral("assetCount"), assetCount);
    return album;
}

QJsonObject assetJson(const QString &id, const QString &checksum, const QString &filename)
{
    QJsonObject asset;
    asset.insert(QStringLiteral("id"), id);
    asset.insert(QStringLiteral("checksum"), checksum);
    asset.insert(QStringLiteral("originalFileName"), filename);
    asset.insert(QStringLiteral("fileCreatedAt"), QStringLiteral("2024-01-01T00:00:00.000Z"));
    asset.insert(QStringLiteral("fileModifiedAt"), QStringLiteral("2024-01-01T00:00:00.000Z"));
    return asset;
}

QJsonObject searchPage(const QJsonArray &items,
                       const QString &nextCursor = QString(),
                       const QString &nextPage = QString())
{
    QJsonObject assets;
    assets.insert(QStringLiteral("items"), items);
    if (!nextCursor.isEmpty()) {
        assets.insert(QStringLiteral("nextCursor"), nextCursor);
    }
    if (!nextPage.isEmpty()) {
        assets.insert(QStringLiteral("nextPage"), nextPage);
    }
    QJsonObject root;
    root.insert(QStringLiteral("assets"), assets);
    return root;
}

} // namespace

/// What actually goes on the wire. These tests are the contract with the Immich API:
/// a change here means the server was misread, not that the app was refactored.
class ImmichWireFormatTest : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // MARK: - Request shape

    void sendsTheApiKeyAsAHeader()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        makeClient(transport).ping();

        QCOMPARE(transport.lastRequest().headers.value(QStringLiteral("x-api-key")),
                 QStringLiteral("test-key"));
        QVERIFY(!transport.lastRequest().headers.contains(QStringLiteral("Authorization")));
    }

    void sendsASessionTokenAsABearerHeader()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        makeClient(transport, ImmichCredentials::sessionToken(QStringLiteral("tok"))).ping();

        QCOMPARE(transport.lastRequest().headers.value(QStringLiteral("Authorization")),
                 QStringLiteral("Bearer tok"));
        QVERIFY(!transport.lastRequest().headers.contains(QStringLiteral("x-api-key")));
    }

    void joinsTheBasePathWithTheEndpointPath()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        makeClient(transport).ping();

        QCOMPARE(transport.lastRequest().url.toString(),
                 QStringLiteral("https://immich.example.com/api/server/ping"));
    }

    void sendsNoCredentialHeaderWhenSignedOut()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        ImmichClient(baseUrl(), std::nullopt, &transport).ping();

        QVERIFY(!transport.lastRequest().headers.contains(QStringLiteral("x-api-key")));
        QVERIFY(!transport.lastRequest().headers.contains(QStringLiteral("Authorization")));
    }

    // MARK: - Albums

    void requestsOnlyOwnedAlbums()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonArray{albumJson(QStringLiteral("a"), QStringLiteral("Trip"))});

        const auto albums = makeClient(transport).ownedAlbums();
        QVERIFY(albums.succeeded());
        QCOMPARE(albums->size(), 1);
        QCOMPARE(albums->first().albumName, QStringLiteral("Trip"));
        // Albums shared *with* the user are out of scope for this app.
        QVERIFY(transport.lastRequest().url.query().contains(QStringLiteral("isOwned=true")));
    }

    void createsAnAlbumByName()
    {
        StubTransport transport;
        transport.enqueueJson(albumJson(QStringLiteral("new"), QStringLiteral("Holiday")));

        const auto album = makeClient(transport).createAlbum(QStringLiteral("Holiday"));
        QVERIFY(album.succeeded());
        QCOMPARE(album->id, QStringLiteral("new"));

        const HttpRequest request = transport.lastRequest();
        QCOMPARE(request.method, QStringLiteral("POST"));
        QCOMPARE(StubTransport::bodyOf(request).value(QStringLiteral("albumName")).toString(),
                 QStringLiteral("Holiday"));
    }

    void renamesAnAlbumWithPatch()
    {
        StubTransport transport;
        transport.enqueueJson(albumJson(QStringLiteral("a"), QStringLiteral("Renamed")));

        makeClient(transport).renameAlbum(QStringLiteral("a"), QStringLiteral("Renamed"));

        const HttpRequest request = transport.lastRequest();
        QCOMPARE(request.method, QStringLiteral("PATCH"));
        QVERIFY(request.url.path().endsWith(QStringLiteral("/albums/a")));
        QCOMPARE(StubTransport::bodyOf(request).value(QStringLiteral("albumName")).toString(),
                 QStringLiteral("Renamed"));
    }

    void addsAndRemovesAlbumAssets()
    {
        StubTransport transport;
        QJsonArray results;
        results.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("asset-1")},
                                   {QStringLiteral("success"), true}});
        transport.enqueueJson(results);

        const auto added =
            makeClient(transport).addAssets(QStringLiteral("album-1"),
                                            {QStringLiteral("asset-1")});
        QVERIFY(added.succeeded());
        QCOMPARE(added->size(), 1);
        QVERIFY(added->first().success);

        const HttpRequest request = transport.lastRequest();
        QCOMPARE(request.method, QStringLiteral("PUT"));
        QCOMPARE(StubTransport::bodyOf(request).value(QStringLiteral("ids")).toArray().size(), 1);
    }

    void removalUsesDelete()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonArray{});
        makeClient(transport).removeAssets(QStringLiteral("album-1"), {QStringLiteral("asset-1")});
        QCOMPARE(transport.lastRequest().method, QStringLiteral("DELETE"));
    }

    void skipsTheRequestEntirelyForAnEmptyAssetList()
    {
        StubTransport transport;
        const auto result = makeClient(transport).addAssets(QStringLiteral("album-1"), {});
        QVERIFY(result.succeeded());
        QVERIFY(result->isEmpty());
        QCOMPARE(transport.requestCount(), 0);
    }

    /// The API caps a bulk album mutation at 1000 IDs, so a larger set has to be split.
    void chunksLargeAssetListsAtTheApiLimit()
    {
        StubTransport transport;
        transport.setHandler([](const HttpRequest &) {
            return StubTransport::Exchange{200, QByteArray("[]"), {}, {}};
        });

        QStringList ids;
        for (int i = 0; i < 2500; ++i) {
            ids.append(QStringLiteral("asset-%1").arg(i));
        }
        QVERIFY(makeClient(transport).addAssets(QStringLiteral("album-1"), ids).succeeded());

        QCOMPARE(transport.requestCount(), 3);
        QCOMPARE(StubTransport::bodyOf(transport.requests().at(0))
                     .value(QStringLiteral("ids"))
                     .toArray()
                     .size(),
                 1000);
        QCOMPARE(StubTransport::bodyOf(transport.requests().at(2))
                     .value(QStringLiteral("ids"))
                     .toArray()
                     .size(),
                 500);
    }

    // MARK: - Search pagination
    //
    // `POST /search/metadata` rejects requests that mix the pre-3.2 flat fields with
    // the 3.2 filter/cursor shape, so the two shapes must never share a request.

    void usesTheCursorShapeOnNewerServers()
    {
        StubTransport transport;
        transport.enqueueJson(searchPage({assetJson(QStringLiteral("a"),
                                                    QStringLiteral("2jmj7l5rSw0yVb/vlWAYkK/YBwk="),
                                                    QStringLiteral("IMG.HEIC"))}));

        const auto assets =
            makeClient(transport).albumAssets(QStringLiteral("album-1"), /*cursor=*/true);
        QVERIFY(assets.succeeded());
        QCOMPARE(assets->size(), 1);

        const QJsonObject body = StubTransport::bodyOf(transport.lastRequest());
        QVERIFY(body.contains(QStringLiteral("filter")));
        QVERIFY(!body.contains(QStringLiteral("albumIds")));
        QVERIFY(!body.contains(QStringLiteral("page")));
        QCOMPARE(body.value(QStringLiteral("size")).toInt(), 1000);
    }

    void usesTheFlatShapeOnOlderServers()
    {
        StubTransport transport;
        transport.enqueueJson(searchPage({}));

        makeClient(transport).albumAssets(QStringLiteral("album-1"), /*cursor=*/false);

        const QJsonObject body = StubTransport::bodyOf(transport.lastRequest());
        QVERIFY(body.contains(QStringLiteral("albumIds")));
        QVERIFY(body.contains(QStringLiteral("page")));
        QVERIFY(!body.contains(QStringLiteral("filter")));
        QVERIFY(!body.contains(QStringLiteral("cursor")));
    }

    void followsTheCursorAcrossPages()
    {
        StubTransport transport;
        transport.enqueueJson(searchPage({assetJson(QStringLiteral("a"),
                                                    QStringLiteral("2jmj7l5rSw0yVb/vlWAYkK/YBwk="),
                                                    QStringLiteral("A.HEIC"))},
                                         QStringLiteral("cursor-2")));
        transport.enqueueJson(searchPage({assetJson(QStringLiteral("b"),
                                                    QStringLiteral("2jmj7l5rSw0yVb/vlWAYkK/YBwl="),
                                                    QStringLiteral("B.HEIC"))}));

        const auto assets =
            makeClient(transport).albumAssets(QStringLiteral("album-1"), /*cursor=*/true);
        QVERIFY(assets.succeeded());
        QCOMPARE(assets->size(), 2);
        QCOMPARE(transport.requestCount(), 2);
        QCOMPARE(StubTransport::bodyOf(transport.requests().at(1))
                     .value(QStringLiteral("cursor"))
                     .toString(),
                 QStringLiteral("cursor-2"));
    }

    void followsPageNumbersOnOlderServers()
    {
        StubTransport transport;
        transport.enqueueJson(searchPage({assetJson(QStringLiteral("a"),
                                                    QStringLiteral("2jmj7l5rSw0yVb/vlWAYkK/YBwk="),
                                                    QStringLiteral("A.HEIC"))},
                                         QString(),
                                         QStringLiteral("2")));
        transport.enqueueJson(searchPage({}));

        const auto assets =
            makeClient(transport).albumAssets(QStringLiteral("album-1"), /*cursor=*/false);
        QVERIFY(assets.succeeded());
        QCOMPARE(transport.requestCount(), 2);
        QCOMPARE(StubTransport::bodyOf(transport.requests().at(1))
                     .value(QStringLiteral("page"))
                     .toInt(),
                 2);
    }

    /// A server that never stops advancing must not spin forever.
    void stopsWhenAPageNumberDoesNotAdvance()
    {
        StubTransport transport;
        transport.setHandler([](const HttpRequest &) {
            QJsonObject assets;
            assets.insert(QStringLiteral("items"), QJsonArray{});
            assets.insert(QStringLiteral("nextPage"), QStringLiteral("1"));
            QJsonObject root;
            root.insert(QStringLiteral("assets"), assets);
            return StubTransport::Exchange{
                200, QJsonDocument(root).toJson(QJsonDocument::Compact), {}, {}};
        });

        QVERIFY(makeClient(transport)
                    .albumAssets(QStringLiteral("album-1"), /*cursor=*/false)
                    .succeeded());
        QCOMPARE(transport.requestCount(), 1);
    }

    // MARK: - Upload pre-check

    void sendsHexChecksumsToBulkUploadCheck()
    {
        StubTransport transport;
        QJsonObject body;
        body.insert(QStringLiteral("results"), QJsonArray{});
        transport.enqueueJson(body);

        makeClient(transport).bulkUploadCheck(
            {{QStringLiteral("Album/a.jpg"),
              QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709")}});

        const QJsonArray assets =
            StubTransport::bodyOf(transport.lastRequest()).value(QStringLiteral("assets")).toArray();
        QCOMPARE(assets.size(), 1);
        QCOMPARE(assets.at(0).toObject().value(QStringLiteral("checksum")).toString(),
                 QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
        QCOMPARE(assets.at(0).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("Album/a.jpg"));
    }

    void readsBackABulkUploadRejection()
    {
        StubTransport transport;
        QJsonArray results;
        results.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("Album/a.jpg")},
                                   {QStringLiteral("action"), QStringLiteral("reject")},
                                   {QStringLiteral("assetId"), QStringLiteral("existing-1")},
                                   {QStringLiteral("isTrashed"), true}});
        QJsonObject body;
        body.insert(QStringLiteral("results"), results);
        transport.enqueueJson(body);

        const auto checked = makeClient(transport).bulkUploadCheck(
            {{QStringLiteral("Album/a.jpg"), QStringLiteral("abc")}});
        QVERIFY(checked.succeeded());
        QCOMPARE(checked->size(), 1);
        QVERIFY(checked->first().isReject());
        QCOMPARE(checked->first().assetId, QStringLiteral("existing-1"));
        QVERIFY(checked->first().isTrashed);
    }

    // MARK: - Error handling

    void surfacesTheServerMessageFromAnErrorBody()
    {
        StubTransport transport;
        transport.enqueueError(404, QStringLiteral("Album not found"));

        const auto album = makeClient(transport).album(QStringLiteral("missing"));
        QVERIFY(!album.succeeded());
        QCOMPARE(album.error.httpStatus(), 404);
        QVERIFY(album.error.message().contains(QStringLiteral("Album not found")));
    }

    void reportsAnHtmlPageAsNotAnImmichServer()
    {
        StubTransport transport;
        transport.enqueue({200, QByteArray("<html><body>Sign in</body></html>"), {}, {}});

        const auto user = makeClient(transport).currentUser();
        QVERIFY(!user.succeeded());
        QCOMPARE(user.error.kind(), ImmichError::Kind::NotAnImmichServer);
    }

    void reportsAWellFormedButUnexpectedBodyAsADecodingFailure()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("unexpected"), true}});

        const auto user = makeClient(transport).currentUser();
        QVERIFY(!user.succeeded());
        QCOMPARE(user.error.kind(), ImmichError::Kind::Decoding);
    }

    /// A wrong password is not transient, and repeated attempts trip server-side rate
    /// limiting.
    void neverRetriesLogin()
    {
        StubTransport transport;
        transport.setHandler([](const HttpRequest &) {
            return StubTransport::Exchange{500, QByteArray(R"({"message":"boom"})"), {}, {}};
        });

        makeClient(transport).login(QStringLiteral("a@b.c"), QStringLiteral("pw"));
        QCOMPARE(transport.requestCount(), 1);
    }

    void retriesIdempotentCallsOnAServerError()
    {
        StubTransport transport;
        transport.enqueueError(503, QStringLiteral("busy"));
        transport.enqueueJson(QJsonArray{albumJson(QStringLiteral("a"), QStringLiteral("Trip"))});

        const auto albums = makeClient(transport).ownedAlbums();
        QVERIFY(albums.succeeded());
        QCOMPARE(transport.requestCount(), 2);
    }

    void doesNotRetryAClientError()
    {
        StubTransport transport;
        transport.setHandler([](const HttpRequest &) {
            return StubTransport::Exchange{403, QByteArray(R"({"message":"denied"})"), {}, {}};
        });

        QVERIFY(!makeClient(transport).ownedAlbums().succeeded());
        QCOMPARE(transport.requestCount(), 1);
    }

    // MARK: - Server discovery

    void normalisesUserInput()
    {
        using namespace ServerDiscovery;
        QCOMPARE(normalizedOrigin(QStringLiteral("immich.example.com"))->toString(),
                 QStringLiteral("https://immich.example.com"));
        QCOMPARE(normalizedOrigin(QStringLiteral("http://localhost:2283/"))->toString(),
                 QStringLiteral("http://localhost:2283"));
        QCOMPARE(normalizedOrigin(QStringLiteral("  https://a.example.com  "))->toString(),
                 QStringLiteral("https://a.example.com"));
    }

    void rejectsInputThatIsNotAnHttpUrl()
    {
        using namespace ServerDiscovery;
        QVERIFY(!normalizedOrigin(QString()).has_value());
        QVERIFY(!normalizedOrigin(QStringLiteral("   ")).has_value());
        QVERIFY(!normalizedOrigin(QStringLiteral("ftp://example.com")).has_value());
        QVERIFY(!normalizedOrigin(QStringLiteral("file:///etc/passwd")).has_value());
    }

    void discoversTheApiEndpointFromWellKnown()
    {
        StubTransport transport;
        QJsonObject api;
        api.insert(QStringLiteral("endpoint"), QStringLiteral("/custom-api"));
        QJsonObject body;
        body.insert(QStringLiteral("api"), api);
        transport.enqueueJson(body);

        const auto resolved =
            ServerDiscovery::resolveApiBaseUrl(QStringLiteral("https://immich.example.com"),
                                               &transport);
        QVERIFY(resolved.has_value());
        QCOMPARE(resolved->toString(), QStringLiteral("https://immich.example.com/custom-api"));
        QCOMPARE(transport.lastRequest().url.path(), QStringLiteral("/.well-known/immich"));
    }

    void fallsBackToTheConventionalApiPath()
    {
        StubTransport transport;
        transport.enqueue({404, QByteArray("not found"), {}, {}});

        const auto resolved =
            ServerDiscovery::resolveApiBaseUrl(QStringLiteral("https://immich.example.com"),
                                               &transport);
        QVERIFY(resolved.has_value());
        QCOMPARE(resolved->toString(), QStringLiteral("https://immich.example.com/api"));
    }

    /// Someone who already typed the full API address must not end up at `/api/api`.
    void respectsAnApiSuffixTheUserAlreadyTyped()
    {
        StubTransport transport;
        transport.enqueue({404, QByteArray("not found"), {}, {}});

        const auto resolved =
            ServerDiscovery::resolveApiBaseUrl(QStringLiteral("https://immich.example.com/api"),
                                               &transport);
        QVERIFY(resolved.has_value());
        QCOMPARE(resolved->toString(), QStringLiteral("https://immich.example.com/api"));
    }

    void probeCollectsEverythingTheEngineNeeds()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("major"), 3},
                                          {QStringLiteral("minor"), 2},
                                          {QStringLiteral("patch"), 0}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("id"), QStringLiteral("user-1")},
                                          {QStringLiteral("email"), QStringLiteral("a@b.c")},
                                          {QStringLiteral("name"), QStringLiteral("Test")}});
        transport.enqueueJson(QJsonObject{
            {QStringLiteral("image"), QJsonArray{QStringLiteral(".jpg")}},
            {QStringLiteral("video"), QJsonArray{QStringLiteral(".mp4")}},
            {QStringLiteral("sidecar"), QJsonArray{QStringLiteral(".xmp")}}});
        transport.enqueueJson(
            QJsonObject{{QStringLiteral("permissions"), QJsonArray{QStringLiteral("all")}}});

        const auto profile = ServerDiscovery::probe(baseUrl(),
                                                    ImmichCredentials::apiKey(QStringLiteral("k")),
                                                    &transport,
                                                    fastRetries());
        QVERIFY2(profile.succeeded(), qUtf8Printable(profile.error.message()));
        QCOMPARE(profile->user.email, QStringLiteral("a@b.c"));
        QVERIFY(profile->supportsCursorPagination());
        QVERIFY(profile->isUsable());
        QVERIFY(profile->missingPermissions.isEmpty());
        QVERIFY(profile->mediaTypes.image.contains(QStringLiteral(".jpg")));
    }

    /// The user is told exactly which permission to add, rather than discovering it
    /// through a 403 halfway through the first sync.
    void probeNamesMissingApiKeyPermissions()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("major"), 3},
                                          {QStringLiteral("minor"), 1},
                                          {QStringLiteral("patch"), 0}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("id"), QStringLiteral("user-1")}});
        transport.enqueueError(404, QStringLiteral("no media types"));
        transport.enqueueJson(QJsonObject{
            {QStringLiteral("permissions"),
             QJsonArray{QStringLiteral("album.read"), QStringLiteral("asset.read")}}});

        const auto profile = ServerDiscovery::probe(baseUrl(),
                                                    ImmichCredentials::apiKey(QStringLiteral("k")),
                                                    &transport,
                                                    fastRetries());
        QVERIFY(profile.succeeded());
        QVERIFY(!profile->isUsable());
        QVERIFY(profile->missingPermissions.contains(ImmichPermission::AssetUpload));
        QVERIFY(!profile->missingPermissions.contains(ImmichPermission::AlbumRead));
        // A server too old for cursor pagination must fall back to page numbers.
        QVERIFY(!profile->supportsCursorPagination());
        // Media types failed, so the built-in catalogue stands in.
        QVERIFY(profile->mediaTypes.image.contains(QStringLiteral(".heic")));
    }

    /// A bad address must be reported as a bad address, not as an authentication
    /// failure — the ping goes first for exactly this reason.
    void probeReportsAnUnreachableServerBeforeCheckingCredentials()
    {
        StubTransport transport;
        StubTransport::Exchange failure;
        failure.error = ImmichError::transport(QNetworkReply::HostNotFoundError,
                                               QStringLiteral("host not found"));
        transport.setHandler([failure](const HttpRequest &) { return failure; });

        const auto profile = ServerDiscovery::probe(baseUrl(),
                                                    ImmichCredentials::apiKey(QStringLiteral("k")),
                                                    &transport,
                                                    fastRetries());
        QVERIFY(!profile.succeeded());
        QCOMPARE(profile.error.kind(), ImmichError::Kind::Transport);
        QVERIFY(!profile.error.isAuthenticationFailure());
    }

    void probeSkipsThePermissionCheckForPasswordSignIn()
    {
        StubTransport transport;
        transport.enqueueJson(QJsonObject{{QStringLiteral("res"), QStringLiteral("pong")}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("major"), 3},
                                          {QStringLiteral("minor"), 2},
                                          {QStringLiteral("patch"), 0}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("id"), QStringLiteral("user-1")}});
        transport.enqueueJson(QJsonObject{{QStringLiteral("image"), QJsonArray{}},
                                          {QStringLiteral("video"), QJsonArray{}},
                                          {QStringLiteral("sidecar"), QJsonArray{}}});

        const auto profile =
            ServerDiscovery::probe(baseUrl(),
                                   ImmichCredentials::sessionToken(QStringLiteral("t")),
                                   &transport,
                                   fastRetries());
        QVERIFY(profile.succeeded());
        QVERIFY(!profile->hasPermissionInformation);
        QVERIFY(profile->isUsable());
        // Four calls, not five: a session token has no API key behind it.
        QCOMPARE(transport.requestCount(), 4);
    }
};

QTEST_APPLESS_MAIN(ImmichWireFormatTest)
#include "ImmichWireFormatTest.moc"
