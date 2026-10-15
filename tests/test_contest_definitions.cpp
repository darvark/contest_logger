#include "logger/contest/contest_definition_loader.h"
#include "logger/contest/contest_engine.h"

#include <QtTest>

#include <algorithm>
#include <QDir>

namespace {

const QString SerialContestYaml = QStringLiteral(R"yaml(
schema_version: 1
id: test-serial
name: Test Serial Contest
description: Unit-test contest definition.
bands: [20m, 40m]
modes: [CW]
exchange:
  sent:
    - name: report
      type: rst
      default: "599"
    - name: serial
      type: serial
      generated: true
      width: 3
  received:
    - name: report
      type: rst
    - name: serial
      type: integer
      min: 1
      max: 999
scoring:
  qso_points: 2
  serial_start: 7
  multipliers:
    - type: prefix
      per_band: false
dupe_key: [callsign, band, mode]
)yaml");

const QString GridContestYaml = QStringLiteral(R"yaml(
schema_version: 1
id: test-grid
name: Test Grid Contest
bands: [20m]
modes: [CW, SSB]
exchange:
  sent:
    - name: report
      type: rst
      default: "599"
  received:
    - name: grid
      type: grid
scoring:
  qso_points: 3
  multipliers:
    - type: exchange
      field: grid
      per_band: true
    - type: prefix
      per_band: false
)yaml");

logger::ContestDefinition loadDefinition(const QString &yaml)
{
    logger::ContestDefinition definition;
    QString error;
    const logger::ContestDefinitionLoader loader;
    if (!loader.loadText(yaml, QStringLiteral("test.yaml"), definition, &error)) {
        qFatal("Could not load test definition: %s", qPrintable(error));
    }
    return definition;
}

} // namespace

class ContestDefinitionTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsValidDefinition();
    void rejectsMalformedYamlWithLocation();
    void rejectsDuplicateExchangeFields();
    void loadsBundledExamples();
    void rejectsInvalidReceivedExchange();
    void rejectsUndefinedReceivedExchangeFields();
    void detectsDuplicateCalls();
    void generatesNextSerialAndScoresQso();
    void awardsOnlyNewMultipliers();
    void validatesOperatorSentExchange();
    void rebuildsMultiplierContextAndScores();
    void generatesNextSerialFromStoredValues();
    void calculatesWpxPrefixes();
    void exposesCallsignValidation();
};

void ContestDefinitionTest::loadsValidDefinition()
{
    const auto definition = loadDefinition(SerialContestYaml);
    QCOMPARE(definition.id, QStringLiteral("test-serial"));
    QCOMPARE(definition.bands, QStringList({QStringLiteral("20m"),
                                            QStringLiteral("40m")}));
    QCOMPARE(definition.sentExchange.size(), 2);
    QCOMPARE(definition.receivedExchange.size(), 2);
    QCOMPARE(definition.serialStart, 7);
    QCOMPARE(definition.qsoPoints, 2);
    QCOMPARE(definition.multipliers.size(), 1);
}

void ContestDefinitionTest::rejectsMalformedYamlWithLocation()
{
    logger::ContestDefinition definition;
    QString error;
    const QString malformed = QStringLiteral(
        "schema_version: [\nid: broken\n");
    QVERIFY(!logger::ContestDefinitionLoader().loadText(
        malformed, QStringLiteral("broken-contest.yaml"), definition, &error));
    QVERIFY(error.contains(QStringLiteral("broken-contest.yaml:")));
    QVERIFY(error.contains(QStringLiteral("YAML syntax error")));
}

void ContestDefinitionTest::rejectsDuplicateExchangeFields()
{
    const QString invalid = SerialContestYaml + QStringLiteral(
        "\n# duplicate field definition\n");
    QString duplicate = invalid;
    duplicate.replace(
        QStringLiteral("      width: 3"),
        QStringLiteral("      width: 3\n    - name: serial\n      type: integer"));
    logger::ContestDefinition definition;
    QString error;
    QVERIFY(!logger::ContestDefinitionLoader().loadText(
        duplicate, QStringLiteral("duplicate.yaml"), definition, &error));
    QVERIFY(error.contains(QStringLiteral("duplicate field 'serial'")));
}

