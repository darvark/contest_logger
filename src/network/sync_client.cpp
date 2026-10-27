#include "logger/network/sync_client.h"

#include <QHostInfo>

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace logger {
namespace {

constexpr int ProtocolVersion = 2;
constexpr qsizetype MaximumFrameSize = 1024 * 1024;
constexpr int RequestTimeoutMs = 30'000;
constexpr int HeartbeatIntervalMs = 15'000;
constexpr int SynchronizationFallbackIntervalMs = 5'000;
constexpr int MaximumReconnectDelayMs = 30'000;

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

bool isUuid(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
                       "[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch();
}

QByteArray encodeFrame(const QJsonObject &message)
{
    const QByteArray payload =
        QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.reserve(payload.size() + 4);
    const quint32 size = static_cast<quint32>(payload.size());
    frame.append(static_cast<char>((size >> 24) & 0xff));
    frame.append(static_cast<char>((size >> 16) & 0xff));
    frame.append(static_cast<char>((size >> 8) & 0xff));
    frame.append(static_cast<char>(size & 0xff));
    frame.append(payload);
    return frame;
}

} // namespace

SyncClient::SyncClient(QObject *parent)
    : QObject(parent),
      m_socket(std::make_unique<QSslSocket>()),
      m_reconnectTimer(std::make_unique<QTimer>(this)),
      m_heartbeatTimer(std::make_unique<QTimer>(this)),
      m_requestTimer(std::make_unique<QTimer>(this)),
      m_syncTimer(std::make_unique<QTimer>(this))
{
    m_reconnectTimer->setSingleShot(true);
    m_heartbeatTimer->setInterval(HeartbeatIntervalMs);
    m_requestTimer->setInterval(250);
    m_syncTimer->setInterval(SynchronizationFallbackIntervalMs);

    connect(m_socket.get(), &QSslSocket::encrypted, this, [this] {
        if (m_configuration.trustedNetworkMode) {
            return;
        }
        m_socketFailureHandled = false;
        m_input.clear();
        beginHandshake();
    });
    connect(m_socket.get(), &QSslSocket::connected, this, [this] {
        if (!m_configuration.trustedNetworkMode) {
            return;
        }
        m_socketFailureHandled = false;
        m_input.clear();
        beginHandshake();
    });
    connect(m_socket.get(), &QSslSocket::readyRead,
            this, &SyncClient::readFrames);
    connect(m_socket.get(), &QSslSocket::sslErrors, this,
            [this](const QList<QSslError> &) {
                handleSocketFailure(tr("The server TLS certificate could not be verified."));
            });
    connect(m_socket.get(), &QSslSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                if (m_socket->state() == QAbstractSocket::UnconnectedState) {
                    handleSocketFailure(m_socket->errorString());
                }
            });
    connect(m_socket.get(), &QSslSocket::disconnected, this, [this] {
        if (!m_manualDisconnect) {
            handleSocketFailure(tr("Connection to synchronization server was lost."));
        }
    });
    connect(m_reconnectTimer.get(), &QTimer::timeout, this, [this] {
        if (m_hasConfiguration && !m_manualDisconnect) {
            m_socketFailureHandled = false;
            setState(State::Connecting, tr("Reconnecting to synchronization server."));
            if (m_configuration.trustedNetworkMode) {
                m_socket->connectToHost(
                    m_configuration.host, m_configuration.port);
            } else {
                m_socket->connectToHostEncrypted(
                    m_configuration.host, m_configuration.port);
            }
        }
    });
    connect(m_heartbeatTimer.get(), &QTimer::timeout,
            this, &SyncClient::heartbeat);
    connect(m_requestTimer.get(), &QTimer::timeout,
            this, &SyncClient::expireRequests);
    connect(m_syncTimer.get(), &QTimer::timeout,
            this, &SyncClient::pumpSynchronization);
    m_requestTimer->start();
}

SyncClient::~SyncClient()
{
    disconnectFromServer();
}

SyncClient::State SyncClient::state() const
{
    return m_state;
}

QString SyncClient::serverUuid() const
{
    return m_serverUuid;
}

qint64 SyncClient::currentRevision() const
{
    return m_currentRevision;
}

QString SyncClient::contestDefinitionYaml() const
{
    return m_definitionYaml;
}

bool SyncClient::isReadOnly() const
{
    return m_readOnly;
}

void SyncClient::setLastAppliedRevision(qint64 revision)
{
    if (revision >= 0) {
        m_lastAppliedRevision = revision;
    }
}

bool SyncClient::startSynchronization(
    SqliteRepository &repository, QString *error)
{
    if (!m_hasConfiguration || m_configuration.logUuid.isEmpty()) {
        if (error != nullptr) {
            *error = tr("Configure the synchronization server before starting sync.");
        }
        return false;
    }
    QString repositoryError;
    const QString localStationUuid = repository.localStationUuid(&repositoryError);
    if (!repositoryError.isEmpty()
        || localStationUuid != m_configuration.stationUuid) {
        if (error != nullptr) {
            *error = repositoryError.isEmpty()
                ? tr("Configured station UUID does not match the local installation.")
                : repositoryError;
        }
        return false;
    }
    const QVector<Contest> localLogs = repository.contests(&repositoryError);
    if (!repositoryError.isEmpty()
        || std::none_of(localLogs.cbegin(), localLogs.cend(),
            [this](const Contest &contest) {
                return contest.logUuid == m_configuration.logUuid;
            })) {
        if (error != nullptr) {
            *error = repositoryError.isEmpty()
                ? tr("Configured central log is not present in the local database.")
                : repositoryError;
        }
        return false;
    }
    if (!repository.synchronizationEnabled(
            m_configuration.logUuid, &repositoryError)
        || !repositoryError.isEmpty()) {
        if (error != nullptr) {
            *error = repositoryError.isEmpty()
                ? tr("Synchronization is not enabled for the selected local log.")
                : repositoryError;
        }
        return false;
    }
    const qint64 cursor = repository.syncCursor(
        m_configuration.logUuid, &repositoryError);
    if (!repositoryError.isEmpty()) {
        if (error != nullptr) {
            *error = repositoryError;
        }
        return false;
    }
    if (!repository.recoverInFlightSyncOperations(
            m_configuration.logUuid, &repositoryError)) {
        if (error != nullptr) {
            *error = repositoryError;
        }
        return false;
    }
    m_repository = &repository;
    m_lastAppliedRevision = cursor;
    m_synchronizationEnabled = true;
    m_syncTimer->start();
    pumpSynchronization();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void SyncClient::stopSynchronization()
{
    m_synchronizationEnabled = false;
    m_syncPending = false;
    m_syncTimer->stop();
    m_repository = nullptr;
    m_outboxRequestId.clear();
    m_changesRequestId.clear();
}

bool SyncClient::synchronizeNow()
{
    if (!m_synchronizationEnabled || m_repository == nullptr
        || m_state != State::Ready || m_manualSyncActive) {
        return false;
    }
    m_manualUploadedOperations = 0;
    m_manualDownloadedEvents = 0;
    m_manualSyncTimer.start();
    m_manualSyncActive = true;
    emit synchronizationStarted();
    m_syncTimer->start();
    pumpSynchronization();
    return m_state == State::Ready;
}

bool SyncClient::selectRemoteLog(
    const QString &logUuid, const QString &contestDefinitionSha256)
{
    if (m_state != State::SelectingLog || !m_configuration.logUuid.isEmpty()
        || !isUuid(logUuid)
        || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                .match(contestDefinitionSha256).hasMatch()) {
        return false;
    }
    m_configuration.logUuid = logUuid;
    m_configuration.contestDefinitionSha256 = contestDefinitionSha256;
    selectLog();
    return true;
}

QString SyncClient::createRemoteLog(
    const Contest &log, const QString &definitionYaml)
{
    if (m_state != State::SelectingLog || !m_configuration.logUuid.isEmpty()
        || !isUuid(log.logUuid) || log.name.trimmed().isEmpty()
        || definitionYaml.trimmed().isEmpty()) {
        return {};
    }
    QJsonObject properties;
    for (auto property = log.properties.constBegin();
         property != log.properties.constEnd(); ++property) {
        properties.insert(property.key(), property.value());
    }
    const QJsonValue startsAt = log.startsAtUtc.has_value()
        ? QJsonValue(log.startsAtUtc->toUTC().toString(Qt::ISODateWithMs))
        : QJsonValue(QJsonValue::Null);
    return sendRequest(QStringLiteral("create_log"), QJsonObject{
        {QStringLiteral("definition_yaml"), definitionYaml},
        {QStringLiteral("log"), QJsonObject{
            {QStringLiteral("log_uuid"), log.logUuid},
            {QStringLiteral("name"), log.name},
            {QStringLiteral("station_callsign"), log.stationCallsign},
            {QStringLiteral("starts_at_utc"), startsAt},
            {QStringLiteral("properties"), properties}
        }}
    });
}

QString SyncClient::importSerialHistory(
    const QString &logUuid, const QJsonArray &reservations)
{
    if (m_state == State::Disconnected || m_state == State::Error
        || !isUuid(logUuid) || reservations.size() > 1000) {
        return {};
    }
    return sendRequest(QStringLiteral("import_serial_reservations"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("reservations"), reservations}
    });
}

