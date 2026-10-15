#include "logger/data/log_exporter.h"

#include "logger/contest/contest_engine.h"

#include <QRegularExpression>
#include <QSaveFile>

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

QByteArray cabrillo(const Contest &contest, const ContestDefinition *definition,
                    const QVector<Qso> &qsos, const Station &station)
{
    QStringList lines{
        QStringLiteral("START-OF-LOG: 3.0"),
        QStringLiteral("CALLSIGN: %1").arg(station.callsign.trimmed().toUpper()),
        QStringLiteral("CONTEST: %1").arg(exportContestCode(contest, definition))
    };
    if (!station.operatorName.trimmed().isEmpty()) {
        lines.append(QStringLiteral("OPERATORS: %1").arg(station.operatorName.trimmed()));
    }
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
            : QString(qso.band).remove(QLatin1Char('m'), Qt::CaseInsensitive);
        lines.append(QStringLiteral("QSO: %1 %2 %3 %4 %5 %6 %7 %8 %9 %10")
                         .arg(frequencyKhz, cabrilloMode(qso.mode), date, time,
                              station.callsign.trimmed().toUpper(),
                              rstValue(qso, true, sentOrder), sentExchange,
                              qso.callsign.trimmed().toUpper(),
                              rstValue(qso, false, receivedOrder), receivedExchange));
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
    if (contest.id <= 0 || contest.code.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Contest identity is missing."));
        return false;
    }
    if (qsos.isEmpty()) {
        setError(error, QStringLiteral("There are no QSOs to export."));
        return false;
    }
    static const QRegularExpression callsignPattern(
        QStringLiteral("^[A-Z0-9/]{3,20}$"));
    for (qsizetype index = 0; index < qsos.size(); ++index) {
        const Qso &qso = qsos.at(index);
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
                context.previousQsos.append(qsos.at(previous));
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
    setError(error, {});
    if (!validate(format, contest, definition, qsos, error)) {
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
        ? cabrillo(contest, definition, qsos, station)
        : adif(contest, definition, qsos, station);
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
