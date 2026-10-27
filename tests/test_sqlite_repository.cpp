#include "logger/data/sqlite_repository.h"
#include "logger/contest/contest_definition_loader.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QtTest>

#include <algorithm>
#include <limits>
#include <memory>
#include <QUuid>

namespace {

class TestDatabase final {
public:
    TestDatabase()
    {
        m_connectionName = QStringLiteral("logger-tests-%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                               m_connectionName);
        m_database.setDatabaseName(QStringLiteral(":memory:"));
        if (!m_database.open()) {
            qFatal("Could not open in-memory SQLite test database: %s",
                   qPrintable(m_database.lastError().text()));
        }
        m_repository = std::make_unique<logger::SqliteRepository>(m_database);
        QString error;
        if (!m_repository->initialize(&error)
            || !m_repository->ensureGeneralLog(&error)) {
            qFatal("Could not initialize test database: %s", qPrintable(error));
        }
    }

    ~TestDatabase()
    {
        m_repository.reset();
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }

    logger::SqliteRepository &repository() { return *m_repository; }
    QSqlDatabase &database() { return m_database; }

private:
    QString m_connectionName;
    QSqlDatabase m_database;
    std::unique_ptr<logger::SqliteRepository> m_repository;
};

logger::Qso sampleQso()
{
    logger::Qso qso;
    qso.contestId = 1;
    qso.callsign = QStringLiteral("w1aw");
    qso.stationCallsign = QStringLiteral("k1test");
    qso.band = QStringLiteral("20m");
    qso.timestampUtc = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:00:00.000Z"), Qt::ISODateWithMs);
    qso.frequencyHz = 14025000;
    qso.mode = QStringLiteral("CW");
    qso.sentExchange = QStringLiteral("599 001");
    qso.receivedExchange = QStringLiteral("599 027");
    qso.points = 2;
    return qso;
}

QJsonObject sampleSyncEvent(const QString &logUuid, const QString &stationUuid,
                            const QString &qsoUuid, const QString &operationUuid,
                            qint64 revision, const QString &callsign)
{
    const QJsonObject qso{
        {QStringLiteral("qso_uuid"), qsoUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("callsign"), callsign},
        {QStringLiteral("timestamp_utc"), QStringLiteral("2026-10-15T10:00:00.000Z")},
        {QStringLiteral("frequency_hz"), 14025000},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")},
        {QStringLiteral("sent_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("received_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("sent_exchange"), QStringLiteral("599 001")},
        {QStringLiteral("received_exchange"), QStringLiteral("599 027")},
        {QStringLiteral("points"), 2},
        {QStringLiteral("qso_version"), 1},
        {QStringLiteral("is_deleted"), false},
        {QStringLiteral("is_invalid"), false}
    };
    return {
        {QStringLiteral("event_uuid"),
         QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("operation_uuid"), operationUuid},
        {QStringLiteral("qso_uuid"), qsoUuid},
        {QStringLiteral("qso_version"), 1},
        {QStringLiteral("kind"), QStringLiteral("qso.upsert")},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("committed_at_utc"), QStringLiteral("2026-10-15T10:00:01.000Z")},
        {QStringLiteral("payload"), qso}
    };
}

QJsonArray sampleQtcTransfers(const QString &qsoUuid, const QString &partnerCallsign)
{
    const QString qtcUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    return {QJsonObject{
        {QStringLiteral("qtc_uuid"), qtcUuid},
        {QStringLiteral("partner_qso_uuid"), qsoUuid},
        {QStringLiteral("partner_callsign"), partnerCallsign},
        {QStringLiteral("direction"), QStringLiteral("received")},
        {QStringLiteral("group_number"), 1},
        {QStringLiteral("messages"), QJsonArray{QJsonObject{
            {QStringLiteral("timestamp_utc"),
             QStringLiteral("2026-10-15T09:40:00.000Z")},
            {QStringLiteral("callsign"), QStringLiteral("DL2XYZ")},
            {QStringLiteral("serial_number"), 18}
        }}}
    }};
}

} // namespace

class SqliteRepositoryTest final : public QObject {
    Q_OBJECT

private slots:
    void savesAndReadsQso();
    void standaloneFilesAreIndependentAndPortable();
    void standaloneFilesRejectOverwriteInvalidFilesAndConcurrentOpen();
    void commitsTransaction();
    void rollsBackFailedTransaction();
    void appliesSchemaMigrationOnce();
    void persistsSerialReservationsAndStatusTransitions();
    void consumesSerialReservationWithQsoAtomically();
    void persistsQueuedSerialReservationRequests();
    void recoversQueuedSerialReservationRequestsAfterDatabaseReopen();
    void migratesExistingVersionOneDatabase();
    void savesContestDefinition();
    void createsAndUpdatesLogProperties();
    void updatesQsoAndPersistsExchangeFields();
    void resolvesSyncConflictByAcceptingServer();
    void resolvesSyncConflictByKeepingLocalAsNewUpdate();
    void resolvesSyncConflictByMergingSelectedFields();
    void loadsAllQsosForDupeChecking();
    void savesAndReadsSpot();
    void savesSpotWithoutMode();
    void prunesRetainedSpots();
    void findsDuplicateQsos();
    void assignsStableSyncIdentities();
    void persistsAtomicSyncOutbox();
    void persistsQtcWithQsoAndQueuesItForSynchronization();
    void upgradesLegacyOutboxInvalidFlag();
    void deletesQsoWithAtomicOutboxTombstone();
    void auditsAndUndoesQsoChangesAsVersionedOperations();
    void undoRollsBackWhenOutboxWriteFails();
    void restoresPendingOutboxAfterDatabaseReopen();
    void atomicallyAppliesSyncChangesAndRecoversInflightOperations();
    void atomicallyReplacesDataFromVerifiedSnapshot();
    void rollsBackQsoWhenOutboxInsertFails();
    void migratesLegacyExchangesAndPreservesOnUpdate();
    void backsUpAndRestoresVerifiedDatabase();
};

void SqliteRepositoryTest::standaloneFilesAreIndependentAndPortable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("First log.sqlite"));
    const QString second = directory.filePath(QStringLiteral("Second log.sqlite"));
    const QString yaml = QStringLiteral(
        "schema_version: 1\nid: portable\nname: Portable\nbands: [20m]\n"
        "modes: [CW]\nexchange:\n  sent:\n    - {name: report, type: rst}\n"
        "  received:\n    - {name: report, type: rst}\nscoring: {qso_points: 1}\n");
    logger::ContestDefinition definition;
    QString error;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(yaml, {}, definition, &error),
             qPrintable(error));
    const QVariantMap settings{
        {QStringLiteral("station/callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("cw/functionKeys/F1/text"), QStringLiteral("CQ {MYCALL}")},
        {QStringLiteral("operating/esmEnabled"), true}};
    const QMap<QString, QByteArray> assets{
        {QStringLiteral("support/master/test"), QByteArray("W1AW\nSP9ABC\n")},
        {QStringLiteral("support/cty"), QByteArray("Poland: 15: 28: EU: 52: -18: -1: SP:\n SP;\n")},
        {QStringLiteral("definitions/portable"), yaml.toUtf8()}};
    {
        logger::SqliteRepository repository{QSqlDatabase()};
        logger::Contest log;
        log.name = QStringLiteral("Ignored in favor of filename");
        log.stationCallsign = QStringLiteral("K1TEST");
        QVERIFY2(repository.createLogFile(first, definition, yaml, log, settings, assets,
                                           {sampleQso()}, &error), qPrintable(error));
        QVERIFY(repository.isStandaloneLog());
        QCOMPARE(repository.contests().size(), 1);
        QCOMPARE(repository.contests().first().name, QStringLiteral("First log"));
        QCOMPARE(repository.contestDefinitionYaml(1), yaml);
        QCOMPARE(repository.logSettings(), settings);
        QCOMPARE(repository.logAssets(), assets);
        QCOMPARE(repository.qsosForContest(1).size(), 1);
        QCOMPARE(repository.createContestLog(definition, yaml, log, &error), qint64(0));
        QVERIFY(!error.isEmpty());
        QVERIFY2(repository.createLogFile(second, definition, yaml, log, {}, {}, {}, &error),
                 qPrintable(error));
        QVERIFY(repository.qsosForContest(1).isEmpty());
        QVERIFY(repository.logAssets().isEmpty());
        QVERIFY(repository.logSettings().isEmpty());
        QVERIFY2(repository.openLogFile(first, &error), qPrintable(error));
        QCOMPARE(repository.qsosForContest(1).size(), 1);
        QVERIFY2(repository.setLogSetting(QStringLiteral("station/operatorCallsign"),
                                           QStringLiteral("SP9OP"), &error), qPrintable(error));
    }
    const QString moved = directory.filePath(QStringLiteral("Copied log.sqlite"));
    QVERIFY(QFile::copy(first, moved));
    QVERIFY(QFile::remove(first));
    logger::SqliteRepository reopened{QSqlDatabase()};
    QVERIFY2(reopened.openLogFile(moved, &error), qPrintable(error));
    QCOMPARE(reopened.qsosForContest(1).first().callsign, QStringLiteral("W1AW"));
    QCOMPARE(reopened.logAssets(), assets);
    QCOMPARE(reopened.logSettings().value(QStringLiteral("station/operatorCallsign")).toString(),
             QStringLiteral("SP9OP"));
}

void SqliteRepositoryTest::standaloneFilesRejectOverwriteInvalidFilesAndConcurrentOpen()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("Protected.sqlite"));
    const QString yaml = QStringLiteral(
        "schema_version: 1\nid: portable\nname: Portable\nbands: [20m]\n"
        "modes: [CW]\nexchange:\n  sent:\n    - {name: report, type: rst}\n"
        "  received:\n    - {name: report, type: rst}\nscoring: {qso_points: 1}\n");
    logger::ContestDefinition definition;
    QString error;
    QVERIFY(logger::ContestDefinitionLoader().loadText(yaml, {}, definition, &error));
    logger::Contest log;
    log.name = QStringLiteral("Protected");
    logger::SqliteRepository repository{QSqlDatabase()};
    QVERIFY2(repository.createLogFile(path, definition, yaml, log, {}, {}, {}, &error),
             qPrintable(error));
    QVERIFY(!repository.createLogFile(path, definition, yaml, log, {}, {}, {}, &error));
    QVERIFY(!error.isEmpty());
    logger::SqliteRepository other{QSqlDatabase()};
    QVERIFY(!other.openLogFile(path, &error));
    QVERIFY(error.contains(QStringLiteral("locked")));
    const QString invalidPath = directory.filePath(QStringLiteral("Not a log.sqlite"));
    {
        QFile invalid(invalidPath);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("not a database");
    }
    QVERIFY(!repository.openLogFile(invalidPath, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(repository.databasePath(), path);
    QFile invalid(invalidPath);
    QVERIFY(invalid.open(QIODevice::ReadOnly));
    QCOMPARE(invalid.readAll(), QByteArray("not a database"));
    QVERIFY(!repository.openLogFile(directory.filePath(QStringLiteral("missing.sqlite")), &error));
    QCOMPARE(repository.contests().size(), 1);
    logger::Qso bad = sampleQso();
    bad.frequencyHz = -1;
    const QString failed = directory.filePath(QStringLiteral("Failed.sqlite"));
    QVERIFY(!other.createLogFile(failed, definition, yaml, log, {}, {}, {bad}, &error));
    QVERIFY(!QFileInfo::exists(failed));
}

void SqliteRepositoryTest::savesAndReadsQso()
{
    TestDatabase database;
    auto &repository = database.repository();
    const auto contests = repository.contests();
    QCOMPARE(contests.size(), 1);
    QCOMPARE(contests.first().code, QStringLiteral("GENERAL"));

    logger::Qso qso = sampleQso();
    QString error;
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QVERIFY(qso.id > 0);

    const auto loaded = repository.qsoById(qso.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->callsign, QStringLiteral("W1AW"));
    QCOMPARE(loaded->stationCallsign, QStringLiteral("K1TEST"));
    QCOMPARE(loaded->band, QStringLiteral("20m"));
    QCOMPARE(loaded->contestId, qso.contestId);
    QCOMPARE(loaded->timestampUtc, qso.timestampUtc.toUTC());
    QCOMPARE(loaded->frequencyHz, qso.frequencyHz);
    QCOMPARE(loaded->mode, qso.mode);
    QCOMPARE(loaded->sentExchange, qso.sentExchange);
    QCOMPARE(loaded->receivedExchange, qso.receivedExchange);
    QCOMPARE(loaded->points, qso.points);
}

void SqliteRepositoryTest::commitsTransaction()
{
    TestDatabase database;
    auto &repository = database.repository();
    logger::Qso qso = sampleQso();
    QString error;

    QVERIFY2(repository.withTransaction(
                 [&repository, &qso](QString *operationError) {
                     return repository.saveQso(qso, operationError);
                 },
                 &error),
             qPrintable(error));
    QVERIFY(repository.qsoById(qso.id).has_value());
}

void SqliteRepositoryTest::rollsBackFailedTransaction()
{
    TestDatabase database;
    auto &repository = database.repository();
    logger::Qso qso = sampleQso();
    QString error;

    QVERIFY(!repository.withTransaction(
        [&repository, &qso](QString *operationError) {
            if (!repository.saveQso(qso, operationError)) {
                return false;
            }
            *operationError = QStringLiteral("Simulated failure");
            return false;
        },
        &error));
    QCOMPARE(error, QStringLiteral("Simulated failure"));
    QVERIFY(!repository.qsoById(qso.id).has_value());
}

void SqliteRepositoryTest::appliesSchemaMigrationOnce()
{
    TestDatabase database;
    QString error;
    QVERIFY2(database.repository().initialize(&error), qPrintable(error));

    QSqlQuery query(database.database());
    QVERIFY(query.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), logger::SqliteRepository::currentSchemaVersion());
    query.finish();

    QVERIFY2(database.repository().initialize(&error), qPrintable(error));
    const auto contests = database.repository().contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(contests.size(), 1);
}

