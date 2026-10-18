#pragma once

#include "logger/domain/contest_definition.h"

namespace logger {

class ContestEngine final {
public:
    using MultiplierResolver = std::function<QMap<QString, QString>(const Qso &)>;

    static bool isValidCallsign(const QString &callsign);
    static QString wpxPrefix(const QString &callsign);
    static bool sentFieldApplies(
        const ExchangeFieldDefinition &field,
        const QMap<QString, QString> &resolvedValues);
    QStringList multiplierKeys(
        const ContestDefinition &definition, const Qso &qso,
        const MultiplierResolver &resolver = {}) const;
    ContestEvaluationContext buildContext(
        const ContestDefinition &definition, const QVector<Qso> &previousQsos,
        const MultiplierResolver &resolver = {}) const;
    ContestScore score(const ContestDefinition &definition,
                       const QVector<Qso> &qsos,
                       const MultiplierResolver &resolver = {}) const;
    int nextSerialNumber(const ContestDefinition &definition,
                         const QVector<Qso> &qsos) const;

    ContestEvaluationResult evaluate(
        const ContestDefinition &definition,
        const QString &callsign,
        const QString &band,
        const QString &mode,
        const QMap<QString, QString> &receivedExchange,
        const ContestEvaluationContext &context = {},
        std::optional<int> serialNumber = std::nullopt,
        const QMap<QString, QString> &sentExchangeOverrides = {},
        bool allowDuplicate = false) const;
};

} // namespace logger
