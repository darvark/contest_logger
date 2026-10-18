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
    void loadsCabrilloContestName();
    void rejectsMalformedYamlWithLocation();
    void rejectsDuplicateExchangeFields();
    void loadsBundledExamples();
    void loadsCtyMultiplierType();
    void scoresSpdxProfiles();
    void rejectsInvalidReceivedExchange();
    void rejectsUndefinedReceivedExchangeFields();
    void detectsDuplicateCalls();
    void duplicateQsosDoNotScoreOrEarnMultipliers();
    void generatesNextSerialAndScoresQso();
    void awardsOnlyNewMultipliers();
    void validatesOperatorSentExchange();
    void rebuildsMultiplierContextAndScores();
    void generatesNextSerialFromStoredValues();
    void calculatesWpxPrefixes();
    void exposesCallsignValidation();
    void validatesConditionalExchangeAndQsoPointRules();
    void scoresLocationRulesAndStationDependentExchange();
    void rejectsUnknownConditionalRuleFields();
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
    QVERIFY(definition.cabrilloContest.isEmpty());
}

void ContestDefinitionTest::loadsCabrilloContestName()
{
    QString yaml = SerialContestYaml;
    yaml.replace(QStringLiteral("modes: [CW]\n"),
                 QStringLiteral("modes: [CW]\ncabrillo: {contest: cq-wpx-cw}\n"));
    QCOMPARE(loadDefinition(yaml).cabrilloContest, QStringLiteral("CQ-WPX-CW"));

    logger::ContestDefinition definition;
    QString error;
    QString invalid = SerialContestYaml;
    invalid.replace(QStringLiteral("modes: [CW]\n"),
                    QStringLiteral("modes: [CW]\ncabrillo: {contest: \"CQ WPX\"}\n"));
    QVERIFY(!logger::ContestDefinitionLoader().loadText(
        invalid, QStringLiteral("invalid.yaml"), definition, &error));
    QVERIFY(error.contains(QStringLiteral("contest.cabrillo.contest")));
    invalid = SerialContestYaml;
    invalid.replace(QStringLiteral("modes: [CW]\n"),
                    QStringLiteral("modes: [CW]\ncabrillo: {name: X}\n"));
    QVERIFY(!logger::ContestDefinitionLoader().loadText(
        invalid, QStringLiteral("invalid.yaml"), definition, &error));
    QVERIFY(error.contains(QStringLiteral("contest.cabrillo.name")));
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
        QStringLiteral("spdx-dx.yaml"),
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
        if (filename == QStringLiteral("cqwpx-cw.yaml")) {
            QCOMPARE(definition.cabrilloContest, QStringLiteral("CQ-WPX-CW"));
        }
    }
}

void ContestDefinitionTest::loadsCtyMultiplierType()
{
    QString yaml = GridContestYaml;
    yaml.replace(QStringLiteral("    - type: prefix\n      per_band: false"),
                 QStringLiteral("    - type: dxcc\n      per_band: true"));
    const auto definition = loadDefinition(yaml);
    logger::Qso qso;
    qso.callsign = QStringLiteral("DL1ABC");
    qso.band = QStringLiteral("20m");

    const auto keys = logger::ContestEngine().multiplierKeys(
        definition, qso, [](const logger::Qso &) {
            return QMap<QString, QString>{{QStringLiteral("dxcc"),
                                          QStringLiteral("Germany")}};
        });
    QVERIFY(keys.contains(QStringLiteral("dxcc:GERMANY@20m")));
}

