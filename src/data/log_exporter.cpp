#include "logger/data/log_exporter.h"

#include "logger/contest/contest_engine.h"

#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <iterator>

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

QMap<QString, QString> exchangeValues(
    const Qso &qso, bool sent,
    const QVector<ExchangeFieldDefinition> *order)
{
    const QMap<QString, QString> &stored =
        sent ? qso.sentExchangeFields : qso.receivedExchangeFields;
    if (!stored.isEmpty()) {
        return stored;
    }
    const QString legacy = sent ? qso.sentExchange : qso.receivedExchange;
    return LogExporter::mapLegacyExchange(
        legacy, order != nullptr ? *order : QVector<ExchangeFieldDefinition>{});
}

QString normalizedFields(const Qso &qso, const QVector<ExchangeFieldDefinition> *order,
                         bool sent, const QString &excludeField = {})
{
    const QMap<QString, QString> fields = exchangeValues(qso, sent, order);
    QStringList values;
    if (order != nullptr) {
        for (const ExchangeFieldDefinition &field : *order) {
            if (field.name.compare(excludeField, Qt::CaseInsensitive) == 0) {
                continue;
            }
            const QString value = fields.value(field.name).trimmed();
            if (!value.isEmpty()) {
                values.append(value.toUpper());
            }
        }
    } else {
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            if (it.key().compare(excludeField, Qt::CaseInsensitive) == 0) {
                continue;
            }
            const QString value = it.value().trimmed();
            if (!value.isEmpty()) {
                values.append(value.toUpper());
            }
        }
    }
    return values.join(QLatin1Char(' '));
}

QString rstFieldName(const QVector<ExchangeFieldDefinition> *fields,
                     const QMap<QString, QString> &values)
{
    if (fields != nullptr) {
        for (const ExchangeFieldDefinition &field : *fields) {
            if (field.type == ExchangeFieldType::Rst
                && !values.value(field.name).trimmed().isEmpty()) {
                return field.name;
            }
        }
    }
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if ((it.key().compare(QStringLiteral("rst"), Qt::CaseInsensitive) == 0
             || it.key().compare(QStringLiteral("report"), Qt::CaseInsensitive) == 0)
            && !it.value().trimmed().isEmpty()) {
            return it.key();
        }
    }
    return {};
}

