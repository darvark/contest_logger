#include "logger/contest/contest_engine.h"
#include "logger/data/log_exporter.h"

#include <QRegularExpression>

#include <algorithm>

namespace logger {
namespace {

void addIssue(ContestEvaluationResult &result, const QString &field,
              const QString &message)
{
    result.issues.append({field, message});
}

QString normalized(const QString &value)
{
    return value.trimmed().toUpper();
}

QMap<QString, QString> conditionFacts(const QString &band, const QString &mode,
                                      const QMap<QString, QString> &exchange,
                                      const QMap<QString, QString> &resolved)
{
    QMap<QString, QString> facts;
    for (auto it = resolved.cbegin(); it != resolved.cend(); ++it) {
        facts.insert(it.key().toLower(), normalized(it.value()));
    }
    if (facts.contains(QStringLiteral("station.dxcc"))) {
        facts.insert(QStringLiteral("station.country"),
                     facts.value(QStringLiteral("station.dxcc")));
    }
    facts.insert(QStringLiteral("band"), band.trimmed().toLower());
    facts.insert(QStringLiteral("mode"), normalized(mode));
    for (auto it = exchange.cbegin(); it != exchange.cend(); ++it) {
        const QString key = it.key().toLower();
        const QString value = normalized(it.value());
        facts.insert(QStringLiteral("exchange.%1").arg(key), value);
        facts.insert(QStringLiteral("received.%1").arg(key), value);
    }
    if (facts.contains(QStringLiteral("dxcc"))) {
        facts.insert(QStringLiteral("country"), facts.value(QStringLiteral("dxcc")));
    }
    for (const QString &key : {QStringLiteral("same_dxcc"),
                               QStringLiteral("same_country"),
                               QStringLiteral("same_continent")}) {
        if (facts.contains(key)) {
            facts.insert(key, normalized(facts.value(key)));
        }
    }
    return facts;
}

bool conditionMatches(const QMap<QString, QString> &condition,
                      const QMap<QString, QString> &facts)
{
    for (auto it = condition.cbegin(); it != condition.cend(); ++it) {
        if (normalized(facts.value(it.key())) != normalized(it.value())) {
            return false;
        }
    }
    return true;
}

QMap<QString, QString> receivedFieldsFor(
    const Qso &qso, const ContestDefinition &definition);

int pointsForQso(const ContestDefinition &definition, const Qso &qso,
                 const QMap<QString, QString> &resolved)
{
    const QMap<QString, QString> exchange =
        receivedFieldsFor(qso, definition);
    const auto facts = conditionFacts(qso.band, qso.mode, exchange, resolved);
    for (const QsoPointsRule &rule : definition.qsoPointRules) {
        if (conditionMatches(rule.when, facts)) {
            return rule.points;
        }
    }
    return definition.qsoPoints;
}

QString expectedPattern(const ExchangeFieldDefinition &field)
{
    if (!field.pattern.isEmpty()) {
        return field.pattern;
    }
    switch (field.type) {
    case ExchangeFieldType::Rst:
        return QStringLiteral("^(?:[1-5][1-9][1-9]|[1-5][1-9])$");
    case ExchangeFieldType::Grid:
        return QStringLiteral("^[A-R]{2}[0-9]{2}(?:[A-X]{2})?$");
    case ExchangeFieldType::Callsign:
        return QStringLiteral("^[A-Z0-9/]{3,}$");
    case ExchangeFieldType::Text:
    case ExchangeFieldType::Serial:
    case ExchangeFieldType::Integer:
    case ExchangeFieldType::Enumeration:
        return {};
    }
    return {};
}

bool validateField(const ExchangeFieldDefinition &field, const QString &rawValue,
                   QString &value, QString &issue)
{
    value = normalized(rawValue);
    if (value.isEmpty()) {
        if (field.required) {
            issue = QStringLiteral("is required");
            return false;
        }
        return true;
    }

    const QString pattern = expectedPattern(field);
    if (!pattern.isEmpty()
        && !QRegularExpression(QRegularExpression::anchoredPattern(pattern))
                .match(value).hasMatch()) {
        issue = QStringLiteral("does not match the required format");
        return false;
    }

    if (field.type == ExchangeFieldType::Integer
        || field.type == ExchangeFieldType::Serial) {
        bool ok = false;
        const qlonglong number = value.toLongLong(&ok);
        if (!ok || number < 0
            || (field.minimum > 0 && number < field.minimum)
            || (field.maximum > 0 && number > field.maximum)) {
            issue = QStringLiteral("must be an integer in the allowed range");
            return false;
        }
    }

    if (field.type == ExchangeFieldType::Enumeration) {
        const bool found = std::any_of(
            field.values.cbegin(), field.values.cend(),
            [&value](const QString &candidate) {
                return candidate.compare(value, Qt::CaseInsensitive) == 0;
            });
        if (!found) {
            issue = QStringLiteral("must be one of: %1").arg(field.values.join(", "));
            return false;
        }
    }

    if (field.width > 0 && value.size() > field.width) {
        issue = QStringLiteral("must be no longer than %1 characters").arg(field.width);
        return false;
    }
    return true;
}

QString multiplierValue(const MultiplierDefinition &multiplier,
                        const QMap<QString, QString> &exchange,
                        const ContestEvaluationContext &context,
                        const QString &callsign)
{
    if (multiplier.type == QStringLiteral("exchange")) {
        return normalized(exchange.value(multiplier.field));
    }
    if (multiplier.type == QStringLiteral("prefix")) {
        return ContestEngine::wpxPrefix(callsign);
    }
    return normalized(context.resolvedMultiplierValues.value(multiplier.type));
}

QString multiplierKey(const MultiplierDefinition &multiplier, const QString &value,
                      const QString &band, const QString &mode)
{
    QString key = multiplier.type == QStringLiteral("exchange")
        ? multiplier.type + QLatin1Char(':') + multiplier.field
            + QLatin1Char(':') + value
        : multiplier.type + QLatin1Char(':') + value;
    if (multiplier.perBand) {
        key += QLatin1Char('@') + band.trimmed().toLower();
    }
    if (multiplier.perMode) {
        key += QLatin1Char('@') + mode.trimmed().toUpper();
    }
    return key;
}

QMap<QString, QString> receivedFieldsFor(const Qso &qso,
                                         const ContestDefinition &definition)
{
    if (!qso.receivedExchangeFields.isEmpty()) {
        return qso.receivedExchangeFields;
    }
    return LogExporter::mapLegacyExchange(qso.receivedExchange,
                                          definition.receivedExchange);
}

} // namespace


bool ContestEngine::isValidCallsign(const QString &callsign)
{
    return QRegularExpression(QStringLiteral("^[A-Z0-9/]{3,}$"))
        .match(normalized(callsign)).hasMatch();
}

bool ContestEngine::sentFieldApplies(
    const ExchangeFieldDefinition &field,
    const QMap<QString, QString> &resolvedValues)
{
    return !field.hasWhen
        || conditionMatches(field.when, conditionFacts({}, {}, {}, resolvedValues));
}

QString ContestEngine::wpxPrefix(const QString &rawCallsign)
{
    const QString callsign = normalized(rawCallsign);
    if (!isValidCallsign(callsign)) {
        return {};
    }
    const QStringList parts = callsign.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString base;
    QString numericOverride;
    for (const QString &part : parts) {
        if (part.size() == 1 && part.at(0).isDigit()) {
            numericOverride = part;
            continue;
        }
        if (base.isEmpty()
            && part.contains(QRegularExpression(QStringLiteral("[A-Z]")))
            && part.contains(QRegularExpression(QStringLiteral("[0-9]")))) {
            base = part;
        }
    }
    if (base.isEmpty()) {
        for (const QString &part : parts) {
            if (part.contains(QRegularExpression(QStringLiteral("[A-Z]")))) {
                base = part;
                break;
            }
        }
    }
    if (base.isEmpty()) {
        return {};
    }
    const qsizetype firstDigit = base.indexOf(QRegularExpression(QStringLiteral("[0-9]")));
    QString prefix;
    if (firstDigit < 0) {
        prefix = base + QLatin1Char('0');
    } else {
        prefix = base.left(firstDigit + 1);
    }
    if (!numericOverride.isEmpty()) {
        const qsizetype digit = prefix.indexOf(QRegularExpression(QStringLiteral("[0-9]")));
        if (digit >= 0) {
            prefix = prefix.left(digit) + numericOverride;
        } else {
            prefix += numericOverride;
        }
    }
    return prefix;
}

QStringList ContestEngine::multiplierKeys(
    const ContestDefinition &definition, const Qso &qso,
    const MultiplierResolver &resolver) const
{
    QStringList keys;
    const QMap<QString, QString> resolved = resolver ? resolver(qso)
                                                      : QMap<QString, QString>{};
    const QMap<QString, QString> exchange = receivedFieldsFor(qso, definition);
    for (const MultiplierDefinition &multiplier : definition.multipliers) {
        const QString value = multiplierValue(multiplier, exchange,
                                              ContestEvaluationContext{.resolvedMultiplierValues = resolved},
                                              qso.callsign);
        if (!value.isEmpty()) {
            keys.append(multiplierKey(multiplier, value, qso.band, qso.mode));
        }
    }
    return keys;
}

ContestEvaluationContext ContestEngine::buildContext(
    const ContestDefinition &definition, const QVector<Qso> &previousQsos,
    const MultiplierResolver &resolver) const
{
    ContestEvaluationContext context;
    context.previousQsos = previousQsos;
    for (const Qso &qso : previousQsos) {
        for (const QString &key : multiplierKeys(definition, qso, resolver)) {
            context.workedMultiplierKeys.insert(key);
        }
    }
    return context;
}

int ContestEngine::nextSerialNumber(const ContestDefinition &definition,
                                    const QVector<Qso> &qsos) const
{
    QString serialField;
    for (const ExchangeFieldDefinition &field : definition.sentExchange) {
        if (field.type == ExchangeFieldType::Serial && field.generated) {
            serialField = field.name;
            break;
        }
    }
    int maximum = definition.serialStart - 1;
    for (const Qso &qso : qsos) {
        QMap<QString, QString> fields = qso.sentExchangeFields;
        if (fields.isEmpty()) {
            fields = LogExporter::mapLegacyExchange(qso.sentExchange,
                                                    definition.sentExchange);
        }
        QString value;
        if (!serialField.isEmpty()) {
            for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
                if (it.key().compare(serialField, Qt::CaseInsensitive) == 0) {
                    value = it.value().trimmed();
                    break;
                }
            }
        }
        bool ok = false;
        const int serial = value.toInt(&ok);
        if (ok && serial > 0) {
            maximum = qMax(maximum, serial);
        }
    }
    // Follow stored serials rather than QSO count; gaps and edited serials are intentional.
    return qMax(definition.serialStart, maximum + 1);
}

