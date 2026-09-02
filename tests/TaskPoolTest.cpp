#include "core/TaskPool.h"

#include <QAtomicInt>
#include <QTest>
#include <QThread>

using namespace immichksync;

/// The bound is the point: each unit of work in production is a multi-megabyte
/// transfer, and an unbounded map would open one connection per asset.
class TaskPoolTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void returnsResultsInInputOrder()
    {
        QList<int> items;
        for (int i = 0; i < 50; ++i) {
            items.append(i);
        }

        const QList<int> results = TaskPool::map<int, int>(items, 8, [](const int &value) {
            // Sleeping in reverse order means an unordered pool would visibly scramble
            // the results.
            QThread::msleep(static_cast<unsigned long>(50 - value) / 10);
            return value * 2;
        });

        QCOMPARE(results.size(), items.size());
        for (int i = 0; i < items.size(); ++i) {
            QCOMPARE(results[i], i * 2);
        }
    }

    void neverExceedsTheConcurrencyLimit()
    {
        QAtomicInt inFlight = 0;
        QAtomicInt peak = 0;

        QList<int> items;
        for (int i = 0; i < 40; ++i) {
            items.append(i);
        }

        TaskPool::map<int, int>(items, 4, [&](const int &value) {
            const int current = inFlight.fetchAndAddOrdered(1) + 1;
            int observed = peak.loadAcquire();
            while (current > observed && !peak.testAndSetOrdered(observed, current)) {
                observed = peak.loadAcquire();
            }
            QThread::msleep(2);
            inFlight.fetchAndSubOrdered(1);
            return value;
        });

        QVERIFY2(peak.loadAcquire() <= 4,
                 qUtf8Printable(QStringLiteral("peak concurrency was %1").arg(peak.loadAcquire())));
    }

    void handlesAnEmptyInput()
    {
        const QList<int> results = TaskPool::map<int, int>({}, 4, [](const int &v) { return v; });
        QVERIFY(results.isEmpty());
    }

    void handlesASingleItem()
    {
        const QList<int> results = TaskPool::map<int, int>({7}, 4, [](const int &v) { return v * 3; });
        QCOMPARE(results, QList<int>{21});
    }

    /// A concurrency of one must still run everything, serially.
    void runsSeriallyWhenTheLimitIsOne()
    {
        QAtomicInt peak = 0;
        QAtomicInt inFlight = 0;

        QList<int> items;
        for (int i = 0; i < 10; ++i) {
            items.append(i);
        }

        const QList<int> results = TaskPool::map<int, int>(items, 1, [&](const int &value) {
            const int current = inFlight.fetchAndAddOrdered(1) + 1;
            if (current > peak.loadAcquire()) {
                peak.storeRelease(current);
            }
            inFlight.fetchAndSubOrdered(1);
            return value;
        });

        QCOMPARE(results.size(), 10);
        QCOMPARE(peak.loadAcquire(), 1);
    }

    /// A zero or negative limit is a caller bug, not a reason to run unbounded.
    void clampsANonPositiveLimitToOne()
    {
        const QList<int> results = TaskPool::map<int, int>({1, 2, 3}, 0, [](const int &v) {
            return v + 1;
        });
        QCOMPARE(results, (QList<int>{2, 3, 4}));
    }

    void chunkedSplitsAtTheRequestedSize()
    {
        QList<int> items;
        for (int i = 0; i < 10; ++i) {
            items.append(i);
        }

        const auto chunks = chunked(items, 4);
        QCOMPARE(chunks.size(), 3);
        QCOMPARE(chunks[0].size(), 4);
        QCOMPARE(chunks[1].size(), 4);
        QCOMPARE(chunks[2].size(), 2);
    }

    void chunkedHandlesEdgeCases()
    {
        QCOMPARE(chunked(QList<int>{}, 5).size(), 0);
        QCOMPARE(chunked(QList<int>{1, 2}, 5).size(), 1);
        QCOMPARE(chunked(QList<int>{1, 2, 3, 4}, 2).size(), 2);
        // An exact multiple must not produce a trailing empty chunk.
        QCOMPARE(chunked(QList<int>{1, 2, 3, 4}, 4).size(), 1);
    }
};

QTEST_APPLESS_MAIN(TaskPoolTest)
#include "TaskPoolTest.moc"
