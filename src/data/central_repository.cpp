#include "logger/data/central_repository.h"

#include "logger/contest/contest_definition_loader.h"
#include "logger/contest/contest_engine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUuid>

#include <cmath>
#include <limits>

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

QString sqlError(const QSqlQuery &query)
{
    return query.lastError().text();
}

QString utcString(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QString rollbackFailure(QSqlDatabase &database, const QString &failure)
{
    if (database.rollback()) {
        return failure;
    }
    return failure + QStringLiteral("; rollback failed: ")
        + database.lastError().text();
}

QJsonValue canonicalJsonValue(const QJsonValue &value)
{
    if (value.isObject()) {
        QJsonObject ordered;
        const QJsonObject object = value.toObject();
        for (const QString &key : object.keys()) {
            ordered.insert(key, canonicalJsonValue(object.value(key)));
        }
        return ordered;
    }
    if (value.isArray()) {
        QJsonArray ordered;
        for (const QJsonValue &item : value.toArray()) {
            ordered.append(canonicalJsonValue(item));
        }
        return ordered;
    }
    return value;
}

QByteArray canonicalObject(const QJsonObject &object)
{
    return QJsonDocument(canonicalJsonValue(object).toObject())
        .toJson(QJsonDocument::Compact);
}

bool isUuid(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
                       "[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch();
}

bool isStringObject(const QJsonValue &value)
{
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!it.value().isString()) {
            return false;
        }
    }
    return true;
}

bool isNetworkStorage(const QString &path)
{
#ifdef Q_OS_WIN
    if (path.startsWith(QStringLiteral("\\\\"))) {
        return true;
    }
#endif
    const QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady()) {
        return false;
    }
    const QString type = QString::fromLatin1(storage.fileSystemType()).toLower();
    return type.startsWith(QStringLiteral("nfs"))
        || type.startsWith(QStringLiteral("cifs"))
        || type.startsWith(QStringLiteral("smb"))
        || type.startsWith(QStringLiteral("sshfs"));
}

QString stringValue(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString();
}

QJsonObject qsoSnapshot(const QJsonObject &input, int version, bool deleted)
{
    QJsonObject snapshot = input;
    snapshot.insert(QStringLiteral("qso_version"), version);
    snapshot.insert(QStringLiteral("is_deleted"), deleted);
    return snapshot;
}

QMap<QString, QString> stringMap(const QJsonValue &value)
{
    QMap<QString, QString> result;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        result.insert(it.key(), it.value().toString());
    }
    return result;
}

Qso qsoFromJson(const QJsonObject &object)
{
    Qso qso;
    qso.qsoUuid = stringValue(object, QStringLiteral("qso_uuid"));
    qso.logUuid = stringValue(object, QStringLiteral("log_uuid"));
    qso.stationUuid = stringValue(object, QStringLiteral("station_uuid"));
    qso.stationCallsign = stringValue(object, QStringLiteral("station_callsign"));
    qso.callsign = stringValue(object, QStringLiteral("callsign"));
    qso.timestampUtc = QDateTime::fromString(
        stringValue(object, QStringLiteral("timestamp_utc")), Qt::ISODateWithMs);
    qso.frequencyHz = object.value(QStringLiteral("frequency_hz")).toVariant().toLongLong();
    qso.band = stringValue(object, QStringLiteral("band"));
    qso.mode = stringValue(object, QStringLiteral("mode"));
    qso.sentExchange = stringValue(object, QStringLiteral("sent_exchange"));
    qso.receivedExchange = stringValue(object, QStringLiteral("received_exchange"));
    qso.sentExchangeFields = stringMap(
        object.value(QStringLiteral("sent_exchange_fields")));
    qso.receivedExchangeFields = stringMap(
        object.value(QStringLiteral("received_exchange_fields")));
    qso.points = object.value(QStringLiteral("points")).toInt();
    return qso;
}

bool conditionNeedsCty(const QMap<QString, QString> &condition)
{
    static const QSet<QString> ctyKeys{
        QStringLiteral("country"), QStringLiteral("dxcc"),
        QStringLiteral("continent"), QStringLiteral("cq_zone"),
        QStringLiteral("itu_zone"), QStringLiteral("station.dxcc"),
        QStringLiteral("station.country"), QStringLiteral("station.continent"),
        QStringLiteral("station.cq_zone"), QStringLiteral("station.itu_zone"),
        QStringLiteral("same_dxcc"), QStringLiteral("same_country"),
        QStringLiteral("same_continent")
    };
    const QStringList keys = condition.keys();
    return std::any_of(keys.cbegin(), keys.cend(),
        [](const QString &key) { return ctyKeys.contains(key); });
}

bool ctyConditionsResolved(const ContestDefinition &definition,
                          const QMap<QString, QString> &resolved)
{
    const auto conditionResolved = [&resolved](const QMap<QString, QString> &condition) {
        for (auto it = condition.cbegin(); it != condition.cend(); ++it) {
            const QString key = it.key();
            if (key == QStringLiteral("country") || key == QStringLiteral("dxcc")) {
                if (!resolved.contains(QStringLiteral("dxcc"))) {
                    return false;
                }
            } else if (key == QStringLiteral("station.country")
                       || key == QStringLiteral("station.dxcc")) {
                if (!resolved.contains(QStringLiteral("station.dxcc"))) {
                    return false;
                }
            } else if (!resolved.contains(key)) {
                return false;
            }
        }
        return true;
    };
    for (const QsoPointsRule &rule : definition.qsoPointRules) {
        if (conditionNeedsCty(rule.when) && !conditionResolved(rule.when)) {
            return false;
        }
    }
    for (const ExchangeFieldDefinition &field : definition.sentExchange) {
        if (field.hasWhen && conditionNeedsCty(field.when)
            && !conditionResolved(field.when)) {
            return false;
        }
    }
    for (const ExchangeFieldDefinition &field : definition.receivedExchange) {
        if (field.hasRequiredWhen && conditionNeedsCty(field.requiredWhen)
            && !conditionResolved(field.requiredWhen)) {
            return false;
        }
    }
    return true;
}

QMap<QString, QString> resolveCtyForQso(
    const Qso &qso, const QVector<CtyPrefixEntry> &prefixes)
{
    if (prefixes.isEmpty()) {
        return {};
    }
    QMap<QString, QString> resolved =
        SupportDataParser::resolveCtyMultipliers(qso.callsign, prefixes);
    const QMap<QString, QString> station = SupportDataParser::resolveCtyMultipliers(
        qso.stationCallsign, prefixes);
    for (const QString &key : {QStringLiteral("dxcc"),
                               QStringLiteral("country"),
                               QStringLiteral("continent"),
                               QStringLiteral("cq_zone"),
                               QStringLiteral("itu_zone")}) {
        if (station.contains(key)) {
            resolved.insert(QStringLiteral("station.%1").arg(key),
                            station.value(key));
        }
    }
    if (resolved.contains(QStringLiteral("dxcc"))
        && station.contains(QStringLiteral("dxcc"))) {
        const bool sameEntity = resolved.value(QStringLiteral("dxcc"))
            == station.value(QStringLiteral("dxcc"));
        resolved.insert(QStringLiteral("same_dxcc"), sameEntity ? "true" : "false");
        resolved.insert(QStringLiteral("same_country"), sameEntity ? "true" : "false");
    }
    if (resolved.contains(QStringLiteral("continent"))
        && station.contains(QStringLiteral("continent"))) {
        resolved.insert(QStringLiteral("same_continent"),
            resolved.value(QStringLiteral("continent"))
                    == station.value(QStringLiteral("continent"))
                ? "true" : "false");
    }
    return resolved;
}

} // namespace

CentralRepository::CentralRepository() = default;

