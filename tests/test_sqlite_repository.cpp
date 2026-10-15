#include "logger/data/sqlite_repository.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QtTest>

#include <algorithm>
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

} // namespace

class SqliteRepositoryTest final : public QObject {
    Q_OBJECT

private slots:
    void savesAndReadsQso();
    void commitsTransaction();
    void rollsBackFailedTransaction();
    void appliesSchemaMigrationOnce();
    void migratesExistingVersionOneDatabase();
    void savesContestDefinition();
    void updatesQsoAndPersistsExchangeFields();
    void loadsAllQsosForDupeChecking();
    void savesAndReadsSpot();
    void savesSpotWithoutMode();
    void prunesRetainedSpots();
    void findsDuplicateQsos();
    void migratesLegacyExchangesAndPreservesOnUpdate();
    void backsUpAndRestoresVerifiedDatabase();
};

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
    QCOMPARE(query.value(0).toInt(), 5);
    query.finish();

    QVERIFY2(database.repository().initialize(&error), qPrintable(error));
    const auto contests = database.repository().contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(contests.size(), 1);
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
