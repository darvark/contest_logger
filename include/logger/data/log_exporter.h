#pragma once

#include "logger/domain/contest_definition.h"
#include "logger/domain/types.h"

#include <QString>
#include <QVector>

#include <optional>

namespace logger {

enum class LogExportFormat {
    Cabrillo,
    Adif
};

struct LogExportOptions {
    // Score shown as CLAIMED-SCORE; when empty it is computed from the definition.
    std::optional<qint64> claimedScore;
    QString createdBy;
};

class LogExporter final {
public:
    static QMap<QString, QString> mapLegacyExchange(
        const QString &legacy, const QVector<ExchangeFieldDefinition> &order = {});
    static bool validate(LogExportFormat format, const Contest &contest,
                         const ContestDefinition *definition,
                         const QVector<Qso> &qsos,
                         QString *error = nullptr);
    static bool writeFile(const QString &filePath, LogExportFormat format,
                          const Contest &contest,
                          const ContestDefinition *definition,
                          const QVector<Qso> &qsos, const Station &station,
                          QString *error = nullptr);
    static bool writeFile(const QString &filePath, LogExportFormat format,
                          const Contest &contest,
                          const ContestDefinition *definition,
                          const QVector<Qso> &qsos, const Station &station,
                          const LogExportOptions &options,
                          QString *error = nullptr);
};

} // namespace logger
