#include "logger/data/sqlite_repository.h"
#include "logger/network/sync_consistency.h"

#include "logger/contest/contest_definition_loader.h"
#include "logger/data/log_exporter.h"
#include "logger/data/support_data_parser.h"
#include "logger/contest/qtc_service.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDataStream>
#include <QLockFile>
#include <QTemporaryFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <cmath>
#include <limits>
#include <utility>

namespace logger {
namespace {

constexpr int CurrentSchemaVersion = 21;
constexpr int SpotPruneInterval = 200;
constexpr int SpotRetentionHours = 24;
constexpr int MaximumRetainedSpots = 5000;

bool isSyncUuid(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
                       "[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch();
}

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

void clearError(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
}

QString queryError(const QSqlQuery &query)
{
    return query.lastError().text();
}

bool verifyLoggerDatabase(QSqlDatabase &database, QString *error)
{
    {
        QSqlQuery integrity(database);
        if (!integrity.exec(QStringLiteral("PRAGMA integrity_check"))
            || !integrity.next()
            || integrity.value(0).toString() != QStringLiteral("ok")) {
            setError(error, integrity.lastError().isValid()
                                ? queryError(integrity)
                                : QStringLiteral("SQLite integrity_check failed."));
            return false;
        }
        integrity.finish();
    }
    QSqlQuery schema(database);
    if (!schema.exec(QStringLiteral(
            "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))
        || !schema.next()) {
        setError(error, QStringLiteral("File is not a Contest Logger database: %1")
                            .arg(queryError(schema)));
        return false;
    }
    const int version = schema.value(0).toInt();
    schema.finish();
    if (version <= 0 || version > CurrentSchemaVersion) {
        setError(error, QStringLiteral("Backup has an unsupported database schema version."));
        return false;
    }
    return true;
}

QString rollbackError(QSqlDatabase &database, const QString &failure)
{
    if (database.rollback()) {
        return failure;
    }
    return failure + QStringLiteral("; rollback failed: ")
        + database.lastError().text();
}

QString utcString(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

bool withSavepoint(
    QSqlDatabase &database,
    const std::function<bool(QString *)> &operation,
    QString *error)
{
    const QString name = QStringLiteral("logger_op_%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)
                 .remove(QLatin1Char('-')));
    QSqlQuery begin(database);
    if (!begin.exec(QStringLiteral("SAVEPOINT %1").arg(name))) {
        setError(error, queryError(begin));
        return false;
    }

    QString operationError;
    if (!operation(&operationError)) {
        QSqlQuery rollback(database);
        if (!rollback.exec(QStringLiteral("ROLLBACK TO SAVEPOINT %1").arg(name))) {
            operationError += QStringLiteral("; rollback failed: ")
                + queryError(rollback);
        }
        QSqlQuery release(database);
        if (!release.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(name))) {
            operationError += QStringLiteral("; savepoint release failed: ")
                + queryError(release);
        }
        setError(error, operationError);
        return false;
    }

    QSqlQuery release(database);
    if (!release.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(name))) {
        const QString failure = queryError(release);
        QSqlQuery rollback(database);
        rollback.exec(QStringLiteral("ROLLBACK TO SAVEPOINT %1").arg(name));
        release.exec(QStringLiteral("RELEASE SAVEPOINT %1").arg(name));
        setError(error, failure);
        return false;
    }
    return true;
}

QJsonObject exchangeObject(const QMap<QString, QString> &fields)
{
    QJsonObject object;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return object;
}

QByteArray syncQsoPayload(const Qso &qso, const QString &operationUuid,
                          const QString &stationUuid, const QString &kind,
                          int expectedVersion)
{
    QJsonObject qsoPayload{
        {QStringLiteral("qso_uuid"), qso.qsoUuid},
        {QStringLiteral("log_uuid"), qso.logUuid},
        {QStringLiteral("station_uuid"), qso.stationUuid},
        {QStringLiteral("callsign"), qso.callsign.trimmed().toUpper()},
        {QStringLiteral("station_callsign"),
         qso.stationCallsign.trimmed().toUpper()},
        {QStringLiteral("band"), qso.band.trimmed().toLower()},
        {QStringLiteral("timestamp_utc"), utcString(qso.timestampUtc)},
        {QStringLiteral("frequency_hz"), qso.frequencyHz},
        {QStringLiteral("mode"), qso.mode},
        {QStringLiteral("sent_exchange"), qso.sentExchange},
        {QStringLiteral("received_exchange"), qso.receivedExchange},
        {QStringLiteral("sent_exchange_fields"), exchangeObject(qso.sentExchangeFields)},
        {QStringLiteral("received_exchange_fields"),
         exchangeObject(qso.receivedExchangeFields)},
        {QStringLiteral("qtc_transfers"), QtcService::toJson(qso.qtcTransfers)},
        {QStringLiteral("points"), qso.points},
        {QStringLiteral("is_invalid"), qso.invalid}
    };
    QJsonObject payload{
        {QStringLiteral("operation_uuid"), operationUuid},
        {QStringLiteral("log_uuid"), qso.logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("created_at_utc"), utcString(QDateTime::currentDateTimeUtc())},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("expected_version"), expectedVersion},
        {QStringLiteral("qso"), qsoPayload}
    };
    if (!qso.reservationUuid.isEmpty()) {
        payload.insert(QStringLiteral("reservation_uuid"), qso.reservationUuid);
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

QJsonObject qsoAuditSnapshot(const Qso &qso, bool deleted)
{
    return {
        {QStringLiteral("qso_uuid"), qso.qsoUuid},
        {QStringLiteral("log_uuid"), qso.logUuid},
        {QStringLiteral("station_uuid"), qso.stationUuid},
        {QStringLiteral("reservation_uuid"), qso.reservationUuid},
        {QStringLiteral("qso_version"), qso.qsoVersion},
        {QStringLiteral("callsign"), qso.callsign.trimmed().toUpper()},
        {QStringLiteral("station_callsign"), qso.stationCallsign.trimmed().toUpper()},
        {QStringLiteral("band"), qso.band.trimmed().toLower()},
        {QStringLiteral("timestamp_utc"), utcString(qso.timestampUtc)},
        {QStringLiteral("frequency_hz"), qso.frequencyHz},
        {QStringLiteral("mode"), qso.mode},
        {QStringLiteral("sent_exchange"), qso.sentExchange},
        {QStringLiteral("received_exchange"), qso.receivedExchange},
        {QStringLiteral("sent_exchange_fields"), exchangeObject(qso.sentExchangeFields)},
        {QStringLiteral("received_exchange_fields"),
         exchangeObject(qso.receivedExchangeFields)},
        {QStringLiteral("qtc_transfers"), QtcService::toJson(qso.qtcTransfers)},
        {QStringLiteral("points"), qso.points},
        {QStringLiteral("is_invalid"), qso.invalid},
        {QStringLiteral("is_deleted"), deleted}
    };
}

bool recordQsoAudit(QSqlDatabase &database, const QString &logUuid,
                    const Qso &qso, const QString &action,
                    const QJsonObject &before, const QJsonObject &after,
                    qint64 undoOfId, qint64 *auditId, QString *error)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO qso_change_audit(log_uuid, qso_uuid, qso_version, action, "
        "before_json, after_json, undo_of_id, created_at_utc) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(logUuid);
    query.addBindValue(qso.qsoUuid);
    query.addBindValue(qso.qsoVersion);
    query.addBindValue(action);
    query.addBindValue(QString::fromUtf8(QJsonDocument(before)
                                             .toJson(QJsonDocument::Compact)));
    query.addBindValue(QString::fromUtf8(QJsonDocument(after)
                                             .toJson(QJsonDocument::Compact)));
    if (undoOfId > 0) {
        query.addBindValue(undoOfId);
    } else {
        query.addBindValue(QVariant());
    }
    query.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    if (auditId != nullptr) {
        *auditId = query.lastInsertId().toLongLong();
    }
    return true;
}

QString newUuid();

bool enqueueQsoOperation(QSqlDatabase &database, const Qso &qso,
                         const QString &operation, int expectedVersion,
                         QString *error, bool hosted = false)
{
    if (hosted) {
        const QString operationUuid = newUuid();
        const QString now = utcString(QDateTime::currentDateTimeUtc());
        const bool deleted = operation == QStringLiteral("qso.delete");
        QJsonObject snapshotObject = qsoAuditSnapshot(qso, deleted);
        snapshotObject.remove(QStringLiteral("reservation_uuid"));
        const QByteArray snapshot = QJsonDocument(snapshotObject).toJson(QJsonDocument::Compact);
        const auto execute = [&database, error](const QString &sql,
                                                const QVariantList &values) {
            QSqlQuery query(database);
            query.prepare(sql);
            for (const QVariant &value : values) {
                query.addBindValue(value);
            }
            if (!query.exec()) {
                setError(error, queryError(query));
                return false;
            }
            return true;
        };
        if (!execute(QStringLiteral(
                "INSERT INTO sync_log_state(log_uuid, current_revision) VALUES(?, 1) "
                "ON CONFLICT(log_uuid) DO UPDATE SET current_revision = current_revision + 1"),
                {qso.logUuid})) {
            return false;
        }
        QSqlQuery revisionQuery(database);
        revisionQuery.prepare(QStringLiteral(
            "SELECT current_revision FROM sync_log_state WHERE log_uuid = ?"));
        revisionQuery.addBindValue(qso.logUuid);
        if (!revisionQuery.exec() || !revisionQuery.next()) {
            setError(error, queryError(revisionQuery));
            return false;
        }
        const qint64 revision = revisionQuery.value(0).toLongLong();
        revisionQuery.finish();
        const QJsonObject result{
            {QStringLiteral("operation_uuid"), operationUuid},
            {QStringLiteral("status"), QStringLiteral("accepted")},
            {QStringLiteral("qso_uuid"), qso.qsoUuid},
            {QStringLiteral("qso_version"), qso.qsoVersion},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("qso"), QJsonDocument::fromJson(snapshot).object()}
        };
        return execute(QStringLiteral(
            "INSERT INTO sync_operations(operation_uuid, log_uuid, station_uuid, "
            "payload_sha256, result_json, created_at_utc) VALUES(?, ?, ?, ?, ?, ?)"),
            {operationUuid, qso.logUuid, qso.stationUuid,
             QString::fromLatin1(QCryptographicHash::hash(
                 snapshot, QCryptographicHash::Sha256).toHex()),
             QJsonDocument(result).toJson(QJsonDocument::Compact), now})
            && execute(QStringLiteral(
                "INSERT INTO qso_history(log_uuid, qso_uuid, qso_version, operation_uuid, "
                "snapshot_json, is_deleted, created_at_utc, revision) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"),
                {qso.logUuid, qso.qsoUuid, qso.qsoVersion, operationUuid,
                 QString::fromUtf8(snapshot), deleted ? 1 : 0, now, revision})
            && execute(QStringLiteral(
                "INSERT INTO sync_events(event_uuid, log_uuid, revision, operation_uuid, "
                "kind, entity_uuid, payload_json, created_at_utc) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"),
                {newUuid(), qso.logUuid, revision, operationUuid, operation,
                 qso.qsoUuid, QString::fromUtf8(snapshot), now});
    }
    QSqlQuery enabled(database);
    enabled.prepare(QStringLiteral(
        "SELECT enabled FROM sync_state WHERE log_uuid = ?"));
    enabled.addBindValue(qso.logUuid);
    if (!enabled.exec()) {
        setError(error, queryError(enabled));
        return false;
    }
    if (!enabled.next() || !enabled.value(0).toBool()) {
        return true;
    }
    enabled.finish();

    QSqlQuery localStation(database);
    if (!localStation.exec(QStringLiteral(
            "SELECT value FROM sync_metadata WHERE key = 'local_station_uuid'"))) {
        setError(error, queryError(localStation));
        return false;
    }
    if (!localStation.next()) {
        setError(error, QStringLiteral("Local station identity is not initialized."));
        return false;
    }
    const QString stationUuid = localStation.value(0).toString();
    localStation.finish();

    const QString operationUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QSqlQuery outbox(database);
    outbox.prepare(QStringLiteral(
        "INSERT INTO sync_outbox(operation_uuid, log_uuid, entity_uuid, operation, "
        "payload_json, created_at_utc) VALUES(?, ?, ?, ?, ?, ?)"));
    outbox.addBindValue(operationUuid);
    outbox.addBindValue(qso.logUuid);
    outbox.addBindValue(qso.qsoUuid);
    outbox.addBindValue(operation);
    outbox.addBindValue(QString::fromUtf8(
        syncQsoPayload(qso, operationUuid, stationUuid, operation, expectedVersion)));
    outbox.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    if (!outbox.exec()) {
        setError(error, queryError(outbox));
        return false;
    }
    return true;
}

Qso readQso(const QSqlQuery &query)
{
    Qso qso;
    qso.id = query.value(QStringLiteral("id")).toLongLong();
    qso.contestId = query.value(QStringLiteral("contest_id")).toLongLong();
    qso.qsoUuid = query.value(QStringLiteral("qso_uuid")).toString();
    qso.logUuid = query.value(QStringLiteral("log_uuid")).toString();
    qso.stationUuid = query.value(QStringLiteral("station_uuid")).toString();
    qso.reservationUuid = query.value(QStringLiteral("reservation_uuid")).toString();
    qso.qsoVersion = query.value(QStringLiteral("qso_version")).toInt();
    const QString syncStatus = query.value(QStringLiteral("sync_status")).toString();
    if (syncStatus == QStringLiteral("pending")) {
        qso.syncStatus = QsoSyncStatus::Pending;
    } else if (syncStatus == QStringLiteral("synced")) {
        qso.syncStatus = QsoSyncStatus::Synced;
    } else if (syncStatus == QStringLiteral("conflict")) {
        qso.syncStatus = QsoSyncStatus::Conflict;
    }
    qso.callsign = query.value(QStringLiteral("callsign")).toString();
    qso.stationCallsign = query.value(QStringLiteral("station_callsign")).toString();
    qso.band = query.value(QStringLiteral("band")).toString();
    qso.timestampUtc = QDateTime::fromString(
        query.value(QStringLiteral("timestamp_utc")).toString(), Qt::ISODateWithMs);
    if (!qso.timestampUtc.isValid()) {
        qso.timestampUtc = QDateTime::fromString(
            query.value(QStringLiteral("timestamp_utc")).toString(), Qt::ISODate);
    }
    qso.timestampUtc = qso.timestampUtc.toUTC();
    qso.frequencyHz = query.value(QStringLiteral("frequency_hz")).toLongLong();
    qso.mode = query.value(QStringLiteral("mode")).toString();
    qso.sentExchange = query.value(QStringLiteral("sent_exchange")).toString();
    qso.receivedExchange = query.value(QStringLiteral("received_exchange")).toString();
    const auto readFields = [&query](const QString &column) {
        QMap<QString, QString> result;
        const QJsonDocument document = QJsonDocument::fromJson(
            query.value(column).toByteArray());
        if (!document.isObject()) {
            return result;
        }
        const QJsonObject object = document.object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.value().isString()) {
                result.insert(it.key(), it.value().toString());
            }
        }
        return result;
    };
    qso.sentExchangeFields = readFields(QStringLiteral("sent_exchange_json"));
    qso.receivedExchangeFields =
        readFields(QStringLiteral("received_exchange_json"));
    qso.qtcTransfers = QtcService::fromJson(QJsonDocument::fromJson(
        query.value(QStringLiteral("qtc_json")).toByteArray()).array());
    ContestDefinition definition;
    const QString yaml = query.value(QStringLiteral("definition_yaml")).toString();
    if (!yaml.isEmpty()) {
        ContestDefinitionLoader().loadText(yaml, QStringLiteral("stored contest"),
                                           definition, nullptr);
    }
    if (qso.sentExchangeFields.isEmpty() && !qso.sentExchange.trimmed().isEmpty()) {
        qso.sentExchangeFields = LogExporter::mapLegacyExchange(
            qso.sentExchange, definition.sentExchange);
    }
    if (qso.receivedExchangeFields.isEmpty()
        && !qso.receivedExchange.trimmed().isEmpty()) {
        qso.receivedExchangeFields = LogExporter::mapLegacyExchange(
            qso.receivedExchange, definition.receivedExchange);
    }
    qso.points = query.value(QStringLiteral("points")).toInt();
    qso.invalid = query.value(QStringLiteral("is_invalid")).toBool();
    return qso;
}

QByteArray exchangeJson(const QMap<QString, QString> &fields)
{
    QJsonObject object;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

const QStringList MigrationOne{
    QStringLiteral(
        "CREATE TABLE contests ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "code TEXT NOT NULL UNIQUE,"
        "name TEXT NOT NULL,"
        "description TEXT NOT NULL DEFAULT '',"
        "starts_at_utc TEXT,"
        "ends_at_utc TEXT)"),
    QStringLiteral(
        "CREATE TABLE stations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "callsign TEXT NOT NULL,"
        "operator_name TEXT NOT NULL DEFAULT '',"
        "grid TEXT NOT NULL DEFAULT '',"
        "power_class TEXT NOT NULL DEFAULT '')"),
    QStringLiteral(
        "CREATE TABLE radios ("
        "id TEXT PRIMARY KEY,"
        "name TEXT NOT NULL,"
        "model TEXT NOT NULL DEFAULT '',"
        "frequency_hz INTEGER NOT NULL DEFAULT 0,"
        "mode TEXT NOT NULL DEFAULT '',"
        "connection_state INTEGER NOT NULL DEFAULT 0)"),
    QStringLiteral(
        "CREATE TABLE vfos ("
        "id TEXT PRIMARY KEY,"
        "radio_id TEXT NOT NULL REFERENCES radios(id) ON DELETE CASCADE,"
        "number INTEGER NOT NULL,"
        "frequency_hz INTEGER NOT NULL DEFAULT 0,"
        "mode TEXT NOT NULL DEFAULT '',"
        "connection_state INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(radio_id, number))"),
    QStringLiteral(
        "CREATE TABLE spots ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "callsign TEXT NOT NULL,"
        "frequency_hz INTEGER NOT NULL,"
        "mode TEXT NOT NULL DEFAULT '',"
        "spotter TEXT NOT NULL DEFAULT '',"
        "comment TEXT NOT NULL DEFAULT '',"
        "timestamp_utc TEXT NOT NULL,"
        "source TEXT NOT NULL DEFAULT '')"),
    QStringLiteral(
        "CREATE TABLE qsos ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contest_id INTEGER NOT NULL REFERENCES contests(id),"
        "callsign TEXT NOT NULL,"
        "station_callsign TEXT NOT NULL,"
        "timestamp_utc TEXT NOT NULL,"
        "frequency_hz INTEGER NOT NULL DEFAULT 0,"
        "mode TEXT NOT NULL DEFAULT '',"
        "sent_exchange TEXT NOT NULL DEFAULT '',"
        "received_exchange TEXT NOT NULL DEFAULT '',"
        "points INTEGER NOT NULL DEFAULT 0)"),
    QStringLiteral(
        "CREATE INDEX idx_qsos_contest_timestamp "
        "ON qsos(contest_id, timestamp_utc DESC)"),
    QStringLiteral("CREATE INDEX idx_qsos_callsign ON qsos(callsign)")
};

const QStringList MigrationTwo{
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN band TEXT NOT NULL DEFAULT ''"),
    QStringLiteral(
        "ALTER TABLE contests ADD COLUMN definition_yaml TEXT NOT NULL DEFAULT ''")
};

const QStringList MigrationThree{
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN sent_exchange_json TEXT NOT NULL DEFAULT '{}'"),
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN received_exchange_json TEXT NOT NULL DEFAULT '{}'")
};

const QStringList MigrationFour{
    QStringLiteral("ALTER TABLE spots ADD COLUMN band TEXT NOT NULL DEFAULT ''")
};

const QStringList MigrationFive{
    QStringLiteral(
        "CREATE INDEX idx_spots_timestamp ON spots(timestamp_utc DESC, id DESC)"),
    QStringLiteral(
        "CREATE INDEX idx_qsos_contest_dupe ON qsos("
        "contest_id, callsign COLLATE NOCASE, band COLLATE NOCASE, "
        "mode COLLATE NOCASE)")
};

const QStringList MigrationSix{
    QStringLiteral(
        "ALTER TABLE contests ADD COLUMN station_callsign TEXT NOT NULL DEFAULT ''"),
    QStringLiteral(
        "ALTER TABLE contests ADD COLUMN properties_json TEXT NOT NULL DEFAULT '{}'")
};

const QStringList MigrationSeven{
    QStringLiteral(
        "ALTER TABLE contests ADD COLUMN log_uuid TEXT NOT NULL DEFAULT ''"),
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN qso_uuid TEXT NOT NULL DEFAULT ''"),
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN station_uuid TEXT NOT NULL DEFAULT ''"),
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN qso_version INTEGER NOT NULL DEFAULT 1"),
    QStringLiteral(
        "CREATE TABLE sync_metadata ("
        "key TEXT PRIMARY KEY,"
        "value TEXT NOT NULL)")
};

const QStringList MigrationEight{
    QStringLiteral(
        "CREATE TABLE sync_state ("
        "log_uuid TEXT PRIMARY KEY,"
        "enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0, 1)),"
        "cursor INTEGER NOT NULL DEFAULT 0 CHECK(cursor >= 0),"
        "last_error TEXT NOT NULL DEFAULT '',"
        "updated_at_utc TEXT NOT NULL)"),
    QStringLiteral(
        "CREATE TABLE sync_outbox ("
        "operation_uuid TEXT PRIMARY KEY,"
        "log_uuid TEXT NOT NULL,"
        "entity_uuid TEXT NOT NULL,"
        "operation TEXT NOT NULL,"
        "payload_json TEXT NOT NULL,"
        "created_at_utc TEXT NOT NULL,"
        "status TEXT NOT NULL DEFAULT 'pending' "
        "CHECK(status IN ('pending', 'in-flight', 'accepted', 'conflict', 'failed')),"
        "attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts >= 0),"
        "last_error TEXT NOT NULL DEFAULT '')"),
    QStringLiteral(
        "CREATE INDEX idx_sync_outbox_log_status_created "
        "ON sync_outbox(log_uuid, status, created_at_utc, operation_uuid)"),
    QStringLiteral(
        "CREATE TABLE sync_conflicts ("
        "operation_uuid TEXT PRIMARY KEY REFERENCES sync_outbox(operation_uuid),"
        "log_uuid TEXT NOT NULL,"
        "entity_uuid TEXT NOT NULL,"
        "local_payload_json TEXT NOT NULL,"
        "remote_payload_json TEXT NOT NULL,"
        "created_at_utc TEXT NOT NULL,"
        "resolved_at_utc TEXT NOT NULL DEFAULT '')")
};

const QStringList MigrationNine{
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN is_deleted INTEGER NOT NULL DEFAULT 0 "
        "CHECK(is_deleted IN (0, 1))"),
    QStringLiteral(
        "CREATE TABLE sync_log_state ("
        "log_uuid TEXT PRIMARY KEY REFERENCES contests(log_uuid),"
        "current_revision INTEGER NOT NULL DEFAULT 0 CHECK(current_revision >= 0))"),
    QStringLiteral(
        "CREATE TABLE sync_clients ("
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "station_uuid TEXT NOT NULL,"
        "cursor INTEGER NOT NULL DEFAULT 0 CHECK(cursor >= 0),"
        "last_seen_at_utc TEXT NOT NULL,"
        "PRIMARY KEY(log_uuid, station_uuid))"),
    QStringLiteral(
        "CREATE TABLE sync_operations ("
        "operation_uuid TEXT PRIMARY KEY,"
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "station_uuid TEXT NOT NULL,"
        "payload_sha256 TEXT NOT NULL,"
        "result_json TEXT NOT NULL,"
        "created_at_utc TEXT NOT NULL)"),
    QStringLiteral(
        "CREATE TABLE sync_events ("
        "event_uuid TEXT PRIMARY KEY,"
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "revision INTEGER NOT NULL CHECK(revision > 0),"
        "operation_uuid TEXT NOT NULL REFERENCES sync_operations(operation_uuid) "
        "DEFERRABLE INITIALLY DEFERRED,"
        "kind TEXT NOT NULL,"
        "entity_uuid TEXT NOT NULL,"
        "payload_json TEXT NOT NULL,"
        "created_at_utc TEXT NOT NULL,"
        "UNIQUE(log_uuid, revision))"),
    QStringLiteral(
        "CREATE TABLE qso_history ("
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "qso_uuid TEXT NOT NULL,"
        "qso_version INTEGER NOT NULL CHECK(qso_version > 0),"
        "operation_uuid TEXT NOT NULL REFERENCES sync_operations(operation_uuid) "
        "DEFERRABLE INITIALLY DEFERRED,"
        "snapshot_json TEXT NOT NULL,"
        "is_deleted INTEGER NOT NULL DEFAULT 0 CHECK(is_deleted IN (0, 1)),"
        "created_at_utc TEXT NOT NULL,"
        "PRIMARY KEY(log_uuid, qso_uuid, qso_version))"),
    QStringLiteral(
        "CREATE INDEX idx_sync_events_log_revision "
        "ON sync_events(log_uuid, revision)"),
    QStringLiteral(
        "CREATE INDEX idx_sync_operations_log_created "
        "ON sync_operations(log_uuid, created_at_utc)")
};

const QStringList MigrationTen{
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN reservation_uuid TEXT NOT NULL DEFAULT ''"),
    QStringLiteral(
        "CREATE TABLE serial_reservations ("
        "reservation_uuid TEXT PRIMARY KEY,"
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "station_uuid TEXT NOT NULL,"
        "scope_key TEXT NOT NULL,"
        "band TEXT NOT NULL DEFAULT '',"
        "mode TEXT NOT NULL DEFAULT '',"
        "serial_number INTEGER NOT NULL CHECK(serial_number > 0),"
        "status TEXT NOT NULL DEFAULT 'reserved' "
        "CHECK(status IN ('reserved', 'used', 'cancelled')),"
        "qso_uuid TEXT NOT NULL DEFAULT '',"
        "created_at_utc TEXT NOT NULL,"
        "updated_at_utc TEXT NOT NULL,"
        "UNIQUE(log_uuid, scope_key, serial_number))"),
    QStringLiteral(
        "CREATE INDEX idx_serial_reservations_log_status "
        "ON serial_reservations(log_uuid, status, serial_number)")
};

const QStringList MigrationEleven{
    QStringLiteral(
        "CREATE TABLE serial_reservation_requests ("
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "station_uuid TEXT NOT NULL,"
        "reservation_uuid TEXT NOT NULL,"
        "operation TEXT NOT NULL CHECK(operation IN ('reserve_serial', 'cancel_serial')),"
        "band TEXT NOT NULL DEFAULT '',"
        "mode TEXT NOT NULL DEFAULT '',"
        "created_at_utc TEXT NOT NULL,"
        "PRIMARY KEY(log_uuid, reservation_uuid, operation))"),
    QStringLiteral(
        "CREATE INDEX idx_serial_reservation_requests_created "
        "ON serial_reservation_requests(log_uuid, station_uuid, created_at_utc)")
};

const QStringList MigrationTwelve{
    QStringLiteral(
        "CREATE TABLE sync_conflict_resolutions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "operation_uuid TEXT NOT NULL,"
        "log_uuid TEXT NOT NULL,"
        "entity_uuid TEXT NOT NULL,"
        "resolution TEXT NOT NULL CHECK(resolution IN ('accept_server', 'keep_local')),"
        "local_payload_json TEXT NOT NULL,"
        "remote_payload_json TEXT NOT NULL,"
        "resolved_at_utc TEXT NOT NULL)")
};

const QStringList MigrationThirteen{
    QStringLiteral(
        "CREATE TABLE sync_import_audit ("
        "import_uuid TEXT PRIMARY KEY,"
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "source_log_uuid TEXT NOT NULL,"
        "source_log_name TEXT NOT NULL,"
        "station_uuid TEXT NOT NULL,"
        "total_qso INTEGER NOT NULL CHECK(total_qso >= 0),"
        "imported_qso INTEGER NOT NULL DEFAULT 0 CHECK(imported_qso >= 0),"
        "skipped_qso INTEGER NOT NULL DEFAULT 0 CHECK(skipped_qso >= 0),"
        "status TEXT NOT NULL CHECK(status IN "
        "('in_progress', 'completed', 'conflict', 'failed', 'cancelled')),"
        "error_message TEXT NOT NULL DEFAULT '',"
        "created_at_utc TEXT NOT NULL,"
        "updated_at_utc TEXT NOT NULL)"),
    QStringLiteral(
        "CREATE INDEX idx_sync_import_audit_source "
        "ON sync_import_audit(log_uuid, source_log_uuid, status, updated_at_utc)")
};

const QStringList MigrationFourteen{
    QStringLiteral(
        "ALTER TABLE qso_history ADD COLUMN revision INTEGER NOT NULL DEFAULT 0 "
        "CHECK(revision >= 0)"),
    QStringLiteral(
        "UPDATE qso_history SET revision = COALESCE(("
        "SELECT revision FROM sync_events WHERE "
        "sync_events.log_uuid = qso_history.log_uuid AND "
        "sync_events.operation_uuid = qso_history.operation_uuid AND "
        "sync_events.entity_uuid = qso_history.qso_uuid), 0)"),
    QStringLiteral(
        "UPDATE sync_events SET created_at_utc = "
        "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')")
};

const QStringList MigrationFifteen{
    QStringLiteral("ALTER TABLE sync_conflict_resolutions "
                   "RENAME TO sync_conflict_resolutions_old"),
    QStringLiteral(
        "CREATE TABLE sync_conflict_resolutions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "operation_uuid TEXT NOT NULL,"
        "log_uuid TEXT NOT NULL,"
        "entity_uuid TEXT NOT NULL,"
        "resolution TEXT NOT NULL CHECK(resolution IN "
        "('accept_server', 'keep_local', 'merge')),"
        "local_payload_json TEXT NOT NULL,"
        "remote_payload_json TEXT NOT NULL,"
        "resolution_payload_json TEXT NOT NULL DEFAULT '',"
        "resolved_at_utc TEXT NOT NULL)"),
    QStringLiteral(
        "INSERT INTO sync_conflict_resolutions(id, operation_uuid, log_uuid, "
        "entity_uuid, resolution, local_payload_json, remote_payload_json, "
        "resolved_at_utc) SELECT id, operation_uuid, log_uuid, entity_uuid, "
        "resolution, local_payload_json, remote_payload_json, resolved_at_utc "
        "FROM sync_conflict_resolutions_old"),
    QStringLiteral("DROP TABLE sync_conflict_resolutions_old")
};

const QStringList MigrationSixteen{
    QStringLiteral(
        "CREATE TABLE serial_reservation_adjustments ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "log_uuid TEXT NOT NULL,"
        "reservation_uuid TEXT NOT NULL,"
        "administrator_station_uuid TEXT NOT NULL,"
        "previous_serial INTEGER NOT NULL,"
        "corrected_serial INTEGER NOT NULL,"
        "reason TEXT NOT NULL,"
        "created_at_utc TEXT NOT NULL)")
};

const QStringList MigrationSeventeen{
    QStringLiteral(
        "ALTER TABLE sync_clients ADD COLUMN pending_count INTEGER NOT NULL "
        "DEFAULT 0 CHECK(pending_count >= 0)"),
    QStringLiteral(
        "ALTER TABLE sync_clients ADD COLUMN error_count INTEGER NOT NULL "
        "DEFAULT 0 CHECK(error_count >= 0)"),
    QStringLiteral(
        "ALTER TABLE sync_clients ADD COLUMN conflict_count INTEGER NOT NULL "
        "DEFAULT 0 CHECK(conflict_count >= 0)"),
    QStringLiteral(
        "CREATE TABLE sync_transaction_metrics ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "station_uuid TEXT NOT NULL,"
        "completed_at_utc TEXT NOT NULL,"
        "duration_ms INTEGER NOT NULL CHECK(duration_ms >= 0),"
        "outcome TEXT NOT NULL CHECK(outcome IN ('success', 'error', 'conflict')),"
        "operation_count INTEGER NOT NULL CHECK(operation_count >= 0))"),
    QStringLiteral(
        "CREATE INDEX idx_sync_transaction_metrics_log_time "
        "ON sync_transaction_metrics(log_uuid, completed_at_utc DESC)")
};

const QStringList MigrationEighteen{
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN is_invalid INTEGER NOT NULL DEFAULT 0 "
        "CHECK(is_invalid IN (0, 1))")
};