CentralRepository::~CentralRepository()
{
    m_repository.reset();
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = QSqlDatabase();
    if (!m_connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    if (m_lockFile) {
        m_lockFile->unlock();
    }
}

bool CentralRepository::initialize(const QString &dataDirectory, QString *error)
{
    if (m_repository || m_database.isValid()) {
        setError(error, QStringLiteral("Central repository is already initialized."));
        return false;
    }
    const QString trimmedDirectory = dataDirectory.trimmed();
    if (trimmedDirectory.isEmpty()) {
        setError(error, QStringLiteral("Central data directory is required."));
        return false;
    }
    const QDir dataDir(QDir::cleanPath(QFileInfo(trimmedDirectory).absoluteFilePath()));
    if (!QDir().mkpath(dataDir.absolutePath())) {
        setError(error, QStringLiteral("Could not create central data directory."));
        return false;
    }
    if (isNetworkStorage(dataDir.absolutePath())) {
        setError(error, QStringLiteral(
            "Central SQLite data must be stored on a local filesystem, not a network share."));
        return false;
    }

    m_ctyPrefixes.clear();
    const QString ctyPath = dataDir.filePath(QStringLiteral("wl_cty.dat"));
    if (QFileInfo::exists(ctyPath)) {
        QFile ctyFile(ctyPath);
        if (!ctyFile.open(QIODevice::ReadOnly)) {
            setError(error, QStringLiteral("Could not read central wl_cty.dat: %1")
                                .arg(ctyFile.errorString()));
            return false;
        }
        QVector<CtyPrefixEntry> prefixes;
        QString ctyError;
        if (!SupportDataParser::parseCtyDat(
                QString::fromUtf8(ctyFile.readAll()), prefixes, &ctyError)) {
            setError(error, QStringLiteral("Central wl_cty.dat is invalid: %1")
                                .arg(ctyError));
            return false;
        }
        m_ctyPrefixes = std::move(prefixes);
    }

    m_databasePath = dataDir.filePath(QStringLiteral("central-log.sqlite"));
    m_backupDirectory = dataDir.filePath(QStringLiteral("backups"));
    if (!QDir().mkpath(m_backupDirectory)) {
        setError(error, QStringLiteral("Could not create central backup directory."));
        return false;
    }

    m_lockFile = std::make_unique<QLockFile>(
        dataDir.filePath(QStringLiteral("central-repository.lock")));
    if (!m_lockFile->tryLock(0)) {
        setError(error, QStringLiteral(
            "Central data directory is already locked by another server process."));
        m_lockFile.reset();
        return false;
    }

    m_connectionName = QStringLiteral("contest-logger-central-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);
    if (!m_database.open()) {
        setError(error, m_database.lastError().text());
        return false;
    }

    QSqlQuery schema(m_database);
    if (!schema.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' "
            "AND name = 'schema_migrations'"))) {
        setError(error, sqlError(schema));
        return false;
    }
    const bool hasMigrationTable = schema.next();
    schema.finish();
    if (hasMigrationTable) {
        QSqlQuery version(m_database);
        if (!version.exec(QStringLiteral(
                "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))
            || !version.next()) {
            setError(error, sqlError(version));
            return false;
        }
        const int currentVersion = version.value(0).toInt();
        version.finish();
        if (currentVersion > 0
            && currentVersion < SqliteRepository::currentSchemaVersion()) {
            const QString backupPath = QDir(m_backupDirectory).filePath(
                QStringLiteral("pre-migration-%1-%2.sqlite")
                    .arg(currentVersion)
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            SqliteRepository backupRepository(m_database);
            if (!backupRepository.backupTo(backupPath, error)) {
                return false;
            }
        }
    }

    m_repository = std::make_unique<SqliteRepository>(m_database);
    if (!m_repository->initialize(error)) {
        m_repository.reset();
        return false;
    }
    if (!m_repository->ensureGeneralLog(error)) {
        m_repository.reset();
        return false;
    }
    return true;
}

qint64 CentralRepository::createLog(
    const ContestDefinition &definition, const QString &yaml,
    const Contest &log, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(log.logUuid)
        || log.name.trimmed().isEmpty() || yaml.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Central log identity or definition is invalid."));
        return 0;
    }
    QString definitionError;
    ContestDefinition parsedDefinition;
    if (!ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("central log creation"),
            parsedDefinition, &definitionError)
        || parsedDefinition.id.compare(definition.id, Qt::CaseInsensitive) != 0) {
        setError(error, definitionError.isEmpty()
            ? QStringLiteral("Contest definition identity does not match.")
            : definitionError);
        return 0;
    }
    const QVector<Contest> logs = m_repository->contests(error);
    if (error != nullptr && !error->isEmpty()) {
        return 0;
    }
    if (std::any_of(logs.cbegin(), logs.cend(), [&log](const Contest &existing) {
            return existing.logUuid == log.logUuid;
        })) {
        setError(error, QStringLiteral("Central log UUID already exists."));
        return 0;
    }
    return m_repository->createContestLog(definition, yaml, log, error);
}

bool CentralRepository::submitBatch(
    const QString &logUuid, const QJsonArray &operations,
    QJsonArray *results, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (m_repository == nullptr || results == nullptr) {
        setError(error, QStringLiteral("Central repository and result output are required."));
        return false;
    }
    if (!isUuid(logUuid) || operations.isEmpty() || operations.size() > 100) {
        setError(error, QStringLiteral("Log UUID or batch size is invalid."));
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    const auto rollback = [this, error](const QString &message) {
        QString failure = message;
        if (!m_database.rollback()) {
            failure += QStringLiteral("; rollback failed: ")
                + m_database.lastError().text();
        }
        setError(error, failure);
        return false;
    };

    QSqlQuery log(m_database);
    log.prepare(QStringLiteral(
        "SELECT id, definition_yaml, station_callsign FROM contests "
        "WHERE log_uuid = ?"));
    log.addBindValue(logUuid);
    if (!log.exec()) {
        return rollback(sqlError(log));
    }
    if (!log.next()) {
        return rollback(QStringLiteral("UNKNOWN_LOG: %1").arg(logUuid));
    }
    const qint64 contestId = log.value(0).toLongLong();
    const QString definitionYaml = log.value(1).toString();
    const QString contestStationCallsign = log.value(2).toString();
    log.finish();
    ContestDefinition definition;
    const bool hasDefinition = !definitionYaml.trimmed().isEmpty();
    QString serialField;
    int serialWidth = 0;
    if (hasDefinition) {
        QString definitionError;
        if (!ContestDefinitionLoader().loadText(
                definitionYaml, QStringLiteral("central contest definition"),
                definition, &definitionError)) {
            return rollback(QStringLiteral("CONTEST_MISMATCH: %1").arg(definitionError));
        }
        for (const ExchangeFieldDefinition &field : definition.sentExchange) {
            if (field.type == ExchangeFieldType::Serial && field.generated) {
                serialField = field.name;
                serialWidth = field.width;
                break;
            }
        }
    }

    QSqlQuery createLogState(m_database);
    createLogState.prepare(QStringLiteral(
        "INSERT INTO sync_log_state(log_uuid, current_revision) VALUES(?, 0) "
        "ON CONFLICT(log_uuid) DO NOTHING"));
    createLogState.addBindValue(logUuid);
    if (!createLogState.exec()) {
        return rollback(sqlError(createLogState));
    }

    QJsonArray batchResults;
    for (const QJsonValue &value : operations) {
        if (!value.isObject()) {
            return rollback(QStringLiteral("INVALID_MESSAGE: operation must be an object."));
        }
        const QJsonObject operation = value.toObject();
        const QStringList expectedOperationKeys{
            QStringLiteral("created_at_utc"), QStringLiteral("expected_version"),
            QStringLiteral("kind"), QStringLiteral("log_uuid"),
            QStringLiteral("operation_uuid"), QStringLiteral("qso"),
            QStringLiteral("station_uuid")
        };
        QStringList keysWithReservation = expectedOperationKeys;
        keysWithReservation.append(QStringLiteral("reservation_uuid"));
        keysWithReservation.sort();
        if (operation.keys() != expectedOperationKeys
            && operation.keys() != keysWithReservation) {
            return rollback(QStringLiteral("INVALID_MESSAGE: operation fields are invalid."));
        }
        const QString operationUuid = stringValue(
            operation, QStringLiteral("operation_uuid"));
        const QString operationLogUuid = stringValue(
            operation, QStringLiteral("log_uuid"));
        const QString stationUuid = stringValue(
            operation, QStringLiteral("station_uuid"));
        const QString kind = stringValue(operation, QStringLiteral("kind"));
        const QString reservationUuid =
            stringValue(operation, QStringLiteral("reservation_uuid"));
        const QString createdAt = stringValue(
            operation, QStringLiteral("created_at_utc"));
        const QJsonValue expectedVersionValue =
            operation.value(QStringLiteral("expected_version"));
        QJsonObject qso = operation.value(QStringLiteral("qso")).toObject();
        const QStringList expectedQsoKeys{
            QStringLiteral("band"), QStringLiteral("callsign"),
            QStringLiteral("frequency_hz"), QStringLiteral("log_uuid"),
            QStringLiteral("mode"), QStringLiteral("points"),
            QStringLiteral("qso_uuid"), QStringLiteral("received_exchange"),
            QStringLiteral("received_exchange_fields"),
            QStringLiteral("sent_exchange"), QStringLiteral("sent_exchange_fields"),
            QStringLiteral("station_callsign"), QStringLiteral("station_uuid"),
            QStringLiteral("timestamp_utc")
        };
        const QDateTime operationTime = QDateTime::fromString(createdAt, Qt::ISODateWithMs);
        const QDateTime qsoTime = QDateTime::fromString(
            stringValue(qso, QStringLiteral("timestamp_utc")), Qt::ISODateWithMs);
        const double expectedVersionNumber = expectedVersionValue.toDouble(-1);
        if (!isUuid(operationUuid) || !isUuid(stationUuid)
            || operationLogUuid != logUuid
            || (kind != QStringLiteral("qso.create")
                && kind != QStringLiteral("qso.update")
                && kind != QStringLiteral("qso.delete"))
            || !expectedVersionValue.isDouble()
            || expectedVersionNumber < 0
            || expectedVersionNumber > std::numeric_limits<int>::max()
            || expectedVersionNumber != static_cast<int>(expectedVersionNumber)
            || !operationTime.isValid() || !createdAt.endsWith(QLatin1Char('Z'))
            || (operation.keys() != expectedOperationKeys
                && operation.keys() != keysWithReservation)
            || qso.keys() != expectedQsoKeys
            || stringValue(qso, QStringLiteral("qso_uuid")).isEmpty()
            || !isUuid(stringValue(qso, QStringLiteral("qso_uuid")))
            || stringValue(qso, QStringLiteral("log_uuid")) != logUuid
            || !isUuid(stringValue(qso, QStringLiteral("station_uuid")))
            || (kind == QStringLiteral("qso.create")
                && stringValue(qso, QStringLiteral("station_uuid")) != stationUuid)
            || (operation.contains(QStringLiteral("reservation_uuid"))
                && !isUuid(reservationUuid))
            || (kind != QStringLiteral("qso.create") && !reservationUuid.isEmpty())
            || stringValue(qso, QStringLiteral("callsign")).trimmed().isEmpty()
            || stringValue(qso, QStringLiteral("station_callsign")).trimmed().isEmpty()
            || stringValue(qso, QStringLiteral("band")).trimmed().isEmpty()
            || stringValue(qso, QStringLiteral("mode")).trimmed().isEmpty()
            || !qsoTime.isValid()
            || !stringValue(qso, QStringLiteral("timestamp_utc"))
                    .endsWith(QLatin1Char('Z'))
            || !isStringObject(qso.value(QStringLiteral("sent_exchange_fields")))
            || !isStringObject(qso.value(QStringLiteral("received_exchange_fields")))) {
            return rollback(QStringLiteral("INVALID_MESSAGE: QSO operation is invalid."));
        }
        const QJsonValue frequencyValue = qso.value(QStringLiteral("frequency_hz"));
        const QJsonValue pointsValue = qso.value(QStringLiteral("points"));
        const double frequencyNumber = frequencyValue.toDouble(-1);
        double pointsNumber = pointsValue.toDouble(-1);
        if (!frequencyValue.isDouble() || !std::isfinite(frequencyNumber)
            || frequencyNumber <= 0
            || frequencyNumber > static_cast<double>(
                   std::numeric_limits<qint64>::max())
            || frequencyNumber != static_cast<qint64>(frequencyNumber)
            || !pointsValue.isDouble() || !std::isfinite(pointsNumber)
            || pointsNumber < 0
            || pointsNumber > std::numeric_limits<int>::max()
            || pointsNumber != static_cast<int>(pointsNumber)) {
            return rollback(QStringLiteral("INVALID_MESSAGE: frequency or points are invalid."));
        }
        const int expectedVersion = static_cast<int>(expectedVersionNumber);
        const QString qsoUuid = stringValue(qso, QStringLiteral("qso_uuid"));
        const QByteArray operationPayload = canonicalObject(operation);
        const QByteArray payloadHash = QCryptographicHash::hash(
            operationPayload, QCryptographicHash::Sha256).toHex();

        QSqlQuery prior(m_database);
        prior.prepare(QStringLiteral(
            "SELECT payload_sha256, result_json FROM sync_operations "
            "WHERE operation_uuid = ?"));
        prior.addBindValue(operationUuid);
        if (!prior.exec()) {
            return rollback(sqlError(prior));
        }
        if (prior.next()) {
            if (prior.value(0).toByteArray() != payloadHash) {
                return rollback(QStringLiteral(
                    "IDEMPOTENCY_KEY_REUSED: operation UUID has a different payload."));
            }
            const QJsonDocument savedResult = QJsonDocument::fromJson(
                prior.value(1).toByteArray());
            prior.finish();
            if (!savedResult.isObject()) {
                return rollback(QStringLiteral("Stored operation result is invalid."));
            }
            batchResults.append(savedResult.object());
            continue;
        }
        prior.finish();

        if (kind == QStringLiteral("qso.create")
            && (definition.serialServerRequired || !reservationUuid.isEmpty())) {
            if (serialField.isEmpty()) {
                return rollback(QStringLiteral(
                    "SERIAL_UNAVAILABLE: contest has no generated sent serial."));
            }
            const QJsonObject sentFields =
                qso.value(QStringLiteral("sent_exchange_fields")).toObject();
            bool serialOk = false;
            const int sentSerial =
                sentFields.value(serialField).toString().toInt(&serialOk);
            if (!serialOk || sentSerial < definition.serialStart) {
                return rollback(QStringLiteral(
                    "SERIAL_UNAVAILABLE: QSO does not contain a valid reserved serial."));
            }
            if (reservationUuid.isEmpty()) {
                return rollback(QStringLiteral(
                    "SERIAL_UNAVAILABLE: server serial reservation is required."));
            }
            const QString band = stringValue(qso, QStringLiteral("band"))
                                     .trimmed().toLower();
            const QString mode = stringValue(qso, QStringLiteral("mode"))
                                     .trimmed().toUpper();
            QString scopeKey = QStringLiteral("global");
            if (definition.serialScope == QStringLiteral("band")) {
                scopeKey = QStringLiteral("band:") + band;
            } else if (definition.serialScope == QStringLiteral("mode")) {
                scopeKey = QStringLiteral("mode:") + mode;
            } else if (definition.serialScope == QStringLiteral("band_mode")) {
                scopeKey = QStringLiteral("band:") + band
                    + QStringLiteral("|mode:") + mode;
            }
            QSqlQuery reservation(m_database);
            reservation.prepare(QStringLiteral(
                "SELECT serial_number, status, scope_key, band, mode, qso_uuid "
                "FROM serial_reservations WHERE reservation_uuid = ? "
                "AND log_uuid = ? AND station_uuid = ?"));
            reservation.addBindValue(reservationUuid);
            reservation.addBindValue(logUuid);
            reservation.addBindValue(stationUuid);
            if (!reservation.exec()) {
                return rollback(sqlError(reservation));
            }
            if (!reservation.next()
                || (reservation.value(1).toString() != QStringLiteral("reserved")
                    && !(reservation.value(1).toString() == QStringLiteral("used")
                         && reservation.value(5).toString() == qsoUuid))
                || reservation.value(2).toString() != scopeKey
                || reservation.value(3).toString() != band
                || reservation.value(4).toString() != mode
                || reservation.value(0).toInt() != sentSerial) {
                return rollback(QStringLiteral(
                    "SERIAL_UNAVAILABLE: reservation is missing, used or does not match QSO."));
            }
        }

        if (hasDefinition && kind != QStringLiteral("qso.delete")) {
            QString readError;
            QVector<Qso> previousQsos = m_repository->qsosForContest(
                contestId, 0, &readError);
            if (!readError.isEmpty()) {
                return rollback(readError);
            }
            previousQsos.erase(
                std::remove_if(previousQsos.begin(), previousQsos.end(),
                    [&qsoUuid](const Qso &previous) {
                        return previous.qsoUuid == qsoUuid;
                    }),
                previousQsos.end());

            const Qso candidate = qsoFromJson(qso);
            const ContestEngine::MultiplierResolver resolver =
                [this, &contestStationCallsign](const Qso &item) {
                    Qso resolvedQso = item;
                    if (!contestStationCallsign.trimmed().isEmpty()) {
                        resolvedQso.stationCallsign = contestStationCallsign;
                    }
                    return resolveCtyForQso(resolvedQso, m_ctyPrefixes);
                };
            const QMap<QString, QString> resolved = resolver(candidate);
            if (!ctyConditionsResolved(definition, resolved)) {
                return rollback(QStringLiteral(
                    "CONTEST_DATA_UNAVAILABLE: central wl_cty.dat or callsign "
                    "location data is required by this contest definition."));
            }

            ContestEngine engine;
            ContestEvaluationContext context =
                engine.buildContext(definition, previousQsos, resolver);
            context.resolvedMultiplierValues = resolved;
            std::optional<int> requestedSerial;
            if (!serialField.isEmpty()) {
                bool serialOk = false;
                const int serial = candidate.sentExchangeFields
                    .value(serialField).toInt(&serialOk);
                if (serialOk) {
                    requestedSerial = serial;
                }
            }
            const ContestEvaluationResult evaluation = engine.evaluate(
                definition, candidate.callsign, candidate.band, candidate.mode,
                candidate.receivedExchangeFields, context, requestedSerial,
                candidate.sentExchangeFields);
            if (evaluation.duplicate) {
                bool existingIdentity = false;
                if (kind == QStringLiteral("qso.create")) {
                    QSqlQuery sameIdentity(m_database);
                    sameIdentity.prepare(QStringLiteral(
                        "SELECT 1 FROM qsos WHERE contest_id = ? AND qso_uuid = ?"));
                    sameIdentity.addBindValue(contestId);
                    sameIdentity.addBindValue(qsoUuid);
                    if (!sameIdentity.exec()) {
                        return rollback(sqlError(sameIdentity));
                    }
                    existingIdentity = sameIdentity.next();
                }
                if (!existingIdentity) {
                    return rollback(QStringLiteral(
                        "DUPLICATE_QSO: QSO matches the contest dupe key."));
                }
            }
            if (!evaluation.valid) {
                QStringList issues;
                for (const ContestValidationIssue &issue : evaluation.issues) {
                    issues.append(issue.field + QStringLiteral(" ")
                                  + issue.message);
                }
                return rollback(QStringLiteral("CONTEST_VALIDATION_FAILED: %1")
                                    .arg(issues.join(QStringLiteral("; "))));
            }
            qso.insert(QStringLiteral("points"), evaluation.points);
            pointsNumber = evaluation.points;
        }

        qint64 qsoId = 0;
        int currentVersion = 0;
        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral(
            "SELECT id, qso_version, is_deleted, station_uuid FROM qsos "
            "WHERE contest_id = ? AND qso_uuid = ?"));
        existing.addBindValue(contestId);
        existing.addBindValue(qsoUuid);
        if (!existing.exec()) {
            return rollback(sqlError(existing));
        }
        const bool exists = existing.next();
        if (exists) {
            qsoId = existing.value(0).toLongLong();
            currentVersion = existing.value(1).toInt();
            const bool deleted = existing.value(2).toBool();
            const QString currentStationUuid = existing.value(3).toString();
            existing.finish();
            if (kind == QStringLiteral("qso.create")) {
                QSqlQuery current(m_database);
                current.prepare(QStringLiteral(
                    "SELECT snapshot_json FROM qso_history WHERE log_uuid = ? "
                    "AND qso_uuid = ? AND qso_version = ?"));
                current.addBindValue(logUuid);
                current.addBindValue(qsoUuid);
                current.addBindValue(currentVersion);
                if (!current.exec() || !current.next()) {
                    return rollback(current.lastError().isValid()
                        ? sqlError(current)
                        : QStringLiteral("Stored QSO history is unavailable."));
                }
                const QJsonDocument currentDocument = QJsonDocument::fromJson(
                    current.value(0).toByteArray());
                current.finish();
                if (!currentDocument.isObject()) {
                    return rollback(QStringLiteral("Stored QSO snapshot is invalid."));
                }
                QJsonObject currentQso = currentDocument.object();
                currentQso.remove(QStringLiteral("qso_version"));
                currentQso.remove(QStringLiteral("is_deleted"));
                const bool identical = !deleted
                    && currentStationUuid == stringValue(qso, QStringLiteral("station_uuid"))
                    && canonicalJsonValue(currentQso)
                           == canonicalJsonValue(QJsonValue(qso));
                if (!identical) {
                    return rollback(QStringLiteral(
                        "VERSION_CONFLICT: QSO UUID already exists with different data."));
                }
                QString revisionError;
                const qint64 revision = currentRevision(logUuid, &revisionError);
                if (!revisionError.isEmpty()) {
                    return rollback(revisionError);
                }
                const QJsonObject result{
                    {QStringLiteral("operation_uuid"), operationUuid},
                    {QStringLiteral("status"), QStringLiteral("accepted")},
                    {QStringLiteral("qso_uuid"), qsoUuid},
                    {QStringLiteral("qso_version"), currentVersion},
                    {QStringLiteral("revision"), revision},
                    {QStringLiteral("qso"), currentDocument.object()}
                };
                QSqlQuery saveOperation(m_database);
                saveOperation.prepare(QStringLiteral(
                    "INSERT INTO sync_operations(operation_uuid, log_uuid, station_uuid, "
                    "payload_sha256, result_json, created_at_utc) VALUES(?, ?, ?, ?, ?, ?)"));
                saveOperation.addBindValue(operationUuid);
                saveOperation.addBindValue(logUuid);
                saveOperation.addBindValue(stationUuid);
                saveOperation.addBindValue(payloadHash);
                saveOperation.addBindValue(QString::fromUtf8(
                    QJsonDocument(result).toJson(QJsonDocument::Compact)));
                saveOperation.addBindValue(createdAt);
                if (!saveOperation.exec()) {
                    return rollback(sqlError(saveOperation));
                }
                batchResults.append(result);
                continue;
            }
            if (deleted) {
                return rollback(QStringLiteral("QSO_NOT_FOUND: QSO is deleted."));
            }
            if (expectedVersion != currentVersion) {
                return rollback(QStringLiteral("VERSION_CONFLICT: expected %1, current %2.")
                                    .arg(expectedVersion).arg(currentVersion));
            }
            if (stringValue(qso, QStringLiteral("station_uuid")) != currentStationUuid) {
                return rollback(QStringLiteral("INVALID_MESSAGE: QSO origin station is immutable."));
            }
        } else {
            existing.finish();
            if (kind != QStringLiteral("qso.create") || expectedVersion != 0) {
                return rollback(QStringLiteral("QSO_NOT_FOUND: QSO does not exist."));
            }
        }

        if (kind != QStringLiteral("qso.delete") && !hasDefinition) {
            QSqlQuery duplicate(m_database);
            duplicate.prepare(QStringLiteral(
                "SELECT qso_uuid FROM qsos WHERE contest_id = ? AND is_deleted = 0 "
                "AND callsign = ? COLLATE NOCASE AND band = ? COLLATE NOCASE "
                "AND mode = ? COLLATE NOCASE AND (? = 0 OR qso_uuid <> ?) LIMIT 1"));
            duplicate.addBindValue(contestId);
            duplicate.addBindValue(stringValue(qso, QStringLiteral("callsign")).trimmed());
            duplicate.addBindValue(stringValue(qso, QStringLiteral("band")).trimmed());
            duplicate.addBindValue(stringValue(qso, QStringLiteral("mode")).trimmed());
            duplicate.addBindValue(kind == QStringLiteral("qso.create") ? 0 : 1);
            duplicate.addBindValue(qsoUuid);
            if (!duplicate.exec()) {
                return rollback(sqlError(duplicate));
            }
            if (duplicate.next()) {
                return rollback(QStringLiteral("DUPLICATE_QSO: %1")
                                    .arg(duplicate.value(0).toString()));
            }
        }

        const int newVersion = kind == QStringLiteral("qso.create")
            ? 1 : currentVersion + 1;
        const bool deleted = kind == QStringLiteral("qso.delete");
        if (kind == QStringLiteral("qso.create")) {
            QSqlQuery insert(m_database);
            insert.prepare(QStringLiteral(
                "INSERT INTO qsos(contest_id, callsign, station_callsign, band, "
                "timestamp_utc, frequency_hz, mode, sent_exchange, received_exchange, "
                "sent_exchange_json, received_exchange_json, points, qso_uuid, "
                "station_uuid, qso_version, reservation_uuid) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?)"));
            insert.addBindValue(contestId);
            insert.addBindValue(stringValue(qso, QStringLiteral("callsign")).trimmed().toUpper());
            insert.addBindValue(stringValue(qso, QStringLiteral("station_callsign"))
                                    .trimmed().toUpper());
            insert.addBindValue(stringValue(qso, QStringLiteral("band")).trimmed().toLower());
            insert.addBindValue(qsoTime.toUTC().toString(Qt::ISODateWithMs));
            insert.addBindValue(static_cast<qint64>(frequencyNumber));
            insert.addBindValue(stringValue(qso, QStringLiteral("mode")).trimmed().toUpper());
            insert.addBindValue(stringValue(qso, QStringLiteral("sent_exchange")));
            insert.addBindValue(stringValue(qso, QStringLiteral("received_exchange")));
            insert.addBindValue(QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("sent_exchange_fields")).toObject())
                                                       .toJson(QJsonDocument::Compact)));
            insert.addBindValue(QString::fromUtf8(QJsonDocument(
                qso.value(QStringLiteral("received_exchange_fields")).toObject())
                                                       .toJson(QJsonDocument::Compact)));
            insert.addBindValue(static_cast<int>(pointsNumber));
            insert.addBindValue(qsoUuid);
            insert.addBindValue(stationUuid);
            insert.addBindValue(reservationUuid.isEmpty()
                ? QStringLiteral("") : reservationUuid);
            if (!insert.exec()) {
                return rollback(sqlError(insert));
            }
            qsoId = insert.lastInsertId().toLongLong();
            if (!reservationUuid.isEmpty()) {
                QSqlQuery consume(m_database);
                consume.prepare(QStringLiteral(
                    "UPDATE serial_reservations SET status = 'used', qso_uuid = ?, "
                    "updated_at_utc = ? WHERE reservation_uuid = ? AND status = 'reserved'"));
                consume.addBindValue(qsoUuid);
                consume.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
                consume.addBindValue(reservationUuid);
                if (!consume.exec()
                    || (consume.numRowsAffected() != 1
                        && consume.lastError().isValid())) {
                    return rollback(consume.lastError().isValid()
                        ? sqlError(consume)
                        : QStringLiteral("SERIAL_UNAVAILABLE: reservation was consumed concurrently."));
                }
            }
        } else {
            QSqlQuery update(m_database);
            if (deleted) {
                update.prepare(QStringLiteral(
                    "UPDATE qsos SET is_deleted = 1, qso_version = ? "
                    "WHERE id = ? AND qso_version = ? AND is_deleted = 0"));
                update.addBindValue(newVersion);
                update.addBindValue(qsoId);
                update.addBindValue(currentVersion);
            } else {
                update.prepare(QStringLiteral(
                    "UPDATE qsos SET callsign = ?, station_callsign = ?, band = ?, "
                    "timestamp_utc = ?, frequency_hz = ?, mode = ?, sent_exchange = ?, "
                    "received_exchange = ?, sent_exchange_json = ?, received_exchange_json = ?, "
                    "points = ?, qso_version = ? "
                    "WHERE id = ? AND qso_version = ? AND is_deleted = 0"));
                update.addBindValue(stringValue(qso, QStringLiteral("callsign"))
                                        .trimmed().toUpper());
                update.addBindValue(stringValue(qso, QStringLiteral("station_callsign"))
                                        .trimmed().toUpper());
                update.addBindValue(stringValue(qso, QStringLiteral("band"))
                                        .trimmed().toLower());
                update.addBindValue(qsoTime.toUTC().toString(Qt::ISODateWithMs));
                update.addBindValue(static_cast<qint64>(frequencyNumber));
                update.addBindValue(stringValue(qso, QStringLiteral("mode"))
                                        .trimmed().toUpper());
                update.addBindValue(stringValue(qso, QStringLiteral("sent_exchange")));
                update.addBindValue(stringValue(qso, QStringLiteral("received_exchange")));
                update.addBindValue(QString::fromUtf8(QJsonDocument(
                    qso.value(QStringLiteral("sent_exchange_fields")).toObject())
                                                           .toJson(QJsonDocument::Compact)));
                update.addBindValue(QString::fromUtf8(QJsonDocument(
                    qso.value(QStringLiteral("received_exchange_fields")).toObject())
                                                           .toJson(QJsonDocument::Compact)));
                update.addBindValue(static_cast<int>(pointsNumber));
                update.addBindValue(newVersion);
                update.addBindValue(qsoId);
                update.addBindValue(currentVersion);
            }
            if (!update.exec() || update.numRowsAffected() != 1) {
                return rollback(update.lastError().isValid()
                    ? sqlError(update)
                    : QStringLiteral("VERSION_CONFLICT: QSO changed during update."));
            }
        }

        QJsonObject snapshot = qsoSnapshot(qso, newVersion, deleted);
        QSqlQuery revisionQuery(m_database);
        revisionQuery.prepare(QStringLiteral(
            "SELECT current_revision FROM sync_log_state WHERE log_uuid = ?"));
        revisionQuery.addBindValue(logUuid);
        if (!revisionQuery.exec() || !revisionQuery.next()) {
            return rollback(revisionQuery.lastError().isValid()
                ? sqlError(revisionQuery)
                : QStringLiteral("Central log revision is unavailable."));
        }
        const qint64 revision = revisionQuery.value(0).toLongLong() + 1;
        revisionQuery.finish();
        const QString committedAt =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

        const QJsonObject operationResult{
            {QStringLiteral("operation_uuid"), operationUuid},
            {QStringLiteral("status"), QStringLiteral("accepted")},
            {QStringLiteral("qso_uuid"), qsoUuid},
            {QStringLiteral("qso_version"), newVersion},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("qso"), snapshot}
        };
        const QByteArray resultJson = QJsonDocument(operationResult)
            .toJson(QJsonDocument::Compact);
        QSqlQuery saveOperation(m_database);
        saveOperation.prepare(QStringLiteral(
            "INSERT INTO sync_operations(operation_uuid, log_uuid, station_uuid, "
            "payload_sha256, result_json, created_at_utc) VALUES(?, ?, ?, ?, ?, ?)"));
        saveOperation.addBindValue(operationUuid);
        saveOperation.addBindValue(logUuid);
        saveOperation.addBindValue(stationUuid);
        saveOperation.addBindValue(payloadHash);
        saveOperation.addBindValue(resultJson);
        saveOperation.addBindValue(createdAt);
        if (!saveOperation.exec()) {
            return rollback(sqlError(saveOperation));
        }

        QSqlQuery history(m_database);
        history.prepare(QStringLiteral(
            "INSERT INTO qso_history(log_uuid, qso_uuid, qso_version, operation_uuid, "
            "snapshot_json, is_deleted, created_at_utc, revision) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
        history.addBindValue(logUuid);
        history.addBindValue(qsoUuid);
        history.addBindValue(newVersion);
        history.addBindValue(operationUuid);
        history.addBindValue(QString::fromUtf8(QJsonDocument(snapshot)
                                                  .toJson(QJsonDocument::Compact)));
        history.addBindValue(deleted ? 1 : 0);
        history.addBindValue(createdAt);
        history.addBindValue(revision);
        if (!history.exec()) {
            return rollback(sqlError(history));
        }

        QSqlQuery event(m_database);
        event.prepare(QStringLiteral(
            "INSERT INTO sync_events(event_uuid, log_uuid, revision, operation_uuid, "
            "kind, entity_uuid, payload_json, created_at_utc) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
        event.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
        event.addBindValue(logUuid);
        event.addBindValue(revision);
        event.addBindValue(operationUuid);
        event.addBindValue(kind);
        event.addBindValue(qsoUuid);
        event.addBindValue(QString::fromUtf8(QJsonDocument(snapshot)
                                                .toJson(QJsonDocument::Compact)));
        event.addBindValue(committedAt);
        if (!event.exec()) {
            return rollback(sqlError(event));
        }

        QSqlQuery updateRevision(m_database);
        updateRevision.prepare(QStringLiteral(
            "UPDATE sync_log_state SET current_revision = ? "
            "WHERE log_uuid = ? AND current_revision = ?"));
        updateRevision.addBindValue(revision);
        updateRevision.addBindValue(logUuid);
        updateRevision.addBindValue(revision - 1);
        if (!updateRevision.exec() || updateRevision.numRowsAffected() != 1) {
            return rollback(updateRevision.lastError().isValid()
                ? sqlError(updateRevision)
                : QStringLiteral("Central log revision changed concurrently."));
        }
        batchResults.append(operationResult);
    }

    if (!m_database.commit()) {
        return rollback(m_database.lastError().text());
    }
    *results = batchResults;
    return true;
}

QJsonObject CentralRepository::reserveSerial(
    const QString &logUuid, const QString &stationUuid,
    const QString &reservationUuid, const QString &requestedBand,
    const QString &requestedMode, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(stationUuid)
        || !isUuid(reservationUuid)) {
        setError(error, QStringLiteral("Serial reservation identity is invalid."));
        return {};
    }
    const QString band = requestedBand.trimmed().toLower();
    const QString mode = requestedMode.trimmed().toUpper();
    QSqlQuery log(m_database);
    log.prepare(QStringLiteral(
        "SELECT id, definition_yaml FROM contests WHERE log_uuid = ?"));
    log.addBindValue(logUuid);
    if (!log.exec() || !log.next()) {
        setError(error, log.lastError().isValid()
            ? sqlError(log) : QStringLiteral("UNKNOWN_LOG: central log does not exist."));
        return {};
    }
    const qint64 contestId = log.value(0).toLongLong();
    const QString yaml = log.value(1).toString();
    log.finish();

    ContestDefinition definition;
    if (!ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("central contest definition"),
            definition, error)) {
        return {};
    }
    QString serialField;
    int serialWidth = 0;
    for (const ExchangeFieldDefinition &field : definition.sentExchange) {
        if (field.type == ExchangeFieldType::Serial && field.generated) {
            serialField = field.name;
            serialWidth = field.width;
            break;
        }
    }
    if (serialField.isEmpty()) {
        setError(error, QStringLiteral(
            "SERIAL_UNAVAILABLE: contest does not define a generated sent serial."));
        return {};
    }
    if ((definition.serialScope == QStringLiteral("band") && band.isEmpty())
        || (definition.serialScope == QStringLiteral("mode") && mode.isEmpty())
        || (definition.serialScope == QStringLiteral("band_mode")
            && (band.isEmpty() || mode.isEmpty()))) {
        setError(error, QStringLiteral("Serial reservation requires band and/or mode."));
        return {};
    }
    QString scopeKey = QStringLiteral("global");
    if (definition.serialScope == QStringLiteral("band")) {
        scopeKey = QStringLiteral("band:") + band;
    } else if (definition.serialScope == QStringLiteral("mode")) {
        scopeKey = QStringLiteral("mode:") + mode;
    } else if (definition.serialScope == QStringLiteral("band_mode")) {
        scopeKey = QStringLiteral("band:") + band
            + QStringLiteral("|mode:") + mode;
    }
    const auto resultObject = [&](int serial, const QString &status,
                                  const QString &qsoUuid) {
        return QJsonObject{
            {QStringLiteral("reservation_uuid"), reservationUuid},
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("band"), band},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("serial"), serial},
            {QStringLiteral("qso_uuid"), qsoUuid},
            {QStringLiteral("formatted_serial"),
             QString::number(serial).rightJustified(serialWidth, QLatin1Char('0'))},
            {QStringLiteral("scope_key"), scopeKey},
            {QStringLiteral("status"), status}
        };
    };

    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return {};
    }
    const auto rollback = [this, error](const QString &message) {
        QString failure = message;
        if (!m_database.rollback()) {
            failure += QStringLiteral("; rollback failed: ")
                + m_database.lastError().text();
        }
        setError(error, failure);
        return QJsonObject{};
    };
    QSqlQuery previous(m_database);
    previous.prepare(QStringLiteral(
        "SELECT station_uuid, scope_key, band, mode, serial_number, status, qso_uuid "
        "FROM serial_reservations WHERE reservation_uuid = ?"));
    previous.addBindValue(reservationUuid);
    if (!previous.exec()) {
        return rollback(sqlError(previous));
    }
    if (previous.next()) {
        if (previous.value(0).toString() != stationUuid
            || previous.value(1).toString() != scopeKey
            || previous.value(2).toString() != band
            || previous.value(3).toString() != mode) {
            return rollback(QStringLiteral(
                "IDEMPOTENCY_KEY_REUSED: reservation UUID has different parameters."));
        }
        const int serial = previous.value(4).toInt();
        const QString status = previous.value(5).toString();
        const QString qsoUuid = previous.value(6).toString();
        previous.finish();
        if (!m_database.commit()) {
            return rollback(m_database.lastError().text());
        }
        return resultObject(serial, status, qsoUuid);
    }
    previous.finish();

    int maximum = definition.serialStart - 1;
    QSqlQuery reserved(m_database);
    reserved.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(serial_number), 0) FROM serial_reservations "
        "WHERE log_uuid = ? AND scope_key = ?"));
    reserved.addBindValue(logUuid);
    reserved.addBindValue(scopeKey);
    if (!reserved.exec() || !reserved.next()) {
        return rollback(sqlError(reserved));
    }
    maximum = qMax(maximum, reserved.value(0).toInt());
    reserved.finish();

    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral(
        "SELECT band, mode, sent_exchange_json FROM qsos WHERE contest_id = ?"));
    existing.addBindValue(contestId);
    if (!existing.exec()) {
        return rollback(sqlError(existing));
    }
    while (existing.next()) {
        const QString existingBand = existing.value(0).toString().trimmed().toLower();
        const QString existingMode = existing.value(1).toString().trimmed().toUpper();
        QString existingScope = QStringLiteral("global");
        if (definition.serialScope == QStringLiteral("band")) {
            existingScope = QStringLiteral("band:") + existingBand;
        } else if (definition.serialScope == QStringLiteral("mode")) {
            existingScope = QStringLiteral("mode:") + existingMode;
        } else if (definition.serialScope == QStringLiteral("band_mode")) {
            existingScope = QStringLiteral("band:") + existingBand
                + QStringLiteral("|mode:") + existingMode;
        }
        if (existingScope != scopeKey) {
            continue;
        }
        const QJsonObject fields = QJsonDocument::fromJson(
            existing.value(2).toByteArray()).object();
        bool serialOk = false;
        const int serial = fields.value(serialField).toString().toInt(&serialOk);
        if (serialOk) {
            maximum = qMax(maximum, serial);
        }
    }
    existing.finish();
    if (maximum == std::numeric_limits<int>::max()) {
        return rollback(QStringLiteral(
            "SERIAL_UNAVAILABLE: serial number range is exhausted."));
    }
    const int serial = qMax(definition.serialStart, maximum + 1);
    const QString now = utcString(QDateTime::currentDateTimeUtc());
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO serial_reservations(reservation_uuid, log_uuid, station_uuid, "
        "scope_key, band, mode, serial_number, status, qso_uuid, created_at_utc, "
        "updated_at_utc) VALUES(?, ?, ?, ?, ?, ?, ?, 'reserved', '', ?, ?)"));
    insert.addBindValue(reservationUuid);
    insert.addBindValue(logUuid);
    insert.addBindValue(stationUuid);
    insert.addBindValue(scopeKey);
    insert.addBindValue(band);
    insert.addBindValue(mode);
    insert.addBindValue(serial);
    insert.addBindValue(now);
    insert.addBindValue(now);
    if (!insert.exec()) {
        return rollback(sqlError(insert));
    }
    if (!m_database.commit()) {
        return rollback(m_database.lastError().text());
    }
    return resultObject(serial, QStringLiteral("reserved"), {});
}

