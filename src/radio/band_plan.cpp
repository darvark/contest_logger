#include "logger/radio/band_plan.h"

#include <array>

namespace logger {
namespace {

struct BandRange {
    qint64 lowerHz;
    qint64 upperHz;
    const char *name;
};

constexpr std::array<BandRange, 14> bandRanges{{
    {1'800'000, 2'000'000, "160m"},
    {3'500'000, 4'000'000, "80m"},
    {5'000'000, 5'500'000, "60m"},
    {7'000'000, 7'300'000, "40m"},
    {10'100'000, 10'150'000, "30m"},
    {14'000'000, 14'350'000, "20m"},
    {18'068'000, 18'168'000, "17m"},
    {21'000'000, 21'450'000, "15m"},
    {24'890'000, 24'990'000, "12m"},
    {28'000'000, 29'700'000, "10m"},
    {50'000'000, 54'000'000, "6m"},
    {70'000'000, 70'500'000, "4m"},
    {144'000'000, 148'000'000, "2m"},
    {420'000'000, 450'000'000, "70cm"},
}};

} // namespace

QString bandForFrequency(qint64 hz)
{
    for (const BandRange &range : bandRanges) {
        if (hz >= range.lowerHz && hz <= range.upperHz) {
            return QString::fromLatin1(range.name);
        }
    }
    return {};
}

} // namespace logger
