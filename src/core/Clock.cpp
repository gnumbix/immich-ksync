#include "core/Clock.h"

#include <QTimeZone>

namespace immichksync {

std::shared_ptr<DateProvider> systemDateProvider()
{
    static const auto provider = std::make_shared<SystemDateProvider>();
    return provider;
}

QString toImmichIso8601(const QDateTime &value)
{
    return value.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz'Z'"));
}

QDateTime fromImmichIso8601(const QString &value)
{
    // Immich sends fractional seconds on some fields and not others, and either a `Z`
    // or a numeric offset. QDateTime's ISO parser handles all four shapes; anything
    // else is a server we do not understand, and a null QDateTime says so honestly.
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value, Qt::ISODate);
    }
    if (!parsed.isValid()) {
        return {};
    }
    if (parsed.timeSpec() == Qt::LocalTime) {
        // A timestamp with no zone is UTC as far as the API is concerned; reading it as
        // local time would shift every capture date by the machine's offset.
        parsed.setTimeZone(QTimeZone::UTC);
    }
    return parsed.toUTC();
}

} // namespace immichksync