const QStringList MigrationNineteen{
    QStringLiteral(
        "CREATE TABLE qso_change_audit ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "log_uuid TEXT NOT NULL REFERENCES contests(log_uuid),"
        "qso_uuid TEXT NOT NULL,"
        "qso_version INTEGER NOT NULL CHECK(qso_version > 0),"
        "action TEXT NOT NULL CHECK(action IN "
        "('create', 'update', 'delete', 'undo')),"
        "before_json TEXT NOT NULL,"
        "after_json TEXT NOT NULL,"
        "undo_of_id INTEGER REFERENCES qso_change_audit(id),"
        "created_at_utc TEXT NOT NULL)"),
    QStringLiteral(
        "CREATE INDEX idx_qso_change_audit_log_created "
        "ON qso_change_audit(log_uuid, id DESC)"),
    QStringLiteral(
        "CREATE INDEX idx_qso_change_audit_undo_of "
        "ON qso_change_audit(undo_of_id)")
};

const QStringList MigrationTwenty{
    QStringLiteral(
        "ALTER TABLE qsos ADD COLUMN qtc_json TEXT NOT NULL DEFAULT '[]'")
};

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

bool backfillSyncIdentities(QSqlDatabase &database, QString *error)
{
    QSqlQuery station(database);
    if (!station.exec(QStringLiteral(
            "SELECT value FROM sync_metadata WHERE key = 'local_station_uuid'"))) {
        setError(error, queryError(station));
        return false;
    }
    QString localStationUuid;
    if (station.next()) {
        localStationUuid = station.value(0).toString();
    }
    station.finish();
    if (localStationUuid.isEmpty()) {
        localStationUuid = newUuid();
        QSqlQuery insertStation(database);
        insertStation.prepare(QStringLiteral(
            "INSERT INTO sync_metadata(key, value) VALUES('local_station_uuid', ?)"));
        insertStation.addBindValue(localStationUuid);
        if (!insertStation.exec()) {
            setError(error, queryError(insertStation));
            return false;
        }
    }

    struct IdentityRow {
        qint64 id = 0;
    };
    QVector<IdentityRow> contests;
    QSqlQuery selectContests(database);
    if (!selectContests.exec(QStringLiteral(
            "SELECT id FROM contests WHERE log_uuid = '' OR log_uuid IS NULL"))) {
        setError(error, queryError(selectContests));
        return false;
    }
    while (selectContests.next()) {
        contests.append(IdentityRow{selectContests.value(0).toLongLong()});
    }
    selectContests.finish();
    for (const IdentityRow &row : contests) {
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE contests SET log_uuid = ? WHERE id = ?"));
        update.addBindValue(newUuid());
        update.addBindValue(row.id);
        if (!update.exec()) {
            setError(error, queryError(update));
            return false;
        }
    }

    QVector<IdentityRow> qsos;
    QSqlQuery selectQsos(database);
    if (!selectQsos.exec(QStringLiteral(
            "SELECT id FROM qsos WHERE qso_uuid = '' OR qso_uuid IS NULL"))) {
        setError(error, queryError(selectQsos));
        return false;
    }
    while (selectQsos.next()) {
        qsos.append(IdentityRow{selectQsos.value(0).toLongLong()});
    }
    selectQsos.finish();
    for (const IdentityRow &row : qsos) {
        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE qsos SET qso_uuid = ?, station_uuid = ? WHERE id = ?"));
        update.addBindValue(newUuid());
        update.addBindValue(localStationUuid);
        update.addBindValue(row.id);
        if (!update.exec()) {
            setError(error, queryError(update));
            return false;
        }
    }

    QSqlQuery logIndex(database);
    if (!logIndex.exec(QStringLiteral(
            "CREATE UNIQUE INDEX idx_contests_log_uuid ON contests(log_uuid)"))) {
        setError(error, queryError(logIndex));
        return false;
    }
    QSqlQuery qsoIndex(database);
    if (!qsoIndex.exec(QStringLiteral(
            "CREATE UNIQUE INDEX idx_qsos_qso_uuid ON qsos(qso_uuid)"))) {
        setError(error, queryError(qsoIndex));
        return false;
    }
    return true;
}

QMap<QString, QString> propertiesFromJson(const QString &json)
{
    QMap<QString, QString> result;
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        result.insert(it.key(), it.value().toString());
    }
    return result;
}

QVariant utcText(const std::optional<QDateTime> &value)
{
    if (!value.has_value() || !value->isValid()) {
        return QVariant(QMetaType::fromType<QString>());
    }
    return value->toUTC().toString(Qt::ISODate);
}

// A null QString binds as SQL NULL, which NOT NULL text columns reject.
QString notNullText(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

bool backfillLegacyExchanges(QSqlDatabase &database, QString *error)
{
    struct LegacyRow {
        qint64 id = 0;
        QString sent;
        QString received;
        QByteArray sentJson;
        QByteArray receivedJson;
        QString yaml;
    };
    QVector<LegacyRow> rows;
    QSqlQuery select(database);
    if (!select.exec(QStringLiteral(
            "SELECT q.id, q.sent_exchange, q.received_exchange, "
            "q.sent_exchange_json, q.received_exchange_json, "
            "c.definition_yaml FROM qsos q LEFT JOIN contests c "
            "ON c.id = q.contest_id"))) {
        setError(error, queryError(select));
        return false;
    }
    while (select.next()) {
        LegacyRow row;
        row.id = select.value(0).toLongLong();
        row.sent = select.value(1).toString();
        row.received = select.value(2).toString();
        row.sentJson = select.value(3).toByteArray();
        row.receivedJson = select.value(4).toByteArray();
        row.yaml = select.value(5).toString();
        rows.append(std::move(row));
    }
    select.finish();
    for (const LegacyRow &row : rows) {
        ContestDefinition definition;
        if (!row.yaml.isEmpty()) {
            ContestDefinitionLoader().loadText(row.yaml, QStringLiteral("stored contest"),
                                               definition, nullptr);
        }
        const QJsonDocument sentDocument = QJsonDocument::fromJson(row.sentJson);
        const QJsonDocument receivedDocument = QJsonDocument::fromJson(row.receivedJson);
        const bool sentEmpty = !sentDocument.isObject() || sentDocument.object().isEmpty();
        const bool receivedEmpty = !receivedDocument.isObject()
            || receivedDocument.object().isEmpty();
        const QMap<QString, QString> sentFields = sentEmpty
            ? LogExporter::mapLegacyExchange(row.sent, definition.sentExchange)
            : QMap<QString, QString>{};
        const QMap<QString, QString> receivedFields = receivedEmpty
            ? LogExporter::mapLegacyExchange(row.received, definition.receivedExchange)
            : QMap<QString, QString>{};
        if ((!sentEmpty || sentFields.isEmpty())
            && (!receivedEmpty || receivedFields.isEmpty())) {
            continue;
        }
        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE qsos SET sent_exchange_json = ?, received_exchange_json = ? "
            "WHERE id = ?"));
        update.addBindValue(sentEmpty ? exchangeJson(sentFields) : row.sentJson);
        update.addBindValue(receivedEmpty ? exchangeJson(receivedFields)
                                         : row.receivedJson);
        update.addBindValue(row.id);
        if (!update.exec()) {
            setError(error, queryError(update));
            return false;
        }
    }
    return true;
}

} // namespace

SqliteRepository::SqliteRepository(QSqlDatabase database)
    : m_database(std::move(database))
{
}

struct SqliteRepository::FileConnection {
    QString name = QStringLiteral("log-file-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::unique_ptr<QLockFile> lock;
    ~FileConnection()
    {
        if (QSqlDatabase::contains(name)) {
            QSqlDatabase::removeDatabase(name);
        }
    }
};

QString SqliteRepository::databasePath() const
{
    return m_database.databaseName() == QLatin1String(":memory:")
        ? QString() : m_database.databaseName();
}

bool SqliteRepository::isStandaloneLog() const
{
    return m_standaloneLog;
}

void SqliteRepository::swap(SqliteRepository &other)
{
    std::swap(m_fileConnection, other.m_fileConnection);
    std::swap(m_database, other.m_database);
    std::swap(m_standaloneLog, other.m_standaloneLog);
    std::swap(m_hostedLogUuid, other.m_hostedLogUuid);
    std::swap(m_spotInsertCount, other.m_spotInsertCount);
}

bool SqliteRepository::openLogFile(const QString &path, QString *error)
{
    clearError(error);
    const QFileInfo file(path);
    if (!file.isFile()) {
        setError(error, QStringLiteral("Log file does not exist: %1").arg(path));
        return false;
    }
    if (m_standaloneLog
        && file.canonicalFilePath() == QFileInfo(databasePath()).canonicalFilePath()) {
        return true;
    }
    SqliteRepository candidate{QSqlDatabase()};
    candidate.m_fileConnection = std::make_shared<FileConnection>();
    candidate.m_fileConnection->lock =
        std::make_unique<QLockFile>(file.canonicalFilePath() + QStringLiteral(".lock"));
    if (!candidate.m_fileConnection->lock->tryLock(0)) {
        setError(error, QStringLiteral("Log file is already open or cannot be locked: %1")
                            .arg(path));
        return false;
    }
    candidate.m_database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), candidate.m_fileConnection->name);
    candidate.m_database.setDatabaseName(file.canonicalFilePath());
    // Validate before initialization so an unrelated SQLite file is never migrated.
    candidate.m_database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!candidate.m_database.open()) {
        setError(error, candidate.m_database.lastError().text());
        return false;
    }
    if (!verifyLoggerDatabase(candidate.m_database, error)) {
        return false;
    }
    {
        QSqlQuery check(candidate.m_database);
        if (!check.exec(QStringLiteral(
                "SELECT version FROM log_file WHERE id = 1"))
            || !check.next() || check.value(0).toInt() != 1) {
            setError(error, QStringLiteral(
                "This is not a standalone log file. The legacy shared database "
                "is unchanged; export its logs with the previous version and import "
                "them into new log files."));
            return false;
        }
    }
    candidate.m_database.close();
    candidate.m_database.setConnectOptions();
    if (!candidate.m_database.open() || !candidate.initialize(error)) {
        if (error != nullptr && error->isEmpty()) {
            *error = candidate.m_database.lastError().text();
        }
        return false;
    }
    if (!candidate.validateStandaloneLog(error)) {
        return false;
    }
    swap(candidate);
    return true;
}

bool SqliteRepository::validateStandaloneLog(QString *error) const
{
    QString localError;
    if (error == nullptr) {
        error = &localError;
    }
    const auto logs = contests(error);
    if ((error != nullptr && !error->isEmpty()) || logs.size() != 1) {
        setError(error, QStringLiteral("A standalone file must contain exactly one log."));
        return false;
    }
    const QString yaml = contestDefinitionYaml(logs.first().id, error);
    ContestDefinition definition;
    if ((error != nullptr && !error->isEmpty())
        || !ContestDefinitionLoader().loadText(yaml, databasePath(), definition, error)) {
        return false;
    }
    logSettings(error);
    if (error != nullptr && !error->isEmpty()) {
        return false;
    }
    const auto assets = logAssets(error);
    if (error != nullptr && !error->isEmpty()) {
        return false;
    }
    for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
        const QString contents = QString::fromUtf8(it.value());
        QStringList callsigns;
        QVector<CtyPrefixEntry> prefixes;
        QHash<QString, CallHistoryEntry> history;
        bool valid = true;
        if (it.key().startsWith(QLatin1String("support/master/"))) {
            valid = SupportDataParser::parseMasterCpy(contents, callsigns, error);
        } else if (it.key() == QLatin1String("support/cty")) {
            valid = SupportDataParser::parseCtyDat(contents, prefixes, error);
        } else if (it.key().startsWith(QLatin1String("support/history/"))) {
            valid = SupportDataParser::parseCallHistory(contents, history, error);
        } else if (it.key().startsWith(QLatin1String("definitions/"))) {
            valid = ContestDefinitionLoader().loadText(contents, it.key(), definition, error);
        }
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool SqliteRepository::createLogFile(
    const QString &path, const ContestDefinition &definition, const QString &yaml,
    const Contest &log, const QVariantMap &settings,
    const QMap<QString, QByteArray> &assets, const QVector<Qso> &qsos, QString *error)
{
    clearError(error);
    const QFileInfo destination(path);
    if (path.isEmpty() || destination.exists() || !destination.dir().exists()) {
        setError(error, QStringLiteral("Choose a new log file in an existing directory."));
        return false;
    }
    QTemporaryFile staging(destination.dir().filePath(QStringLiteral(".new-log-XXXXXX")));
    if (!staging.open()) {
        setError(error, staging.errorString());
        return false;
    }
    const QString stagedPath = staging.fileName();
    staging.close();
    {
        SqliteRepository candidate{QSqlDatabase()};
        candidate.m_fileConnection = std::make_shared<FileConnection>();
        candidate.m_database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), candidate.m_fileConnection->name);
        candidate.m_database.setDatabaseName(stagedPath);
        if (!candidate.m_database.open()) {
            setError(error, candidate.m_database.lastError().text());
            return false;
        }
        if (!candidate.initialize(error)
            || !candidate.withTransaction([&](QString *failure) {
                Contest stored = log;
                stored.name = destination.completeBaseName();
                const qint64 id = candidate.createContestLog(definition, yaml, stored, failure);
                if (id <= 0) {
                    return false;
                }
                QSqlQuery marker(candidate.m_database);
                if (!marker.exec(QStringLiteral("INSERT INTO log_file VALUES(1, 1)"))) {
                    setError(failure, queryError(marker));
                    return false;
                }
                for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
                    if (!candidate.setLogSetting(it.key(), it.value(), failure)) {
                        return false;
                    }
                }
                for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
                    if (!candidate.setLogAsset(it.key(), it.value(), failure)) {
                        return false;
                    }
                }
                for (Qso qso : qsos) {
                    qso.id = 0;
                    qso.contestId = id;
                    if (!candidate.saveQso(qso, failure)) {
                        return false;
                    }
                }
                return true;
            }, error)) {
            return false;
        }
        if (!candidate.validateStandaloneLog(error)) {
            return false;
        }
        QSqlQuery checkpoint(candidate.m_database);
        if (!checkpoint.exec(QStringLiteral("PRAGMA journal_mode = DELETE"))) {
            setError(error, queryError(checkpoint));
            return false;
        }
    }
    // rename never overwrites an existing destination, including a concurrent creator.
    if (!QFile::rename(stagedPath, destination.absoluteFilePath())) {
        setError(error, QStringLiteral("Could not install the new log file: %1").arg(path));
        return false;
    }
    if (!openLogFile(destination.absoluteFilePath(), error)) {
        return false;
    }
    return true;
}

