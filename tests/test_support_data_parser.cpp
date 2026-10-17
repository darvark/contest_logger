#include "logger/data/support_data_parser.h"

#include <QtTest>

class SupportDataParserTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesHeaderedCallHistory();
    void parsesHeaderlessCallHistory();
    void parsesMasterCpyAndDeduplicates();
    void parsesMultilineCtyDat();
    void rejectsMalformedSupportFiles();
    void resolvesCtyMultiplierValues();
    void parsesCtyOverridesAndBearing();
};

void SupportDataParserTest::parsesHeaderedCallHistory()
{
    QHash<QString, logger::CallHistoryEntry> entries;
    QString error;
    QVERIFY2(logger::SupportDataParser::parseCallHistory(
                 QStringLiteral("CALL,NAME,QTH,STATE,GRID\n"
                                "w1aw,\"ARRL HQ\",Newington,CT,FN31\n"),
                 entries, &error),
             qPrintable(error));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.value(QStringLiteral("W1AW")).fields.value(QStringLiteral("name")),
             QStringLiteral("ARRL HQ"));
    QCOMPARE(entries.value(QStringLiteral("W1AW")).fields.value(QStringLiteral("grid")),
             QStringLiteral("FN31"));
}

void SupportDataParserTest::parsesHeaderlessCallHistory()
{
    QHash<QString, logger::CallHistoryEntry> entries;
    QString error;
    QVERIFY2(logger::SupportDataParser::parseCallHistory(
                 QStringLiteral("K1ABC Alice Boston 599 MA\n"),
                 entries, &error),
             qPrintable(error));
    QCOMPARE(entries.value(QStringLiteral("K1ABC")).fields.value(QStringLiteral("name")),
             QStringLiteral("Alice"));
    QCOMPARE(entries.value(QStringLiteral("K1ABC")).fields.value(QStringLiteral("qth")),
             QStringLiteral("Boston"));
    QCOMPARE(entries.value(QStringLiteral("K1ABC")).fields.value(QStringLiteral("exchange")),
             QStringLiteral("599"));
}

void SupportDataParserTest::parsesMasterCpyAndDeduplicates()
{
    QStringList callsigns;
    QString error;
    QVERIFY2(logger::SupportDataParser::parseMasterCpy(
                 QStringLiteral("!!Order,1,1\n# Super Check Partial\n"
                                "# master list\nK1ABC 123\nW1AW\nK1ABC\n"),
                 callsigns, &error),
             qPrintable(error));
    QCOMPARE(callsigns, QStringList({QStringLiteral("K1ABC"),
                                     QStringLiteral("W1AW")}));
}

void SupportDataParserTest::parsesMultilineCtyDat()
{
    QVector<logger::CtyPrefixEntry> entries;
    QString error;
    QVERIFY2(logger::SupportDataParser::parseCtyDat(
                 QStringLiteral("United States: 5: 8: NA: 37: 95: 5: 8: "
                                "K,KA,KB,\nKC,KD,=K1ABC;\n"),
                 entries, &error),
             qPrintable(error));
    QCOMPARE(entries.size(), 6);
    QCOMPARE(entries.first().prefix, QStringLiteral("K"));
    QCOMPARE(entries.first().entity, QStringLiteral("United States"));
    QCOMPARE(entries.first().cqZone, 5);
    QCOMPARE(entries.first().ituZone, 8);
    QCOMPARE(entries.first().continent, QStringLiteral("NA"));
    QVERIFY(entries.last().exactCallsign);
    QCOMPARE(entries.last().prefix, QStringLiteral("K1ABC"));
}

void SupportDataParserTest::rejectsMalformedSupportFiles()
{
    QHash<QString, logger::CallHistoryEntry> history;
    QStringList calls;
    QVector<logger::CtyPrefixEntry> prefixes;
    QString error;
    QVERIFY(!logger::SupportDataParser::parseCallHistory(
        QStringLiteral("not-a-call\n"), history, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!logger::SupportDataParser::parseMasterCpy(
        QStringLiteral("invalid!\n"), calls, &error));
    QVERIFY(!logger::SupportDataParser::parseCtyDat(
        QStringLiteral("Broken entity record;\n"), prefixes, &error));
}

void SupportDataParserTest::resolvesCtyMultiplierValues()
{
    QVector<logger::CtyPrefixEntry> entries;
    QString error;
    QVERIFY2(logger::SupportDataParser::parseCtyDat(
                 QStringLiteral("United States: 5: 8: NA: 37: 95: 5: 8: K,=K1ABC;\n"
                                "Germany: 14: 28: EU: 230: 75: 14: 28: DL;"),
                 entries, &error), qPrintable(error));
    const auto us = logger::SupportDataParser::resolveCtyMultipliers(
        QStringLiteral("K1ABC/P"), entries);
    QCOMPARE(us.value(QStringLiteral("dxcc")), QStringLiteral("United States"));
    QCOMPARE(us.value(QStringLiteral("primary_prefix")), QStringLiteral("K1ABC"));
    QCOMPARE(us.value(QStringLiteral("cq_zone")), QStringLiteral("5"));
    QCOMPARE(us.value(QStringLiteral("itu_zone")), QStringLiteral("8"));
    QCOMPARE(us.value(QStringLiteral("continent")), QStringLiteral("NA"));
    QCOMPARE(logger::SupportDataParser::resolveCtyMultipliers(
                 QStringLiteral("DL1ABC"), entries).value(QStringLiteral("dxcc")),
             QStringLiteral("Germany"));
}

void SupportDataParserTest::parsesCtyOverridesAndBearing()
{
    QVector<logger::CtyPrefixEntry> entries;
    QString error;
    QVERIFY2(logger::SupportDataParser::parseCtyDat(
                 QStringLiteral("Poland: 15: 28: EU: 52.28: -18.67: -1.0: SP:\n"
                                "    3Z,SP,SQ(16)[29],=SP9ABC/P;\n"),
                 entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 4);
    const auto *sq = logger::SupportDataParser::findCtyEntry(
        QStringLiteral("SQ1X"), entries);
    QVERIFY(sq != nullptr);
    QCOMPARE(sq->cqZone, 16);
    QCOMPARE(sq->ituZone, 29);
    QCOMPARE(sq->longitude, 18.67);
    QVERIFY(logger::SupportDataParser::findCtyEntry(
                QStringLiteral("SP9ABC/P"), entries) != nullptr);

    QVERIFY2(logger::SupportDataParser::parseCtyDat(
                 QStringLiteral("# BY0: BY0A(23)[42],\n         BY0B(23)[42];\n"
                                "Poland: 15: 28: EU: 52.28: -18.67: -1.0: SP:\n    SP;\n"),
                 entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 1);

    double lat = 0;
    double lon = 0;
    QVERIFY(logger::SupportDataParser::maidenheadToLatLon(
        QStringLiteral("JO91"), lat, lon));
    const auto east = logger::SupportDataParser::bearingBetween(0, 0, 0, 10);
    QVERIFY(qAbs(east.azimuthDegrees - 90.0) < 0.01);
    QVERIFY(!logger::SupportDataParser::maidenheadToLatLon(
        QStringLiteral("ZZ99"), lat, lon));
}

QTEST_GUILESS_MAIN(SupportDataParserTest)
#include "test_support_data_parser.moc"
