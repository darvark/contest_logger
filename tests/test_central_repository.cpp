#include "logger/data/central_repository.h"

#include "logger/contest/contest_definition_loader.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <QUuid>

namespace {

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

QJsonObject sampleOperation(const QString &logUuid, const QString &stationUuid,
                            const QString &operationUuid, const QString &qsoUuid,
                            const QString &kind, int expectedVersion,
                            const QString &callsign)
{
    const QString now = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs);
    return {
        {QStringLiteral("operation_uuid"), operationUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("created_at_utc"), now},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("expected_version"), expectedVersion},
        {QStringLiteral("qso"), QJsonObject{
            {QStringLiteral("qso_uuid"), qsoUuid},
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
            {QStringLiteral("callsign"), callsign},
            {QStringLiteral("timestamp_utc"), now},
            {QStringLiteral("frequency_hz"), 14025000},
            {QStringLiteral("band"), QStringLiteral("20m")},
            {QStringLiteral("mode"), QStringLiteral("CW")},
            {QStringLiteral("sent_exchange_fields"), QJsonObject{
                {QStringLiteral("report"), QStringLiteral("599")}}},
            {QStringLiteral("received_exchange_fields"), QJsonObject{
                {QStringLiteral("report"), QStringLiteral("599")}}},
            {QStringLiteral("sent_exchange"), QStringLiteral("599")},
            {QStringLiteral("received_exchange"), QStringLiteral("599")},
            {QStringLiteral("points"), 2},
            {QStringLiteral("is_invalid"), false}
        }}
    };
}

QString serialContestYaml(const QString &scope, bool serverRequired)
{
    return QStringLiteral(R"yaml(
schema_version: 1
id: serial-test
name: Serial Test
bands: [20m, 40m]
modes: [CW, SSB]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
    - {name: serial, type: serial, generated: true, width: 3}
  received:
    - {name: report, type: rst}
scoring:
  serial_start: 7
  serial_scope: %1
  serial_server_required: %2
dupe_key: [callsign, band, mode]
)yaml").arg(scope, serverRequired ? QStringLiteral("true")
                                      : QStringLiteral("false"));
}

qint64 createSerialLog(logger::CentralRepository &central, const QString &scope,
                       bool serverRequired, QString *logUuid, QString *error)
{
    const QString yaml = serialContestYaml(scope, serverRequired);
    logger::ContestDefinition definition;
    if (!logger::ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("serial test"), definition, error)) {
        return 0;
    }
    logger::Contest log;
    log.name = QStringLiteral("Serial test");
    log.stationCallsign = QStringLiteral("K1TEST");
    const qint64 id = central.repository()->createContestLog(
        definition, yaml, log, error);
    if (id > 0) {
        const auto logs = central.repository()->contests(error);
        const auto found = std::find_if(logs.cbegin(), logs.cend(),
            [id](const logger::Contest &contest) { return contest.id == id; });
        if (found != logs.cend()) {
            *logUuid = found->logUuid;
        }
    }
    return id;
}

} // namespace

class CentralRepositoryTest final : public QObject {
    Q_OBJECT

private slots:
    void initializesHeadlessCentralDatabase();
    void rejectsConcurrentCentralRepository();
    void backsUpBeforeSchemaMigration();
    void restoresCentralBackupWithRevisionAndIdempotency();
    void recordsResumableLogImportAudit();
    void commitsIdempotentRevisionsAtomically();
    void synchronizesWaeQtcWithItsPartnerQso();
    void persistsClientAndTransactionDiagnostics();
    void compactsAcknowledgedEventsAndKeepsSnapshotsAvailable();
    void exportsPagedSnapshotAtPinnedRevision();
    void reservesSerialsIdempotentlyByConfiguredScope();
    void correctsUnusedSerialReservationWithAudit();
    void requiresAndConsumesReservedSerialOnQsoCommit();
    void createsLogsAndImportsHistoricalSerialsIdempotently();
    void validatesDuplicateKeysAndRecalculatesPointsOnServer();
};

void CentralRepositoryTest::initializesHeadlessCentralDatabase()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(central.databasePath()));
    QVERIFY(QFileInfo(central.backupDirectory()).isDir());
    QVERIFY(central.repository() != nullptr);
    const auto logs = central.repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logs.size(), 1);
    QCOMPARE(logs.first().code, QStringLiteral("GENERAL"));
}

void CentralRepositoryTest::rejectsConcurrentCentralRepository()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository first;
    QString error;
    QVERIFY2(first.initialize(directory.path(), &error), qPrintable(error));

    logger::CentralRepository second;
    QVERIFY(!second.initialize(directory.path(), &error));
    QVERIFY(error.contains(QStringLiteral("already locked")));
}

