#pragma once

#include "storage/Records.h"
#include "sync/SyncPlan.h"

#include <QDateTime>
#include <QList>

namespace immichksync {

/// Refuses to execute a plan that would strip an implausible share of an album.
///
/// Two-way sync reads "the files are gone" and "the user deleted them" as the same
/// observation. Usually that is right; it is catastrophically wrong when an external
/// drive was unmounted, a folder was dragged to the desktop, or a sync root was
/// pointed at the wrong place. The gate converts that class of accident from silent
/// data movement into a visible, reversible prompt.
struct SafetyGate {
    /// Fraction of an album's tracked assets that may disappear without review.
    double removalRatioThreshold = 0.25;
    /// Below this many removals the ratio is not meaningful — a two-photo album should
    /// not need confirmation to lose one photo.
    int minimumRemovalsBeforeGating = 10;

    struct Verdict {
        bool isHeld = false;
        int removals = 0;
        int tracked = 0;
    };

    struct Outcome {
        Verdict verdict;
        /// The withheld work, so it can be shown to the user for confirmation.
        QList<HeldRemoval> held;
    };

    Verdict evaluate(const AlbumPlan &plan) const;

    /// Strips removals from `plan` when the gate trips.
    Outcome apply(AlbumPlan &plan, const QDateTime &now) const;

    bool operator==(const SafetyGate &other) const
    {
        return qFuzzyCompare(removalRatioThreshold, other.removalRatioThreshold)
            && minimumRemovalsBeforeGating == other.minimumRemovalsBeforeGating;
    }
};

} // namespace immichksync