void SqliteRepositoryTest::persistsSerialReservationsAndStatusTransitions()
{
    TestDatabase database;
    QString error;
    QVERIFY2(database.repository().initialize(&error), qPrintable(error));
    const QVector<logger::Contest> contests =
        database.repository().contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(contests.size(), 1);
    const QString stationUuid = database.repository().localStationUuid(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    logger::SerialReservation reservation{
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
        contests.first().logUuid,
        stationUuid,
        QStringLiteral("global"),
        QStringLiteral("20m"),
        QStringLiteral("CW"),
        12,
        QStringLiteral("reserved"),
        {}
    };
    QVERIFY2(database.repository().saveSerialReservation(reservation, &error),
             qPrintable(error));
    QVERIFY2(database.repository().saveSerialReservation(reservation, &error),
             qPrintable(error));
    const std::optional<logger::SerialReservation> stored =
        database.repository().serialReservation(
            reservation.logUuid, reservation.reservationUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(stored.has_value());
    QCOMPARE(stored->serialNumber, 12);
    QCOMPARE(stored->status, QStringLiteral("reserved"));

    logger::SerialReservation conflicting = reservation;
    conflicting.serialNumber = 13;
    QVERIFY(!database.repository().saveSerialReservation(conflicting, &error));
    QVERIFY(error.contains(QStringLiteral("conflicts")));

    QVERIFY2(database.repository().setSerialReservationStatus(
                 reservation.logUuid, reservation.reservationUuid,
                 QStringLiteral("cancelled"), {}, &error), qPrintable(error));
    QVERIFY2(database.repository().setSerialReservationStatus(
                 reservation.logUuid, reservation.reservationUuid,
                 QStringLiteral("cancelled"), {}, &error), qPrintable(error));
    QVERIFY(!database.repository().setSerialReservationStatus(
        reservation.logUuid, reservation.reservationUuid,
        QStringLiteral("used"),
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(), &error));
    QVERIFY(error.contains(QStringLiteral("finalized")));

    const QVector<logger::SerialReservation> cached =
        database.repository().serialReservations(reservation.logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(cached.size(), 1);
    QCOMPARE(cached.first().status, QStringLiteral("cancelled"));
}

void SqliteRepositoryTest::consumesSerialReservationWithQsoAtomically()
{
    TestDatabase database;
    QString error;
    const logger::Contest contest = database.repository().contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString stationUuid = database.repository().localStationUuid(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const logger::SerialReservation reservation{
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
        contest.logUuid,
        stationUuid,
        QStringLiteral("global"),
        QStringLiteral("20m"),
        QStringLiteral("CW"),
        12,
        QStringLiteral("reserved"),
        {}
    };
    QVERIFY2(database.repository().saveSerialReservation(reservation, &error),
             qPrintable(error));

    logger::Qso invalidQso = sampleQso();
    invalidQso.reservationUuid = reservation.reservationUuid;
    invalidQso.band = QStringLiteral("40m");
    QVERIFY(!database.repository().saveQso(invalidQso, &error));
    const auto stillReserved = database.repository().serialReservation(
        contest.logUuid, reservation.reservationUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(stillReserved.has_value());
    QCOMPARE(stillReserved->status, QStringLiteral("reserved"));
    QCOMPARE(database.repository().qsosForContest(contest.id, 0, &error).size(), 0);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    logger::Qso qso = sampleQso();
    qso.reservationUuid = reservation.reservationUuid;
    QVERIFY2(database.repository().saveQso(qso, &error), qPrintable(error));
    const auto used = database.repository().serialReservation(
        contest.logUuid, reservation.reservationUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(used.has_value());
    QCOMPARE(used->status, QStringLiteral("used"));
    QCOMPARE(used->qsoUuid, qso.qsoUuid);

    logger::Qso duplicateUse = sampleQso();
    duplicateUse.callsign = QStringLiteral("K2AW");
    duplicateUse.reservationUuid = reservation.reservationUuid;
    QVERIFY(!database.repository().saveQso(duplicateUse, &error));
    QVERIFY(error.contains(QStringLiteral("reservation")));
    QVERIFY2(database.repository().qsosForContest(contest.id, 0, &error).size() == 1,
             qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void SqliteRepositoryTest::persistsQueuedSerialReservationRequests()
{
    TestDatabase database;
    QString error;
    const logger::Contest contest = database.repository().contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString stationUuid = database.repository().localStationUuid(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(database.repository().setSynchronizationEnabled(
                 contest.logUuid, true, &error), qPrintable(error));

    const logger::SerialReservationRequest reserve{
        contest.logUuid, stationUuid,
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
        QStringLiteral("reserve_serial"), QStringLiteral("20m"),
        QStringLiteral("CW")
    };
    QVERIFY2(database.repository().enqueueSerialReservationRequest(
                 reserve, &error), qPrintable(error));
    QVERIFY2(database.repository().enqueueSerialReservationRequest(
                 reserve, &error), qPrintable(error));

    logger::SerialReservationRequest conflicting = reserve;
    conflicting.mode = QStringLiteral("SSB");
    QVERIFY(!database.repository().enqueueSerialReservationRequest(
        conflicting, &error));
    QVERIFY(error.contains(QStringLiteral("reused")));

    const QVector<logger::SerialReservationRequest> queued =
        database.repository().pendingSerialReservationRequests(
            contest.logUuid, stationUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(queued.size(), 1);
    QCOMPARE(queued.first().reservationUuid, reserve.reservationUuid);

    QVERIFY2(database.repository().removeSerialReservationRequest(
                 contest.logUuid, reserve.reservationUuid,
                 QStringLiteral("reserve_serial"), &error), qPrintable(error));
    QVERIFY2(database.repository().removeSerialReservationRequest(
                 contest.logUuid, reserve.reservationUuid,
                 QStringLiteral("reserve_serial"), &error), qPrintable(error));
    QVERIFY(database.repository().pendingSerialReservationRequests(
        contest.logUuid, stationUuid, &error).isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void SqliteRepositoryTest::recoversQueuedSerialReservationRequestsAfterDatabaseReopen()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("serial-queue.sqlite"));
    const QString firstConnection = QStringLiteral("serial-queue-first-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString logUuid;
    QString stationUuid;
    const QString reservationUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), firstConnection);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        {
            logger::SqliteRepository repository(database);
            QString error;
            QVERIFY2(repository.initialize(&error), qPrintable(error));
            QVERIFY2(repository.ensureGeneralLog(&error), qPrintable(error));
            const logger::Contest contest = repository.contests(&error).first();
            QVERIFY2(error.isEmpty(), qPrintable(error));
            logUuid = contest.logUuid;
            stationUuid = repository.localStationUuid(&error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
                     qPrintable(error));
            const logger::SerialReservationRequest request{
                logUuid, stationUuid, reservationUuid,
                QStringLiteral("reserve_serial"), QStringLiteral("20m"),
                QStringLiteral("CW")
            };
            QVERIFY2(repository.enqueueSerialReservationRequest(request, &error),
                     qPrintable(error));
        }
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(firstConnection);

    const QString reopenedConnection = QStringLiteral("serial-queue-reopened-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), reopenedConnection);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        {
            logger::SqliteRepository repository(database);
            QString error;
            QVERIFY2(repository.initialize(&error), qPrintable(error));
            const auto requests = repository.pendingSerialReservationRequests(
                logUuid, stationUuid, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(requests.size(), 1);
            QCOMPARE(requests.first().reservationUuid, reservationUuid);
            QCOMPARE(requests.first().band, QStringLiteral("20m"));
            QCOMPARE(requests.first().mode, QStringLiteral("CW"));
        }
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(reopenedConnection);
}

void SqliteRepositoryTest::migratesExistingVersionOneDatabase()
{
    const QString connectionName = QStringLiteral("logger-v1-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());

    {
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, "
            "applied_at_utc TEXT NOT NULL)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO schema_migrations VALUES(1, '2026-01-01T00:00:00Z')")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE contests (id INTEGER PRIMARY KEY, code TEXT NOT NULL, "
            "name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', "
            "starts_at_utc TEXT, ends_at_utc TEXT)")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE spots (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "callsign TEXT NOT NULL, frequency_hz INTEGER NOT NULL, "
            "mode TEXT NOT NULL DEFAULT '', spotter TEXT NOT NULL DEFAULT '', "
            "comment TEXT NOT NULL DEFAULT '', timestamp_utc TEXT NOT NULL, "
            "source TEXT NOT NULL DEFAULT '')")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE qsos (id INTEGER PRIMARY KEY, contest_id INTEGER NOT NULL, "
            "callsign TEXT NOT NULL, station_callsign TEXT NOT NULL, "
            "timestamp_utc TEXT NOT NULL, frequency_hz INTEGER NOT NULL DEFAULT 0, "
            "mode TEXT NOT NULL DEFAULT '', sent_exchange TEXT NOT NULL DEFAULT '', "
            "received_exchange TEXT NOT NULL DEFAULT '', points INTEGER NOT NULL DEFAULT 0)")));
        query.finish();

        logger::SqliteRepository repository(database);
        QString error;
        QVERIFY2(repository.initialize(&error), qPrintable(error));
        QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(qsos)")));
        bool foundBand = false;
        while (query.next()) {
            foundBand = foundBand || query.value(QStringLiteral("name")).toString()
                                         == QStringLiteral("band");
        }
        QVERIFY(foundBand);
        query.finish();
        QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(qsos)")));
        bool foundReceivedJson = false;
        while (query.next()) {
            foundReceivedJson =
                foundReceivedJson
                || query.value(QStringLiteral("name")).toString()
                       == QStringLiteral("received_exchange_json");
        }
        QVERIFY(foundReceivedJson);
        query.finish();
        QVERIFY2(repository.initialize(&error), qPrintable(error));
    }

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SqliteRepositoryTest::updatesQsoAndPersistsExchangeFields()
{
    TestDatabase database;
    auto &repository = database.repository();
    logger::Qso qso = sampleQso();
    qso.sentExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("001"));
    qso.receivedExchangeFields.insert(QStringLiteral("rst"), QStringLiteral("599"));
    qso.receivedExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("027"));
    QString error;
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));

    qso.callsign = QStringLiteral("N0CALL");
    qso.receivedExchangeFields[QStringLiteral("serial")] = QStringLiteral("028");
    qso.receivedExchange = QStringLiteral("rst=599 serial=028");
    QVERIFY2(repository.updateQso(qso, &error), qPrintable(error));

    const auto loaded = repository.qsoById(qso.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->callsign, QStringLiteral("N0CALL"));
    QCOMPARE(loaded->sentExchangeFields.value(QStringLiteral("serial")),
             QStringLiteral("001"));
    QCOMPARE(loaded->receivedExchangeFields.value(QStringLiteral("serial")),
             QStringLiteral("028"));
}

void SqliteRepositoryTest::loadsAllQsosForDupeChecking()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    for (int index = 0; index < 5; ++index) {
        logger::Qso qso = sampleQso();
        qso.callsign = QStringLiteral("K%1TEST").arg(index);
        qso.timestampUtc = qso.timestampUtc.addSecs(index);
        QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    }
    QCOMPARE(repository.recentQsos(1, 2, &error).size(), 2);
    QCOMPARE(repository.qsosForContest(1, 0, &error).size(), 5);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void SqliteRepositoryTest::savesAndReadsSpot()
{
    TestDatabase database;
    logger::Spot spot;
    spot.callsign = QStringLiteral("w1aw");
    spot.frequencyHz = 14'025'000;
    spot.band = QStringLiteral("20m");
    spot.mode = QStringLiteral("CW");
    spot.spotter = QStringLiteral("k1abc");
    spot.comment = QStringLiteral("CQ TEST");
    spot.timestampUtc = QDateTime::currentDateTimeUtc();
    spot.source = QStringLiteral("DX cluster");
    QString error;
    QVERIFY2(database.repository().saveSpot(spot, &error), qPrintable(error));
    QVERIFY(spot.id > 0);
    const QVector<logger::Spot> spots = database.repository().recentSpots(10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(spots.size(), 1);
    QCOMPARE(spots.first().callsign, QStringLiteral("W1AW"));
    QCOMPARE(spots.first().frequencyHz, qint64(14'025'000));
    QCOMPARE(spots.first().band, QStringLiteral("20m"));
    QCOMPARE(spots.first().mode, QStringLiteral("CW"));
    QCOMPARE(spots.first().spotter, QStringLiteral("K1ABC"));
}

void SqliteRepositoryTest::savesSpotWithoutMode()
{
    TestDatabase database;
    logger::Spot spot;
    spot.callsign = QStringLiteral("W1AW");
    spot.frequencyHz = 14'025'000;
    spot.band = QStringLiteral("20m");
    spot.spotter = QStringLiteral("K1ABC");
    spot.comment = QStringLiteral("");
    spot.timestampUtc = QDateTime::currentDateTimeUtc();
    spot.source = QStringLiteral("DX cluster");
    QString error;

    QVERIFY2(database.repository().saveSpot(spot, &error), qPrintable(error));
    const QVector<logger::Spot> spots = database.repository().recentSpots(10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(spots.size(), 1);
    QVERIFY(spots.first().mode.isEmpty());
}

void SqliteRepositoryTest::savesContestDefinition()
{
    TestDatabase database;
    logger::ContestDefinition definition;
    definition.id = QStringLiteral("test-definition");
    definition.name = QStringLiteral("Test Definition");
    definition.description = QStringLiteral("Saved YAML definition");
    const QString yaml = QStringLiteral("id: test-definition\n");
    QString error;

    QVERIFY2(database.repository().saveContestDefinition(definition, yaml, &error),
             qPrintable(error));
    const auto contests = database.repository().contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto found = std::find_if(
        contests.cbegin(), contests.cend(),
        [&definition](const logger::Contest &contest) {
            return contest.code == QStringLiteral("TEST-DEFINITION");
        });
    QVERIFY(found != contests.cend());
    QCOMPARE(found->name, definition.name);
    QCOMPARE(database.repository().contestDefinitionYaml(found->id, &error), yaml);
}

void SqliteRepositoryTest::createsAndUpdatesLogProperties()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    QVERIFY2(repository.initialize(&error), qPrintable(error));
    logger::ContestDefinition definition;
    definition.id = QStringLiteral("props");
    definition.name = QStringLiteral("Props Contest");
    logger::Contest log;
    log.name = QStringLiteral("SP9ABC Props 2026");
    log.stationCallsign = QStringLiteral("sp9abc");
    log.startsAtUtc = QDateTime::fromString(QStringLiteral("2026-11-28T00:00:00Z"),
                                            Qt::ISODate);
    log.properties = {{QStringLiteral("CATEGORY-POWER"), QStringLiteral("LOW")}};

    const qint64 firstId = repository.createContestLog(
        definition, QStringLiteral("id: props\n"), log, &error);
    QVERIFY2(firstId > 0, qPrintable(error));
    const qint64 secondId = repository.createContestLog(
        definition, QStringLiteral("id: props\n"), log, &error);
    QVERIFY2(secondId > 0, qPrintable(error));

    const auto find = [&repository, &error](qint64 id) {
        const auto contests = repository.contests(&error);
        const auto found = std::find_if(
            contests.cbegin(), contests.cend(),
            [id](const logger::Contest &contest) { return contest.id == id; });
        return found == contests.cend() ? logger::Contest{} : *found;
    };
    logger::Contest stored = find(firstId);
    QCOMPARE(stored.code, QStringLiteral("PROPS"));
    QCOMPARE(find(secondId).code, QStringLiteral("PROPS-2"));
    QCOMPARE(stored.stationCallsign, QStringLiteral("SP9ABC"));
    QVERIFY(stored.startsAtUtc.has_value());
    QCOMPARE(*stored.startsAtUtc, *log.startsAtUtc);
    QCOMPARE(stored.properties.value(QStringLiteral("CATEGORY-POWER")),
             QStringLiteral("LOW"));

    stored.name = QStringLiteral("Renamed");
    stored.stationCallsign = QStringLiteral("SN9X");
    stored.properties = {{QStringLiteral("OPERATORS"), QStringLiteral("SP9ABC SP9XYZ")}};
    QVERIFY2(repository.updateContestLog(stored, &error), qPrintable(error));
    const logger::Contest updated = find(firstId);
    QCOMPARE(updated.name, QStringLiteral("Renamed"));
    QCOMPARE(updated.stationCallsign, QStringLiteral("SN9X"));
    QVERIFY(!updated.properties.contains(QStringLiteral("CATEGORY-POWER")));
    QCOMPARE(updated.properties.value(QStringLiteral("OPERATORS")),
             QStringLiteral("SP9ABC SP9XYZ"));

    logger::Contest missing = updated;
    missing.id = 999999;
    QVERIFY(!repository.updateContestLog(missing, &error));
}

void SqliteRepositoryTest::prunesRetainedSpots()
{
    TestDatabase database;
    auto &repository = database.repository();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QString error;
    for (int index = 0; index < 6000; ++index) {
        logger::Spot spot;
        spot.callsign = QStringLiteral("W1AW");
        spot.frequencyHz = 14'025'000;
        spot.band = QStringLiteral("20m");
        spot.mode = QStringLiteral("CW");
        spot.spotter = QStringLiteral("K1TEST");
        spot.comment = QStringLiteral("");
        spot.source = QStringLiteral("test");
        spot.timestampUtc = now.addSecs(index - 6000);
        QVERIFY2(repository.saveSpot(spot, &error), qPrintable(error));
    }
    const QVector<logger::Spot> retained = repository.recentSpots(6000, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(retained.size() <= 5000);
    QVERIFY(retained.size() >= 4800);

    QVERIFY2(repository.pruneSpots(now.addSecs(-100), 25, &error), qPrintable(error));
    QCOMPARE(repository.recentSpots(100, &error).size(), 25);
}

void SqliteRepositoryTest::findsDuplicateQsos()
{
    TestDatabase database;
    auto &repository = database.repository();
    logger::Qso first = sampleQso();
    first.callsign = QStringLiteral("W1AW");
    first.band = QStringLiteral("20m");
    first.mode = QStringLiteral("CW");
    QString error;
    QVERIFY2(repository.saveQso(first, &error), qPrintable(error));

    const auto found = repository.findDuplicate(
        first.contestId, QStringLiteral("w1aw"), QStringLiteral("20M"),
        QStringLiteral("cw"), 0, &error);
    QVERIFY2(found.has_value(), qPrintable(error));
    QCOMPARE(*found, first.id);
    QVERIFY(!repository.findDuplicate(first.contestId, first.callsign, first.band,
                                      first.mode, first.id, &error).has_value());
    QVERIFY(repository.findDuplicate(first.contestId, first.callsign,
                                     QStringLiteral("40m"), first.mode,
                                     0, &error) == std::nullopt);
}

void SqliteRepositoryTest::assignsStableSyncIdentities()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString stationUuid = repository.localStationUuid(&error);
    QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));
    const auto logs = repository.contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logs.size(), 1);
    QVERIFY(!logs.first().logUuid.isEmpty());

    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QVERIFY(!qso.qsoUuid.isEmpty());
    QCOMPARE(qso.stationUuid, stationUuid);
    QCOMPARE(qso.qsoVersion, 1);

    auto loaded = repository.qsoById(qso.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->qsoUuid, qso.qsoUuid);
    QCOMPARE(loaded->logUuid, logs.first().logUuid);
    QCOMPARE(loaded->stationUuid, stationUuid);
    QCOMPARE(loaded->qsoVersion, 1);

    logger::Qso anotherQso = sampleQso();
    anotherQso.callsign = QStringLiteral("K2TEST");
    QVERIFY2(repository.saveQso(anotherQso, &error), qPrintable(error));
    QVERIFY(anotherQso.qsoUuid != qso.qsoUuid);
    QSqlQuery duplicateUuid(database.database());
    duplicateUuid.prepare(QStringLiteral(
        "UPDATE qsos SET qso_uuid = ? WHERE id = ?"));
    duplicateUuid.addBindValue(qso.qsoUuid);
    duplicateUuid.addBindValue(anotherQso.id);
    QVERIFY(!duplicateUuid.exec());

    loaded->callsign = QStringLiteral("N0CALL");
    QVERIFY2(repository.updateQso(*loaded, &error), qPrintable(error));
    loaded = repository.qsoById(qso.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->qsoUuid, qso.qsoUuid);
    QCOMPARE(loaded->qsoVersion, 2);

    QVERIFY2(repository.initialize(&error), qPrintable(error));
    loaded = repository.qsoById(qso.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->qsoUuid, qso.qsoUuid);
    QCOMPARE(loaded->stationUuid, stationUuid);
    QCOMPARE(loaded->qsoVersion, 2);
    QCOMPARE(repository.localStationUuid(&error), stationUuid);
}

void SqliteRepositoryTest::persistsAtomicSyncOutbox()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString logUuid = repository.contests(&error).first().logUuid;
    QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
             qPrintable(error));
    QVERIFY(repository.synchronizationEnabled(logUuid, &error));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(0));
    QVERIFY2(repository.setSyncCursor(logUuid, 12, &error), qPrintable(error));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(12));
    QVERIFY(!repository.setSyncCursor(logUuid, 11, &error));
    QVERIFY(!error.isEmpty());
    error.clear();

    logger::Qso qso = sampleQso();
    qso.invalid = true;
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    auto operations = repository.syncOutbox(logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    QCOMPARE(operations.first().operation, QStringLiteral("qso.create"));
    QCOMPARE(operations.first().status, QStringLiteral("pending"));
    const QString operationUuid = operations.first().operationUuid;
    const QJsonObject createPayload =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QCOMPARE(createPayload.value(QStringLiteral("kind")).toString(),
             QStringLiteral("qso.create"));
    QCOMPARE(createPayload.value(QStringLiteral("expected_version")).toInt(), 0);
    QCOMPARE(createPayload.value(QStringLiteral("qso")).toObject()
                 .value(QStringLiteral("qso_uuid")).toString(), qso.qsoUuid);
    QVERIFY(createPayload.value(QStringLiteral("qso")).toObject()
                .value(QStringLiteral("is_invalid")).toBool());
    QCOMPARE(createPayload.value(QStringLiteral("log_uuid")).toString(), logUuid);

    auto stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->syncStatus, logger::QsoSyncStatus::Pending);
    QVERIFY(stored->invalid);
    QVERIFY2(repository.setSyncOperationState(
                 operationUuid, QStringLiteral("in-flight"), {}, &error),
             qPrintable(error));
    operations = repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(operations.first().attempts, 1);
    QVERIFY2(repository.setSyncOperationState(
                 operationUuid, QStringLiteral("failed"),
                 QStringLiteral("connection lost"), &error),
             qPrintable(error));
    operations = repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(operations.first().lastError, QStringLiteral("connection lost"));
    QVERIFY2(repository.retrySyncOperation(operationUuid, &error), qPrintable(error));
    operations = repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(operations.first().status, QStringLiteral("pending"));
    QVERIFY2(repository.setSyncOperationState(
                 operationUuid, QStringLiteral("in-flight"), {}, &error),
             qPrintable(error));
    QVERIFY2(repository.setSyncOperationState(
                 operationUuid, QStringLiteral("accepted"), {}, &error),
             qPrintable(error));
    stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->syncStatus, logger::QsoSyncStatus::Synced);
    QCOMPARE(repository.syncOutbox(logUuid, false, &error).size(), 0);
    QCOMPARE(repository.syncOutbox(logUuid, true, &error).size(), 1);

    stored->callsign = QStringLiteral("N0CALL");
    QVERIFY2(repository.updateQso(*stored, &error), qPrintable(error));
    logger::Qso staleEdit = *stored;
    staleEdit.callsign = QStringLiteral("K3STALE");
    QVERIFY(!repository.updateQso(staleEdit, &error));
    QVERIFY(error.contains(QStringLiteral("reload it")));
    error.clear();
    operations = repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(operations.size(), 1);
    QCOMPARE(operations.first().operation, QStringLiteral("qso.update"));
    const QJsonObject updatePayload =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QCOMPARE(updatePayload.value(QStringLiteral("expected_version")).toInt(), 1);
    QCOMPARE(updatePayload.value(QStringLiteral("qso")).toObject()
                 .value(QStringLiteral("qso_uuid")).toString(), qso.qsoUuid);
    QJsonObject remoteVersion =
        updatePayload.value(QStringLiteral("qso")).toObject();
    remoteVersion.insert(QStringLiteral("callsign"), QStringLiteral("W1AW"));
    remoteVersion.insert(QStringLiteral("qso_version"), 2);
    remoteVersion.insert(QStringLiteral("is_deleted"), false);
    remoteVersion.insert(QStringLiteral("is_invalid"), false);
    QVERIFY2(repository.recordSyncConflict(
                 operations.first().operationUuid,
                 QString::fromUtf8(QJsonDocument(remoteVersion)
                                       .toJson(QJsonDocument::Compact)),
                 &error),
             qPrintable(error));
    const QVector<logger::SyncConflict> conflicts =
        repository.syncConflicts(logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(conflicts.size(), 1);
    QCOMPARE(conflicts.first().operationUuid, operations.first().operationUuid);
    QCOMPARE(conflicts.first().entityUuid, qso.qsoUuid);
    QVERIFY(conflicts.first().localPayloadJson.contains(QStringLiteral("qso")));
    QCOMPARE(QJsonDocument::fromJson(conflicts.first().remotePayloadJson.toUtf8())
                 .object().value(QStringLiteral("callsign")).toString(),
             QStringLiteral("W1AW"));
    stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->syncStatus, logger::QsoSyncStatus::Conflict);
    QVERIFY(!repository.retrySyncOperation(
        operations.first().operationUuid, &error));
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY2(repository.resolveSyncConflictKeepLocal(
                 operations.first().operationUuid, &error), qPrintable(error));
    QVERIFY2(repository.syncConflicts(logUuid, &error).isEmpty(), qPrintable(error));
    stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->callsign, QStringLiteral("N0CALL"));
    QCOMPARE(stored->qsoVersion, 3);
    operations = repository.syncOutbox(logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    QCOMPARE(operations.first().operation, QStringLiteral("qso.update"));
    const QJsonObject rebasedUpdate =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QCOMPARE(rebasedUpdate.value(QStringLiteral("expected_version")).toInt(), 2);
    QCOMPARE(rebasedUpdate.value(QStringLiteral("qso")).toObject()
                 .value(QStringLiteral("callsign")).toString(),
             QStringLiteral("N0CALL"));

    QVERIFY2(repository.setSynchronizationEnabled(logUuid, false, &error),
             qPrintable(error));
    logger::Qso offlineQso = sampleQso();
    offlineQso.callsign = QStringLiteral("K2TEST");
    QVERIFY2(repository.saveQso(offlineQso, &error), qPrintable(error));
    QCOMPARE(repository.syncOutbox(logUuid, false, &error).size(), 1);
    const auto local = repository.qsoById(offlineQso.id, &error);
    QVERIFY2(local.has_value(), qPrintable(error));
    QCOMPARE(local->syncStatus, logger::QsoSyncStatus::Local);
}