QVariantMap SqliteRepository::logSettings(QString *error) const
{
    clearError(error);
    QVariantMap result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT key, value FROM log_settings"))) {
        setError(error, queryError(query));
        return {};
    }
    while (query.next()) {
        const QByteArray bytes = query.value(1).toByteArray();
        QDataStream stream(bytes);
        stream.setVersion(QDataStream::Qt_6_0);
        QVariant value;
        stream >> value;
        if (stream.status() != QDataStream::Ok) {
            setError(error, QStringLiteral("Invalid log setting: %1").arg(query.value(0).toString()));
            return {};
        }
        result.insert(query.value(0).toString(), value);
    }
    return result;
}

bool SqliteRepository::setLogSetting(const QString &key, const QVariant &value, QString *error)
{
    clearError(error);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << value;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO log_settings(key, value) VALUES(?, ?)"));
    query.addBindValue(key);
    query.addBindValue(bytes);
    if (stream.status() != QDataStream::Ok || !query.exec()) {
        setError(error, stream.status() != QDataStream::Ok
                            ? QStringLiteral("Could not serialize log setting: %1").arg(key)
                            : queryError(query));
        return false;
    }
    return true;
}

QMap<QString, QByteArray> SqliteRepository::logAssets(QString *error) const
{
    clearError(error);
    QMap<QString, QByteArray> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT key, contents FROM log_assets"))) {
        setError(error, queryError(query));
        return {};
    }
    while (query.next()) {
        result.insert(query.value(0).toString(), query.value(1).toByteArray());
    }
    return result;
}

bool SqliteRepository::setLogAsset(const QString &key, const QByteArray &contents, QString *error)
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO log_assets(key, contents) VALUES(?, ?)"));
    query.addBindValue(key);
    query.addBindValue(contents);
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    return true;
}

int SqliteRepository::currentSchemaVersion()
{
    return CurrentSchemaVersion;
}

bool SqliteRepository::initialize(QString *error)
{
    clearError(error);
    if (!m_database.isValid() || !m_database.isOpen()) {
        setError(error, QStringLiteral("SQLite database is not open."));
        return false;
    }

    const QStringList pragmas{
        QStringLiteral("PRAGMA foreign_keys = ON"),
        QStringLiteral("PRAGMA busy_timeout = 5000"),
        QStringLiteral("PRAGMA synchronous = FULL"),
        QStringLiteral("PRAGMA journal_mode = WAL")
    };
    for (const QString &statement : pragmas) {
        QSqlQuery pragma(m_database);
        if (!pragma.exec(statement)) {
            setError(error, queryError(pragma));
            return false;
        }
        pragma.finish();
    }
    QSqlQuery integrity(m_database);
    if (!integrity.exec(QStringLiteral("PRAGMA quick_check")) || !integrity.next()
        || integrity.value(0).toString() != QStringLiteral("ok")) {
        setError(error, integrity.lastError().isValid()
                            ? queryError(integrity)
                            : QStringLiteral("SQLite quick_check reported database corruption."));
        return false;
    }
    integrity.finish();

    QSqlQuery migrationTable(m_database);
    if (!migrationTable.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "version INTEGER PRIMARY KEY,"
            "applied_at_utc TEXT NOT NULL)"))) {
        setError(error, queryError(migrationTable));
        return false;
    }

    QSqlQuery versionQuery(m_database);
    if (!versionQuery.exec(QStringLiteral(
            "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))
        || !versionQuery.next()) {
        setError(error, queryError(versionQuery));
        return false;
    }
    const int currentVersion = versionQuery.value(0).toInt();
    versionQuery.finish();
    if (currentVersion > CurrentSchemaVersion) {
        setError(error, QStringLiteral(
                            "Database schema is newer than this application."));
        return false;
    }
    if (currentVersion < 0) {
        setError(error, QStringLiteral("No migration is available for this schema version."));
        return false;
    }

    for (int version = currentVersion + 1; version <= CurrentSchemaVersion; ++version) {
        const QStringList *migration = nullptr;
        if (version == 1) {
            migration = &MigrationOne;
        } else if (version == 2) {
            migration = &MigrationTwo;
        } else if (version == 3) {
            migration = &MigrationThree;
        } else if (version == 4) {
            migration = &MigrationFour;
        } else if (version == 5) {
            migration = &MigrationFive;
        } else if (version == 6) {
            migration = &MigrationSix;
        } else if (version == 7) {
            migration = &MigrationSeven;
        } else if (version == 8) {
            migration = &MigrationEight;
        } else if (version == 9) {
            migration = &MigrationNine;
        } else if (version == 10) {
            migration = &MigrationTen;
        } else if (version == 11) {
            migration = &MigrationEleven;
        } else if (version == 12) {
            migration = &MigrationTwelve;
        } else if (version == 13) {
            migration = &MigrationThirteen;
        } else if (version == 14) {
            migration = &MigrationFourteen;
        } else if (version == 15) {
            migration = &MigrationFifteen;
        } else if (version == 16) {
            migration = &MigrationSixteen;
        } else if (version == 17) {
            migration = &MigrationSeventeen;
        } else if (version == 18) {
            migration = &MigrationEighteen;
        } else if (version == 19) {
            migration = &MigrationNineteen;
        } else if (version == 20) {
            migration = &MigrationTwenty;
        } else if (version == 21) {
            static const QStringList fileMigration{
                QStringLiteral("CREATE TABLE log_file (id INTEGER PRIMARY KEY CHECK(id = 1), version INTEGER NOT NULL)"),
                QStringLiteral("CREATE TABLE log_settings (key TEXT PRIMARY KEY, value BLOB NOT NULL)"),
                QStringLiteral("CREATE TABLE log_assets (key TEXT PRIMARY KEY, contents BLOB NOT NULL)")
            };
            migration = &fileMigration;
        }
        if (migration == nullptr) {
            setError(error, QStringLiteral("No migration is available for schema version %1.")
                                .arg(version));
            return false;
        }

        if (!m_database.transaction()) {
            setError(error, m_database.lastError().text());
            return false;
        }
        for (const QString &statement : *migration) {
            QSqlQuery query(m_database);
            if (!query.exec(statement)) {
                const QString failure = rollbackError(m_database, queryError(query));
                setError(error, failure);
                return false;
            }
        }

        if (version == 5 && !backfillLegacyExchanges(m_database, error)) {
            const QString failure = rollbackError(
                m_database, error != nullptr ? *error
                                             : QStringLiteral("Legacy exchange backfill failed."));
            setError(error, failure);
            return false;
        }
        if (version == 7 && !backfillSyncIdentities(m_database, error)) {
            const QString failure = rollbackError(
                m_database, error != nullptr ? *error
                                             : QStringLiteral("Sync identity backfill failed."));
            setError(error, failure);
            return false;
        }

        QSqlQuery insertVersion(m_database);
        insertVersion.prepare(QStringLiteral(
            "INSERT INTO schema_migrations(version, applied_at_utc) VALUES(?, ?)"));
        insertVersion.addBindValue(version);
        insertVersion.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
        if (!insertVersion.exec()) {
            const QString failure =
                rollbackError(m_database, queryError(insertVersion));
            setError(error, failure);
            return false;
        }
        if (!m_database.commit()) {
            setError(error, rollbackError(m_database, m_database.lastError().text()));
            return false;
        }
    }

    QSqlQuery marker(m_database);
    if (!marker.exec(QStringLiteral("SELECT version FROM log_file WHERE id = 1"))) {
        setError(error, queryError(marker));
        return false;
    }
    m_standaloneLog = marker.next();
    return true;
}

bool SqliteRepository::backupTo(const QString &filePath, QString *error) const
{
    clearError(error);
    if (!m_database.isValid() || !m_database.isOpen()) {
        setError(error, QStringLiteral("SQLite database is not open."));
        return false;
    }
    const QFileInfo destinationInfo(filePath);
    if (filePath.trimmed().isEmpty() || destinationInfo.exists()
        || !destinationInfo.dir().exists()) {
        setError(error, QStringLiteral(
                            "Backup path must name a new file in an existing directory."));
        return false;
    }
    QSqlQuery databaseList(m_database);
    if (!databaseList.exec(QStringLiteral("PRAGMA database_list"))) {
        setError(error, queryError(databaseList));
        return false;
    }
    QString sourcePath;
    while (databaseList.next()) {
        if (databaseList.value(1).toString() == QStringLiteral("main")) {
            sourcePath = databaseList.value(2).toString();
            break;
        }
    }
    databaseList.finish();
    if (sourcePath.isEmpty()) {
        setError(error, QStringLiteral("Cannot back up an in-memory SQLite database."));
        return false;
    }
    const QString absoluteDestination = destinationInfo.absoluteFilePath();
    if (QFileInfo(sourcePath).absoluteFilePath() == absoluteDestination) {
        setError(error, QStringLiteral("Backup destination is the active database."));
        return false;
    }
    const QString escapedDestination =
        QString(absoluteDestination).replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery backup(m_database);
    if (!backup.exec(QStringLiteral("VACUUM main INTO '%1'").arg(escapedDestination))) {
        setError(error, queryError(backup));
        return false;
    }
    backup.finish();

    const QString connectionName = QStringLiteral("logger-backup-check-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool valid = false;
    {
        QSqlDatabase checkDatabase = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        checkDatabase.setDatabaseName(absoluteDestination);
        if (!checkDatabase.open()) {
            setError(error, checkDatabase.lastError().text());
        } else {
            valid = verifyLoggerDatabase(checkDatabase, error);
            checkDatabase.close();
        }
        checkDatabase = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (!valid) {
        QFile::remove(absoluteDestination);
        return false;
    }
    return true;
}

bool SqliteRepository::restoreFromBackup(const QString &databasePath,
                                         const QString &backupPath,
                                         QString *error)
{
    clearError(error);
    const QFileInfo sourceInfo(backupPath);
    const QFileInfo targetInfo(databasePath);
    if (!sourceInfo.isFile() || !targetInfo.dir().exists()
        || sourceInfo.absoluteFilePath() == targetInfo.absoluteFilePath()) {
        setError(error, QStringLiteral("Backup or destination path is invalid."));
        return false;
    }
    QLockFile targetLock(targetInfo.absoluteFilePath() + QStringLiteral(".lock"));
    if (!targetLock.tryLock(0)) {
        setError(error, QStringLiteral("Close the log before restoring its backup."));
        return false;
    }
    const QString validationConnection = QStringLiteral("logger-restore-check-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool valid = false;
    {
        QSqlDatabase checkDatabase = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), validationConnection);
        checkDatabase.setDatabaseName(sourceInfo.absoluteFilePath());
        if (!checkDatabase.open()) {
            setError(error, checkDatabase.lastError().text());
        } else {
            valid = verifyLoggerDatabase(checkDatabase, error);
            checkDatabase.close();
        }
        checkDatabase = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(validationConnection);
    if (!valid) {
        return false;
    }

    const QString temporaryPath = targetInfo.absoluteFilePath()
        + QStringLiteral(".restore-%1")
              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QFile::copy(sourceInfo.absoluteFilePath(), temporaryPath)) {
        setError(error, QStringLiteral("Could not stage backup for restoration."));
        return false;
    }
    const QString safetyPath = targetInfo.absoluteFilePath()
        + QStringLiteral(".before-restore-%1.sqlite")
              .arg(QDateTime::currentDateTimeUtc().toString(
                  QStringLiteral("yyyyMMdd-HHmmss")));
    const bool hadTarget = targetInfo.exists();
    if (hadTarget && !QFile::rename(targetInfo.absoluteFilePath(), safetyPath)) {
        QFile::remove(temporaryPath);
        setError(error, QStringLiteral("Could not preserve the current database before restore."));
        return false;
    }
    QStringList preservedSidecars;
    for (const QString &suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString sidecarPath = targetInfo.absoluteFilePath() + suffix;
        if (!QFileInfo::exists(sidecarPath)) {
            continue;
        }
        const QString preservedPath = safetyPath + suffix;
        if (!QFile::rename(sidecarPath, preservedPath)) {
            for (const QString &preserved : preservedSidecars) {
                QFile::rename(preserved, targetInfo.absoluteFilePath()
                                             + preserved.mid(safetyPath.size()));
            }
            if (hadTarget) {
                QFile::rename(safetyPath, targetInfo.absoluteFilePath());
            }
            QFile::remove(temporaryPath);
            setError(error, QStringLiteral("Could not preserve SQLite recovery sidecars."));
            return false;
        }
        preservedSidecars.append(preservedPath);
    }
    if (!QFile::rename(temporaryPath, targetInfo.absoluteFilePath())) {
        for (const QString &preserved : preservedSidecars) {
            QFile::rename(preserved, targetInfo.absoluteFilePath()
                                         + preserved.mid(safetyPath.size()));
        }
        if (hadTarget) {
            QFile::rename(safetyPath, targetInfo.absoluteFilePath());
        }
        QFile::remove(temporaryPath);
        setError(error, QStringLiteral("Could not install the restored database."));
        return false;
    }
    return true;
}

bool SqliteRepository::ensureGeneralLog(QString *error)
{
    clearError(error);
    if (m_standaloneLog) {
        return validateStandaloneLog(error);
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO contests(code, name, description, log_uuid) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(code) DO NOTHING"));
    query.addBindValue(QStringLiteral("GENERAL"));
    query.addBindValue(QStringLiteral("General QSO Logging"));
    query.addBindValue(QStringLiteral(
        "General-purpose log. Contest rules are not configured in this stage."));
    query.addBindValue(newUuid());
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    return true;
}

QVector<Contest> SqliteRepository::contests(QString *error) const
{
    clearError(error);
    QVector<Contest> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id, log_uuid, code, name, description, starts_at_utc, ends_at_utc, "
            "station_callsign, properties_json "
            "FROM contests ORDER BY name COLLATE NOCASE"))) {
        setError(error, queryError(query));
        return result;
    }

    while (query.next()) {
        Contest contest;
        contest.id = query.value(QStringLiteral("id")).toLongLong();
        contest.logUuid = query.value(QStringLiteral("log_uuid")).toString();
        contest.code = query.value(QStringLiteral("code")).toString();
        contest.name = query.value(QStringLiteral("name")).toString();
        contest.description = query.value(QStringLiteral("description")).toString();
        const QString startsAt = query.value(QStringLiteral("starts_at_utc")).toString();
        const QString endsAt = query.value(QStringLiteral("ends_at_utc")).toString();
        if (!startsAt.isEmpty()) {
            contest.startsAtUtc = QDateTime::fromString(startsAt, Qt::ISODate).toUTC();
        }
        if (!endsAt.isEmpty()) {
            contest.endsAtUtc = QDateTime::fromString(endsAt, Qt::ISODate).toUTC();
        }
        contest.stationCallsign =
            query.value(QStringLiteral("station_callsign")).toString();
        contest.properties = propertiesFromJson(
            query.value(QStringLiteral("properties_json")).toString());
        result.append(std::move(contest));
    }
    return result;
}

QString SqliteRepository::localStationUuid(QString *error) const
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT value FROM sync_metadata WHERE key = 'local_station_uuid'"));
    if (!query.exec()) {
        setError(error, queryError(query));
        return {};
    }
    if (!query.next()) {
        setError(error, QStringLiteral("Local station identity is not initialized."));
        return {};
    }
    return query.value(0).toString();
}

bool SqliteRepository::setSynchronizationEnabled(
    const QString &logUuid, bool enabled, QString *error)
{
    clearError(error);
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required."));
        return false;
    }
    return withSavepoint(m_database, [this, &logUuid, enabled](QString *operationError) {
        QSqlQuery log(m_database);
        log.prepare(QStringLiteral("SELECT 1 FROM contests WHERE log_uuid = ?"));
        log.addBindValue(logUuid);
        if (!log.exec()) {
            setError(operationError, queryError(log));
            return false;
        }
        if (!log.next()) {
            setError(operationError, QStringLiteral("Log %1 does not exist.")
                                         .arg(logUuid));
            return false;
        }
        log.finish();

        QSqlQuery state(m_database);
        state.prepare(QStringLiteral(
            "INSERT INTO sync_state(log_uuid, enabled, updated_at_utc) VALUES(?, ?, ?) "
            "ON CONFLICT(log_uuid) DO UPDATE SET "
            "enabled = excluded.enabled, updated_at_utc = excluded.updated_at_utc"));
        state.addBindValue(logUuid);
        state.addBindValue(enabled ? 1 : 0);
        state.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
        if (!state.exec()) {
            setError(operationError, queryError(state));
            return false;
        }
        return true;
    }, error);
}

bool SqliteRepository::setHostedLog(const QString &logUuid, QString *error)
{
    clearError(error);
    if (logUuid.isEmpty()) {
        m_hostedLogUuid.clear();
        return true;
    }
    if (!isSyncUuid(logUuid)) {
        setError(error, QStringLiteral("Invalid hosted log UUID."));
        return false;
    }
    const bool success = withTransaction([this, &logUuid](QString *operationError) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "SELECT q.*, c.log_uuid, c.definition_yaml, '' AS sync_status "
            "FROM qsos q JOIN contests c ON c.id = q.contest_id "
            "WHERE c.log_uuid = ? AND NOT EXISTS (SELECT 1 FROM qso_history h "
            "WHERE h.log_uuid = c.log_uuid AND h.qso_uuid = q.qso_uuid "
            "AND h.qso_version = q.qso_version) ORDER BY q.id"));
        query.addBindValue(logUuid);
        if (!query.exec()) {
            setError(operationError, queryError(query));
            return false;
        }
        QVector<QPair<Qso, bool>> pending;
        while (query.next()) {
            pending.append({readQso(query),
                            query.value(QStringLiteral("is_deleted")).toBool()});
        }
        query.finish();
        for (const auto &entry : pending) {
            if (!enqueueQsoOperation(m_database, entry.first,
                    entry.second ? QStringLiteral("qso.delete")
                                 : QStringLiteral("qso.create"),
                    0, operationError, true)) {
                return false;
            }
        }
        QSqlQuery accepted(m_database);
        accepted.prepare(QStringLiteral(
            "UPDATE sync_outbox SET status = 'accepted' "
            "WHERE log_uuid = ? AND status IN ('pending', 'in-flight')"));
        accepted.addBindValue(logUuid);
        if (!accepted.exec()) {
            setError(operationError, queryError(accepted));
            return false;
        }
        return true;
    }, error);
    if (success) {
        m_hostedLogUuid = logUuid;
    }
    return success;
}

bool SqliteRepository::bindToServerLog(
    const QString &localLogUuid, const QString &serverLogUuid, QString *error)
{
    clearError(error);
    if (!isSyncUuid(localLogUuid) || !isSyncUuid(serverLogUuid)) {
        setError(error, QStringLiteral("Invalid local or server log UUID."));
        return false;
    }
    if (localLogUuid == serverLogUuid) {
        return true;
    }
    return withTransaction([this, &localLogUuid, &serverLogUuid](QString *operationError) {
        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral(
            "SELECT 1 FROM contests WHERE log_uuid = ? UNION ALL "
            "SELECT 1 FROM sync_log_state WHERE log_uuid = ? AND current_revision > 0 "
            "UNION ALL SELECT 1 FROM sync_state WHERE log_uuid = ? AND cursor > 0"));
        existing.addBindValue(serverLogUuid);
        existing.addBindValue(localLogUuid);
        existing.addBindValue(localLogUuid);
        if (!existing.exec()) {
            setError(operationError, queryError(existing));
            return false;
        }
        if (existing.next()) {
            setError(operationError, QStringLiteral(
                "This log already belongs to another server, or the server log "
                "is already open locally. Open the corresponding log."));
            return false;
        }
        existing.finish();
        QSqlQuery defer(m_database);
        if (!defer.exec(QStringLiteral("PRAGMA defer_foreign_keys = ON"))) {
            setError(operationError, queryError(defer));
            return false;
        }
        // Keep local rows, pending operations and audit records in the same log.
        for (const QString &table : m_database.tables()) {
            QSqlQuery columns(m_database);
            if (!columns.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table))) {
                setError(operationError, queryError(columns));
                return false;
            }
            QStringList jsonColumns;
            bool hasLogUuid = false;
            while (columns.next()) {
                const QString name = columns.value(1).toString();
                hasLogUuid = hasLogUuid || name == QStringLiteral("log_uuid");
                if (name.endsWith(QStringLiteral("_json"))) {
                    jsonColumns.append(name);
                }
            }
            columns.finish();
            if (!hasLogUuid) {
                continue;
            }
            QStringList assignments{QStringLiteral("log_uuid = ?")};
            for (const QString &column : jsonColumns) {
                assignments.append(QStringLiteral("\"%1\" = replace(\"%1\", ?, ?)").arg(column));
            }
            QSqlQuery update(m_database);
            update.prepare(QStringLiteral("UPDATE \"%1\" SET %2 WHERE log_uuid = ?")
                               .arg(table, assignments.join(QStringLiteral(", "))));
            update.addBindValue(serverLogUuid);
            for (qsizetype i = 0; i < jsonColumns.size(); ++i) {
                update.addBindValue(localLogUuid);
                update.addBindValue(serverLogUuid);
            }
            update.addBindValue(localLogUuid);
            if (!update.exec()) {
                setError(operationError, queryError(update));
                return false;
            }
        }
        return enqueueExistingQsosForSync(serverLogUuid, operationError);
    }, error);
}

bool SqliteRepository::enqueueExistingQsosForSync(
    const QString &logUuid, QString *error)
{
    clearError(error);
    if (!isSyncUuid(logUuid)) {
        setError(error, QStringLiteral("Log UUID is invalid."));
        return false;
    }
    return withSavepoint(m_database, [this, &logUuid](QString *operationError) {
        QSqlQuery contest(m_database);
        contest.prepare(QStringLiteral(
            "SELECT id FROM contests WHERE log_uuid = ?"));
        contest.addBindValue(logUuid);
        if (!contest.exec() || !contest.next()) {
            setError(operationError, contest.lastError().isValid()
                ? queryError(contest)
                : QStringLiteral("Log does not exist."));
            return false;
        }
        const qint64 contestId = contest.value(0).toLongLong();
        contest.finish();
        const QString stationUuid = localStationUuid(operationError);
        if (stationUuid.isEmpty()) {
            return false;
        }
        const QVector<Qso> qsos = qsosForContest(contestId, 0, operationError);
        if (operationError != nullptr && !operationError->isEmpty()) {
            return false;
        }
        for (const Qso &qso : qsos) {
            if (qso.stationUuid != stationUuid) {
                setError(operationError, QStringLiteral(
                    "Log contains QSO from another station; historical import "
                    "requires administrator authorization."));
                return false;
            }
        }
        for (const Qso &qso : qsos) {
            QSqlQuery existing(m_database);
            existing.prepare(QStringLiteral(
                "SELECT status FROM sync_outbox WHERE log_uuid = ? "
                "AND entity_uuid = ? ORDER BY created_at_utc DESC LIMIT 1"));
            existing.addBindValue(logUuid);
            existing.addBindValue(qso.qsoUuid);
            if (!existing.exec()) {
                setError(operationError, queryError(existing));
                return false;
            }
            if (existing.next()) {
                existing.finish();
                continue;
            }
            existing.finish();
            if (!enqueueQsoOperation(m_database, qso,
                                     QStringLiteral("qso.create"), 0,
                                     operationError)) {
                return false;
            }
        }
        return true;
    }, error);
}

bool SqliteRepository::synchronizationEnabled(
    const QString &logUuid, QString *error) const
{
    clearError(error);
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required."));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT enabled FROM sync_state WHERE log_uuid = ?"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    if (!query.next()) {
        return false;
    }
    return query.value(0).toBool();
}

qint64 SqliteRepository::syncCursor(const QString &logUuid, QString *error) const
{
    clearError(error);
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required."));
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT cursor FROM sync_state WHERE log_uuid = ?"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return 0;
    }
    return query.next() ? query.value(0).toLongLong() : 0;
}

