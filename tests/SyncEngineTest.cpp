#include "Fixtures.h"

#include "notifications/UserNotifier.h"
#include "storage/SyncStore.h"
#include "sync/SyncEngine.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace immichksync;

namespace {

/// A transport that answers nothing, so the engine gets as far as trying to reach the
/// server and then stops. Enough to exercise the cycle's control flow without one.
///
/// The failure is deliberately a *non-retryable* one: a retryable error would make each
/// cycle wait out the full backoff ladder, and these tests run hundreds of cycles.
class DeadTransport : public Transport {
public:
    HttpResponse send(const HttpRequest &request) override
    {
        Q_UNUSED(request)
        ++calls;
        HttpResponse response;
        response.error = ImmichError::transport(QNetworkReply::ContentAccessDenied,
                                                QStringLiteral("no server in this test"));
        return response;
    }

    int calls = 0;
};

} // namespace

/// The engine's control flow: what runs, when, and — most importantly — that two
/// cycles never overlap on one folder.
class SyncEngineTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_directory = std::make_unique<QTemporaryDir>();
        QVERIFY(m_directory->isValid());

        m_store = std::make_unique<SyncStore>();
        QVERIFY(m_store->open(m_directory->filePath(QStringLiteral("state.sqlite")), nullptr));

        m_transport = std::make_unique<DeadTransport>();
        m_notifier = std::make_unique<UserNotifier>();
        m_notifier->setDryRun(true);

        m_engine = std::make_unique<SyncEngine>(m_store.get(),
                                                m_transport.get(),
                                                m_notifier.get(),
                                                std::make_shared<FixedDateProvider>());
    }

    void cleanup()
    {
        m_engine.reset();
        m_transport.reset();
        m_notifier.reset();
        m_store.reset();
        m_directory.reset();
    }

    /// A background agent that silently does nothing is the hardest kind of bug to
    /// diagnose, so an unconfigured engine has to say which piece is missing.
    void reportsWhichPieceOfConfigurationIsMissing()
    {
        QSignalSpy states(m_engine.get(), &SyncEngine::stateChanged);

        m_engine->runOnce();
        QVERIFY(!states.isEmpty());
        auto state = states.last().at(0).value<SyncState>();
        QCOMPARE(state.kind, SyncState::Kind::NotConfigured);
        QVERIFY(state.message.contains(QStringLiteral("server address")));

        SyncSettings settings;
        settings.apiBaseUrl = QUrl(QStringLiteral("https://immich.example.com/api"));
        m_engine->applyConfiguration(settings, std::nullopt);
        states.clear();
        m_engine->runOnce();
        state = states.last().at(0).value<SyncState>();
        QVERIFY(state.message.contains(QStringLiteral("Sign in")));

        settings.rootFolder = m_directory->path();
        m_engine->applyConfiguration(settings, std::nullopt);
        states.clear();
        m_engine->runOnce();
        state = states.last().at(0).value<SyncState>();
        QVERIFY(state.message.contains(QStringLiteral("Sign in")));
    }

    /// A missing volume must pause the sync with an explanation, never be read as the
    /// user having deleted everything.
    void refusesToRunWhenTheSyncFolderIsGone()
    {
        SyncSettings settings = configured();
        settings.rootFolder = m_directory->filePath(QStringLiteral("not-there"));
        m_engine->applyConfiguration(settings, ImmichCredentials::apiKey(QStringLiteral("k")));

        QSignalSpy states(m_engine.get(), &SyncEngine::stateChanged);
        m_engine->runOnce();

        const auto state = states.last().at(0).value<SyncState>();
        QCOMPARE(state.kind, SyncState::Kind::Failed);
        QVERIFY(state.message.contains(QStringLiteral("not available")));
        // It must not have gone anywhere near the server.
        QCOMPARE(m_transport->calls, 0);
    }

    /// The trigger that arrives mid-cycle must run afterwards, exactly once, without
    /// the two cycles overlapping.
    void aTriggerDuringACycleRunsAfterIt()
    {
        m_engine->applyConfiguration(configured(),
                                     ImmichCredentials::apiKey(QStringLiteral("k")));

        int cycles = 0;
        bool overlapped = false;
        bool inside = false;
        connect(m_engine.get(), &SyncEngine::stateChanged, this, [&](const SyncState &state) {
            if (state.kind == SyncState::Kind::Preparing) {
                if (inside) {
                    overlapped = true;
                }
                inside = true;
                ++cycles;
                if (cycles == 1) {
                    // Re-entering here is exactly what a folder change does.
                    m_engine->trigger(SyncTrigger::FolderChanged);
                }
            } else if (state.kind == SyncState::Kind::Idle
                       || state.kind == SyncState::Kind::Failed) {
                inside = false;
            }
        });

        m_engine->runOnce();

        QCOMPARE(cycles, 2);
        QVERIFY2(!overlapped, "two cycles must never run over the same folder at once");
    }

    /// Draining has to be iterative: a sustained stream of triggers would otherwise add
    /// a stack frame per cycle for as long as it kept up.
    void drainsALongRunOfTriggersWithoutRecursing()
    {
        m_engine->applyConfiguration(configured(),
                                     ImmichCredentials::apiKey(QStringLiteral("k")));

        int cycles = 0;
        connect(m_engine.get(), &SyncEngine::stateChanged, this, [&](const SyncState &state) {
            if (state.kind != SyncState::Kind::Preparing) {
                return;
            }
            ++cycles;
            if (cycles < 400) {
                m_engine->trigger(SyncTrigger::FolderChanged);
            }
        });

        m_engine->runOnce();
        QCOMPARE(cycles, 400);
    }

    void reportsAFailureToReachTheServer()
    {
        m_engine->applyConfiguration(configured(),
                                     ImmichCredentials::apiKey(QStringLiteral("k")));

        QSignalSpy errors(m_engine.get(), &SyncEngine::errorMessageChanged);
        m_engine->runOnce();

        QVERIFY(!errors.isEmpty());
        QVERIFY(!errors.last().at(0).toString().isEmpty());
        QVERIFY(m_transport->calls > 0);
    }

    void startAndStopAreIdempotent()
    {
        m_engine->applyConfiguration(configured(),
                                     ImmichCredentials::apiKey(QStringLiteral("k")));
        QVERIFY(!m_engine->isRunning());
        m_engine->start();
        QVERIFY(m_engine->isRunning());
        m_engine->start();
        QVERIFY(m_engine->isRunning());
        m_engine->stop();
        QVERIFY(!m_engine->isRunning());
        m_engine->stop();
        QVERIFY(!m_engine->isRunning());
    }

private:
    SyncSettings configured() const
    {
        SyncSettings settings;
        settings.apiBaseUrl = QUrl(QStringLiteral("https://immich.example.com/api"));
        settings.rootFolder = m_directory->path();
        return settings;
    }

    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<SyncStore> m_store;
    std::unique_ptr<DeadTransport> m_transport;
    std::unique_ptr<UserNotifier> m_notifier;
    std::unique_ptr<SyncEngine> m_engine;
};

QTEST_MAIN(SyncEngineTest)
#include "SyncEngineTest.moc"
