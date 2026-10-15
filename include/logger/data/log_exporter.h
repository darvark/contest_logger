#pragma once

#include "logger/domain/contest_definition.h"
#include "logger/domain/types.h"

#include <QString>
#include <QVector>

namespace logger {

enum class LogExportFormat {
    Cabrillo,
    Adif
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
};

} // namespace logger