bool SqliteRepository::setSyncCursor(
    const QString &logUuid, qint64 cursor, QString *error)
{
    clearError(error);
    if (logUuid.trimmed().isEmpty() || cursor < 0) {
        setError(error, QStringLiteral("Log UUID and non-negative cursor are required."));
        return false;
    }
    return withSavepoint(m_database, [this, &logUuid, cursor](QString *operationError) {
        QSqlQuery log(m_database);
        log.prepare(QStringLiteral("SELECT 1 FROM contests WHERE log_uuid = ?"));
        log.addBindValue(logUuid);
        if (!log.exec()) {
            setError(operationError, queryError(log));
            return false;
        }
        if (!log.next()) {
            setError(operationError, QStringLiteral("Log %1 does not exist.")
                                         .arg(logUuid));
            return false;
        }
        log.finish();

        QSqlQuery state(m_database);
        state.prepare(QStringLiteral(
            "INSERT INTO sync_state(log_uuid, enabled, cursor, updated_at_utc) "
            "VALUES(?, 0, ?, ?) ON CONFLICT(log_uuid) DO UPDATE SET "
            "cursor = excluded.cursor, updated_at_utc = excluded.updated_at_utc "
            "WHERE excluded.cursor >= sync_state.cursor"));
        state.addBindValue(logUuid);
        state.addBindValue(cursor);
        state.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
        if (!state.exec()) {
            setError(operationError, queryError(state));
            return false;
        }
        if (state.numRowsAffected() != 1) {
            setError(operationError, QStringLiteral("Sync cursor cannot move backwards."));
            return false;
        }
        return true;
    }, error);
}

QVector<SyncOutboxOperation> SqliteRepository::syncOutbox(
    const QString &logUuid, bool includeTerminal, QString *error, int limit,
    bool readyOnly) const
{
    clearError(error);
    QVector<SyncOutboxOperation> result;
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required."));
        return result;
    }
    QSqlQuery query(m_database);
    const QString select = QStringLiteral(
        "SELECT operation_uuid, log_uuid, entity_uuid, operation, payload_json, "
        "created_at_utc, status, attempts, last_error FROM sync_outbox AS candidate "
        "WHERE log_uuid = ? ");
    if (limit < 0) {
        setError(error, QStringLiteral("Sync outbox limit cannot be negative."));
        return result;
    }
    query.prepare(select + (readyOnly
        ? QStringLiteral(
            "AND status = 'pending' AND NOT EXISTS ("
            "SELECT 1 FROM sync_outbox AS predecessor "
            "WHERE predecessor.log_uuid = candidate.log_uuid "
            "AND predecessor.entity_uuid = candidate.entity_uuid "
            "AND predecessor.rowid < candidate.rowid "
            "AND predecessor.status NOT IN ('accepted', 'pending')) ORDER BY rowid")
        : includeTerminal
        ? QStringLiteral("ORDER BY rowid")
        : QStringLiteral("AND status <> 'accepted' "
                         "ORDER BY rowid"))
        + (limit > 0 ? QStringLiteral(" LIMIT ?") : QString()));
    query.addBindValue(logUuid);
    if (limit > 0) {
        query.addBindValue(limit);
    }
    if (!query.exec()) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        SyncOutboxOperation operation;
        operation.operationUuid = query.value(0).toString();
        operation.logUuid = query.value(1).toString();
        operation.entityUuid = query.value(2).toString();
        operation.operation = query.value(3).toString();
        operation.payloadJson = query.value(4).toString();
        const QJsonDocument payload = QJsonDocument::fromJson(
            operation.payloadJson.toUtf8());
        if (payload.isObject()) {
            QJsonObject operationJson = payload.object();
            QJsonObject qso = operationJson.value(QStringLiteral("qso")).toObject();
            if (!qso.isEmpty() && !qso.contains(QStringLiteral("is_invalid"))) {
                qso.insert(QStringLiteral("is_invalid"), false);
                operationJson.insert(QStringLiteral("qso"), qso);
                operation.payloadJson = QString::fromUtf8(
                    QJsonDocument(operationJson).toJson(QJsonDocument::Compact));
            }
        }
        operation.createdAtUtc = QDateTime::fromString(
            query.value(5).toString(), Qt::ISODateWithMs).toUTC();
        operation.status = query.value(6).toString();
        operation.attempts = query.value(7).toInt();
        operation.lastError = query.value(8).toString();
        result.append(std::move(operation));
    }
    return result;
}

QVector<SyncConflict> SqliteRepository::syncConflicts(
    const QString &logUuid, QString *error) const
{
    clearError(error);
    QVector<SyncConflict> result;
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required."));
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT operation_uuid, log_uuid, entity_uuid, local_payload_json, "
        "remote_payload_json, created_at_utc FROM sync_conflicts "
        "WHERE log_uuid = ? AND resolved_at_utc = '' "
        "ORDER BY created_at_utc, operation_uuid"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        SyncConflict conflict;
        conflict.operationUuid = query.value(0).toString();
        conflict.logUuid = query.value(1).toString();
        conflict.entityUuid = query.value(2).toString();
        conflict.localPayloadJson = query.value(3).toString();
        conflict.remotePayloadJson = query.value(4).toString();
        conflict.createdAtUtc = QDateTime::fromString(
            query.value(5).toString(), Qt::ISODateWithMs).toUTC();
        result.append(std::move(conflict));
    }
    return result;
}

bool SqliteRepository::setSyncOperationState(
    const QString &operationUuid, const QString &state,
    const QString &errorMessage, QString *error)
{
    clearError(error);
    static const QStringList allowedStates{
        QStringLiteral("in-flight"), QStringLiteral("accepted"),
        QStringLiteral("failed")
    };
    if (operationUuid.trimmed().isEmpty() || !allowedStates.contains(state)) {
        setError(error, QStringLiteral("Operation UUID or target state is invalid."));
        return false;
    }
    if (state == QStringLiteral("failed") && errorMessage.trimmed().isEmpty()) {
        setError(error, QStringLiteral("A failure state requires an error message."));
        return false;
    }

    QSqlQuery current(m_database);
    current.prepare(QStringLiteral(
        "SELECT log_uuid, status FROM sync_outbox WHERE operation_uuid = ?"));
    current.addBindValue(operationUuid);
    if (!current.exec()) {
        setError(error, queryError(current));
        return false;
    }
    if (!current.next()) {
        setError(error, QStringLiteral("Sync operation %1 does not exist.")
                            .arg(operationUuid));
        return false;
    }
    const QString logUuid = current.value(0).toString();
    const QString currentState = current.value(1).toString();
    current.finish();

    const bool permitted =
        (state == QStringLiteral("in-flight") && currentState == QStringLiteral("pending"))
        || ((state == QStringLiteral("accepted") || state == QStringLiteral("failed"))
            && (currentState == QStringLiteral("pending")
                || currentState == QStringLiteral("in-flight")));
    if (!permitted) {
        setError(error, QStringLiteral("Invalid sync operation state transition: %1 -> %2.")
                            .arg(currentState, state));
        return false;
    }

    return withSavepoint(m_database,
        [this, &operationUuid, &state, &errorMessage, &currentState, &logUuid]
        (QString *operationError) {
            QSqlQuery update(m_database);
            update.prepare(QStringLiteral(
                "UPDATE sync_outbox SET status = ?, "
                "attempts = attempts + CASE WHEN ? = 'in-flight' THEN 1 ELSE 0 END, "
                "last_error = ? WHERE operation_uuid = ? AND status = ?"));
            update.addBindValue(state);
            update.addBindValue(state);
            update.addBindValue(state == QStringLiteral("failed")
                                    ? errorMessage.trimmed() : QStringLiteral(""));
            update.addBindValue(operationUuid);
            update.addBindValue(currentState);
            if (!update.exec()) {
                setError(operationError, queryError(update));
                return false;
            }
            if (update.numRowsAffected() != 1) {
                setError(operationError, QStringLiteral(
                    "Sync operation changed during state transition."));
                return false;
            }
            QSqlQuery syncState(m_database);
            syncState.prepare(QStringLiteral(
                "UPDATE sync_state SET last_error = ?, updated_at_utc = ? "
                "WHERE log_uuid = ?"));
            syncState.addBindValue(state == QStringLiteral("failed")
                                       ? errorMessage.trimmed() : QStringLiteral(""));
            syncState.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            syncState.addBindValue(logUuid);
            if (!syncState.exec()) {
                setError(operationError, queryError(syncState));
                return false;
            }
            return true;
        }, error);
}

bool SqliteRepository::retrySyncOperation(
    const QString &operationUuid, QString *error)
{
    clearError(error);
    if (operationUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Operation UUID is required."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &operationUuid](QString *operationError) {
            QSqlQuery update(m_database);
            update.prepare(QStringLiteral(
                "UPDATE sync_outbox SET status = 'pending', last_error = '' "
                "WHERE operation_uuid = ? AND status = 'failed'"));
            update.addBindValue(operationUuid);
            if (!update.exec()) {
                setError(operationError, queryError(update));
                return false;
            }
            if (update.numRowsAffected() != 1) {
                setError(operationError, QStringLiteral(
                    "Only a failed sync operation can be retried."));
                return false;
            }
            QSqlQuery state(m_database);
            state.prepare(QStringLiteral(
                "UPDATE sync_state SET last_error = '', updated_at_utc = ? "
                "WHERE log_uuid = (SELECT log_uuid FROM sync_outbox "
                "WHERE operation_uuid = ?)"));
            state.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            state.addBindValue(operationUuid);
            if (!state.exec()) {
                setError(operationError, queryError(state));
                return false;
            }
            return true;
        }, error);
}

bool SqliteRepository::recoverInFlightSyncOperations(
    const QString &logUuid, QString *error)
{
    clearError(error);
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required."));
        return false;
    }
    return withSavepoint(m_database, [this, &logUuid](QString *operationError) {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE sync_outbox SET status = 'pending', "
            "last_error = 'Recovered after interrupted synchronization' "
            "WHERE log_uuid = ? AND status = 'in-flight'"));
        update.addBindValue(logUuid);
        if (!update.exec()) {
            setError(operationError, queryError(update));
            return false;
        }
        QSqlQuery syncState(m_database);
        syncState.prepare(QStringLiteral(
            "UPDATE sync_state SET last_error = "
            "'Recovered interrupted synchronization', updated_at_utc = ? "
            "WHERE log_uuid = ? AND EXISTS(SELECT 1 FROM sync_outbox "
            "WHERE log_uuid = ? AND status = 'pending' "
            "AND last_error = 'Recovered after interrupted synchronization')"));
        syncState.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
        syncState.addBindValue(logUuid);
        syncState.addBindValue(logUuid);
        if (!syncState.exec()) {
            setError(operationError, queryError(syncState));
            return false;
        }
        return true;
    }, error);
}

bool SqliteRepository::saveSerialReservation(
    const SerialReservation &reservation, QString *error)
{
    clearError(error);
    if (!isSyncUuid(reservation.reservationUuid)
        || !isSyncUuid(reservation.logUuid)
        || !isSyncUuid(reservation.stationUuid)
        || reservation.scopeKey.trimmed().isEmpty()
        || reservation.serialNumber < 1
        || (reservation.status != QStringLiteral("reserved")
            && reservation.status != QStringLiteral("used")
            && reservation.status != QStringLiteral("cancelled"))
        || (reservation.status == QStringLiteral("used")
            ? !isSyncUuid(reservation.qsoUuid)
            : !reservation.qsoUuid.isEmpty())) {
        setError(error, QStringLiteral("Serial reservation data is invalid."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &reservation](QString *operationError) {
            const QString stationIdentity = localStationUuid(operationError);
            if (stationIdentity.isEmpty()) {
                return false;
            }
            if (reservation.stationUuid != stationIdentity) {
                setError(operationError, QStringLiteral(
                    "Serial reservation belongs to a different station."));
                return false;
            }

            QSqlQuery existing(m_database);
            existing.prepare(QStringLiteral(
                "SELECT log_uuid, station_uuid, scope_key, band, mode, "
                "serial_number, status, qso_uuid FROM serial_reservations "
                "WHERE reservation_uuid = ?"));
            existing.addBindValue(reservation.reservationUuid);
            if (!existing.exec()) {
                setError(operationError, queryError(existing));
                return false;
            }
            if (existing.next()) {
                const bool sameIdentity =
                    existing.value(0).toString() == reservation.logUuid
                    && existing.value(1).toString() == reservation.stationUuid
                    && existing.value(2).toString() == reservation.scopeKey
                    && existing.value(3).toString() == reservation.band
                    && existing.value(4).toString() == reservation.mode
                    && existing.value(5).toInt() == reservation.serialNumber;
                const QString currentStatus = existing.value(6).toString();
                const QString currentQsoUuid = existing.value(7).toString();
                existing.finish();
                if (!sameIdentity
                    || (currentStatus != reservation.status
                        && currentStatus != QStringLiteral("reserved"))
                    || (currentStatus == reservation.status
                        && currentStatus != QStringLiteral("reserved")
                        && currentQsoUuid != reservation.qsoUuid)) {
                    setError(operationError, QStringLiteral(
                        "Serial reservation conflicts with its stored state."));
                    return false;
                }
                if (currentStatus == reservation.status) {
                    return true;
                }
                QSqlQuery update(m_database);
                update.prepare(QStringLiteral(
                    "UPDATE serial_reservations SET status = ?, qso_uuid = ?, "
                    "updated_at_utc = ? WHERE reservation_uuid = ? "
                    "AND status = 'reserved'"));
                update.addBindValue(reservation.status);
                update.addBindValue(reservation.qsoUuid.isNull()
                    ? QStringLiteral("") : reservation.qsoUuid);
                update.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
                update.addBindValue(reservation.reservationUuid);
                if (!update.exec() || update.numRowsAffected() != 1) {
                    setError(operationError, queryError(update));
                    return false;
                }
                return true;
            }
            existing.finish();

            QSqlQuery insert(m_database);
            insert.prepare(QStringLiteral(
                "INSERT INTO serial_reservations(reservation_uuid, log_uuid, "
                "station_uuid, scope_key, band, mode, serial_number, status, "
                "qso_uuid, created_at_utc, updated_at_utc) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
            const QString now = utcString(QDateTime::currentDateTimeUtc());
            insert.addBindValue(reservation.reservationUuid);
            insert.addBindValue(reservation.logUuid);
            insert.addBindValue(reservation.stationUuid);
            insert.addBindValue(reservation.scopeKey);
            insert.addBindValue(reservation.band);
            insert.addBindValue(reservation.mode);
            insert.addBindValue(reservation.serialNumber);
            insert.addBindValue(reservation.status);
            insert.addBindValue(reservation.qsoUuid.isNull()
                ? QStringLiteral("") : reservation.qsoUuid);
            insert.addBindValue(now);
            insert.addBindValue(now);
            if (!insert.exec()) {
                setError(operationError, queryError(insert));
                return false;
            }
            return true;
        }, error);
}

std::optional<SerialReservation> SqliteRepository::serialReservation(
    const QString &logUuid, const QString &reservationUuid, QString *error) const
{
    clearError(error);
    if (!isSyncUuid(logUuid) || !isSyncUuid(reservationUuid)) {
        setError(error, QStringLiteral("Log and reservation UUIDs are invalid."));
        return std::nullopt;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT reservation_uuid, log_uuid, station_uuid, scope_key, band, mode, "
        "serial_number, status, qso_uuid, created_at_utc, updated_at_utc "
        "FROM serial_reservations "
        "WHERE log_uuid = ? AND reservation_uuid = ?"));
    query.addBindValue(logUuid);
    query.addBindValue(reservationUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return SerialReservation{
        query.value(0).toString(), query.value(1).toString(),
        query.value(2).toString(), query.value(3).toString(),
        query.value(4).toString(), query.value(5).toString(),
        query.value(6).toInt(), query.value(7).toString(),
        query.value(8).toString(),
        QDateTime::fromString(query.value(9).toString(), Qt::ISODateWithMs),
        QDateTime::fromString(query.value(10).toString(), Qt::ISODateWithMs)
    };
}

QVector<SerialReservation> SqliteRepository::serialReservations(
    const QString &logUuid, QString *error) const
{
    clearError(error);
    QVector<SerialReservation> result;
    if (!isSyncUuid(logUuid)) {
        setError(error, QStringLiteral("Log UUID is invalid."));
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT reservation_uuid, log_uuid, station_uuid, scope_key, band, mode, "
        "serial_number, status, qso_uuid, created_at_utc, updated_at_utc "
        "FROM serial_reservations "
        "WHERE log_uuid = ? ORDER BY serial_number"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        result.append(SerialReservation{
            query.value(0).toString(), query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(),
            query.value(4).toString(), query.value(5).toString(),
            query.value(6).toInt(), query.value(7).toString(),
            query.value(8).toString(),
            QDateTime::fromString(query.value(9).toString(), Qt::ISODateWithMs),
            QDateTime::fromString(query.value(10).toString(), Qt::ISODateWithMs)
        });
    }
    return result;
}

bool SqliteRepository::adjustCachedSerialReservation(
    const QString &logUuid, const QString &reservationUuid,
    int expectedSerial, int correctedSerial, QString *error)
{
    clearError(error);
    if (!isSyncUuid(logUuid) || !isSyncUuid(reservationUuid)
        || expectedSerial < 1 || correctedSerial < 1
        || expectedSerial == correctedSerial) {
        setError(error, QStringLiteral("Serial cache correction is invalid."));
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE serial_reservations SET serial_number = ?, updated_at_utc = ? "
        "WHERE log_uuid = ? AND reservation_uuid = ? AND serial_number = ? "
        "AND status = 'reserved' AND station_uuid = ?"));
    update.addBindValue(correctedSerial);
    update.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    update.addBindValue(logUuid);
    update.addBindValue(reservationUuid);
    update.addBindValue(expectedSerial);
    update.addBindValue(localStationUuid(error));
    if (error != nullptr && !error->isEmpty()) {
        return false;
    }
    if (!update.exec()) {
        setError(error, queryError(update));
        return false;
    }
    if (update.numRowsAffected() == 0) {
        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral(
            "SELECT serial_number, status FROM serial_reservations "
            "WHERE log_uuid = ? AND reservation_uuid = ? AND station_uuid = ?"));
        existing.addBindValue(logUuid);
        existing.addBindValue(reservationUuid);
        existing.addBindValue(localStationUuid(error));
        if (error != nullptr && !error->isEmpty()) {
            return false;
        }
        if (!existing.exec()) {
            setError(error, queryError(existing));
            return false;
        }
        if (existing.next()
            && (existing.value(0).toInt() != correctedSerial
                || existing.value(1).toString() != QStringLiteral("reserved"))) {
            setError(error, QStringLiteral(
                "Cached serial reservation changed before correction was applied."));
            return false;
        }
    }
    return true;
}

bool SqliteRepository::enqueueSerialReservationRequest(
    const SerialReservationRequest &request, QString *error)
{
    clearError(error);
    if (!isSyncUuid(request.logUuid) || !isSyncUuid(request.stationUuid)
        || !isSyncUuid(request.reservationUuid)
        || (request.operation != QStringLiteral("reserve_serial")
            && request.operation != QStringLiteral("cancel_serial"))
        || (request.operation == QStringLiteral("reserve_serial")
            ? (request.band.trimmed().isEmpty() || request.mode.trimmed().isEmpty())
            : (!request.band.isEmpty() || !request.mode.isEmpty()))) {
        setError(error, QStringLiteral("Serial reservation request is invalid."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &request](QString *operationError) {
            const QString localStation = localStationUuid(operationError);
            if (localStation.isEmpty()) {
                return false;
            }
            if (request.stationUuid != localStation) {
                setError(operationError, QStringLiteral(
                    "Serial reservation request belongs to a different station."));
                return false;
            }
            if (!synchronizationEnabled(request.logUuid, operationError)) {
                if (operationError != nullptr && operationError->isEmpty()) {
                    setError(operationError, QStringLiteral(
                        "Synchronization is not enabled for the serial reservation log."));
                }
                return false;
            }

            QSqlQuery insert(m_database);
            insert.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO serial_reservation_requests("
                "log_uuid, station_uuid, reservation_uuid, operation, band, mode, "
                "created_at_utc) VALUES(?, ?, ?, ?, ?, ?, ?)"));
            insert.addBindValue(request.logUuid);
            insert.addBindValue(request.stationUuid);
            insert.addBindValue(request.reservationUuid);
            insert.addBindValue(request.operation);
            insert.addBindValue(request.band.isNull()
                ? QStringLiteral("") : request.band);
            insert.addBindValue(request.mode.isNull()
                ? QStringLiteral("") : request.mode);
            insert.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            if (!insert.exec()) {
                setError(operationError, queryError(insert));
                return false;
            }

            QSqlQuery stored(m_database);
            stored.prepare(QStringLiteral(
                "SELECT station_uuid, band, mode FROM serial_reservation_requests "
                "WHERE log_uuid = ? AND reservation_uuid = ? AND operation = ?"));
            stored.addBindValue(request.logUuid);
            stored.addBindValue(request.reservationUuid);
            stored.addBindValue(request.operation);
            if (!stored.exec() || !stored.next()) {
                setError(operationError, stored.lastError().isValid()
                    ? queryError(stored)
                    : QStringLiteral("Serial reservation request was not persisted."));
                return false;
            }
            if (stored.value(0).toString() != request.stationUuid
                || stored.value(1).toString() != request.band
                || stored.value(2).toString() != request.mode) {
                setError(operationError, QStringLiteral(
                    "Serial reservation UUID was reused with different request data."));
                return false;
            }
            return true;
        }, error);
}

QVector<SerialReservationRequest>
SqliteRepository::pendingSerialReservationRequests(
    const QString &logUuid, const QString &stationUuid, QString *error) const
{
    clearError(error);
    QVector<SerialReservationRequest> result;
    if (!isSyncUuid(logUuid) || !isSyncUuid(stationUuid)) {
        setError(error, QStringLiteral("Log and station UUIDs are invalid."));
        return result;
    }
    const QString localStation = localStationUuid(error);
    if (localStation.isEmpty()) {
        return result;
    }
    if (stationUuid != localStation) {
        setError(error, QStringLiteral(
            "Serial reservation queue belongs to a different station."));
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT log_uuid, station_uuid, reservation_uuid, operation, band, mode, "
        "created_at_utc "
        "FROM serial_reservation_requests WHERE log_uuid = ? AND station_uuid = ? "
        "ORDER BY created_at_utc, reservation_uuid, operation"));
    query.addBindValue(logUuid);
    query.addBindValue(stationUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        result.append(SerialReservationRequest{
            query.value(0).toString(), query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(),
            query.value(4).toString(), query.value(5).toString(),
            QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs)
        });
    }
    return result;
}

bool SqliteRepository::removeSerialReservationRequest(
    const QString &logUuid, const QString &reservationUuid,
    const QString &operation, QString *error)
{
    clearError(error);
    if (!isSyncUuid(logUuid) || !isSyncUuid(reservationUuid)
        || (operation != QStringLiteral("reserve_serial")
            && operation != QStringLiteral("cancel_serial"))) {
        setError(error, QStringLiteral("Serial reservation request identity is invalid."));
        return false;
    }
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM serial_reservation_requests WHERE log_uuid = ? "
        "AND reservation_uuid = ? AND operation = ?"));
    remove.addBindValue(logUuid);
    remove.addBindValue(reservationUuid);
    remove.addBindValue(operation);
    if (!remove.exec()) {
        setError(error, queryError(remove));
        return false;
    }
    return true;
}