bool CentralRepository::cancelSerialReservation(
    const QString &logUuid, const QString &stationUuid,
    const QString &reservationUuid, bool administrator, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(stationUuid)
        || !isUuid(reservationUuid)) {
        setError(error, QStringLiteral("Serial reservation identity is invalid."));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT station_uuid, status FROM serial_reservations "
        "WHERE reservation_uuid = ? AND log_uuid = ?"));
    query.addBindValue(reservationUuid);
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return false;
    }
    if (!query.next()) {
        setError(error, QStringLiteral("SERIAL_UNAVAILABLE: reservation does not exist."));
        return false;
    }
    const QString owner = query.value(0).toString();
    const QString status = query.value(1).toString();
    query.finish();
    if (!administrator && owner != stationUuid) {
        setError(error, QStringLiteral("FORBIDDEN: reservation belongs to another station."));
        return false;
    }
    if (status == QStringLiteral("cancelled")) {
        return true;
    }
    if (status != QStringLiteral("reserved")) {
        setError(error, QStringLiteral("SERIAL_UNAVAILABLE: used reservation cannot be cancelled."));
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE serial_reservations SET status = 'cancelled', updated_at_utc = ? "
        "WHERE reservation_uuid = ? AND status = 'reserved'"));
    update.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    update.addBindValue(reservationUuid);
    if (!update.exec() || update.numRowsAffected() != 1) {
        setError(error, update.lastError().isValid()
            ? sqlError(update)
            : QStringLiteral("Serial reservation changed concurrently."));
        return false;
    }
    return true;
}

