#include "logger/contest/contest_definition_loader.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <limits>

namespace logger {
namespace {

void clearError(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
}

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool checkKeys(const YAML::Node &node, const QStringList &allowed,
               const QString &path, QString *error)
{
    if (!node.IsMap()) {
        setError(error, QStringLiteral("%1 must be a mapping.").arg(path));
        return false;
    }
    QSet<QString> seen;
    for (const auto &entry : node) {
        if (!entry.first.IsScalar()) {
            setError(error, QStringLiteral("%1 contains a non-scalar key.").arg(path));
            return false;
        }
        const QString key = QString::fromStdString(entry.first.as<std::string>());
        if (seen.contains(key)) {
            setError(error, QStringLiteral("%1 contains duplicate key '%2'.")
                                .arg(path, key));
            return false;
        }
        seen.insert(key);
        if (!allowed.contains(key)) {
            setError(error, QStringLiteral("%1.%2 is not a supported setting.")
                                .arg(path, key));
            return false;
        }
    }
    return true;
}

bool requiredString(const YAML::Node &node, const QString &key,
                    const QString &path, QString &output, QString *error)
{
    const YAML::Node value = node[key.toStdString()];
    if (!value || !value.IsScalar()) {
        setError(error, QStringLiteral("%1.%2 is required and must be a string.")
                            .arg(path, key));
        return false;
    }
    output = QString::fromStdString(value.as<std::string>()).trimmed();
    if (output.isEmpty()) {
        setError(error, QStringLiteral("%1.%2 cannot be empty.").arg(path, key));
        return false;
    }
    return true;
}

bool optionalString(const YAML::Node &node, const QString &key,
                    const QString &path, QString &output, QString *error)
{
    const YAML::Node value = node[key.toStdString()];
    if (!value) {
        return true;
    }
    if (!value.IsScalar()) {
        setError(error, QStringLiteral("%1.%2 must be a string.").arg(path, key));
        return false;
    }
    output = QString::fromStdString(value.as<std::string>()).trimmed();
    return true;
}

bool optionalBool(const YAML::Node &node, const QString &key,
                  const QString &path, bool defaultValue, bool &output,
                  QString *error)
{
    const YAML::Node value = node[key.toStdString()];
    if (!value) {
        output = defaultValue;
        return true;
    }
    if (!value.IsScalar()) {
        setError(error, QStringLiteral("%1.%2 must be true or false.")
                            .arg(path, key));
        return false;
    }
    const QString text = QString::fromStdString(value.as<std::string>()).toLower();
    if (text != QStringLiteral("true") && text != QStringLiteral("false")) {
        setError(error, QStringLiteral("%1.%2 must be true or false.")
                            .arg(path, key));
        return false;
    }
    output = text == QStringLiteral("true");
    return true;
}

bool optionalInt(const YAML::Node &node, const QString &key,
                 const QString &path, int defaultValue, int &output,
                 QString *error)
{
    const YAML::Node value = node[key.toStdString()];
    if (!value) {
        output = defaultValue;
        return true;
    }
    if (!value.IsScalar()) {
        setError(error, QStringLiteral("%1.%2 must be an integer.")
                            .arg(path, key));
        return false;
    }
    bool ok = false;
    const qlonglong parsed =
        QString::fromStdString(value.as<std::string>()).toLongLong(&ok);
    if (!ok || parsed < std::numeric_limits<int>::min()
        || parsed > std::numeric_limits<int>::max()) {
        setError(error, QStringLiteral("%1.%2 must be an integer.")
                            .arg(path, key));
        return false;
    }
    output = static_cast<int>(parsed);
    return true;
}

bool stringList(const YAML::Node &node, const QString &path,
                QStringList &output, QString *error, bool allowEmpty = false)
{
    if (!node || !node.IsSequence()) {
        setError(error, QStringLiteral("%1 must be a list of strings.").arg(path));
        return false;
    }
    QSet<QString> seen;
    for (const YAML::Node &value : node) {
        if (!value.IsScalar()) {
            setError(error, QStringLiteral("%1 must contain only strings.").arg(path));
            return false;
        }
        const QString item =
            QString::fromStdString(value.as<std::string>()).trimmed();
        const QString normalized = item.toUpper();
        if (item.isEmpty() || seen.contains(normalized)) {
            setError(error, QStringLiteral("%1 contains an empty or duplicate value.")
                                .arg(path));
            return false;
        }
        seen.insert(normalized);
        output.append(item);
        if (output.size() > 256) {
            setError(error, QStringLiteral("%1 contains too many values.").arg(path));
            return false;
        }
    }
    if (!allowEmpty && output.isEmpty()) {
        setError(error, QStringLiteral("%1 must contain at least one value.")
                            .arg(path));
        return false;
    }
    return true;
}

bool parseFieldType(const QString &value, ExchangeFieldType &type)
{
    if (value == QStringLiteral("rst")) {
        type = ExchangeFieldType::Rst;
    } else if (value == QStringLiteral("serial")) {
        type = ExchangeFieldType::Serial;
    } else if (value == QStringLiteral("integer")) {
        type = ExchangeFieldType::Integer;
    } else if (value == QStringLiteral("text")) {
        type = ExchangeFieldType::Text;
    } else if (value == QStringLiteral("grid")) {
        type = ExchangeFieldType::Grid;
    } else if (value == QStringLiteral("callsign")) {
        type = ExchangeFieldType::Callsign;
    } else if (value == QStringLiteral("enum")) {
        type = ExchangeFieldType::Enumeration;
    } else {
        return false;
    }
    return true;
}

bool defaultValueIsValid(const ExchangeFieldDefinition &field)
{
    if (field.defaultValue.isEmpty()) {
        return true;
    }
    const QString value = field.defaultValue.trimmed().toUpper();
    if (field.width > 0 && value.size() > field.width) {
        return false;
    }
    if (!field.pattern.isEmpty()) {
        const QRegularExpression pattern(
            QRegularExpression::anchoredPattern(field.pattern));
        return pattern.match(value).hasMatch();
    }

    switch (field.type) {
    case ExchangeFieldType::Rst:
        return QRegularExpression(
                   QStringLiteral("^(?:[1-5][1-9][1-9]|[1-5][1-9])$"))
            .match(value).hasMatch();
    case ExchangeFieldType::Grid:
        return QRegularExpression(
                   QStringLiteral("^[A-R]{2}[0-9]{2}(?:[A-X]{2})?$"))
            .match(value).hasMatch();
    case ExchangeFieldType::Callsign:
        return QRegularExpression(QStringLiteral("^[A-Z0-9/]{3,}$"))
            .match(value).hasMatch();
    case ExchangeFieldType::Integer:
    case ExchangeFieldType::Serial: {
        bool ok = false;
        const qlonglong number = value.toLongLong(&ok);
        return ok && number >= 0
            && (field.minimum == 0 || number >= field.minimum)
            && (field.maximum == 0 || number <= field.maximum);
    }
    case ExchangeFieldType::Text:
        return true;
    case ExchangeFieldType::Enumeration:
        return std::any_of(field.values.cbegin(), field.values.cend(),
                           [&value](const QString &candidate) {
                               return candidate.compare(value, Qt::CaseInsensitive) == 0;
                           });
    }
    return false;
}

bool parseConditionMap(const YAML::Node &node, const QString &path,
                       QMap<QString, QString> &conditions, QString *error)
{
    if (!node.IsMap() || node.size() == 0 || node.size() > 16) {
        setError(error, QStringLiteral("%1 must be a non-empty mapping with at most 16 conditions.")
                            .arg(path));
        return false;
    }
    for (const auto &entry : node) {
        if (!entry.first.IsScalar() || !entry.second.IsScalar()) {
            setError(error, QStringLiteral("%1 keys and values must be strings.")
                                .arg(path));
            return false;
        }
        const QString key =
            QString::fromStdString(entry.first.as<std::string>()).trimmed().toLower();
        const QString value =
            QString::fromStdString(entry.second.as<std::string>()).trimmed();
        if (key.isEmpty() || value.isEmpty() || conditions.contains(key)) {
            setError(error, QStringLiteral("%1 contains an empty or duplicate condition.")
                                .arg(path));
            return false;
        }
        conditions.insert(key, value);
    }
    return true;
}

bool validateConditionKeys(
    const QMap<QString, QString> &conditions,
    const QVector<ExchangeFieldDefinition> &receivedFields, const QString &path,
    QString *error)
{
    const QSet<QString> builtIn{QStringLiteral("band"),
                                QStringLiteral("mode"),
                                QStringLiteral("country"),
                                QStringLiteral("dxcc"),
                                QStringLiteral("continent"),
                                QStringLiteral("cq_zone"),
                                QStringLiteral("itu_zone"),
                                QStringLiteral("station.dxcc"),
                                QStringLiteral("station.country"),
                                QStringLiteral("station.continent"),
                                QStringLiteral("station.cq_zone"),
                                QStringLiteral("station.itu_zone"),
                                QStringLiteral("same_dxcc"),
                                QStringLiteral("same_country"),
                                QStringLiteral("same_continent"),
                                QStringLiteral("same_itu_zone")};
    for (auto it = conditions.cbegin(); it != conditions.cend(); ++it) {
        if (builtIn.contains(it.key())) {
            continue;
        }
        QString fieldName;
        if (it.key().startsWith(QStringLiteral("exchange."))) {
            fieldName = it.key().mid(QStringLiteral("exchange.").size());
        } else if (it.key().startsWith(QStringLiteral("received."))) {
            fieldName = it.key().mid(QStringLiteral("received.").size());
        } else if (it.key().startsWith(QStringLiteral("has_received."))) {
            fieldName = it.key().mid(QStringLiteral("has_received.").size());
        } else {
            setError(error, QStringLiteral("%1 has unsupported condition '%2'.")
                                .arg(path, it.key()));
            return false;
        }
        const bool exists
            = std::any_of(receivedFields.cbegin(), receivedFields.cend(),
                          [&fieldName](const ExchangeFieldDefinition &field) {
                              return field.name == fieldName;
                          });
        if (!exists) {
            setError(error, QStringLiteral(
                                "%1 references unknown received field '%2'.")
                                .arg(path, fieldName));
            return false;
        }
    }
    return true;
}

bool parseFields(const YAML::Node &node, const QString &path, bool isSent,
                 QVector<ExchangeFieldDefinition> &fields, QString *error)
{
    if (!node || !node.IsSequence() || node.size() == 0) {
        setError(error, QStringLiteral("%1 must be a non-empty list.").arg(path));
        return false;
    }
    if (node.size() > 32) {
        setError(error, QStringLiteral("%1 may contain at most 32 fields.").arg(path));
        return false;
    }

    QSet<QString> names;
    for (std::size_t index = 0; index < node.size(); ++index) {
        const YAML::Node item = node[index];
        const QString itemPath = QStringLiteral("%1[%2]").arg(path).arg(index);
        if (!checkKeys(item,
                       {QStringLiteral("name"), QStringLiteral("label"),
                        QStringLiteral("type"), QStringLiteral("required"),
                        QStringLiteral("generated"), QStringLiteral("default"),
                        QStringLiteral("pattern"), QStringLiteral("width"),
                        QStringLiteral("min"), QStringLiteral("max"),
                        QStringLiteral("values"),
                        QStringLiteral("required_when"),
                        QStringLiteral("when")},
                       itemPath, error)) {
            return false;
        }

        ExchangeFieldDefinition field;
        QString typeName;
        if (!requiredString(item, QStringLiteral("name"), itemPath, field.name,
                            error)
            || !requiredString(item, QStringLiteral("type"), itemPath, typeName,
                               error)) {
            return false;
        }
        field.name = field.name.toLower();
        if (!QRegularExpression(QStringLiteral("^[a-z][a-z0-9_]*$"))
                 .match(field.name)
                 .hasMatch()) {
            setError(error,
                     QStringLiteral("%1.name must use lowercase letters, "
                                    "digits, and underscores.")
                         .arg(itemPath));
            return false;
        }
        if (names.contains(field.name)) {
            setError(error, QStringLiteral("%1 contains duplicate field '%2'.")
                                .arg(path, field.name));
            return false;
        }
        names.insert(field.name);

        if (!parseFieldType(typeName.toLower(), field.type)) {
            setError(error, QStringLiteral("%1.type '%2' is not supported.")
                                .arg(itemPath, typeName));
            return false;
        }
        field.label = field.name;
        if (!optionalString(item, QStringLiteral("label"), itemPath,
                            field.label, error)
            || !optionalString(item, QStringLiteral("default"), itemPath,
                               field.defaultValue, error)
            || !optionalString(item, QStringLiteral("pattern"), itemPath,
                               field.pattern, error)
            || !optionalInt(item, QStringLiteral("width"), itemPath, 0,
                            field.width, error)
            || !optionalInt(item, QStringLiteral("min"), itemPath, 0,
                            field.minimum, error)
            || !optionalInt(item, QStringLiteral("max"), itemPath, 0,
                            field.maximum, error)
            || !optionalBool(item, QStringLiteral("required"), itemPath, true,
                             field.required, error)
            || !optionalBool(item, QStringLiteral("generated"), itemPath, false,
                             field.generated, error)) {
            return false;
        }
        if (item["required_when"]) {
            if (isSent) {
                setError(error, QStringLiteral("%1.required_when is only "
                                               "supported for received fields.")
                                    .arg(itemPath));
                return false;
            }
            if (!parseConditionMap(item["required_when"],
                                   itemPath + QStringLiteral(".required_when"),
                                   field.requiredWhen, error)) {
                return false;
            }
            field.hasRequiredWhen = true;
        }
        if (item["when"]) {
            if (!isSent) {
                setError(
                    error,
                    QStringLiteral("%1.when is only supported for sent fields.")
                        .arg(itemPath));
                return false;
            }
            if (!parseConditionMap(item["when"],
                                   itemPath + QStringLiteral(".when"),
                                   field.when, error)) {
                return false;
            }
            const QSet<QString> stationConditions{
                QStringLiteral("station.dxcc"),
                QStringLiteral("station.country"),
                QStringLiteral("station.continent"),
                QStringLiteral("station.cq_zone"),
                QStringLiteral("station.itu_zone")};
            for (auto condition = field.when.cbegin();
                 condition != field.when.cend(); ++condition) {
                if (!stationConditions.contains(condition.key())) {
                    setError(error,
                             QStringLiteral("%1.when only supports station "
                                            "location conditions.")
                                 .arg(itemPath));
                    return false;
                }
            }
            field.hasWhen = true;
        }

        if (field.width < 0 || field.width > 32 || field.minimum < 0
            || field.maximum < 0
            || (field.maximum != 0 && field.minimum > field.maximum)) {
            setError(error, QStringLiteral("%1 has an invalid width or range.")
                                .arg(itemPath));
            return false;
        }
        if (!field.pattern.isEmpty()) {
            const QRegularExpression pattern(field.pattern);
            if (!pattern.isValid()) {
                setError(error, QStringLiteral("%1.pattern is invalid: %2")
                                    .arg(itemPath, pattern.errorString()));
                return false;
            }
        }

        if (item["values"]
            && !stringList(item["values"], itemPath + QStringLiteral(".values"),
                           field.values, error)) {
            return false;
        }
        if (field.type == ExchangeFieldType::Enumeration
            && field.values.isEmpty()) {
            setError(error, QStringLiteral("%1.values is required for enum fields.")
                                .arg(itemPath));
            return false;
        }
        if (field.type != ExchangeFieldType::Enumeration && !field.values.isEmpty()) {
            setError(error, QStringLiteral("%1.values is only valid for enum fields.")
                                .arg(itemPath));
            return false;
        }
        if (field.type == ExchangeFieldType::Serial
            && isSent && !field.generated) {
            setError(error, QStringLiteral(
                "%1 serial fields in sent exchange must be generated.")
                                .arg(itemPath));
            return false;
        }
        if (field.generated
            && (!isSent || field.type != ExchangeFieldType::Serial)) {
            setError(error, QStringLiteral(
                                "%1.generated is only valid for sent serial fields.")
                                .arg(itemPath));
            return false;
        }
        if (!field.defaultValue.isEmpty() && (!isSent || field.generated)) {
            setError(error, QStringLiteral(
                                "%1.default is only valid for non-generated sent fields.")
                                .arg(itemPath));
            return false;
        }
        if (!defaultValueIsValid(field)) {
            setError(error, QStringLiteral("%1.default is invalid for its field definition.")
                                .arg(itemPath));
            return false;
        }
        fields.append(std::move(field));
    }
    return true;
}

bool parseDefinition(const YAML::Node &root, ContestDefinition &definition,
                     QString *error)
{
    if (!checkKeys(root,
                   {QStringLiteral("schema_version"), QStringLiteral("id"),
                    QStringLiteral("name"), QStringLiteral("description"),
                    QStringLiteral("bands"), QStringLiteral("modes"),
                    QStringLiteral("exchange"), QStringLiteral("scoring"),
                    QStringLiteral("dupe_key"), QStringLiteral("cabrillo")},
                   QStringLiteral("contest"), error)) {
        return false;
    }

    int schemaVersion = 1;
    if (!optionalInt(root, QStringLiteral("schema_version"),
                     QStringLiteral("contest"), 1, schemaVersion, error)
        || schemaVersion != 1) {
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("contest.schema_version must be 1.");
        }
        return false;
    }
    definition.schemaVersion = schemaVersion;
    if (!requiredString(root, QStringLiteral("id"), QStringLiteral("contest"),
                        definition.id, error)
        || !requiredString(root, QStringLiteral("name"), QStringLiteral("contest"),
                           definition.name, error)
        || !optionalString(root, QStringLiteral("description"),
                           QStringLiteral("contest"), definition.description, error)
        || !stringList(root["bands"], QStringLiteral("contest.bands"),
                       definition.bands, error)
        || !stringList(root["modes"], QStringLiteral("contest.modes"),
                       definition.modes, error)) {
        return false;
    }

