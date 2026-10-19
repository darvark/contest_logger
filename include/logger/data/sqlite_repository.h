#pragma once

#include "logger/domain/contest_definition.h"
#include "logger/domain/types.h"

#include <QJsonArray>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <functional>

namespace logger {

class SqliteRepository final {
public:
    explicit SqliteRepository(QSqlDatabase database);

    static int currentSchemaVersion();
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
    QString localStationUuid(QString *error = nullptr) const;
    bool setSynchronizationEnabled(const QString &logUuid, bool enabled,
                                   QString *error = nullptr);
    bool enqueueExistingQsosForSync(const QString &logUuid,
                                    QString *error = nullptr);
    bool synchronizationEnabled(const QString &logUuid,
                                QString *error = nullptr) const;
    qint64 syncCursor(const QString &logUuid, QString *error = nullptr) const;
    bool setSyncCursor(const QString &logUuid, qint64 cursor,
                       QString *error = nullptr);
    QVector<SyncOutboxOperation> syncOutbox(
        const QString &logUuid, bool includeTerminal = false,
        QString *error = nullptr) const;
    QVector<SyncConflict> syncConflicts(const QString &logUuid,
                                        QString *error = nullptr) const;
    bool setSyncOperationState(const QString &operationUuid, const QString &state,
                               const QString &errorMessage = {},
                               QString *error = nullptr);
    bool retrySyncOperation(const QString &operationUuid, QString *error = nullptr);
    bool recoverInFlightSyncOperations(const QString &logUuid,
                                       QString *error = nullptr);
    bool saveSerialReservation(const SerialReservation &reservation,
                               QString *error = nullptr);
    std::optional<SerialReservation> serialReservation(
        const QString &logUuid, const QString &reservationUuid,
        QString *error = nullptr) const;
    QVector<SerialReservation> serialReservations(
        const QString &logUuid, QString *error = nullptr) const;
    bool enqueueSerialReservationRequest(
        const SerialReservationRequest &request, QString *error = nullptr);
    QVector<SerialReservationRequest> pendingSerialReservationRequests(
        const QString &logUuid, const QString &stationUuid,
        QString *error = nullptr) const;
    bool removeSerialReservationRequest(
        const QString &logUuid, const QString &reservationUuid,
        const QString &operation, QString *error = nullptr);
    bool setSerialReservationStatus(const QString &logUuid,
                                    const QString &reservationUuid,
                                    const QString &status,
                                    const QString &qsoUuid = {},
                                    QString *error = nullptr);
    bool recordSyncConflict(const QString &operationUuid,
                            const QString &remotePayloadJson,
                            QString *error = nullptr);
    bool resolveSyncConflictAcceptServer(const QString &operationUuid,
                                         QString *error = nullptr);
    bool resolveSyncConflictKeepLocal(const QString &operationUuid,
                                      QString *error = nullptr);
    bool applySyncChanges(const QString &logUuid, const QJsonArray &events,
                          qint64 fromRevision, qint64 toRevision,
                          QString *error = nullptr);
    bool applySyncSnapshot(const QString &logUuid, const QJsonArray &qsos,
                           qint64 revision, const QString &sha256,
                           QString *error = nullptr);
    bool saveQso(Qso &qso, QString *error = nullptr);
    bool updateQso(const Qso &qso, QString *error = nullptr);
    bool deleteQso(qint64 id, QString *error = nullptr);
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
