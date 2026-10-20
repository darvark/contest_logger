#include "logger/data/log_importer.h"

#include "logger/data/log_exporter.h"
#include "logger/radio/band_plan.h"

#include <QFile>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace logger {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

QString normalizedMode(QString mode)
{
    mode = mode.trimmed().toUpper();
    if (mode == QLatin1String("PH") || mode == QLatin1String("USB")
        || mode == QLatin1String("LSB")) {
        return QStringLiteral("SSB");
    }
    if (mode == QLatin1String("RY")) {
        return QStringLiteral("RTTY");
    }
    if (mode == QLatin1String("DG")) {
        return QStringLiteral("DATA");
    }
    if (mode == QLatin1String("CWR")) {
        return QStringLiteral("CW");
    }
    return mode;
}

QDateTime utcTimestamp(const QString &date, const QString &time, bool cabrillo)
{
    const QDate day = QDate::fromString(
        date, cabrillo ? QStringLiteral("yyyy-MM-dd") : QStringLiteral("yyyyMMdd"));
    const QTime clock = QTime::fromString(
        time, time.size() == 4 ? QStringLiteral("HHmm") : QStringLiteral("HHmmss"));
    return QDateTime(day, clock, QTimeZone::utc());
}

bool frequency(const QString &text, double scale, qint64 *hz)
{
    bool ok = false;
    const double value = text.toDouble(&ok) * scale;
    if (!ok || !std::isfinite(value) || value <= 0 || value > 1e12) {
        return false;
    }
    *hz = qRound64(value);
    return true;
}

bool readAdif(const QByteArray &bytes, ImportedLog *log, QString *error)
{
    QMap<QString, QString> fields;
    int record = 1;
    bool headerEnded = false;
    qsizetype position = 0;
    while (position < bytes.size()) {
        const qsizetype open = bytes.indexOf('<', position);
        if (open < 0) {
            break;
        }
        const qsizetype close = bytes.indexOf('>', open + 1);
        if (close < 0) {
            return fail(error, QStringLiteral("ADIF record %1: unterminated tag.").arg(record));
        }
        const QByteArray tag = bytes.mid(open + 1, close - open - 1).trimmed().toUpper();
        position = close + 1;
        if (tag == "EOH") {
            if (headerEnded || !log->records.isEmpty()) {
                return fail(error, QStringLiteral("ADIF: unexpected EOH."));
            }
            log->contestCode = fields.value(QStringLiteral("CONTEST_ID"));
            fields.clear();
            headerEnded = true;
            continue;
        }
        if (tag == "EOR") {
            ImportedQso imported;
            imported.fields = fields;
            imported.record = record++;
            Qso &qso = imported.qso;
            qso.callsign = fields.value(QStringLiteral("CALL")).trimmed().toUpper();
            qso.stationCallsign = fields.value(QStringLiteral("STATION_CALLSIGN"))
                                      .trimmed().toUpper();
            qso.timestampUtc = utcTimestamp(fields.value(QStringLiteral("QSO_DATE")),
                                           fields.value(QStringLiteral("TIME_ON")), false);
            qso.mode = normalizedMode(fields.value(QStringLiteral("MODE")));
            qso.band = fields.value(QStringLiteral("BAND")).trimmed().toLower();
            if (fields.contains(QStringLiteral("FREQ"))) {
                if (!frequency(fields.value(QStringLiteral("FREQ")), 1e6, &qso.frequencyHz)) {
                    return fail(error, QStringLiteral("ADIF record %1: invalid FREQ.")
                                           .arg(imported.record));
                }
                const QString frequencyBand = bandForFrequency(qso.frequencyHz);
                if (qso.band.isEmpty()) {
                    qso.band = frequencyBand;
                } else if (!frequencyBand.isEmpty() && qso.band != frequencyBand) {
                    return fail(error, QStringLiteral("ADIF record %1: BAND and FREQ disagree.")
                                           .arg(imported.record));
                }
            }
            if (log->stationCallsign.isEmpty()) {
                log->stationCallsign = qso.stationCallsign;
            }
            if (log->contestCode.isEmpty()) {
                log->contestCode = fields.value(QStringLiteral("CONTEST_ID"));
            }
            log->records.append(imported);
            fields.clear();
            continue;
        }
        const QList<QByteArray> parts = tag.split(':');
        bool ok = false;
        const qlonglong length = parts.size() >= 2 ? parts[1].toLongLong(&ok) : -1;
        if (!ok || length < 0 || length > bytes.size() - position
            || parts.size() > 3 || parts.first().isEmpty()) {
            return fail(error, QStringLiteral("ADIF record %1: invalid field length/tag.")
                                   .arg(record));
        }
        const QString key = QString::fromLatin1(parts.first());
        if (fields.contains(key)) {
            return fail(error, QStringLiteral("ADIF record %1: repeated field %2.")
                                   .arg(record).arg(key));
        }
        fields.insert(key, QString::fromUtf8(bytes.mid(position, length)));
        position += length;
    }
    if (!fields.isEmpty()) {
        return fail(error, QStringLiteral("ADIF: last record has no EOR."));
    }
    return true;
}