void ContestDefinitionTest::scoresSpdxProfiles()
{
    const QString examplesDirectory = QStringLiteral(
        CONTEST_LOGGER_TEST_SOURCE_DIR "/contests/examples");
    logger::ContestDefinition polishProfile;
    logger::ContestDefinition dxProfile;
    QString error;
    const logger::ContestDefinitionLoader loader;
    QVERIFY2(loader.loadFile(examplesDirectory + QStringLiteral("/spdx.yaml"),
                             polishProfile, &error),
             qPrintable(error));
    QVERIFY2(loader.loadFile(examplesDirectory + QStringLiteral("/spdx-dx.yaml"),
                             dxProfile, &error),
             qPrintable(error));

    logger::ContestEvaluationContext context;
    context.resolvedMultiplierValues = {
        {QStringLiteral("continent"), QStringLiteral("EU")},
        {QStringLiteral("dxcc"), QStringLiteral("Germany")}
    };
    const auto euQso = logger::ContestEngine().evaluate(
        polishProfile, QStringLiteral("DL1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("123")}},
        context, std::nullopt, {{QStringLiteral("province"), QStringLiteral("M")}});
    QVERIFY2(euQso.valid, qPrintable(euQso.issues.isEmpty()
                                         ? QString()
                                         : euQso.issues.first().message));
    QCOMPARE(euQso.points, 1);
    QVERIFY(euQso.newMultiplierKeys.contains(
        QStringLiteral("dxcc:GERMANY@20m")));

    context.resolvedMultiplierValues = {
        {QStringLiteral("continent"), QStringLiteral("NA")},
        {QStringLiteral("dxcc"), QStringLiteral("United States")}
    };
    const auto dxQso = logger::ContestEngine().evaluate(
        polishProfile, QStringLiteral("K1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("serial"), QStringLiteral("124")}},
        context, std::nullopt, {{QStringLiteral("province"), QStringLiteral("M")}});
    QVERIFY(dxQso.valid);
    QCOMPARE(dxQso.points, 3);

    context.resolvedMultiplierValues = {
        {QStringLiteral("country"), QStringLiteral("Poland")}
    };
    const auto spQso = logger::ContestEngine().evaluate(
        dxProfile, QStringLiteral("SP9ABC"), QStringLiteral("40m"),
        QStringLiteral("SSB"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("province"), QStringLiteral("M")}},
        context);
    QVERIFY(spQso.valid);
    QCOMPARE(spQso.points, 3);
    QVERIFY(spQso.newMultiplierKeys.contains(
        QStringLiteral("exchange:province:M@40m")));
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

void ContestDefinitionTest::duplicateQsosDoNotScoreOrEarnMultipliers()
{
    logger::ContestDefinition definition;
    definition.qsoPoints = 3;
    definition.receivedExchange.append(
        {QStringLiteral("state"), QStringLiteral("State"),
         logger::ExchangeFieldType::Text, false});
    definition.multipliers.append(
        {QStringLiteral("exchange"), QStringLiteral("state"), false, false});
    QVector<logger::Qso> qsos(2);
    qsos[0].id = 1;
    qsos[0].callsign = QStringLiteral("K1ABC");
    qsos[0].band = QStringLiteral("20m");
    qsos[0].mode = QStringLiteral("CW");
    qsos[0].points = 3;
    qsos[0].receivedExchangeFields.insert(QStringLiteral("state"), QStringLiteral("CA"));
    qsos[1] = qsos[0];
    qsos[1].id = 2;
    qsos[1].points = 99;
    qsos[1].receivedExchangeFields.insert(QStringLiteral("state"), QStringLiteral("NY"));

    const logger::ContestEngine engine;
    const logger::ContestScore score = engine.score(definition, qsos);
    QCOMPARE(score.totalQsos, 2);
    QCOMPARE(score.totalPoints, 3);
    QCOMPARE(score.totalMultipliers, 1);
    QCOMPARE(score.newMultipliersByQsoId.value(1), 1);
    QCOMPARE(score.newMultipliersByQsoId.value(2), 0);
    QCOMPARE(score.finalScore, 3);

    const auto context = engine.buildContext(definition, {qsos.at(1), qsos.at(0)});
    QVERIFY(context.workedMultiplierKeys.contains(QStringLiteral("exchange:state:CA")));
    QVERIFY(!context.workedMultiplierKeys.contains(QStringLiteral("exchange:state:NY")));
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

void ContestDefinitionTest::validatesConditionalExchangeAndQsoPointRules()
{
    const auto definition = loadDefinition(QStringLiteral(R"yaml(
schema_version: 1
id: conditional-scoring
name: Conditional scoring
bands: [20m, 40m]
modes: [CW]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
  received:
    - {name: report, type: rst}
    - name: state
      type: text
      required: false
      required_when:
        continent: NA
scoring:
  qso_points: 1
  qso_point_rules:
    - points: 5
      when:
        continent: NA
        band: 20m
    - points: 3
      when:
        country: United States
)yaml"));

    logger::ContestEvaluationContext context;
    context.resolvedMultiplierValues = {
        {QStringLiteral("continent"), QStringLiteral("NA")},
        {QStringLiteral("dxcc"), QStringLiteral("United States")}
    };
    const auto missingRequiredField = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")}}, context);
    QVERIFY(!missingRequiredField.valid);
    QVERIFY(std::any_of(
        missingRequiredField.issues.cbegin(), missingRequiredField.issues.cend(),
        [](const logger::ContestValidationIssue &issue) {
            return issue.field == QStringLiteral("received.state");
        }));

    const auto twentyMeters = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")},
         {QStringLiteral("state"), QStringLiteral("NY")}},
        context);
    QVERIFY(twentyMeters.valid);
    QCOMPARE(twentyMeters.points, 5);

    auto otherContinent = context;
    otherContinent.resolvedMultiplierValues.insert(
        QStringLiteral("continent"), QStringLiteral("EU"));
    const auto fortyMeters = logger::ContestEngine().evaluate(
        definition, QStringLiteral("W1AW"), QStringLiteral("40m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")}},
        otherContinent);
    QVERIFY(fortyMeters.valid);
    QCOMPARE(fortyMeters.points, 3);

    logger::Qso qso;
    qso.callsign = QStringLiteral("W1AW");
    qso.band = QStringLiteral("20m");
    qso.mode = QStringLiteral("CW");
    qso.timestampUtc = QDateTime::fromSecsSinceEpoch(1).toUTC();
    qso.receivedExchangeFields = {
        {QStringLiteral("report"), QStringLiteral("599")},
        {QStringLiteral("state"), QStringLiteral("NY")}
    };
    const auto score = logger::ContestEngine().score(
        definition, {qso}, [&context](const logger::Qso &) {
            return context.resolvedMultiplierValues;
        });
    QCOMPARE(score.totalPoints, 5);
}