    definition.id = definition.id.toLower();
    if (!QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9_-]*$"))
             .match(definition.id).hasMatch()) {
        setError(error, QStringLiteral(
                            "contest.id must use lowercase letters, digits, '_' or '-'."));
        return false;
    }
    for (QString &mode : definition.modes) {
        mode = mode.toUpper();
    }
    for (QString &band : definition.bands) {
        band = band.toLower();
    }

    const YAML::Node cabrillo = root["cabrillo"];
    if (cabrillo) {
        if (!checkKeys(cabrillo, {QStringLiteral("contest")},
                       QStringLiteral("contest.cabrillo"), error)
            || !optionalString(cabrillo, QStringLiteral("contest"),
                               QStringLiteral("contest.cabrillo"),
                               definition.cabrilloContest, error)) {
            return false;
        }
        definition.cabrilloContest = definition.cabrilloContest.trimmed().toUpper();
        if (!definition.cabrilloContest.isEmpty()
            && !QRegularExpression(QStringLiteral("^[A-Z0-9][A-Z0-9_-]*$"))
                    .match(definition.cabrilloContest).hasMatch()) {
            setError(error, QStringLiteral(
                                "contest.cabrillo.contest must use letters, digits, '_' or '-'."));
            return false;
        }
    }

    const YAML::Node exchange = root["exchange"];
    if (!exchange || !checkKeys(exchange,
                                {QStringLiteral("sent"), QStringLiteral("received")},
                                QStringLiteral("contest.exchange"), error)
        || !parseFields(exchange["sent"], QStringLiteral("contest.exchange.sent"),
                        true, definition.sentExchange, error)
        || !parseFields(exchange["received"],
                        QStringLiteral("contest.exchange.received"),
                        false, definition.receivedExchange, error)) {
        return false;
    }
    for (const ExchangeFieldDefinition &field : definition.receivedExchange) {
        if (field.hasRequiredWhen
            && !validateConditionKeys(
                field.requiredWhen, definition.receivedExchange,
                QStringLiteral("contest.exchange.received.%1.required_when")
                    .arg(field.name),
                error)) {
            return false;
        }
    }