bool SqliteRepository::setSerialReservationStatus(
    const QString &logUuid, const QString &reservationUuid,
    const QString &status, const QString &qsoUuid, QString *error)
{
    clearError(error);
    if (!isSyncUuid(logUuid) || !isSyncUuid(reservationUuid)
        || (status != QStringLiteral("used")
            && status != QStringLiteral("cancelled"))
        || (status == QStringLiteral("used")
            ? !isSyncUuid(qsoUuid) : !qsoUuid.isEmpty())) {
        setError(error, QStringLiteral("Serial reservation status is invalid."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &logUuid, &reservationUuid, &status, &qsoUuid]
        (QString *operationError) {
            QSqlQuery update(m_database);
            update.prepare(QStringLiteral(
                "UPDATE serial_reservations SET status = ?, qso_uuid = ?, "
                "updated_at_utc = ? WHERE log_uuid = ? AND reservation_uuid = ? "
                "AND status = 'reserved'"));
            update.addBindValue(status);
            update.addBindValue(qsoUuid.isNull() ? QStringLiteral("") : qsoUuid);
            update.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            update.addBindValue(logUuid);
            update.addBindValue(reservationUuid);
            if (!update.exec()) {
                setError(operationError, queryError(update));
                return false;
            }
            if (update.numRowsAffected() == 1) {
                return true;
            }
            QSqlQuery existing(m_database);
            existing.prepare(QStringLiteral(
                "SELECT status, qso_uuid FROM serial_reservations "
                "WHERE log_uuid = ? AND reservation_uuid = ?"));
            existing.addBindValue(logUuid);
            existing.addBindValue(reservationUuid);
            if (!existing.exec()) {
                setError(operationError, queryError(existing));
                return false;
            }
            if (existing.next()
                && existing.value(0).toString() == status
                && existing.value(1).toString() == qsoUuid) {
                return true;
            }
            setError(operationError, QStringLiteral(
                "Serial reservation does not exist or has already been finalized."));
            return false;
        }, error);
}

bool SqliteRepository::recordSyncConflict(
    const QString &operationUuid, const QString &remotePayloadJson,
    QString *error)
{
    clearError(error);
    if (operationUuid.trimmed().isEmpty()
        || !QJsonDocument::fromJson(remotePayloadJson.toUtf8()).isObject()) {
        setError(error, QStringLiteral("Operation UUID and valid remote JSON object are required."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &operationUuid, &remotePayloadJson](QString *operationError) {
            QSqlQuery operation(m_database);
            operation.prepare(QStringLiteral(
                "SELECT log_uuid, entity_uuid, payload_json, status "
                "FROM sync_outbox WHERE operation_uuid = ?"));
            operation.addBindValue(operationUuid);
            if (!operation.exec()) {
                setError(operationError, queryError(operation));
                return false;
            }
            if (!operation.next()) {
                setError(operationError, QStringLiteral(
                    "Sync operation %1 does not exist.").arg(operationUuid));
                return false;
            }
            const QString logUuid = operation.value(0).toString();
            const QString entityUuid = operation.value(1).toString();
            const QString payload = operation.value(2).toString();
            const QString status = operation.value(3).toString();
            operation.finish();
            if (status != QStringLiteral("pending")
                && status != QStringLiteral("in-flight")) {
                setError(operationError, QStringLiteral(
                    "Only a pending or in-flight operation can become a conflict."));
                return false;
            }

            QSqlQuery conflict(m_database);
            conflict.prepare(QStringLiteral(
                "INSERT INTO sync_conflicts(operation_uuid, log_uuid, entity_uuid, "
                "local_payload_json, remote_payload_json, created_at_utc) "
                "VALUES(?, ?, ?, ?, ?, ?)"));
            conflict.addBindValue(operationUuid);
            conflict.addBindValue(logUuid);
            conflict.addBindValue(entityUuid);
            conflict.addBindValue(payload);
            conflict.addBindValue(remotePayloadJson);
            conflict.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            if (!conflict.exec()) {
                setError(operationError, queryError(conflict));
                return false;
            }
            QSqlQuery update(m_database);
            update.prepare(QStringLiteral(
                "UPDATE sync_outbox SET status = 'conflict', "
                "last_error = 'Server reported a version conflict' "
                "WHERE operation_uuid = ? AND status = ?"));
            update.addBindValue(operationUuid);
            update.addBindValue(status);
            if (!update.exec() || update.numRowsAffected() != 1) {
                setError(operationError, update.lastError().isValid()
                    ? queryError(update)
                    : QStringLiteral("Sync operation changed while recording conflict."));
                return false;
            }
            QSqlQuery state(m_database);
            state.prepare(QStringLiteral(
                "UPDATE sync_state SET last_error = ?, updated_at_utc = ? "
                "WHERE log_uuid = ?"));
            state.addBindValue(QStringLiteral("Server reported a version conflict"));
            state.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            state.addBindValue(logUuid);
            if (!state.exec()) {
                setError(operationError, queryError(state));
                return false;
            }
            return true;
        }, error);
}

bool SqliteRepository::resolveSyncConflictAcceptServer(
    const QString &operationUuid, QString *error)
{
    clearError(error);
    if (operationUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Operation UUID is required."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &operationUuid](QString *operationError) {
            QSqlQuery lookup(m_database);
            lookup.prepare(QStringLiteral(
                "SELECT c.log_uuid, c.entity_uuid, c.local_payload_json, "
                "c.remote_payload_json, o.status FROM sync_conflicts c "
                "JOIN sync_outbox o ON o.operation_uuid = c.operation_uuid "
                "WHERE c.operation_uuid = ? AND c.resolved_at_utc = ''"));
            lookup.addBindValue(operationUuid);
            if (!lookup.exec()) {
                setError(operationError, queryError(lookup));
                return false;
            }
            if (!lookup.next()) {
                setError(operationError, QStringLiteral(
                    "Unresolved synchronization conflict does not exist."));
                return false;
            }
            const QString logUuid = lookup.value(0).toString();
            const QString entityUuid = lookup.value(1).toString();
            const QString localPayload = lookup.value(2).toString();
            const QString remotePayload = lookup.value(3).toString();
            const QString operationState = lookup.value(4).toString();
            lookup.finish();
            if (operationState != QStringLiteral("conflict")) {
                setError(operationError, QStringLiteral(
                    "Conflict operation is no longer in conflict state."));
                return false;
            }
            const QJsonDocument remoteDocument =
                QJsonDocument::fromJson(remotePayload.toUtf8());
            if (!remoteDocument.isObject()) {
                setError(operationError, QStringLiteral(
                    "Stored server conflict version is invalid JSON."));
                return false;
            }
            const QJsonObject remote = remoteDocument.object();
            const QStringList requiredStrings{
                QStringLiteral("qso_uuid"), QStringLiteral("log_uuid"),
                QStringLiteral("station_uuid"), QStringLiteral("callsign"),
                QStringLiteral("station_callsign"), QStringLiteral("band"),
                QStringLiteral("timestamp_utc"), QStringLiteral("mode"),
                QStringLiteral("sent_exchange"), QStringLiteral("received_exchange")
            };
            for (const QString &key : requiredStrings) {
                if (!remote.value(key).isString()) {
                    setError(operationError, QStringLiteral(
                        "Server conflict version is missing field %1.").arg(key));
                    return false;
                }
            }
            const QString qsoUuid = remote.value(QStringLiteral("qso_uuid")).toString();
            const QString remoteLogUuid = remote.value(QStringLiteral("log_uuid")).toString();
            const QString remoteStationUuid =
                remote.value(QStringLiteral("station_uuid")).toString();
            const QDateTime timestamp = QDateTime::fromString(
                remote.value(QStringLiteral("timestamp_utc")).toString(),
                Qt::ISODateWithMs);
            const QJsonValue frequencyValue = remote.value(QStringLiteral("frequency_hz"));
            const QJsonValue versionValue = remote.value(QStringLiteral("qso_version"));
            const QJsonValue pointsValue = remote.value(QStringLiteral("points"));
            const auto validStringMap = [](const QJsonValue &value) {
                if (!value.isObject()) {
                    return false;
                }
                for (const QJsonValue &entry : value.toObject()) {
                    if (!entry.isString()) {
                        return false;
                    }
                }
                return true;
            };
            const auto stringMap = [](const QJsonObject &object) {
                QMap<QString, QString> fields;
                for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                    fields.insert(it.key(), it.value().toString());
                }
                return fields;
            };
            const double frequency = frequencyValue.toDouble(-1);
            const int version = versionValue.toInt(-1);
            const double points = pointsValue.toDouble(-1);
            if (!isSyncUuid(qsoUuid) || qsoUuid != entityUuid
                || remoteLogUuid != logUuid || !isSyncUuid(remoteStationUuid)
                || !timestamp.isValid()
                || !remote.value(QStringLiteral("is_deleted")).isBool()
                || !remote.value(QStringLiteral("is_invalid")).isBool()
                || !frequencyValue.isDouble() || !std::isfinite(frequency)
                || frequency < 0
                || frequency >= static_cast<double>(
                    std::numeric_limits<qint64>::max())
                || frequency != static_cast<qint64>(frequency)
                || version < 1 || !versionValue.isDouble()
                || !pointsValue.isDouble() || !std::isfinite(points) || points < 0
                || points > std::numeric_limits<int>::max()
                || points != static_cast<int>(points)
                || !validStringMap(remote.value(
                    QStringLiteral("sent_exchange_fields")))
                || !validStringMap(remote.value(
                    QStringLiteral("received_exchange_fields")))) {
                setError(operationError, QStringLiteral(
                    "Server conflict version has invalid identity or QSO fields."));
                return false;
            }

            QSqlQuery local(m_database);
            local.prepare(QStringLiteral(
                "SELECT q.id FROM qsos q JOIN contests c ON c.id = q.contest_id "
                "WHERE q.qso_uuid = ? AND c.log_uuid = ?"));
            local.addBindValue(entityUuid);
            local.addBindValue(logUuid);
            if (!local.exec()) {
                setError(operationError, queryError(local));
                return false;
            }
            if (!local.next()) {
                setError(operationError, QStringLiteral(
                    "Local QSO for the conflict no longer exists."));
                return false;
            }
            const qint64 localId = local.value(0).toLongLong();
            local.finish();

            QSqlQuery updateQso(m_database);
            updateQso.prepare(QStringLiteral(
                "UPDATE qsos SET callsign = ?, station_callsign = ?, band = ?, "
                "timestamp_utc = ?, frequency_hz = ?, mode = ?, sent_exchange = ?, "
                "received_exchange = ?, sent_exchange_json = ?, "
                "received_exchange_json = ?, points = ?, station_uuid = ?, "
                "qso_version = ?, is_deleted = ?, is_invalid = ? WHERE id = ?"));
            updateQso.addBindValue(
                remote.value(QStringLiteral("callsign")).toString().trimmed().toUpper());
            updateQso.addBindValue(remote.value(
                QStringLiteral("station_callsign")).toString().trimmed().toUpper());
            updateQso.addBindValue(
                remote.value(QStringLiteral("band")).toString().trimmed().toLower());
            updateQso.addBindValue(utcString(timestamp.toUTC()));
            updateQso.addBindValue(static_cast<qint64>(frequency));
            updateQso.addBindValue(
                remote.value(QStringLiteral("mode")).toString().trimmed().toUpper());
            updateQso.addBindValue(remote.value(
                QStringLiteral("sent_exchange")).toString());
            updateQso.addBindValue(remote.value(
                QStringLiteral("received_exchange")).toString());
            updateQso.addBindValue(exchangeJson(
                stringMap(remote.value(
                    QStringLiteral("sent_exchange_fields")).toObject())));
            updateQso.addBindValue(exchangeJson(
                stringMap(remote.value(
                    QStringLiteral("received_exchange_fields")).toObject())));
            updateQso.addBindValue(static_cast<int>(points));
            updateQso.addBindValue(remoteStationUuid);
            updateQso.addBindValue(version);
            updateQso.addBindValue(
                remote.value(QStringLiteral("is_deleted")).toBool() ? 1 : 0);
            updateQso.addBindValue(
                remote.value(QStringLiteral("is_invalid")).toBool() ? 1 : 0);
            updateQso.addBindValue(localId);
            if (!updateQso.exec() || updateQso.numRowsAffected() != 1) {
                setError(operationError, updateQso.lastError().isValid()
                    ? queryError(updateQso)
                    : QStringLiteral("Local QSO changed while resolving conflict."));
                return false;
            }

            QSqlQuery finishConflict(m_database);
            finishConflict.prepare(QStringLiteral(
                "UPDATE sync_conflicts SET resolved_at_utc = ? "
                "WHERE operation_uuid = ? AND resolved_at_utc = ''"));
            finishConflict.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            finishConflict.addBindValue(operationUuid);
            if (!finishConflict.exec() || finishConflict.numRowsAffected() != 1) {
                setError(operationError, finishConflict.lastError().isValid()
                    ? queryError(finishConflict)
                    : QStringLiteral("Conflict was resolved concurrently."));
                return false;
            }
            QSqlQuery finishOperation(m_database);
            finishOperation.prepare(QStringLiteral(
                "UPDATE sync_outbox SET status = 'accepted', last_error = '' "
                "WHERE operation_uuid = ? AND status = 'conflict'"));
            finishOperation.addBindValue(operationUuid);
            if (!finishOperation.exec() || finishOperation.numRowsAffected() != 1) {
                setError(operationError, finishOperation.lastError().isValid()
                    ? queryError(finishOperation)
                    : QStringLiteral("Conflict operation changed during resolution."));
                return false;
            }
            QSqlQuery audit(m_database);
            audit.prepare(QStringLiteral(
                "INSERT INTO sync_conflict_resolutions(operation_uuid, log_uuid, "
                "entity_uuid, resolution, local_payload_json, remote_payload_json, "
                "resolved_at_utc) VALUES(?, ?, ?, 'accept_server', ?, ?, ?)"));
            audit.addBindValue(operationUuid);
            audit.addBindValue(logUuid);
            audit.addBindValue(entityUuid);
            audit.addBindValue(localPayload);
            audit.addBindValue(remotePayload);
            audit.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            if (!audit.exec()) {
                setError(operationError, queryError(audit));
                return false;
            }
            QSqlQuery state(m_database);
            state.prepare(QStringLiteral(
                "UPDATE sync_state SET last_error = '', updated_at_utc = ? "
                "WHERE log_uuid = ? AND NOT EXISTS(SELECT 1 FROM sync_outbox "
                "WHERE log_uuid = ? AND status = 'conflict')"));
            state.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            state.addBindValue(logUuid);
            state.addBindValue(logUuid);
            if (!state.exec()) {
                setError(operationError, queryError(state));
                return false;
            }
            return true;
        }, error);
}

bool SqliteRepository::resolveSyncConflictKeepLocal(
    const QString &operationUuid, QString *error)
{
    return resolveSyncConflictWithValues(operationUuid, nullptr, error);
}

bool SqliteRepository::resolveSyncConflictMerge(
    const QString &operationUuid, const QJsonObject &mergedQso, QString *error)
{
    if (mergedQso.isEmpty()) {
        clearError(error);
        setError(error, QStringLiteral("Merged QSO fields are required."));
        return false;
    }
    return resolveSyncConflictWithValues(operationUuid, &mergedQso, error);
}

bool SqliteRepository::resolveSyncConflictWithValues(
    const QString &operationUuid, const QJsonObject *mergedQso, QString *error)
{
    clearError(error);
    if (operationUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Operation UUID is required."));
        return false;
    }
    return withSavepoint(m_database,
        [this, &operationUuid, mergedQso](QString *operationError) {
            QSqlQuery lookup(m_database);
            lookup.prepare(QStringLiteral(
                "SELECT c.log_uuid, c.entity_uuid, c.local_payload_json, "
                "c.remote_payload_json, o.status FROM sync_conflicts c "
                "JOIN sync_outbox o ON o.operation_uuid = c.operation_uuid "
                "WHERE c.operation_uuid = ? AND c.resolved_at_utc = ''"));
            lookup.addBindValue(operationUuid);
            if (!lookup.exec()) {
                setError(operationError, queryError(lookup));
                return false;
            }
            if (!lookup.next()) {
                setError(operationError, QStringLiteral(
                    "Unresolved synchronization conflict does not exist."));
                return false;
            }
            const QString logUuid = lookup.value(0).toString();
            const QString entityUuid = lookup.value(1).toString();
            const QString localPayload = lookup.value(2).toString();
            const QString remotePayload = lookup.value(3).toString();
            const QString operationState = lookup.value(4).toString();
            lookup.finish();
            if (operationState != QStringLiteral("conflict")) {
                setError(operationError, QStringLiteral(
                    "Conflict operation is no longer in conflict state."));
                return false;
            }
            const QJsonDocument localDocument =
                QJsonDocument::fromJson(localPayload.toUtf8());
            const QJsonDocument remoteDocument =
                QJsonDocument::fromJson(remotePayload.toUtf8());
            if (!localDocument.isObject() || !remoteDocument.isObject()) {
                setError(operationError, QStringLiteral(
                    "Stored conflict versions are invalid JSON."));
                return false;
            }
            const QJsonObject localRoot = localDocument.object();
            const QJsonObject localQso = localRoot.value(QStringLiteral("qso")).toObject();
            const QJsonObject remote = remoteDocument.object();
            const QString qsoUuid =
                remote.value(QStringLiteral("qso_uuid")).toString();
            const QString remoteLogUuid =
                remote.value(QStringLiteral("log_uuid")).toString();
            const int remoteVersion =
                remote.value(QStringLiteral("qso_version")).toInt(-1);
            if (qsoUuid != entityUuid || remoteLogUuid != logUuid
                || remoteVersion < 1
                || remote.value(QStringLiteral("is_deleted")).toBool()) {
                setError(operationError, QStringLiteral(
                    "The server version is deleted or invalid; local changes cannot "
                    "be retried as an update."));
                return false;
            }
            QSqlQuery localRow(m_database);
            localRow.prepare(QStringLiteral(
                "SELECT q.id, q.contest_id, q.station_uuid FROM qsos q "
                "JOIN contests c ON c.id = q.contest_id "
                "WHERE q.qso_uuid = ? AND c.log_uuid = ?"));
            localRow.addBindValue(entityUuid);
            localRow.addBindValue(logUuid);
            if (!localRow.exec()) {
                setError(operationError, queryError(localRow));
                return false;
            }
            if (!localRow.next()) {
                setError(operationError, QStringLiteral(
                    "Local QSO for the conflict no longer exists."));
                return false;
            }
            Qso retry;
            retry.id = localRow.value(0).toLongLong();
            retry.contestId = localRow.value(1).toLongLong();
            retry.stationUuid = localRow.value(2).toString();
            localRow.finish();
            const QStringList requiredStrings{
                QStringLiteral("callsign"), QStringLiteral("station_callsign"),
                QStringLiteral("band"), QStringLiteral("timestamp_utc"),
                QStringLiteral("mode"), QStringLiteral("sent_exchange"),
                QStringLiteral("received_exchange")
            };
            for (const QString &key : requiredStrings) {
                if (!localQso.value(key).isString()) {
                    setError(operationError, QStringLiteral(
                        "Stored local conflict version is missing field %1.").arg(key));
                    return false;
                }
            }
            const QDateTime timestamp = QDateTime::fromString(
                localQso.value(QStringLiteral("timestamp_utc")).toString(),
                Qt::ISODateWithMs);
            const QJsonValue frequencyValue =
                localQso.value(QStringLiteral("frequency_hz"));
            const QJsonValue pointsValue = localQso.value(QStringLiteral("points"));
            const auto readFields = [](const QJsonValue &value,
                                       QMap<QString, QString> &fields) {
                if (!value.isObject()) {
                    return false;
                }
                const QJsonObject object = value.toObject();
                for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                    if (!it.value().isString()) {
                        return false;
                    }
                    fields.insert(it.key(), it.value().toString());
                }
                return true;
            };
            bool frequencyOk = false;
            bool pointsOk = false;
            const qint64 frequency = frequencyValue.toVariant().toLongLong(&frequencyOk);
            const int points = pointsValue.toInt(-1);
            pointsOk = points >= 0 && pointsValue.isDouble();
            if (!timestamp.isValid() || !frequencyOk || frequency < 0
                || !pointsOk
                || !readFields(localQso.value(
                                   QStringLiteral("sent_exchange_fields")),
                               retry.sentExchangeFields)
                || !readFields(localQso.value(
                                   QStringLiteral("received_exchange_fields")),
                               retry.receivedExchangeFields)) {
                setError(operationError, QStringLiteral(
                    "Stored local conflict version has invalid QSO fields."));
                return false;
            }
            retry.qsoUuid = entityUuid;
            retry.logUuid = logUuid;
            retry.qsoVersion = remoteVersion;
            retry.callsign = localQso.value(QStringLiteral("callsign")).toString();
            retry.stationCallsign =
                localQso.value(QStringLiteral("station_callsign")).toString();
            retry.band = localQso.value(QStringLiteral("band")).toString();
            retry.timestampUtc = timestamp.toUTC();
            retry.frequencyHz = frequency;
            retry.mode = localQso.value(QStringLiteral("mode")).toString();
            retry.sentExchange =
                localQso.value(QStringLiteral("sent_exchange")).toString();
            retry.receivedExchange =
                localQso.value(QStringLiteral("received_exchange")).toString();
            retry.points = points;
            retry.reservationUuid =
                localRoot.value(QStringLiteral("reservation_uuid")).toString();

            if (mergedQso != nullptr) {
                const QStringList expectedFields{
                    QStringLiteral("callsign"), QStringLiteral("station_callsign"),
                    QStringLiteral("band"), QStringLiteral("timestamp_utc"),
                    QStringLiteral("frequency_hz"), QStringLiteral("mode"),
                    QStringLiteral("sent_exchange"),
                    QStringLiteral("received_exchange"),
                    QStringLiteral("sent_exchange_fields"),
                    QStringLiteral("received_exchange_fields"),
                    QStringLiteral("points")
                };
                QSet<QString> allowedFields;
                for (const QString &field : expectedFields) {
                    allowedFields.insert(field);
                }
                for (const QString &field : mergedQso->keys()) {
                    if (!allowedFields.contains(field)) {
                        setError(operationError, QStringLiteral(
                            "Merged QSO contains unsupported field %1.").arg(field));
                        return false;
                    }
                }
                for (const QString &field : expectedFields) {
                    if (!mergedQso->contains(field)) {
                        setError(operationError, QStringLiteral(
                            "Merged QSO is missing field %1.").arg(field));
                        return false;
                    }
                }
                const auto readMergedMap = [](const QJsonValue &value,
                                              QMap<QString, QString> &fields) {
                    if (!value.isObject()) {
                        return false;
                    }
                    fields.clear();
                    const QJsonObject object = value.toObject();
                    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                        if (!it.value().isString()) {
                            return false;
                        }
                        fields.insert(it.key(), it.value().toString());
                    }
                    return true;
                };
                const QJsonValue mergedTimestamp =
                    mergedQso->value(QStringLiteral("timestamp_utc"));
                const QJsonValue mergedFrequency =
                    mergedQso->value(QStringLiteral("frequency_hz"));
                const QJsonValue mergedPoints =
                    mergedQso->value(QStringLiteral("points"));
                const QDateTime parsedTimestamp = QDateTime::fromString(
                    mergedTimestamp.toString(), Qt::ISODateWithMs);
                bool mergedFrequencyOk = false;
                const qint64 parsedFrequency =
                    mergedFrequency.toVariant().toLongLong(&mergedFrequencyOk);
                const int parsedPoints = mergedPoints.toInt(-1);
                if (!mergedQso->value(QStringLiteral("callsign")).isString()
                    || !mergedQso->value(QStringLiteral("station_callsign")).isString()
                    || !mergedQso->value(QStringLiteral("band")).isString()
                    || !parsedTimestamp.isValid()
                    || !mergedFrequencyOk || parsedFrequency < 0
                    || !mergedFrequency.isDouble()
                    || parsedPoints < 0 || !mergedPoints.isDouble()
                    || !mergedQso->value(QStringLiteral("mode")).isString()
                    || !mergedQso->value(QStringLiteral("sent_exchange")).isString()
                    || !mergedQso->value(
                           QStringLiteral("received_exchange")).isString()
                    || !readMergedMap(mergedQso->value(
                                          QStringLiteral("sent_exchange_fields")),
                                      retry.sentExchangeFields)
                    || !readMergedMap(mergedQso->value(
                                          QStringLiteral("received_exchange_fields")),
                                      retry.receivedExchangeFields)) {
                    setError(operationError, QStringLiteral(
                        "Merged QSO contains invalid field values."));
                    return false;
                }
                retry.callsign =
                    mergedQso->value(QStringLiteral("callsign")).toString();
                retry.stationCallsign =
                    mergedQso->value(QStringLiteral("station_callsign")).toString();
                retry.band = mergedQso->value(QStringLiteral("band")).toString();
                retry.timestampUtc = parsedTimestamp.toUTC();
                retry.frequencyHz = parsedFrequency;
                retry.mode = mergedQso->value(QStringLiteral("mode")).toString();
                retry.sentExchange =
                    mergedQso->value(QStringLiteral("sent_exchange")).toString();
                retry.receivedExchange =
                    mergedQso->value(QStringLiteral("received_exchange")).toString();
                retry.points = parsedPoints;
            }

            QSqlQuery setBaseVersion(m_database);
            setBaseVersion.prepare(QStringLiteral(
                "UPDATE qsos SET qso_version = ?, is_deleted = 0 WHERE id = ? "
                "AND qso_uuid = ?"));
            setBaseVersion.addBindValue(remoteVersion);
            setBaseVersion.addBindValue(retry.id);
            setBaseVersion.addBindValue(entityUuid);
            if (!setBaseVersion.exec() || setBaseVersion.numRowsAffected() != 1) {
                setError(operationError, setBaseVersion.lastError().isValid()
                    ? queryError(setBaseVersion)
                    : QStringLiteral("Local QSO changed while preparing conflict retry."));
                return false;
            }
            QString updateError;
            if (!updateQso(retry, &updateError)) {
                setError(operationError, updateError);
                return false;
            }

            QSqlQuery finishConflict(m_database);
            finishConflict.prepare(QStringLiteral(
                "UPDATE sync_conflicts SET resolved_at_utc = ? "
                "WHERE operation_uuid = ? AND resolved_at_utc = ''"));
            finishConflict.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            finishConflict.addBindValue(operationUuid);
            if (!finishConflict.exec() || finishConflict.numRowsAffected() != 1) {
                setError(operationError, finishConflict.lastError().isValid()
                    ? queryError(finishConflict)
                    : QStringLiteral("Conflict was resolved concurrently."));
                return false;
            }
            QSqlQuery finishOperation(m_database);
            finishOperation.prepare(QStringLiteral(
                "UPDATE sync_outbox SET status = 'accepted', last_error = '' "
                "WHERE operation_uuid = ? AND status = 'conflict'"));
            finishOperation.addBindValue(operationUuid);
            if (!finishOperation.exec() || finishOperation.numRowsAffected() != 1) {
                setError(operationError, finishOperation.lastError().isValid()
                    ? queryError(finishOperation)
                    : QStringLiteral("Conflict operation changed during resolution."));
                return false;
            }
            QSqlQuery audit(m_database);
            audit.prepare(QStringLiteral(
                "INSERT INTO sync_conflict_resolutions(operation_uuid, log_uuid, "
                "entity_uuid, resolution, local_payload_json, remote_payload_json, "
                "resolution_payload_json, resolved_at_utc) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
            audit.addBindValue(operationUuid);
            audit.addBindValue(logUuid);
            audit.addBindValue(entityUuid);
            audit.addBindValue(mergedQso == nullptr
                ? QStringLiteral("keep_local") : QStringLiteral("merge"));
            audit.addBindValue(localPayload);
            audit.addBindValue(remotePayload);
            audit.addBindValue(QString::fromUtf8(syncQsoPayload(
                retry, operationUuid, retry.stationUuid,
                QStringLiteral("qso.update"), remoteVersion)));
            audit.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            if (!audit.exec()) {
                setError(operationError, queryError(audit));
                return false;
            }
            QSqlQuery state(m_database);
            state.prepare(QStringLiteral(
                "UPDATE sync_state SET last_error = '', updated_at_utc = ? "
                "WHERE log_uuid = ? AND NOT EXISTS(SELECT 1 FROM sync_outbox "
                "WHERE log_uuid = ? AND status = 'conflict')"));
            state.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
            state.addBindValue(logUuid);
            state.addBindValue(logUuid);
            if (!state.exec()) {
                setError(operationError, queryError(state));
                return false;
            }
            return true;
        }, error);
}