void SqliteRepositoryTest::persistsQtcWithQsoAndQueuesItForSynchronization()
{
    TestDatabase database;
    logger::SqliteRepository &repository = database.repository();
    QString error;
    const QVector<logger::Contest> contests = repository.contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(contests.size(), 1);
    QVERIFY2(repository.setSynchronizationEnabled(
                 contests.first().logUuid, true, &error),
             qPrintable(error));
    logger::Qso qso = sampleQso();
    qso.qsoUuid = QStringLiteral("11111111-1111-4111-8111-111111111111");
    logger::QtcTransfer transfer;
    transfer.qtcUuid = QStringLiteral("22222222-2222-4222-8222-222222222222");
    transfer.partnerQsoUuid = qso.qsoUuid;
    transfer.partnerCallsign = QStringLiteral("W1AW");
    transfer.direction = logger::QtcDirection::Received;
    transfer.groupNumber = 1;
    transfer.messages.append({
        QDateTime::fromString(QStringLiteral("2026-10-15T09:40:00.000Z"),
                              Qt::ISODateWithMs),
        QStringLiteral("DL2XYZ"), 18
    });
    qso.qtcTransfers.append(transfer);
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));

    const std::optional<logger::Qso> loaded = repository.qsoById(qso.id, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->qtcTransfers.size(), 1);
    QCOMPARE(loaded->qtcTransfers.first().qtcUuid, transfer.qtcUuid);
    const QVector<logger::SyncOutboxOperation> outbox =
        repository.syncOutbox(contests.first().logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(outbox.size(), 1);
    const QJsonObject operation =
        QJsonDocument::fromJson(outbox.first().payloadJson.toUtf8()).object();
    const QJsonArray transfers = operation.value(QStringLiteral("qso")).toObject()
        .value(QStringLiteral("qtc_transfers")).toArray();
    QCOMPARE(transfers.size(), 1);
    QCOMPARE(transfers.first().toObject().value(QStringLiteral("qtc_uuid")).toString(),
             transfer.qtcUuid);
}