    const YAML::Node scoring = root["scoring"];
    if (!scoring || !checkKeys(scoring,
                               {QStringLiteral("qso_points"),
                                QStringLiteral("serial_start"),
                                QStringLiteral("serial_scope"),
                                QStringLiteral("serial_server_required"),
                                QStringLiteral("multipliers"),
                                QStringLiteral("qso_point_rules")},
                               QStringLiteral("contest.scoring"), error)
        || !optionalInt(scoring, QStringLiteral("qso_points"),
                        QStringLiteral("contest.scoring"), 1,
                        definition.qsoPoints, error)
        || !optionalInt(scoring, QStringLiteral("serial_start"),
                        QStringLiteral("contest.scoring"), 1,
                        definition.serialStart, error)
        || !optionalString(scoring, QStringLiteral("serial_scope"),
                           QStringLiteral("contest.scoring"),
                           definition.serialScope, error)
        || !optionalBool(scoring, QStringLiteral("serial_server_required"),
                         QStringLiteral("contest.scoring"), false,
                         definition.serialServerRequired, error)) {
        return false;
    }
    if (definition.qsoPoints < 0 || definition.serialStart < 1) {
        setError(error, QStringLiteral(
                            "contest.scoring.qso_points must be non-negative and serial_start must be positive."));
        return false;
    }
    static const QStringList serialScopes{
        QStringLiteral("global"), QStringLiteral("band"),
        QStringLiteral("mode"), QStringLiteral("band_mode")
    };
    if (!serialScopes.contains(definition.serialScope)) {
        setError(error, QStringLiteral(
            "contest.scoring.serial_scope must be global, band, mode or band_mode."));
        return false;
    }
    const bool hasGeneratedSerial = std::any_of(
        definition.sentExchange.cbegin(), definition.sentExchange.cend(),
        [](const ExchangeFieldDefinition &field) {
            return field.type == ExchangeFieldType::Serial && field.generated;
        });
    if (definition.serialServerRequired && !hasGeneratedSerial) {
        setError(error, QStringLiteral(
            "contest.scoring.serial_server_required requires a generated sent serial field."));
        return false;
    }

