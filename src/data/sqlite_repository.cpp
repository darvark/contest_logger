#include "logger/data/sqlite_repository.h"

#include "logger/contest/contest_definition_loader.h"
#include "logger/data/log_exporter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

namespace logger {
namespace {

constexpr int CurrentSchemaVersion = 5;
constexpr int SpotPruneInterval = 200;
constexpr int SpotRetentionHours = 24;
constexpr int MaximumRetainedSpots = 5000;

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

Qso readQso(const QSqlQuery &query)
{
    Qso qso;
    qso.id = query.value(QStringLiteral("id")).toLongLong();
    qso.contestId = query.value(QStringLiteral("contest_id")).toLongLong();
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
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO contests(code, name, description) "
        "VALUES(?, ?, ?) ON CONFLICT(code) DO NOTHING"));
    query.addBindValue(QStringLiteral("GENERAL"));
    query.addBindValue(QStringLiteral("General QSO Logging"));
    query.addBindValue(QStringLiteral(
        "General-purpose log. Contest rules are not configured in this stage."));
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
            "SELECT id, code, name, description, starts_at_utc, ends_at_utc "
            "FROM contests ORDER BY name COLLATE NOCASE"))) {
        setError(error, queryError(query));
        return result;
    }

    while (query.next()) {
        Contest contest;
        contest.id = query.value(QStringLiteral("id")).toLongLong();
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
        result.append(std::move(contest));
    }
    return result;
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
    clearError(error);
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
        "INSERT INTO contests(code, name, description, definition_yaml) "
        "VALUES(?, ?, ?, ?)"));
    query.addBindValue(code);
    query.addBindValue(name);
    query.addBindValue(definition.description);
    query.addBindValue(yaml);
    if (!query.exec()) {
        setError(error, queryError(query));
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool SqliteRepository::saveContestDefinition(
    const ContestDefinition &definition, const QString &yaml, QString *error)
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO contests(code, name, description, definition_yaml) "
        "VALUES(?, ?, ?, ?) "
        "ON CONFLICT(code) DO UPDATE SET "
        "name = excluded.name, description = excluded.description, "
        "definition_yaml = excluded.definition_yaml"));
    query.addBindValue(definition.id.toUpper());
    query.addBindValue(definition.name);
    query.addBindValue(definition.description);
    query.addBindValue(yaml);
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

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO qsos(contest_id, callsign, station_callsign, band, timestamp_utc, "
        "frequency_hz, mode, sent_exchange, received_exchange, "
        "sent_exchange_json, received_exchange_json, points) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
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
    query.addBindValue(qso.points);

    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    qso.id = query.lastInsertId().toLongLong();
    return true;
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

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE qsos SET contest_id = ?, callsign = ?, station_callsign = ?, "
        "band = ?, timestamp_utc = ?, frequency_hz = ?, mode = ?, "
        "sent_exchange = ?, received_exchange = ?, sent_exchange_json = ?, "
        "received_exchange_json = ?, points = ? WHERE id = ?"));
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
    query.addBindValue(qso.points);
    query.addBindValue(qso.id);
    if (!query.exec()) {
        setError(error, queryError(query));
        return false;
    }
    if (query.numRowsAffected() == 0) {
        setError(error, QStringLiteral("QSO %1 does not exist.").arg(qso.id));
        return false;
    }
    return true;
}

std::optional<Qso> SqliteRepository::qsoById(qint64 id, QString *error) const
{
    clearError(error);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT q.id, q.contest_id, q.callsign, q.station_callsign, q.band, q.timestamp_utc, "
        "q.frequency_hz, q.mode, q.sent_exchange, q.received_exchange, "
        "sent_exchange_json, received_exchange_json, points, "
        "c.definition_yaml AS definition_yaml FROM qsos q "
        "LEFT JOIN contests c ON c.id = q.contest_id WHERE q.id = ?"));
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
        "SELECT id FROM qsos WHERE contest_id = ? "
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
        "sent_exchange_json, received_exchange_json, points, "
        "c.definition_yaml AS definition_yaml FROM qsos q "
        "LEFT JOIN contests c ON c.id = q.contest_id "
        "WHERE q.contest_id = ? ORDER BY q.timestamp_utc DESC, q.id DESC");
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
