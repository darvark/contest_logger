#pragma once

#include "logger/data/sqlite_repository.h"
#include "logger/data/support_data_parser.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>
#include <memory>

class QLockFile;

namespace logger {

class CentralRepository final {
public:
    CentralRepository();
    ~CentralRepository();

    CentralRepository(const CentralRepository &) = delete;
    CentralRepository &operator=(const CentralRepository &) = delete;

    bool initialize(const QString &dataDirectory, QString *error = nullptr);
    SqliteRepository *repository();
    qint64 createLog(const ContestDefinition &definition, const QString &yaml,
                     const Contest &log, QString *error = nullptr);
    bool importSerialReservations(const QString &logUuid,
                                 const QJsonArray &reservations,
                                 QString *error = nullptr);
    bool submitBatch(const QString &logUuid, const QJsonArray &operations,
                     QJsonArray *results, QString *error = nullptr);
    QJsonObject reserveSerial(const QString &logUuid, const QString &stationUuid,
                              const QString &reservationUuid, const QString &band,
                              const QString &mode, QString *error = nullptr);
    bool cancelSerialReservation(const QString &logUuid, const QString &stationUuid,
                                 const QString &reservationUuid, bool administrator,
                                 QString *error = nullptr);
    QJsonArray serialReservations(const QString &logUuid,
                                 QString *error = nullptr) const;
    QJsonObject synchronizationMetrics(const QString &logUuid,
                                       QString *error = nullptr) const;
    QString beginLogImport(const QString &logUuid, const QString &sourceLogUuid,
                           const QString &sourceLogName, const QString &stationUuid,
                           int totalQso, QString *error = nullptr);
    bool updateLogImport(const QString &importUuid, int importedQso,
                         int skippedQso, const QString &status,
                         const QString &errorMessage = {},
                         QString *error = nullptr);
    QJsonObject logImportAudit(const QString &importUuid,
                               QString *error = nullptr) const;
    QJsonArray eventsSince(const QString &logUuid, qint64 afterRevision,
                           int limit, QString *error = nullptr) const;
    QJsonObject snapshotPage(const QString &logUuid, qint64 revision,
                             int offset, int limit,
                             QString *error = nullptr) const;
    qint64 earliestRevision(const QString &logUuid,
                            QString *error = nullptr) const;
    QJsonObject currentQso(const QString &logUuid, const QString &qsoUuid,
                           QString *error = nullptr) const;
    qint64 currentRevision(const QString &logUuid, QString *error = nullptr) const;
    bool recordClientCursor(const QString &logUuid, const QString &stationUuid,
                            qint64 cursor, QString *error = nullptr);
    qint64 clientCursor(const QString &logUuid, const QString &stationUuid,
                        QString *error = nullptr) const;
    QString serverUuid(QString *error = nullptr) const;
    QString databasePath() const;
    QString backupDirectory() const;

private:
    QString m_connectionName;
    QString m_databasePath;
    QString m_backupDirectory;
    QVector<CtyPrefixEntry> m_ctyPrefixes;
    QSqlDatabase m_database;
    std::unique_ptr<SqliteRepository> m_repository;
    std::unique_ptr<QLockFile> m_lockFile;
};

} // namespace logger