void ContestDefinitionTest::loadsBundledExamples()
{
    const QString examplesDirectory = QStringLiteral(
        CONTEST_LOGGER_TEST_SOURCE_DIR "/contests/examples");
    const QStringList filenames =
        QDir(examplesDirectory).entryList({QStringLiteral("*.yaml")},
                                         QDir::Files, QDir::Name);
    QVERIFY(filenames.size() >= 15);
    const QStringList requiredProfiles{
        QStringLiteral("cqww-cw.yaml"), QStringLiteral("cqww-ssb.yaml"),
        QStringLiteral("cqwpx-cw.yaml"), QStringLiteral("cqwpx-ssb.yaml"),
        QStringLiteral("iaruhfc.yaml"), QStringLiteral("spdx.yaml"),
        QStringLiteral("wag.yaml"), QStringLiteral("arrl-dx-cw.yaml"),
        QStringLiteral("arrl-dx-ssb.yaml"), QStringLiteral("arrl-dx-10m.yaml"),
        QStringLiteral("eudx.yaml"), QStringLiteral("euhfc.yaml")
    };
    for (const QString &required : requiredProfiles) {
        QVERIFY2(filenames.contains(required), qPrintable(required));
    }
    for (const QString &filename : filenames) {
        logger::ContestDefinition definition;
        QString error;
        QVERIFY2(logger::ContestDefinitionLoader().loadFile(
                     examplesDirectory + QLatin1Char('/') + filename,
                     definition, &error),
                 qPrintable(error));
        QVERIFY(!definition.id.isEmpty());
        QVERIFY(!definition.sentExchange.isEmpty());
        QVERIFY(!definition.receivedExchange.isEmpty());
    }
}

void ContestDefinitionTest::rejectsInvalidReceivedExchange()
{
    const auto definition = loadDefinition(SerialContestYaml);
    logger::ContestEvaluationContext context;
    const auto result = logger::ContestEngine().evaluate(
        definition, QStringLiteral("K1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("not-a-number")}},
        context);
    QVERIFY(!result.valid);
    QVERIFY(!result.issues.isEmpty());
    QCOMPARE(result.issues.first().field, QStringLiteral("received.serial"));
}

void ContestDefinitionTest::rejectsUndefinedReceivedExchangeFields()
{
    const auto definition = loadDefinition(SerialContestYaml);
    const auto result = logger::ContestEngine().evaluate(
        definition, QStringLiteral("K1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("15")},
         {QStringLiteral("county"), QStringLiteral("X")}},
        {});
    QVERIFY(!result.valid);
    QVERIFY(std::any_of(result.issues.cbegin(), result.issues.cend(),
                        [](const logger::ContestValidationIssue &issue) {
                            return issue.field == QStringLiteral("received.county");
                        }));
}

void ContestDefinitionTest::detectsDuplicateCalls()
{
    const auto definition = loadDefinition(SerialContestYaml);
    logger::ContestEvaluationContext context;
    logger::Qso previous;
    previous.callsign = QStringLiteral("K1ABC");
    previous.band = QStringLiteral("20m");
    previous.mode = QStringLiteral("CW");
    context.previousQsos.append(previous);

    const auto duplicate = logger::ContestEngine().evaluate(
        definition, QStringLiteral("k1abc"), QStringLiteral("20m"),
        QStringLiteral("cw"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("42")}},
        context);
    QVERIFY(duplicate.duplicate);
    QVERIFY(!duplicate.valid);

    const auto otherBand = logger::ContestEngine().evaluate(
        definition, QStringLiteral("K1ABC"), QStringLiteral("40m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("42")}},
        context);
    QVERIFY(otherBand.valid);
    QVERIFY(!otherBand.duplicate);
}

void ContestDefinitionTest::generatesNextSerialAndScoresQso()
{
    const auto definition = loadDefinition(SerialContestYaml);
    logger::ContestEvaluationContext context;
    context.previousQsos.resize(2);
    const auto result = logger::ContestEngine().evaluate(
        definition, QStringLiteral("K1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("15")}},
        context);

    QVERIFY(result.valid);
    QCOMPARE(result.serialNumber, 7);
    QCOMPARE(result.sentExchange.value(QStringLiteral("serial")),
             QStringLiteral("007"));
    QCOMPARE(result.points, 2);
}

void ContestDefinitionTest::awardsOnlyNewMultipliers()
{
    const auto definition = loadDefinition(GridContestYaml);
    logger::ContestEvaluationContext context;
    context.workedMultiplierKeys.insert(QStringLiteral("exchange:grid:FN20@20m"));
    context.resolvedMultiplierValues.insert(QStringLiteral("prefix"),
                                            QStringLiteral("W1"));

    const auto result = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("grid"), QStringLiteral("FN20")}},
        context);
    QVERIFY(result.valid);
    QCOMPARE(result.points, 3);
    QCOMPARE(result.newMultiplierKeys,
             QStringList({QStringLiteral("prefix:W1")}));

    const auto unresolved = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("20m"),
        QStringLiteral("CW"), {{QStringLiteral("grid"), QStringLiteral("FN20")}});
    QVERIFY(unresolved.valid);
    QVERIFY(unresolved.newMultiplierKeys.contains(QStringLiteral("prefix:W1")));
    QVERIFY(unresolved.warnings.isEmpty());
}

