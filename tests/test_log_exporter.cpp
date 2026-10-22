#include "logger/data/log_exporter.h"
#include "logger/contest/contest_definition_loader.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
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
    void writesArrlSweepstakesCabrilloWithoutRst();
    void exportsBundledProfileCabrilloExchanges();
    void exportsWaeQtcImmediatelyAfterPartnerQso();
    void writesAdifWithByteLengths();
    void exportsLegacyExchangeStrings();
    void rejectsInvalidLogWithoutReplacingFile();
    void omitsInvalidQsosFromExport();
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

void LogExporterTest::writesArrlSweepstakesCabrilloWithoutRst()
{
    logger::ContestDefinition contestDefinition;
    QString error;
    const QString yamlPath = QStringLiteral(
        CONTEST_LOGGER_TEST_SOURCE_DIR "/contests/examples/arrl-ss-cw.yaml");
    QVERIFY2(logger::ContestDefinitionLoader().loadFile(
                 yamlPath, contestDefinition, &error),
             qPrintable(error));

    logger::Contest sweepstakes = contest();
    sweepstakes.code = contestDefinition.id;
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    logger::Qso contact = qso();
    contact.sentExchangeFields = {
        {QStringLiteral("serial"), QStringLiteral("001")},
        {QStringLiteral("precedence"), QStringLiteral("A")},
        {QStringLiteral("check"), QStringLiteral("79")},
        {QStringLiteral("section"), QStringLiteral("CT")}
    };
    contact.receivedExchangeFields = {
        {QStringLiteral("serial"), QStringLiteral("027")},
        {QStringLiteral("precedence"), QStringLiteral("B")},
        {QStringLiteral("check"), QStringLiteral("72")},
        {QStringLiteral("section"), QStringLiteral("EMA")}
    };
    contact.sentExchange.clear();
    contact.receivedExchange.clear();
    contact.points = 2;

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sweepstakes.log"));
    QVERIFY2(logger::LogExporter::writeFile(
                 path, logger::LogExportFormat::Cabrillo, sweepstakes,
                 &contestDefinition, {contact}, station, &error),
             qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(contents.contains("CONTEST: ARRL-SS-CW\n"));
    QVERIFY(contents.contains(
        "QSO: 14025 CW 2026-10-15 1000 K1TEST 001 A 79 CT W1AW 027 B 72 EMA\n"));
    QVERIFY(!contents.contains("K1TEST 599 001"));
}

void LogExporterTest::exportsBundledProfileCabrilloExchanges()
{
    struct Scenario {
        QString file;
        QString band;
        QString mode;
        QMap<QString, QString> sent;
        QMap<QString, QString> received;
        QString sentCabrillo;
        QString receivedCabrillo;
    };
    const QVector<Scenario> scenarios{
        {QStringLiteral("arrl-dx-10m.yaml"), QStringLiteral("10m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("exchange"), QStringLiteral("CT")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("exchange"), QStringLiteral("CA")}},
         QStringLiteral("CT"), QStringLiteral("CA")},
        {QStringLiteral("arrl-dx-cw.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("exchange"), QStringLiteral("100")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("exchange"), QStringLiteral("CT")}},
         QStringLiteral("100"), QStringLiteral("CT")},
        {QStringLiteral("arrl-dx-ssb.yaml"), QStringLiteral("20m"),
         QStringLiteral("SSB"),
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("exchange"), QStringLiteral("100")}},
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("exchange"), QStringLiteral("CT")}},
         QStringLiteral("100"), QStringLiteral("CT")},
        {QStringLiteral("arrl-dx-cw-w-ve.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("state_province"), QStringLiteral("CT")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("power"), QStringLiteral("100")}},
         QStringLiteral("CT"), QStringLiteral("100")},
        {QStringLiteral("arrl-dx-ssb-w-ve.yaml"), QStringLiteral("20m"),
         QStringLiteral("SSB"),
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("state_province"), QStringLiteral("CT")}},
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("power"), QStringLiteral("100")}},
         QStringLiteral("CT"), QStringLiteral("100")},
        {QStringLiteral("arrl-ss-cw.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("serial"), QStringLiteral("001")},
          {QStringLiteral("precedence"), QStringLiteral("A")},
          {QStringLiteral("check"), QStringLiteral("72")},
          {QStringLiteral("section"), QStringLiteral("CT")}},
         {{QStringLiteral("serial"), QStringLiteral("027")},
          {QStringLiteral("precedence"), QStringLiteral("B")},
          {QStringLiteral("check"), QStringLiteral("79")},
          {QStringLiteral("section"), QStringLiteral("EMA")}},
         QStringLiteral("001 A 72 CT"), QStringLiteral("027 B 79 EMA")},
        {QStringLiteral("arrl-ss-ssb.yaml"), QStringLiteral("20m"),
         QStringLiteral("SSB"),
         {{QStringLiteral("serial"), QStringLiteral("001")},
          {QStringLiteral("precedence"), QStringLiteral("A")},
          {QStringLiteral("check"), QStringLiteral("72")},
          {QStringLiteral("section"), QStringLiteral("CT")}},
         {{QStringLiteral("serial"), QStringLiteral("027")},
          {QStringLiteral("precedence"), QStringLiteral("B")},
          {QStringLiteral("check"), QStringLiteral("79")},
          {QStringLiteral("section"), QStringLiteral("EMA")}},
         QStringLiteral("001 A 72 CT"), QStringLiteral("027 B 79 EMA")},
        {QStringLiteral("cqww-cw.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("zone"), QStringLiteral("05")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("zone"), QStringLiteral("04")}},
         QStringLiteral("05"), QStringLiteral("04")},
        {QStringLiteral("cqww-ssb.yaml"), QStringLiteral("20m"),
         QStringLiteral("SSB"),
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("zone"), QStringLiteral("05")}},
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("zone"), QStringLiteral("04")}},
         QStringLiteral("05"), QStringLiteral("04")},
        {QStringLiteral("cqwpx-cw.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("001"), QStringLiteral("027")},
        {QStringLiteral("cqwpx-ssb.yaml"), QStringLiteral("20m"),
         QStringLiteral("SSB"),
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("report"), QStringLiteral("59")},
          {QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("001"), QStringLiteral("027")},
        {QStringLiteral("eudx.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("region"), QStringLiteral("EM10")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("region"), QStringLiteral("EM20")}},
         QStringLiteral("EM10"), QStringLiteral("EM20")},
        {QStringLiteral("euhfc.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("year"), QStringLiteral("82")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("year"), QStringLiteral("75")}},
         QStringLiteral("82"), QStringLiteral("75")},
        {QStringLiteral("iaruhfc.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("itu_zone"), QStringLiteral("08")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("itu_zone"), QStringLiteral("05")}},
         QStringLiteral("08"), QStringLiteral("05")},
        {QStringLiteral("spdx.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("province"), QStringLiteral("M")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("M"), QStringLiteral("027")},
        {QStringLiteral("spdx-dx.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("province"), QStringLiteral("M")}},
         QStringLiteral("001"), QStringLiteral("M")},
        {QStringLiteral("wag.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("exchange"), QStringLiteral("F11")}},
         QStringLiteral("001"), QStringLiteral("F11")},
        {QStringLiteral("general-logging.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")}},
         {{QStringLiteral("report"), QStringLiteral("599")}},
         QString(), QString()},
        {QStringLiteral("grid-field-day.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("grid"), QStringLiteral("FN20")}},
         QString(), QStringLiteral("FN20")},
        {QStringLiteral("serial-sprint.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("report"), QStringLiteral("599")},
          {QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("001"), QStringLiteral("027")},
        {QStringLiteral("wae-cw.yaml"), QStringLiteral("20m"),
         QStringLiteral("CW"),
         {{QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("001"), QStringLiteral("027")},
        {QStringLiteral("wae-ssb.yaml"), QStringLiteral("20m"),
         QStringLiteral("SSB"),
         {{QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("001"), QStringLiteral("027")},
        {QStringLiteral("wae-rtty.yaml"), QStringLiteral("20m"),
         QStringLiteral("RTTY"),
         {{QStringLiteral("serial"), QStringLiteral("001")}},
         {{QStringLiteral("serial"), QStringLiteral("027")}},
         QStringLiteral("001"), QStringLiteral("027")}
    };
    QCOMPARE(scenarios.size(), 23);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    const auto expectedFrequency = [](const QString &band) {
        return band == QStringLiteral("10m") ? QStringLiteral("28025")
                                              : QStringLiteral("14025");
    };
    for (qsizetype index = 0; index < scenarios.size(); ++index) {
        const Scenario &scenario = scenarios.at(index);
        logger::ContestDefinition contestDefinition;
        QString error;
        const QString definitionPath = QStringLiteral(
            CONTEST_LOGGER_TEST_SOURCE_DIR "/contests/examples/%1")
                                           .arg(scenario.file);
        QVERIFY2(logger::ContestDefinitionLoader().loadFile(
                     definitionPath, contestDefinition, &error),
                 qPrintable(error));

        logger::Qso record = qso();
        record.band = scenario.band;
        record.mode = scenario.mode;
        record.frequencyHz = scenario.band == QStringLiteral("10m")
            ? 28'025'000 : 14'025'000;
        record.sentExchangeFields = scenario.sent;
        record.receivedExchangeFields = scenario.received;
        const auto legacyExchange = [](
            const QVector<logger::ExchangeFieldDefinition> &fields,
            const QMap<QString, QString> &values) {
            QStringList parts;
            for (const logger::ExchangeFieldDefinition &field : fields) {
                const QString value = values.value(field.name).trimmed();
                if (!value.isEmpty()) {
                    parts.append(value);
                }

            }
            return parts.join(QLatin1Char(' '));
        };
        record.sentExchange = legacyExchange(contestDefinition.sentExchange,
                                             scenario.sent);
        record.receivedExchange = legacyExchange(
            contestDefinition.receivedExchange, scenario.received);
        const QString outputPath = directory.filePath(
            QStringLiteral("profile-%1.log").arg(index));
        QVERIFY2(logger::LogExporter::writeFile(
                     outputPath, logger::LogExportFormat::Cabrillo, contest(),
                     &contestDefinition, {record}, station, &error),
                 qPrintable((scenario.file + QStringLiteral(": ") + error)));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        const QString mode = scenario.mode == QStringLiteral("SSB")
            ? QStringLiteral("PH")
            : scenario.mode == QStringLiteral("RTTY")
                ? QStringLiteral("RY") : scenario.mode;
        QStringList expectedFields{
            QStringLiteral("QSO:"), expectedFrequency(scenario.band), mode,
            QStringLiteral("2026-10-15"), QStringLiteral("1000"),
            QStringLiteral("K1TEST")
        };
        const auto hasRst = [](const QVector<logger::ExchangeFieldDefinition> &fields) {
            return std::any_of(
                fields.cbegin(), fields.cend(),
                [](const logger::ExchangeFieldDefinition &field) {
                    return field.type == logger::ExchangeFieldType::Rst;
                });
        };
        if (hasRst(contestDefinition.sentExchange)) {
            expectedFields.append(scenario.sent.value(QStringLiteral("report")));
        }
        if (!scenario.sentCabrillo.isEmpty()) {
            expectedFields.append(scenario.sentCabrillo);
        }
        expectedFields.append(QStringLiteral("W1AW"));
        if (hasRst(contestDefinition.receivedExchange)) {
            expectedFields.append(scenario.received.value(QStringLiteral("report")));
        }
        if (!scenario.receivedCabrillo.isEmpty()) {
            expectedFields.append(scenario.receivedCabrillo);
        }
        const QString expectedLine
            = expectedFields.join(QLatin1Char(' ')) + QLatin1Char('\n');
        const QByteArray outputData = output.readAll();
        QVERIFY2(outputData.contains(expectedLine.toUtf8()),
                 qPrintable(scenario.file + QStringLiteral(": expected ")
                            + expectedLine + QStringLiteral(" in:\n")
                            + QString::fromUtf8(outputData)));
    }
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

void LogExporterTest::omitsInvalidQsosFromExport()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    const logger::ContestDefinition contestDefinition = definition();
    logger::Qso invalid = qso();
    invalid.invalid = true;
    QString error;
    QVERIFY(!logger::LogExporter::writeFile(
        directory.filePath(QStringLiteral("empty.log")),
        logger::LogExportFormat::Cabrillo, contest(), &contestDefinition,
        {invalid}, station, &error));
    QVERIFY(error.contains(QStringLiteral("no QSOs"), Qt::CaseInsensitive));
}

void LogExporterTest::exportsWaeQtcImmediatelyAfterPartnerQso()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("wae.log"));
    logger::ContestDefinition contestDefinition = definition();
    contestDefinition.id = QStringLiteral("wae-cw");
    contestDefinition.cabrilloContest = QStringLiteral("WAE-DC-CW");
    logger::Qso contact = qso();
    contact.qsoUuid = QStringLiteral("11111111-1111-4111-8111-111111111111");
    logger::QtcTransfer transfer;
    transfer.qtcUuid = QStringLiteral("22222222-2222-4222-8222-222222222222");
    transfer.partnerQsoUuid = contact.qsoUuid;
    transfer.partnerCallsign = contact.callsign;
    transfer.direction = logger::QtcDirection::Sent;
    transfer.groupNumber = 1;
    transfer.messages.append({
        QDateTime::fromString(QStringLiteral("2026-10-15T09:40:00.000Z"),
                              Qt::ISODateWithMs),
        QStringLiteral("DL2XYZ"), 18
    });
    contact.qtcTransfers.append(transfer);
    logger::Station station;
    station.callsign = QStringLiteral("K1TEST");
    QString error;
    QVERIFY2(logger::LogExporter::writeFile(
                 path, logger::LogExportFormat::Cabrillo, contest(),
                 &contestDefinition, {contact}, station, &error),
             qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray output = file.readAll();
    const QByteArray qsoLine(
        "QSO: 14025 CW 2026-10-15 1000 K1TEST 599 001 W1AW 599 027\n");
    const QByteArray qtcLine("QTC: 1 0940 DL2XYZ 018\n");
    const qsizetype qsoOffset = output.indexOf(qsoLine);
    const qsizetype qtcOffset = output.indexOf(qtcLine);
    QVERIFY(qsoOffset >= 0);
    QVERIFY(qtcOffset > qsoOffset);
    QCOMPARE(qtcOffset, qsoOffset + qsoLine.size());

    const QString adifPath = path + QStringLiteral(".adi");
    QVERIFY2(logger::LogExporter::writeFile(
                 adifPath, logger::LogExportFormat::Adif, contest(),
                 &contestDefinition, {contact}, station, &error),
             qPrintable(error));
    QFile adifFile(adifPath);
    QVERIFY(adifFile.open(QIODevice::ReadOnly));
    const QByteArray adif = adifFile.readAll();
    QVERIFY(adif.contains("APP_CONTESTLOGGER_QTC"));
    QVERIFY(adif.contains("\"group_number\":1"));
    QVERIFY(adif.contains("\"serial_number\":18"));
}

QTEST_GUILESS_MAIN(LogExporterTest)
#include "test_log_exporter.moc"