    const YAML::Node pointRules = scoring["qso_point_rules"];
    if (pointRules) {
        if (!pointRules.IsSequence() || pointRules.size() > 64) {
            setError(error, QStringLiteral(
                                "contest.scoring.qso_point_rules must be a list of at most 64 rules."));
            return false;
        }
        for (std::size_t index = 0; index < pointRules.size(); ++index) {
            const YAML::Node item = pointRules[index];
            const QString itemPath =
                QStringLiteral("contest.scoring.qso_point_rules[%1]").arg(index);
            if (!checkKeys(item, {QStringLiteral("points"), QStringLiteral("when")},
                           itemPath, error)) {
                return false;
            }
            QsoPointsRule rule;
            if (!optionalInt(item, QStringLiteral("points"), itemPath, -1, rule.points, error)
                || rule.points < 0
                || !parseConditionMap(item["when"], itemPath + QStringLiteral(".when"), rule.when,
                                      error)
                || !validateConditionKeys(rule.when, definition.receivedExchange,
                                          itemPath + QStringLiteral(".when"), error)) {
                if (error != nullptr && error->isEmpty()) {
                    setError(error,
                             QStringLiteral("%1.points must be non-negative.").arg(itemPath));
                }
                return false;
            }
            definition.qsoPointRules.append(std::move(rule));
        }
    }

