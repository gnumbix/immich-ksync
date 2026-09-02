#include "core/Retry.h"

#include <QElapsedTimer>
#include <QSet>
#include <QTest>

using namespace immichksync;

/// Retrying the wrong thing is worse than not retrying: a wrong password retried four
/// times with backoff is a two-minute wait ending in a message that says nothing.
class RetryTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void succeedsWithoutRetrying()
    {
        int attempts = 0;
        const RetryDecision decision =
            Retry::run(RetryPolicy::none(), QStringLiteral("op"), [&]() {
                ++attempts;
                return RetryDecision{true, false, std::nullopt, {}};
            });

        QVERIFY(decision.succeeded);
        QCOMPARE(attempts, 1);
    }

    void doesNotRetryAnUnretryableFailure()
    {
        int attempts = 0;
        RetryPolicy policy;
        policy.baseDelay = Milliseconds{1};
        policy.maximumDelay = Milliseconds{1};

        const RetryDecision decision = Retry::run(policy, QStringLiteral("op"), [&]() {
            ++attempts;
            return RetryDecision{false, false, std::nullopt, QStringLiteral("nope")};
        });

        QVERIFY(!decision.succeeded);
        QCOMPARE(attempts, 1);
    }

    void retriesUpToTheAttemptLimit()
    {
        int attempts = 0;
        RetryPolicy policy;
        policy.maximumAttempts = 3;
        policy.baseDelay = Milliseconds{1};
        policy.maximumDelay = Milliseconds{1};

        const RetryDecision decision = Retry::run(policy, QStringLiteral("op"), [&]() {
            ++attempts;
            return RetryDecision{false, true, std::nullopt, QStringLiteral("transient")};
        });

        QVERIFY(!decision.succeeded);
        QCOMPARE(attempts, 3);
    }

    void stopsAsSoonAsAnAttemptSucceeds()
    {
        int attempts = 0;
        RetryPolicy policy;
        policy.maximumAttempts = 5;
        policy.baseDelay = Milliseconds{1};
        policy.maximumDelay = Milliseconds{1};

        const RetryDecision decision = Retry::run(policy, QStringLiteral("op"), [&]() {
            ++attempts;
            return RetryDecision{attempts == 3, true, std::nullopt, {}};
        });

        QVERIFY(decision.succeeded);
        QCOMPARE(attempts, 3);
    }

    void honoursAServerSuppliedRetryAfter()
    {
        int attempts = 0;
        RetryPolicy policy;
        policy.maximumAttempts = 2;
        policy.baseDelay = Milliseconds{30000};
        policy.maximumDelay = Milliseconds{30000};

        QElapsedTimer timer;
        timer.start();
        Retry::run(policy, QStringLiteral("op"), [&]() {
            ++attempts;
            // A 1 ms Retry-After must win over the policy's 30 s backoff.
            return RetryDecision{false, true, Milliseconds{1}, QStringLiteral("busy")};
        });

        QCOMPARE(attempts, 2);
        QVERIFY2(timer.elapsed() < 5000, "Retry-After should have overridden the policy delay");
    }

    void backoffGrowsAndIsCapped()
    {
        RetryPolicy policy;
        policy.baseDelay = Milliseconds{1000};
        policy.maximumDelay = Milliseconds{30000};

        QCOMPARE(policy.delayForAttempt(0), Milliseconds{0});
        // Full jitter means the delay is a random point inside the window, so only the
        // ceiling is deterministic.
        for (int attempt = 1; attempt <= 10; ++attempt) {
            const Milliseconds delay = policy.delayForAttempt(attempt);
            QVERIFY(delay.count() >= 0);
            QVERIFY(delay <= policy.maximumDelay);
        }
    }

    /// Jitter matters when a cycle retries dozens of transfers that all failed at the
    /// same instant; without it they would all come back together.
    void backoffIsJittered()
    {
        RetryPolicy policy;
        policy.baseDelay = Milliseconds{1000};
        policy.maximumDelay = Milliseconds{30000};

        QSet<qint64> observed;
        for (int i = 0; i < 40; ++i) {
            observed.insert(policy.delayForAttempt(4).count());
        }
        QVERIFY2(observed.size() > 1, "delays should not be identical across calls");
    }

    void formatsSecondsForTheLog()
    {
        QCOMPARE(formatSeconds(Milliseconds{1500}), QStringLiteral("1.5s"));
        QCOMPARE(formatSeconds(Milliseconds{0}), QStringLiteral("0.0s"));
    }
};

QTEST_APPLESS_MAIN(RetryTest)
#include "RetryTest.moc"
