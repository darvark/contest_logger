#pragma once

#include <QDateTime>

namespace logger {

inline constexpr qint64 StationOperatingStateStaleAfterMs = 45'000;

inline bool isStationOperatingStateStale(
    const QDateTime &updatedUtc, const QDateTime &nowUtc)
{
    return updatedUtc.isValid()
        && updatedUtc.msecsTo(nowUtc) > StationOperatingStateStaleAfterMs;
}

} // namespace logger