void ContestDefinitionTest::scoresLocationRulesAndStationDependentExchange()
{
    const auto definition = loadDefinition(QStringList{
        QStringLiteral("schema_version: 1"),
        QStringLiteral("id: station-scoring"),
        QStringLiteral("name: Station scoring"),
        QStringLiteral("bands: [20m]"),
        QStringLiteral("modes: [CW, SSB]"),
        QStringLiteral("exchange:"),
        QStringLiteral("  sent:"),
        QStringLiteral("    - {name: report, type: rst, default: \"599\"}"),
        QStringLiteral("    - name: state"),
        QStringLiteral("      type: text"),
        QStringLiteral("      when:"),
        QStringLiteral("        station.country: Poland"),
        QStringLiteral("    - name: province"),
        QStringLiteral("      type: text"),
        QStringLiteral("      when:"),
        QStringLiteral("        station.country: Canada"),
        QStringLiteral("  received:"),
        QStringLiteral("    - {name: report, type: rst}"),
        QStringLiteral("scoring:"),
        QStringLiteral("  qso_points: 1"),
        QStringLiteral("  qso_point_rules:"),
        QStringLiteral("    - {points: 5, when: {same_country: true}}"),
        QStringLiteral("    - {points: 3, when: {same_continent: true}}"),
        QStringLiteral("  multipliers:"),
        QStringLiteral("    - {type: country, per_mode: true}")}
                                               .join(QLatin1Char('\n')));

    logger::ContestEvaluationContext context;
    context.resolvedMultiplierValues
        = {{QStringLiteral("dxcc"), QStringLiteral("Poland")},
           {QStringLiteral("continent"), QStringLiteral("EU")},
           {QStringLiteral("station.dxcc"), QStringLiteral("Poland")},
           {QStringLiteral("station.continent"), QStringLiteral("EU")},
           {QStringLiteral("same_country"), QStringLiteral("true")},
           {QStringLiteral("same_continent"), QStringLiteral("true")},
           {QStringLiteral("country"), QStringLiteral("Poland")}};
    const auto localQso = logger::ContestEngine().evaluate(
        definition, QStringLiteral("SP9ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")}}, context,
        std::nullopt,
        {{QStringLiteral("state"), QStringLiteral("MA")},
         {QStringLiteral("province"), QStringLiteral("ON")}});
    QVERIFY(localQso.valid);
    QCOMPARE(localQso.points, 5);
    QCOMPARE(localQso.sentExchange.value(QStringLiteral("state")),
             QStringLiteral("MA"));
    QVERIFY(!localQso.sentExchange.contains(QStringLiteral("province")));

    auto remoteContext = context;
    remoteContext.resolvedMultiplierValues.insert(
        QStringLiteral("same_country"), QStringLiteral("false"));
    const auto remoteQso = logger::ContestEngine().evaluate(
        definition, QStringLiteral("DL1ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")}}, remoteContext,
        std::nullopt, {{QStringLiteral("state"), QStringLiteral("MA")}});
    QVERIFY(remoteQso.valid);
    QCOMPARE(remoteQso.points, 3);

    QVector<logger::Qso> qsos(2);
    qsos[0].id = 1;
    qsos[0].callsign = QStringLiteral("DL1ABC");
    qsos[0].band = QStringLiteral("20m");
    qsos[0].mode = QStringLiteral("CW");
    qsos[0].timestampUtc = QDateTime::fromSecsSinceEpoch(1).toUTC();
    qsos[1] = qsos[0];
    qsos[1].id = 2;
    qsos[1].callsign = QStringLiteral("JA1ABC");
    qsos[1].mode = QStringLiteral("SSB");
    qsos[1].timestampUtc = QDateTime::fromSecsSinceEpoch(2).toUTC();
    const auto score = logger::ContestEngine().score(
        definition, qsos, [&context](const logger::Qso &) {
            return context.resolvedMultiplierValues;
        });
    QCOMPARE(score.totalMultipliers, 2);
    QCOMPARE(score.perMode.value(QStringLiteral("CW")).multipliers, 1);
    QCOMPARE(score.perMode.value(QStringLiteral("SSB")).multipliers, 1);
    QVERIFY(score.multiplierCounts.value(QStringLiteral("country")) == 2);

    const auto missingStationLocation = logger::ContestEngine().evaluate(
        definition, QStringLiteral("SP9ABC"), QStringLiteral("20m"),
        QStringLiteral("CW"),
        {{QStringLiteral("report"), QStringLiteral("599")}});
    QVERIFY(!missingStationLocation.valid);
}

void ContestDefinitionTest::rejectsUnknownConditionalRuleFields()
{
    const QString yaml = QStringLiteral(R"yaml(
schema_version: 1
id: invalid-condition
name: Invalid condition
bands: [20m]
modes: [CW]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
  received:
    - {name: report, type: rst}
scoring:
  qso_points: 1
  qso_point_rules:
    - points: 5
      when:
        dxcc_group: NA
)yaml");
    logger::ContestDefinition definition;
    QString error;
    QVERIFY(!logger::ContestDefinitionLoader().loadText(
        yaml, QStringLiteral("invalid-condition.yaml"), definition, &error));
    QVERIFY(
        error.contains(QStringLiteral("unsupported condition 'dxcc_group'")));
}

QTEST_GUILESS_MAIN(ContestDefinitionTest)

#include "test_contest_definitions.moc"