QString rstValue(const Qso &qso, bool sent,
                 const QVector<ExchangeFieldDefinition> *fields)
{
    const QMap<QString, QString> values = exchangeValues(qso, sent, fields);
    const QString key = rstFieldName(fields, values);
    if (!key.isEmpty()) {
        return values.value(key).trimmed().toUpper();
    }
    const QString fallback = sent ? qso.sentExchange : qso.receivedExchange;
    const QStringList tokens = fallback.split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (!tokens.isEmpty()
        && QRegularExpression(QStringLiteral("^[1-5][1-9][1-9]?$"))
               .match(tokens.first()).hasMatch()) {
        return tokens.first().toUpper();
    }
    return qso.mode.compare(QStringLiteral("CW"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("599") : QStringLiteral("59");
}

QString exportExchange(const Qso &qso, bool sent,
                       const QVector<ExchangeFieldDefinition> *fields)
{
    const QMap<QString, QString> values = exchangeValues(qso, sent, fields);
    const QString rstKey = rstFieldName(fields, values);
    const QString structured = normalizedFields(qso, fields, sent, rstKey);
    if (!structured.isEmpty()) {
        return structured;
    }
    QString legacy = sent ? qso.sentExchange : qso.receivedExchange;
    if (legacy.contains(QLatin1Char('='))) {
        return {};
    }
    const QString rst = rstValue(qso, sent, fields);
    if (legacy.trimmed().startsWith(rst, Qt::CaseInsensitive)) {
        legacy = legacy.trimmed().mid(rst.size()).trimmed();
    }
    return legacy.toUpper();
}

QString cabrilloMode(const QString &mode)
{
    const QString normalized = mode.trimmed().toUpper();
    if (normalized == QStringLiteral("SSB")
        || normalized == QStringLiteral("USB")
        || normalized == QStringLiteral("LSB")) {
        return QStringLiteral("PH");
    }
    if (normalized == QStringLiteral("RTTY")) {
        return QStringLiteral("RY");
    }
    if (normalized == QStringLiteral("DATA")
        || normalized == QStringLiteral("FT8")
        || normalized == QStringLiteral("FT4")
        || normalized == QStringLiteral("PSK")) {
        return QStringLiteral("DG");
    }
    if (normalized == QStringLiteral("CW")
        || normalized == QStringLiteral("FM")
        || normalized == QStringLiteral("AM")) {
        return normalized;
    }
    return {};
}

QString adifField(const QString &name, const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    return QStringLiteral("<%1:%2>%3")
        .arg(name.toUpper())
        .arg(bytes.size())
        .arg(QString::fromUtf8(bytes));
}

QString adifExchange(const QMap<QString, QString> &fields)
{
    QStringList parts;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (!it.value().trimmed().isEmpty()) {
            parts.append(QStringLiteral("%1=%2").arg(it.key().toUpper(),
                                                     it.value().trimmed().toUpper()));
        }
    }
    return parts.join(QLatin1Char(' '));
}

QString exportContestCode(const Contest &contest, const ContestDefinition *definition)
{
    // Several logs may share one definition; their stored codes carry a numeric suffix.
    return (definition != nullptr && !definition->id.isEmpty() ? definition->id
                                                              : contest.code)
        .trimmed().toUpper();
}

QString cabrilloContestName(const Contest &contest, const ContestDefinition *definition)
{
    if (definition != nullptr && !definition->cabrilloContest.trimmed().isEmpty()) {
        return definition->cabrilloContest.trimmed().toUpper();
    }
    return exportContestCode(contest, definition);
}

QString cabrilloBand(const QString &band)
{
    static const QHash<QString, QString> microwave{
        {QStringLiteral("1.25M"), QStringLiteral("222")},
        {QStringLiteral("70CM"), QStringLiteral("432")},
        {QStringLiteral("33CM"), QStringLiteral("902")},
        {QStringLiteral("23CM"), QStringLiteral("1.2G")},
        {QStringLiteral("13CM"), QStringLiteral("2.3G")},
        {QStringLiteral("9CM"), QStringLiteral("3.4G")},
        {QStringLiteral("6CM"), QStringLiteral("5.7G")},
        {QStringLiteral("3CM"), QStringLiteral("10G")}};
    const QString normalized = band.trimmed().toUpper();
    return microwave.value(normalized, normalized);
}

QString bandFrequencyKhz(const QString &band)
{
    // Lower band edges used when no rig frequency was recorded with the QSO.
    static const QHash<QString, QString> edges{
        {QStringLiteral("160m"), QStringLiteral("1800")},
        {QStringLiteral("80m"), QStringLiteral("3500")},
        {QStringLiteral("60m"), QStringLiteral("5351")},
        {QStringLiteral("40m"), QStringLiteral("7000")},
        {QStringLiteral("30m"), QStringLiteral("10100")},
        {QStringLiteral("20m"), QStringLiteral("14000")},
        {QStringLiteral("17m"), QStringLiteral("18068")},
        {QStringLiteral("15m"), QStringLiteral("21000")},
        {QStringLiteral("12m"), QStringLiteral("24890")},
        {QStringLiteral("10m"), QStringLiteral("28000")},
        {QStringLiteral("6m"), QStringLiteral("50")},
        {QStringLiteral("4m"), QStringLiteral("70")},
        {QStringLiteral("2m"), QStringLiteral("144")},
        {QStringLiteral("1.25m"), QStringLiteral("222")},
        {QStringLiteral("70cm"), QStringLiteral("432")},
        {QStringLiteral("33cm"), QStringLiteral("902")},
        {QStringLiteral("23cm"), QStringLiteral("1.2G")}};
    const QString normalized = band.trimmed().toLower();
    return edges.value(normalized,
                       QString(normalized).remove(QLatin1Char('m')));
}

QString categoryModeFor(const QString &mode)
{
    const QString cabrillo = cabrilloMode(mode);
    if (cabrillo == QStringLiteral("PH") || cabrillo == QStringLiteral("AM")) {
        return QStringLiteral("SSB");
    }
    if (cabrillo == QStringLiteral("RY")) {
        return QStringLiteral("RTTY");
    }
    if (cabrillo == QStringLiteral("DG")) {
        return QStringLiteral("DIGI");
    }
    return cabrillo;
}

QStringList textLines(const QString &text)
{
    QStringList lines;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) {
            lines.append(line.trimmed());
        }
    }
    return lines;
}

