#include "logger/data/support_data_parser.h"

#include <QRegularExpression>
#include <QSet>

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool validCallsign(const QString &callsign)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Z0-9/]{3,20}$"));
    return pattern.match(callsign).hasMatch();
}

QStringList linesWithoutBom(const QString &contents)
{
    QString text = contents;
    if (!text.isEmpty() && text.front() == QChar::ByteOrderMark) {
        text.remove(0, 1);
    }
    return text.split(QLatin1Char('\n'));
}

QString normalizedHeader(QString header)
{
    header = header.trimmed().toLower();
    if (header == QStringLiteral("call") || header == QStringLiteral("callsign")
        || header == QStringLiteral("callsign/locator")) {
        return QStringLiteral("callsign");
    }
    if (header == QStringLiteral("rst") || header == QStringLiteral("rs")
        || header == QStringLiteral("report")) {
        return QStringLiteral("report");
    }
    header.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                   QStringLiteral("_"));
    if (header == QStringLiteral("state_province")
        || header == QStringLiteral("province")) {
        return QStringLiteral("state");
    }
    if (header == QStringLiteral("grid_square")) {
        return QStringLiteral("grid");
    }
    if (header == QStringLiteral("serial_number")) {
        return QStringLiteral("serial");
    }
    while (header.startsWith(QLatin1Char('_'))) {
        header.remove(0, 1);
    }
    while (header.endsWith(QLatin1Char('_'))) {
        header.chop(1);
    }
    return header;
}

QStringList splitDelimitedLine(const QString &line, QChar delimiter)
{
    QStringList cells;
    QString cell;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (quoted && i + 1 < line.size()
                && line.at(i + 1) == QLatin1Char('"')) {
                cell.append(ch);
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == delimiter && !quoted) {
            cells.append(cell.trimmed());
            cell.clear();
        } else {
            cell.append(ch);
        }
    }
    cells.append(cell.trimmed());
    return cells;
}

bool parseCtyRecord(const QString &record, QVector<CtyPrefixEntry> &output)
{
    const qsizetype initialSize = output.size();
    const QStringList columns = record.split(QLatin1Char(':'));
    if (columns.size() < 9) {
        return false;
    }
    bool cqOk = false;
    bool ituOk = false;
    const int cqZone = columns.at(1).trimmed().toInt(&cqOk);
    const int ituZone = columns.at(2).trimmed().toInt(&ituOk);
    const QString entity = columns.at(0).trimmed();
    const QString continent = columns.at(3).trimmed().toUpper();
    if (entity.isEmpty() || !cqOk || !ituOk || cqZone < 1 || cqZone > 40
        || ituZone < 1 || ituZone > 90
        || !QRegularExpression(QStringLiteral("^[A-Z]{2}$"))
                .match(continent).hasMatch()) {
        return false;
    }

    const QString prefixText = columns.mid(8).join(QLatin1Char(':'));
    const QStringList aliases = prefixText.split(QLatin1Char(','),
                                                  Qt::SkipEmptyParts);
    for (QString alias : aliases) {
        alias = alias.trimmed();
        const bool exactCallsign = alias.startsWith(QLatin1Char('='));
        while (alias.startsWith(QLatin1Char('*'))
               || alias.startsWith(QLatin1Char('='))) {
            alias.remove(0, 1);
        }
        alias = alias.toUpper();
        if (QRegularExpression(QStringLiteral("^[A-Z0-9]{1,8}$"))
                .match(alias).hasMatch()) {
            output.append({alias, entity, continent, cqZone, ituZone,
                           exactCallsign});
        }
    }
    return output.size() > initialSize;
}

} // namespace

bool SupportDataParser::parseCallHistory(
    const QString &contents, QHash<QString, CallHistoryEntry> &entries,
    QString *error)
{
    entries.clear();
    const QStringList lines = linesWithoutBom(contents);
    QStringList headers;
    bool hasHeader = false;
    for (qsizetype lineNumber = 0; lineNumber < lines.size(); ++lineNumber) {
        const QString line = lines.at(lineNumber).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char(';'))) {
            continue;
        }

        QChar delimiter;
        if (line.contains(QLatin1Char(','))) {
            delimiter = QLatin1Char(',');
        } else if (line.contains(QLatin1Char('\t'))) {
            delimiter = QLatin1Char('\t');
        } else if (line.contains(QLatin1Char(';'))) {
            delimiter = QLatin1Char(';');
        } else {
            delimiter = QLatin1Char(' ');
        }
        QStringList cells = delimiter == QLatin1Char(' ')
            ? line.split(QRegularExpression(QStringLiteral("\\s+")),
                         Qt::SkipEmptyParts)
            : splitDelimitedLine(line, delimiter);
        if (cells.isEmpty()) {
            continue;
        }

        if (!hasHeader && (cells.first().compare(QStringLiteral("call"),
                                                  Qt::CaseInsensitive) == 0
                           || cells.first().compare(QStringLiteral("callsign"),
                                                    Qt::CaseInsensitive) == 0)) {
            hasHeader = true;
            for (const QString &cell : cells) {
                headers.append(normalizedHeader(cell));
            }
            continue;
        }

        const QString callsign = cells.first().trimmed().toUpper();
        if (!validCallsign(callsign)) {
            setError(error, QStringLiteral("Call history line %1 has an invalid callsign.")
                                .arg(lineNumber + 1));
            entries.clear();
            return false;
        }
        if (!hasHeader) {
            headers = {QStringLiteral("callsign"), QStringLiteral("name"),
                       QStringLiteral("qth"), QStringLiteral("exchange")};
        }
        CallHistoryEntry entry;
        entry.callsign = callsign;
        for (qsizetype column = 1; column < cells.size(); ++column) {
            const QString key = column < headers.size()
                ? headers.at(column) : QStringLiteral("field_%1").arg(column);
            if (!key.isEmpty()) {
                entry.fields.insert(key, cells.at(column).trimmed());
            }
        }
        entries.insert(callsign, entry);
    }
    if (entries.isEmpty()) {
        setError(error, QStringLiteral("Call history contains no usable records."));
        return false;
    }
    setError(error, {});
    return true;
}

