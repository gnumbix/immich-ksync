#include "core/Retry.h"

#include <QRandomGenerator>
#include <QThread>

#include <algorithm>

namespace immichksync {

Milliseconds RetryPolicy::delayForAttempt(int attempt) const
{
    if (attempt <= 0) {
        return Milliseconds{0};
    }
    const int exponent = std::min(attempt - 1, 16);
    const qint64 scaled = baseDelay.count() * (qint64(1) << exponent);
    const qint64 capped = std::min(scaled, static_cast<qint64>(maximumDelay.count()));
    if (capped <= 0) {
        return Milliseconds{0};
    }
    return Milliseconds{QRandomGenerator::global()->bounded(qint64(1), capped + 1)};
}

QString formatSeconds(Milliseconds duration)
{
    return QStringLiteral("%1s").arg(duration.count() / 1000.0, 0, 'f', 1);
}

namespace Retry {

RetryDecision run(const RetryPolicy &policy,
                  const QString &label,
                  const std::function<RetryDecision()> &operation)
{
    int attempt = 0;
    while (true) {
        ++attempt;
        RetryDecision decision = operation();
        if (decision.succeeded || !decision.isRetryable || attempt >= policy.maximumAttempts) {
            return decision;
        }

        const Milliseconds delay = decision.retryAfter.value_or(policy.delayForAttempt(attempt));
        log::api.notice(QStringLiteral("%1 failed (attempt %2/%3), retrying in %4: %5")
                            .arg(label)
                            .arg(attempt)
                            .arg(policy.maximumAttempts)
                            .arg(formatSeconds(delay), decision.errorMessage));
        QThread::msleep(static_cast<unsigned long>(delay.count()));
    }
}

} // namespace Retry

} // namespace immichksync
