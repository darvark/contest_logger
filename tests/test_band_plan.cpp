#include "logger/radio/band_plan.h"

#include <QtTest>

class BandPlanTest final : public QObject {
    Q_OBJECT

private slots:
    void returnsBandAtBothEdges();
    void returnsEmptyOutsideBands();
};

void BandPlanTest::returnsBandAtBothEdges()
{
    const QList<QPair<qint64, QString>> edges{
        {1'800'000, QStringLiteral("160m")}, {2'000'000, QStringLiteral("160m")},
        {3'500'000, QStringLiteral("80m")}, {4'000'000, QStringLiteral("80m")},
        {5'000'000, QStringLiteral("60m")}, {5'500'000, QStringLiteral("60m")},
        {7'000'000, QStringLiteral("40m")}, {7'300'000, QStringLiteral("40m")},
        {10'100'000, QStringLiteral("30m")}, {10'150'000, QStringLiteral("30m")},
        {14'000'000, QStringLiteral("20m")}, {14'350'000, QStringLiteral("20m")},
        {18'068'000, QStringLiteral("17m")}, {18'168'000, QStringLiteral("17m")},
        {21'000'000, QStringLiteral("15m")}, {21'450'000, QStringLiteral("15m")},
        {24'890'000, QStringLiteral("12m")}, {24'990'000, QStringLiteral("12m")},
        {28'000'000, QStringLiteral("10m")}, {29'700'000, QStringLiteral("10m")},
        {50'000'000, QStringLiteral("6m")}, {54'000'000, QStringLiteral("6m")},
        {70'000'000, QStringLiteral("4m")}, {70'500'000, QStringLiteral("4m")},
        {144'000'000, QStringLiteral("2m")}, {148'000'000, QStringLiteral("2m")},
        {420'000'000, QStringLiteral("70cm")}, {450'000'000, QStringLiteral("70cm")},
    };
    for (const auto &[frequency, expected] : edges) {
        QCOMPARE(logger::bandForFrequency(frequency), expected);
    }
}

void BandPlanTest::returnsEmptyOutsideBands()
{
    QVERIFY(logger::bandForFrequency(0).isEmpty());
    QVERIFY(logger::bandForFrequency(1'799'999).isEmpty());
    QVERIFY(logger::bandForFrequency(2'000'001).isEmpty());
    QVERIFY(logger::bandForFrequency(100'000'000).isEmpty());
    QVERIFY(logger::bandForFrequency(450'000'001).isEmpty());
}

QTEST_GUILESS_MAIN(BandPlanTest)
#include "test_band_plan.moc"