void SqliteRepositoryTest::resolvesSyncConflictByAcceptingServer()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const logger::Contest contest = repository.contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(contest.logUuid, true, &error),
             qPrintable(error));

    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QVector<logger::SyncOutboxOperation> operations =
        repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    QVERIFY2(repository.setSyncOperationState(
                 operations.first().operationUuid, QStringLiteral("in-flight"),
                 {}, &error), qPrintable(error));
    QVERIFY2(repository.setSyncOperationState(
                 operations.first().operationUuid, QStringLiteral("accepted"),
                 {}, &error), qPrintable(error));

    qso.callsign = QStringLiteral("N0LOCAL");
    QVERIFY2(repository.updateQso(qso, &error), qPrintable(error));
    operations = repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    const QJsonObject localPayload =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QJsonObject remote = localPayload.value(QStringLiteral("qso")).toObject();
    remote.insert(QStringLiteral("callsign"), QStringLiteral("W1SERVER"));
    remote.insert(QStringLiteral("qso_version"), 2);
    remote.insert(QStringLiteral("is_deleted"), false);
    remote.insert(QStringLiteral("is_invalid"), false);
    QVERIFY2(repository.recordSyncConflict(
                 operations.first().operationUuid,
                 QString::fromUtf8(QJsonDocument(remote)
                                       .toJson(QJsonDocument::Compact)),
                 &error), qPrintable(error));

    QVERIFY2(repository.resolveSyncConflictAcceptServer(
                 operations.first().operationUuid, &error), qPrintable(error));
    const auto stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->callsign, QStringLiteral("W1SERVER"));
    QCOMPARE(stored->qsoVersion, 2);
    QCOMPARE(stored->syncStatus, logger::QsoSyncStatus::Synced);
    QVERIFY2(repository.syncConflicts(contest.logUuid, &error).isEmpty(),
             qPrintable(error));
    operations = repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(operations.isEmpty());

    QSqlQuery audit(database.database());
    QVERIFY(audit.exec(QStringLiteral(
        "SELECT resolution, local_payload_json, remote_payload_json "
        "FROM sync_conflict_resolutions")));
    QVERIFY(audit.next());
    QCOMPARE(audit.value(0).toString(), QStringLiteral("accept_server"));
    QVERIFY(audit.value(1).toString().contains(QStringLiteral("N0LOCAL")));
    QVERIFY(audit.value(2).toString().contains(QStringLiteral("W1SERVER")));
}