QString SyncClient::importLogBatch(const QJsonArray &operations)
{
    if (m_state != State::Ready || m_readOnly
        || operations.isEmpty() || operations.size() > 100) {
        return {};
    }
    return sendRequest(QStringLiteral("import_log_batch"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("atomic"), true},
        {QStringLiteral("operations"), operations}
    });
}

void SyncClient::connectToServer(
    const SyncClientConfiguration &configuration)
{
    m_stationIdentitySupported = false;
    if (m_readOnly) {
        m_readOnly = false;
        emit readOnlyChanged(false);
    }
    if (configuration.host.trimmed().isEmpty() || configuration.port == 0
        || (!configuration.trustedNetworkMode
            && configuration.peerName.trimmed().isEmpty())
        || !isUuid(configuration.stationUuid)
        || (!configuration.trustedNetworkMode && configuration.token.size() < 32)
        || (!configuration.logUuid.isEmpty() && !isUuid(configuration.logUuid))
        || (!configuration.contestDefinitionSha256.isEmpty()
            && !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                    .match(configuration.contestDefinitionSha256).hasMatch())) {
        setState(State::Error, tr("Synchronization connection settings are invalid."));
        return;
    }
    m_configuration = configuration;
    m_hasConfiguration = true;
    m_manualDisconnect = false;
    m_reconnectDelayMs = 1000;
    m_serverUuid.clear();
    m_definitionYaml.clear();
    m_currentRevision = 0;
    m_lastAppliedRevision = 0;
    m_syncPending = false;
    m_pendingRequests.clear();
    m_outboxRequestId.clear();
    m_changesRequestId.clear();
    m_reconnectTimer->stop();
    if (m_synchronizationEnabled) {
        m_syncTimer->start();
    }
    m_manualDisconnect = true;
    closeSocket();
    m_manualDisconnect = false;

    m_trustedNetworkSupported = false;
    if (!configuration.trustedNetworkMode) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        if (!configuration.caCertificates.isEmpty()) {
            QList<QSslCertificate> certificates = ssl.caCertificates();
            for (const QSslCertificate &certificate : configuration.caCertificates) {
                if (!certificates.contains(certificate)) {
                    certificates.append(certificate);
                }
            }
            ssl.setCaCertificates(certificates);
        }
        m_socket->setSslConfiguration(ssl);
        m_socket->setPeerVerifyName(configuration.peerName);
    }
    m_socketFailureHandled = false;
    setState(State::Connecting, tr("Connecting to synchronization server."));
    if (configuration.trustedNetworkMode) {
        m_socket->connectToHost(configuration.host, configuration.port);
    } else {
        m_socket->connectToHostEncrypted(configuration.host, configuration.port);
    }
}

void SyncClient::disconnectFromServer()
{
    m_manualDisconnect = true;
    m_hasConfiguration = false;
    m_reconnectTimer->stop();
    m_heartbeatTimer->stop();
    m_syncTimer->stop();
    m_pendingRequests.clear();
    m_outboxRequestId.clear();
    m_outboxOperationUuids.clear();
    m_changesRequestId.clear();
    m_snapshotRequestId.clear();
    m_syncPending = false;
    closeSocket();
    m_sessionUuid.clear();
    m_handshakeRequestId.clear();
    m_configuration.token.clear();
    if (m_readOnly) {
        m_readOnly = false;
        emit readOnlyChanged(false);
    }
    setState(State::Disconnected);
}

QString SyncClient::submitBatch(const QJsonArray &operations)
{
    if (m_state != State::Ready || m_readOnly
        || operations.isEmpty() || operations.size() > 100) {
        return {};
    }
    return sendRequest(QStringLiteral("submit_batch"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("atomic"), true},
        {QStringLiteral("operations"), operations}
    });
}

QString SyncClient::getChanges(qint64 afterRevision, int limit)
{
    if (m_state != State::Ready || afterRevision < 0 || limit < 1 || limit > 500) {
        return {};
    }
    return sendRequest(QStringLiteral("get_changes"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("after_revision"), afterRevision},
        {QStringLiteral("limit"), limit}
    });
}

QString SyncClient::acknowledgeRevision(qint64 revision)
{
    if (m_state != State::Ready || revision < 0) {
        return {};
    }
    return sendRequest(QStringLiteral("ack_revision"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("revision"), revision}
    });
}

QString SyncClient::getStatus()
{
    if (m_state != State::Ready) {
        return {};
    }
    return sendRequest(QStringLiteral("get_status"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid}
    });
}

QString SyncClient::checkConsistency()
{
    if (!m_synchronizationEnabled || m_repository == nullptr
        || m_state != State::Ready || m_readOnly) {
        return {};
    }
    QString error;
    if (!m_repository->syncOutbox(m_configuration.logUuid, false, &error).isEmpty()
        || !error.isEmpty()) {
        return {};
    }
    const qint64 cursor = m_repository->syncCursor(m_configuration.logUuid, &error);
    if (!error.isEmpty() || cursor != m_currentRevision) {
        return {};
    }
    return sendRequest(QStringLiteral("get_consistency_digest"), QJsonObject{
        {QStringLiteral("digest_version"), 1},
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("revision"), cursor}
    });
}

void SyncClient::flushPendingChanges()
{
    if (!m_synchronizationEnabled || !m_repository || m_state != State::Ready) {
        return;
    }
    QString error;
    const auto pending = m_repository->syncOutbox(
        m_configuration.logUuid, false, &error, 1);
    if (!error.isEmpty()) {
        emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"), error, false);
        return;
    }
    if (!pending.isEmpty() && pending.first().status == QStringLiteral("pending")) {
        m_syncPending = true;
        pumpSynchronization();
    }
}

void SyncClient::setStationIdentity(
    const QString &stationCallsign, const QString &operatorName)
{
    m_configuration.stationCallsign = stationCallsign;
    m_configuration.operatorName = operatorName;
    if (m_stationIdentitySupported && m_state == State::Ready) {
        sendRequest(QStringLiteral("set_station_identity"), QJsonObject{
            {QStringLiteral("computer_name"), m_configuration.computerName.isEmpty()
                 ? QHostInfo::localHostName() : m_configuration.computerName},
            {QStringLiteral("station_callsign"), stationCallsign},
            {QStringLiteral("operator_name"), operatorName}
        });
    }
}

