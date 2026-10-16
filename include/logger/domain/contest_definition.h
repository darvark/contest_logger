#pragma once

#include "logger/domain/types.h"

#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>

namespace logger {

enum class ExchangeFieldType {
    Rst,
    Serial,
    Integer,
    Text,
    Grid,
    Callsign,
    Enumeration
};

struct ExchangeFieldDefinition {
    QString name;
    QString label;
    ExchangeFieldType type = ExchangeFieldType::Text;
    bool required = true;
    bool generated = false;
    QString defaultValue;
    QString pattern;
    int width = 0;
    int minimum = 0;
    int maximum = 0;
    QStringList values;
    QMap<QString, QString> requiredWhen;
    bool hasRequiredWhen = false;
};

struct MultiplierDefinition {
    QString type;
    QString field;
    bool perBand = false;
};

struct QsoPointsRule {
    int points = 0;
    QMap<QString, QString> when;
};

struct ContestDefinition {
    int schemaVersion = 1;
    QString id;
    QString name;
    QString description;
    // Official Cabrillo CONTEST: identifier; empty means the uppercased id.
    QString cabrilloContest;
    QStringList bands;
    QStringList modes;
    QVector<ExchangeFieldDefinition> sentExchange;
    QVector<ExchangeFieldDefinition> receivedExchange;
    int serialStart = 1;
    int qsoPoints = 1;
    QVector<QsoPointsRule> qsoPointRules;
    QVector<MultiplierDefinition> multipliers;
    QStringList dupeKey{QStringLiteral("callsign"),
                        QStringLiteral("band"),
                        QStringLiteral("mode")};
};

struct ContestValidationIssue {
    QString field;
    QString message;
};

struct ContestEvaluationContext {
    QVector<Qso> previousQsos;
    QSet<QString> workedMultiplierKeys;
    QMap<QString, QString> resolvedMultiplierValues;
};

struct ContestBandScore {
    int qsos = 0;
    int points = 0;
    int multipliers = 0;
    QMap<QString, int> multiplierCounts;
};

struct ContestScore {
    int totalQsos = 0;
    int totalPoints = 0;
    int totalMultipliers = 0;
    int finalScore = 0;
    QMap<QString, int> multiplierCounts;
    QMap<QString, ContestBandScore> perBand;
    QStringList unresolvedMultiplierKeys;
    QHash<qint64, int> newMultipliersByQsoId;
};

struct ContestEvaluationResult {
    bool valid = false;
    bool duplicate = false;
    int serialNumber = 0;
    int points = 0;
    QMap<QString, QString> sentExchange;
    QMap<QString, QString> receivedExchange;
    QStringList newMultiplierKeys;
    QStringList warnings;
    QVector<ContestValidationIssue> issues;
};

} // namespace logger