void SqliteRepositoryTest::resolvesSyncConflictByKeepingLocalAsNewUpdate()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const logger::Contest contest = repository.contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(contest.logUuid, true, &error),
             qPrintable(error));

    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QVector<logger::SyncOutboxOperation> operations =
        repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    QVERIFY2(repository.setSyncOperationState(
                 operations.first().operationUuid, QStringLiteral("in-flight"),
                 {}, &error), qPrintable(error));
    QVERIFY2(repository.setSyncOperationState(
                 operations.first().operationUuid, QStringLiteral("accepted"),
                 {}, &error), qPrintable(error));

    qso.callsign = QStringLiteral("N0LOCAL");
    QVERIFY2(repository.updateQso(qso, &error), qPrintable(error));
    operations = repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    const QString conflictedOperationUuid = operations.first().operationUuid;
    const QJsonObject localPayload =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QJsonObject remote = localPayload.value(QStringLiteral("qso")).toObject();
    remote.insert(QStringLiteral("callsign"), QStringLiteral("W1SERVER"));
    remote.insert(QStringLiteral("qso_version"), 2);
    remote.insert(QStringLiteral("is_deleted"), false);
    remote.insert(QStringLiteral("is_invalid"), false);
    QVERIFY2(repository.recordSyncConflict(
                 conflictedOperationUuid,
                 QString::fromUtf8(QJsonDocument(remote)
                                       .toJson(QJsonDocument::Compact)),
                 &error), qPrintable(error));

    QVERIFY2(repository.resolveSyncConflictKeepLocal(
                 conflictedOperationUuid, &error), qPrintable(error));
    const auto stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->callsign, QStringLiteral("N0LOCAL"));
    QCOMPARE(stored->qsoVersion, 3);
    QCOMPARE(stored->syncStatus, logger::QsoSyncStatus::Pending);

    operations = repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    QVERIFY(operations.first().operationUuid != conflictedOperationUuid);
    QCOMPARE(operations.first().operation, QStringLiteral("qso.update"));
    const QJsonObject rebased =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QCOMPARE(rebased.value(QStringLiteral("expected_version")).toInt(), 2);
    QCOMPARE(rebased.value(QStringLiteral("qso"))
                 .toObject().value(QStringLiteral("callsign")).toString(),
             QStringLiteral("N0LOCAL"));
    QVERIFY2(repository.syncConflicts(contest.logUuid, &error).isEmpty(),
             qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QSqlQuery audit(database.database());
    QVERIFY(audit.exec(QStringLiteral(
        "SELECT resolution, local_payload_json, remote_payload_json "
        "FROM sync_conflict_resolutions")));
    QVERIFY(audit.next());
    QCOMPARE(audit.value(0).toString(), QStringLiteral("keep_local"));
    QVERIFY(audit.value(1).toString().contains(QStringLiteral("N0LOCAL")));
    QVERIFY(audit.value(2).toString().contains(QStringLiteral("W1SERVER")));
}