    const YAML::Node multipliers = scoring["multipliers"];
    if (multipliers) {
        if (!multipliers.IsSequence()) {
            setError(error, QStringLiteral("contest.scoring.multipliers must be a list."));
            return false;
        }
        QSet<QString> multiplierNames;
        for (std::size_t index = 0; index < multipliers.size(); ++index) {
            const YAML::Node item = multipliers[index];
            const QString itemPath
                = QStringLiteral("contest.scoring.multipliers[%1]").arg(index);
            if (!checkKeys(item,
                           {QStringLiteral("type"), QStringLiteral("field"),
                            QStringLiteral("per_band"),
                            QStringLiteral("per_mode"),
                            QStringLiteral("exclude")},
                           itemPath, error)) {
                return false;
            }
            MultiplierDefinition multiplier;
            if (!requiredString(item, QStringLiteral("type"), itemPath,
                                multiplier.type, error)) {
                return false;
            }
            multiplier.type = multiplier.type.toLower();
            if (multiplier.type == QStringLiteral("exchange")
                || multiplier.type == QStringLiteral("exchange_prefix")) {
                if (!requiredString(item, QStringLiteral("field"), itemPath,
                                    multiplier.field, error)) {
                    return false;
                }
                multiplier.field = multiplier.field.toLower();
                const bool fieldExists = std::any_of(
                    definition.receivedExchange.cbegin(),
                    definition.receivedExchange.cend(),
                    [&multiplier](const ExchangeFieldDefinition &field) {
                        return field.name == multiplier.field;
                    });
                if (!fieldExists) {
                    setError(error,
                             QStringLiteral(
                                 "%1 references unknown received field '%2'.")
                                 .arg(itemPath, multiplier.field));
                    return false;
                }
            } else if (multiplier.type != QStringLiteral("prefix")
                       && multiplier.type != QStringLiteral("dxcc")
                       && multiplier.type != QStringLiteral("country")) {
                setError(error,
                         QStringLiteral("%1.type must be 'exchange', 'prefix', "
                                        "'exchange_prefix', 'dxcc', or 'country'.")
                             .arg(itemPath));
                return false;
            } else if (item["field"]) {
                setError(error,
                         QStringLiteral(
                             "%1.field is only valid for exchange multipliers.")
                             .arg(itemPath));
                return false;
            }
            if (!optionalBool(item, QStringLiteral("per_band"), itemPath, false,
                              multiplier.perBand, error)
                || !optionalBool(item, QStringLiteral("per_mode"), itemPath,
                                 false, multiplier.perMode, error)) {
                return false;
            }
            if (item["exclude"]) {
                if (multiplier.type != QStringLiteral("dxcc")
                    && multiplier.type != QStringLiteral("country")) {
                    setError(error,
                             QStringLiteral("%1.exclude is only supported for "
                                            "DXCC or country multipliers.")
                                 .arg(itemPath));
                    return false;
                }
                if (!stringList(item["exclude"],
                                itemPath + QStringLiteral(".exclude"),
                                multiplier.excludedValues, error)
                    || multiplier.excludedValues.isEmpty()) {
                    if (error != nullptr && error->isEmpty()) {
                        setError(error, QStringLiteral("%1.exclude must not be empty.")
                                            .arg(itemPath));
                    }
                    return false;
                }
            }
            const QString key
                = multiplier.type + QLatin1Char(':') + multiplier.field;
            if (multiplierNames.contains(key)) {
                setError(error,
                         QStringLiteral("%1 is duplicated.").arg(itemPath));
                return false;
            }
            multiplierNames.insert(key);
            definition.multipliers.append(std::move(multiplier));
        }
    }