bool CentralRepository::adjustSerialReservation(
    const QString &logUuid, const QString &administratorStationUuid,
    const QString &reservationUuid, int expectedSerial, int correctedSerial,
    const QString &reason, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    const QString normalizedReason = reason.trimmed();
    if (!m_database.isOpen() || !isUuid(logUuid)
        || !isUuid(administratorStationUuid) || !isUuid(reservationUuid)
        || expectedSerial < 1 || correctedSerial < 1
        || expectedSerial == correctedSerial || normalizedReason.isEmpty()
        || normalizedReason.size() > 500) {
        setError(error, QStringLiteral("Serial correction request is invalid."));
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    const auto fail = [this, error](const QString &message) {
        setError(error, rollbackFailure(m_database, message));
        return false;
    };

    QSqlQuery reservation(m_database);
    reservation.prepare(QStringLiteral(
        "SELECT scope_key, serial_number, status FROM serial_reservations "
        "WHERE log_uuid = ? AND reservation_uuid = ?"));
    reservation.addBindValue(logUuid);
    reservation.addBindValue(reservationUuid);
    if (!reservation.exec()) {
        return fail(sqlError(reservation));
    }
    if (!reservation.next()) {
        return fail(QStringLiteral(
            "SERIAL_UNAVAILABLE: reservation does not exist."));
    }
    const QString scopeKey = reservation.value(0).toString();
    const int currentSerial = reservation.value(1).toInt();
    const QString status = reservation.value(2).toString();
    reservation.finish();
    if (status != QStringLiteral("reserved")) {
        return fail(QStringLiteral(
            "SERIAL_UNAVAILABLE: only unused reservations can be corrected."));
    }
    if (currentSerial != expectedSerial) {
        return fail(QStringLiteral(
            "SERIAL_CHANGED: reservation number no longer matches confirmation."));
    }

    QSqlQuery log(m_database);
    log.prepare(QStringLiteral(
        "SELECT definition_yaml FROM contests WHERE log_uuid = ?"));
    log.addBindValue(logUuid);
    if (!log.exec() || !log.next()) {
        return fail(log.lastError().isValid()
            ? sqlError(log) : QStringLiteral("UNKNOWN_LOG: central log does not exist."));
    }
    ContestDefinition definition;
    const QString yaml = log.value(0).toString();
    log.finish();
    QString definitionError;
    if (yaml.trimmed().isEmpty()
        || !ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("serial correction"),
            definition, &definitionError)) {
        return fail(definitionError.isEmpty()
            ? QStringLiteral("SERIAL_UNAVAILABLE: contest definition is required.")
            : definitionError);
    }
    if (correctedSerial < definition.serialStart) {
        return fail(QStringLiteral(
            "SERIAL_UNAVAILABLE: corrected number is below the contest start."));
    }
    QSqlQuery duplicate(m_database);
    duplicate.prepare(QStringLiteral(
        "SELECT 1 FROM serial_reservations WHERE log_uuid = ? AND scope_key = ? "
        "AND serial_number = ? AND reservation_uuid <> ?"));
    duplicate.addBindValue(logUuid);
    duplicate.addBindValue(scopeKey);
    duplicate.addBindValue(correctedSerial);
    duplicate.addBindValue(reservationUuid);
    if (!duplicate.exec()) {
        return fail(sqlError(duplicate));
    }
    if (duplicate.next()) {
        return fail(QStringLiteral(
            "SERIAL_UNAVAILABLE: corrected number is already reserved or consumed."));
    }
    duplicate.finish();

    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE serial_reservations SET serial_number = ?, updated_at_utc = ? "
        "WHERE log_uuid = ? AND reservation_uuid = ? AND serial_number = ? "
        "AND status = 'reserved'"));
    update.addBindValue(correctedSerial);
    update.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    update.addBindValue(logUuid);
    update.addBindValue(reservationUuid);
    update.addBindValue(expectedSerial);
    if (!update.exec() || update.numRowsAffected() != 1) {
        return fail(update.lastError().isValid()
            ? sqlError(update)
            : QStringLiteral("Serial reservation changed concurrently."));
    }

    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral(
        "INSERT INTO serial_reservation_adjustments(log_uuid, reservation_uuid, "
        "administrator_station_uuid, previous_serial, corrected_serial, reason, "
        "created_at_utc) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    audit.addBindValue(logUuid);
    audit.addBindValue(reservationUuid);
    audit.addBindValue(administratorStationUuid);
    audit.addBindValue(expectedSerial);
    audit.addBindValue(correctedSerial);
    audit.addBindValue(normalizedReason);
    audit.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    if (!audit.exec()) {
        return fail(sqlError(audit));
    }
    if (!m_database.commit()) {
        return fail(m_database.lastError().text());
    }
    return true;
}