void SqliteRepositoryTest::resolvesSyncConflictByMergingSelectedFields()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const logger::Contest contest = repository.contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(contest.logUuid, true, &error),
             qPrintable(error));

    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QVector<logger::SyncOutboxOperation> operations =
        repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSyncOperationState(
                 operations.first().operationUuid, QStringLiteral("in-flight"),
                 {}, &error), qPrintable(error));
    QVERIFY2(repository.setSyncOperationState(
                 operations.first().operationUuid, QStringLiteral("accepted"),
                 {}, &error), qPrintable(error));

    qso.callsign = QStringLiteral("N0LOCAL");
    qso.sentExchange = QStringLiteral("599 101");
    QVERIFY2(repository.updateQso(qso, &error), qPrintable(error));
    operations = repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString conflictedOperationUuid = operations.first().operationUuid;
    const QJsonObject localPayload =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QJsonObject remote = localPayload.value(QStringLiteral("qso")).toObject();
    remote.insert(QStringLiteral("callsign"), QStringLiteral("W1SERVER"));
    remote.insert(QStringLiteral("received_exchange"), QStringLiteral("599 202"));
    remote.insert(QStringLiteral("qso_version"), 2);
    remote.insert(QStringLiteral("is_deleted"), false);
    remote.insert(QStringLiteral("is_invalid"), false);
    QVERIFY2(repository.recordSyncConflict(
                 conflictedOperationUuid,
                 QString::fromUtf8(QJsonDocument(remote)
                                       .toJson(QJsonDocument::Compact)),
                 &error), qPrintable(error));

    const QJsonObject localQso = localPayload.value(QStringLiteral("qso")).toObject();
    QJsonObject merged;
    const QStringList mergeFields{
        QStringLiteral("callsign"), QStringLiteral("station_callsign"),
        QStringLiteral("band"), QStringLiteral("timestamp_utc"),
        QStringLiteral("frequency_hz"), QStringLiteral("mode"),
        QStringLiteral("sent_exchange"), QStringLiteral("received_exchange"),
        QStringLiteral("sent_exchange_fields"),
        QStringLiteral("received_exchange_fields"), QStringLiteral("points")
    };
    for (const QString &field : mergeFields) {
        merged.insert(field, localQso.value(field));
    }
    merged.insert(QStringLiteral("callsign"), QStringLiteral("K2MERGED"));
    merged.insert(QStringLiteral("received_exchange"), QStringLiteral("599 202"));
    QVERIFY2(repository.resolveSyncConflictMerge(
                 conflictedOperationUuid, merged, &error), qPrintable(error));

    const auto stored = repository.qsoById(qso.id, &error);
    QVERIFY2(stored.has_value(), qPrintable(error));
    QCOMPARE(stored->callsign, QStringLiteral("K2MERGED"));
    QCOMPARE(stored->sentExchange, QStringLiteral("599 101"));
    QCOMPARE(stored->receivedExchange, QStringLiteral("599 202"));
    QCOMPARE(stored->qsoVersion, 3);
    QCOMPARE(stored->syncStatus, logger::QsoSyncStatus::Pending);

    operations = repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    QVERIFY(operations.first().operationUuid != conflictedOperationUuid);
    const QJsonObject rebased =
        QJsonDocument::fromJson(operations.first().payloadJson.toUtf8()).object();
    QCOMPARE(rebased.value(QStringLiteral("expected_version")).toInt(), 2);
    QCOMPARE(rebased.value(QStringLiteral("qso")).toObject()
                 .value(QStringLiteral("callsign")).toString(),
             QStringLiteral("K2MERGED"));

    QSqlQuery audit(database.database());
    QVERIFY(audit.exec(QStringLiteral(
        "SELECT resolution, resolution_payload_json "
        "FROM sync_conflict_resolutions")));
    QVERIFY(audit.next());
    QCOMPARE(audit.value(0).toString(), QStringLiteral("merge"));
    const QJsonObject auditPayload =
        QJsonDocument::fromJson(audit.value(1).toString().toUtf8())
            .object().value(QStringLiteral("qso")).toObject();
    QCOMPARE(auditPayload.value(QStringLiteral("callsign")).toString(),
             QStringLiteral("K2MERGED"));
    QCOMPARE(auditPayload.value(QStringLiteral("received_exchange")).toString(),
             QStringLiteral("599 202"));
}

void SqliteRepositoryTest::upgradesLegacyOutboxInvalidFlag()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString logUuid = repository.contests(&error).first().logUuid;
    QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
             qPrintable(error));
    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));

    QSqlQuery query(database.database());
    QVERIFY(query.exec(QStringLiteral(
        "SELECT operation_uuid, payload_json FROM sync_outbox")));
    QVERIFY(query.next());
    const QString operationUuid = query.value(0).toString();
    QJsonObject operation = QJsonDocument::fromJson(
        query.value(1).toByteArray()).object();
    QJsonObject payloadQso = operation.value(QStringLiteral("qso")).toObject();
    QVERIFY(payloadQso.contains(QStringLiteral("is_invalid")));
    payloadQso.remove(QStringLiteral("is_invalid"));
    operation.insert(QStringLiteral("qso"), payloadQso);
    query.finish();
    query.prepare(QStringLiteral(
        "UPDATE sync_outbox SET payload_json = ? WHERE operation_uuid = ?"));
    query.addBindValue(QString::fromUtf8(
        QJsonDocument(operation).toJson(QJsonDocument::Compact)));
    query.addBindValue(operationUuid);
    QVERIFY(query.exec());

    const auto operations = repository.syncOutbox(logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 1);
    const QJsonObject normalized = QJsonDocument::fromJson(
        operations.first().payloadJson.toUtf8()).object();
    QVERIFY(!normalized.value(QStringLiteral("qso")).toObject()
                 .value(QStringLiteral("is_invalid")).toBool());
}

void SqliteRepositoryTest::deletesQsoWithAtomicOutboxTombstone()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString logUuid = repository.contests(&error).first().logUuid;
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
             qPrintable(error));

    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QVERIFY2(repository.deleteQso(qso.id, &error), qPrintable(error));
    QVERIFY(!repository.qsoById(qso.id, &error).has_value());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(repository.qsosForContest(qso.contestId, 0, &error).isEmpty());
    const auto operations = repository.syncOutbox(logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 2);
    QCOMPARE(operations.at(0).operation, QStringLiteral("qso.create"));
    QCOMPARE(operations.at(1).operation, QStringLiteral("qso.delete"));
    const QJsonObject deletion = QJsonDocument::fromJson(
        operations.at(1).payloadJson.toUtf8()).object();
    QCOMPARE(deletion.value(QStringLiteral("expected_version")).toInt(), 1);
    QCOMPARE(deletion.value(QStringLiteral("kind")).toString(),
             QStringLiteral("qso.delete"));

    QSqlQuery stored(database.database());
    stored.prepare(QStringLiteral(
        "SELECT is_deleted, qso_version FROM qsos WHERE id = ?"));
    stored.addBindValue(qso.id);
    QVERIFY2(stored.exec(), qPrintable(stored.lastError().text()));
    QVERIFY(stored.next());
    QCOMPARE(stored.value(0).toInt(), 1);
    QCOMPARE(stored.value(1).toInt(), 2);
}