void SyncClient::setOperatingState(const QString &band, const QString &mode)
{
    m_configuration.operatingBand = band;
    m_configuration.operatingMode = mode;
    if (m_operatingStateSupported && m_state == State::Ready) {
        sendRequest(QStringLiteral("set_operating_state"), QJsonObject{
            {QStringLiteral("band"), band},
            {QStringLiteral("mode"), mode}
        });
    }
}

QString SyncClient::getMetrics()
{
    if (m_state != State::Ready) {
        return {};
    }
    return sendRequest(QStringLiteral("get_metrics"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid}
    });
}

QString SyncClient::reserveSerial(
    const QString &reservationUuid, const QString &band, const QString &mode)
{
    if (!m_synchronizationEnabled || m_repository == nullptr
        || !isUuid(reservationUuid) || band.trimmed().isEmpty()
        || mode.trimmed().isEmpty() || m_readOnly) {
        return {};
    }
    const SerialReservationRequest request{
        m_configuration.logUuid, m_configuration.stationUuid, reservationUuid,
        QStringLiteral("reserve_serial"), band, mode
    };
    QString error;
    if (!m_repository->enqueueSerialReservationRequest(request, &error)) {
        setState(State::Error, error);
        emit requestFailed(reservationUuid, QStringLiteral("LOCAL_STORAGE_ERROR"),
                           error, false);
        return {};
    }
    return pumpSerialReservationRequests(QStringLiteral("reserve_serial"),
                                         reservationUuid);
}

QString SyncClient::cancelSerial(const QString &reservationUuid)
{
    if (!m_synchronizationEnabled || m_repository == nullptr
        || !isUuid(reservationUuid) || m_readOnly) {
        return {};
    }
    QString repositoryError;
    const std::optional<SerialReservation> reservation =
        m_repository->serialReservation(
            m_configuration.logUuid, reservationUuid, &repositoryError);
    if (!repositoryError.isEmpty() || !reservation.has_value()
        || reservation->stationUuid != m_configuration.stationUuid
        || reservation->status != QStringLiteral("reserved")) {
        return {};
    }
    const SerialReservationRequest request{
        m_configuration.logUuid, m_configuration.stationUuid, reservationUuid,
        QStringLiteral("cancel_serial"), {}, {}
    };
    if (!m_repository->enqueueSerialReservationRequest(request, &repositoryError)) {
        setState(State::Error, repositoryError);
        emit requestFailed(reservationUuid, QStringLiteral("LOCAL_STORAGE_ERROR"),
                           repositoryError, false);
        return {};
    }
    return pumpSerialReservationRequests(QStringLiteral("cancel_serial"),
                                         reservationUuid);
}

QString SyncClient::getSerialReservations()
{
    if (m_state != State::Ready) {
        return {};
    }
    return sendRequest(QStringLiteral("get_serial_reservations"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid}
    });
}

QString SyncClient::adjustSerialReservation(
    const QString &reservationUuid, int expectedSerial, int correctedSerial,
    const QString &reason)
{
    if (m_state != State::Ready || m_readOnly || !isUuid(reservationUuid)
        || expectedSerial < 1 || correctedSerial < 1
        || expectedSerial == correctedSerial || reason.trimmed().isEmpty()
        || reason.trimmed().size() > 500) {
        return {};
    }
    return sendRequest(QStringLiteral("adjust_serial"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("reservation_uuid"), reservationUuid},
        {QStringLiteral("expected_serial"), expectedSerial},
        {QStringLiteral("corrected_serial"), correctedSerial},
        {QStringLiteral("reason"), reason.trimmed()}
    });
}

QString SyncClient::sendRequest(
    const QString &type, const QJsonObject &payload, bool handshakeRequest)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState
        || (!m_configuration.trustedNetworkMode && !m_socket->isEncrypted())) {
        return {};
    }
    const QString requestId = newUuid();
    QJsonObject trackedPayload;
    if (type == QStringLiteral("submit_batch")
        || type == QStringLiteral("reserve_serial")
        || type == QStringLiteral("cancel_serial")) {
        trackedPayload = payload;
    }
    m_pendingRequests.insert(requestId, PendingRequest{
        type, QDateTime::currentDateTimeUtc().addMSecs(RequestTimeoutMs),
        handshakeRequest, trackedPayload
    });
    if (handshakeRequest) {
        m_handshakeRequestId = requestId;
    }
    m_socket->write(encodeFrame(QJsonObject{
        {QStringLiteral("protocol_version"), ProtocolVersion},
        {QStringLiteral("request_id"), requestId},
        {QStringLiteral("type"), type},
        {QStringLiteral("payload"), payload}
    }));
    return requestId;
}

void SyncClient::beginHandshake()
{
    setState(State::Handshaking, tr("Negotiating synchronization protocol."));
    sendRequest(QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("client_version"), QStringLiteral("0.1.0")},
        {QStringLiteral("min_protocol_version"), ProtocolVersion},
        {QStringLiteral("max_protocol_version"), ProtocolVersion},
        {QStringLiteral("capabilities"), QJsonArray{
            QStringLiteral("submit_batch"), QStringLiteral("get_changes"),
            QStringLiteral("ack_revision"), QStringLiteral("heartbeat"),
            QStringLiteral("event_notifications"),
            QStringLiteral("station_identity"),
            QStringLiteral("operating_state"),
            QStringLiteral("get_status"), QStringLiteral("list_logs"),
            QStringLiteral("create_log"),
            QStringLiteral("import_serial_reservations"),
            QStringLiteral("import_log_batch")}}
    }, true);
}

void SyncClient::beginAuthentication()
{
    if (m_configuration.trustedNetworkMode) {
        setState(State::Authenticating, tr("Registering station on trusted network."));
        sendRequest(QStringLiteral("register_station"), QJsonObject{
            {QStringLiteral("station_uuid"), m_configuration.stationUuid},
            {QStringLiteral("station_identity"), QJsonObject{
                {QStringLiteral("computer_name"),
                 m_configuration.computerName.isEmpty()
                     ? QHostInfo::localHostName() : m_configuration.computerName},
                {QStringLiteral("station_callsign"), m_configuration.stationCallsign},
                {QStringLiteral("operator_name"), m_configuration.operatorName}
            }}
        }, true);
        return;
    }
    setState(State::Authenticating, tr("Authenticating station."));
    QJsonObject payload{
        {QStringLiteral("station_uuid"), m_configuration.stationUuid},
        {QStringLiteral("token"), m_configuration.token}
    };
    if (m_stationIdentitySupported) {
        payload.insert(QStringLiteral("station_identity"), QJsonObject{
            {QStringLiteral("computer_name"), m_configuration.computerName.isEmpty()
                 ? QHostInfo::localHostName() : m_configuration.computerName},
            {QStringLiteral("station_callsign"), m_configuration.stationCallsign},
            {QStringLiteral("operator_name"), m_configuration.operatorName}
        });
    }
    sendRequest(QStringLiteral("authenticate"), payload, true);
}

void SyncClient::selectLog()
{
    if (m_configuration.logUuid.isEmpty()) {
        setState(State::SelectingLog, tr("Discovering central logs."));
        sendRequest(QStringLiteral("list_logs"), {}, true);
        return;
    }
    setState(State::SelectingLog, tr("Selecting central log."));
    const QJsonValue definitionHash = m_configuration.contestDefinitionSha256.isEmpty()
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(m_configuration.contestDefinitionSha256);
    sendRequest(QStringLiteral("select_log"), QJsonObject{
        {QStringLiteral("log_uuid"), m_configuration.useServerActiveLog
             ? QJsonValue(QJsonValue::Null) : QJsonValue(m_configuration.logUuid)},
        {QStringLiteral("contest_definition_sha256"), definitionHash}
    }, true);
}

