#pragma once

#include "logger/domain/contest_definition.h"
#include "logger/domain/types.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <functional>

namespace logger {

class SqliteRepository final {
public:
    explicit SqliteRepository(QSqlDatabase database);

    bool initialize(QString *error = nullptr);
    bool ensureGeneralLog(QString *error = nullptr);
    QVector<Contest> contests(QString *error = nullptr) const;
    QStringList contestDefinitionCodes(QString *error = nullptr) const;
    QString contestDefinitionYaml(qint64 contestId, QString *error = nullptr) const;
    bool saveContestDefinition(const ContestDefinition &definition,
                               const QString &yaml, QString *error = nullptr);
    // Always inserts a new log; the code gets a numeric suffix when the id is taken.
    qint64 createContestLog(const ContestDefinition &definition, const QString &yaml,
                            const QString &name, QString *error = nullptr);
    // Uses name, stationCallsign, startsAtUtc and properties from log.
    qint64 createContestLog(const ContestDefinition &definition, const QString &yaml,
                            const Contest &log, QString *error = nullptr);
    // Updates name, stationCallsign, startsAtUtc and properties of an existing log.
    bool updateContestLog(const Contest &log, QString *error = nullptr);
    bool saveQso(Qso &qso, QString *error = nullptr);
    bool updateQso(const Qso &qso, QString *error = nullptr);
    std::optional<Qso> qsoById(qint64 id, QString *error = nullptr) const;
    QVector<Qso> qsosForContest(qint64 contestId, int limit = 0,
                                QString *error = nullptr) const;
    QVector<Qso> recentQsos(qint64 contestId, int limit = 100,
                            QString *error = nullptr) const;
    bool saveSpot(Spot &spot, QString *error = nullptr);
    bool pruneSpots(const QDateTime &olderThanUtc, int maxRows,
                    QString *error = nullptr);
    std::optional<qint64> findDuplicate(
        qint64 contestId, const QString &callsign, const QString &band,
        const QString &mode, qint64 excludeQsoId = 0,
        QString *error = nullptr) const;
    QVector<Spot> recentSpots(int limit = 500, QString *error = nullptr) const;
    bool backupTo(const QString &filePath, QString *error = nullptr) const;
    static bool restoreFromBackup(const QString &databasePath,
                                  const QString &backupPath,
                                  QString *error = nullptr);
    bool withTransaction(const std::function<bool(QString *)> &operation,
                         QString *error = nullptr);

private:
    QSqlDatabase m_database;
    int m_spotInsertCount = 0;
};

} // namespace logger