void ContestDefinitionTest::validatesOperatorSentExchange()
{
    auto definition = loadDefinition(GridContestYaml);
    logger::ExchangeFieldDefinition zone;
    zone.name = QStringLiteral("zone");
    zone.label = QStringLiteral("CQ Zone (station)");
    zone.type = logger::ExchangeFieldType::Integer;
    zone.minimum = 1;
    zone.maximum = 40;
    definition.sentExchange.append(zone);

    const QMap<QString, QString> received{
        {QStringLiteral("grid"), QStringLiteral("FN20")}
    };
    const auto missing = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("20m"),
        QStringLiteral("CW"), received);
    QVERIFY(!missing.valid);
    QVERIFY(std::any_of(missing.issues.cbegin(), missing.issues.cend(),
                        [](const logger::ContestValidationIssue &issue) {
                            return issue.field == QStringLiteral("sent.zone");
                        }));

    const auto supplied = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("20m"),
        QStringLiteral("CW"), received, {},
        std::nullopt, {{QStringLiteral("zone"), QStringLiteral("5")}});
    QVERIFY(supplied.valid);
    QCOMPARE(supplied.sentExchange.value(QStringLiteral("zone")),
             QStringLiteral("5"));
}

void ContestDefinitionTest::rebuildsMultiplierContextAndScores()
{
    auto definition = loadDefinition(GridContestYaml);
    definition.bands.append(QStringLiteral("40m"));
    QVector<logger::Qso> qsos;
    const auto addQso = [&qsos](const QString &call, const QString &band,
                                const QString &grid, int seconds) {
        logger::Qso qso;
        qso.callsign = call;
        qso.band = band;
        qso.mode = QStringLiteral("CW");
        qso.timestampUtc = QDateTime::fromSecsSinceEpoch(seconds).toUTC();
        qso.receivedExchangeFields.insert(QStringLiteral("grid"), grid);
        qsos.append(qso);
    };
    addQso(QStringLiteral("W1AA"), QStringLiteral("20m"), QStringLiteral("FN20"), 1);
    addQso(QStringLiteral("W1BB"), QStringLiteral("20m"), QStringLiteral("FN20"), 2);
    addQso(QStringLiteral("K2CC"), QStringLiteral("40m"), QStringLiteral("FN20"), 3);
    qsos[0].id = 101;
    qsos[1].id = 102;
    qsos[2].id = 103;
    for (logger::Qso &qso : qsos) {
        qso.receivedExchangeFields.insert(QStringLiteral("state"), QStringLiteral("FN20"));
    }
    logger::ExchangeFieldDefinition stateField;
    stateField.name = QStringLiteral("state");
    stateField.type = logger::ExchangeFieldType::Text;
    definition.receivedExchange.append(stateField);
    definition.multipliers.append(
        {QStringLiteral("exchange"), QStringLiteral("state"), true});

    const logger::ContestEngine engine;
    const auto context = engine.buildContext(
        definition, {qsos.at(0), qsos.at(1)});
    QVERIFY(context.workedMultiplierKeys.contains(
        QStringLiteral("exchange:grid:FN20@20m")));
    QVERIFY(context.workedMultiplierKeys.contains(QStringLiteral("prefix:W1")));
    const auto second = engine.evaluate(
        definition, qsos.at(1).callsign, qsos.at(1).band, qsos.at(1).mode,
        qsos.at(1).receivedExchangeFields, engine.buildContext(definition, {qsos.at(0)}));
    QVERIFY(second.newMultiplierKeys.isEmpty());

    const auto score = engine.score(definition, qsos);
    QCOMPARE(score.totalQsos, 3);
    QCOMPARE(score.totalPoints, 9);
    QCOMPARE(score.totalMultipliers, 6);
    QCOMPARE(score.multiplierCounts.value(QStringLiteral("exchange")), 4);
    QCOMPARE(score.multiplierCounts.value(QStringLiteral("prefix")), 2);
    QCOMPARE(score.finalScore, 54);
    QCOMPARE(score.perBand.value(QStringLiteral("20m")).multipliers, 3);
    QCOMPARE(score.perBand.value(QStringLiteral("40m")).multipliers, 3);
    QCOMPARE(score.newMultipliersByQsoId.value(101), 3);
    QCOMPARE(score.newMultipliersByQsoId.value(102), 0);
    QCOMPARE(score.newMultipliersByQsoId.value(103), 3);
    QCOMPARE(engine.multiplierKeys(definition, qsos.first()).size(), 3);

    const auto sameValue = engine.evaluate(
        definition, QStringLiteral("W2ZZ"), QStringLiteral("20m"),
        QStringLiteral("CW"), {{QStringLiteral("grid"), QStringLiteral("FN20")},
                                {QStringLiteral("state"), QStringLiteral("FN20")}});
    QVERIFY(sameValue.newMultiplierKeys.contains(
        QStringLiteral("exchange:grid:FN20@20m")));
    QVERIFY(sameValue.newMultiplierKeys.contains(
        QStringLiteral("exchange:state:FN20@20m")));

    qsos[0].points = 7;
    const auto storedPoints = engine.score(definition, qsos);
    QCOMPARE(storedPoints.totalPoints, 13);
    QCOMPARE(storedPoints.finalScore, 78);
}

