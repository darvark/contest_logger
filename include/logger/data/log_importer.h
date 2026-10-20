#pragma once

#include "logger/contest/contest_engine.h"

namespace logger {

struct ImportedQso {
    Qso qso;
    QMap<QString, QString> fields;
    QStringList cabrilloColumns;
    int record = 0;
};

struct ImportedLog {
    QString contestCode;
    QString stationCallsign;
    QVector<ImportedQso> records;
};

struct LogImportPreview {
    QVector<Qso> qsos;
    int skipped = 0;
};

class LogImporter final {
public:
    static bool readFile(const QString &path, ImportedLog *log, QString *error);
    static bool prepare(const ImportedLog &log, const ContestDefinition &definition,
                        const QString &stationCallsign, const QVector<Qso> &existing,
                        const ContestEngine::MultiplierResolver &resolver,
                        LogImportPreview *preview, QString *error);
};

} // namespace logger