bool readCabrillo(const QByteArray &bytes, ImportedLog *log, QString *error)
{
    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); ++index) {
        const QString line = lines[index].trimmed();
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            if (!line.isEmpty()) {
                return fail(error, QStringLiteral("Cabrillo line %1: missing tag.").arg(index + 1));
            }
            continue;
        }
        const QString tag = line.left(colon).toUpper();
        const QString value = line.mid(colon + 1).trimmed();
        if (tag == QLatin1String("CONTEST")) {
            log->contestCode = value;
        } else if (tag == QLatin1String("CALLSIGN")) {
            log->stationCallsign = value.toUpper();
        } else if (tag == QLatin1String("QSO")) {
            ImportedQso imported;
            imported.record = index + 1;
            imported.cabrilloColumns = value.split(
                QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            const auto &columns = imported.cabrilloColumns;
            if (columns.size() < 7) {
                return fail(error, QStringLiteral("Cabrillo line %1: incomplete QSO.")
                                       .arg(index + 1));
            }
            QString freq = columns[0].toUpper();
            double scale = 1000;
            if (freq.endsWith(QLatin1Char('G'))) {
                freq.chop(1);
                scale = 1e9;
            } else if (freq == QLatin1String("50") || freq == QLatin1String("70")
                       || freq == QLatin1String("144") || freq == QLatin1String("432")) {
                scale = 1e6;
            }
            if (!frequency(freq, scale, &imported.qso.frequencyHz)) {
                return fail(error, QStringLiteral("Cabrillo line %1: invalid frequency.")
                                       .arg(index + 1));
            }
            imported.qso.band = bandForFrequency(imported.qso.frequencyHz);
            imported.qso.mode = normalizedMode(columns[1]);
            imported.qso.timestampUtc = utcTimestamp(columns[2], columns[3], true);
            imported.qso.stationCallsign = columns[4].toUpper();
            log->records.append(imported);
        }
    }
    return true;
}

bool mapExchange(QString text, const QVector<ExchangeFieldDefinition> &order,
                 bool cabrillo, QMap<QString, QString> *fields)
{
    QStringList tokens = text.split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const bool hasRst = std::any_of(order.cbegin(), order.cend(),
        [](const ExchangeFieldDefinition &field) { return field.type == ExchangeFieldType::Rst; });
    if (cabrillo && !hasRst && !tokens.isEmpty()
        && QRegularExpression(QStringLiteral("^[1-5][1-9][1-9]?$")).match(tokens.first()).hasMatch()) {
        tokens.removeFirst();
    }
    const bool keyed = text.contains(QLatin1Char('='));
    if (!keyed && tokens.size() > order.size()) {
        return false;
    }
    *fields = LogExporter::mapLegacyExchange(tokens.join(QLatin1Char(' ')), order);
    return true;
}