QStringList cabrilloHeader(const Contest &contest, const ContestDefinition *definition,
                           const QVector<Qso> &qsos, const Station &station,
                           const LogExportOptions &options)
{
    const QMap<QString, QString> &properties = contest.properties;
    const auto property = [&properties](const QString &tag) {
        return properties.value(tag).trimmed();
    };
    const QString callsign = station.callsign.trimmed().toUpper();
    QStringList operators = property(QStringLiteral("OPERATORS")).toUpper()
        .split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    operators.removeDuplicates();

    QString categoryOperator = property(QStringLiteral("CATEGORY-OPERATOR")).toUpper();
    if (categoryOperator.isEmpty()) {
        categoryOperator = operators.size() > 1 ? QStringLiteral("MULTI-OP")
                                                : QStringLiteral("SINGLE-OP");
    }
    if (operators.isEmpty() && categoryOperator == QStringLiteral("SINGLE-OP")) {
        operators.append(callsign);
    }
    QString categoryBand = cabrilloBand(property(QStringLiteral("CATEGORY-BAND")));
    if (categoryBand.isEmpty()) {
        QSet<QString> bands;
        for (const Qso &qso : qsos) {
            bands.insert(cabrilloBand(qso.band));
        }
        categoryBand = bands.size() == 1 ? *bands.cbegin() : QStringLiteral("ALL");
    }
    QString categoryMode = property(QStringLiteral("CATEGORY-MODE")).toUpper();
    if (categoryMode.isEmpty()) {
        QSet<QString> modes;
        for (const Qso &qso : qsos) {
            modes.insert(categoryModeFor(qso.mode));
        }
        categoryMode = modes.size() == 1 ? *modes.cbegin() : QStringLiteral("MIXED");
    }
    QString categoryPower = property(QStringLiteral("CATEGORY-POWER")).toUpper();
    if (categoryPower.isEmpty()) {
        const QString configured = station.powerClass.trimmed().toUpper();
        if (configured == QStringLiteral("HIGH") || configured == QStringLiteral("LOW")
            || configured == QStringLiteral("QRP")) {
            categoryPower = configured;
        }
    }
    QString categoryAssisted = property(QStringLiteral("CATEGORY-ASSISTED")).toUpper();
    if (categoryAssisted.isEmpty() && categoryOperator != QStringLiteral("CHECKLOG")) {
        categoryAssisted = QStringLiteral("NON-ASSISTED");
    }
    QString categoryTransmitter =
        property(QStringLiteral("CATEGORY-TRANSMITTER")).toUpper();
    if (categoryTransmitter.isEmpty() && categoryOperator == QStringLiteral("SINGLE-OP")) {
        categoryTransmitter = QStringLiteral("ONE");
    }

    qint64 claimedScore = 0;
    if (options.claimedScore.has_value()) {
        claimedScore = *options.claimedScore;
    } else if (definition != nullptr) {
        claimedScore = ContestEngine().score(*definition, qsos).finalScore;
    } else {
        for (const Qso &qso : qsos) {
            claimedScore += qso.points;
        }
    }

    QStringList lines{QStringLiteral("START-OF-LOG: 3.0")};
    const auto add = [&lines](const QString &tag, const QString &value) {
        if (!value.trimmed().isEmpty()) {
            lines.append(QStringLiteral("%1: %2").arg(tag, value.trimmed()));
        }
    };
    add(QStringLiteral("CREATED-BY"), options.createdBy.trimmed().isEmpty()
                                          ? QStringLiteral("Contest Logger")
                                          : options.createdBy);
    add(QStringLiteral("CONTEST"), cabrilloContestName(contest, definition));
    add(QStringLiteral("CALLSIGN"), callsign);
    add(QStringLiteral("LOCATION"), property(QStringLiteral("LOCATION")).toUpper());
    add(QStringLiteral("CATEGORY-OPERATOR"), categoryOperator);
    add(QStringLiteral("CATEGORY-ASSISTED"), categoryAssisted);
    add(QStringLiteral("CATEGORY-BAND"), categoryBand);
    add(QStringLiteral("CATEGORY-MODE"), categoryMode);
    add(QStringLiteral("CATEGORY-POWER"), categoryPower);
    add(QStringLiteral("CATEGORY-STATION"),
        property(QStringLiteral("CATEGORY-STATION")).toUpper());
    add(QStringLiteral("CATEGORY-TRANSMITTER"), categoryTransmitter);
    add(QStringLiteral("CATEGORY-TIME"), property(QStringLiteral("CATEGORY-TIME")).toUpper());
    add(QStringLiteral("CATEGORY-OVERLAY"),
        property(QStringLiteral("CATEGORY-OVERLAY")).toUpper());
    add(QStringLiteral("GRID-LOCATOR"), station.grid.trimmed().toUpper());
    add(QStringLiteral("CLAIMED-SCORE"), QString::number(claimedScore));
    add(QStringLiteral("CLUB"), property(QStringLiteral("CLUB")));
    add(QStringLiteral("NAME"), station.operatorName);
    add(QStringLiteral("EMAIL"), station.email);
    for (const QString &line : textLines(station.address).mid(0, 6)) {
        add(QStringLiteral("ADDRESS"), line);
    }
    add(QStringLiteral("ADDRESS-CITY"), station.city);
    add(QStringLiteral("ADDRESS-STATE-PROVINCE"), station.stateProvince);
    add(QStringLiteral("ADDRESS-POSTALCODE"), station.postalCode);
    add(QStringLiteral("ADDRESS-COUNTRY"), station.country);
    add(QStringLiteral("OPERATORS"), operators.join(QLatin1Char(' ')));
    for (const QString &line : textLines(property(QStringLiteral("SOAPBOX")))) {
        add(QStringLiteral("SOAPBOX"), line);
    }
    return lines;
}