void SyncClient::readFrames()
{
    m_input.append(m_socket->readAll());
    while (m_input.size() >= 4) {
        const auto *bytes = reinterpret_cast<const uchar *>(m_input.constData());
        const quint32 length = (static_cast<quint32>(bytes[0]) << 24)
            | (static_cast<quint32>(bytes[1]) << 16)
            | (static_cast<quint32>(bytes[2]) << 8)
            | static_cast<quint32>(bytes[3]);
        if (length == 0 || length > MaximumFrameSize) {
            handleSocketFailure(tr("Server sent an invalid or oversized frame."));
            return;
        }
        if (m_input.size() < static_cast<qsizetype>(length) + 4) {
            return;
        }
        const QByteArray frame = m_input.mid(4, length);
        m_input.remove(0, static_cast<qsizetype>(length) + 4);
        const QString decoded = QString::fromUtf8(frame.constData(), frame.size());
        if (decoded.toUtf8() != frame) {
            handleSocketFailure(tr("Server sent invalid UTF-8 data."));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(frame);
        if (!document.isObject()) {
            handleSocketFailure(tr("Server sent an invalid protocol message."));
            return;
        }
        processResponse(document.object());
        if (m_socket->state() == QAbstractSocket::UnconnectedState) {
            return;
        }
    }
}

void SyncClient::processResponse(const QJsonObject &message)
{
    if (message.keys() != QStringList{
            QStringLiteral("payload"), QStringLiteral("protocol_version"),
            QStringLiteral("request_id"), QStringLiteral("type")}
        || message.value(QStringLiteral("protocol_version")).toInt(-1)
               != ProtocolVersion
        || !message.value(QStringLiteral("payload")).isObject()) {
        handleSocketFailure(tr("Server sent an invalid protocol envelope."));
        return;
    }
    const QString requestId = message.value(QStringLiteral("request_id")).toString();
    const QString type = message.value(QStringLiteral("type")).toString();
    const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();
    if (message.value(QStringLiteral("request_id")).isNull()) {
        const QJsonValue revisionValue =
            payload.value(QStringLiteral("current_revision"));
        const qint64 revision = revisionValue.toInteger(-1);
        if (type != QStringLiteral("event_available")
            || payload.keys() != QStringList{
                QStringLiteral("current_revision"), QStringLiteral("log_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != m_configuration.logUuid
            || !revisionValue.isDouble() || revision < 0
            || revisionValue.toDouble() != static_cast<double>(revision)) {
            handleSocketFailure(tr("Server sent an invalid event notification."));
            return;
        }
        m_currentRevision = qMax(m_currentRevision, revision);
        emit remoteChangesAvailable(revision);
        if (m_synchronizationEnabled && m_repository != nullptr
            && revision > m_lastAppliedRevision) {
            m_syncPending = true;
            if (m_state == State::Ready && m_outboxRequestId.isEmpty()
                && m_changesRequestId.isEmpty()
                && m_snapshotRequestId.isEmpty() && m_snapshotRevision < 0) {
                pumpSynchronization();
            }
        }
        return;
    }
    if (requestId.isEmpty() || !m_pendingRequests.contains(requestId)) {
        handleSocketFailure(tr("Server sent an unsolicited or uncorrelated response."));
        return;
    }
    const PendingRequest pending = m_pendingRequests.take(requestId);
    if (type == QStringLiteral("error")) {
        const QString code = payload.value(QStringLiteral("code")).toString();
        const QString messageText = payload.value(QStringLiteral("message")).toString();
        const bool retryable = payload.value(QStringLiteral("retryable")).toBool();
        emit requestFailed(requestId, code, messageText, retryable);
        if (pending.handshake) {
            if (!retryable || code == QStringLiteral("UNSUPPORTED_PROTOCOL")
                || code == QStringLiteral("UNAUTHENTICATED")
                || code == QStringLiteral("FORBIDDEN")
                || code == QStringLiteral("UNKNOWN_LOG")
                || code == QStringLiteral("CONTEST_MISMATCH")
                || code == QStringLiteral("INVALID_MESSAGE")) {
                setState(State::Error, messageText);
                m_hasConfiguration = false;
                m_reconnectTimer->stop();
                closeSocket();
            } else {
                handleSocketFailure(messageText);
            }
        } else if (requestId == m_outboxRequestId && m_repository != nullptr) {
            m_outboxRequestId.clear();
            const QStringList operationUuids = m_outboxOperationUuids;
            m_outboxOperationUuids.clear();
            QString repositoryError;
            if (operationUuids.size() > 1) {
                if (!m_repository->recoverInFlightSyncOperations(
                        m_configuration.logUuid, &repositoryError)) {
                    setState(State::Error, repositoryError);
                    emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                                       repositoryError, false);
                    return;
                }
                if (retryable) {
                    handleSocketFailure(messageText);
                } else {
                    m_outboxBatchSize = 1;
                    pumpSynchronization();
                }
                return;
            }
            const QString operationUuid = operationUuids.isEmpty()
                ? QString() : operationUuids.first();
            const QJsonObject currentQso =
                payload.value(QStringLiteral("context")).toObject()
                    .value(QStringLiteral("current_qso")).toObject();
            if (code == QStringLiteral("VERSION_CONFLICT")
                && !currentQso.isEmpty()) {
                if (!m_repository->recordSyncConflict(
                        operationUuid,
                        QString::fromUtf8(QJsonDocument(currentQso)
                                              .toJson(QJsonDocument::Compact)),
                        &repositoryError)) {
                    setState(State::Error, repositoryError);
                    emit requestFailed(requestId,
                                       QStringLiteral("LOCAL_STORAGE_ERROR"),
                                       repositoryError, false);
                    return;
                }
            } else {
                if (!m_repository->setSyncOperationState(
                        operationUuid, QStringLiteral("failed"), messageText,
                        &repositoryError)) {
                    setState(State::Error, repositoryError);
                    emit requestFailed(requestId,
                                       QStringLiteral("LOCAL_STORAGE_ERROR"),
                                       repositoryError, false);
                    return;
                }
                if (retryable) {
                    if (!m_repository->retrySyncOperation(
                            operationUuid, &repositoryError)) {
                        setState(State::Error, repositoryError);
                        emit requestFailed(requestId,
                                           QStringLiteral("LOCAL_STORAGE_ERROR"),
                                           repositoryError, false);
                        return;
                    }
                    m_outboxBatchSize = 16;
                    handleSocketFailure(messageText);
                    return;
                }
            }
            m_outboxBatchSize = 16;
            pumpSynchronization();
        } else if ((pending.type == QStringLiteral("reserve_serial")
                    || pending.type == QStringLiteral("cancel_serial"))
                   && m_repository != nullptr) {
            if (retryable) {
                handleSocketFailure(messageText);
                return;
            }
            QString repositoryError;
            if (!m_repository->removeSerialReservationRequest(
                    m_configuration.logUuid,
                    pending.payload.value(QStringLiteral("reservation_uuid")).toString(),
                    pending.type, &repositoryError)) {
                setState(State::Error, repositoryError);
                emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                                   repositoryError, false);
                return;
            }
        } else if (requestId == m_changesRequestId) {
            m_changesRequestId.clear();
            if (code == QStringLiteral("SNAPSHOT_REQUIRED")) {
                beginSnapshot();
                return;
            }
            if (retryable) {
                handleSocketFailure(messageText);
                return;
            }
            setState(State::Error, messageText);
            m_syncTimer->stop();
        } else if (requestId == m_snapshotRequestId) {
            m_snapshotRequestId.clear();
            if (retryable) {
                handleSocketFailure(messageText);
                return;
            }
            setState(State::Error, messageText);
            m_syncTimer->stop();
        }
        return;
    }
    if (pending.handshake) {
        if (pending.type == QStringLiteral("hello")
            && type == QStringLiteral("hello_ack")) {
            m_stationIdentitySupported = payload.value(QStringLiteral("capabilities"))
                .toArray().contains(QStringLiteral("station_identity"));
            m_operatingStateSupported = payload.value(QStringLiteral("capabilities"))
                .toArray().contains(QStringLiteral("operating_state"));
            m_trustedNetworkSupported = payload.value(QStringLiteral("capabilities"))
                .toArray().contains(QStringLiteral("trusted_network"));
            if (m_configuration.trustedNetworkMode && !m_trustedNetworkSupported) {
                handleSocketFailure(
                    tr("The server does not support trusted-network registration."));
                return;
            }
            beginAuthentication();
            return;
        }
        if ((pending.type == QStringLiteral("authenticate")
             && type == QStringLiteral("authenticated"))
            || (pending.type == QStringLiteral("register_station")
                && type == QStringLiteral("registered"))) {
            m_sessionUuid = payload.value(QStringLiteral("session_uuid")).toString();
            if (!isUuid(m_sessionUuid)) {
                handleSocketFailure(tr("Server returned an invalid session identity."));
                return;
            }
            const QJsonValue permissionsValue =
                payload.value(QStringLiteral("permissions"));
            if (!permissionsValue.isArray()) {
                handleSocketFailure(tr("Server returned invalid station permissions."));
                return;
            }
            bool canRead = false;
            bool canWrite = false;
            for (const QJsonValue &permission : permissionsValue.toArray()) {
                if (!permission.isString()) {
                    handleSocketFailure(
                        tr("Server returned invalid station permissions."));
                    return;
                }
                canRead = canRead || permission.toString() == QStringLiteral("read");
                canWrite = canWrite || permission.toString() == QStringLiteral("write");
            }
            if (!canRead) {
                handleSocketFailure(
                    tr("The station credential does not include read permission."));
                return;
            }
            const bool readOnly = !canWrite;
            if (m_readOnly != readOnly) {
                m_readOnly = readOnly;
                emit readOnlyChanged(m_readOnly);
            }
            selectLog();
            return;
        }
        if (pending.type == QStringLiteral("list_logs")
            && type == QStringLiteral("logs")) {
            const QJsonArray logs = payload.value(QStringLiteral("logs")).toArray();
            if (payload.keys() != QStringList{QStringLiteral("logs")}
                || !payload.value(QStringLiteral("logs")).isArray()) {
                handleSocketFailure(tr("Server returned an invalid central log list."));
                return;
            }
            for (const QJsonValue &value : logs) {
                if (!value.isObject()) {
                    handleSocketFailure(
                        tr("Server returned an invalid central log entry."));
                    return;
                }
                const QJsonObject log = value.toObject();
                if (!isUuid(log.value(QStringLiteral("log_uuid")).toString())
                    || log.value(QStringLiteral("name")).toString().trimmed().isEmpty()
                    || !log.value(QStringLiteral("definition_yaml")).isString()
                    || !log.value(QStringLiteral("current_revision")).isDouble()
                    || log.value(QStringLiteral("current_revision")).toInteger(-1) < 0) {
                    handleSocketFailure(
                        tr("Server returned an invalid central log entry."));
                    return;
                }
            }
            emit logsDiscovered(logs);
            return;
        }
        if (pending.type == QStringLiteral("select_log")
            && type == QStringLiteral("welcome")) {
            if (m_configuration.useServerActiveLog) {
                QString error;
                const QString serverLogUuid =
                    payload.value(QStringLiteral("log_uuid")).toString();
                if (!m_repository || !m_repository->bindToServerLog(
                        m_configuration.logUuid, serverLogUuid, &error)) {
                    m_manualDisconnect = true;
                    closeSocket();
                    setState(State::Error, error.isEmpty()
                        ? tr("The open local log could not be connected to the server log.")
                        : error);
                    return;
                }
                m_configuration.logUuid = serverLogUuid;
                emit logBound(serverLogUuid);
            }
            m_serverUuid = payload.value(QStringLiteral("server_uuid")).toString();
            m_currentRevision =
                payload.value(QStringLiteral("current_revision")).toInteger(-1);
            const qint64 earliestRevision =
                payload.value(QStringLiteral("earliest_revision")).toInteger(-1);
            m_definitionYaml =
                payload.value(QStringLiteral("definition_yaml")).toString();
            if (!isUuid(m_serverUuid) || m_currentRevision < 0
                || earliestRevision < 1) {
                handleSocketFailure(tr("Server returned an invalid log welcome."));
                return;
            }
            bool snapshotRequired = false;
            if (m_synchronizationEnabled && m_repository != nullptr) {
                QString error;
                const qint64 localCursor = m_repository->syncCursor(
                    m_configuration.logUuid, &error);
                if (!error.isEmpty()) {
                    setState(State::Error, error);
                    emit requestFailed(requestId,
                                       QStringLiteral("LOCAL_STORAGE_ERROR"),
                                       error, false);
                    return;
                }
                if (localCursor + 1 < earliestRevision) {
                    snapshotRequired = true;
                }
            }
            m_reconnectDelayMs = 1000;
            setState(State::Ready, tr("Connected and synchronized with server."));
            m_heartbeatTimer->start();
            emit ready(m_serverUuid, m_currentRevision, m_definitionYaml);
            if (m_synchronizationEnabled && m_repository != nullptr) {
                QString error;
                if (!m_repository->recoverInFlightSyncOperations(
                        m_configuration.logUuid, &error)) {
                    setState(State::Error, error);
                    emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"),
                                       error, false);
                    return;
                }
                m_lastAppliedRevision = m_repository->syncCursor(
                    m_configuration.logUuid, &error);
                if (!error.isEmpty()) {
                    setState(State::Error, error);
                    emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"),
                                       error, false);
                    return;
                }
                if (snapshotRequired) {
                    beginSnapshot();
                } else {
                    pumpSynchronization();
                }
            }
            return;
        }
        handleSocketFailure(tr("Server returned an unexpected handshake response."));
        return;
    }

    if (type == QStringLiteral("snapshot_page")
        && requestId == m_snapshotRequestId && m_repository != nullptr) {
        m_snapshotRequestId.clear();
        const qint64 revision =
            payload.value(QStringLiteral("snapshot_revision")).toInteger(-1);
        const int offset = payload.value(QStringLiteral("offset")).toInt(-1);
        const int total = payload.value(QStringLiteral("total_qsos")).toInt(-1);
        const QString sha256 = payload.value(QStringLiteral("sha256")).toString();
        const QJsonArray qsos = payload.value(QStringLiteral("qsos")).toArray();
        const bool hasMore = payload.value(QStringLiteral("has_more")).toBool();
        const bool firstPage = m_snapshotRevision < 0;
        if (revision < 0 || offset != m_snapshotOffset || total < 0
            || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                    .match(sha256).hasMatch()
            || (firstPage && offset != 0)
            || (!firstPage && (revision != m_snapshotRevision
                               || total != m_snapshotTotal
                               || sha256 != m_snapshotSha256))
            || qsos.size() > 25 || offset + qsos.size() > total
            || hasMore != (offset + qsos.size() < total)
            || (hasMore && qsos.isEmpty())) {
            setState(State::Error, tr("Server returned an invalid snapshot page."));
            emit requestFailed(requestId, QStringLiteral("INVALID_SNAPSHOT_PAGE"),
                               tr("Server returned an invalid snapshot page."), false);
            m_syncTimer->stop();
            return;
        }
        if (firstPage) {
            m_snapshotRevision = revision;
            m_snapshotTotal = total;
            m_snapshotSha256 = sha256;
        }
        for (const QJsonValue &qso : qsos) {
            m_snapshotQsos.append(qso);
        }
        m_snapshotOffset += qsos.size();
        if (hasMore) {
            requestSnapshotPage();
            return;
        }
        const QByteArray checksum = QCryptographicHash::hash(
            QJsonDocument(m_snapshotQsos).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256).toHex();
        QString error;
        if (m_snapshotOffset != m_snapshotTotal
            || QString::fromLatin1(checksum) != m_snapshotSha256
            || !m_repository->applySyncSnapshot(
                m_configuration.logUuid, m_snapshotQsos, m_snapshotRevision,
                m_snapshotSha256, &error)) {
            if (error.isEmpty()) {
                error = tr("Snapshot checksum or QSO count is invalid.");
            }
            setState(State::Error, error);
            emit requestFailed(requestId, QStringLiteral("INVALID_SNAPSHOT"),
                               error, false);
            m_syncTimer->stop();
            return;
        }
        m_lastAppliedRevision = m_snapshotRevision;
        m_currentRevision = qMax(m_currentRevision, m_snapshotRevision);
        m_snapshotQsos = {};
        m_snapshotRevision = -1;
        m_snapshotOffset = 0;
        m_snapshotTotal = -1;
        m_snapshotSha256.clear();
        if (acknowledgeRevision(m_lastAppliedRevision).isEmpty()) {
            handleSocketFailure(tr("Could not acknowledge the applied snapshot."));
            return;
        }
        emit responseReceived(requestId, type, payload);
        pumpSynchronization();
        return;
    }

    if (type == QStringLiteral("batch_result")
        && requestId == m_outboxRequestId && m_repository != nullptr) {
        const QJsonArray results = payload.value(QStringLiteral("results")).toArray();
        QString error;
        bool localStorageFailure = false;
        const QJsonArray submittedOperations =
            pending.payload.value(QStringLiteral("operations")).toArray();
        if (results.size() != m_outboxOperationUuids.size()
            || submittedOperations.size() != m_outboxOperationUuids.size()) {
            error = tr("Server returned an invalid batch result.");
        }
        for (qsizetype index = 0; error.isEmpty() && index < results.size(); ++index) {
            const QJsonObject result = results.at(index).toObject();
            const QJsonObject operation = submittedOperations.at(index).toObject();
            if (result.value(QStringLiteral("operation_uuid")).toString()
                    != m_outboxOperationUuids.at(index)
                || result.value(QStringLiteral("status")).toString()
                    != QStringLiteral("accepted")) {
                error = tr("Server returned an invalid operation result.");
                break;
            }
            const QString reservationUuid =
                operation.value(QStringLiteral("reservation_uuid")).toString();
            if (operation.value(QStringLiteral("kind")).toString()
                    == QStringLiteral("qso.create")
                && !reservationUuid.isEmpty()) {
                const QString qsoUuid = operation.value(QStringLiteral("qso"))
                                            .toObject()
                                            .value(QStringLiteral("qso_uuid"))
                                            .toString();
                if (!m_repository->setSerialReservationStatus(
                        m_configuration.logUuid, reservationUuid,
                        QStringLiteral("used"), qsoUuid, &error)) {
                    localStorageFailure = true;
                    if (error.isEmpty()) {
                        error = tr("Could not persist serial reservation use.");
                    }
                }
            }
            if (error.isEmpty()
                && !m_repository->setSyncOperationState(
                    m_outboxOperationUuids.at(index),
                    QStringLiteral("accepted"), {}, &error)) {
                localStorageFailure = true;
            }
        }
        if (!error.isEmpty()) {
            setState(State::Error, error);
            emit requestFailed(requestId,
                               localStorageFailure
                                   ? QStringLiteral("LOCAL_STORAGE_ERROR")
                                   : QStringLiteral("INVALID_BATCH_RESULT"),
                               error, false);
            return;
        }
        m_outboxRequestId.clear();
        m_manualUploadedOperations += m_outboxOperationUuids.size();
        m_outboxOperationUuids.clear();
        emit responseReceived(requestId, type, payload);
        pumpSynchronization();
        return;
    }
    if (type == QStringLiteral("changes")
        && requestId == m_changesRequestId && m_repository != nullptr) {
        m_changesRequestId.clear();
        const qint64 fromRevision =
            payload.value(QStringLiteral("from_revision")).toInteger(-1);
        const qint64 toRevision =
            payload.value(QStringLiteral("to_revision")).toInteger(-1);
        const qint64 currentRevision =
            payload.value(QStringLiteral("current_revision")).toInteger(-1);
        const QJsonArray events = payload.value(QStringLiteral("events")).toArray();
        QString error;
        if (fromRevision < 1 || toRevision < 0 || currentRevision < toRevision
            || !m_repository->applySyncChanges(
                m_configuration.logUuid, events, fromRevision, toRevision, &error)) {
            if (error.isEmpty()) {
                error = tr("Server returned an invalid change page.");
            }
            setState(State::Error, error);
            emit requestFailed(requestId, QStringLiteral("INVALID_CHANGE_PAGE"),
                               error, false);
            m_syncTimer->stop();
            return;
        }
        m_lastAppliedRevision = toRevision;
        m_currentRevision = currentRevision;
        m_manualDownloadedEvents += events.size();
        const QString ackRequestId = acknowledgeRevision(toRevision);
        if (ackRequestId.isEmpty()) {
            handleSocketFailure(tr("Could not acknowledge locally applied changes."));
            return;
        }
        emit responseReceived(requestId, type, payload);
        if (payload.value(QStringLiteral("has_more")).toBool() || m_syncPending) {
            m_syncPending = false;
            pumpSynchronization();
        }
        return;
    }
    if (type == QStringLiteral("changes")
        && payload.value(QStringLiteral("to_revision")).isDouble()) {
        m_currentRevision = payload.value(QStringLiteral("current_revision"))
                                .toInteger(m_currentRevision);
    }
    if (pending.type == QStringLiteral("reserve_serial")) {
        if (type != QStringLiteral("serial_reserved") || m_repository == nullptr) {
            const QString error = tr("Serial reservation response cannot be stored locally.");
            emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                               error, false);
            return;
        }
        bool serialOk = false;
        const int serial = payload.value(QStringLiteral("serial")).toInt(-1);
        serialOk = serial > 0;
        const SerialReservation reservation{
            payload.value(QStringLiteral("reservation_uuid")).toString(),
            payload.value(QStringLiteral("log_uuid")).toString(),
            payload.value(QStringLiteral("station_uuid")).toString(),
            payload.value(QStringLiteral("scope_key")).toString(),
            payload.value(QStringLiteral("band")).toString(),
            payload.value(QStringLiteral("mode")).toString(),
            serial,
            payload.value(QStringLiteral("status")).toString(),
            payload.value(QStringLiteral("qso_uuid")).toString()
        };
        QString repositoryError;
        if (!serialOk || reservation.reservationUuid
                != pending.payload.value(QStringLiteral("reservation_uuid")).toString()
            || reservation.logUuid != m_configuration.logUuid
            || reservation.stationUuid != m_configuration.stationUuid
            || !m_repository->saveSerialReservation(reservation, &repositoryError)) {
            if (repositoryError.isEmpty()) {
                repositoryError = tr("Server returned an invalid serial reservation.");
            }
            emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                               repositoryError, false);
            return;
        }
        if (!m_repository->removeSerialReservationRequest(
                m_configuration.logUuid, reservation.reservationUuid,
                pending.type, &repositoryError)) {
            setState(State::Error, repositoryError);
            emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                               repositoryError, false);
            return;
        }
    } else if (pending.type == QStringLiteral("cancel_serial")) {
        if (type != QStringLiteral("serial_cancelled") || m_repository == nullptr
            || payload.value(QStringLiteral("reservation_uuid")).toString()
                != pending.payload.value(QStringLiteral("reservation_uuid")).toString()) {
            emit requestFailed(requestId, QStringLiteral("INVALID_RESPONSE"),
                               tr("Server returned an invalid serial cancellation response."),
                               false);
            return;
        }
        QString repositoryError;
        if (!m_repository->setSerialReservationStatus(
                m_configuration.logUuid,
                pending.payload.value(QStringLiteral("reservation_uuid")).toString(),
                QStringLiteral("cancelled"), {}, &repositoryError)) {
            emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                               repositoryError, false);
            return;
        }
        if (!m_repository->removeSerialReservationRequest(
                m_configuration.logUuid,
                pending.payload.value(QStringLiteral("reservation_uuid")).toString(),
                pending.type, &repositoryError)) {
            setState(State::Error, repositoryError);
            emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                               repositoryError, false);
            return;
        }
    } else if (pending.type == QStringLiteral("adjust_serial")) {
        if (type != QStringLiteral("serial_adjusted") || m_repository == nullptr
            || payload.value(QStringLiteral("log_uuid")).toString()
                != m_configuration.logUuid
            || payload.value(QStringLiteral("reservation_uuid")).toString()
                != pending.payload.value(
                    QStringLiteral("reservation_uuid")).toString()
            || payload.value(QStringLiteral("previous_serial")).toInt(-1)
                != pending.payload.value(
                    QStringLiteral("expected_serial")).toInt(-2)
            || payload.value(QStringLiteral("corrected_serial")).toInt(-1)
                != pending.payload.value(
                    QStringLiteral("corrected_serial")).toInt(-2)) {
            emit requestFailed(requestId, QStringLiteral("INVALID_RESPONSE"),
                               tr("Server returned an invalid serial correction response."),
                               false);
            return;
        }
        QString repositoryError;
        if (!m_repository->adjustCachedSerialReservation(
                m_configuration.logUuid,
                pending.payload.value(QStringLiteral("reservation_uuid")).toString(),
                pending.payload.value(QStringLiteral("expected_serial")).toInt(),
                pending.payload.value(QStringLiteral("corrected_serial")).toInt(),
                &repositoryError)) {
            emit requestFailed(requestId, QStringLiteral("LOCAL_STORAGE_ERROR"),
                               repositoryError, false);
            return;
        }
    }
    if (pending.type == QStringLiteral("ack_revision")
        && type == QStringLiteral("acknowledged")
        && m_manualSyncActive && m_repository != nullptr) {
        QString syncError;
        const qint64 cursor = m_repository->syncCursor(
            m_configuration.logUuid, &syncError);
        if (!syncError.isEmpty()) {
            finishManualSynchronization(false, syncError);
        } else if (m_syncPending || cursor < m_currentRevision) {
            m_syncPending = false;
            pumpSynchronization();
        } else {
            const QVector<SyncOutboxOperation> remaining =
                m_repository->syncOutbox(m_configuration.logUuid, false, &syncError);
            if (!syncError.isEmpty()) {
                finishManualSynchronization(false, syncError);
            } else {
                const bool unfinished = std::any_of(
                    remaining.cbegin(), remaining.cend(),
                    [](const SyncOutboxOperation &operation) {
                        return operation.status == QStringLiteral("pending")
                            || operation.status == QStringLiteral("in-flight");
                    });
                const int failed = static_cast<int>(std::count_if(
                    remaining.cbegin(), remaining.cend(),
                    [](const SyncOutboxOperation &operation) {
                        return operation.status == QStringLiteral("failed")
                            || operation.status == QStringLiteral("conflict");
                    }));
                if (unfinished) {
                    pumpSynchronization();
                } else {
                    finishManualSynchronization(
                        failed == 0,
                        failed == 0
                            ? tr("Synchronization completed.")
                            : tr("Synchronization completed with %1 failed or conflicted operation(s).")
                                  .arg(failed));
                }
            }
        }
    }
    if (pending.type == QStringLiteral("set_station_identity")
        && (type != QStringLiteral("station_identity_updated") || !payload.isEmpty())) {
        emit requestFailed(requestId, QStringLiteral("INVALID_RESPONSE"),
                           tr("Server returned an invalid station identity confirmation."),
                           false);
        return;
    }
    emit responseReceived(requestId, type, payload);
    if (pending.type == QStringLiteral("reserve_serial")
        || pending.type == QStringLiteral("cancel_serial")) {
        pumpSynchronization();
    }
}