ContestScore ContestEngine::score(const ContestDefinition &definition,
                                  const QVector<Qso> &qsos,
                                  const MultiplierResolver &resolver) const
{
    ContestScore result;
    QVector<Qso> ordered = qsos;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Qso &left, const Qso &right) {
        if (left.timestampUtc != right.timestampUtc) {
            return left.timestampUtc < right.timestampUtc;
        }
        return left.id < right.id;
    });
    QSet<QString> worked;
    for (const Qso &qso : ordered) {
        ++result.totalQsos;
        ++result.perBand[qso.band.trimmed().toLower()].qsos;
        ++result.perMode[qso.mode.trimmed().toUpper()].qsos;
        const QMap<QString, QString> resolved = resolver ? resolver(qso)
                                                          : QMap<QString, QString>{};
        const int points = qso.points > 0
            ? qso.points : pointsForQso(definition, qso, resolved);
        result.totalPoints += points;
        result.perBand[qso.band.trimmed().toLower()].points += points;
        result.perMode[qso.mode.trimmed().toUpper()].points += points;
        const MultiplierResolver currentResolver = [&resolved](const Qso &) {
            return resolved;
        };
        const QStringList keys = multiplierKeys(definition, qso, currentResolver);
        ContestEvaluationContext context;
        context.resolvedMultiplierValues = resolved;
        const QMap<QString, QString> exchange = receivedFieldsFor(qso, definition);
        result.newMultipliersByQsoId.insert(qso.id, 0);
        for (qsizetype index = 0; index < definition.multipliers.size(); ++index) {
            const MultiplierDefinition &multiplier = definition.multipliers.at(index);
            const QString value = multiplierValue(multiplier, exchange, context,
                                                  qso.callsign);
            if (value.isEmpty()) {
                const QString unresolved = multiplier.type + QLatin1Char(':')
                    + qso.callsign.trimmed().toUpper();
                if (!result.unresolvedMultiplierKeys.contains(unresolved)) {
                    result.unresolvedMultiplierKeys.append(unresolved);
                }
                continue;
            }
            const QString key = multiplierKey(multiplier, value, qso.band, qso.mode);
            if (!keys.contains(key) || worked.contains(key)) {
                continue;
            }
            worked.insert(key);
            ++result.newMultipliersByQsoId[qso.id];
            ++result.totalMultipliers;
            ++result.multiplierCounts[multiplier.type];
            ContestBandScore &bandScore = result.perBand[qso.band.trimmed().toLower()];
            ++bandScore.multipliers;
            ++bandScore.multiplierCounts[multiplier.type];
            ContestBandScore &modeScore = result.perMode[qso.mode.trimmed().toUpper()];
            ++modeScore.multipliers;
            ++modeScore.multiplierCounts[multiplier.type];
        }
    }
    result.finalScore = result.totalPoints
        * (definition.multipliers.isEmpty() ? 1 : result.totalMultipliers);
    return result;
}