QByteArray cabrillo(const Contest &contest, const ContestDefinition *definition,
                    QVector<Qso> qsos, const Station &station,
                    const LogExportOptions &options)
{
    std::stable_sort(qsos.begin(), qsos.end(), [](const Qso &left, const Qso &right) {
        if (left.timestampUtc != right.timestampUtc) {
            return left.timestampUtc < right.timestampUtc;
        }
        return left.id < right.id;
    });
    QStringList lines = cabrilloHeader(contest, definition, qsos, station, options);
    for (const Qso &qso : qsos) {
        const QVector<ExchangeFieldDefinition> *sentOrder =
            definition == nullptr ? nullptr : &definition->sentExchange;
        const QVector<ExchangeFieldDefinition> *receivedOrder =
            definition == nullptr ? nullptr : &definition->receivedExchange;
        const QString sentExchange =
            exportExchange(qso, true, sentOrder);
        const QString receivedExchange =
            exportExchange(qso, false, receivedOrder);
        const QString date = qso.timestampUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd"));
        const QString time = qso.timestampUtc.toUTC().toString(QStringLiteral("hhmm"));
        const QString frequencyKhz = qso.frequencyHz > 0
            ? QString::number(qso.frequencyHz / 1000)
            : bandFrequencyKhz(qso.band);
        QStringList fields{QStringLiteral("QSO:"), frequencyKhz,
                           cabrilloMode(qso.mode), date, time,
                           station.callsign.trimmed().toUpper()};
        if (sentOrder != nullptr
            && std::any_of(sentOrder->cbegin(), sentOrder->cend(),
                           [](const ExchangeFieldDefinition &field) {
                               return field.type == ExchangeFieldType::Rst;
                           })) {
            fields.append(rstValue(qso, true, sentOrder));
        }
        if (!sentExchange.isEmpty()) {
            fields.append(sentExchange);
        }
        fields.append(qso.callsign.trimmed().toUpper());
        if (receivedOrder != nullptr
            && std::any_of(receivedOrder->cbegin(), receivedOrder->cend(),
                           [](const ExchangeFieldDefinition &field) {
                               return field.type == ExchangeFieldType::Rst;
                           })) {
            fields.append(rstValue(qso, false, receivedOrder));
        }
        if (!receivedExchange.isEmpty()) {
            fields.append(receivedExchange);
        }
        lines.append(fields.join(QLatin1Char(' ')));
    }
    lines.append(QStringLiteral("END-OF-LOG:"));
    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}