void SyncClient::handleSocketFailure(const QString &message)
{
    if (m_socketFailureHandled || m_manualDisconnect || !m_hasConfiguration) {
        return;
    }
    m_socketFailureHandled = true;
    m_heartbeatTimer->stop();
    for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end(); ++it) {
        emit requestFailed(it.key(), QStringLiteral("CONNECTION_LOST"),
                           message, true);
    }
    m_pendingRequests.clear();
    m_outboxRequestId.clear();
    m_outboxOperationUuids.clear();
    m_changesRequestId.clear();
    closeSocket();
    scheduleReconnect(message);
}

void SyncClient::scheduleReconnect(const QString &message)
{
    setState(State::Backoff, message);
    emit disconnected(message);
    m_reconnectTimer->start(m_reconnectDelayMs);
    m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, MaximumReconnectDelayMs);
}

void SyncClient::setState(State state, const QString &message)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state, message);
    if (m_manualSyncActive
        && (state == State::Backoff || state == State::Error
            || state == State::Disconnected)) {
        finishManualSynchronization(false,
            message.isEmpty() ? tr("Connection was lost during synchronization.")
                              : message);
    }
}

void SyncClient::finishManualSynchronization(
    bool success, const QString &detail)
{
    if (!m_manualSyncActive) {
        return;
    }
    const qint64 elapsedMs = m_manualSyncTimer.isValid()
        ? m_manualSyncTimer.elapsed() : 0;
    m_manualSyncActive = false;
    emit synchronizationFinished(
        m_manualUploadedOperations, m_manualDownloadedEvents,
        elapsedMs, success, detail);
}