void CentralRepositoryTest::backsUpBeforeSchemaMigration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.path();
    {
        logger::CentralRepository central;
        QString error;
        QVERIFY2(central.initialize(path, &error), qPrintable(error));
    }

    const QString connectionName = QStringLiteral("central-migration-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(QDir(path).filePath(
            QStringLiteral("central-log.sqlite")));
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "DELETE FROM schema_migrations WHERE version >= 9")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE log_file")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE log_settings")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE log_assets")));
        QVERIFY(query.exec(QStringLiteral("ALTER TABLE qsos DROP COLUMN qtc_json")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE qso_change_audit")));
        QVERIFY(query.exec(QStringLiteral(
            "DROP TABLE sync_transaction_metrics")));
        QVERIFY(query.exec(QStringLiteral(
            "DROP TABLE serial_reservation_adjustments")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_import_audit")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_conflict_resolutions")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE serial_reservation_requests")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE serial_reservations")));
        QVERIFY(query.exec(QStringLiteral("ALTER TABLE qsos DROP COLUMN reservation_uuid")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_events")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE qso_history")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_operations")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_clients")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_log_state")));
        QVERIFY(query.exec(QStringLiteral("ALTER TABLE qsos DROP COLUMN is_deleted")));
        QVERIFY(query.exec(QStringLiteral("ALTER TABLE qsos DROP COLUMN is_invalid")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_conflicts")));
        QVERIFY(query.exec(QStringLiteral("DROP INDEX idx_sync_outbox_log_status_created")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_outbox")));
        QVERIFY(query.exec(QStringLiteral("DROP TABLE sync_state")));
        query.finish();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connectionName);

    logger::CentralRepository migrated;
    QString error;
    QVERIFY2(migrated.initialize(path, &error), qPrintable(error));
    const QStringList backups = QDir(migrated.backupDirectory()).entryList(
        {QStringLiteral("pre-migration-8-*.sqlite")}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    const auto logs = migrated.repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logs.size(), 1);
}

void CentralRepositoryTest::restoresCentralBackupWithRevisionAndIdempotency()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString backupPath =
        directory.filePath(QStringLiteral("central-backup.sqlite"));
    QString logUuid;
    QString stationUuid;
    QString serialLogUuid;
    QJsonObject create;
    QJsonArray committedResults;
    {
        logger::CentralRepository central;
        QString error;
        QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
        const QVector<logger::Contest> logs = central.repository()->contests(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        logUuid = logs.first().logUuid;
        stationUuid = central.repository()->localStationUuid(&error);
        QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));
        QVERIFY(createSerialLog(
            central, QStringLiteral("band"), false, &serialLogUuid, &error) > 0);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonObject reservation = central.reserveSerial(
            serialLogUuid, stationUuid, newUuid(),
            QStringLiteral("20M"), QStringLiteral("CW"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(reservation.value(QStringLiteral("serial")).toInt(), 7);
        create = sampleOperation(
            logUuid, stationUuid, newUuid(), newUuid(),
            QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
        QVERIFY2(central.submitBatch(
                     logUuid, {create}, &committedResults, &error), qPrintable(error));
        QCOMPARE(committedResults.first().toObject()
                     .value(QStringLiteral("revision")).toInteger(), qint64(1));
        QVERIFY2(central.repository()->backupTo(backupPath, &error), qPrintable(error));
    }

    const QString databasePath =
        QDir(directory.path()).filePath(QStringLiteral("central-log.sqlite"));
    {
        const QString connectionName = QStringLiteral("central-restore-corruption-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery corrupt(database);
        QVERIFY(corrupt.exec(QStringLiteral("DELETE FROM sync_events")));
        QVERIFY(corrupt.exec(QStringLiteral("DELETE FROM qso_history")));
        QVERIFY(corrupt.exec(QStringLiteral("DELETE FROM sync_operations")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
    QString error;
    QVERIFY2(logger::SqliteRepository::restoreFromBackup(
                 databasePath, backupPath, &error), qPrintable(error));
    const QStringList preservedDatabases = QDir(directory.path()).entryList(
        {QStringLiteral("central-log.sqlite.before-restore-*.sqlite")}, QDir::Files);
    QCOMPARE(preservedDatabases.size(), 1);
    {
        logger::CentralRepository restored;
        QVERIFY2(restored.initialize(directory.path(), &error), qPrintable(error));
        QCOMPARE(restored.currentRevision(logUuid, &error), qint64(1));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonObject current = restored.currentQso(
            logUuid, create.value(QStringLiteral("qso")).toObject()
                         .value(QStringLiteral("qso_uuid")).toString(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(current.value(QStringLiteral("callsign")).toString(),
                 QStringLiteral("W1AW"));
        QJsonArray replayResults;
        QVERIFY2(restored.submitBatch(
                     logUuid, {create}, &replayResults, &error), qPrintable(error));
        QCOMPARE(replayResults, committedResults);
        QCOMPARE(restored.currentRevision(logUuid, &error), qint64(1));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray reservations =
            restored.serialReservations(serialLogUuid, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(reservations.size(), 1);
        QCOMPARE(reservations.first().toObject()
                     .value(QStringLiteral("serial")).toInt(), 7);
    }
}

void CentralRepositoryTest::recordsResumableLogImportAudit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    QString logUuid;
    QVERIFY(createSerialLog(central, QStringLiteral("global"), false,
                            &logUuid, &error) > 0);
    const QString sourceLogUuid = newUuid();
    const QString stationUuid = newUuid();
    const QString importUuid = central.beginLogImport(
        logUuid, sourceLogUuid, QStringLiteral("Local source"), stationUuid, 2, &error);
    QVERIFY2(!importUuid.isEmpty(), qPrintable(error));
    const QJsonObject firstOperation = sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(), QStringLiteral("qso.create"),
        0, QStringLiteral("W1AW"));
    QJsonArray results;
    QVERIFY2(central.submitBatch(logUuid, {firstOperation}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.size(), 1);
    QVERIFY2(central.updateLogImport(
        importUuid, 1, 0, QStringLiteral("in_progress"), {}, &error),
        qPrintable(error));

    const QString resumedUuid = central.beginLogImport(
        logUuid, sourceLogUuid, QStringLiteral("Local source"), stationUuid, 2, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(resumedUuid, importUuid);
    const QJsonObject secondOperation = sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(), QStringLiteral("qso.create"),
        0, QStringLiteral("K2XYZ"));
    QVERIFY2(central.submitBatch(logUuid, {secondOperation}, &results, &error),
             qPrintable(error));
    QVERIFY2(central.updateLogImport(
        importUuid, 2, 0, QStringLiteral("completed"), {}, &error),
        qPrintable(error));
    const qint64 revision = central.currentRevision(logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(central.submitBatch(logUuid, {firstOperation}, &results, &error),
             qPrintable(error));
    QCOMPARE(central.currentRevision(logUuid, &error), revision);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonObject audit = central.logImportAudit(importUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(audit.value(QStringLiteral("total_qso")).toInt(), 2);
    QCOMPARE(audit.value(QStringLiteral("imported_qso")).toInt(), 2);
    QCOMPARE(audit.value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QVERIFY(!central.updateLogImport(
        importUuid, 3, 0, QStringLiteral("completed"), {}, &error));
    QVERIFY(error.contains(QStringLiteral("counts exceed total")));
}

void CentralRepositoryTest::commitsIdempotentRevisionsAtomically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    const auto logs = central.repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString logUuid = logs.first().logUuid;
    const QString stationUuid = central.repository()->localStationUuid(&error);
    QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));

    const QString operationUuid = newUuid();
    const QString qsoUuid = newUuid();
    const QJsonObject create = sampleOperation(
        logUuid, stationUuid, operationUuid, qsoUuid,
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
    QJsonArray results;
    QVERIFY2(central.submitBatch(logUuid, {create}, &results, &error), qPrintable(error));
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().toObject().value(QStringLiteral("revision")).toInteger(),
             qint64(1));
    QCOMPARE(central.currentRevision(logUuid, &error), qint64(1));
    QCOMPARE(central.eventsSince(logUuid, 0, 10, &error).size(), 1);

    QJsonArray replayResults;
    QVERIFY2(central.submitBatch(
                 logUuid, {create}, &replayResults, &error), qPrintable(error));
    QCOMPARE(replayResults, results);
    QCOMPARE(central.currentRevision(logUuid, &error), qint64(1));

    const QJsonObject duplicate = sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
    QVERIFY(!central.submitBatch(logUuid, {duplicate}, &replayResults, &error));
    QVERIFY(error.contains(QStringLiteral("DUPLICATE_QSO")));
    QCOMPARE(central.currentRevision(logUuid), qint64(1));

    QJsonObject reusedKey = create;
    QJsonObject reusedQso = reusedKey.value(QStringLiteral("qso")).toObject();
    reusedQso.insert(QStringLiteral("callsign"), QStringLiteral("N0CALL"));
    reusedKey.insert(QStringLiteral("qso"), reusedQso);
    QVERIFY(!central.submitBatch(logUuid, {reusedKey}, &replayResults, &error));
    QVERIFY(error.contains(QStringLiteral("IDEMPOTENCY_KEY_REUSED")));
    QCOMPARE(central.currentRevision(logUuid), qint64(1));

    const QJsonObject uncommittedCreate = sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("K2TEST"));
    const QJsonObject staleUpdate = sampleOperation(
        logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 5, QStringLiteral("W1AW"));
    QVERIFY(!central.submitBatch(
        logUuid, {uncommittedCreate, staleUpdate}, &replayResults, &error));
    QVERIFY(error.contains(QStringLiteral("VERSION_CONFLICT")));
    QCOMPARE(central.currentRevision(logUuid), qint64(1));
    QCOMPARE(central.eventsSince(logUuid, 0, 10).size(), 1);
    const auto storedQsos = central.repository()->qsosForContest(logs.first().id, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(storedQsos.size(), 1);

    QJsonObject update = sampleOperation(
        logUuid, newUuid(), newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 1, QStringLiteral("W1AW"));
    QJsonObject updateQso = update.value(QStringLiteral("qso")).toObject();
    updateQso.insert(QStringLiteral("station_uuid"), stationUuid);
    update.insert(QStringLiteral("qso"), updateQso);
    QVERIFY2(central.submitBatch(logUuid, {update}, &replayResults, &error),
             qPrintable(error));
    QCOMPARE(replayResults.first().toObject()
                 .value(QStringLiteral("qso_version")).toInt(), 2);
    QCOMPARE(central.currentRevision(logUuid), qint64(2));

    QJsonObject remove = sampleOperation(
        logUuid, newUuid(), newUuid(), qsoUuid,
        QStringLiteral("qso.delete"), 2, QStringLiteral("W1AW"));
    QJsonObject removeQso = remove.value(QStringLiteral("qso")).toObject();
    removeQso.insert(QStringLiteral("station_uuid"), stationUuid);
    remove.insert(QStringLiteral("qso"), removeQso);
    QVERIFY2(central.submitBatch(logUuid, {remove}, &replayResults, &error),
             qPrintable(error));
    QCOMPARE(replayResults.first().toObject()
                 .value(QStringLiteral("qso_version")).toInt(), 3);
    QCOMPARE(central.currentRevision(logUuid), qint64(3));
    QVERIFY2(central.recordClientCursor(logUuid, stationUuid, 2, &error),
             qPrintable(error));
    QCOMPARE(central.clientCursor(logUuid, stationUuid, &error), qint64(2));
    QVERIFY(!central.recordClientCursor(logUuid, stationUuid, 4, &error));
    QVERIFY(error.contains(QStringLiteral("cannot exceed")));
    QVERIFY(!central.recordClientCursor(logUuid, stationUuid, 1, &error));
    QVERIFY(error.contains(QStringLiteral("move backwards")));
    const QJsonArray events = central.eventsSince(logUuid, 0, 10, &error);
    QCOMPARE(events.size(), 3);
    QCOMPARE(events.last().toObject().value(QStringLiteral("revision")).toInteger(),
             qint64(3));
    QCOMPARE(events.last().toObject().value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("is_deleted")).toBool(), true);
}

void CentralRepositoryTest::synchronizesWaeQtcWithItsPartnerQso()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    const auto logs = central.repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const logger::Contest log = logs.first();
    const QString stationUuid = central.repository()->localStationUuid(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString qsoUuid = newUuid();
    QJsonObject operation = sampleOperation(
        log.logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.create"), 0, QStringLiteral("DL1ABC"));
    QJsonObject qso = operation.value(QStringLiteral("qso")).toObject();
    qso.insert(QStringLiteral("qtc_transfers"), QJsonArray{QJsonObject{
        {QStringLiteral("qtc_uuid"), newUuid()},
        {QStringLiteral("partner_qso_uuid"), qsoUuid},
        {QStringLiteral("partner_callsign"), QStringLiteral("DL1ABC")},
        {QStringLiteral("direction"), QStringLiteral("received")},
        {QStringLiteral("group_number"), 1},
        {QStringLiteral("messages"), QJsonArray{QJsonObject{
            {QStringLiteral("timestamp_utc"),
             QStringLiteral("2026-10-15T09:40:00.000Z")},
            {QStringLiteral("callsign"), QStringLiteral("DL2XYZ")},
            {QStringLiteral("serial_number"), 18}
        }}}
    }});
    operation.insert(QStringLiteral("qso"), qso);
    QJsonArray results;
    QVERIFY2(central.submitBatch(log.logUuid, {operation}, &results, &error),
             qPrintable(error));

    const QJsonArray events = central.eventsSince(log.logUuid, 0, 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().toObject().value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("qtc_transfers")).toArray().size(), 1);
    const QJsonObject snapshot = central.snapshotPage(log.logUuid, 1, 0, 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonArray snapshotQsos = snapshot.value(QStringLiteral("qsos")).toArray();
    QCOMPARE(snapshotQsos.size(), 1);
    QCOMPARE(snapshotQsos.first().toObject()
                 .value(QStringLiteral("qtc_transfers")).toArray().size(), 1);
}

void CentralRepositoryTest::persistsClientAndTransactionDiagnostics()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logUuid;
    QString stationUuid;
    {
        logger::CentralRepository central;
        QString error;
        QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
        const auto logs = central.repository()->contests(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        logUuid = logs.first().logUuid;
        stationUuid = central.repository()->localStationUuid(&error);
        QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));
        QVERIFY2(central.recordClientHeartbeat(
                     logUuid, stationUuid, 0, 3, 2, 1, &error),
                 qPrintable(error));
        QVERIFY2(central.recordSyncTransactionMetric(
                     logUuid, stationUuid, 14, QStringLiteral("success"), 2, &error),
                 qPrintable(error));
        QVERIFY2(central.recordSyncTransactionMetric(
                     logUuid, stationUuid, 28, QStringLiteral("conflict"), 1, &error),
                 qPrintable(error));
        QVERIFY2(central.recordSyncTransactionMetric(
                     logUuid, stationUuid, 7, QStringLiteral("error"), 1, &error),
                 qPrintable(error));
    }
    {
        logger::CentralRepository restarted;
        QString error;
        QVERIFY2(restarted.initialize(directory.path(), &error), qPrintable(error));
        const QJsonObject metrics =
            restarted.synchronizationMetrics(logUuid, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray clients = metrics.value(QStringLiteral("clients")).toArray();
        QCOMPARE(clients.size(), 1);
        const QJsonObject client = clients.first().toObject();
        QCOMPARE(client.value(QStringLiteral("station_uuid")).toString(), stationUuid);
        QCOMPARE(client.value(QStringLiteral("pending_count")).toInt(), 3);
        QCOMPARE(client.value(QStringLiteral("error_count")).toInt(), 2);
        QCOMPARE(client.value(QStringLiteral("conflict_count")).toInt(), 1);
        QVERIFY(!client.value(QStringLiteral("last_contact_utc")).toString().isEmpty());
        QCOMPARE(metrics.value(QStringLiteral("transaction_count")).toInt(), 3);
        QCOMPARE(metrics.value(QStringLiteral("transaction_maximum_ms")).toInt(), 28);
        QCOMPARE(metrics.value(QStringLiteral("transaction_error_count")).toInt(), 1);
        QCOMPARE(metrics.value(QStringLiteral("transaction_conflict_count")).toInt(), 1);
        QCOMPARE(metrics.value(QStringLiteral("transaction_history")).toArray().size(), 3);
    }
}

void CentralRepositoryTest::exportsPagedSnapshotAtPinnedRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    const auto logs = central.repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString logUuid = logs.first().logUuid;
    const QString stationUuid = central.repository()->localStationUuid(&error);
    const QString qsoUuid = newUuid();
    const QString operationUuid = newUuid();
    QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));

    QJsonArray results;
    QVERIFY2(central.submitBatch(logUuid, QJsonArray{sampleOperation(
        logUuid, stationUuid, operationUuid, qsoUuid,
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"))},
        &results, &error), qPrintable(error));
    const qint64 pinnedRevision = central.currentRevision(logUuid, &error);
    QCOMPARE(pinnedRevision, qint64(1));

    QVERIFY2(central.submitBatch(logUuid, QJsonArray{sampleOperation(
        logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 1, QStringLiteral("W1AW/P"))},
        &results, &error), qPrintable(error));
    QVERIFY2(central.submitBatch(logUuid, QJsonArray{sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("K2TEST"))},
        &results, &error), qPrintable(error));

    const QJsonObject page = central.snapshotPage(logUuid, pinnedRevision, 0, 1, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(page.value(QStringLiteral("snapshot_revision")).toInteger(), pinnedRevision);
    QCOMPARE(page.value(QStringLiteral("total_qsos")).toInt(), 1);
    QCOMPARE(page.value(QStringLiteral("has_more")).toBool(), false);
    const QJsonArray qsos = page.value(QStringLiteral("qsos")).toArray();
    QCOMPARE(qsos.size(), 1);
    QCOMPARE(qsos.first().toObject().value(QStringLiteral("callsign")).toString(),
             QStringLiteral("W1AW"));
    QCOMPARE(qsos.first().toObject().value(QStringLiteral("qso_version")).toInt(), 1);
    QCOMPARE(page.value(QStringLiteral("sha256")).toString(),
             QString::fromLatin1(QCryptographicHash::hash(
                 QJsonDocument(qsos).toJson(QJsonDocument::Compact),
                 QCryptographicHash::Sha256).toHex()));

    const QJsonObject nextPage = central.snapshotPage(
        logUuid, pinnedRevision, 1, 1, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(nextPage.value(QStringLiteral("qsos")).toArray().isEmpty());
    QCOMPARE(nextPage.value(QStringLiteral("sha256")).toString(),
             page.value(QStringLiteral("sha256")).toString());

    const qint64 latestRevision = central.currentRevision(logUuid, &error);
    QCOMPARE(latestRevision, qint64(3));
    const QJsonObject latest = central.snapshotPage(
        logUuid, latestRevision, 0, 1, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(latest.value(QStringLiteral("total_qsos")).toInt(), 2);
    QVERIFY(latest.value(QStringLiteral("has_more")).toBool());
    const QJsonObject latestSecondPage = central.snapshotPage(
        logUuid, latestRevision, 1, 1, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!latestSecondPage.value(QStringLiteral("has_more")).toBool());
    QCOMPARE(latest.value(QStringLiteral("sha256")).toString(),
             latestSecondPage.value(QStringLiteral("sha256")).toString());
    QJsonArray completeSnapshot = latest.value(QStringLiteral("qsos")).toArray();
    const QJsonArray remaining = latestSecondPage.value(QStringLiteral("qsos")).toArray();
    QCOMPARE(remaining.size(), 1);
    completeSnapshot.append(remaining.first());
    QVERIFY(std::any_of(completeSnapshot.cbegin(), completeSnapshot.cend(),
        [](const QJsonValue &value) {
            return value.toObject().value(QStringLiteral("callsign")).toString()
                       == QStringLiteral("W1AW/P")
                && value.toObject().value(QStringLiteral("qso_version")).toInt() == 2;
        }));
    QCOMPARE(latest.value(QStringLiteral("sha256")).toString(),
             QString::fromLatin1(QCryptographicHash::hash(
                 QJsonDocument(completeSnapshot).toJson(QJsonDocument::Compact),
                 QCryptographicHash::Sha256).toHex()));
}

void CentralRepositoryTest::compactsAcknowledgedEventsAndKeepsSnapshotsAvailable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    const auto logs = central.repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString logUuid = logs.first().logUuid;
    const QString stationUuid = central.repository()->localStationUuid(&error);
    const QString qsoUuid = newUuid();
    QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));

    QJsonArray results;
    QVERIFY2(central.submitBatch(logUuid, {sampleOperation(
        logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"))},
        &results, &error), qPrintable(error));
    QVERIFY2(central.submitBatch(logUuid, {sampleOperation(
        logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 1, QStringLiteral("W1AW/P"))},
        &results, &error), qPrintable(error));
    QVERIFY2(central.submitBatch(logUuid, {sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("K2TEST"))},
        &results, &error), qPrintable(error));

    const QString connectionName = QStringLiteral("central-retention-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(central.databasePath());
        QVERIFY(database.open());
        QSqlQuery ageEvents(database);
        ageEvents.prepare(QStringLiteral(
            "UPDATE sync_events SET created_at_utc = ? WHERE log_uuid = ?"));
        ageEvents.addBindValue(QDateTime::currentDateTimeUtc().addDays(-60)
                                   .toString(Qt::ISODateWithMs));
        ageEvents.addBindValue(logUuid);
        QVERIFY2(ageEvents.exec(), qPrintable(ageEvents.lastError().text()));
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY2(central.recordClientCursor(logUuid, stationUuid, 1, &error),
             qPrintable(error));
    int removedEvents = 0;
    QVERIFY2(central.compactSyncEvents(
                 logUuid, QDateTime::currentDateTimeUtc().addDays(-30),
                 &removedEvents, &error), qPrintable(error));
    QCOMPARE(removedEvents, 1);
    QCOMPARE(central.earliestRevision(logUuid, &error), qint64(2));
    QCOMPARE(central.eventsSince(logUuid, 0, 10, &error).size(), 2);

    const QJsonObject historicalSnapshot =
        central.snapshotPage(logUuid, 1, 0, 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonArray historicalQsos =
        historicalSnapshot.value(QStringLiteral("qsos")).toArray();
    QCOMPARE(historicalQsos.size(), 1);
    QCOMPARE(historicalQsos.first().toObject()
                 .value(QStringLiteral("callsign")).toString(),
             QStringLiteral("W1AW"));

    QVERIFY2(central.recordClientHeartbeat(logUuid, stationUuid, 2, &error),
             qPrintable(error));
    QCOMPARE(central.clientCursor(logUuid, stationUuid, &error), qint64(2));
    QVERIFY2(central.recordClientHeartbeat(logUuid, stationUuid, 1, &error),
             qPrintable(error));
    QCOMPARE(central.clientCursor(logUuid, stationUuid, &error), qint64(2));
    QVERIFY(!central.recordClientHeartbeat(logUuid, stationUuid, 4, &error));
    QVERIFY2(central.compactSyncEvents(
                 logUuid, QDateTime::currentDateTimeUtc().addDays(-30),
                 &removedEvents, &error), qPrintable(error));
    QCOMPARE(removedEvents, 1);
    QCOMPARE(central.earliestRevision(logUuid, &error), qint64(3));
    QCOMPARE(central.eventsSince(logUuid, 0, 10, &error).size(), 1);

    QVERIFY2(central.recordClientCursor(logUuid, stationUuid, 3, &error),
             qPrintable(error));
    QVERIFY2(central.compactSyncEvents(
                 logUuid, QDateTime::currentDateTimeUtc().addDays(-30),
                 &removedEvents, &error), qPrintable(error));
    QCOMPARE(removedEvents, 1);
    QCOMPARE(central.earliestRevision(logUuid, &error), qint64(4));
    QVERIFY(central.eventsSince(logUuid, 0, 10, &error).isEmpty());

    const QJsonObject compactedSnapshot =
        central.snapshotPage(logUuid, 3, 0, 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(compactedSnapshot.value(QStringLiteral("total_qsos")).toInt(), 2);
    const QJsonArray compactedQsos =
        compactedSnapshot.value(QStringLiteral("qsos")).toArray();
    QVERIFY(std::any_of(compactedQsos.cbegin(), compactedQsos.cend(),
        [&qsoUuid](const QJsonValue &value) {
            const QJsonObject qso = value.toObject();
            return qso.value(QStringLiteral("qso_uuid")).toString() == qsoUuid
                && qso.value(QStringLiteral("callsign")).toString()
                    == QStringLiteral("W1AW/P")
                && qso.value(QStringLiteral("qso_version")).toInt() == 2;
        }));
}

void CentralRepositoryTest::reservesSerialsIdempotentlyByConfiguredScope()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    QString logUuid;
    QVERIFY(createSerialLog(central, QStringLiteral("band"), false,
                                &logUuid, &error) > 0);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString stationUuid = newUuid();
    const QString firstReservation = newUuid();
    const QJsonObject first = central.reserveSerial(
        logUuid, stationUuid, firstReservation,
        QStringLiteral("20M"), QStringLiteral("cw"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(first.value(QStringLiteral("serial")).toInt(), 7);
    QCOMPARE(first.value(QStringLiteral("formatted_serial")).toString(),
                 QStringLiteral("007"));
    QCOMPARE(first.value(QStringLiteral("scope_key")).toString(),
                 QStringLiteral("band:20m"));

    const QJsonObject retry = central.reserveSerial(
        logUuid, stationUuid, firstReservation,
        QStringLiteral("20m"), QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(retry.value(QStringLiteral("serial")).toInt(), 7);
    QVERIFY(central.reserveSerial(
        logUuid, stationUuid, firstReservation,
        QStringLiteral("40m"), QStringLiteral("CW"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("IDEMPOTENCY_KEY_REUSED")));

    const QJsonObject second = central.reserveSerial(
        logUuid, newUuid(), newUuid(), QStringLiteral("20m"),
        QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(second.value(QStringLiteral("serial")).toInt(), 8);
    const QJsonObject independentBand = central.reserveSerial(
        logUuid, newUuid(), newUuid(), QStringLiteral("40m"),
        QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(independentBand.value(QStringLiteral("serial")).toInt(), 7);

    QVERIFY2(central.cancelSerialReservation(
        logUuid, stationUuid, firstReservation, false, &error), qPrintable(error));
    QVERIFY2(central.cancelSerialReservation(
        logUuid, stationUuid, firstReservation, false, &error), qPrintable(error));
    QVERIFY(!central.cancelSerialReservation(
        logUuid, newUuid(), second.value(QStringLiteral("reservation_uuid")).toString(),
        false, &error));
    QVERIFY(error.contains(QStringLiteral("FORBIDDEN")));
    const QJsonObject third = central.reserveSerial(
        logUuid, newUuid(), newUuid(), QStringLiteral("20m"),
        QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(third.value(QStringLiteral("serial")).toInt(), 9);
    QCOMPARE(central.serialReservations(logUuid, &error).size(), 4);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void CentralRepositoryTest::correctsUnusedSerialReservationWithAudit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    QString logUuid;
    QVERIFY(createSerialLog(central, QStringLiteral("band"), false,
                            &logUuid, &error) > 0);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString administratorStationUuid = newUuid();
    const QJsonObject reservation = central.reserveSerial(
        logUuid, newUuid(), newUuid(), QStringLiteral("20m"),
        QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString reservationUuid =
        reservation.value(QStringLiteral("reservation_uuid")).toString();
    const int previous = reservation.value(QStringLiteral("serial")).toInt();

    QVERIFY2(central.adjustSerialReservation(
                 logUuid, administratorStationUuid, reservationUuid,
                 previous, previous + 2, QStringLiteral("Correction verified"),
                 &error), qPrintable(error));
    const QJsonArray reservations = central.serialReservations(logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(reservations.first().toObject()
                 .value(QStringLiteral("serial")).toInt(), previous + 2);

    const QJsonObject occupiedReservation = central.reserveSerial(
        logUuid, newUuid(), newUuid(), QStringLiteral("20m"),
        QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!central.adjustSerialReservation(
        logUuid, administratorStationUuid, reservationUuid,
        previous + 2,
        occupiedReservation.value(QStringLiteral("serial")).toInt(),
        QStringLiteral("Attempt collision"), &error));
    QVERIFY(error.startsWith(QStringLiteral("SERIAL_UNAVAILABLE")));

    QVERIFY(!central.adjustSerialReservation(
        logUuid, administratorStationUuid, reservationUuid,
        previous, previous + 3, QStringLiteral("Stale correction"), &error));
    QVERIFY(error.startsWith(QStringLiteral("SERIAL_CHANGED")));

    const QString connectionName = QStringLiteral("serial-adjustment-audit-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(central.databasePath());
        QVERIFY(database.open());
        QSqlQuery audit(database);
        QVERIFY2(audit.exec(QStringLiteral(
            "SELECT administrator_station_uuid, previous_serial, corrected_serial, "
            "reason FROM serial_reservation_adjustments")),
            qPrintable(audit.lastError().text()));
        QVERIFY(audit.next());
        QCOMPARE(audit.value(0).toString(), administratorStationUuid);
        QCOMPARE(audit.value(1).toInt(), previous);
        QCOMPARE(audit.value(2).toInt(), previous + 2);
        QCOMPARE(audit.value(3).toString(), QStringLiteral("Correction verified"));
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

void CentralRepositoryTest::requiresAndConsumesReservedSerialOnQsoCommit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    QString logUuid;
    const qint64 contestId = createSerialLog(
        central, QStringLiteral("global"), true, &logUuid, &error);
    QVERIFY2(contestId > 0, qPrintable(error));
    const QString stationUuid = newUuid();
    const QJsonObject reservation = central.reserveSerial(
        logUuid, stationUuid, newUuid(), QStringLiteral("20m"),
        QStringLiteral("CW"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString reservationUuid =
        reservation.value(QStringLiteral("reservation_uuid")).toString();

    QJsonObject operation = sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
    QJsonObject qso = operation.value(QStringLiteral("qso")).toObject();
    qso.insert(QStringLiteral("sent_exchange"), QStringLiteral("599 007"));
    qso.insert(QStringLiteral("sent_exchange_fields"), QJsonObject{
        {QStringLiteral("report"), QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("007")}
    });
    operation.insert(QStringLiteral("qso"), qso);
    QJsonArray results;
    QVERIFY(!central.submitBatch(logUuid, QJsonArray{operation}, &results, &error));
    QVERIFY(error.contains(QStringLiteral("SERIAL_UNAVAILABLE")));
    QCOMPARE(central.currentRevision(logUuid, &error), qint64(0));

    operation.insert(QStringLiteral("reservation_uuid"), reservationUuid);
    QVERIFY2(central.submitBatch(logUuid, QJsonArray{operation}, &results, &error),
                 qPrintable(error));
    QCOMPARE(results.first().toObject().value(QStringLiteral("revision")).toInteger(),
                 qint64(1));
    const QJsonArray audit = central.serialReservations(logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(audit.size(), 1);
    QCOMPARE(audit.first().toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("used"));
    QCOMPARE(audit.first().toObject().value(QStringLiteral("qso_uuid")).toString(),
                 qso.value(QStringLiteral("qso_uuid")).toString());

    QJsonObject another = sampleOperation(
        logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("K2TEST"));
    QJsonObject anotherQso = another.value(QStringLiteral("qso")).toObject();
    anotherQso.insert(QStringLiteral("sent_exchange_fields"), QJsonObject{
        {QStringLiteral("report"), QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("007")}
    });
    another.insert(QStringLiteral("qso"), anotherQso);
    QVERIFY(!central.submitBatch(logUuid, QJsonArray{another}, &results, &error));
    QVERIFY(error.contains(QStringLiteral("SERIAL_UNAVAILABLE")));
    QCOMPARE(central.currentRevision(logUuid, &error), qint64(1));
}

void CentralRepositoryTest::createsLogsAndImportsHistoricalSerialsIdempotently()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));

    const QString yaml = serialContestYaml(QStringLiteral("global"), true);
    logger::ContestDefinition definition;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(
                 yaml, QStringLiteral("serial import test"), definition, &error),
             qPrintable(error));
    logger::Contest log;
    log.logUuid = newUuid();
    log.name = QStringLiteral("Imported serial test");
    log.stationCallsign = QStringLiteral("K1TEST");
    const qint64 logId = central.createLog(definition, yaml, log, &error);
    QVERIFY2(logId > 0, qPrintable(error));
    QVERIFY(error.isEmpty());

    const QString stationUuid = newUuid();
    const QString reservationUuid = newUuid();
    const QString qsoUuid = newUuid();
    const QJsonObject history{
        {QStringLiteral("reservation_uuid"), reservationUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("scope_key"), QStringLiteral("global")},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")},
        {QStringLiteral("serial"), 7},
        {QStringLiteral("status"), QStringLiteral("used")},
        {QStringLiteral("qso_uuid"), qsoUuid}
    };
    QVERIFY2(central.importSerialReservations(log.logUuid, {history}, &error),
             qPrintable(error));
    QVERIFY2(central.importSerialReservations(log.logUuid, {history}, &error),
             qPrintable(error));

    QJsonObject operation = sampleOperation(
        log.logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
    QJsonObject operationQso = operation.value(QStringLiteral("qso")).toObject();
    operationQso.insert(QStringLiteral("sent_exchange_fields"), QJsonObject{
        {QStringLiteral("report"), QStringLiteral("599")},
        {QStringLiteral("serial"), QStringLiteral("7")}
    });
    operation.insert(QStringLiteral("qso"), operationQso);
    operation.insert(QStringLiteral("reservation_uuid"), reservationUuid);
    QJsonArray results;
    QVERIFY2(central.submitBatch(log.logUuid, {operation}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.size(), 1);
    QCOMPARE(central.currentRevision(log.logUuid, &error), qint64(1));
    QVERIFY(error.isEmpty());

    QJsonObject retry = operation;
    retry.insert(QStringLiteral("operation_uuid"), newUuid());
    retry.insert(QStringLiteral("created_at_utc"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QVERIFY2(central.submitBatch(log.logUuid, {retry}, &results, &error),
             qPrintable(error));
    QCOMPARE(central.currentRevision(log.logUuid, &error), qint64(1));
    QVERIFY(error.isEmpty());

    QJsonArray reservations = central.serialReservations(log.logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(reservations.size(), 1);
    const QJsonObject imported = reservations.first().toObject();
    QCOMPARE(imported.value(QStringLiteral("reservation_uuid")).toString(),
             reservationUuid);
    QCOMPARE(imported.value(QStringLiteral("station_uuid")).toString(), stationUuid);
    QCOMPARE(imported.value(QStringLiteral("scope_key")).toString(),
             QStringLiteral("global"));
    QCOMPARE(imported.value(QStringLiteral("serial")).toInt(), 7);
    QCOMPARE(imported.value(QStringLiteral("status")).toString(),
             QStringLiteral("used"));
    QCOMPARE(imported.value(QStringLiteral("qso_uuid")).toString(), qsoUuid);

    QJsonObject conflictingHistory = history;
    conflictingHistory.insert(QStringLiteral("serial"), 8);
    QVERIFY(!central.importSerialReservations(
        log.logUuid, {conflictingHistory}, &error));
    QVERIFY(error.contains(QStringLiteral("SERIAL_HISTORY_CONFLICT")));
    QCOMPARE(central.serialReservations(log.logUuid, &error).size(), 1);
    QVERIFY(error.isEmpty());

    QVERIFY(central.createLog(definition, yaml, log, &error) <= 0);
    QVERIFY(error.contains(QStringLiteral("already exists")));
}

void CentralRepositoryTest::validatesDuplicateKeysAndRecalculatesPointsOnServer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    logger::CentralRepository central;
    QString error;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));

    const QString yaml = QStringLiteral(R"yaml(
schema_version: 1
id: server-scoring-test
name: Server Scoring Test
bands: [20m, 40m]
modes: [CW, SSB]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
  received:
    - {name: report, type: rst}
scoring:
  qso_points: 3
  qso_point_rules:
    - {points: 7, when: {band: 20m}}
dupe_key: [callsign]
)yaml");
    logger::ContestDefinition definition;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(
                 yaml, QStringLiteral("server scoring test"), definition, &error),
             qPrintable(error));
    logger::Contest log;
    log.logUuid = newUuid();
    log.name = QStringLiteral("Server scoring test");
    log.stationCallsign = QStringLiteral("K1TEST");
    const qint64 contestId = central.createLog(definition, yaml, log, &error);
    QVERIFY2(contestId > 0, qPrintable(error));

    const QString stationUuid = newUuid();
    const QString qsoUuid = newUuid();
    QJsonObject create = sampleOperation(
        log.logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
    QJsonObject createQso = create.value(QStringLiteral("qso")).toObject();
    createQso.insert(QStringLiteral("points"), 9999);
    create.insert(QStringLiteral("qso"), createQso);
    QJsonArray results;
    QVERIFY2(central.submitBatch(log.logUuid, {create}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.first().toObject().value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("points")).toInt(), 7);
    QCOMPARE(central.currentQso(log.logUuid, qsoUuid, &error)
                 .value(QStringLiteral("points")).toInt(), 7);
    QVERIFY(error.isEmpty());

    QJsonObject duplicate = sampleOperation(
        log.logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("W1AW"));
    QJsonObject duplicateQso = duplicate.value(QStringLiteral("qso")).toObject();
    duplicateQso.insert(QStringLiteral("band"), QStringLiteral("40m"));
    duplicateQso.insert(QStringLiteral("mode"), QStringLiteral("SSB"));
    duplicate.insert(QStringLiteral("qso"), duplicateQso);
    QVERIFY(!central.submitBatch(log.logUuid, {duplicate}, &results, &error));
    QVERIFY(error.contains(QStringLiteral("DUPLICATE_QSO")));
    QCOMPARE(central.currentRevision(log.logUuid, &error), qint64(1));
    QVERIFY(error.isEmpty());

    QJsonObject update = sampleOperation(
        log.logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 1, QStringLiteral("W1AW"));
    QJsonObject updateQso = update.value(QStringLiteral("qso")).toObject();
    updateQso.insert(QStringLiteral("band"), QStringLiteral("40m"));
    updateQso.insert(QStringLiteral("mode"), QStringLiteral("SSB"));
    updateQso.insert(QStringLiteral("points"), 500);
    update.insert(QStringLiteral("qso"), updateQso);
    QVERIFY2(central.submitBatch(log.logUuid, {update}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.first().toObject().value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("points")).toInt(), 3);
    QCOMPARE(central.currentQso(log.logUuid, qsoUuid, &error)
                 .value(QStringLiteral("points")).toInt(), 3);
    QCOMPARE(central.currentRevision(log.logUuid, &error), qint64(2));
    QVERIFY(error.isEmpty());

    QJsonObject invalidate = sampleOperation(
        log.logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 2, QStringLiteral("W1AW"));
    QJsonObject invalidQso = invalidate.value(QStringLiteral("qso")).toObject();
    invalidQso.insert(QStringLiteral("is_invalid"), true);
    invalidate.insert(QStringLiteral("qso"), invalidQso);
    QVERIFY2(central.submitBatch(log.logUuid, {invalidate}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.first().toObject().value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("points")).toInt(), 0);
    QVERIFY(results.first().toObject().value(QStringLiteral("qso"))
                .toObject().value(QStringLiteral("is_invalid")).toBool());
    QCOMPARE(central.currentQso(log.logUuid, qsoUuid, &error)
                 .value(QStringLiteral("points")).toInt(), 0);
    QVERIFY(central.currentQso(log.logUuid, qsoUuid, &error)
                .value(QStringLiteral("is_invalid")).toBool());
    const QJsonArray invalidEvents = central.eventsSince(
        log.logUuid, 2, 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(invalidEvents.size(), 1);
    QVERIFY(invalidEvents.first().toObject()
                .value(QStringLiteral("payload")).toObject()
                .value(QStringLiteral("is_invalid")).toBool());

    QJsonObject restore = sampleOperation(
        log.logUuid, stationUuid, newUuid(), qsoUuid,
        QStringLiteral("qso.update"), 3, QStringLiteral("W1AW"));
    QVERIFY2(central.submitBatch(log.logUuid, {restore}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.first().toObject().value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("points")).toInt(), 7);
    QVERIFY(!results.first().toObject().value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("is_invalid")).toBool());
    QCOMPARE(central.currentRevision(log.logUuid, &error), qint64(4));

    QString ctyYaml = yaml;
    ctyYaml.replace(QStringLiteral("id: server-scoring-test"),
                    QStringLiteral("id: server-cty-test"));
    ctyYaml.replace(QStringLiteral("name: Server Scoring Test"),
                    QStringLiteral("name: Server CTY Test"));
    ctyYaml.replace(QStringLiteral("- {points: 7, when: {band: 20m}}"),
                    QStringLiteral("- {points: 7, when: {continent: EU}}"));
    logger::ContestDefinition ctyDefinition;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(
                 ctyYaml, QStringLiteral("server CTY test"),
                 ctyDefinition, &error), qPrintable(error));
    QCOMPARE(ctyDefinition.qsoPointRules.size(), 1);
    QVERIFY(ctyDefinition.qsoPointRules.first().when.contains(
        QStringLiteral("continent")));
    logger::Contest ctyLog;
    ctyLog.logUuid = newUuid();
    ctyLog.name = QStringLiteral("Server CTY test");
    ctyLog.stationCallsign = QStringLiteral("K1TEST");
    const qint64 ctyLogId = central.createLog(
        ctyDefinition, ctyYaml, ctyLog, &error);
    QVERIFY2(ctyLogId > 0,
             qPrintable(error));
    QVERIFY(central.repository()->contestDefinitionYaml(ctyLogId, &error)
                .contains(QStringLiteral("continent")));
    const QJsonObject ctyQso = sampleOperation(
        ctyLog.logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("DL1XYZ"));
    QVERIFY2(!central.submitBatch(ctyLog.logUuid, {ctyQso}, &results, &error),
             qPrintable(error));
    QVERIFY(error.contains(QStringLiteral("CONTEST_DATA_UNAVAILABLE")));
    QCOMPARE(central.currentRevision(ctyLog.logUuid, &error), qint64(0));
    QVERIFY(error.isEmpty());

    QTemporaryDir ctyDirectory;
    QVERIFY(ctyDirectory.isValid());
    QFile ctyFile(ctyDirectory.filePath(QStringLiteral("wl_cty.dat")));
    QVERIFY(ctyFile.open(QIODevice::WriteOnly));
    const QByteArray ctyData =
        "United States: 5: 8: NA: 37: 95: 5: 8: K;\n"
        "Germany: 14: 28: EU: 230: 75: 14: 28: DL;\n";
    QCOMPARE(ctyFile.write(ctyData), ctyData.size());
    ctyFile.close();
    logger::CentralRepository centralWithCty;
    QVERIFY2(centralWithCty.initialize(ctyDirectory.path(), &error),
             qPrintable(error));
    logger::Contest ctyBackedLog;
    ctyBackedLog.logUuid = newUuid();
    ctyBackedLog.name = QStringLiteral("CTY-backed scoring test");
    ctyBackedLog.stationCallsign = QStringLiteral("K1TEST");
    QVERIFY2(centralWithCty.createLog(
                 ctyDefinition, ctyYaml, ctyBackedLog, &error) > 0,
             qPrintable(error));
    QJsonObject ctyBackedOperation = sampleOperation(
        ctyBackedLog.logUuid, stationUuid, newUuid(), newUuid(),
        QStringLiteral("qso.create"), 0, QStringLiteral("DL1XYZ"));
    QJsonObject ctyBackedQso =
        ctyBackedOperation.value(QStringLiteral("qso")).toObject();
    ctyBackedQso.insert(QStringLiteral("points"), 0);
    ctyBackedOperation.insert(QStringLiteral("qso"), ctyBackedQso);
    QVERIFY2(centralWithCty.submitBatch(
                 ctyBackedLog.logUuid, {ctyBackedOperation}, &results, &error),
             qPrintable(error));
    QCOMPARE(results.first().toObject().value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("points")).toInt(), 7);
}

QTEST_GUILESS_MAIN(CentralRepositoryTest)

#include "test_central_repository.moc"