bool adifExchange(const QMap<QString, QString> &adif,
                  const QVector<ExchangeFieldDefinition> &order, bool sent,
                  QMap<QString, QString> *fields)
{
    const QString prefix = sent ? QStringLiteral("STX") : QStringLiteral("SRX");
    QString text = adif.value(prefix + QStringLiteral("_STRING"));
    const QString rst = adif.value(sent ? QStringLiteral("RST_SENT") : QStringLiteral("RST_RCVD"));
    QVector<ExchangeFieldDefinition> textOrder = order;
    if (!rst.isEmpty() && !text.contains(QLatin1Char('='))
        && text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size()
            < order.size()) {
        textOrder.erase(std::remove_if(textOrder.begin(), textOrder.end(),
            [](const ExchangeFieldDefinition &field) {
                return field.type == ExchangeFieldType::Rst;
            }), textOrder.end());
    }
    if (!text.isEmpty() && !mapExchange(text, textOrder, false, fields)) {
        return false;
    }
    const QString serial = adif.value(prefix);
    for (const ExchangeFieldDefinition &field : order) {
        QString value;
        if (field.type == ExchangeFieldType::Rst) {
            value = rst;
        } else if (field.type == ExchangeFieldType::Serial
                   || field.name == QLatin1String("serial")) {
            if (QRegularExpression(QStringLiteral("^\\d+$")).match(serial).hasMatch()) {
                value = serial;
            }
        } else if (!sent) {
            const QMap<QString, QString> tags{
                {QStringLiteral("zone"), QStringLiteral("CQZ")},
                {QStringLiteral("cqzone"), QStringLiteral("CQZ")},
                {QStringLiteral("cq_zone"), QStringLiteral("CQZ")},
                {QStringLiteral("ituzone"), QStringLiteral("ITUZ")},
                {QStringLiteral("itu_zone"), QStringLiteral("ITUZ")},
                {QStringLiteral("state"), QStringLiteral("STATE")},
                {QStringLiteral("section"), QStringLiteral("ARRL_SECT")},
                {QStringLiteral("grid"), QStringLiteral("GRIDSQUARE")},
                {QStringLiteral("name"), QStringLiteral("NAME")}
            };
            const QString tag = tags.value(field.name, field.name.toUpper());
            value = adif.value(tag);
        }
        if (!value.isEmpty() && !fields->contains(field.name)) {
            fields->insert(field.name, value);
        }
    }
    return true;
}

QString identity(const Qso &qso)
{
    return QStringLiteral("%1|%2|%3|%4").arg(qso.timestampUtc.toUTC().toString(Qt::ISODate),
        qso.callsign.trimmed().toUpper(), qso.band.trimmed().toLower(),
        qso.mode.trimmed().toUpper());
}

} // namespace

bool LogImporter::readFile(const QString &path, ImportedLog *log, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, file.errorString());
    }
    QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return fail(error, file.errorString());
    }
    ImportedLog parsed;
    if (bytes.startsWith(QByteArray::fromHex("efbbbf"))) {
        bytes.remove(0, 3);
    }
    const bool cabrillo = bytes.trimmed().toUpper().startsWith("START-OF-LOG:");
    if (!(cabrillo ? readCabrillo(bytes, &parsed, error) : readAdif(bytes, &parsed, error))) {
        return false;
    }
    if (parsed.records.isEmpty()) {
        return fail(error, QStringLiteral("No QSO records found (expected ADIF or Cabrillo)."));
    }
    *log = std::move(parsed);
    return true;
}