void SyncClient::heartbeat()
{
    if (m_state != State::Ready) {
        return;
    }
    QJsonObject payload{
        {QStringLiteral("last_applied_revision"), m_lastAppliedRevision},
        {QStringLiteral("session_uuid"), m_sessionUuid}
    };
    if (m_operatingStateSupported) {
        payload.insert(QStringLiteral("operating_band"), m_configuration.operatingBand);
        payload.insert(QStringLiteral("operating_mode"), m_configuration.operatingMode);
    }
    if (m_synchronizationEnabled && m_repository != nullptr) {
        QString error;
        const QVector<SyncOutboxOperation> operations =
            m_repository->syncOutbox(m_configuration.logUuid, false, &error);
        if (!error.isEmpty()) {
            emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"), error, false);
        } else {
            int pendingCount = 0;
            int errorCount = 0;
            int conflictCount = 0;
            for (const SyncOutboxOperation &operation : operations) {
                if (operation.status == QStringLiteral("pending")
                    || operation.status == QStringLiteral("in-flight")) {
                    ++pendingCount;
                } else if (operation.status == QStringLiteral("failed")) {
                    ++errorCount;
                } else if (operation.status == QStringLiteral("conflict")) {
                    ++conflictCount;
                }
            }
            payload.insert(QStringLiteral("pending_count"), pendingCount);
            payload.insert(QStringLiteral("error_count"), errorCount);
            payload.insert(QStringLiteral("conflict_count"), conflictCount);
            if (!m_operatingStateSupported) {
                payload.remove(QStringLiteral("operating_band"));
                payload.remove(QStringLiteral("operating_mode"));
            }
        }
    }
    sendRequest(QStringLiteral("heartbeat"), payload);
}