void ContestDefinitionTest::generatesNextSerialFromStoredValues()
{
    const auto definition = loadDefinition(SerialContestYaml);
    QVector<logger::Qso> qsos(4);
    qsos[0].sentExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("007"));
    qsos[1].sentExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("021"));
    qsos[2].sentExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("bad"));
    qsos[3].sentExchange = QStringLiteral("599 045");
    QCOMPARE(logger::ContestEngine().nextSerialNumber(definition, qsos), 46);
    qsos[3].sentExchange = QStringLiteral("599 006");
    QCOMPARE(logger::ContestEngine().nextSerialNumber(definition, qsos), 22);

    const auto evaluated = logger::ContestEngine().evaluate(
        definition, QStringLiteral("K1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"), {{QStringLiteral("report"), QStringLiteral("599")}},
        logger::ContestEngine().buildContext(definition, qsos));
    QCOMPARE(evaluated.serialNumber, 22);
}

void ContestDefinitionTest::calculatesWpxPrefixes()
{
    QCOMPARE(logger::ContestEngine::wpxPrefix(QStringLiteral("K1ABC")),
             QStringLiteral("K1"));
    QCOMPARE(logger::ContestEngine::wpxPrefix(QStringLiteral("DL/K1ABC")),
             QStringLiteral("K1"));
    QCOMPARE(logger::ContestEngine::wpxPrefix(QStringLiteral("K1ABC/P")),
             QStringLiteral("K1"));
    QCOMPARE(logger::ContestEngine::wpxPrefix(QStringLiteral("K1ABC/4")),
             QStringLiteral("K4"));
    QCOMPARE(logger::ContestEngine::wpxPrefix(QStringLiteral("GMM")),
             QStringLiteral("GMM0"));
}

void ContestDefinitionTest::exposesCallsignValidation()
{
    QVERIFY(logger::ContestEngine::isValidCallsign(QStringLiteral(" k1abc/p ")));
    QVERIFY(!logger::ContestEngine::isValidCallsign(QStringLiteral("bad!")));
}

QTEST_GUILESS_MAIN(ContestDefinitionTest)

#include "test_contest_definitions.moc"