    if (root["dupe_key"]) {
        QStringList keys;
        if (!stringList(root["dupe_key"], QStringLiteral("contest.dupe_key"),
                        keys, error)) {
            return false;
        }
        for (QString &key : keys) {
            key = key.toLower();
            if (key != QStringLiteral("callsign") && key != QStringLiteral("band")
                && key != QStringLiteral("mode")) {
                setError(error, QStringLiteral(
                                    "contest.dupe_key supports only callsign, band, and mode."));
                return false;
            }
        }
        if (!keys.contains(QStringLiteral("callsign"))) {
            setError(error, QStringLiteral("contest.dupe_key must include callsign."));
            return false;
        }
        definition.dupeKey = keys;
    }

    return true;
}

} // namespace

bool ContestDefinitionLoader::loadFile(const QString &filePath,
                                       ContestDefinition &definition,
                                       QString *error) const
{
    clearError(error);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error, QStringLiteral("%1: %2").arg(filePath, file.errorString()));
        return false;
    }
    constexpr qint64 MaximumDefinitionSize = 1024 * 1024;
    if (file.size() > MaximumDefinitionSize) {
        setError(error, QStringLiteral("%1: contest definition exceeds 1 MiB.")
                            .arg(filePath));
        return false;
    }
    return loadText(QString::fromUtf8(file.readAll()), filePath, definition, error);
}