bool SqliteRepository::applySyncChanges(
    const QString &logUuid, const QJsonArray &events,
    qint64 fromRevision, qint64 toRevision, QString *error)
{
    clearError(error);
    if (logUuid.trimmed().isEmpty() || fromRevision < 1 || toRevision < 0) {
        setError(error, QStringLiteral("Log UUID or revision range is invalid."));
        return false;
    }
    return withTransaction([this, &logUuid, &events, fromRevision, toRevision]
                           (QString *operationError) {
        QSqlQuery state(m_database);
        state.prepare(QStringLiteral(
            "SELECT cursor FROM sync_state WHERE log_uuid = ?"));
        state.addBindValue(logUuid);
        if (!state.exec()) {
            setError(operationError, queryError(state));
            return false;
        }
        if (!state.next()) {
            setError(operationError, QStringLiteral(
                "Synchronization state for log %1 does not exist.").arg(logUuid));
            return false;
        }
        const qint64 cursor = state.value(0).toLongLong();
        state.finish();
        if (fromRevision != cursor + 1) {
            setError(operationError, QStringLiteral(
                "Change page does not start at the next local revision."));
            return false;
        }

        QSqlQuery contest(m_database);
        contest.prepare(QStringLiteral(
            "SELECT id FROM contests WHERE log_uuid = ?"));
        contest.addBindValue(logUuid);
        if (!contest.exec() || !contest.next()) {
            setError(operationError, contest.lastError().isValid()
                ? queryError(contest)
                : QStringLiteral("Central log does not exist locally."));
            return false;
        }
        const qint64 contestId = contest.value(0).toLongLong();
        contest.finish();

        qint64 expectedRevision = fromRevision;
        for (const QJsonValue &eventValue : events) {
            if (!eventValue.isObject()) {
                setError(operationError, QStringLiteral("Change event is not an object."));
                return false;
            }
            const QJsonObject event = eventValue.toObject();
            QJsonObject qso = event.value(QStringLiteral("payload")).toObject();
            if (!qso.contains(QStringLiteral("qtc_transfers"))) {
                qso.insert(QStringLiteral("qtc_transfers"), QJsonArray{});
            }
            const qint64 revision =
                event.value(QStringLiteral("revision")).toInteger(-1);
            const QString eventLogUuid =
                event.value(QStringLiteral("log_uuid")).toString();
            const QString qsoUuid = event.value(QStringLiteral("qso_uuid")).toString();
            const QString operationUuid =
                event.value(QStringLiteral("operation_uuid")).toString();
            const QString kind = event.value(QStringLiteral("kind")).toString();
            const int version = qso.value(QStringLiteral("qso_version")).toInt(-1);
            const bool deleted = qso.value(QStringLiteral("is_deleted")).toBool();
            const QStringList requiredQsoKeys{
                QStringLiteral("band"), QStringLiteral("callsign"),
                QStringLiteral("frequency_hz"), QStringLiteral("is_deleted"),
                QStringLiteral("is_invalid"),
                QStringLiteral("log_uuid"), QStringLiteral("mode"),
                QStringLiteral("points"), QStringLiteral("qso_uuid"),
                QStringLiteral("qso_version"), QStringLiteral("qtc_transfers"),
                QStringLiteral("received_exchange"),
                QStringLiteral("received_exchange_fields"),
                QStringLiteral("sent_exchange"),
                QStringLiteral("sent_exchange_fields"),
                QStringLiteral("station_callsign"), QStringLiteral("station_uuid"),
                QStringLiteral("timestamp_utc")
            };
            const QDateTime timestamp = QDateTime::fromString(
                qso.value(QStringLiteral("timestamp_utc")).toString(),
                Qt::ISODateWithMs);
            if (revision != expectedRevision || eventLogUuid != logUuid
                || !isSyncUuid(event.value(QStringLiteral("event_uuid")).toString())
                || !isSyncUuid(operationUuid) || !isSyncUuid(qsoUuid)
                || (kind != QStringLiteral("qso.upsert")
                    && kind != QStringLiteral("qso.delete"))
                || qso.keys() != requiredQsoKeys
                || !qso.value(QStringLiteral("is_deleted")).isBool()
                || !qso.value(QStringLiteral("is_invalid")).isBool()
                || qso.value(QStringLiteral("qso_uuid")).toString() != qsoUuid
                || qso.value(QStringLiteral("log_uuid")).toString() != logUuid
                || !isSyncUuid(qso.value(QStringLiteral("station_uuid")).toString())
                || event.value(QStringLiteral("qso_version")).toInt(-1) != version
                || event.value(QStringLiteral("station_uuid")).toString()
                       != qso.value(QStringLiteral("station_uuid")).toString()
                || version < 1 || !timestamp.isValid()
                || !qso.value(QStringLiteral("timestamp_utc")).toString()
                        .endsWith(QLatin1Char('Z'))
                || qso.value(QStringLiteral("callsign")).toString().trimmed().isEmpty()
                || qso.value(QStringLiteral("station_callsign")).toString()
                       .trimmed().isEmpty()
                || qso.value(QStringLiteral("band")).toString().trimmed().isEmpty()
                || qso.value(QStringLiteral("mode")).toString().trimmed().isEmpty()
                || (kind == QStringLiteral("qso.delete")) != deleted) {
                setError(operationError, QStringLiteral(
                    "Change event fields or revision sequence are invalid."));
                return false;
            }
            const QJsonValue frequencyValue =
                qso.value(QStringLiteral("frequency_hz"));
            const QJsonValue pointsValue = qso.value(QStringLiteral("points"));
            const double frequency = frequencyValue.toDouble(-1);
            const double points = pointsValue.toDouble(-1);
            const auto validStringObject = [](const QJsonValue &value) {
                if (!value.isObject()) {
                    return false;
                }
                const QJsonObject object = value.toObject();
                for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                    if (!it.value().isString()) {
                        return false;
                    }
                }
                return true;
            };
            if (!frequencyValue.isDouble() || !std::isfinite(frequency) || frequency < 0
                || frequency >= static_cast<double>(
                    std::numeric_limits<qint64>::max())
                || frequency != static_cast<qint64>(frequency)
                || !pointsValue.isDouble() || !std::isfinite(points) || points < 0
                || points > std::numeric_limits<int>::max()
                || points != static_cast<int>(points)
                || !validStringObject(qso.value(QStringLiteral("sent_exchange_fields")))
                || !validStringObject(
                    qso.value(QStringLiteral("received_exchange_fields")))) {
                setError(operationError, QStringLiteral("Change event QSO data is invalid."));
                return false;
            }
            Qso qtcPartner;
            qtcPartner.qsoUuid = qsoUuid;
            qtcPartner.callsign = qso.value(QStringLiteral("callsign")).toString();
            if (!QtcService::validateJson(
                    qtcPartner, qso.value(QStringLiteral("qtc_transfers")),
                    operationError)) {
                return false;
            }

            QSqlQuery localOperations(m_database);
            localOperations.prepare(QStringLiteral(
                "SELECT operation_uuid, payload_json, status FROM sync_outbox "
                "WHERE log_uuid = ? AND entity_uuid = ? "
                "AND status IN ('pending', 'in-flight', 'failed')"));
            localOperations.addBindValue(logUuid);
            localOperations.addBindValue(qsoUuid);
            if (!localOperations.exec()) {
                setError(operationError, queryError(localOperations));
                return false;
            }
            QVector<QPair<QString, QString>> conflicts;
            while (localOperations.next()) {
                const QString localOperationUuid = localOperations.value(0).toString();
                if (localOperationUuid != operationUuid) {
                    conflicts.append({localOperationUuid,
                                      localOperations.value(1).toString()});
                }
            }
            localOperations.finish();
            for (const auto &conflict : conflicts) {
                QSqlQuery saveConflict(m_database);
                saveConflict.prepare(QStringLiteral(
                    "INSERT OR IGNORE INTO sync_conflicts(operation_uuid, log_uuid, "
                    "entity_uuid, local_payload_json, remote_payload_json, created_at_utc) "
                    "VALUES(?, ?, ?, ?, ?, ?)"));
                saveConflict.addBindValue(conflict.first);
                saveConflict.addBindValue(logUuid);
                saveConflict.addBindValue(qsoUuid);
                saveConflict.addBindValue(conflict.second);
                saveConflict.addBindValue(QString::fromUtf8(
                    QJsonDocument(qso).toJson(QJsonDocument::Compact)));
                saveConflict.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
                if (!saveConflict.exec()) {
                    setError(operationError, queryError(saveConflict));
                    return false;
                }
                QSqlQuery markConflict(m_database);
                markConflict.prepare(QStringLiteral(
                    "UPDATE sync_outbox SET status = 'conflict', "
                    "last_error = 'Concurrent central QSO update' "
                    "WHERE operation_uuid = ? AND status IN "
                    "('pending', 'in-flight', 'failed')"));
                markConflict.addBindValue(conflict.first);
                if (!markConflict.exec()) {
                    setError(operationError, queryError(markConflict));
                    return false;
                }
            }

            QSqlQuery existing(m_database);
            existing.prepare(QStringLiteral(
                "SELECT id FROM qsos WHERE contest_id = ? AND qso_uuid = ?"));
            existing.addBindValue(contestId);
            existing.addBindValue(qsoUuid);
            if (!existing.exec()) {
                setError(operationError, queryError(existing));
                return false;
            }
            const bool exists = existing.next();
            const qint64 localId = exists ? existing.value(0).toLongLong() : 0;
            existing.finish();

            const QString timestampUtc = timestamp.toUTC().toString(Qt::ISODateWithMs);
            const QString callsign =
                qso.value(QStringLiteral("callsign")).toString().trimmed().toUpper();
            const QString stationCallsign =
                qso.value(QStringLiteral("station_callsign")).toString()
                    .trimmed().toUpper();
            const QString band =
                qso.value(QStringLiteral("band")).toString().trimmed().toLower();
            const QString mode =
                qso.value(QStringLiteral("mode")).toString().trimmed().toUpper();
            const QString sentFields = QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("sent_exchange_fields")).toObject())
                .toJson(QJsonDocument::Compact));
            const QString receivedFields = QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("received_exchange_fields")).toObject())
                .toJson(QJsonDocument::Compact));
            const QString qtcJson = QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("qtc_transfers")).toArray())
                .toJson(QJsonDocument::Compact));
            QSqlQuery persist(m_database);
            if (!exists) {
                persist.prepare(QStringLiteral(
                    "INSERT INTO qsos(contest_id, callsign, station_callsign, band, "
                    "timestamp_utc, frequency_hz, mode, sent_exchange, received_exchange, "
                    "sent_exchange_json, received_exchange_json, points, qso_uuid, "
                    "station_uuid, qso_version, is_deleted, is_invalid, qtc_json) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
                persist.addBindValue(contestId);
                persist.addBindValue(callsign);
                persist.addBindValue(stationCallsign);
                persist.addBindValue(band);
                persist.addBindValue(timestampUtc);
                persist.addBindValue(static_cast<qint64>(frequency));
                persist.addBindValue(mode);
                persist.addBindValue(qso.value(QStringLiteral("sent_exchange")).toString());
                persist.addBindValue(
                    qso.value(QStringLiteral("received_exchange")).toString());
                persist.addBindValue(sentFields);
                persist.addBindValue(receivedFields);
                persist.addBindValue(static_cast<int>(points));
                persist.addBindValue(qsoUuid);
                persist.addBindValue(qso.value(QStringLiteral("station_uuid")).toString());
                persist.addBindValue(version);
                persist.addBindValue(deleted ? 1 : 0);
                persist.addBindValue(
                    qso.value(QStringLiteral("is_invalid")).toBool() ? 1 : 0);
                persist.addBindValue(qtcJson);
            } else {
                persist.prepare(QStringLiteral(
                    "UPDATE qsos SET callsign = ?, station_callsign = ?, band = ?, "
                    "timestamp_utc = ?, frequency_hz = ?, mode = ?, sent_exchange = ?, "
                    "received_exchange = ?, sent_exchange_json = ?, received_exchange_json = ?, "
                    "points = ?, station_uuid = ?, qso_version = ?, is_deleted = ?, "
                    "is_invalid = ?, qtc_json = ? "
                    "WHERE id = ?"));
                persist.addBindValue(callsign);
                persist.addBindValue(stationCallsign);
                persist.addBindValue(band);
                persist.addBindValue(timestampUtc);
                persist.addBindValue(static_cast<qint64>(frequency));
                persist.addBindValue(mode);
                persist.addBindValue(qso.value(QStringLiteral("sent_exchange")).toString());
                persist.addBindValue(
                    qso.value(QStringLiteral("received_exchange")).toString());
                persist.addBindValue(sentFields);
                persist.addBindValue(receivedFields);
                persist.addBindValue(static_cast<int>(points));
                persist.addBindValue(qso.value(QStringLiteral("station_uuid")).toString());
                persist.addBindValue(version);
                persist.addBindValue(deleted ? 1 : 0);
                persist.addBindValue(
                    qso.value(QStringLiteral("is_invalid")).toBool() ? 1 : 0);
                persist.addBindValue(qtcJson);
                persist.addBindValue(localId);
            }
            if (!persist.exec() || (exists && persist.numRowsAffected() != 1)) {
                setError(operationError, persist.lastError().isValid()
                    ? queryError(persist)
                    : QStringLiteral("Local QSO changed while applying central event."));
                return false;
            }

            QSqlQuery acknowledgeOperation(m_database);
            acknowledgeOperation.prepare(QStringLiteral(
                "UPDATE sync_outbox SET status = 'accepted', last_error = '' "
                "WHERE operation_uuid = ? AND log_uuid = ? "
                "AND status IN ('pending', 'in-flight', 'failed')"));
            acknowledgeOperation.addBindValue(operationUuid);
            acknowledgeOperation.addBindValue(logUuid);
            if (!acknowledgeOperation.exec()) {
                setError(operationError, queryError(acknowledgeOperation));
                return false;
            }
            expectedRevision = revision + 1;
        }
        if (toRevision != expectedRevision - 1
            || (events.isEmpty() && toRevision != cursor)) {
            setError(operationError, QStringLiteral(
                "Change page revision bounds do not match its events."));
            return false;
        }
        QSqlQuery updateCursor(m_database);
        updateCursor.prepare(QStringLiteral(
            "UPDATE sync_state SET cursor = ?, updated_at_utc = ? "
            "WHERE log_uuid = ? AND cursor = ?"));
        updateCursor.addBindValue(toRevision);
        updateCursor.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
        updateCursor.addBindValue(logUuid);
        updateCursor.addBindValue(cursor);
        if (!updateCursor.exec() || updateCursor.numRowsAffected() != 1) {
            setError(operationError, updateCursor.lastError().isValid()
                ? queryError(updateCursor)
                : QStringLiteral("Local synchronization cursor changed concurrently."));
            return false;
        }
        return true;
    }, error);
}

