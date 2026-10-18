#include "logger/data/log_exporter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <utility>

namespace {

logger::Contest contest()
{
    logger::Contest value;
    value.id = 17;
    value.code = QStringLiteral("TEST-CONTEST");
    value.name = QStringLiteral("Test Contest");
    return value;
}

logger::ContestDefinition definition()
{
    logger::ContestDefinition value;
    value.id = QStringLiteral("test-contest");
    value.name = QStringLiteral("Test Contest");
    value.bands = {QStringLiteral("20m")};
    value.modes = {QStringLiteral("CW")};
    value.sentExchange = {
        {QStringLiteral("report"), QStringLiteral("RST"),
         logger::ExchangeFieldType::Rst, true, false, QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("Serial"),
         logger::ExchangeFieldType::Serial, true, true, {}, {}, 3}
    };
    value.receivedExchange = {
        {QStringLiteral("report"), QStringLiteral("RST"),
         logger::ExchangeFieldType::Rst},
        {QStringLiteral("serial"), QStringLiteral("Serial"),
         logger::ExchangeFieldType::Integer, true, false, {}, {}, 0, 1, 999}
    };
    return value;
}

logger::Qso qso()
{
    logger::Qso value;
    value.contestId = 17;
    value.callsign = QStringLiteral("W1AW");
    value.stationCallsign = QStringLiteral("K1TEST");
    value.band = QStringLiteral("20m");
    value.timestampUtc = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:00:00Z"), Qt::ISODate);
    value.frequencyHz = 14'025'000;
    value.mode = QStringLiteral("CW");
    value.sentExchange = QStringLiteral("599 001");
    value.receivedExchange = QStringLiteral("599 027");
    value.sentExchangeFields = {
        {QStringLiteral("report"), QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("001")}
    };
    value.receivedExchangeFields = {
        {QStringLiteral("report"), QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("027")}
    };
    value.points = 2;
    return value;
}

} // namespace

class LogExporterTest final : public QObject {
    Q_OBJECT

private slots:
    void writesValidatedCabrilloAtomically();
    void writesCabrilloCategoriesFromLogProperties();
    void writesCompleteCabrilloV3Header();
    void writesAdifWithByteLengths();
    void exportsLegacyExchangeStrings();
    void rejectsInvalidLogWithoutReplacingFile();
};

void LogExporterTest::writesValidatedCabrilloAtomically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("test.log"));
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    const logger::ContestDefinition contestDefinition = definition();
    QString error;
    QVERIFY2(logger::LogExporter::writeFile(
                 path, logger::LogExportFormat::Cabrillo, contest(),
                 &contestDefinition, {qso()}, station, &error),
             qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(contents.startsWith("START-OF-LOG: 3.0\n"));
    QVERIFY(contents.contains(
        "QSO: 14025 CW 2026-10-15 1000 K1TEST 599 001 W1AW 599 027\n"));
    QVERIFY(contents.endsWith("END-OF-LOG:\n"));
}

void LogExporterTest::writesCabrilloCategoriesFromLogProperties()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("categories.log"));
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    station.operatorName = QStringLiteral("Ignored Name");
    logger::Contest log = contest();
    log.properties = {
        {QStringLiteral("CATEGORY-OPERATOR"), QStringLiteral("SINGLE-OP")},
        {QStringLiteral("CATEGORY-POWER"), QStringLiteral("LOW")},
        {QStringLiteral("OPERATORS"), QStringLiteral("k1test w1aw")}
    };
    const logger::ContestDefinition contestDefinition = definition();
    QString error;
    QVERIFY2(logger::LogExporter::writeFile(
                 path, logger::LogExportFormat::Cabrillo, log,
                 &contestDefinition, {qso()}, station, &error),
             qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(contents.contains("CALLSIGN: K1TEST\n"));
    QVERIFY(contents.contains("OPERATORS: K1TEST W1AW\n"));
    QVERIFY(contents.contains("NAME: Ignored Name\n"));
    QVERIFY(!contents.contains("OPERATORS: Ignored Name"));
    QVERIFY(contents.contains("CATEGORY-OPERATOR: SINGLE-OP\n"));
    QVERIFY(contents.contains("CATEGORY-POWER: LOW\n"));
    QVERIFY(contents.contains("CATEGORY-BAND: 20M\n"));
    QVERIFY(contents.contains("CATEGORY-MODE: CW\n"));
}