bool CentralRepository::importSerialReservations(
    const QString &logUuid, const QJsonArray &reservations, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid)
        || reservations.size() > 1000) {
        setError(error, QStringLiteral("Serial reservation import is invalid."));
        return false;
    }
    if (reservations.isEmpty()) {
        return true;
    }
    QSqlQuery log(m_database);
    log.prepare(QStringLiteral(
        "SELECT definition_yaml FROM contests WHERE log_uuid = ?"));
    log.addBindValue(logUuid);
    if (!log.exec() || !log.next()) {
        setError(error, log.lastError().isValid()
            ? sqlError(log)
            : QStringLiteral("Central log does not exist."));
        return false;
    }
    ContestDefinition definition;
    const QString yaml = log.value(0).toString();
    log.finish();
    QString definitionError;
    if (yaml.trimmed().isEmpty()
        || !ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("serial history import"),
            definition, &definitionError)) {
        setError(error, definitionError.isEmpty()
            ? QStringLiteral("Central contest definition is required for serial history.")
            : definitionError);
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    const auto fail = [this, error](const QString &message) {
        setError(error, rollbackFailure(m_database, message));
        return false;
    };
    QSet<QString> seenReservations;
    for (const QJsonValue &value : reservations) {
        if (!value.isObject()) {
            return fail(QStringLiteral("Serial history entry must be an object."));
        }
        const QJsonObject item = value.toObject();
        const QStringList expectedKeys{
            QStringLiteral("band"), QStringLiteral("mode"),
            QStringLiteral("qso_uuid"), QStringLiteral("reservation_uuid"),
            QStringLiteral("scope_key"), QStringLiteral("serial"),
            QStringLiteral("station_uuid"), QStringLiteral("status")
        };
        const QString reservationUuid =
            item.value(QStringLiteral("reservation_uuid")).toString();
        const QString stationUuid =
            item.value(QStringLiteral("station_uuid")).toString();
        const QString scope = item.value(QStringLiteral("scope_key")).toString();
        const QString band = item.value(QStringLiteral("band")).toString()
                                 .trimmed().toLower();
        const QString mode = item.value(QStringLiteral("mode")).toString()
                                 .trimmed().toUpper();
        const QString status = item.value(QStringLiteral("status")).toString();
        const QString qsoUuid = item.value(QStringLiteral("qso_uuid")).toString();
        const QJsonValue serialValue = item.value(QStringLiteral("serial"));
        const int serial = serialValue.toInt(-1);
        QString expectedScope = QStringLiteral("global");
        if (definition.serialScope == QStringLiteral("band")) {
            expectedScope = QStringLiteral("band:") + band;
        } else if (definition.serialScope == QStringLiteral("mode")) {
            expectedScope = QStringLiteral("mode:") + mode;
        } else if (definition.serialScope == QStringLiteral("band_mode")) {
            expectedScope = QStringLiteral("band:") + band
                + QStringLiteral("|mode:") + mode;
        }
        if (item.keys() != expectedKeys || !isUuid(reservationUuid)
            || !isUuid(stationUuid) || !serialValue.isDouble() || serial < 1
            || seenReservations.contains(reservationUuid)
            || (status != QStringLiteral("reserved")
                && status != QStringLiteral("used")
                && status != QStringLiteral("cancelled"))
            || (status == QStringLiteral("used")
                ? !isUuid(qsoUuid) : !qsoUuid.isEmpty())
            || scope != expectedScope
            || (definition.serialServerRequired && serial < definition.serialStart)
            || (status != QStringLiteral("cancelled")
                && (band.isEmpty() || mode.isEmpty()))) {
            return fail(QStringLiteral(
                "Serial history entry has invalid identity, state or scope."));
        }
        seenReservations.insert(reservationUuid);

        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral(
            "SELECT log_uuid, station_uuid, scope_key, band, mode, serial_number, "
            "status, qso_uuid FROM serial_reservations WHERE reservation_uuid = ?"));
        existing.addBindValue(reservationUuid);
        if (!existing.exec()) {
            return fail(sqlError(existing));
        }
        if (existing.next()) {
            const bool same = existing.value(0).toString() == logUuid
                && existing.value(1).toString() == stationUuid
                && existing.value(2).toString() == scope
                && existing.value(3).toString() == band
                && existing.value(4).toString() == mode
                && existing.value(5).toInt() == serial
                && existing.value(6).toString() == status
                && existing.value(7).toString() == qsoUuid;
            existing.finish();
            if (!same) {
                return fail(QStringLiteral(
                    "SERIAL_HISTORY_CONFLICT: reservation identity or state differs."));
            }
            continue;
        }
        existing.finish();
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral(
            "INSERT INTO serial_reservations(reservation_uuid, log_uuid, station_uuid, "
            "scope_key, band, mode, serial_number, status, qso_uuid, created_at_utc, "
            "updated_at_utc) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        const QString now = utcString(QDateTime::currentDateTimeUtc());
        insert.addBindValue(reservationUuid);
        insert.addBindValue(logUuid);
        insert.addBindValue(stationUuid);
        insert.addBindValue(scope);
        insert.addBindValue(band);
        insert.addBindValue(mode);
        insert.addBindValue(serial);
        insert.addBindValue(status);
        insert.addBindValue(qsoUuid);
        insert.addBindValue(now);
        insert.addBindValue(now);
        if (!insert.exec()) {
            return fail(sqlError(insert));
        }
    }
    if (!m_database.commit()) {
        return fail(m_database.lastError().text());
    }
    return true;
}