bool SqliteRepository::applySyncSnapshot(
    const QString &logUuid, const QJsonArray &qsos, qint64 revision,
    const QString &sha256, QString *error)
{
    clearError(error);
    const QByteArray actualHash = QCryptographicHash::hash(
        QJsonDocument(qsos).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    if (!isSyncUuid(logUuid) || revision < 0
        || sha256 != QString::fromLatin1(actualHash)) {
        setError(error, QStringLiteral("Snapshot identity, revision or checksum is invalid."));
        return false;
    }
    return withTransaction([this, &logUuid, &qsos, revision]
                           (QString *operationError) {
        QSqlQuery state(m_database);
        state.prepare(QStringLiteral(
            "SELECT cursor FROM sync_state WHERE log_uuid = ?"));
        state.addBindValue(logUuid);
        if (!state.exec() || !state.next()) {
            setError(operationError, state.lastError().isValid()
                ? queryError(state)
                : QStringLiteral("Synchronization state for snapshot does not exist."));
            return false;
        }
        const qint64 cursor = state.value(0).toLongLong();
        state.finish();
        if (revision < cursor) {
            setError(operationError, QStringLiteral(
                "Snapshot revision cannot move the local cursor backwards."));
            return false;
        }

        QSqlQuery contest(m_database);
        contest.prepare(QStringLiteral(
            "SELECT id FROM contests WHERE log_uuid = ?"));
        contest.addBindValue(logUuid);
        if (!contest.exec() || !contest.next()) {
            setError(operationError, contest.lastError().isValid()
                ? queryError(contest)
                : QStringLiteral("Snapshot log does not exist locally."));
            return false;
        }
        const qint64 contestId = contest.value(0).toLongLong();
        contest.finish();

        QSet<QString> snapshotUuids;
        const QStringList requiredKeys{
            QStringLiteral("band"), QStringLiteral("callsign"),
            QStringLiteral("frequency_hz"), QStringLiteral("is_deleted"),
            QStringLiteral("is_invalid"),
            QStringLiteral("log_uuid"), QStringLiteral("mode"),
            QStringLiteral("points"), QStringLiteral("qso_uuid"),
            QStringLiteral("qso_version"), QStringLiteral("qtc_transfers"),
            QStringLiteral("received_exchange"),
            QStringLiteral("received_exchange_fields"),
            QStringLiteral("sent_exchange"),
            QStringLiteral("sent_exchange_fields"),
            QStringLiteral("station_callsign"), QStringLiteral("station_uuid"),
            QStringLiteral("timestamp_utc")
        };
        for (const QJsonValue &value : qsos) {
            if (!value.isObject()) {
                setError(operationError, QStringLiteral("Snapshot QSO is not an object."));
                return false;
            }
            QJsonObject qso = value.toObject();
            if (!qso.contains(QStringLiteral("qtc_transfers"))) {
                qso.insert(QStringLiteral("qtc_transfers"), QJsonArray{});
            }
            const QString qsoUuid = qso.value(QStringLiteral("qso_uuid")).toString();
            const QDateTime timestamp = QDateTime::fromString(
                qso.value(QStringLiteral("timestamp_utc")).toString(),
                Qt::ISODateWithMs);
            const QJsonValue frequencyValue = qso.value(QStringLiteral("frequency_hz"));
            const double frequency = frequencyValue.toDouble(-1);
            const QJsonValue pointsValue = qso.value(QStringLiteral("points"));
            const double points = pointsValue.toDouble(-1);
            if (qso.keys() != requiredKeys || !isSyncUuid(qsoUuid)
                || snapshotUuids.contains(qsoUuid)
                || qso.value(QStringLiteral("log_uuid")).toString() != logUuid
                || !isSyncUuid(qso.value(QStringLiteral("station_uuid")).toString())
                || qso.value(QStringLiteral("is_deleted")).toBool()
                || !qso.value(QStringLiteral("is_deleted")).isBool()
                || !qso.value(QStringLiteral("is_invalid")).isBool()
                || qso.value(QStringLiteral("qso_version")).toInt(-1) < 1
                || !frequencyValue.isDouble() || !std::isfinite(frequency)
                || frequency < 0
                || frequency >= static_cast<double>(std::numeric_limits<qint64>::max())
                || frequency != static_cast<qint64>(frequency)
                || !pointsValue.isDouble() || !std::isfinite(points) || points < 0
                || points > std::numeric_limits<int>::max()
                || points != static_cast<int>(points)
                || !qso.value(QStringLiteral("received_exchange_fields")).isObject()
                || !qso.value(QStringLiteral("sent_exchange_fields")).isObject()
                || !qso.value(QStringLiteral("qtc_transfers")).isArray()
                || !timestamp.isValid()
                || !qso.value(QStringLiteral("timestamp_utc")).toString()
                        .endsWith(QLatin1Char('Z'))
                || qso.value(QStringLiteral("callsign")).toString().trimmed().isEmpty()
                || qso.value(QStringLiteral("station_callsign")).toString()
                       .trimmed().isEmpty()) {
                setError(operationError, QStringLiteral("Snapshot contains an invalid QSO."));
                return false;
            }
            Qso qtcPartner;
            qtcPartner.qsoUuid = qsoUuid;
            qtcPartner.callsign = qso.value(QStringLiteral("callsign")).toString();
            if (!QtcService::validateJson(
                    qtcPartner, qso.value(QStringLiteral("qtc_transfers")),
                    operationError)) {
                return false;
            }
            snapshotUuids.insert(qsoUuid);
        }

        QSqlQuery removeMissing(m_database);
        removeMissing.prepare(QStringLiteral(
            "DELETE FROM qsos WHERE contest_id = ? AND qso_uuid NOT IN ("
            "SELECT entity_uuid FROM sync_outbox WHERE log_uuid = ? "
            "AND status IN ('pending', 'in-flight', 'failed', 'conflict'))"));
        removeMissing.addBindValue(contestId);
        removeMissing.addBindValue(logUuid);
        if (!removeMissing.exec()) {
            setError(operationError, queryError(removeMissing));
            return false;
        }

        for (const QJsonValue &value : qsos) {
            const QJsonObject qso = value.toObject();
            const QString qsoUuid = qso.value(QStringLiteral("qso_uuid")).toString();
            QSqlQuery localOperation(m_database);
            localOperation.prepare(QStringLiteral(
                "SELECT 1 FROM sync_outbox WHERE log_uuid = ? AND entity_uuid = ? "
                "AND status IN ('pending', 'in-flight', 'failed', 'conflict') LIMIT 1"));
            localOperation.addBindValue(logUuid);
            localOperation.addBindValue(qsoUuid);
            if (!localOperation.exec()) {
                setError(operationError, queryError(localOperation));
                return false;
            }
            if (localOperation.next()) {
                continue;
            }
            QSqlQuery existing(m_database);
            existing.prepare(QStringLiteral(
                "SELECT id FROM qsos WHERE contest_id = ? AND qso_uuid = ?"));
            existing.addBindValue(contestId);
            existing.addBindValue(qsoUuid);
            if (!existing.exec()) {
                setError(operationError, queryError(existing));
                return false;
            }
            const bool exists = existing.next();
            const qint64 localId = exists ? existing.value(0).toLongLong() : 0;
            existing.finish();
            const QString timestampUtc = QDateTime::fromString(
                qso.value(QStringLiteral("timestamp_utc")).toString(),
                Qt::ISODateWithMs).toUTC().toString(Qt::ISODateWithMs);
            const QString sentFields = QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("sent_exchange_fields")).toObject())
                .toJson(QJsonDocument::Compact));
            const QString receivedFields = QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("received_exchange_fields")).toObject())
                .toJson(QJsonDocument::Compact));
            const QString qtcJson = QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("qtc_transfers")).toArray())
                .toJson(QJsonDocument::Compact));
            QSqlQuery persist(m_database);
            if (exists) {
                persist.prepare(QStringLiteral(
                    "UPDATE qsos SET callsign = ?, station_callsign = ?, band = ?, "
                    "timestamp_utc = ?, frequency_hz = ?, mode = ?, sent_exchange = ?, "
                    "received_exchange = ?, sent_exchange_json = ?, received_exchange_json = ?, "
                    "points = ?, station_uuid = ?, qso_version = ?, is_deleted = 0, "
                    "is_invalid = ?, qtc_json = ? "
                    "WHERE id = ?"));
                persist.addBindValue(qso.value(QStringLiteral("callsign")).toString()
                                         .trimmed().toUpper());
                persist.addBindValue(qso.value(QStringLiteral("station_callsign")).toString()
                                         .trimmed().toUpper());
                persist.addBindValue(qso.value(QStringLiteral("band")).toString()
                                         .trimmed().toLower());
                persist.addBindValue(timestampUtc);
                persist.addBindValue(qso.value(QStringLiteral("frequency_hz")).toInteger());
                persist.addBindValue(qso.value(QStringLiteral("mode")).toString()
                                         .trimmed().toUpper());
                persist.addBindValue(qso.value(QStringLiteral("sent_exchange")).toString());
                persist.addBindValue(qso.value(QStringLiteral("received_exchange")).toString());
                persist.addBindValue(sentFields);
                persist.addBindValue(receivedFields);
                persist.addBindValue(qso.value(QStringLiteral("points")).toInt());
                persist.addBindValue(qso.value(QStringLiteral("station_uuid")).toString());
                persist.addBindValue(qso.value(QStringLiteral("qso_version")).toInt());
                persist.addBindValue(
                    qso.value(QStringLiteral("is_invalid")).toBool() ? 1 : 0);
                persist.addBindValue(qtcJson);
                persist.addBindValue(localId);
            } else {
                persist.prepare(QStringLiteral(
                    "INSERT INTO qsos(contest_id, callsign, station_callsign, band, "
                    "timestamp_utc, frequency_hz, mode, sent_exchange, received_exchange, "
                    "sent_exchange_json, received_exchange_json, points, qso_uuid, "
                    "station_uuid, qso_version, is_deleted, is_invalid, qtc_json) "
                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?)"));
                persist.addBindValue(contestId);
                persist.addBindValue(qso.value(QStringLiteral("callsign")).toString()
                                         .trimmed().toUpper());
                persist.addBindValue(qso.value(QStringLiteral("station_callsign")).toString()
                                         .trimmed().toUpper());
                persist.addBindValue(qso.value(QStringLiteral("band")).toString()
                                         .trimmed().toLower());
                persist.addBindValue(timestampUtc);
                persist.addBindValue(qso.value(QStringLiteral("frequency_hz")).toInteger());
                persist.addBindValue(qso.value(QStringLiteral("mode")).toString()
                                         .trimmed().toUpper());
                persist.addBindValue(qso.value(QStringLiteral("sent_exchange")).toString());
                persist.addBindValue(qso.value(QStringLiteral("received_exchange")).toString());
                persist.addBindValue(sentFields);
                persist.addBindValue(receivedFields);
                persist.addBindValue(qso.value(QStringLiteral("points")).toInt());
                persist.addBindValue(qsoUuid);
                persist.addBindValue(qso.value(QStringLiteral("station_uuid")).toString());
                persist.addBindValue(qso.value(QStringLiteral("qso_version")).toInt());
                persist.addBindValue(
                    qso.value(QStringLiteral("is_invalid")).toBool() ? 1 : 0);
                persist.addBindValue(qtcJson);
            }
            if (!persist.exec()) {
                setError(operationError, queryError(persist));
                return false;
            }
        }

        QSqlQuery updateCursor(m_database);
        updateCursor.prepare(QStringLiteral(
            "UPDATE sync_state SET cursor = ?, updated_at_utc = ? "
            "WHERE log_uuid = ? AND cursor = ?"));
        updateCursor.addBindValue(revision);
        updateCursor.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
        updateCursor.addBindValue(logUuid);
        updateCursor.addBindValue(cursor);
        if (!updateCursor.exec() || updateCursor.numRowsAffected() != 1) {
            setError(operationError, updateCursor.lastError().isValid()
                ? queryError(updateCursor)
                : QStringLiteral("Local synchronization cursor changed concurrently."));
            return false;
        }
        return true;
    }, error);
}

QJsonObject SqliteRepository::syncConsistencyDigest(
    const QString &logUuid, QString *error) const
{
    clearError(error);
    if (logUuid.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log UUID is required for consistency audit."));
        return {};
    }
    QJsonArray snapshots;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT q.qso_uuid, c.log_uuid, q.station_uuid, q.callsign, "
        "q.station_callsign, q.band, q.timestamp_utc, q.frequency_hz, q.mode, "
        "q.sent_exchange, q.received_exchange, q.sent_exchange_json, "
        "q.received_exchange_json, q.points, q.qso_version, q.is_deleted, "
        "q.is_invalid, q.qtc_json "
        "FROM qsos q JOIN contests c ON c.id = q.contest_id "
        "WHERE c.log_uuid = ? ORDER BY q.qso_uuid"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return {};
    }
    while (query.next()) {
        const QJsonDocument sentFields = QJsonDocument::fromJson(
            query.value(11).toByteArray());
        const QJsonDocument receivedFields = QJsonDocument::fromJson(
            query.value(12).toByteArray());
        const QJsonDocument qtcTransfers = QJsonDocument::fromJson(
            query.value(17).toByteArray());
        if (!sentFields.isObject() || !receivedFields.isObject()
            || !qtcTransfers.isArray()) {
            setError(error, QStringLiteral(
                "Stored QSO exchange or QTC fields are invalid."));
            return {};
        }
        snapshots.append(QJsonObject{
            {QStringLiteral("qso_uuid"), query.value(0).toString()},
            {QStringLiteral("log_uuid"), query.value(1).toString()},
            {QStringLiteral("station_uuid"), query.value(2).toString()},
            {QStringLiteral("callsign"), query.value(3).toString()},
            {QStringLiteral("station_callsign"), query.value(4).toString()},
            {QStringLiteral("band"), query.value(5).toString()},
            {QStringLiteral("timestamp_utc"), query.value(6).toString()},
            {QStringLiteral("frequency_hz"),
             QJsonValue::fromVariant(query.value(7))},
            {QStringLiteral("mode"), query.value(8).toString()},
            {QStringLiteral("sent_exchange"), query.value(9).toString()},
            {QStringLiteral("received_exchange"), query.value(10).toString()},
            {QStringLiteral("sent_exchange_fields"), sentFields.object()},
            {QStringLiteral("received_exchange_fields"), receivedFields.object()},
            {QStringLiteral("points"), query.value(13).toInt()},
            {QStringLiteral("qso_version"), query.value(14).toInt()},
            {QStringLiteral("is_deleted"), query.value(15).toBool()},
            {QStringLiteral("is_invalid"), query.value(16).toBool()},
            {QStringLiteral("qtc_transfers"), qtcTransfers.array()}
        });
    }
    return logger::syncConsistencyDigest(snapshots);
}

QStringList SqliteRepository::contestDefinitionCodes(QString *error) const
{
    clearError(error);
    QStringList result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT code FROM contests ORDER BY code"))) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        result.append(query.value(0).toString());
    }
    return result;
}

QString SqliteRepository::contestDefinitionYaml(qint64 contestId, QString *error) const
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT definition_yaml FROM contests WHERE id = ?"));
    query.addBindValue(contestId);
    if (!query.exec()) {
        setError(error, queryError(query));
        return {};
    }
    if (!query.next()) {
        setError(error, QStringLiteral("Contest %1 does not exist.").arg(contestId));
        return {};
    }
    return query.value(0).toString();
}

qint64 SqliteRepository::createContestLog(
    const ContestDefinition &definition, const QString &yaml, const QString &name,
    QString *error)
{
    Contest log;
    log.name = name;
    return createContestLog(definition, yaml, log, error);
}

qint64 SqliteRepository::createContestLog(
    const ContestDefinition &definition, const QString &yaml, const Contest &log,
    QString *error)
{
    clearError(error);
    if (m_standaloneLog) {
        setError(error, QStringLiteral("Create a separate file for each new log."));
        return 0;
    }
    if (log.name.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log name is required."));
        return 0;
    }
    QString listError;
    const QStringList taken = contestDefinitionCodes(&listError);
    if (!listError.isEmpty()) {
        setError(error, listError);
        return 0;
    }
    const QString base = definition.id.toUpper();
    QString code = base;
    for (int suffix = 2; taken.contains(code, Qt::CaseInsensitive); ++suffix) {
        code = QStringLiteral("%1-%2").arg(base).arg(suffix);
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO contests(code, name, description, definition_yaml, "
        "starts_at_utc, station_callsign, properties_json, log_uuid) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(code);
    query.addBindValue(log.name.trimmed());
    query.addBindValue(notNullText(definition.description));
    query.addBindValue(notNullText(yaml));
    query.addBindValue(utcText(log.startsAtUtc));
    query.addBindValue(notNullText(log.stationCallsign.trimmed().toUpper()));
    query.addBindValue(QString::fromUtf8(exchangeJson(log.properties)));
    query.addBindValue(log.logUuid.isEmpty() ? newUuid() : log.logUuid);
    if (!query.exec()) {
        setError(error, queryError(query));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool SqliteRepository::updateContestLog(const Contest &log, QString *error)
{
    clearError(error);
    if (log.id <= 0 || log.name.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Log id and name are required."));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE contests SET name = ?, starts_at_utc = ?, station_callsign = ?, "
        "properties_json = ? WHERE id = ?"));
    query.addBindValue(log.name.trimmed());
    query.addBindValue(utcText(log.startsAtUtc));
    query.addBindValue(notNullText(log.stationCallsign.trimmed().toUpper()));
    query.addBindValue(QString::fromUtf8(exchangeJson(log.properties)));
    query.addBindValue(log.id);
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("Log %1 does not exist.").arg(log.id));
        return false;
    }
    return true;
}

bool SqliteRepository::saveContestDefinition(
    const ContestDefinition &definition, const QString &yaml, QString *error)
{
    clearError(error);
    if (m_standaloneLog) {
        setError(error, QStringLiteral("The contest definition of a standalone log is fixed."));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO contests(code, name, description, definition_yaml, log_uuid) "
        "VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(code) DO UPDATE SET "
        "name = excluded.name, description = excluded.description, "
        "definition_yaml = excluded.definition_yaml"));
    query.addBindValue(definition.id.toUpper());
    query.addBindValue(definition.name);
    query.addBindValue(definition.description);
    query.addBindValue(yaml);
    query.addBindValue(newUuid());
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    return true;
}

bool SqliteRepository::saveQso(Qso &qso, QString *error)
{
    clearError(error);
    if (qso.contestId <= 0 || qso.callsign.trimmed().isEmpty()
        || qso.stationCallsign.trimmed().isEmpty() || !qso.timestampUtc.isValid()
        || qso.frequencyHz < 0) {
        setError(error, QStringLiteral("QSO is missing required or valid data."));
        return false;
    }
    qint64 savedId = 0;
    const bool saved = withSavepoint(m_database, [this, &qso, &savedId]
                                     (QString *operationError) {
        QSqlQuery logQuery(m_database);
        logQuery.prepare(QStringLiteral(
            "SELECT log_uuid FROM contests WHERE id = ?"));
        logQuery.addBindValue(qso.contestId);
        if (!logQuery.exec()) {
            setError(operationError, queryError(logQuery));
            return false;
        }
        if (!logQuery.next()) {
            setError(operationError, QStringLiteral("Contest log %1 does not exist.")
                                        .arg(qso.contestId));
            return false;
        }
        const QString logUuid = logQuery.value(0).toString();
        logQuery.finish();
        if (!qso.logUuid.isEmpty() && qso.logUuid != logUuid) {
            setError(operationError, QStringLiteral(
                "QSO log identity does not match its contest."));
            return false;
        }
        qso.logUuid = logUuid;
        if (qso.qsoUuid.isEmpty()) {
            qso.qsoUuid = newUuid();
        }
        if (qso.stationUuid.isEmpty()) {
            qso.stationUuid = localStationUuid(operationError);
            if (qso.stationUuid.isEmpty()) {
                return false;
            }
        }
        qso.qsoVersion = 1;
        if (!qso.reservationUuid.isEmpty()) {
            QSqlQuery reservation(m_database);
            reservation.prepare(QStringLiteral(
                "SELECT station_uuid, band, mode, status FROM serial_reservations "
                "WHERE log_uuid = ? AND reservation_uuid = ?"));
            reservation.addBindValue(logUuid);
            reservation.addBindValue(qso.reservationUuid);
            if (!reservation.exec()) {
                setError(operationError, queryError(reservation));
                return false;
            }
            if (!reservation.next()
                || reservation.value(0).toString() != qso.stationUuid
                || reservation.value(1).toString() != qso.band.trimmed().toLower()
                || reservation.value(2).toString() != qso.mode.trimmed().toUpper()
                || reservation.value(3).toString() != QStringLiteral("reserved")) {
                setError(operationError, QStringLiteral(
                    "Serial reservation is unavailable or does not match this QSO."));
                return false;
            }
        }

        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO qsos(contest_id, callsign, station_callsign, band, timestamp_utc, "
            "frequency_hz, mode, sent_exchange, received_exchange, "
            "sent_exchange_json, received_exchange_json, points, qso_uuid, station_uuid, "
            "qso_version, reservation_uuid, is_invalid, qtc_json) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(qso.contestId);
        query.addBindValue(qso.callsign.trimmed().toUpper());
        query.addBindValue(qso.stationCallsign.trimmed().toUpper());
        query.addBindValue(qso.band.trimmed().toLower());
        query.addBindValue(utcString(qso.timestampUtc));
        query.addBindValue(qso.frequencyHz);
        query.addBindValue(qso.mode);
        query.addBindValue(qso.sentExchange);
        query.addBindValue(qso.receivedExchange);
        query.addBindValue(exchangeJson(qso.sentExchangeFields));
        query.addBindValue(exchangeJson(qso.receivedExchangeFields));
        query.addBindValue(qso.invalid ? 0 : qso.points);
        query.addBindValue(qso.qsoUuid);
        query.addBindValue(qso.stationUuid);
        query.addBindValue(qso.qsoVersion);
        query.addBindValue(qso.reservationUuid.isNull()
            ? QStringLiteral("") : qso.reservationUuid);
        query.addBindValue(qso.invalid ? 1 : 0);
        query.addBindValue(QString::fromUtf8(QJsonDocument(
            QtcService::toJson(qso.qtcTransfers)).toJson(QJsonDocument::Compact)));
        if (!query.exec()) {
            setError(operationError, queryError(query));
            return false;
        }
        savedId = query.lastInsertId().toLongLong();
        qso.id = savedId;
        if (!recordQsoAudit(m_database, logUuid, qso, QStringLiteral("create"),
                            {}, qsoAuditSnapshot(qso, false), 0, nullptr,
                            operationError)) {
            return false;
        }
        if (!qso.reservationUuid.isEmpty()) {
            QSqlQuery consumeReservation(m_database);
            consumeReservation.prepare(QStringLiteral(
                "UPDATE serial_reservations SET status = 'used', qso_uuid = ?, "
                "updated_at_utc = ? WHERE log_uuid = ? AND reservation_uuid = ? "
                "AND station_uuid = ? AND band = ? AND mode = ? AND status = 'reserved'"));
            consumeReservation.addBindValue(qso.qsoUuid);
            consumeReservation.addBindValue(
                utcString(QDateTime::currentDateTimeUtc()));
            consumeReservation.addBindValue(logUuid);
            consumeReservation.addBindValue(qso.reservationUuid);
            consumeReservation.addBindValue(qso.stationUuid);
            consumeReservation.addBindValue(qso.band.trimmed().toLower());
            consumeReservation.addBindValue(qso.mode.trimmed().toUpper());
            if (!consumeReservation.exec()) {
                setError(operationError, queryError(consumeReservation));
                return false;
            }
            if (consumeReservation.numRowsAffected() != 1) {
                setError(operationError, QStringLiteral(
                    "Serial reservation was finalized before the QSO could be saved."));
                return false;
            }
        }
        return enqueueQsoOperation(m_database, qso, QStringLiteral("qso.create"),
                                   0, operationError, qso.logUuid == m_hostedLogUuid);
    }, error);
    if (saved) {
        qso.id = savedId;
    }
    return saved;
}

bool SqliteRepository::updateQso(const Qso &qso, QString *error)
{
    clearError(error);
    if (qso.id <= 0 || qso.contestId <= 0 || qso.callsign.trimmed().isEmpty()
        || qso.stationCallsign.trimmed().isEmpty() || !qso.timestampUtc.isValid()
        || qso.frequencyHz < 0) {
        setError(error, QStringLiteral("QSO is missing required or valid data."));
        return false;
    }

    return withSavepoint(m_database, [this, &qso](QString *operationError) {
        const std::optional<Qso> previous = qsoById(qso.id, operationError);
        if (!previous.has_value()) {
            if (operationError->isEmpty()) {
                setError(operationError,
                         QStringLiteral("QSO %1 does not exist.").arg(qso.id));
            }
            return false;
        }
        QSqlQuery current(m_database);
        current.prepare(QStringLiteral(
            "SELECT q.qso_uuid, q.station_uuid, q.qso_version, c.log_uuid "
            "FROM qsos q JOIN contests c ON c.id = q.contest_id WHERE q.id = ?"));
        current.addBindValue(qso.id);
        if (!current.exec()) {
            setError(operationError, queryError(current));
            return false;
        }
        if (!current.next()) {
            setError(operationError, QStringLiteral("QSO %1 does not exist.").arg(qso.id));
            return false;
        }
        const QString qsoUuid = current.value(0).toString();
        const QString stationUuid = current.value(1).toString();
        const int currentVersion = current.value(2).toInt();
        const QString logUuid = current.value(3).toString();
        current.finish();
        if (qso.qsoVersion != currentVersion) {
            setError(operationError, QStringLiteral(
                "QSO %1 has changed; reload it before editing.").arg(qso.id));
            return false;
        }
        if ((!qso.qsoUuid.isEmpty() && qso.qsoUuid != qsoUuid)
            || (!qso.stationUuid.isEmpty() && qso.stationUuid != stationUuid)
            || (!qso.logUuid.isEmpty() && qso.logUuid != logUuid)) {
            setError(operationError, QStringLiteral("QSO sync identity cannot be changed."));
            return false;
        }

        Qso updated = qso;
        updated.qsoUuid = qsoUuid;
        updated.stationUuid = stationUuid;
        updated.logUuid = logUuid;
        updated.qsoVersion = currentVersion + 1;

        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "UPDATE qsos SET contest_id = ?, callsign = ?, station_callsign = ?, "
            "band = ?, timestamp_utc = ?, frequency_hz = ?, mode = ?, "
            "sent_exchange = ?, received_exchange = ?, sent_exchange_json = ?, "
            "received_exchange_json = ?, points = ?, qso_version = ?, "
            "reservation_uuid = ?, is_invalid = ?, qtc_json = ? "
            "WHERE id = ? AND qso_version = ? AND is_deleted = 0"));
        query.addBindValue(updated.contestId);
        query.addBindValue(updated.callsign.trimmed().toUpper());
        query.addBindValue(updated.stationCallsign.trimmed().toUpper());
        query.addBindValue(updated.band.trimmed().toLower());
        query.addBindValue(utcString(updated.timestampUtc));
        query.addBindValue(updated.frequencyHz);
        query.addBindValue(updated.mode);
        query.addBindValue(updated.sentExchange);
        query.addBindValue(updated.receivedExchange);
        query.addBindValue(exchangeJson(updated.sentExchangeFields));
        query.addBindValue(exchangeJson(updated.receivedExchangeFields));
        query.addBindValue(updated.invalid ? 0 : updated.points);
        query.addBindValue(updated.qsoVersion);
        query.addBindValue(updated.reservationUuid.isNull()
            ? QStringLiteral("") : updated.reservationUuid);
        query.addBindValue(updated.invalid ? 1 : 0);
        query.addBindValue(QString::fromUtf8(QJsonDocument(
            QtcService::toJson(updated.qtcTransfers)).toJson(QJsonDocument::Compact)));
        query.addBindValue(updated.id);
        query.addBindValue(currentVersion);
        if (!query.exec()) {
            setError(operationError, queryError(query));
            return false;
        }
        if (query.numRowsAffected() != 1) {
            setError(operationError, QStringLiteral(
                "QSO %1 changed during update.").arg(qso.id));
            return false;
        }
        if (!recordQsoAudit(
                m_database, logUuid, updated, QStringLiteral("update"),
                qsoAuditSnapshot(*previous, false),
                qsoAuditSnapshot(updated, false), 0, nullptr, operationError)) {
            return false;
        }
        return enqueueQsoOperation(m_database, updated, QStringLiteral("qso.update"),
                                   currentVersion, operationError,
                                   updated.logUuid == m_hostedLogUuid);
    }, error);
}