void SyncClient::expireRequests()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QStringList expired;
    for (auto it = m_pendingRequests.cbegin(); it != m_pendingRequests.cend(); ++it) {
        if (it.value().deadline <= now) {
            expired.append(it.key());
        }
    }
    for (const QString &requestId : expired) {
        const PendingRequest request = m_pendingRequests.take(requestId);
        emit requestFailed(requestId, QStringLiteral("TIMEOUT"),
                           tr("Synchronization request timed out."), true);
        if (request.handshake || requestId == m_outboxRequestId
            || requestId == m_changesRequestId
            || requestId == m_snapshotRequestId
            || request.type == QStringLiteral("reserve_serial")
            || request.type == QStringLiteral("cancel_serial")) {
            handleSocketFailure(tr("Synchronization request timed out."));
            return;
        }
    }
}

void SyncClient::closeSocket()
{
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_input.clear();
}

void SyncClient::pumpSynchronization()
{
    if (!m_synchronizationEnabled || m_repository == nullptr
        || m_state != State::Ready || !m_outboxRequestId.isEmpty()
        || !m_changesRequestId.isEmpty() || !m_snapshotRequestId.isEmpty()
        || m_snapshotRevision >= 0) {
        return;
    }
    m_syncPending = false;
    pumpSerialReservationRequests();
    if (m_state != State::Ready) {
        return;
    }
    QString error;
    QVector<SyncOutboxOperation> operations =
        m_repository->syncOutbox(m_configuration.logUuid, false, &error,
                                 m_outboxBatchSize);
    if (!error.isEmpty()) {
        setState(State::Error, error);
        emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"), error, false);
        m_syncTimer->stop();
        return;
    }

    if (m_readOnly) {
        operations.clear();
    }
    QJsonArray submittedOperations;
    m_outboxOperationUuids.clear();
    for (const SyncOutboxOperation &operation : operations) {
        if (operation.status != QStringLiteral("pending")) {
            break;
        }
        const QJsonDocument document =
            QJsonDocument::fromJson(operation.payloadJson.toUtf8());
        if (!document.isObject()) {
            const QString invalidOperation =
                tr("Stored synchronization operation is not a JSON object.");
            QString storageError;
            if (!m_repository->setSyncOperationState(
                    operation.operationUuid, QStringLiteral("failed"),
                    invalidOperation, &storageError)) {
                setState(State::Error, storageError);
                emit requestFailed(operation.operationUuid,
                                   QStringLiteral("LOCAL_STORAGE_ERROR"),
                                   storageError, false);
                m_syncTimer->stop();
                return;
            }
            emit requestFailed(operation.operationUuid,
                               QStringLiteral("INVALID_OUTBOX_OPERATION"),
                               invalidOperation, false);
            continue;
        }
        if (!m_repository->setSyncOperationState(
                operation.operationUuid, QStringLiteral("in-flight"), {}, &error)) {
            setState(State::Error, error);
            emit requestFailed(operation.operationUuid,
                               QStringLiteral("LOCAL_STORAGE_ERROR"), error, false);
            m_syncTimer->stop();
            return;
        }
        m_outboxOperationUuids.append(operation.operationUuid);
        submittedOperations.append(document.object());
    }
    if (!submittedOperations.isEmpty()) {
        m_outboxRequestId = sendRequest(QStringLiteral("submit_batch"), QJsonObject{
            {QStringLiteral("log_uuid"), m_configuration.logUuid},
            {QStringLiteral("atomic"), true},
            {QStringLiteral("operations"), submittedOperations}
        });
        if (m_outboxRequestId.isEmpty()) {
            m_outboxOperationUuids.clear();
            handleSocketFailure(tr("Could not send pending synchronization operations."));
        }
        return;
    }

    const qint64 cursor =
        m_repository->syncCursor(m_configuration.logUuid, &error);
    if (!error.isEmpty()) {
        setState(State::Error, error);
        emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"), error, false);
        m_syncTimer->stop();
        return;
    }
    m_lastAppliedRevision = cursor;
    m_changesRequestId = getChanges(cursor);
    if (m_changesRequestId.isEmpty()) {
        handleSocketFailure(tr("Could not request central log changes."));
    }
}