QJsonArray CentralRepository::serialReservations(
    const QString &logUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    QJsonArray result;
    if (!m_database.isOpen() || !isUuid(logUuid)) {
        setError(error, QStringLiteral("Log UUID is invalid."));
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT reservation_uuid, station_uuid, scope_key, band, mode, "
        "serial_number, status, qso_uuid, created_at_utc, updated_at_utc "
        "FROM serial_reservations WHERE log_uuid = ? ORDER BY serial_number"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return result;
    }
    while (query.next()) {
        result.append(QJsonObject{
            {QStringLiteral("reservation_uuid"), query.value(0).toString()},
            {QStringLiteral("station_uuid"), query.value(1).toString()},
            {QStringLiteral("scope_key"), query.value(2).toString()},
            {QStringLiteral("band"), query.value(3).toString()},
            {QStringLiteral("mode"), query.value(4).toString()},
            {QStringLiteral("serial"), query.value(5).toInt()},
            {QStringLiteral("status"), query.value(6).toString()},
            {QStringLiteral("qso_uuid"), query.value(7).toString()},
            {QStringLiteral("created_at_utc"), query.value(8).toString()},
            {QStringLiteral("updated_at_utc"), query.value(9).toString()}
        });
    }
    return result;
}

QJsonObject CentralRepository::synchronizationMetrics(
    const QString &logUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid)) {
        setError(error, QStringLiteral("Log UUID is invalid."));
        return {};
    }
    QSqlQuery log(m_database);
    log.prepare(QStringLiteral("SELECT 1 FROM contests WHERE log_uuid = ?"));
    log.addBindValue(logUuid);
    if (!log.exec()) {
        setError(error, sqlError(log));
        return {};
    }
    if (!log.next()) {
        setError(error, QStringLiteral("UNKNOWN_LOG: %1").arg(logUuid));
        return {};
    }
    log.finish();

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT "
        "COALESCE((SELECT current_revision FROM sync_log_state "
        "WHERE log_uuid = ?), 0), "
        "(SELECT COUNT(*) FROM sync_events WHERE log_uuid = ?), "
        "(SELECT COUNT(*) FROM sync_operations WHERE log_uuid = ?), "
        "(SELECT COUNT(*) FROM sync_clients WHERE log_uuid = ?), "
        "COALESCE((SELECT MAX(MAX(0, state.current_revision - clients.cursor)) "
        "FROM sync_clients clients JOIN sync_log_state state "
        "ON state.log_uuid = clients.log_uuid WHERE clients.log_uuid = ?), 0), "
        "(SELECT COUNT(*) FROM qsos WHERE contest_id = "
        "(SELECT id FROM contests WHERE log_uuid = ?) AND is_deleted = 0), "
        "(SELECT COUNT(*) FROM serial_reservations "
        "WHERE log_uuid = ? AND status = 'reserved'), "
        "(SELECT COUNT(*) FROM serial_reservations "
        "WHERE log_uuid = ? AND status = 'used'), "
        "(SELECT COUNT(*) FROM serial_reservations "
        "WHERE log_uuid = ? AND status = 'cancelled')"));
    for (int index = 0; index < 9; ++index) {
        query.addBindValue(logUuid);
    }
    if (!query.exec() || !query.next()) {
        setError(error, query.lastError().isValid()
            ? sqlError(query)
            : QStringLiteral("Could not read synchronization metrics."));
        return {};
    }
    QJsonObject metrics{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("current_revision"), query.value(0).toLongLong()},
        {QStringLiteral("event_count"), query.value(1).toLongLong()},
        {QStringLiteral("operation_count"), query.value(2).toLongLong()},
        {QStringLiteral("registered_station_count"), query.value(3).toLongLong()},
        {QStringLiteral("maximum_cursor_lag"), query.value(4).toLongLong()},
        {QStringLiteral("active_qso_count"), query.value(5).toLongLong()},
        {QStringLiteral("reserved_serial_count"), query.value(6).toLongLong()},
        {QStringLiteral("used_serial_count"), query.value(7).toLongLong()},
        {QStringLiteral("cancelled_serial_count"), query.value(8).toLongLong()}
    };
    const qint64 currentRevision = query.value(0).toLongLong();

    QJsonArray clients;
    qint64 totalPending = 0;
    qint64 totalErrors = 0;
    qint64 totalConflicts = 0;
    QSqlQuery clientQuery(m_database);
    clientQuery.prepare(QStringLiteral(
        "SELECT station_uuid, cursor, last_seen_at_utc, pending_count, "
        "error_count, conflict_count FROM sync_clients WHERE log_uuid = ? "
        "ORDER BY station_uuid"));
    clientQuery.addBindValue(logUuid);
    if (!clientQuery.exec()) {
        setError(error, sqlError(clientQuery));
        return {};
    }
    while (clientQuery.next()) {
        const qint64 pending = clientQuery.value(3).toLongLong();
        const qint64 errors = clientQuery.value(4).toLongLong();
        const qint64 conflicts = clientQuery.value(5).toLongLong();
        totalPending += pending;
        totalErrors += errors;
        totalConflicts += conflicts;
        clients.append(QJsonObject{
            {QStringLiteral("station_uuid"), clientQuery.value(0).toString()},
            {QStringLiteral("cursor"), clientQuery.value(1).toLongLong()},
            {QStringLiteral("cursor_lag"), qMax(
                 qint64(0), currentRevision - clientQuery.value(1).toLongLong())},
            {QStringLiteral("last_contact_utc"), clientQuery.value(2).toString()},
            {QStringLiteral("pending_count"), pending},
            {QStringLiteral("error_count"), errors},
            {QStringLiteral("conflict_count"), conflicts}
        });
    }
    metrics.insert(QStringLiteral("clients"), clients);
    metrics.insert(QStringLiteral("pending_count"), totalPending);
    metrics.insert(QStringLiteral("error_count"), totalErrors);
    metrics.insert(QStringLiteral("conflict_count"), totalConflicts);

    QSqlQuery transactionSummary(m_database);
    transactionSummary.prepare(QStringLiteral(
        "SELECT COUNT(*), COALESCE(AVG(duration_ms), 0), "
        "COALESCE(MAX(duration_ms), 0), "
        "SUM(CASE WHEN outcome = 'error' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN outcome = 'conflict' THEN 1 ELSE 0 END) "
        "FROM sync_transaction_metrics WHERE log_uuid = ?"));
    transactionSummary.addBindValue(logUuid);
    if (!transactionSummary.exec() || !transactionSummary.next()) {
        setError(error, transactionSummary.lastError().isValid()
            ? sqlError(transactionSummary)
            : QStringLiteral("Could not read synchronization transaction metrics."));
        return {};
    }
    metrics.insert(QStringLiteral("transaction_count"),
                   transactionSummary.value(0).toLongLong());
    metrics.insert(QStringLiteral("transaction_average_ms"),
                   transactionSummary.value(1).toDouble());
    metrics.insert(QStringLiteral("transaction_maximum_ms"),
                   transactionSummary.value(2).toLongLong());
    metrics.insert(QStringLiteral("transaction_error_count"),
                   transactionSummary.value(3).toLongLong());
    metrics.insert(QStringLiteral("transaction_conflict_count"),
                   transactionSummary.value(4).toLongLong());

    QJsonArray transactionHistory;
    QSqlQuery history(m_database);
    history.prepare(QStringLiteral(
        "SELECT station_uuid, completed_at_utc, duration_ms, outcome, "
        "operation_count FROM sync_transaction_metrics WHERE log_uuid = ? "
        "ORDER BY id DESC LIMIT 100"));
    history.addBindValue(logUuid);
    if (!history.exec()) {
        setError(error, sqlError(history));
        return {};
    }
    while (history.next()) {
        transactionHistory.append(QJsonObject{
            {QStringLiteral("station_uuid"), history.value(0).toString()},
            {QStringLiteral("completed_at_utc"), history.value(1).toString()},
            {QStringLiteral("duration_ms"), history.value(2).toLongLong()},
            {QStringLiteral("outcome"), history.value(3).toString()},
            {QStringLiteral("operation_count"), history.value(4).toInt()}
        });
    }
    metrics.insert(QStringLiteral("transaction_history"), transactionHistory);
    return metrics;
}