void SqliteRepositoryTest::auditsAndUndoesQsoChangesAsVersionedOperations()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const logger::Contest contest = repository.contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(contest.logUuid, true, &error),
             qPrintable(error));

    logger::Qso qso = sampleQso();
    qso.contestId = contest.id;
    qso.receivedExchangeFields.insert(QStringLiteral("serial"),
                                      QStringLiteral("027"));
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));

    qso.callsign = QStringLiteral("N0CALL");
    QVERIFY2(repository.updateQso(qso, &error), qPrintable(error));
    QVERIFY2(repository.deleteQso(qso.id, &error), qPrintable(error));
    QVERIFY(!repository.qsoById(qso.id, &error).has_value());

    QVERIFY2(repository.undoLastQsoChange(contest.id, &error), qPrintable(error));
    auto restored = repository.qsoById(qso.id, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->callsign, QStringLiteral("N0CALL"));
    QCOMPARE(restored->qsoVersion, 4);

    QVERIFY2(repository.undoLastQsoChange(contest.id, &error), qPrintable(error));
    restored = repository.qsoById(qso.id, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->callsign, QStringLiteral("W1AW"));
    QCOMPARE(restored->qsoVersion, 5);

    QVERIFY2(repository.undoLastQsoChange(contest.id, &error), qPrintable(error));
    QVERIFY(!repository.qsoById(qso.id, &error).has_value());
    QVERIFY(error.isEmpty());
    QVERIFY(!repository.undoLastQsoChange(contest.id, &error));
    QVERIFY(error.contains(QStringLiteral("no QSO change to undo")));

    QSqlQuery audit(database.database());
    QVERIFY(audit.exec(QStringLiteral(
        "SELECT action, COUNT(*) FROM qso_change_audit "
        "GROUP BY action ORDER BY action")));
    QMap<QString, int> actions;
    while (audit.next()) {
        actions.insert(audit.value(0).toString(), audit.value(1).toInt());
    }
    QCOMPARE(actions.value(QStringLiteral("create")), 1);
    QCOMPARE(actions.value(QStringLiteral("update")), 1);
    QCOMPARE(actions.value(QStringLiteral("delete")), 1);
    QCOMPARE(actions.value(QStringLiteral("undo")), 3);

    const QVector<logger::SyncOutboxOperation> operations =
        repository.syncOutbox(contest.logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 6);
    QCOMPARE(operations.at(3).operation, QStringLiteral("qso.update"));
    QCOMPARE(QJsonDocument::fromJson(operations.at(3).payloadJson.toUtf8())
                 .object().value(QStringLiteral("expected_version")).toInt(), 3);
    QCOMPARE(operations.at(5).operation, QStringLiteral("qso.delete"));
    QCOMPARE(QJsonDocument::fromJson(operations.at(5).payloadJson.toUtf8())
                 .object().value(QStringLiteral("expected_version")).toInt(), 5);
}

void SqliteRepositoryTest::undoRollsBackWhenOutboxWriteFails()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const logger::Contest contest = repository.contests(&error).first();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(contest.logUuid, true, &error),
             qPrintable(error));

    logger::Qso qso = sampleQso();
    QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
    QSqlQuery trigger(database.database());
    QVERIFY2(trigger.exec(QStringLiteral(
        "CREATE TRIGGER fail_undo_outbox BEFORE INSERT ON sync_outbox "
        "BEGIN SELECT RAISE(ABORT, 'simulated outbox failure'); END")),
        qPrintable(trigger.lastError().text()));

    QVERIFY(!repository.undoLastQsoChange(contest.id, &error));
    QVERIFY(error.contains(QStringLiteral("simulated outbox failure")));
    const auto unchanged = repository.qsoById(qso.id, &error);
    QVERIFY2(unchanged.has_value(), qPrintable(error));
    QCOMPARE(unchanged->qsoVersion, 1);
    QSqlQuery audit(database.database());
    QVERIFY2(audit.exec(QStringLiteral(
        "SELECT COUNT(*) FROM qso_change_audit WHERE action = 'undo'")),
        qPrintable(audit.lastError().text()));
    QVERIFY(audit.next());
    QCOMPARE(audit.value(0).toInt(), 0);
}

void SqliteRepositoryTest::restoresPendingOutboxAfterDatabaseReopen()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("sync.sqlite"));
    const QString connectionName = QStringLiteral("logger-sync-restart-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString logUuid;
    QString operationUuid;
    qint64 qsoId = 0;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        {
            logger::SqliteRepository repository(database);
            QString error;
            QVERIFY2(repository.initialize(&error), qPrintable(error));
            QVERIFY2(repository.ensureGeneralLog(&error), qPrintable(error));
            logUuid = repository.contests(&error).first().logUuid;
            QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
                     qPrintable(error));
            logger::Qso qso = sampleQso();
            QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
            qsoId = qso.id;
            const auto entries = repository.syncOutbox(logUuid, false, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(entries.size(), 1);
            operationUuid = entries.first().operationUuid;
        }
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
    {
        const QString reopenedConnectionName = QStringLiteral("logger-sync-reopen-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), reopenedConnectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        {
            logger::SqliteRepository repository(database);
            QString error;
            QVERIFY2(repository.initialize(&error), qPrintable(error));
            const auto entries = repository.syncOutbox(logUuid, false, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(entries.size(), 1);
            QCOMPARE(entries.first().operationUuid, operationUuid);
            QCOMPARE(entries.first().status, QStringLiteral("pending"));
            const auto qso = repository.qsoById(qsoId, &error);
            QVERIFY2(qso.has_value(), qPrintable(error));
            QCOMPARE(qso->syncStatus, logger::QsoSyncStatus::Pending);
        }
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(reopenedConnectionName);
    }
}

void SqliteRepositoryTest::atomicallyAppliesSyncChangesAndRecoversInflightOperations()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString logUuid = repository.contests(&error).first().logUuid;
    QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
             qPrintable(error));

    logger::Qso local = sampleQso();
    QVERIFY2(repository.saveQso(local, &error), qPrintable(error));
    const auto outbox = repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(outbox.size(), 1);
    QVERIFY2(repository.setSyncOperationState(
                 outbox.first().operationUuid, QStringLiteral("in-flight"), {}, &error),
             qPrintable(error));
    QVERIFY2(repository.recoverInFlightSyncOperations(logUuid, &error),
             qPrintable(error));
    auto recovered = repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(recovered.first().status, QStringLiteral("pending"));
    QCOMPARE(recovered.first().operationUuid, outbox.first().operationUuid);

    const QString remoteStation = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString remoteQso = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString remoteOperation = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QJsonObject event = sampleSyncEvent(
        logUuid, remoteStation, remoteQso, remoteOperation, 1,
        QStringLiteral("W1AW"));
    QJsonObject remotePayload = event.value(QStringLiteral("payload")).toObject();
    remotePayload.insert(QStringLiteral("qtc_transfers"),
                         sampleQtcTransfers(remoteQso, QStringLiteral("W1AW")));
    event.insert(QStringLiteral("payload"), remotePayload);
    QJsonObject invalidPayload = event.value(QStringLiteral("payload")).toObject();
    invalidPayload.insert(QStringLiteral("is_invalid"), true);
    invalidPayload.insert(QStringLiteral("points"), 0);
    event.insert(QStringLiteral("payload"), invalidPayload);
    const QString concurrentOperation = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QJsonObject concurrentEvent = sampleSyncEvent(
        logUuid, local.stationUuid, local.qsoUuid, concurrentOperation, 2,
        QStringLiteral("K2REMOTE"));

    QJsonObject invalidNextEvent = event;
    invalidNextEvent.insert(QStringLiteral("event_uuid"),
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
    invalidNextEvent.insert(QStringLiteral("revision"), 3);
    QVERIFY(!repository.applySyncChanges(
        logUuid, QJsonArray{event, invalidNextEvent}, 1, 3, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(0));
    QVERIFY(!repository.qsoById(std::numeric_limits<qint64>::max(), &error).has_value());

    QVERIFY2(repository.applySyncChanges(
                 logUuid, QJsonArray{event}, 1, 1, &error), qPrintable(error));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(1));
    QVERIFY2(repository.applySyncChanges(
                 logUuid, QJsonArray{concurrentEvent}, 2, 2, &error),
             qPrintable(error));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(2));
    const auto qsos = repository.qsosForContest(1, 0, &error);
    QCOMPARE(qsos.size(), 2);
    const auto found = std::find_if(qsos.cbegin(), qsos.cend(),
        [&remoteQso](const logger::Qso &qso) { return qso.qsoUuid == remoteQso; });
    QVERIFY(found != qsos.cend());
    QCOMPARE(found->callsign, QStringLiteral("W1AW"));
    QCOMPARE(found->qsoVersion, 1);
    QVERIFY(found->invalid);
    QCOMPARE(found->points, 0);
    QCOMPARE(found->qtcTransfers.size(), 1);
    QCOMPARE(found->qtcTransfers.first().messages.first().serialNumber, 18);
    const auto conflicted = repository.qsoById(local.id, &error);
    QVERIFY2(conflicted.has_value(), qPrintable(error));
    QCOMPARE(conflicted->callsign, QStringLiteral("K2REMOTE"));
    QCOMPARE(conflicted->syncStatus, logger::QsoSyncStatus::Conflict);
    const auto operationsAfterConflict =
        repository.syncOutbox(logUuid, false, &error);
    QCOMPARE(operationsAfterConflict.size(), 1);
    QCOMPARE(operationsAfterConflict.first().status, QStringLiteral("conflict"));
}