QString SyncClient::pumpSerialReservationRequests(
    const QString &operation, const QString &reservationUuid)
{
    if (!m_synchronizationEnabled || m_repository == nullptr
        || m_state != State::Ready || m_readOnly) {
        return {};
    }
    QString error;
    const QVector<SerialReservationRequest> requests =
        m_repository->pendingSerialReservationRequests(
            m_configuration.logUuid, m_configuration.stationUuid, &error);
    if (!error.isEmpty()) {
        setState(State::Error, error);
        emit requestFailed({}, QStringLiteral("LOCAL_STORAGE_ERROR"), error, false);
        m_syncTimer->stop();
        return {};
    }
    QString firstRequestId;
    QString requestedId;
    for (const SerialReservationRequest &request : requests) {
        bool alreadyPending = false;
        for (auto it = m_pendingRequests.cbegin();
             it != m_pendingRequests.cend(); ++it) {
            if (it.value().type == request.operation
                && it.value().payload.value(QStringLiteral("reservation_uuid"))
                    .toString() == request.reservationUuid) {
                alreadyPending = true;
                if (firstRequestId.isEmpty()) {
                    firstRequestId = it.key();
                }
                if (request.operation == operation
                    && request.reservationUuid == reservationUuid) {
                    requestedId = it.key();
                }
                break;
            }
        }
        if (alreadyPending) {
            continue;
        }
        QJsonObject payload{
            {QStringLiteral("log_uuid"), request.logUuid},
            {QStringLiteral("reservation_uuid"), request.reservationUuid}
        };
        if (request.operation == QStringLiteral("reserve_serial")) {
            payload.insert(QStringLiteral("band"), request.band);
            payload.insert(QStringLiteral("mode"), request.mode);
        }
        const QString requestId = sendRequest(request.operation, payload);
        if (requestId.isEmpty()) {
            handleSocketFailure(
                tr("Could not send a queued serial reservation request."));
            return {};
        }
        if (firstRequestId.isEmpty()) {
            firstRequestId = requestId;
        }
        if (request.operation == operation
            && request.reservationUuid == reservationUuid) {
            requestedId = requestId;
        }
    }
    return requestedId.isEmpty() ? firstRequestId : requestedId;
}

void SyncClient::beginSnapshot()
{
    m_snapshotQsos = {};
    m_snapshotSha256.clear();
    m_snapshotRevision = -1;
    m_snapshotOffset = 0;
    m_snapshotTotal = -1;
    requestSnapshotPage();
}

void SyncClient::requestSnapshotPage()
{
    QJsonObject payload{
        {QStringLiteral("log_uuid"), m_configuration.logUuid},
        {QStringLiteral("offset"), m_snapshotOffset},
        {QStringLiteral("limit"), 25}
    };
    if (m_snapshotRevision >= 0) {
        payload.insert(QStringLiteral("snapshot_revision"), m_snapshotRevision);
    }
    m_snapshotRequestId = sendRequest(QStringLiteral("get_snapshot"), payload);
    if (m_snapshotRequestId.isEmpty()) {
        handleSocketFailure(tr("Could not request a synchronization snapshot."));
    }
}

} // namespace logger