void LogExporterTest::writesCompleteCabrilloV3Header()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("header.log"));
    logger::Station station;
    station.callsign = QStringLiteral("SN6U");
    station.operatorName = QStringLiteral("Jan Kowalski");
    station.grid = QStringLiteral("jo81");
    station.powerClass = QStringLiteral("low");
    station.email = QStringLiteral("sn6u@example.com");
    station.address = QStringLiteral("ul. Radiowa 1\nskr. 7");
    station.city = QStringLiteral("Wroclaw");
    station.postalCode = QStringLiteral("50-001");
    station.country = QStringLiteral("Poland");
    logger::Contest log = contest();
    log.properties = {
        {QStringLiteral("LOCATION"), QStringLiteral("DX")},
        {QStringLiteral("CLUB"), QStringLiteral("SP DX Club")},
        {QStringLiteral("SOAPBOX"), QStringLiteral("Great conditions")}
    };
    logger::ContestDefinition contestDefinition = definition();
    contestDefinition.cabrilloContest = QStringLiteral("CQ-WPX-CW");
    contestDefinition.bands.append(QStringLiteral("160m"));
    logger::Qso later = qso();
    later.id = 2;
    later.callsign = QStringLiteral("K3LR");
    later.band = QStringLiteral("160m");
    later.frequencyHz = 0;
    later.timestampUtc = later.timestampUtc.addSecs(60);
    later.sentExchangeFields[QStringLiteral("serial")] = QStringLiteral("002");
    logger::Qso earlier = qso();
    earlier.id = 1;
    logger::LogExportOptions options;
    options.claimedScore = 1234;
    options.createdBy = QStringLiteral("Contest Logger 9.9");
    QString error;
    QVERIFY2(logger::LogExporter::writeFile(
                 path, logger::LogExportFormat::Cabrillo, log, &contestDefinition,
                 {later, earlier}, station, options, &error),
             qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(file.readAll());
    const QStringList expectedHeader{
        QStringLiteral("START-OF-LOG: 3.0"),
        QStringLiteral("CREATED-BY: Contest Logger 9.9"),
        QStringLiteral("CONTEST: CQ-WPX-CW"),
        QStringLiteral("CALLSIGN: SN6U"),
        QStringLiteral("LOCATION: DX"),
        QStringLiteral("CATEGORY-OPERATOR: SINGLE-OP"),
        QStringLiteral("CATEGORY-ASSISTED: NON-ASSISTED"),
        QStringLiteral("CATEGORY-BAND: ALL"),
        QStringLiteral("CATEGORY-MODE: CW"),
        QStringLiteral("CATEGORY-POWER: LOW"),
        QStringLiteral("CATEGORY-TRANSMITTER: ONE"),
        QStringLiteral("GRID-LOCATOR: JO81"),
        QStringLiteral("CLAIMED-SCORE: 1234"),
        QStringLiteral("CLUB: SP DX Club"),
        QStringLiteral("NAME: Jan Kowalski"),
        QStringLiteral("EMAIL: sn6u@example.com"),
        QStringLiteral("ADDRESS: ul. Radiowa 1"),
        QStringLiteral("ADDRESS: skr. 7"),
        QStringLiteral("ADDRESS-CITY: Wroclaw"),
        QStringLiteral("ADDRESS-POSTALCODE: 50-001"),
        QStringLiteral("ADDRESS-COUNTRY: Poland"),
        QStringLiteral("OPERATORS: SN6U"),
        QStringLiteral("SOAPBOX: Great conditions"),
        QStringLiteral("QSO: 14025 CW 2026-10-15 1000 SN6U 599 001 W1AW 599 027"),
        QStringLiteral("QSO: 1800 CW 2026-10-15 1001 SN6U 599 002 K3LR 599 027"),
        QStringLiteral("END-OF-LOG:")};
    QCOMPARE(contents.split(QLatin1Char('\n'), Qt::SkipEmptyParts), expectedHeader);
}

void LogExporterTest::writesAdifWithByteLengths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("test.adi"));
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    const logger::ContestDefinition contestDefinition = definition();
    QString error;
    QVERIFY2(logger::LogExporter::writeFile(
                 path, logger::LogExportFormat::Adif, contest(),
                 &contestDefinition, {qso()}, station, &error),
             qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(contents.contains("<EOH>"));
    QVERIFY(contents.contains("<CALL:4>W1AW"));
    QVERIFY(contents.contains("<QSO_DATE:8>20261015"));
    QVERIFY(contents.contains("<FREQ:9>14.025000"));
    QVERIFY(contents.contains("<STX_STRING:7>599 001"));
    QVERIFY(contents.contains("<SRX_STRING:7>599 027"));
    QVERIFY(contents.contains("<EOR>"));
}

void LogExporterTest::exportsLegacyExchangeStrings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    const logger::ContestDefinition contestDefinition = definition();
    logger::Qso legacy = qso();
    legacy.sentExchangeFields.clear();
    legacy.receivedExchangeFields.clear();

    for (const auto &exchange : {
             std::pair{QStringLiteral("599 001"), QStringLiteral("599 027")},
             std::pair{QStringLiteral("report=599 serial=001"),
                       QStringLiteral("report=599 serial=027")}}) {
        legacy.sentExchange = exchange.first;
        legacy.receivedExchange = exchange.second;
        const QString path = directory.filePath(
            QStringLiteral("legacy-%1.log").arg(exchange.first.contains('=') ? 1 : 0));
        QString error;
        QVERIFY2(logger::LogExporter::writeFile(
                     path, logger::LogExportFormat::Cabrillo, contest(),
                     &contestDefinition, {legacy}, station, &error),
                 qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains(
            "QSO: 14025 CW 2026-10-15 1000 K1TEST 599 001 W1AW 599 027\n"));
    }
}

void LogExporterTest::rejectsInvalidLogWithoutReplacingFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.log"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("existing"), qint64(8));
    existing.close();

    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    const logger::ContestDefinition contestDefinition = definition();
    logger::Qso invalid = qso();
    invalid.mode = QStringLiteral("INVALID");
    QString error;
    QVERIFY(!logger::LogExporter::writeFile(
        path, logger::LogExportFormat::Cabrillo, contest(), &contestDefinition,
        {invalid}, station, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("existing"));
}

QTEST_GUILESS_MAIN(LogExporterTest)
#include "test_log_exporter.moc"