bool ContestDefinitionLoader::loadText(const QString &yaml,
                                       const QString &sourceName,
                                       ContestDefinition &definition,
                                       QString *error) const
{
    clearError(error);
    constexpr qsizetype MaximumDefinitionSize = 1024 * 1024;
    if (yaml.toUtf8().size() > MaximumDefinitionSize) {
        setError(error, QStringLiteral("%1: contest definition exceeds 1 MiB.")
                            .arg(sourceName));
        return false;
    }
    try {
        const YAML::Node root = YAML::Load(yaml.toStdString());
        ContestDefinition candidate;
        if (!parseDefinition(root, candidate, error)) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2").arg(sourceName, *error);
            }
            return false;
        }
        definition = std::move(candidate);
        return true;
    } catch (const YAML::ParserException &exception) {
        setError(error, QStringLiteral("%1:%2:%3: YAML syntax error: %4")
                            .arg(sourceName)
                            .arg(exception.mark.line + 1)
                            .arg(exception.mark.column + 1)
                            .arg(QString::fromUtf8(exception.what())));
    } catch (const YAML::Exception &exception) {
        setError(error, QStringLiteral("%1: invalid YAML value: %2")
                            .arg(sourceName, QString::fromUtf8(exception.what())));
    }
    return false;
}

} // namespace logger