bool CentralRepository::recordSyncTransactionMetric(
    const QString &logUuid, const QString &stationUuid, qint64 durationMs,
    const QString &outcome, int operationCount, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(stationUuid)
        || durationMs < 0 || operationCount < 0
        || !QStringList{QStringLiteral("success"), QStringLiteral("error"),
                        QStringLiteral("conflict")}.contains(outcome)) {
        setError(error, QStringLiteral("Synchronization transaction metric is invalid."));
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO sync_transaction_metrics(log_uuid, station_uuid, "
        "completed_at_utc, duration_ms, outcome, operation_count) "
        "VALUES(?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(logUuid);
    insert.addBindValue(stationUuid);
    insert.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    insert.addBindValue(durationMs);
    insert.addBindValue(outcome);
    insert.addBindValue(operationCount);
    if (!insert.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(insert)));
        return false;
    }
    QSqlQuery trim(m_database);
    trim.prepare(QStringLiteral(
        "DELETE FROM sync_transaction_metrics WHERE log_uuid = ? AND id NOT IN "
        "(SELECT id FROM sync_transaction_metrics WHERE log_uuid = ? "
        "ORDER BY id DESC LIMIT 1000)"));
    trim.addBindValue(logUuid);
    trim.addBindValue(logUuid);
    if (!trim.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(trim)));
        return false;
    }
    if (!m_database.commit()) {
        setError(error, rollbackFailure(m_database, m_database.lastError().text()));
        return false;
    }
    return true;
}

QString CentralRepository::beginLogImport(
    const QString &logUuid, const QString &sourceLogUuid,
    const QString &sourceLogName, const QString &stationUuid, int totalQso,
    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(sourceLogUuid)
        || !isUuid(stationUuid) || sourceLogName.trimmed().isEmpty()
        || totalQso < 0) {
        setError(error, QStringLiteral("Log import identity or QSO count is invalid."));
        return {};
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return {};
    }
    const QString now = utcString(QDateTime::currentDateTimeUtc());
    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral(
        "SELECT import_uuid FROM sync_import_audit "
        "WHERE log_uuid = ? AND source_log_uuid = ? AND station_uuid = ? "
        "AND status = 'in_progress' ORDER BY updated_at_utc DESC LIMIT 1"));
    existing.addBindValue(logUuid);
    existing.addBindValue(sourceLogUuid);
    existing.addBindValue(stationUuid);
    if (!existing.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(existing)));
        return {};
    }
    QString importUuid;
    if (existing.next()) {
        importUuid = existing.value(0).toString();
    }
    existing.finish();

    QSqlQuery persist(m_database);
    if (importUuid.isEmpty()) {
        importUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        persist.prepare(QStringLiteral(
            "INSERT INTO sync_import_audit(import_uuid, log_uuid, source_log_uuid, "
            "source_log_name, station_uuid, total_qso, status, created_at_utc, "
            "updated_at_utc) VALUES(?, ?, ?, ?, ?, ?, 'in_progress', ?, ?)"));
        persist.addBindValue(importUuid);
        persist.addBindValue(logUuid);
        persist.addBindValue(sourceLogUuid);
        persist.addBindValue(sourceLogName.trimmed());
        persist.addBindValue(stationUuid);
        persist.addBindValue(totalQso);
        persist.addBindValue(now);
        persist.addBindValue(now);
    } else {
        persist.prepare(QStringLiteral(
            "UPDATE sync_import_audit SET source_log_name = ?, total_qso = ?, "
            "updated_at_utc = ? WHERE import_uuid = ? AND status = 'in_progress'"));
        persist.addBindValue(sourceLogName.trimmed());
        persist.addBindValue(totalQso);
        persist.addBindValue(now);
        persist.addBindValue(importUuid);
    }
    if (!persist.exec() || persist.numRowsAffected() != 1) {
        setError(error, rollbackFailure(m_database,
            persist.lastError().isValid()
                ? sqlError(persist)
                : QStringLiteral("Could not persist log import audit.")));
        return {};
    }
    if (!m_database.commit()) {
        setError(error, rollbackFailure(m_database, m_database.lastError().text()));
        return {};
    }
    return importUuid;
}

bool CentralRepository::updateLogImport(
    const QString &importUuid, int importedQso, int skippedQso,
    const QString &status, const QString &errorMessage, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    const QStringList allowedStatuses{
        QStringLiteral("in_progress"), QStringLiteral("completed"),
        QStringLiteral("conflict"), QStringLiteral("failed"),
        QStringLiteral("cancelled")
    };
    if (!m_database.isOpen() || !isUuid(importUuid) || importedQso < 0
        || skippedQso < 0 || !allowedStatuses.contains(status)) {
        setError(error, QStringLiteral("Log import audit update is invalid."));
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE sync_import_audit SET imported_qso = ?, skipped_qso = ?, "
        "status = ?, error_message = ?, updated_at_utc = ? "
        "WHERE import_uuid = ? AND ? + ? <= total_qso"));
    update.addBindValue(importedQso);
    update.addBindValue(skippedQso);
    update.addBindValue(status);
    update.addBindValue(errorMessage.isNull() ? QStringLiteral("") : errorMessage);
    update.addBindValue(utcString(QDateTime::currentDateTimeUtc()));
    update.addBindValue(importUuid);
    update.addBindValue(importedQso);
    update.addBindValue(skippedQso);
    if (!update.exec()) {
        setError(error, sqlError(update));
        return false;
    }
    if (update.numRowsAffected() != 1) {
        setError(error, QStringLiteral("Log import audit does not exist or counts exceed total."));
        return false;
    }
    return true;
}

QJsonObject CentralRepository::logImportAudit(
    const QString &importUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(importUuid)) {
        setError(error, QStringLiteral("Log import UUID is invalid."));
        return {};
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT import_uuid, log_uuid, source_log_uuid, source_log_name, "
        "station_uuid, total_qso, imported_qso, skipped_qso, status, "
        "error_message, created_at_utc, updated_at_utc "
        "FROM sync_import_audit WHERE import_uuid = ?"));
    query.addBindValue(importUuid);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return {};
    }
    if (!query.next()) {
        setError(error, QStringLiteral("Log import audit record does not exist."));
        return {};
    }
    return QJsonObject{
        {QStringLiteral("import_uuid"), query.value(0).toString()},
        {QStringLiteral("log_uuid"), query.value(1).toString()},
        {QStringLiteral("source_log_uuid"), query.value(2).toString()},
        {QStringLiteral("source_log_name"), query.value(3).toString()},
        {QStringLiteral("station_uuid"), query.value(4).toString()},
        {QStringLiteral("total_qso"), query.value(5).toInt()},
        {QStringLiteral("imported_qso"), query.value(6).toInt()},
        {QStringLiteral("skipped_qso"), query.value(7).toInt()},
        {QStringLiteral("status"), query.value(8).toString()},
        {QStringLiteral("error_message"), query.value(9).toString()},
        {QStringLiteral("created_at_utc"), query.value(10).toString()},
        {QStringLiteral("updated_at_utc"), query.value(11).toString()}
    };
}

QJsonArray CentralRepository::eventsSince(
    const QString &logUuid, qint64 afterRevision, int limit, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    QJsonArray result;
    if (!m_database.isOpen() || !isUuid(logUuid) || afterRevision < 0
        || limit < 1 || limit > 500) {
        setError(error, QStringLiteral("Log UUID, cursor, or event page size is invalid."));
        return result;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT event_uuid, log_uuid, revision, operation_uuid, kind, entity_uuid, "
        "payload_json, created_at_utc FROM sync_events "
        "WHERE log_uuid = ? AND revision > ? ORDER BY revision LIMIT ?"));
    query.addBindValue(logUuid);
    query.addBindValue(afterRevision);
    query.addBindValue(limit);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return result;
    }
    while (query.next()) {
        const QJsonDocument payload = QJsonDocument::fromJson(
            query.value(6).toByteArray());
        if (!payload.isObject()) {
            setError(error, QStringLiteral("Stored event payload is invalid."));
            return {};
        }
        result.append(QJsonObject{
            {QStringLiteral("event_uuid"), query.value(0).toString()},
            {QStringLiteral("log_uuid"), query.value(1).toString()},
            {QStringLiteral("revision"), query.value(2).toLongLong()},
            {QStringLiteral("operation_uuid"), query.value(3).toString()},
            {QStringLiteral("kind"), query.value(4).toString()},
            {QStringLiteral("entity_uuid"), query.value(5).toString()},
            {QStringLiteral("payload"), payload.object()},
            {QStringLiteral("created_at_utc"), query.value(7).toString()}
        });
    }
    return result;
}

