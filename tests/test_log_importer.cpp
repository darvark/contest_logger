#include "logger/data/log_importer.h"
#include "logger/data/log_exporter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace logger;

namespace {

ContestDefinition definition()
{
    ContestDefinition value;
    value.id = QStringLiteral("test");
    value.name = QStringLiteral("Test");
    value.bands = {QStringLiteral("20m"), QStringLiteral("40m")};
    value.modes = {QStringLiteral("CW"), QStringLiteral("SSB")};
    value.sentExchange = {
        {QStringLiteral("report"), QStringLiteral("RST"), ExchangeFieldType::Rst,
         true, false, QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("Serial"), ExchangeFieldType::Serial,
         true, true, {}, {}, 3}
    };
    value.receivedExchange = {
        {QStringLiteral("report"), QStringLiteral("RST"), ExchangeFieldType::Rst},
        {QStringLiteral("serial"), QStringLiteral("Serial"), ExchangeFieldType::Integer,
         true, false, {}, {}, 0, 1, 9999}
    };
    value.qsoPoints = 2;
    return value;
}

Qso qso()
{
    Qso value;
    value.contestId = 1;
    value.callsign = QStringLiteral("W1AW");
    value.stationCallsign = QStringLiteral("SP9TEST");
    value.timestampUtc = QDateTime::fromString(QStringLiteral("2026-10-20T10:11:00Z"), Qt::ISODate);
    value.band = QStringLiteral("20m");
    value.frequencyHz = 14'025'000;
    value.mode = QStringLiteral("CW");
    value.sentExchangeFields = {{QStringLiteral("report"), QStringLiteral("599")},
                               {QStringLiteral("serial"), QStringLiteral("007")}};
    value.receivedExchangeFields = {{QStringLiteral("report"), QStringLiteral("599")},
                                   {QStringLiteral("serial"), QStringLiteral("012")}};
    value.points = 2;
    return value;
}

QByteArray adif()
{
    return QByteArray("<ADIF_VER:5>3.1.4<EOH>\n"
        "<QSO_DATE:8>20261020<TIME_ON:4>1011<CALL:4>W1AW"
        "<BAND:3>20m<MODE:2>CW<FREQ:6>14.025"
        "<RST_SENT:3>599<STX:1>7<RST_RCVD:3>599<SRX:2>12<EOR>");
}

bool write(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

} // namespace

class LogImporterTest final : public QObject {
    Q_OBJECT
private slots:
    void roundTripsExportedLogs_data();
    void roundTripsExportedLogs();
    void readsStandardAdifAndPreservesByteLengths();
    void readsCabrilloWithTransmitterAndVoiceMode();
    void readsCabrilloWithoutRstFields();
    void skipsExactDuplicatesButRetainsContestDupes();
    void rejectsMalformedOrIncompatibleLogs_data();
    void rejectsMalformedOrIncompatibleLogs();
};

void LogImporterTest::roundTripsExportedLogs_data()
{
    QTest::addColumn<bool>("cabrillo");
    QTest::newRow("adif") << false;
    QTest::newRow("cabrillo") << true;
}

void LogImporterTest::roundTripsExportedLogs()
{
    QFETCH(bool, cabrillo);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("log.txt"));
    Contest contest;
    contest.id = 1;
    contest.code = QStringLiteral("TEST");
    contest.name = QStringLiteral("Test");
    Station station;
    station.callsign = QStringLiteral("SP9TEST");
    const auto rules = definition();
    QString error;
    QVERIFY2(LogExporter::writeFile(path,
        cabrillo ? LogExportFormat::Cabrillo : LogExportFormat::Adif,
        contest, &rules, {qso()}, station, &error), qPrintable(error));
    ImportedLog parsed;
    QVERIFY2(LogImporter::readFile(path, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed.records.size(), 1);
    LogImportPreview preview;
    QVERIFY2(LogImporter::prepare(parsed, rules, station.callsign, {}, {},
                                  &preview, &error), qPrintable(error));
    QCOMPARE(preview.qsos.size(), 1);
    const Qso actual = preview.qsos.first();
    QCOMPARE(actual.callsign, qso().callsign);
    QCOMPARE(actual.timestampUtc, qso().timestampUtc);
    QCOMPARE(actual.frequencyHz, qso().frequencyHz);
    QCOMPARE(actual.sentExchangeFields, qso().sentExchangeFields);
    QCOMPARE(actual.receivedExchangeFields, qso().receivedExchangeFields);
    QCOMPARE(actual.points, 2);
}

void LogImporterTest::readsStandardAdifAndPreservesByteLengths()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("log.adi"));
    QByteArray contents = adif();
    contents.replace("<MODE:2>CW", "<MODE:2>CW<OPERATOR:5>K1ABC");
    const QByteArray comment = QString::fromUtf8("Zażółć <EOR>").toUtf8();
    contents.replace("<BAND:3>20m", "<COMMENT:" + QByteArray::number(comment.size())
                     + ">" + comment + "<BAND:3>20m");
    // STX_STRING/SRX_STRING commonly omit the separately supplied RST.
    contents.replace("<STX:1>7", "<STX:1>7<STX_STRING:3>007");
    contents.replace("<SRX:2>12", "<SRX:2>12<SRX_STRING:3>012");
    QVERIFY(write(path, contents.toLower()));
    ImportedLog parsed;
    QString error;
    QVERIFY2(LogImporter::readFile(path, &parsed, &error), qPrintable(error));
    LogImportPreview preview;
    QVERIFY2(LogImporter::prepare(parsed, definition(), QStringLiteral("SP9TEST"), {}, {},
                                  &preview, &error), qPrintable(error));
    QCOMPARE(preview.qsos.first().timestampUtc, qso().timestampUtc);
    QCOMPARE(preview.qsos.first().sentExchangeFields, qso().sentExchangeFields);
    QCOMPARE(preview.qsos.first().receivedExchangeFields, qso().receivedExchangeFields);
}