ContestEvaluationResult ContestEngine::evaluate(
    const ContestDefinition &definition, const QString &callsign,
    const QString &band, const QString &mode,
    const QMap<QString, QString> &receivedExchange,
    const ContestEvaluationContext &context,
    std::optional<int> requestedSerialNumber,
    const QMap<QString, QString> &sentExchangeOverrides) const
{
    ContestEvaluationResult result;
    const QString normalizedCall = normalized(callsign);
    const QString normalizedBand = band.trimmed().toLower();
    const QString normalizedMode = normalized(mode);

    if (!isValidCallsign(normalizedCall)) {
        addIssue(result, QStringLiteral("callsign"),
                 QStringLiteral("must be a valid callsign"));
    }
    if (std::none_of(definition.bands.cbegin(), definition.bands.cend(),
                     [&normalizedBand](const QString &allowed) {
                         return allowed.compare(normalizedBand, Qt::CaseInsensitive) == 0;
                     })) {
        addIssue(result, QStringLiteral("band"),
                 QStringLiteral("is not allowed by this contest"));
    }
    if (std::none_of(definition.modes.cbegin(), definition.modes.cend(),
                     [&normalizedMode](const QString &allowed) {
                         return allowed.compare(normalizedMode, Qt::CaseInsensitive) == 0;
                     })) {
        addIssue(result, QStringLiteral("mode"),
                 QStringLiteral("is not allowed by this contest"));
    }

    QMap<QString, QString> normalizedReceived;
    for (auto it = receivedExchange.cbegin(); it != receivedExchange.cend(); ++it) {
        const QString key = it.key().trimmed().toLower();
        if (normalizedReceived.contains(key)) {
            addIssue(result, QStringLiteral("received.%1").arg(key),
                     QStringLiteral("was provided more than once"));
            continue;
        }
        normalizedReceived.insert(key, it.value());
    }
    QMap<QString, QString> normalizedSentOverrides;
    for (auto it = sentExchangeOverrides.cbegin();
         it != sentExchangeOverrides.cend(); ++it) {
        const QString key = it.key().trimmed().toLower();
        if (normalizedSentOverrides.contains(key)) {
            addIssue(result, QStringLiteral("sent.%1").arg(key),
                     QStringLiteral("was provided more than once"));
            continue;
        }
        normalizedSentOverrides.insert(key, it.value());
    }
    for (auto it = normalizedSentOverrides.cbegin();
         it != normalizedSentOverrides.cend(); ++it) {
        const bool knownField = std::any_of(
            definition.sentExchange.cbegin(), definition.sentExchange.cend(),
            [&it](const ExchangeFieldDefinition &field) {
                return field.name == it.key();
            });
        if (!knownField) {
            addIssue(result, QStringLiteral("sent.%1").arg(it.key()),
                     QStringLiteral("is not defined by this contest"));
        }
    }
    for (auto it = normalizedReceived.cbegin(); it != normalizedReceived.cend(); ++it) {
        const bool knownField = std::any_of(
            definition.receivedExchange.cbegin(),
            definition.receivedExchange.cend(),
            [&it](const ExchangeFieldDefinition &field) {
                return field.name == it.key();
            });
        if (!knownField) {
            addIssue(result, QStringLiteral("received.%1").arg(it.key()),
                     QStringLiteral("is not defined by this contest"));
        }
    }

    for (const Qso &previous : context.previousQsos) {
        const bool sameCall =
            normalized(previous.callsign) == normalizedCall;
        const bool sameBand = normalized(previous.band).toLower() == normalizedBand;
        const bool sameMode = normalized(previous.mode) == normalizedMode;
        bool duplicate = sameCall;
        for (const QString &key : definition.dupeKey) {
            if (key == QStringLiteral("band")) {
                duplicate = duplicate && sameBand;
            } else if (key == QStringLiteral("mode")) {
                duplicate = duplicate && sameMode;
            }
        }
        if (duplicate) {
            result.duplicate = true;
            addIssue(result, QStringLiteral("callsign"),
                     QStringLiteral("is a duplicate for the configured dupe key"));
            break;
        }
    }

    int serial = requestedSerialNumber.value_or(
        nextSerialNumber(definition, context.previousQsos));
    if (serial < definition.serialStart) {
        addIssue(result, QStringLiteral("sent.serial"),
                 QStringLiteral("must not be lower than the configured starting serial"));
    }
    result.serialNumber = serial;

    const auto fieldConditionFacts = conditionFacts(
        normalizedBand, normalizedMode, normalizedReceived,
        context.resolvedMultiplierValues);

    for (const ExchangeFieldDefinition &field : definition.sentExchange) {
        if (field.hasWhen) {
            bool locationUnresolved = false;
            for (auto condition = field.when.cbegin(); condition != field.when.cend();
                 ++condition) {
                if (!fieldConditionFacts.contains(condition.key())) {
                    addIssue(result, QStringLiteral("sent.%1").arg(field.name),
                             QStringLiteral("requires station CTY data to select the exchange"));
                    locationUnresolved = true;
                    break;
                }
            }
            if (locationUnresolved) {
                continue;
            }
        }
        if (!sentFieldApplies(field, context.resolvedMultiplierValues)) {
            continue;
        }
        QString value = field.defaultValue;
        if (field.type == ExchangeFieldType::Serial && field.generated) {
            value = QString::number(serial);
            if (field.width > 0) {
                value = value.rightJustified(field.width, QLatin1Char('0'));
            }
        } else if (normalizedSentOverrides.contains(field.name)) {
            value = normalizedSentOverrides.value(field.name);
        }
        QString issue;
        QString checkedValue;
        if (!validateField(field, value, checkedValue, issue)) {
            addIssue(result, QStringLiteral("sent.%1").arg(field.name), issue);
        } else if (!checkedValue.isEmpty()) {
            result.sentExchange.insert(field.name, checkedValue);
        }
    }

    for (const ExchangeFieldDefinition &field : definition.receivedExchange) {
        const QString rawValue = normalizedReceived.value(field.name);
        ExchangeFieldDefinition validationField = field;
        if (field.hasRequiredWhen) {
            validationField.required = conditionMatches(field.requiredWhen,
                                                        fieldConditionFacts);
        }
        QString issue;
        QString checkedValue;
        if (!validateField(validationField, rawValue, checkedValue, issue)) {
            addIssue(result, QStringLiteral("received.%1").arg(field.name), issue);
        } else if (!checkedValue.isEmpty()) {
            result.receivedExchange.insert(field.name, checkedValue);
        }
    }

    result.valid = result.issues.isEmpty();
    if (!result.valid) {
        return result;
    }

    const auto facts = conditionFacts(normalizedBand, normalizedMode,
                                      result.receivedExchange,
                                      context.resolvedMultiplierValues);
    result.points = definition.qsoPoints;
    for (const QsoPointsRule &rule : definition.qsoPointRules) {
        if (conditionMatches(rule.when, facts)) {
            result.points = rule.points;
            break;
        }
    }
    for (const MultiplierDefinition &multiplier : definition.multipliers) {
        const QString value = multiplierValue(
            multiplier, result.receivedExchange, context, normalizedCall);
        if (value.isEmpty()) {
            result.warnings.append(
                QStringLiteral("Multiplier '%1' could not be resolved.")
                    .arg(multiplier.type));
            continue;
        }
        const QString key = multiplierKey(multiplier, value, normalizedBand, normalizedMode);
        if (!context.workedMultiplierKeys.contains(key)) {
            result.newMultiplierKeys.append(key);
        }
    }
    return result;
}

} // namespace logger
