#pragma once

#include "core/Logging.h"

#include <QString>

#include <chrono>
#include <functional>
#include <optional>

namespace immichksync {

using Milliseconds = std::chrono::milliseconds;

/// Exponential backoff with full jitter, matching the retry behaviour of the official
/// Immich CLI but adding `Retry-After` support so a server under load is not hammered.
struct RetryPolicy {
    int maximumAttempts = 4;
    Milliseconds baseDelay{1000};
    Milliseconds maximumDelay{30000};

    static RetryPolicy standard() { return {}; }
    static RetryPolicy none() { return {1, Milliseconds{0}, Milliseconds{0}}; }

    /// Full-jitter backoff: `random(0 ... base * 2^attempt)`, capped. Jitter matters
    /// when a cycle retries dozens of transfers that all failed at the same moment.
    Milliseconds delayForAttempt(int attempt) const;
};

/// What one attempt decided. `Retry::run` owns the loop; the operation only reports.
struct RetryDecision {
    bool succeeded = false;
    bool isRetryable = false;
    /// Server-provided delay, which overrides the computed backoff.
    std::optional<Milliseconds> retryAfter;
    QString errorMessage;
};

namespace Retry {

/// Runs `operation` until it succeeds, is not retryable, or runs out of attempts.
///
/// `operation` returns a decision rather than throwing, so the retry loop needs no
/// exception machinery and every call site is explicit about what is transient.
/// Sleeps happen on the calling thread, which is always a worker.
RetryDecision run(const RetryPolicy &policy,
                  const QString &label,
                  const std::function<RetryDecision()> &operation);

inline RetryDecision run(const QString &label, const std::function<RetryDecision()> &operation)
{
    return run(RetryPolicy::standard(), label, operation);
}

} // namespace Retry

QString formatSeconds(Milliseconds duration);

} // namespace immichksync
