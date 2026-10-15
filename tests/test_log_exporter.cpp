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
