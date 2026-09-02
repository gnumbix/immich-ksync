#pragma once

#include <QDateTime>

#include <memory>

namespace immichksync {

/// Injectable "now", so time-dependent policy (deep-scan intervals, write-settle
/// windows, retry schedules) can be tested without sleeping.
class DateProvider {
public:
    virtual ~DateProvider() = default;
    virtual QDateTime now() const = 0;
};

class SystemDateProvider : public DateProvider {
public:
    QDateTime now() const override { return QDateTime::currentDateTimeUtc(); }
};

/// Shared default, so call sites that do not care about time injection do not each
/// allocate one.
std::shared_ptr<DateProvider> systemDateProvider();

/// ISO-8601 with fractional seconds and a `Z` suffix — the exact shape
/// `AssetMediaCreateDto.fileCreatedAt` validates against server-side.
QString toImmichIso8601(const QDateTime &value);
QDateTime fromImmichIso8601(const QString &value);

} // namespace immichksync
