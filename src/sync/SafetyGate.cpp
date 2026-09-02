#include "sync/SafetyGate.h"

#include "core/Logging.h"

#include <algorithm>

namespace immichksync {

SafetyGate::Verdict SafetyGate::evaluate(const AlbumPlan &plan) const
{
    const int removals = plan.removalCount();
    if (removals <= minimumRemovalsBeforeGating) {
        return {};
    }

    const int tracked = std::max(plan.baselineCount, 1);
    const double ratio = static_cast<double>(removals) / static_cast<double>(tracked);
    if (ratio <= removalRatioThreshold) {
        return {};
    }

    return {true, removals, plan.baselineCount};
}

SafetyGate::Outcome SafetyGate::apply(AlbumPlan &plan, const QDateTime &now) const
{
    Outcome outcome;
    outcome.verdict = evaluate(plan);
    if (!outcome.verdict.isHeld) {
        return outcome;
    }

    log::sync.warning(
        QStringLiteral("Safety hold on “%1”: %2 of %3 tracked assets would be removed. "
                       "Nothing was removed; confirm in Settings ▸ Albums.")
            .arg(plan.albumName)
            .arg(outcome.verdict.removals)
            .arg(outcome.verdict.tracked));

    outcome.held.reserve(plan.removalCount());
    for (const PlannedAlbumRemoval &removal : std::as_const(plan.albumRemovals)) {
        HeldRemoval held;
        held.albumId = removal.baseline.albumId;
        held.checksum = removal.baseline.checksum;
        held.direction = HeldRemoval::Direction::RemoveFromAlbum;
        held.displayName = removal.baseline.originalFileName;
        held.detectedAt = now;
        outcome.held.append(held);
    }
    for (const PlannedLocalTrashing &trashing : std::as_const(plan.localTrashings)) {
        HeldRemoval held;
        held.albumId = trashing.baseline.albumId;
        held.checksum = trashing.baseline.checksum;
        held.direction = HeldRemoval::Direction::TrashLocalFile;
        held.displayName = trashing.local.filename;
        held.detectedAt = now;
        outcome.held.append(held);
    }

    plan.albumRemovals.clear();
    plan.localTrashings.clear();
    return outcome;
}

} // namespace immichksync