bool SupportDataParser::parseMasterCpy(const QString &contents,
                                       QStringList &callsigns, QString *error)
{
    callsigns.clear();
    QSet<QString> seen;
    const QStringList lines = linesWithoutBom(contents);
    bool firstDataLine = true;
    for (qsizetype lineNumber = 0; lineNumber < lines.size(); ++lineNumber) {
        const QString line = lines.at(lineNumber).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char(';'))
            || line.startsWith(QLatin1Char('!'))) {
            continue;
        }
        const QString token = line.section(QRegularExpression(QStringLiteral("\\s+")),
                                           0, 0).toUpper();
        if (firstDataLine
            && (token == QStringLiteral("CALL")
                || token == QStringLiteral("CALLSIGN"))) {
            firstDataLine = false;
            continue;
        }
        firstDataLine = false;
        if (!validCallsign(token)) {
            setError(error, QStringLiteral("master.cpy line %1 has an invalid callsign.")
                                .arg(lineNumber + 1));
            callsigns.clear();
            return false;
        }
        if (!seen.contains(token)) {
            seen.insert(token);
            callsigns.append(token);
        }
    }
    if (callsigns.isEmpty()) {
        setError(error, QStringLiteral("master.cpy contains no callsigns."));
        return false;
    }
    setError(error, {});
    return true;
}

bool SupportDataParser::parseCtyDat(const QString &contents,
                                    QVector<CtyPrefixEntry> &prefixes,
                                    QString *error)
{
    prefixes.clear();
    QString pending;
    const QStringList lines = linesWithoutBom(contents);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (!pending.isEmpty()) {
            pending.append(QLatin1Char(' '));
        }
        pending.append(trimmed);
        while (pending.contains(QLatin1Char(';'))) {
            const qsizetype end = pending.indexOf(QLatin1Char(';'));
            const QString record = pending.left(end).trimmed();
            if (!parseCtyRecord(record, prefixes)) {
                setError(error, QStringLiteral("wl_cty.dat contains a malformed entity record."));
                prefixes.clear();
                return false;
            }
            pending = pending.mid(end + 1).trimmed();
        }
    }
    if (!pending.trimmed().isEmpty()) {
        setError(error, QStringLiteral("wl_cty.dat ends with an unterminated entity record."));
        prefixes.clear();
        return false;
    }
    if (prefixes.isEmpty()) {
        setError(error, QStringLiteral("wl_cty.dat contains no usable prefixes."));
        return false;
    }
    setError(error, {});
    return true;
}


QMap<QString, QString> SupportDataParser::resolveCtyMultipliers(
    const QString &rawCallsign, const QVector<CtyPrefixEntry> &prefixes)
{
    const QString callsign = rawCallsign.trimmed().toUpper();
    const QStringList parts = callsign.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString base = callsign;
    for (const QString &part : parts) {
        if (part.contains(QRegularExpression(QStringLiteral("[A-Z]")))
            && part.contains(QRegularExpression(QStringLiteral("[0-9]")))) {
            base = part;
            break;
        }
    }
    const CtyPrefixEntry *match = nullptr;
    for (const CtyPrefixEntry &entry : prefixes) {
        if (entry.exactCallsign
            && (entry.prefix.compare(callsign, Qt::CaseInsensitive) == 0
                || entry.prefix.compare(base, Qt::CaseInsensitive) == 0)) {
            match = &entry;
            break;
        }
    }
    if (match == nullptr) {
        qsizetype longest = -1;
        for (const CtyPrefixEntry &entry : prefixes) {
            if (entry.exactCallsign || entry.prefix.size() <= longest) {
                continue;
            }
            if (base.startsWith(entry.prefix, Qt::CaseInsensitive)) {
                match = &entry;
                longest = entry.prefix.size();
            }
        }
    }
    if (match == nullptr) {
        return {};
    }
    const QString cqZone = QString::number(match->cqZone);
    const QString ituZone = QString::number(match->ituZone);
    return {{QStringLiteral("dxcc"), match->entity},
            {QStringLiteral("primary_prefix"), match->prefix},
            {QStringLiteral("dxcc_prefix"), match->prefix},
            {QStringLiteral("cq_zone"), cqZone},
            {QStringLiteral("itu_zone"), ituZone},
            {QStringLiteral("continent"), match->continent}};
}

} // namespace logger