bool LogImporter::prepare(const ImportedLog &log, const ContestDefinition &definition,
                          const QString &stationCallsign, const QVector<Qso> &existing,
                          const ContestEngine::MultiplierResolver &resolver,
                          LogImportPreview *preview, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    LogImportPreview result;
    QVector<Qso> previous = existing;
    QSet<QString> seen;
    for (const Qso &qso : existing) {
        seen.insert(identity(qso));
    }
    auto records = log.records;
    std::stable_sort(records.begin(), records.end(), [](const ImportedQso &a, const ImportedQso &b) {
        return a.qso.timestampUtc < b.qso.timestampUtc;
    });
    const ContestEngine engine;
    for (const ImportedQso &record : records) {
        Qso qso = record.qso;
        const QString location = QStringLiteral("Record/line %1: ").arg(record.record);
        if (!record.cabrilloColumns.isEmpty()) {
            const auto &columns = record.cabrilloColumns;
            int matches = 0;
            for (int call = 5; call < columns.size(); ++call) {
                const QString candidate = columns[call].toUpper();
                if (!ContestEngine::isValidCallsign(candidate)
                    || !candidate.contains(QRegularExpression(QStringLiteral("[A-Z]")))
                    || !candidate.contains(QRegularExpression(QStringLiteral("\\d")))) {
                    continue;
                }
                QMap<QString, QString> sent, received;
                QStringList receivedTokens = columns.mid(call + 1);
                if (!mapExchange(columns.mid(5, call - 5).join(QLatin1Char(' ')),
                                 definition.sentExchange, true, &sent)) {
                    continue;
                }
                if (!mapExchange(receivedTokens.join(QLatin1Char(' ')),
                                 definition.receivedExchange, true, &received)) {
                    if (receivedTokens.isEmpty()
                        || (receivedTokens.last() != QLatin1String("0")
                            && receivedTokens.last() != QLatin1String("1"))) {
                        continue;
                    }
                    receivedTokens.removeLast();
                    if (!mapExchange(receivedTokens.join(QLatin1Char(' ')),
                                     definition.receivedExchange, true, &received)) {
                        continue;
                    }
                }
                ++matches;
                qso.callsign = candidate;
                qso.sentExchangeFields = sent;
                qso.receivedExchangeFields = received;
            }
            if (matches != 1) {
                return fail(error, location + QStringLiteral(
                    "Cabrillo exchange layout does not match the selected contest or is ambiguous."));
            }
        } else if (!adifExchange(record.fields, definition.sentExchange, true,
                                 &qso.sentExchangeFields)
                   || !adifExchange(record.fields, definition.receivedExchange, false,
                                    &qso.receivedExchangeFields)) {
            return fail(error, location + QStringLiteral("Exchange has too many fields."));
        }
        if (!qso.timestampUtc.isValid() || qso.band.isEmpty() || qso.mode.isEmpty()
            || !ContestEngine::isValidCallsign(qso.callsign)) {
            return fail(error, location + QStringLiteral("Missing/invalid date, time, call, band or mode."));
        }
        if (qso.stationCallsign.isEmpty()) {
            qso.stationCallsign = stationCallsign.trimmed().toUpper();
        }
        if (!ContestEngine::isValidCallsign(qso.stationCallsign)
            || qso.stationCallsign.compare(stationCallsign.trimmed(), Qt::CaseInsensitive) != 0) {
            return fail(error, location + QStringLiteral("Station callsign does not match the destination log."));
        }
        if (seen.contains(identity(qso))) {
            ++result.skipped;
            continue;
        }
        auto context = engine.buildContext(definition, previous, resolver);
        if (resolver) {
            context.resolvedMultiplierValues = resolver(qso);
        }
        std::optional<int> serial;
        for (const auto &field : definition.sentExchange) {
            if (!ContestEngine::sentFieldApplies(field, context.resolvedMultiplierValues)) {
                continue;
            }
            if (field.generated && field.type == ExchangeFieldType::Serial) {
                bool ok = false;
                const int value = qso.sentExchangeFields.value(field.name).toInt(&ok);
                if (!ok) {
                    return fail(error, location + QStringLiteral("Missing/invalid sent serial number."));
                }
                serial = value;
            } else if (field.required && !qso.sentExchangeFields.contains(field.name)) {
                return fail(error, location + QStringLiteral("Missing sent exchange field: %1.").arg(field.name));
            }
        }
        const auto evaluation = engine.evaluate(definition, qso.callsign, qso.band, qso.mode,
            qso.receivedExchangeFields, context, serial, qso.sentExchangeFields, true);
        if (!evaluation.valid) {
            QStringList issues;
            for (const auto &issue : evaluation.issues) {
                issues.append(issue.field + QStringLiteral(": ") + issue.message);
            }
            return fail(error, location + issues.join(QStringLiteral("; ")));
        }
        qso.sentExchangeFields = evaluation.sentExchange;
        qso.receivedExchangeFields = evaluation.receivedExchange;
        auto exchangeText = [](const QMap<QString, QString> &fields) {
            QStringList values;
            for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
                values.append(it.key() + QLatin1Char('=') + it.value());
            }
            return values.join(QLatin1Char(' '));
        };
        qso.sentExchange = exchangeText(qso.sentExchangeFields);
        qso.receivedExchange = exchangeText(qso.receivedExchangeFields);
        qso.points = evaluation.duplicate ? 0 : evaluation.points;
        seen.insert(identity(qso));
        previous.append(qso);
        result.qsos.append(qso);
    }
    *preview = std::move(result);
    return true;
}

} // namespace logger