void SqliteRepositoryTest::atomicallyReplacesDataFromVerifiedSnapshot()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString logUuid = repository.contests(&error).first().logUuid;
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
             qPrintable(error));

    logger::Qso pendingQso = sampleQso();
    pendingQso.callsign = QStringLiteral("W1LOCAL");
    QVERIFY2(repository.saveQso(pendingQso, &error), qPrintable(error));

    const QString remoteQsoUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString stationUuid = repository.localStationUuid(&error);
    QVERIFY2(!stationUuid.isEmpty(), qPrintable(error));
    QJsonObject snapshotQso = sampleSyncEvent(
        logUuid, stationUuid, remoteQsoUuid, QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower(), 1,
        QStringLiteral("W1REMOTE")).value(QStringLiteral("payload")).toObject();
    snapshotQso.insert(QStringLiteral("qtc_transfers"),
                       sampleQtcTransfers(remoteQsoUuid,
                                          QStringLiteral("W1REMOTE")));
    const QJsonArray snapshot{snapshotQso};
    const QString checksum = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    QVERIFY2(repository.applySyncSnapshot(
                 logUuid, snapshot, 3, checksum, &error), qPrintable(error));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(3));

    QVector<logger::Qso> qsos = repository.qsosForContest(1, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(qsos.size(), 2);
    const auto remote = std::find_if(qsos.cbegin(), qsos.cend(),
        [&remoteQsoUuid](const logger::Qso &qso) {
            return qso.qsoUuid == remoteQsoUuid
                && qso.callsign == QStringLiteral("W1REMOTE");
        });
    QVERIFY(remote != qsos.cend());
    QCOMPARE(remote->qtcTransfers.size(), 1);
    QCOMPARE(remote->qtcTransfers.first().messages.first().serialNumber, 18);
    QVERIFY(std::any_of(qsos.cbegin(), qsos.cend(),
        [&pendingQso](const logger::Qso &qso) {
            return qso.qsoUuid == pendingQso.qsoUuid
                && qso.syncStatus == logger::QsoSyncStatus::Pending;
        }));

    const QJsonArray emptySnapshot;
    const QString emptyChecksum = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(emptySnapshot).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    QVERIFY2(repository.applySyncSnapshot(
                 logUuid, emptySnapshot, 4, emptyChecksum, &error), qPrintable(error));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(4));
    qsos = repository.qsosForContest(1, 0, &error);
    QCOMPARE(qsos.size(), 1);
    QCOMPARE(qsos.first().qsoUuid, pendingQso.qsoUuid);

    QVERIFY(!repository.applySyncSnapshot(
        logUuid, snapshot, 5, QStringLiteral("invalid"), &error));
    QVERIFY(error.contains(QStringLiteral("checksum")));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(4));
    QCOMPARE(repository.qsosForContest(1, 0, &error).size(), 1);

    const QJsonArray malformedSnapshot{QJsonObject{
        {QStringLiteral("qso_uuid"), QUuid::createUuid()
             .toString(QUuid::WithoutBraces).toLower()}
    }};
    const QString malformedChecksum = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(malformedSnapshot).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    QVERIFY(!repository.applySyncSnapshot(
        logUuid, malformedSnapshot, 5, malformedChecksum, &error));
    QVERIFY(error.contains(QStringLiteral("invalid QSO")));
    QCOMPARE(repository.syncCursor(logUuid, &error), qint64(4));
    QCOMPARE(repository.qsosForContest(1, 0, &error).size(), 1);
}

void SqliteRepositoryTest::rollsBackQsoWhenOutboxInsertFails()
{
    TestDatabase database;
    auto &repository = database.repository();
    QString error;
    const QString logUuid = repository.contests(&error).first().logUuid;
    QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
             qPrintable(error));
    QSqlQuery trigger(database.database());
    QVERIFY(trigger.exec(QStringLiteral(
        "CREATE TRIGGER reject_sync_outbox BEFORE INSERT ON sync_outbox "
        "BEGIN SELECT RAISE(ABORT, 'injected outbox failure'); END")));

    logger::Qso qso = sampleQso();
    QVERIFY(!repository.saveQso(qso, &error));
    QVERIFY(error.contains(QStringLiteral("injected outbox failure")));
    QVERIFY(!repository.qsoById(qso.id, &error).has_value());
    QVERIFY(repository.syncOutbox(logUuid, true, &error).isEmpty());
}

void SqliteRepositoryTest::migratesLegacyExchangesAndPreservesOnUpdate()
{
    const QString connectionName = QStringLiteral("logger-v4-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    const QString yaml = QStringLiteral(
        "schema_version: 1\nid: legacy\nname: Legacy\nbands: [20m]\n"
        "modes: [CW]\nexchange:\n  sent:\n    - {name: report, type: rst, default: \"599\"}\n"
        "    - {name: serial, type: serial, generated: true}\n"
        "  received:\n    - {name: report, type: rst}\n"
        "    - {name: serial, type: integer}\nscoring: {qso_points: 1}\n");
    {
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, "
            "applied_at_utc TEXT NOT NULL)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO schema_migrations VALUES(4, '2026-01-01T00:00:00Z')")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE contests (id INTEGER PRIMARY KEY, code TEXT, name TEXT, "
            "description TEXT DEFAULT '', starts_at_utc TEXT, ends_at_utc TEXT, "
            "definition_yaml TEXT NOT NULL DEFAULT '')")));
        QVERIFY(query.prepare(QStringLiteral(
            "INSERT INTO contests(id, code, name, definition_yaml) VALUES(1, 'LEGACY', 'Legacy', ?)")));
        query.addBindValue(yaml);
        QVERIFY(query.exec());
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE spots (id INTEGER PRIMARY KEY AUTOINCREMENT, callsign TEXT NOT NULL, "
            "frequency_hz INTEGER NOT NULL, mode TEXT DEFAULT '', spotter TEXT DEFAULT '', "
            "comment TEXT DEFAULT '', timestamp_utc TEXT NOT NULL, source TEXT DEFAULT '', band TEXT DEFAULT '')")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE qsos (id INTEGER PRIMARY KEY, contest_id INTEGER, callsign TEXT, "
            "station_callsign TEXT, timestamp_utc TEXT, frequency_hz INTEGER DEFAULT 0, "
            "mode TEXT DEFAULT '', sent_exchange TEXT DEFAULT '', received_exchange TEXT DEFAULT '', "
            "points INTEGER DEFAULT 0, band TEXT DEFAULT '', sent_exchange_json TEXT DEFAULT '{}', "
            "received_exchange_json TEXT DEFAULT '{}')")));
        QVERIFY(query.prepare(QStringLiteral(
            "INSERT INTO qsos(id, contest_id, callsign, station_callsign, timestamp_utc, "
            "mode, sent_exchange, received_exchange) VALUES(10, 1, 'W1AW', 'K1TEST', "
            "'2026-10-15T10:00:00.000Z', 'CW', '599 007', '599 027')")));
        QVERIFY(query.exec());
        query.finish();
    }
    {
        logger::SqliteRepository repository(database);
        QString error;
        QVERIFY2(repository.initialize(&error), qPrintable(error));
        auto loaded = repository.qsoById(10, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QVERIFY(!loaded->qsoUuid.isEmpty());
        QVERIFY(!loaded->logUuid.isEmpty());
        QCOMPARE(loaded->stationUuid, repository.localStationUuid(&error));
        QCOMPARE(loaded->qsoVersion, 1);
        QCOMPARE(loaded->sentExchangeFields.value(QStringLiteral("serial")),
                 QStringLiteral("007"));
        QCOMPARE(loaded->receivedExchangeFields.value(QStringLiteral("serial")),
                 QStringLiteral("027"));
        loaded->callsign = QStringLiteral("N0CALL");
        QVERIFY2(repository.updateQso(*loaded, &error), qPrintable(error));
        loaded = repository.qsoById(10, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(loaded->sentExchangeFields.value(QStringLiteral("serial")),
                 QStringLiteral("007"));
        QCOMPARE(loaded->receivedExchangeFields.value(QStringLiteral("report")),
                 QStringLiteral("599"));
        QCOMPARE(repository.qsosForContest(1, 0, &error).first()
                     .receivedExchangeFields.value(QStringLiteral("serial")),
                 QStringLiteral("027"));
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SqliteRepositoryTest::backsUpAndRestoresVerifiedDatabase()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString backupPath = directory.filePath(QStringLiteral("backup.sqlite"));
    const QString restoredPath = directory.filePath(QStringLiteral("restored.sqlite"));
    const QString invalidBackupPath = directory.filePath(QStringLiteral("invalid.sqlite"));
    const QString connectionName = QStringLiteral("logger-backup-source-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        {
            logger::SqliteRepository repository(database);
            QString error;
            QVERIFY2(repository.initialize(&error), qPrintable(error));
            QVERIFY2(repository.ensureGeneralLog(&error), qPrintable(error));
            logger::Qso qso = sampleQso();
            QVERIFY2(repository.saveQso(qso, &error), qPrintable(error));
            QVERIFY2(repository.backupTo(backupPath, &error), qPrintable(error));
            QVERIFY(QFileInfo::exists(backupPath));
            QVERIFY(!repository.backupTo(backupPath, &error));
            QVERIFY(!error.isEmpty());
        }
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connectionName);

    QFile invalidBackup(invalidBackupPath);
    QVERIFY(invalidBackup.open(QIODevice::WriteOnly));
    QCOMPARE(invalidBackup.write("not a logger database"), qint64(21));
    invalidBackup.close();
    QFile target(restoredPath);
    QVERIFY(target.open(QIODevice::WriteOnly));
    QCOMPARE(target.write("preserve this file"), qint64(18));
    target.close();
    QString error;
    QVERIFY(!logger::SqliteRepository::restoreFromBackup(
        restoredPath, invalidBackupPath, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), QByteArray("preserve this file"));
    target.close();
    QVERIFY(target.remove());

    QVERIFY2(logger::SqliteRepository::restoreFromBackup(
                 restoredPath, backupPath, &error),
             qPrintable(error));
    const QString restoredConnection = QStringLiteral("logger-backup-restored-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase restored = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), restoredConnection);
        restored.setDatabaseName(restoredPath);
        QVERIFY(restored.open());
        QSqlQuery integrity(restored);
        QVERIFY(integrity.exec(QStringLiteral("PRAGMA integrity_check")));
        QVERIFY(integrity.next());
        QCOMPARE(integrity.value(0).toString(), QStringLiteral("ok"));
        integrity.finish();
        QSqlQuery count(restored);
        QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM qsos")));
        QVERIFY(count.next());
        QCOMPARE(count.value(0).toInt(), 1);
        count.finish();
        restored.close();
        restored = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(restoredConnection);
}

QTEST_GUILESS_MAIN(SqliteRepositoryTest)

#include "test_sqlite_repository.moc"