void LogImporterTest::readsCabrilloWithTransmitterAndVoiceMode()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("log.cbr"));
    QVERIFY(write(path, QByteArray("START-OF-LOG: 3.0\r\nCALLSIGN: SP9TEST\r\n"
        "CONTEST: TEST\r\nQSO: 14025 PH 2026-10-20 1011 SP9TEST 59 007 W1AW 59 012 0\r\n"
        "END-OF-LOG:\r\n")));
    ImportedLog parsed;
    QString error;
    QVERIFY2(LogImporter::readFile(path, &parsed, &error), qPrintable(error));
    LogImportPreview preview;
    QVERIFY2(LogImporter::prepare(parsed, definition(), QStringLiteral("SP9TEST"), {}, {},
                                  &preview, &error), qPrintable(error));
    QCOMPARE(preview.qsos.first().mode, QStringLiteral("SSB"));
    QCOMPARE(preview.qsos.first().receivedExchangeFields.value(QStringLiteral("serial")),
             QStringLiteral("012"));
}

void LogImporterTest::skipsExactDuplicatesButRetainsContestDupes()
{
    ImportedLog parsed;
    ImportedQso record;
    record.record = 1;
    record.qso = qso();
    record.fields = {
        {QStringLiteral("STX_STRING"), QStringLiteral("599 007")},
        {QStringLiteral("SRX_STRING"), QStringLiteral("599 012")}
    };
    parsed.records = {record, record};
    record.qso.timestampUtc = record.qso.timestampUtc.addSecs(60);
    parsed.records.append(record);
    LogImportPreview preview;
    QString error;
    QVERIFY2(LogImporter::prepare(parsed, definition(), QStringLiteral("SP9TEST"), {}, {},
                                  &preview, &error), qPrintable(error));
    QCOMPARE(preview.qsos.size(), 2);
    QCOMPARE(preview.skipped, 1);
    QCOMPARE(preview.qsos[0].points, 2);
    QCOMPARE(preview.qsos[1].points, 0);
    QVERIFY2(LogImporter::prepare(parsed, definition(), QStringLiteral("SP9TEST"),
                                  {qso()}, {}, &preview, &error), qPrintable(error));
    QCOMPARE(preview.qsos.size(), 1);
    QCOMPARE(preview.skipped, 2);
}