bool SqliteRepository::deleteQso(qint64 id, QString *error)
{
    clearError(error);
    if (id <= 0) {
        setError(error, QStringLiteral("QSO id must be positive."));
        return false;
    }
    return withSavepoint(m_database, [this, id](QString *operationError) {
        QSqlQuery current(m_database);
        current.prepare(QStringLiteral(
            "SELECT q.contest_id, q.qso_uuid, q.station_uuid, q.qso_version, "
            "q.callsign, q.station_callsign, q.band, q.timestamp_utc, q.frequency_hz, "
            "q.mode, q.sent_exchange, q.received_exchange, q.sent_exchange_json, "
            "q.received_exchange_json, q.points, c.log_uuid, q.reservation_uuid, "
            "q.is_invalid "
            "FROM qsos q JOIN contests c ON c.id = q.contest_id "
            "WHERE q.id = ? AND q.is_deleted = 0"));
        current.addBindValue(id);
        if (!current.exec()) {
            setError(operationError, queryError(current));
            return false;
        }
        if (!current.next()) {
            setError(operationError, QStringLiteral("QSO %1 does not exist.").arg(id));
            return false;
        }
        Qso deleted;
        deleted.id = id;
        deleted.contestId = current.value(0).toLongLong();
        deleted.qsoUuid = current.value(1).toString();
        deleted.stationUuid = current.value(2).toString();
        deleted.qsoVersion = current.value(3).toInt();
        deleted.callsign = current.value(4).toString();
        deleted.stationCallsign = current.value(5).toString();
        deleted.band = current.value(6).toString();
        deleted.timestampUtc = QDateTime::fromString(
            current.value(7).toString(), Qt::ISODateWithMs);
        if (!deleted.timestampUtc.isValid()) {
            deleted.timestampUtc = QDateTime::fromString(
                current.value(7).toString(), Qt::ISODate);
        }
        deleted.frequencyHz = current.value(8).toLongLong();
        deleted.mode = current.value(9).toString();
        deleted.sentExchange = current.value(10).toString();
        deleted.receivedExchange = current.value(11).toString();
        const auto readFields = [&current](int column) {
            QMap<QString, QString> fields;
            const QJsonObject object = QJsonDocument::fromJson(
                current.value(column).toByteArray()).object();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.value().isString()) {
                    fields.insert(it.key(), it.value().toString());
                }
            }
            return fields;
        };
        deleted.sentExchangeFields = readFields(12);
        deleted.receivedExchangeFields = readFields(13);
        deleted.points = current.value(14).toInt();
        deleted.logUuid = current.value(15).toString();
        deleted.reservationUuid = current.value(16).toString();
        deleted.invalid = current.value(17).toBool();
        current.finish();

        const int expectedVersion = deleted.qsoVersion;
        const QJsonObject before = qsoAuditSnapshot(deleted, false);
        ++deleted.qsoVersion;
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE qsos SET is_deleted = 1, qso_version = ? "
            "WHERE id = ? AND qso_version = ? AND is_deleted = 0"));
        update.addBindValue(deleted.qsoVersion);
        update.addBindValue(id);
        update.addBindValue(expectedVersion);
        if (!update.exec() || update.numRowsAffected() != 1) {
            setError(operationError, update.lastError().isValid()
                ? queryError(update)
                : QStringLiteral("QSO changed while it was being deleted."));
            return false;
        }
        if (!recordQsoAudit(
                m_database, deleted.logUuid, deleted, QStringLiteral("delete"),
                before, qsoAuditSnapshot(deleted, true), 0, nullptr,
                operationError)) {
            return false;
        }
        return enqueueQsoOperation(m_database, deleted, QStringLiteral("qso.delete"),
                                   expectedVersion, operationError,
                                   deleted.logUuid == m_hostedLogUuid);
    }, error);
}

bool SqliteRepository::undoLastQsoChange(qint64 contestId, QString *error)
{
    clearError(error);
    if (contestId <= 0) {
        setError(error, QStringLiteral("Contest id must be positive."));
        return false;
    }
    return withSavepoint(m_database, [this, contestId](QString *operationError) {
        QSqlQuery latest(m_database);
        latest.prepare(QStringLiteral(
            "SELECT a.id, a.qso_uuid, a.qso_version, a.action, a.before_json, "
            "a.after_json, c.log_uuid FROM qso_change_audit a "
            "JOIN contests c ON c.log_uuid = a.log_uuid "
            "WHERE c.id = ? AND a.action <> 'undo' AND NOT EXISTS("
            "SELECT 1 FROM qso_change_audit undo WHERE undo.undo_of_id = a.id) "
            "ORDER BY a.id DESC LIMIT 1"));
        latest.addBindValue(contestId);
        if (!latest.exec()) {
            setError(operationError, queryError(latest));
            return false;
        }
        if (!latest.next()) {
            setError(operationError, QStringLiteral("There is no QSO change to undo."));
            return false;
        }
        const qint64 auditId = latest.value(0).toLongLong();
        const QString qsoUuid = latest.value(1).toString();
        const int auditedVersion = latest.value(2).toInt();
        const QString action = latest.value(3).toString();
        const QJsonObject before = QJsonDocument::fromJson(
            latest.value(4).toString().toUtf8()).object();
        const QJsonObject after = QJsonDocument::fromJson(
            latest.value(5).toString().toUtf8()).object();
        const QString logUuid = latest.value(6).toString();
        latest.finish();
        if (action != QStringLiteral("create")
            && action != QStringLiteral("update")
            && action != QStringLiteral("delete")) {
            setError(operationError, QStringLiteral("The latest QSO change cannot be undone."));
            return false;
        }

        QSqlQuery current(m_database);
        current.prepare(QStringLiteral(
            "SELECT q.id, q.contest_id, q.callsign, q.station_callsign, q.band, "
            "q.timestamp_utc, q.frequency_hz, q.mode, q.sent_exchange, "
            "q.received_exchange, q.sent_exchange_json, q.received_exchange_json, "
            "q.points, q.qso_uuid, q.station_uuid, q.qso_version, c.log_uuid, "
            "q.reservation_uuid, q.is_invalid, 'local' AS sync_status, "
            "c.definition_yaml, q.is_deleted, q.qtc_json FROM qsos q "
            "JOIN contests c ON c.id = q.contest_id "
            "WHERE q.qso_uuid = ? AND q.contest_id = ?"));
        current.addBindValue(qsoUuid);
        current.addBindValue(contestId);
        if (!current.exec()) {
            setError(operationError, queryError(current));
            return false;
        }
        if (!current.next()) {
            setError(operationError, QStringLiteral("The QSO to undo no longer exists."));
            return false;
        }
        Qso currentQso = readQso(current);
        const qint64 qsoId = currentQso.id;
        const int currentVersion = currentQso.qsoVersion;
        const bool currentDeleted = current.value(21).toBool();
        QJsonObject currentSnapshot = qsoAuditSnapshot(currentQso, currentDeleted);
        current.finish();
        QSqlQuery currentAudit(m_database);
        currentAudit.prepare(QStringLiteral(
            "SELECT 1 FROM qso_change_audit WHERE log_uuid = ? AND qso_uuid = ? "
            "AND qso_version = ? LIMIT 1"));
        currentAudit.addBindValue(logUuid);
        currentAudit.addBindValue(qsoUuid);
        currentAudit.addBindValue(currentVersion);
        if (!currentAudit.exec()) {
            setError(operationError, queryError(currentAudit));
            return false;
        }
        if (!currentAudit.next()) {
            setError(operationError, QStringLiteral(
                "The current QSO version is not in the local audit; undo was refused."));
            return false;
        }
        currentAudit.finish();
        QJsonObject expectedSnapshot = after;
        currentSnapshot.remove(QStringLiteral("qso_version"));
        expectedSnapshot.remove(QStringLiteral("qso_version"));
        for (const QString &exchange : {QStringLiteral("sent_exchange"),
                                        QStringLiteral("received_exchange")}) {
            if (currentSnapshot.value(exchange) == expectedSnapshot.value(exchange)) {
                const QString fields = exchange + QStringLiteral("_fields");
                currentSnapshot.remove(fields);
                expectedSnapshot.remove(fields);
            }
        }
        if (after.value(QStringLiteral("qso_version")).toInt(-1) != auditedVersion
            || currentSnapshot != expectedSnapshot) {
            QStringList differences;
            for (auto it = expectedSnapshot.begin();
                 it != expectedSnapshot.end(); ++it) {
                if (currentSnapshot.value(it.key()) != it.value()) {
                    differences.append(it.key());
                }
            }
            setError(operationError, QStringLiteral(
                "The QSO changed after the audited operation; undo was not applied "
                "(audit version %1, current version %2; differences: %3).")
                    .arg(auditedVersion)
                    .arg(currentVersion)
                    .arg(differences.join(QStringLiteral(", "))));
            return false;
        }

        Qso restored;
        const QJsonObject target = action == QStringLiteral("create")
            ? after : before;
        restored.id = qsoId;
        restored.contestId = contestId;
        restored.qsoUuid = qsoUuid;
        restored.logUuid = logUuid;
        restored.stationUuid =
            target.value(QStringLiteral("station_uuid")).toString();
        restored.reservationUuid =
            target.value(QStringLiteral("reservation_uuid")).toString();
        restored.callsign = target.value(QStringLiteral("callsign")).toString();
        restored.stationCallsign =
            target.value(QStringLiteral("station_callsign")).toString();
        restored.band = target.value(QStringLiteral("band")).toString();
        restored.timestampUtc = QDateTime::fromString(
            target.value(QStringLiteral("timestamp_utc")).toString(),
            Qt::ISODateWithMs);
        restored.frequencyHz =
            target.value(QStringLiteral("frequency_hz")).toInteger();
        restored.mode = target.value(QStringLiteral("mode")).toString();
        restored.sentExchange =
            target.value(QStringLiteral("sent_exchange")).toString();
        restored.receivedExchange =
            target.value(QStringLiteral("received_exchange")).toString();
        const auto fields = [](const QJsonValue &value) {
            QMap<QString, QString> result;
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.value().isString()) {
                    result.insert(it.key(), it.value().toString());
                }
            }
            return result;
        };
        restored.sentExchangeFields =
            fields(target.value(QStringLiteral("sent_exchange_fields")));
        restored.receivedExchangeFields =
            fields(target.value(QStringLiteral("received_exchange_fields")));
        restored.qtcTransfers = QtcService::fromJson(
            target.value(QStringLiteral("qtc_transfers")).toArray());
        restored.points = target.value(QStringLiteral("points")).toInt();
        restored.invalid =
            target.value(QStringLiteral("is_invalid")).toBool();
        if (!operationError->isEmpty()) {
            return false;
        }
        restored.id = qsoId;
        restored.contestId = contestId;
        restored.qsoUuid = qsoUuid;
        restored.logUuid = logUuid;
        restored.qsoVersion = currentVersion + 1;
        const bool deleted = action == QStringLiteral("create");

        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE qsos SET callsign = ?, station_callsign = ?, band = ?, "
            "timestamp_utc = ?, frequency_hz = ?, mode = ?, sent_exchange = ?, "
            "received_exchange = ?, sent_exchange_json = ?, received_exchange_json = ?, "
            "points = ?, qso_version = ?, reservation_uuid = ?, is_invalid = ?, "
            "is_deleted = ?, qtc_json = ? WHERE id = ? AND qso_version = ?"));
        update.addBindValue(restored.callsign.trimmed().toUpper());
        update.addBindValue(restored.stationCallsign.trimmed().toUpper());
        update.addBindValue(restored.band.trimmed().toLower());
        update.addBindValue(utcString(restored.timestampUtc));
        update.addBindValue(restored.frequencyHz);
        update.addBindValue(restored.mode);
        update.addBindValue(restored.sentExchange);
        update.addBindValue(restored.receivedExchange);
        update.addBindValue(exchangeJson(restored.sentExchangeFields));
        update.addBindValue(exchangeJson(restored.receivedExchangeFields));
        update.addBindValue(restored.invalid ? 0 : restored.points);
        update.addBindValue(restored.qsoVersion);
        update.addBindValue(restored.reservationUuid);
        update.addBindValue(restored.invalid ? 1 : 0);
        update.addBindValue(deleted ? 1 : 0);
        update.addBindValue(QString::fromUtf8(QJsonDocument(
            QtcService::toJson(restored.qtcTransfers))
            .toJson(QJsonDocument::Compact)));
        update.addBindValue(qsoId);
        update.addBindValue(currentVersion);
        if (!update.exec() || update.numRowsAffected() != 1) {
            setError(operationError, update.lastError().isValid()
                ? queryError(update)
                : QStringLiteral("The QSO changed while undo was being applied."));
            return false;
        }

        currentSnapshot = qsoAuditSnapshot(restored, currentDeleted);
        const QJsonObject restoredSnapshot = qsoAuditSnapshot(restored, deleted);
        if (!recordQsoAudit(m_database, logUuid, restored, QStringLiteral("undo"),
                            currentSnapshot, restoredSnapshot, auditId,
                            nullptr, operationError)) {
            return false;
        }
        return enqueueQsoOperation(
            m_database, restored,
            deleted ? QStringLiteral("qso.delete") : QStringLiteral("qso.update"),
            currentVersion, operationError, logUuid == m_hostedLogUuid);
    }, error);
}

std::optional<Qso> SqliteRepository::qsoById(qint64 id, QString *error) const
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT q.id, q.contest_id, q.callsign, q.station_callsign, q.band, q.timestamp_utc, "
        "q.frequency_hz, q.mode, q.sent_exchange, q.received_exchange, "
        "q.sent_exchange_json, q.received_exchange_json, q.points, q.qso_uuid, "
        "q.station_uuid, q.qso_version, c.log_uuid, q.reservation_uuid, "
        "q.is_invalid, q.qtc_json, "
        "CASE WHEN EXISTS(SELECT 1 FROM sync_outbox so "
        "WHERE so.entity_uuid = q.qso_uuid AND so.status = 'conflict') "
        "THEN 'conflict' WHEN EXISTS(SELECT 1 FROM sync_outbox so "
        "WHERE so.entity_uuid = q.qso_uuid AND so.status IN "
        "('pending', 'in-flight', 'failed')) THEN 'pending' "
        "WHEN EXISTS(SELECT 1 FROM sync_outbox so "
        "WHERE so.entity_uuid = q.qso_uuid AND so.status = 'accepted') "
        "THEN 'synced' WHEN EXISTS(SELECT 1 FROM sync_state ss "
        "WHERE ss.log_uuid = c.log_uuid AND ss.enabled = 1) "
        "THEN 'synced' ELSE 'local' END AS sync_status, "
        "c.definition_yaml AS definition_yaml FROM qsos q "
        "LEFT JOIN contests c ON c.id = q.contest_id "
        "WHERE q.id = ? AND q.is_deleted = 0"));
    query.addBindValue(id);
    if (!query.exec()) {
        setError(error, queryError(query));
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return readQso(query);
}

QVector<Qso> SqliteRepository::recentQsos(qint64 contestId, int limit,
                                          QString *error) const
{
    return qsosForContest(contestId, qMax(1, limit), error);
}

bool SqliteRepository::saveSpot(Spot &spot, QString *error)
{
    clearError(error);
    if (spot.callsign.trimmed().isEmpty() || spot.frequencyHz <= 0
        || !spot.timestampUtc.isValid()) {
        setError(error, QStringLiteral("Spot is missing required or valid data."));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO spots(callsign, frequency_hz, mode, spotter, comment, "
        "timestamp_utc, source, band) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(spot.callsign.trimmed().toUpper());
    query.addBindValue(spot.frequencyHz);
    QString mode = spot.mode.trimmed().toUpper();
    if (mode.isNull()) {
        mode = QStringLiteral("");
    }
    query.addBindValue(mode);
    query.addBindValue(spot.spotter.trimmed().toUpper());
    query.addBindValue(spot.comment);
    query.addBindValue(utcString(spot.timestampUtc));
    query.addBindValue(spot.source);
    query.addBindValue(spot.band);
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    spot.id = query.lastInsertId().toLongLong();
    ++m_spotInsertCount;
    if (m_spotInsertCount >= SpotPruneInterval) {
        m_spotInsertCount = 0;
        const QDateTime cutoff = QDateTime::currentDateTimeUtc()
            .addSecs(-SpotRetentionHours * 60 * 60);
        if (!pruneSpots(cutoff, MaximumRetainedSpots, error)) {
            return false;
        }
    }
    return true;
}

bool SqliteRepository::pruneSpots(const QDateTime &olderThanUtc, int maxRows,
                                  QString *error)
{
    clearError(error);
    if (!olderThanUtc.isValid()) {
        setError(error, QStringLiteral("Spot retention cutoff is invalid."));
        return false;
    }
    QSqlQuery oldSpots(m_database);
    oldSpots.prepare(QStringLiteral("DELETE FROM spots WHERE timestamp_utc < ?"));
    oldSpots.addBindValue(utcString(olderThanUtc));
    if (!oldSpots.exec()) {
        setError(error, queryError(oldSpots));
        return false;
    }
    QSqlQuery excess(m_database);
    excess.prepare(QStringLiteral(
        "DELETE FROM spots WHERE id NOT IN ("
        "SELECT id FROM spots ORDER BY timestamp_utc DESC, id DESC LIMIT ?)"));
    excess.addBindValue(qMax(0, maxRows));
    if (!excess.exec()) {
        setError(error, queryError(excess));
        return false;
    }
    return true;
}

std::optional<qint64> SqliteRepository::findDuplicate(
    qint64 contestId, const QString &callsign, const QString &band,
    const QString &mode, qint64 excludeQsoId, QString *error) const
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id FROM qsos WHERE contest_id = ? AND is_deleted = 0 "
        "AND is_invalid = 0 "
        "AND callsign = ? COLLATE NOCASE AND band = ? COLLATE NOCASE "
        "AND mode = ? COLLATE NOCASE AND (? <= 0 OR id <> ?) "
        "ORDER BY id LIMIT 1"));
    query.addBindValue(contestId);
    query.addBindValue(callsign.trimmed());
    query.addBindValue(band.trimmed());
    query.addBindValue(mode.trimmed());
    query.addBindValue(excludeQsoId);
    query.addBindValue(excludeQsoId);
    if (!query.exec()) {
        setError(error, queryError(query));
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    return query.value(0).toLongLong();
}

QVector<Spot> SqliteRepository::recentSpots(int limit, QString *error) const
{
    clearError(error);
    QVector<Spot> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, callsign, frequency_hz, mode, spotter, comment, "
        "timestamp_utc, source, band FROM spots "
        "ORDER BY timestamp_utc DESC, id DESC LIMIT ?"));
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        Spot spot;
        spot.id = query.value(QStringLiteral("id")).toLongLong();
        spot.callsign = query.value(QStringLiteral("callsign")).toString();
        spot.frequencyHz = query.value(QStringLiteral("frequency_hz")).toLongLong();
        spot.mode = query.value(QStringLiteral("mode")).toString();
        spot.spotter = query.value(QStringLiteral("spotter")).toString();
        spot.comment = query.value(QStringLiteral("comment")).toString();
        spot.timestampUtc = QDateTime::fromString(
            query.value(QStringLiteral("timestamp_utc")).toString(), Qt::ISODateWithMs);
        if (!spot.timestampUtc.isValid()) {
            spot.timestampUtc = QDateTime::fromString(
                query.value(QStringLiteral("timestamp_utc")).toString(), Qt::ISODate);
        }
        spot.timestampUtc = spot.timestampUtc.toUTC();
        spot.source = query.value(QStringLiteral("source")).toString();
        spot.band = query.value(QStringLiteral("band")).toString();
        result.append(std::move(spot));
    }
    return result;
}

QVector<Qso> SqliteRepository::qsosForContest(qint64 contestId, int limit,
                                              QString *error) const
{
    clearError(error);
    QVector<Qso> result;
    const QString select = QStringLiteral(
        "SELECT q.id, q.contest_id, q.callsign, q.station_callsign, q.band, q.timestamp_utc, "
        "q.frequency_hz, q.mode, q.sent_exchange, q.received_exchange, "
        "q.sent_exchange_json, q.received_exchange_json, q.points, q.qso_uuid, "
        "q.station_uuid, q.qso_version, c.log_uuid, q.reservation_uuid, "
        "q.is_invalid, q.qtc_json, "
        "CASE WHEN EXISTS(SELECT 1 FROM sync_outbox so "
        "WHERE so.entity_uuid = q.qso_uuid AND so.status = 'conflict') "
        "THEN 'conflict' WHEN EXISTS(SELECT 1 FROM sync_outbox so "
        "WHERE so.entity_uuid = q.qso_uuid AND so.status IN "
        "('pending', 'in-flight', 'failed')) THEN 'pending' "
        "WHEN EXISTS(SELECT 1 FROM sync_outbox so "
        "WHERE so.entity_uuid = q.qso_uuid AND so.status = 'accepted') "
        "THEN 'synced' WHEN EXISTS(SELECT 1 FROM sync_state ss "
        "WHERE ss.log_uuid = c.log_uuid AND ss.enabled = 1) "
        "THEN 'synced' ELSE 'local' END AS sync_status, "
        "c.definition_yaml AS definition_yaml FROM qsos q "
        "LEFT JOIN contests c ON c.id = q.contest_id "
        "WHERE q.contest_id = ? AND q.is_deleted = 0 "
        "ORDER BY q.timestamp_utc DESC, q.id DESC");
    QSqlQuery query(m_database);
    if (limit > 0) {
        query.prepare(select + QStringLiteral(" LIMIT ?"));
        query.addBindValue(contestId);
        query.addBindValue(limit);
    } else {
        query.prepare(select);
        query.addBindValue(contestId);
    }
    if (!query.exec()) {
        setError(error, queryError(query));
        return result;
    }
    while (query.next()) {
        result.append(readQso(query));
    }
    return result;
}

bool SqliteRepository::withTransaction(
    const std::function<bool(QString *)> &operation, QString *error)
{
    clearError(error);
    if (!operation) {
        setError(error, QStringLiteral("Transaction operation is empty."));
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }

    QString operationError;
    if (!operation(&operationError)) {
        const QString failure = operationError.isEmpty()
            ? QStringLiteral("Transaction operation failed.")
            : operationError;
        setError(error, rollbackError(m_database, failure));
        return false;
    }
    if (!m_database.commit()) {
        setError(error, rollbackError(m_database, m_database.lastError().text()));
        return false;
    }
    return true;
}

} // namespace logger