bool CentralRepository::compactSyncEvents(
    const QString &logUuid, const QDateTime &olderThanUtc,
    int *removedEvents, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (removedEvents != nullptr) {
        *removedEvents = 0;
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !olderThanUtc.isValid()) {
        setError(error, QStringLiteral("Log UUID or event retention cutoff is invalid."));
        return false;
    }
    const QString cutoff = olderThanUtc.toUTC().toString(Qt::ISODateWithMs);
    QString revisionError;
    const qint64 revision = currentRevision(logUuid, &revisionError);
    if (!revisionError.isEmpty()) {
        setError(error, revisionError);
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    const auto rollback = [this, error](const QString &message) {
        setError(error, rollbackFailure(m_database, message));
        return false;
    };

    QSqlQuery activeCursor(m_database);
    activeCursor.prepare(QStringLiteral(
        "SELECT MIN(cursor) FROM sync_clients "
        "WHERE log_uuid = ? AND last_seen_at_utc >= ?"));
    activeCursor.addBindValue(logUuid);
    activeCursor.addBindValue(cutoff);
    if (!activeCursor.exec() || !activeCursor.next()) {
        return rollback(sqlError(activeCursor));
    }
    const qint64 safeRevision = activeCursor.value(0).isNull()
        ? revision : activeCursor.value(0).toLongLong();
    activeCursor.finish();

    QSqlQuery pruneThrough(m_database);
    pruneThrough.prepare(QStringLiteral(
        "SELECT MAX(event.revision) FROM sync_events event "
        "WHERE event.log_uuid = ? AND event.revision <= ? "
        "AND event.created_at_utc < ? AND NOT EXISTS ("
        "SELECT 1 FROM sync_events newer WHERE newer.log_uuid = event.log_uuid "
        "AND newer.revision <= event.revision AND newer.created_at_utc >= ?)"));
    pruneThrough.addBindValue(logUuid);
    pruneThrough.addBindValue(safeRevision);
    pruneThrough.addBindValue(cutoff);
    pruneThrough.addBindValue(cutoff);
    if (!pruneThrough.exec() || !pruneThrough.next()) {
        return rollback(sqlError(pruneThrough));
    }
    if (pruneThrough.value(0).isNull()) {
        if (!m_database.commit()) {
            return rollback(m_database.lastError().text());
        }
        return true;
    }
    const qint64 lastPrunedRevision = pruneThrough.value(0).toLongLong();
    pruneThrough.finish();

    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM sync_events WHERE log_uuid = ? AND revision <= ?"));
    remove.addBindValue(logUuid);
    remove.addBindValue(lastPrunedRevision);
    if (!remove.exec()) {
        return rollback(sqlError(remove));
    }
    const int removed = remove.numRowsAffected();
    if (!m_database.commit()) {
        return rollback(m_database.lastError().text());
    }
    if (removedEvents != nullptr) {
        *removedEvents = removed;
    }
    return true;
}

QJsonObject CentralRepository::snapshotPage(
    const QString &logUuid, qint64 revision, int offset, int limit,
    QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || revision < 0
        || offset < 0 || limit < 1 || limit > 100) {
        setError(error, QStringLiteral("Snapshot request is invalid."));
        return {};
    }
    QString revisionError;
    const qint64 current = currentRevision(logUuid, &revisionError);
    if (!revisionError.isEmpty() || revision > current) {
        setError(error, revisionError.isEmpty()
            ? QStringLiteral("Snapshot revision is newer than the central log.")
            : revisionError);
        return {};
    }

    QJsonArray qsos;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT h.snapshot_json FROM qso_history h "
        "WHERE h.log_uuid = ? AND h.revision <= ? AND h.is_deleted = 0 "
        "AND h.qso_version = ("
        "SELECT MAX(h2.qso_version) FROM qso_history h2 "
        "WHERE h2.log_uuid = h.log_uuid AND h2.qso_uuid = h.qso_uuid "
        "AND h2.revision <= ?) ORDER BY h.qso_uuid"));
    query.addBindValue(logUuid);
    query.addBindValue(revision);
    query.addBindValue(revision);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return {};
    }
    while (query.next()) {
        const QJsonDocument document = QJsonDocument::fromJson(
            query.value(0).toByteArray());
        if (!document.isObject()) {
            setError(error, QStringLiteral("Stored snapshot QSO is invalid."));
            return {};
        }
        qsos.append(document.object());
    }
    const QByteArray checksum = QCryptographicHash::hash(
        QJsonDocument(canonicalJsonValue(qsos).toArray())
            .toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    QJsonArray page;
    const int end = qMin(offset + limit, qsos.size());
    for (int index = offset; index < end; ++index) {
        page.append(qsos.at(index));
    }
    return QJsonObject{
        {QStringLiteral("snapshot_revision"), revision},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("total_qsos"), qsos.size()},
        {QStringLiteral("sha256"), QString::fromLatin1(checksum)},
        {QStringLiteral("qsos"), page},
        {QStringLiteral("has_more"), end < qsos.size()}
    };
}

qint64 CentralRepository::earliestRevision(
    const QString &logUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid)) {
        setError(error, QStringLiteral("Log UUID is invalid."));
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT MIN(revision) FROM sync_events WHERE log_uuid = ?"));
    query.addBindValue(logUuid);
    if (!query.exec() || !query.next()) {
        setError(error, sqlError(query));
        return 0;
    }
    if (!query.value(0).isNull()) {
        return query.value(0).toLongLong();
    }
    QString currentError;
    const qint64 current = currentRevision(logUuid, &currentError);
    if (!currentError.isEmpty()) {
        setError(error, currentError);
        return 0;
    }
    return current + 1;
}

QJsonObject CentralRepository::currentQso(
    const QString &logUuid, const QString &qsoUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(qsoUuid)) {
        setError(error, QStringLiteral("Log or QSO UUID is invalid."));
        return {};
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT snapshot_json FROM qso_history "
        "WHERE log_uuid = ? AND qso_uuid = ? "
        "ORDER BY qso_version DESC LIMIT 1"));
    query.addBindValue(logUuid);
    query.addBindValue(qsoUuid);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return {};
    }
    if (!query.next()) {
        setError(error, QStringLiteral("QSO does not exist."));
        return {};
    }
    const QJsonDocument snapshot = QJsonDocument::fromJson(
        query.value(0).toByteArray());
    if (!snapshot.isObject()) {
        setError(error, QStringLiteral("Stored QSO snapshot is invalid."));
        return {};
    }
    return snapshot.object();
}

qint64 CentralRepository::currentRevision(
    const QString &logUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid)) {
        setError(error, QStringLiteral("Log UUID is invalid."));
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COALESCE(s.current_revision, 0) FROM contests c "
        "LEFT JOIN sync_log_state s ON s.log_uuid = c.log_uuid "
        "WHERE c.log_uuid = ?"));
    query.addBindValue(logUuid);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return 0;
    }
    if (!query.next()) {
        setError(error, QStringLiteral("Unknown central log."));
        return 0;
    }
    return query.value(0).toLongLong();
}

bool CentralRepository::recordClientCursor(
    const QString &logUuid, const QString &stationUuid, qint64 cursor,
    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(stationUuid)
        || cursor < 0) {
        setError(error, QStringLiteral("Log UUID, station UUID, or cursor is invalid."));
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    QSqlQuery ensureLogState(m_database);
    ensureLogState.prepare(QStringLiteral(
        "INSERT INTO sync_log_state(log_uuid, current_revision) "
        "SELECT log_uuid, 0 FROM contests WHERE log_uuid = ? "
        "ON CONFLICT(log_uuid) DO NOTHING"));
    ensureLogState.addBindValue(logUuid);
    if (!ensureLogState.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(ensureLogState)));
        return false;
    }
    QSqlQuery revisionQuery(m_database);
    revisionQuery.prepare(QStringLiteral(
        "SELECT current_revision FROM sync_log_state WHERE log_uuid = ?"));
    revisionQuery.addBindValue(logUuid);
    if (!revisionQuery.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(revisionQuery)));
        return false;
    }
    if (!revisionQuery.next()) {
        setError(error, rollbackFailure(
            m_database, QStringLiteral("Unknown or unsynchronized log.")));
        return false;
    }
    const qint64 revision = revisionQuery.value(0).toLongLong();
    revisionQuery.finish();
    if (cursor > revision) {
        setError(error, rollbackFailure(
            m_database, QStringLiteral("Client cursor cannot exceed the log revision.")));
        return false;
    }

    QSqlQuery upsert(m_database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO sync_clients(log_uuid, station_uuid, cursor, last_seen_at_utc) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(log_uuid, station_uuid) DO UPDATE SET "
        "cursor = excluded.cursor, last_seen_at_utc = excluded.last_seen_at_utc "
        "WHERE excluded.cursor >= sync_clients.cursor"));
    upsert.addBindValue(logUuid);
    upsert.addBindValue(stationUuid);
    upsert.addBindValue(cursor);
    upsert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!upsert.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(upsert)));
        return false;
    }
    if (upsert.numRowsAffected() != 1) {
        setError(error, rollbackFailure(
            m_database, QStringLiteral("Client cursor cannot move backwards.")));
        return false;
    }
    if (!m_database.commit()) {
        setError(error, rollbackFailure(m_database, m_database.lastError().text()));
        return false;
    }
    return true;
}

bool CentralRepository::recordClientHeartbeat(
    const QString &logUuid, const QString &stationUuid,
    qint64 lastAppliedRevision, QString *error)
{
    return recordClientHeartbeat(logUuid, stationUuid, lastAppliedRevision,
                                 -1, -1, -1, error);
}

bool CentralRepository::recordClientHeartbeat(
    const QString &logUuid, const QString &stationUuid,
    qint64 lastAppliedRevision, int pendingCount, int errorCount,
    int conflictCount, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(stationUuid)
        || lastAppliedRevision < 0
        || !((pendingCount == -1 && errorCount == -1 && conflictCount == -1)
             || (pendingCount >= 0 && errorCount >= 0 && conflictCount >= 0))) {
        setError(error, QStringLiteral(
            "Log UUID, station UUID, or heartbeat revision is invalid."));
        return false;
    }
    QString revisionError;
    const qint64 revision = currentRevision(logUuid, &revisionError);
    if (!revisionError.isEmpty()) {
        setError(error, revisionError);
        return false;
    }
    if (lastAppliedRevision > revision) {
        setError(error, QStringLiteral(
            "Heartbeat revision exceeds the central log revision."));
        return false;
    }
    if (!m_database.transaction()) {
        setError(error, m_database.lastError().text());
        return false;
    }
    QSqlQuery upsert(m_database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO sync_clients(log_uuid, station_uuid, cursor, last_seen_at_utc, "
        "pending_count, error_count, conflict_count) "
        "VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(log_uuid, station_uuid) DO UPDATE SET "
        "cursor = MAX(sync_clients.cursor, excluded.cursor), "
        "last_seen_at_utc = excluded.last_seen_at_utc, "
        "pending_count = CASE WHEN ? >= 0 THEN excluded.pending_count "
        "ELSE sync_clients.pending_count END, "
        "error_count = CASE WHEN ? >= 0 THEN excluded.error_count "
        "ELSE sync_clients.error_count END, "
        "conflict_count = CASE WHEN ? >= 0 THEN excluded.conflict_count "
        "ELSE sync_clients.conflict_count END"));
    upsert.addBindValue(logUuid);
    upsert.addBindValue(stationUuid);
    upsert.addBindValue(lastAppliedRevision);
    upsert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    upsert.addBindValue(qMax(0, pendingCount));
    upsert.addBindValue(qMax(0, errorCount));
    upsert.addBindValue(qMax(0, conflictCount));
    upsert.addBindValue(pendingCount);
    upsert.addBindValue(errorCount);
    upsert.addBindValue(conflictCount);
    if (!upsert.exec()) {
        setError(error, rollbackFailure(m_database, sqlError(upsert)));
        return false;
    }
    if (!m_database.commit()) {
        setError(error, rollbackFailure(m_database, m_database.lastError().text()));
        return false;
    }
    return true;
}

qint64 CentralRepository::clientCursor(
    const QString &logUuid, const QString &stationUuid, QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen() || !isUuid(logUuid) || !isUuid(stationUuid)) {
        setError(error, QStringLiteral("Log UUID or station UUID is invalid."));
        return 0;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT cursor FROM sync_clients WHERE log_uuid = ? AND station_uuid = ?"));
    query.addBindValue(logUuid);
    query.addBindValue(stationUuid);
    if (!query.exec()) {
        setError(error, sqlError(query));
        return 0;
    }
    return query.next() ? query.value(0).toLongLong() : 0;
}

QString CentralRepository::serverUuid(QString *error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_database.isOpen()) {
        setError(error, QStringLiteral("Central database is not open."));
        return {};
    }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT value FROM sync_metadata WHERE key = 'central_server_uuid'"))) {
        setError(error, sqlError(query));
        return {};
    }
    if (query.next()) {
        return query.value(0).toString();
    }
    query.finish();

    const QString identifier = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO sync_metadata(key, value) VALUES('central_server_uuid', ?)"));
    insert.addBindValue(identifier);
    if (!insert.exec()) {
        setError(error, sqlError(insert));
        return {};
    }
    return identifier;
}

SqliteRepository *CentralRepository::repository()
{
    return m_repository.get();
}

QString CentralRepository::databasePath() const
{
    return m_databasePath;
}

QString CentralRepository::backupDirectory() const
{
    return m_backupDirectory;
}

} // namespace logger