QByteArray adif(const Contest &contest, const ContestDefinition *definition,
                const QVector<Qso> &qsos,
                const Station &station)
{
    QStringList lines{
        QStringLiteral("Generated by Contest Logger"),
        adifField(QStringLiteral("PROGRAMID"), QStringLiteral("ContestLogger")),
        adifField(QStringLiteral("CONTEST_ID"), exportContestCode(contest, definition)),
        QStringLiteral("<EOH>")
    };
    for (const Qso &qso : qsos) {
        const QDateTime utc = qso.timestampUtc.toUTC();
        QStringList fields{
            adifField(QStringLiteral("QSO_DATE"), utc.toString(QStringLiteral("yyyyMMdd"))),
            adifField(QStringLiteral("TIME_ON"), utc.toString(QStringLiteral("hhmmss"))),
            adifField(QStringLiteral("CALL"), qso.callsign.trimmed().toUpper()),
            adifField(QStringLiteral("BAND"), qso.band.trimmed().toLower()),
            adifField(QStringLiteral("MODE"), qso.mode.trimmed().toUpper()),
            adifField(QStringLiteral("STATION_CALLSIGN"),
                      qso.stationCallsign.trimmed().isEmpty()
                          ? station.callsign.trimmed().toUpper()
                          : qso.stationCallsign.trimmed().toUpper()),
            adifField(QStringLiteral("STX_STRING"),
                      qso.sentExchange.trimmed().isEmpty()
                          ? adifExchange(qso.sentExchangeFields)
                          : qso.sentExchange.trimmed()),
            adifField(QStringLiteral("SRX_STRING"),
                      qso.receivedExchange.trimmed().isEmpty()
                          ? adifExchange(qso.receivedExchangeFields)
                          : qso.receivedExchange.trimmed())
        };
        if (qso.frequencyHz > 0) {
            fields.append(adifField(
                QStringLiteral("FREQ"),
                QString::number(static_cast<double>(qso.frequencyHz) / 1'000'000.0,
                                'f', 6)));
        }
        if (!qso.sentExchangeFields.isEmpty()) {
            fields.append(adifField(QStringLiteral("STX"),
                                    adifExchange(qso.sentExchangeFields)));
        }
        if (!qso.receivedExchangeFields.isEmpty()) {
            fields.append(adifField(QStringLiteral("SRX"),
                                    adifExchange(qso.receivedExchangeFields)));
        }
        fields.append(adifField(QStringLiteral("APP_CONTESTLOGGER_POINTS"),
                                QString::number(qso.points)));
        lines.append(fields.join(QLatin1Char('\n')) + QStringLiteral("\n<EOR>"));
    }
    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}

} // namespace

QMap<QString, QString> LogExporter::mapLegacyExchange(
    const QString &legacy, const QVector<ExchangeFieldDefinition> &order)
{
    const QStringList tokens = legacy.split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QMap<QString, QString> parsed;
    bool keyValueFormat = !tokens.isEmpty();
    for (const QString &token : tokens) {
        const qsizetype separator = token.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            keyValueFormat = false;
            break;
        }
        parsed.insert(token.left(separator).toLower(), token.mid(separator + 1));
    }
    if (keyValueFormat) {
        return parsed;
    }
    parsed.clear();
    const qsizetype count = qMin(tokens.size(), order.size());
    for (qsizetype index = 0; index < count; ++index) {
        parsed.insert(order.at(index).name, tokens.at(index));
    }
    if (order.isEmpty()) {
        for (qsizetype index = 0; index < tokens.size(); ++index) {
            parsed.insert(QStringLiteral("field_%1").arg(index + 1), tokens.at(index));
        }
    }
    return parsed;
}

