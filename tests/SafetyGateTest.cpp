#include "Fixtures.h"

#include "sync/SafetyGate.h"

#include <QTest>

using namespace immichksync;

namespace {

/// A plan with `removals` withheld-able removals against a baseline of `tracked`.
AlbumPlan planWithRemovals(int removals, int tracked)
{
    AlbumPlan plan;
    plan.albumId = QStringLiteral("album-1");
    plan.albumName = QStringLiteral("Album");
    plan.folderName = QStringLiteral("Album");
    plan.baselineCount = tracked;
    for (int i = 0; i < removals; ++i) {
        plan.albumRemovals.append(PlannedAlbumRemoval{Fixture::baseline(i + 1)});
    }
    return plan;
}

} // namespace

/// Two-way sync reads "the files are gone" and "the user deleted them" as the same
/// observation. The gate is what turns the disastrous reading of that ambiguity into a
/// visible, reversible prompt.
class SafetyGateTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void allowsSmallRemovals()
    {
        const SafetyGate gate;
        // 5 of 100 is well under both the ratio and the minimum.
        QVERIFY(!gate.evaluate(planWithRemovals(5, 100)).isHeld);
    }

    /// A two-photo album should not need confirmation to lose one photo, however
    /// large that is as a fraction.
    void allowsRemovalsBelowTheMinimumEvenAtAHighRatio()
    {
        const SafetyGate gate;
        QVERIFY(!gate.evaluate(planWithRemovals(10, 10)).isHeld);
    }

    void holdsWhenBothThresholdsAreExceeded()
    {
        const SafetyGate gate;
        const SafetyGate::Verdict verdict = gate.evaluate(planWithRemovals(50, 100));
        QVERIFY(verdict.isHeld);
        QCOMPARE(verdict.removals, 50);
        QCOMPARE(verdict.tracked, 100);
    }

    void holdsWhenTheWholeAlbumWouldGo()
    {
        const SafetyGate gate;
        QVERIFY(gate.evaluate(planWithRemovals(100, 100)).isHeld);
    }

    void respectsACustomRatio()
    {
        SafetyGate gate;
        gate.removalRatioThreshold = 0.9;
        QVERIFY(!gate.evaluate(planWithRemovals(50, 100)).isHeld);
        QVERIFY(gate.evaluate(planWithRemovals(95, 100)).isHeld);
    }

    void respectsACustomMinimum()
    {
        SafetyGate gate;
        gate.minimumRemovalsBeforeGating = 100;
        QVERIFY(!gate.evaluate(planWithRemovals(50, 60)).isHeld);
    }

    void stripsRemovalsAndReportsThemWhenHeld()
    {
        const SafetyGate gate;
        AlbumPlan plan = planWithRemovals(50, 100);
        const SafetyGate::Outcome outcome = gate.apply(plan, Fixture::referenceDate());

        QVERIFY(outcome.verdict.isHeld);
        QCOMPARE(outcome.held.size(), 50);
        QVERIFY(plan.albumRemovals.isEmpty());
        QVERIFY(plan.localTrashings.isEmpty());
        QCOMPARE(outcome.held[0].direction, HeldRemoval::Direction::RemoveFromAlbum);
        QCOMPARE(outcome.held[0].detectedAt, Fixture::referenceDate());
    }

    /// Transfers are unrelated to the gate: holding an album's removals must not stop
    /// it from downloading what is genuinely new.
    void leavesTransfersAloneWhenHolding()
    {
        const SafetyGate gate;
        AlbumPlan plan = planWithRemovals(50, 100);
        PlannedDownload download;
        download.asset = Fixture::remoteAsset(1);
        download.filename = QStringLiteral("IMG_0001.HEIC");
        plan.downloads.append(download);
        plan.uploads.append(PlannedUpload{Fixture::localAsset(2)});

        gate.apply(plan, Fixture::referenceDate());

        QCOMPARE(plan.downloads.size(), 1);
        QCOMPARE(plan.uploads.size(), 1);
    }

    void leavesAPassingPlanUntouched()
    {
        const SafetyGate gate;
        AlbumPlan plan = planWithRemovals(5, 100);
        const SafetyGate::Outcome outcome = gate.apply(plan, Fixture::referenceDate());

        QVERIFY(!outcome.verdict.isHeld);
        QVERIFY(outcome.held.isEmpty());
        QCOMPARE(plan.albumRemovals.size(), 5);
    }

    void recordsBothDirectionsOfRemoval()
    {
        const SafetyGate gate;
        AlbumPlan plan = planWithRemovals(20, 100);
        for (int i = 0; i < 20; ++i) {
            plan.localTrashings.append(
                PlannedLocalTrashing{Fixture::baseline(100 + i), Fixture::localAsset(100 + i)});
        }

        const SafetyGate::Outcome outcome = gate.apply(plan, Fixture::referenceDate());

        QVERIFY(outcome.verdict.isHeld);
        QCOMPARE(outcome.held.size(), 40);
        int fromAlbum = 0;
        int toTrash = 0;
        for (const HeldRemoval &removal : outcome.held) {
            removal.direction == HeldRemoval::Direction::RemoveFromAlbum ? ++fromAlbum : ++toTrash;
        }
        QCOMPARE(fromAlbum, 20);
        QCOMPARE(toTrash, 20);
    }

    /// An album with no baseline at all divides by a floor of 1 rather than by zero.
    void survivesAnEmptyBaseline()
    {
        const SafetyGate gate;
        const SafetyGate::Verdict verdict = gate.evaluate(planWithRemovals(20, 0));
        QVERIFY(verdict.isHeld);
        QCOMPARE(verdict.tracked, 0);
    }
};

QTEST_APPLESS_MAIN(SafetyGateTest)
#include "SafetyGateTest.moc"