void LogImporterTest::readsCabrilloWithoutRstFields()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("log.cbr"));
    QVERIFY(write(path, QByteArray::fromHex("efbbbf")
        + QByteArray("START-OF-LOG: 3.0\nCALLSIGN: SP9TEST\n"
            "QSO: 7025 CW 2026-10-20 1011 SP9TEST 599 007 W1AW 599 1\n"
            "QSO: 7025 CW 2026-10-20 1012 SP9TEST 599 008 W2XYZ 599 2 0\n"
            "END-OF-LOG:\n")));
    ImportedLog parsed;
    QString error;
    QVERIFY2(LogImporter::readFile(path, &parsed, &error), qPrintable(error));
    auto rules = definition();
    rules.sentExchange.removeFirst();
    rules.receivedExchange.removeFirst();
    LogImportPreview preview;
    QVERIFY2(LogImporter::prepare(parsed, rules, QStringLiteral("SP9TEST"), {}, {},
                                  &preview, &error), qPrintable(error));
    QCOMPARE(preview.qsos.size(), 2);
    QCOMPARE(preview.qsos.first().receivedExchangeFields.value(QStringLiteral("serial")),
             QStringLiteral("1"));
    QCOMPARE(preview.qsos.last().receivedExchangeFields.value(QStringLiteral("serial")),
             QStringLiteral("2"));
}

void LogImporterTest::rejectsMalformedOrIncompatibleLogs_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<bool>("parseFailure");
    QTest::newRow("truncated-length") << QByteArray("<CALL:999>W1AW<EOR>") << true;
    QTest::newRow("missing-eor") << QByteArray("<CALL:4>W1AW") << true;
    QTest::newRow("empty") << QByteArray("not a log") << true;
    QTest::newRow("repeated-tag") << QByteArray("<CALL:4>W1AW<CALL:4>W1AW<EOR>") << true;
    QTest::newRow("invalid-date") << QByteArray(adif()).replace("20261020", "20261320") << false;
    QTest::newRow("wrong-band") << QByteArray(adif()).replace("<BAND:3>20m", "<BAND:3>40m") << true;
    QTest::newRow("wrong-mode") << QByteArray(adif()).replace("<MODE:2>CW", "<MODE:2>FM") << false;
    QTest::newRow("invalid-exchange") << QByteArray(adif()).replace("<SRX:2>12", "<SRX:2>XX") << false;
    QTest::newRow("wrong-station") << (adif() + QByteArray("<QSO_DATE:8>20261020"
        "<TIME_ON:4>1012<CALL:4>W1AW<BAND:3>20m<MODE:2>CW<STATION_CALLSIGN:5>K1ABC<EOR>")) << false;
    QTest::newRow("missing-serial") << QByteArray(adif()).replace("<STX:1>7", "") << false;
}

void LogImporterTest::rejectsMalformedOrIncompatibleLogs()
{
    QFETCH(QByteArray, contents);
    QFETCH(bool, parseFailure);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("log.txt"));
    QVERIFY(write(path, contents));
    ImportedLog parsed;
    QString error;
    const bool read = LogImporter::readFile(path, &parsed, &error);
    if (parseFailure) {
        QVERIFY(!read);
    } else {
        QVERIFY2(read, qPrintable(error));
        LogImportPreview preview;
        QVERIFY(!LogImporter::prepare(parsed, definition(), QStringLiteral("SP9TEST"),
                                      {}, {}, &preview, &error));
        QVERIFY(preview.qsos.isEmpty());
    }
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(LogImporterTest)
#include "test_log_importer.moc"