bool LogExporter::validate(LogExportFormat format, const Contest &contest,
                           const ContestDefinition *definition,
                           const QVector<Qso> &qsos, QString *error)
{
    setError(error, {});
    QVector<Qso> activeQsos;
    std::copy_if(qsos.cbegin(), qsos.cend(), std::back_inserter(activeQsos),
                 [](const Qso &qso) { return !qso.invalid; });
    if (contest.id <= 0 || contest.code.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Contest identity is missing."));
        return false;
    }
    if (activeQsos.isEmpty()) {
        setError(error, QStringLiteral("There are no QSOs to export."));
        return false;
    }
    static const QRegularExpression callsignPattern(
        QStringLiteral("^[A-Z0-9/]{3,20}$"));
    for (qsizetype index = 0; index < activeQsos.size(); ++index) {
        const Qso &qso = activeQsos.at(index);
        const QString prefix = QStringLiteral("QSO %1").arg(index + 1);
        if (qso.contestId != contest.id) {
            setError(error, QStringLiteral("%1 belongs to a different contest.").arg(prefix));
            return false;
        }
        if (!callsignPattern.match(qso.callsign.trimmed().toUpper()).hasMatch()
            || (!qso.stationCallsign.trimmed().isEmpty()
                && !callsignPattern.match(
                    qso.stationCallsign.trimmed().toUpper()).hasMatch())) {
            setError(error, QStringLiteral("%1 has an invalid station or contact callsign.")
                                .arg(prefix));
            return false;
        }
        if (!qso.timestampUtc.isValid()) {
            setError(error, QStringLiteral("%1 has an invalid UTC timestamp.").arg(prefix));
            return false;
        }
        if (qso.frequencyHz < 0 || qso.band.trimmed().isEmpty()) {
            setError(error, QStringLiteral("%1 has an invalid frequency or missing band.")
                                .arg(prefix));
            return false;
        }
        if (qso.mode.trimmed().isEmpty()
            || (format == LogExportFormat::Cabrillo
                && cabrilloMode(qso.mode).isEmpty())) {
            setError(error, QStringLiteral("%1 uses an unsupported Cabrillo mode: %2.")
                                .arg(prefix, qso.mode));
            return false;
        }
        if (definition != nullptr) {
            ContestEvaluationContext context;
            for (qsizetype previous = 0; previous < index; ++previous) {
                context.previousQsos.append(activeQsos.at(previous));
            }
            const QMap<QString, QString> sentValues =
                exchangeValues(qso, true, &definition->sentExchange);
            const QMap<QString, QString> receivedValues =
                exchangeValues(qso, false, &definition->receivedExchange);
            const QString serialValue = sentValues.value(QStringLiteral("serial"));
            bool serialOk = false;
            const int serial = serialValue.toInt(&serialOk);
            const ContestEvaluationResult evaluation = ContestEngine().evaluate(
                *definition, qso.callsign, qso.band, qso.mode,
                receivedValues, context,
                serialOk ? std::optional<int>(serial) : std::nullopt,
                sentValues);
            if (!evaluation.valid || evaluation.duplicate) {
                QStringList reasons;
                for (const ContestValidationIssue &issue : evaluation.issues) {
                    reasons.append(QStringLiteral("%1: %2").arg(issue.field, issue.message));
                }
                setError(error, QStringLiteral("%1 failed contest validation: %2")
                                    .arg(prefix, reasons.join(QStringLiteral("; "))));
                return false;
            }
            for (const ExchangeFieldDefinition &field : definition->sentExchange) {
                if (field.required && field.defaultValue.isEmpty()
                    && !field.generated
                    && sentValues.value(field.name).trimmed().isEmpty()) {
                    setError(error, QStringLiteral("%1 is missing sent exchange field '%2'.")
                                        .arg(prefix, field.name));
                    return false;
                }
            }
        }
    }
    return true;
}

bool LogExporter::writeFile(const QString &filePath, LogExportFormat format,
                            const Contest &contest,
                            const ContestDefinition *definition,
                            const QVector<Qso> &qsos, const Station &station,
                            QString *error)
{
    return writeFile(filePath, format, contest, definition, qsos, station,
                     LogExportOptions{}, error);
}

bool LogExporter::writeFile(const QString &filePath, LogExportFormat format,
                            const Contest &contest,
                            const ContestDefinition *definition,
                            const QVector<Qso> &qsos, const Station &station,
                            const LogExportOptions &options, QString *error)
{
    setError(error, {});
    QVector<Qso> activeQsos;
    std::copy_if(qsos.cbegin(), qsos.cend(), std::back_inserter(activeQsos),
                 [](const Qso &qso) { return !qso.invalid; });
    if (!validate(format, contest, definition, activeQsos, error)) {
        return false;
    }
    if (station.callsign.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Configure the station callsign before exporting."));
        return false;
    }
    if (!QRegularExpression(QStringLiteral("^[A-Z0-9/]{3,20}$"))
             .match(station.callsign.trimmed().toUpper()).hasMatch()) {
        setError(error, QStringLiteral("The configured station callsign is invalid."));
        return false;
    }
    const QByteArray contents = format == LogExportFormat::Cabrillo
        ? cabrillo(contest, definition, activeQsos, station, options)
        : adif(contest, definition, activeQsos, station);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    if (file.write(contents) != contents.size()) {
        setError(error, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

} // namespace logger
