#include "logger/network/sync_server.h"

#include "logger/contest/contest_definition_loader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace logger {

class TlsTcpServer final : public QTcpServer {
public:
    using AcceptedHandler = std::function<void(QSslSocket *)>;

    explicit TlsTcpServer(AcceptedHandler handler)
        : m_handler(std::move(handler))
    {
    }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            socket->deleteLater();
            return;
        }
        m_handler(socket);
    }

private:
    AcceptedHandler m_handler;
};

namespace {

constexpr int ProtocolVersion = 1;
constexpr qsizetype MaximumFrameSize = 1024 * 1024;
constexpr qsizetype MaximumCredentialFileSize = 1024 * 1024;
constexpr int MaximumBatchSize = 100;
constexpr int MaximumPageSize = 500;
constexpr int IdleTimeoutMs = 60'000;
constexpr int EventRetentionDays = 30;
constexpr int EventRetentionIntervalMs = 24 * 60 * 60 * 1000;

QString uuidString()
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

bool isStringArray(const QJsonValue &value)
{
    if (!value.isArray()) {
        return false;
    }
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString()) {
            return false;
        }
    }
    return true;
}

bool isNonNegativeInteger(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble(-1);
    return number >= 0 && number <= static_cast<double>(
        std::numeric_limits<qint64>::max()) && number == static_cast<qint64>(number);
}

bool secureTokenEqual(const QByteArray &left, const QByteArray &right)
{
    const QByteArray leftHash = QCryptographicHash::hash(
        left, QCryptographicHash::Sha256);
    const QByteArray rightHash = QCryptographicHash::hash(
        right, QCryptographicHash::Sha256);
    unsigned char difference = 0;
    for (qsizetype index = 0; index < leftHash.size(); ++index) {
        difference |= static_cast<unsigned char>(
            leftHash.at(index) ^ rightHash.at(index));
    }
    return difference == 0;
}

QByteArray frameBytes(const QJsonObject &message)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray frame;
    const quint32 size = static_cast<quint32>(payload.size());
    frame.reserve(payload.size() + 4);
    frame.append(static_cast<char>((size >> 24) & 0xff));
    frame.append(static_cast<char>((size >> 16) & 0xff));
    frame.append(static_cast<char>((size >> 8) & 0xff));
    frame.append(static_cast<char>(size & 0xff));
    frame.append(payload);
    return frame;
}

} // namespace

namespace {

bool isStationIdentity(const QJsonValue &value)
{
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject identity = value.toObject();
    if (identity.keys() != QStringList{
            QStringLiteral("computer_name"), QStringLiteral("operator_name"),
            QStringLiteral("station_callsign")}) {
        return false;
    }
    for (auto it = identity.begin(); it != identity.end(); ++it) {
        if (!it.value().isString() || it.value().toString().size() > 255) {
            return false;
        }
        for (const QChar character : it.value().toString()) {
            if (character.category() == QChar::Other_Control) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

struct SyncServer::ClientSession {
    QByteArray input;
    QTimer *idleTimer = nullptr;
    bool tlsReady = false;
    bool protocolSelected = false;
    bool authenticated = false;
    QString stationUuid;
    QString sessionUuid;
    QJsonObject identity;
    QString activeLogUuid;
    QStringList permissions;
    QStringList allowedLogUuids;
    QSet<QString> capabilities;
    QSet<QString> requestIds;
    qint64 lastNotifiedRevision = 0;
    int protocolVersion = ProtocolVersion;
};

SyncServer::SyncServer(CentralRepository &repository, QObject *parent)
    : QObject(parent),
      m_repository(repository),
      m_server(std::make_unique<TlsTcpServer>(
          [this](QSslSocket *socket) { beginTls(socket); })),
      m_eventRetentionTimer(std::make_unique<QTimer>(this))
{
    m_eventRetentionTimer->setInterval(EventRetentionIntervalMs);
    connect(m_eventRetentionTimer.get(), &QTimer::timeout, this, [this] {
        QString error;
        if (!runEventRetention(&error)) {
            qWarning().noquote() << "Synchronization event retention failed:" << error;
        }
    });
}

SyncServer::~SyncServer()
{
    close();
}

bool SyncServer::listen(const QString &address, quint16 port,
                        const QString &certificatePath,
                        const QString &privateKeyPath,
                        const QString &tokenFilePath,
                        int maximumConnections, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (m_server->isListening()) {
        if (error != nullptr) {
            *error = QStringLiteral("Sync server is already listening.");
        }
        return false;
    }
    if (maximumConnections < 1 || maximumConnections > 256) {
        if (error != nullptr) {
            *error = QStringLiteral("Maximum connections must be between 1 and 256.");
        }
        return false;
    }

    QFile certificateFile(certificatePath);
    if (!certificateFile.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Cannot open TLS certificate: %1")
                         .arg(certificateFile.errorString());
        }
        return false;
    }
    const QSslCertificate certificate(&certificateFile, QSsl::Pem);
    certificateFile.close();
    if (certificate.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("TLS certificate is invalid or not PEM encoded.");
        }
        return false;
    }

    QFile privateKeyFile(privateKeyPath);
    if (!privateKeyFile.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Cannot open TLS private key: %1")
                         .arg(privateKeyFile.errorString());
        }
        return false;
    }
    const QSslKey privateKey(&privateKeyFile, QSsl::Rsa, QSsl::Pem);
    privateKeyFile.close();
    if (privateKey.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("TLS private key is invalid, encrypted, or not RSA PEM.");
        }
        return false;
    }

    QFile tokenFile(tokenFilePath);
    if (!tokenFile.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Cannot open station token file: %1")
                         .arg(tokenFile.errorString());
        }
        return false;
    }
    if (tokenFile.size() <= 0 || tokenFile.size() > MaximumCredentialFileSize) {
        if (error != nullptr) {
            *error = QStringLiteral("Station token file size is invalid.");
        }
        return false;
    }
    const QJsonDocument tokensDocument = QJsonDocument::fromJson(tokenFile.readAll());
    if (!tokensDocument.isObject()
        || !tokensDocument.object().value(QStringLiteral("stations")).isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Station token file must contain a stations array.");
        }
        return false;
    }
    QHash<QString, QJsonObject> credentials;
    for (const QJsonValue &value
         : tokensDocument.object().value(QStringLiteral("stations")).toArray()) {
        if (!value.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("Station credentials must be JSON objects.");
            }
            return false;
        }
        const QJsonObject credential = value.toObject();
        const QString stationUuid = credential.value(QStringLiteral("station_uuid")).toString();
        const QString token = credential.value(QStringLiteral("token")).toString();
        const QJsonValue permissionsValue = credential.value(QStringLiteral("permissions"));
        if (credential.keys() != QStringList{
                QStringLiteral("log_uuids"), QStringLiteral("permissions"),
                QStringLiteral("station_uuid"), QStringLiteral("token")}
            || !isUuid(stationUuid) || token.size() < 32
            || !isStringArray(permissionsValue)
            || !isStringArray(credential.value(QStringLiteral("log_uuids")))
            || credentials.contains(stationUuid)) {
            if (error != nullptr) {
                *error = QStringLiteral("Station credentials are invalid or duplicated.");
            }
            return false;
        }
        QSet<QString> permissions;
        for (const QJsonValue &permission : permissionsValue.toArray()) {
            const QString name = permission.toString();
            if (name != QStringLiteral("read") && name != QStringLiteral("write")
                && name != QStringLiteral("admin")) {
                if (error != nullptr) {
                    *error = QStringLiteral("Station credential has an unknown permission.");
                }
                return false;
            }
            permissions.insert(name);
        }
        for (const QJsonValue &allowedLog
             : credential.value(QStringLiteral("log_uuids")).toArray()) {
            if (!isUuid(allowedLog.toString())) {
                if (error != nullptr) {
                    *error = QStringLiteral("Station credential contains an invalid log UUID.");
                }
                return false;
            }
        }
        if (permissions.contains(QStringLiteral("write"))
            && !permissions.contains(QStringLiteral("read"))) {
            if (error != nullptr) {
                *error = QStringLiteral("Write permission also requires read permission.");
            }
            return false;
        }
        credentials.insert(stationUuid, credential);
    }
    if (credentials.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("At least one station credential is required.");
        }
        return false;
    }

    m_server->setMaxPendingConnections(maximumConnections);
    m_maximumConnections = maximumConnections;
    m_credentials = std::move(credentials);
    QHostAddress host;
    if (!host.setAddress(address)) {
        if (error != nullptr) {
            *error = QStringLiteral("Listen address must be a numeric IP address.");
        }
        m_credentials.clear();
        return false;
    }
    QString retentionError;
    if (!runEventRetention(&retentionError)) {
        if (error != nullptr) {
            *error = retentionError;
        }
        m_credentials.clear();
        return false;
    }
    if (!m_server->listen(host, port)) {
        if (error != nullptr) {
            *error = m_server->errorString();
        }
        m_credentials.clear();
        return false;
    }
    m_certificate = certificate;
    m_privateKey = privateKey;
    m_eventRetentionTimer->start();
    return true;
}

void SyncServer::close()
{
    m_eventRetentionTimer->stop();
    if (m_server) {
        m_server->close();
    }
    const auto sockets = m_sessions.keys();
    for (QSslSocket *socket : sockets) {
        closeSession(socket);
    }
    m_credentials.clear();
}

bool SyncServer::runEventRetention(QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    SqliteRepository *repository = m_repository.repository();
    if (repository == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("Central repository is not initialized.");
        }
        return false;
    }
    QString repositoryError;
    const QVector<Contest> logs = repository->contests(&repositoryError);
    if (!repositoryError.isEmpty()) {
        if (error != nullptr) {
            *error = repositoryError;
        }
        return false;
    }
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(
        -EventRetentionDays);
    for (const Contest &log : logs) {
        if (!m_repository.compactSyncEvents(
                log.logUuid, cutoff, nullptr, &repositoryError)) {
            if (error != nullptr) {
                *error = repositoryError;
            }
            return false;
        }
    }
    return true;
}

quint16 SyncServer::serverPort() const
{
    return m_server->serverPort();
}

int SyncServer::activeConnections() const
{
    return m_sessions.size();
}

QJsonArray SyncServer::connectedStations(const QString &logUuid) const
{
    QJsonArray stations;
    if (logUuid.isEmpty()) {
        return stations;
    }
    for (const ClientSession *session : m_sessions) {
        if (session->authenticated && session->activeLogUuid == logUuid) {
            stations.append(QJsonObject{
                {QStringLiteral("station_uuid"), session->stationUuid},
                {QStringLiteral("session_uuid"), session->sessionUuid},
                {QStringLiteral("computer_name"),
                 session->identity.value(QStringLiteral("computer_name")).toString()},
                {QStringLiteral("station_callsign"),
                 session->identity.value(QStringLiteral("station_callsign")).toString()},
                {QStringLiteral("operator_name"),
                 session->identity.value(QStringLiteral("operator_name")).toString()}
            });
        }
    }
    return stations;
}

bool SyncServer::stationCanAccessLog(
    const ClientSession *session, const QString &logUuid) const
{
    if (session == nullptr || !isUuid(logUuid)) {
        return false;
    }
    if (session->permissions.contains(QStringLiteral("admin"))) {
        return true;
    }
    return session->allowedLogUuids.contains(logUuid);
}

void SyncServer::beginTls(QSslSocket *socket)
{
    if (m_sessions.size() >= m_maximumConnections) {
        socket->disconnectFromHost();
        socket->deleteLater();
        return;
    }
    auto *session = new ClientSession;
    session->idleTimer = new QTimer(socket);
    session->idleTimer->setSingleShot(true);
    session->idleTimer->setInterval(IdleTimeoutMs);
    m_sessions.insert(socket, session);

    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificate(m_certificate);
    configuration.setPrivateKey(m_privateKey);
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    socket->setSslConfiguration(configuration);

    connect(session->idleTimer, &QTimer::timeout, socket, [this, socket] {
        sendError(socket, ProtocolVersion, QString(), QStringLiteral("RATE_LIMITED"),
                  QStringLiteral("Connection idle timeout."));
        closeSession(socket);
    });
    session->idleTimer->start();
    connect(socket, &QSslSocket::encrypted, this, [this, socket] {
        const auto it = m_sessions.find(socket);
        if (it == m_sessions.end()) {
            return;
        }
        it.value()->tlsReady = true;
        touchSession(socket);
    });
    connect(socket, &QSslSocket::readyRead,
            this, [this, socket] { processFrames(socket); });
    connect(socket, &QSslSocket::sslErrors, this,
            [this, socket](const QList<QSslError> &) { closeSession(socket); });
    connect(socket, &QSslSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
                if (socket->state() == QAbstractSocket::UnconnectedState) {
                    closeSession(socket);
                }
            });
    connect(socket, &QSslSocket::disconnected,
            this, [this, socket] { closeSession(socket); });
    socket->startServerEncryption();
}

void SyncServer::touchSession(QSslSocket *socket)
{
    const auto it = m_sessions.find(socket);
    if (it != m_sessions.end()) {
        it.value()->idleTimer->start();
    }
}

void SyncServer::processFrames(QSslSocket *socket)
{
    const auto it = m_sessions.find(socket);
    if (it == m_sessions.end() || !it.value()->tlsReady) {
        return;
    }
    ClientSession *session = it.value();
    session->input.append(socket->readAll());
    while (session->input.size() >= 4) {
        const auto *bytes = reinterpret_cast<const uchar *>(session->input.constData());
        const quint32 length = (static_cast<quint32>(bytes[0]) << 24)
            | (static_cast<quint32>(bytes[1]) << 16)
            | (static_cast<quint32>(bytes[2]) << 8)
            | static_cast<quint32>(bytes[3]);
        if (length == 0 || length > MaximumFrameSize) {
            sendError(socket, session->protocolVersion, QString(),
                      length > MaximumFrameSize ? QStringLiteral("MESSAGE_TOO_LARGE")
                                                : QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("Frame length is invalid."));
            closeSession(socket);
            return;
        }
        if (session->input.size() < static_cast<qsizetype>(length) + 4) {
            return;
        }
        const QByteArray frame = session->input.mid(4, length);
        session->input.remove(0, static_cast<qsizetype>(length) + 4);
        processMessage(socket, frame);
        if (!m_sessions.contains(socket)) {
            return;
        }
        touchSession(socket);
    }
    if (session->input.size() > MaximumFrameSize + 4) {
        sendError(socket, session->protocolVersion, QString(),
                  QStringLiteral("MESSAGE_TOO_LARGE"),
                  QStringLiteral("Buffered frame exceeds the maximum size."));
        closeSession(socket);
    }
}

void SyncServer::processMessage(QSslSocket *socket, const QByteArray &frame)
{
    ClientSession *session = m_sessions.value(socket, nullptr);
    if (session == nullptr) {
        return;
    }
    const QString decoded = QString::fromUtf8(frame.constData(), frame.size());
    if (decoded.toUtf8() != frame) {
        sendError(socket, session->protocolVersion, QString(),
                  QStringLiteral("INVALID_MESSAGE"), QStringLiteral("Frame is not valid UTF-8."));
        closeSession(socket);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(frame, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        sendError(socket, session->protocolVersion, QString(),
                  QStringLiteral("INVALID_MESSAGE"), QStringLiteral("Frame is not a JSON object."));
        closeSession(socket);
        return;
    }
    const QJsonObject message = document.object();
    const QStringList envelopeKeys{
        QStringLiteral("payload"), QStringLiteral("protocol_version"),
        QStringLiteral("request_id"), QStringLiteral("type")
    };
    if (message.keys() != envelopeKeys || !message.value(QStringLiteral("payload")).isObject()
        || !message.value(QStringLiteral("protocol_version")).isDouble()
        || message.value(QStringLiteral("protocol_version")).toDouble(-1) < 0
        || message.value(QStringLiteral("protocol_version")).toDouble(-1)
               != message.value(QStringLiteral("protocol_version")).toInt(-1)
        || !message.value(QStringLiteral("type")).isString()) {
        sendError(socket, session->protocolVersion, QString(),
                  QStringLiteral("INVALID_MESSAGE"), QStringLiteral("Envelope is invalid."));
        closeSession(socket);
        return;
    }
    const QString requestId = message.value(QStringLiteral("request_id")).toString();
    if (!isUuid(requestId) || session->requestIds.contains(requestId)) {
        sendError(socket, session->protocolVersion, QString(),
                  QStringLiteral("INVALID_MESSAGE"),
                  QStringLiteral("request_id must be a unique UUID."));
        closeSession(socket);
        return;
    }
    session->requestIds.insert(requestId);
    if (session->requestIds.size() > 4096) {
        session->requestIds.clear();
        session->requestIds.insert(requestId);
    }
    const int protocolVersion =
        message.value(QStringLiteral("protocol_version")).toInt();
    const QString type = message.value(QStringLiteral("type")).toString();
    const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();

    if (!session->protocolSelected) {
        if (type != QStringLiteral("hello")) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNSUPPORTED_PROTOCOL"),
                      QStringLiteral("Send hello before other requests."));
            closeSession(socket);
            return;
        }
        const QStringList expectedKeys{
            QStringLiteral("capabilities"), QStringLiteral("client_version"),
            QStringLiteral("max_protocol_version"), QStringLiteral("min_protocol_version")
        };
        const QJsonValue minVersion = payload.value(QStringLiteral("min_protocol_version"));
        const QJsonValue maxVersion = payload.value(QStringLiteral("max_protocol_version"));
        if (payload.keys() != expectedKeys || !isStringArray(
                payload.value(QStringLiteral("capabilities")))
            || !payload.value(QStringLiteral("client_version")).isString()
            || !isNonNegativeInteger(minVersion) || !isNonNegativeInteger(maxVersion)
            || minVersion.toInt() > maxVersion.toInt()
            || protocolVersion != maxVersion.toInt()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"), QStringLiteral("Hello payload is invalid."));
            closeSession(socket);
            return;
        }
        if (minVersion.toInt() > ProtocolVersion
            || maxVersion.toInt() < ProtocolVersion) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNSUPPORTED_PROTOCOL"),
                      QStringLiteral("No mutually supported protocol version."));
            closeSession(socket);
            return;
        }
        session->protocolSelected = true;
        session->protocolVersion = ProtocolVersion;
        for (const QJsonValue &capability
             : payload.value(QStringLiteral("capabilities")).toArray()) {
            session->capabilities.insert(capability.toString());
        }
        sendMessage(socket, ProtocolVersion, requestId, QStringLiteral("hello_ack"),
                    QJsonObject{
                        {QStringLiteral("protocol_version"), ProtocolVersion},
                        {QStringLiteral("server_version"), QStringLiteral("0.1.0")},
                        {QStringLiteral("capabilities"), QJsonArray{
                            QStringLiteral("list_logs"),
                            QStringLiteral("create_log"),
                            QStringLiteral("import_serial_reservations"),
                            QStringLiteral("submit_batch"), QStringLiteral("get_changes"),
                            QStringLiteral("get_snapshot"),
                            QStringLiteral("reserve_serial"),
                            QStringLiteral("cancel_serial"),
                            QStringLiteral("get_serial_reservations"),
                            QStringLiteral("ack_revision"), QStringLiteral("heartbeat"),
                            QStringLiteral("get_status"), QStringLiteral("get_metrics"),
                            QStringLiteral("station_identity"),
                            QStringLiteral("event_notifications")}}
                    });
        return;
    }
    if (protocolVersion != session->protocolVersion) {
        sendError(socket, session->protocolVersion, requestId,
                  QStringLiteral("UNSUPPORTED_PROTOCOL"),
                  QStringLiteral("Request protocol does not match the negotiated version."));
        closeSession(socket);
        return;
    }

    if (!session->authenticated) {
        QJsonObject authentication = payload;
        const QJsonValue identity = authentication.take(QStringLiteral("station_identity"));
        if (type != QStringLiteral("authenticate")
            || authentication.keys() != QStringList{
                QStringLiteral("station_uuid"), QStringLiteral("token")}
            || (!identity.isUndefined()
                && (!session->capabilities.contains(QStringLiteral("station_identity"))
                    || !isStationIdentity(identity)))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNAUTHENTICATED"),
                      QStringLiteral("Authenticate before sending other requests."));
            closeSession(socket);
            return;
        }
        const QString stationUuid = payload.value(QStringLiteral("station_uuid")).toString();
        const QString token = payload.value(QStringLiteral("token")).toString();
        const auto credential = m_credentials.constFind(stationUuid);
        if (!isUuid(stationUuid) || credential == m_credentials.cend()
            || token.isEmpty()
            || !secureTokenEqual(token.toUtf8(),
                credential->value(QStringLiteral("token")).toString().toUtf8())) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNAUTHENTICATED"),
                      QStringLiteral("Station credentials are invalid."));
            closeSession(socket);
            return;
        }
        session->stationUuid = stationUuid;
        session->identity = identity.toObject();
        session->sessionUuid = uuidString();
        session->permissions.clear();
        for (const QJsonValue &permission
             : credential->value(QStringLiteral("permissions")).toArray()) {
            session->permissions.append(permission.toString());
        }
        for (const QJsonValue &logUuid
             : credential->value(QStringLiteral("log_uuids")).toArray()) {
            session->allowedLogUuids.append(logUuid.toString());
        }
        session->authenticated = true;
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("authenticated"),
                    QJsonObject{
                        {QStringLiteral("session_uuid"), session->sessionUuid},
                        {QStringLiteral("permissions"), QJsonArray::fromStringList(
                             session->permissions)}
                    });
        return;
    }

    if (type == QStringLiteral("set_station_identity")) {
        if (!session->capabilities.contains(QStringLiteral("station_identity"))
            || !isStationIdentity(payload)) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("Station identity is invalid."));
            return;
        }
        session->identity = payload;
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("station_identity_updated"), {});
        return;
    }
    if (type == QStringLiteral("heartbeat")) {
        const QStringList legacyHeartbeatKeys{
            QStringLiteral("last_applied_revision"), QStringLiteral("session_uuid")};
        const QStringList diagnosticHeartbeatKeys{
            QStringLiteral("conflict_count"), QStringLiteral("error_count"),
            QStringLiteral("last_applied_revision"), QStringLiteral("pending_count"),
            QStringLiteral("session_uuid")};
        const bool includesDiagnostics =
            payload.keys() == diagnosticHeartbeatKeys;
        if ((payload.keys() != legacyHeartbeatKeys && !includesDiagnostics)
            || !isNonNegativeInteger(
                payload.value(QStringLiteral("last_applied_revision")))
            || (includesDiagnostics
                && (!isNonNegativeInteger(
                        payload.value(QStringLiteral("pending_count")))
                    || !isNonNegativeInteger(
                        payload.value(QStringLiteral("error_count")))
                    || !isNonNegativeInteger(
                        payload.value(QStringLiteral("conflict_count")))))
            || payload.value(QStringLiteral("session_uuid")).toString()
                   != session->sessionUuid) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"), QStringLiteral("Heartbeat is invalid."));
            return;
        }
        if (!session->activeLogUuid.isEmpty()
            && session->permissions.contains(QStringLiteral("read"))) {
            QString error;
            const bool recorded = includesDiagnostics
                ? m_repository.recordClientHeartbeat(
                      session->activeLogUuid, session->stationUuid,
                      payload.value(QStringLiteral("last_applied_revision")).toInteger(),
                      payload.value(QStringLiteral("pending_count")).toInt(),
                      payload.value(QStringLiteral("error_count")).toInt(),
                      payload.value(QStringLiteral("conflict_count")).toInt(),
                      &error)
                : m_repository.recordClientHeartbeat(
                      session->activeLogUuid, session->stationUuid,
                      payload.value(QStringLiteral("last_applied_revision")).toInteger(),
                      &error);
            if (!recorded) {
                const QString code = error.startsWith(
                    QStringLiteral("Heartbeat revision exceeds"))
                    ? QStringLiteral("INVALID_REVISION")
                    : QStringLiteral("INTERNAL_ERROR");
                sendError(socket, protocolVersion, requestId, code, error);
                return;
            }
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("heartbeat_ack"),
                    QJsonObject{{QStringLiteral("server_time_utc"),
                                 QDateTime::currentDateTimeUtc()
                                     .toString(Qt::ISODateWithMs)}});
        return;
    }
    if (type == QStringLiteral("get_status")) {
        if (payload.keys() != QStringList{QStringLiteral("log_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("get_status is invalid."));
            return;
        }
        if (!session->permissions.contains(QStringLiteral("read"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Read permission is required."));
            return;
        }
        QString error;
        const qint64 revision =
            m_repository.currentRevision(session->activeLogUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read synchronization revision."));
            return;
        }
        const qint64 cursor = m_repository.clientCursor(
            session->activeLogUuid, session->stationUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read station cursor."));
            return;
        }
        const QString serverUuid = m_repository.serverUuid(&error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read synchronization status."));
            return;
        }
        sendMessage(socket, protocolVersion, requestId, QStringLiteral("status"),
                    QJsonObject{
                        {QStringLiteral("server_uuid"), serverUuid},
                        {QStringLiteral("log_uuid"), session->activeLogUuid},
                        {QStringLiteral("current_revision"), revision},
                        {QStringLiteral("station_uuid"), session->stationUuid},
                        {QStringLiteral("station_cursor"), cursor},
                        {QStringLiteral("connected_stations"),
                         connectedStations(session->activeLogUuid)}
                    });
        return;
    }
    if (type == QStringLiteral("get_metrics")) {
        if (payload.keys() != QStringList{QStringLiteral("log_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("get_metrics is invalid."));
            return;
        }
        if (!session->permissions.contains(QStringLiteral("admin"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Administrator permission is required."));
            return;
        }
        QString error;
        QJsonObject metrics = m_repository.synchronizationMetrics(
            session->activeLogUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read synchronization metrics."));
            return;
        }
        int onlineStationCount = 0;
        for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
            const ClientSession *connected = it.value();
            if (connected->authenticated
                && connected->activeLogUuid == session->activeLogUuid) {
                ++onlineStationCount;
            }
        }
        metrics.insert(QStringLiteral("online_station_count"), onlineStationCount);
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("metrics"), metrics);
        return;
    }
    if (type == QStringLiteral("list_logs")) {
        if (!payload.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("list_logs payload must be empty."));
            return;
        }
        if (!session->permissions.contains(QStringLiteral("read"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Read permission is required."));
            return;
        }
        QString error;
        const QVector<Contest> logs = m_repository.repository()->contests(&error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), QStringLiteral("Could not list logs."));
            return;
        }
        QJsonArray result;
        for (const Contest &log : logs) {
            if (!stationCanAccessLog(session, log.logUuid)) {
                continue;
            }
            const QString yaml = m_repository.repository()->contestDefinitionYaml(
                log.id, &error);
            if (!error.isEmpty()) {
                sendError(socket, protocolVersion, requestId,
                          QStringLiteral("INTERNAL_ERROR"),
                          QStringLiteral("Could not read log definition."));
                return;
            }
            QJsonObject properties;
            for (auto property = log.properties.cbegin();
                 property != log.properties.cend(); ++property) {
                properties.insert(property.key(), property.value());
            }
            result.append(QJsonObject{
                {QStringLiteral("log_uuid"), log.logUuid},
                {QStringLiteral("name"), log.name},
                {QStringLiteral("station_callsign"), log.stationCallsign},
                {QStringLiteral("starts_at_utc"), log.startsAtUtc.has_value()
                     ? QJsonValue(log.startsAtUtc->toUTC().toString(Qt::ISODateWithMs))
                     : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("properties"), properties},
                {QStringLiteral("definition_yaml"), yaml},
                {QStringLiteral("current_revision"),
                 m_repository.currentRevision(log.logUuid, &error)}
            });
            if (!error.isEmpty()) {
                sendError(socket, protocolVersion, requestId,
                          QStringLiteral("INTERNAL_ERROR"),
                          QStringLiteral("Could not read log revision."));
                return;
            }
        }
        sendMessage(socket, protocolVersion, requestId, QStringLiteral("logs"),
                    QJsonObject{{QStringLiteral("logs"), result}});
        return;
    }

    if (type == QStringLiteral("create_log")) {
        if (!session->permissions.contains(QStringLiteral("admin"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Administrator permission is required to create logs."));
            return;
        }
        const QStringList expectedKeys{
            QStringLiteral("definition_yaml"), QStringLiteral("log")
        };
        if (payload.keys() != expectedKeys
            || !payload.value(QStringLiteral("definition_yaml")).isString()
            || !payload.value(QStringLiteral("log")).isObject()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("create_log payload is invalid."));
            return;
        }
        const QJsonObject logObject = payload.value(QStringLiteral("log")).toObject();
        const QStringList expectedLogKeys{
            QStringLiteral("log_uuid"), QStringLiteral("name"),
            QStringLiteral("properties"), QStringLiteral("starts_at_utc"),
            QStringLiteral("station_callsign")
        };
        const QJsonValue startsAt = logObject.value(QStringLiteral("starts_at_utc"));
        const QJsonValue propertiesValue = logObject.value(QStringLiteral("properties"));
        if (logObject.keys() != expectedLogKeys
            || !logObject.value(QStringLiteral("log_uuid")).isString()
            || !logObject.value(QStringLiteral("name")).isString()
            || !logObject.value(QStringLiteral("station_callsign")).isString()
            || !propertiesValue.isObject()
            || !(startsAt.isNull() || startsAt.isString())) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("create_log metadata is invalid."));
            return;
        }
        Contest log;
        log.logUuid = logObject.value(QStringLiteral("log_uuid")).toString();
        log.name = logObject.value(QStringLiteral("name")).toString();
        log.stationCallsign =
            logObject.value(QStringLiteral("station_callsign")).toString();
        if (startsAt.isString()) {
            const QString value = startsAt.toString();
            const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
            if (!parsed.isValid() || !value.endsWith(QLatin1Char('Z'))) {
                sendError(socket, protocolVersion, requestId,
                          QStringLiteral("INVALID_MESSAGE"),
                          QStringLiteral("create_log start time must be UTC."));
                return;
            }
            log.startsAtUtc = parsed.toUTC();
        }
        const QJsonObject properties = propertiesValue.toObject();
        for (auto property = properties.constBegin();
             property != properties.constEnd(); ++property) {
            if (!property.value().isString()) {
                sendError(socket, protocolVersion, requestId,
                          QStringLiteral("INVALID_MESSAGE"),
                          QStringLiteral("create_log properties must be strings."));
                return;
            }
            log.properties.insert(property.key(), property.value().toString());
        }
        ContestDefinition definition;
        const QString yaml = payload.value(QStringLiteral("definition_yaml")).toString();
        QString error;
        if (yaml.trimmed().isEmpty()
            || !ContestDefinitionLoader().loadText(
                yaml, QStringLiteral("create_log request"), definition, &error)) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("CONTEST_MISMATCH"),
                      error.isEmpty() ? QStringLiteral("Contest definition is empty.")
                                      : error);
            return;
        }
        const qint64 logId = m_repository.createLog(definition, yaml, log, &error);
        if (logId <= 0) {
            const bool conflict = error.contains(QStringLiteral("already exists"));
            sendError(socket, protocolVersion, requestId,
                      conflict ? QStringLiteral("LOG_EXISTS")
                               : QStringLiteral("INTERNAL_ERROR"),
                      error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("log_created"),
                    QJsonObject{
                        {QStringLiteral("log_uuid"), log.logUuid},
                        {QStringLiteral("name"), log.name},
                        {QStringLiteral("definition_yaml"), yaml},
                        {QStringLiteral("current_revision"), 0}
                    });
        return;
    }

    if (type == QStringLiteral("import_serial_reservations")) {
        if (!session->permissions.contains(QStringLiteral("admin"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Administrator permission is required to import serial history."));
            return;
        }
        const QStringList expectedKeys{
            QStringLiteral("log_uuid"), QStringLiteral("reservations")
        };
        if (payload.keys() != expectedKeys
            || !payload.value(QStringLiteral("log_uuid")).isString()
            || !payload.value(QStringLiteral("reservations")).isArray()
            || payload.value(QStringLiteral("reservations")).toArray().size() > 1000) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("import_serial_reservations payload is invalid."));
            return;
        }
        const QString logUuid = payload.value(QStringLiteral("log_uuid")).toString();
        if (!stationCanAccessLog(session, logUuid)) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Station is not authorized for this log."));
            return;
        }
        QString error;
        if (!m_repository.importSerialReservations(
                logUuid, payload.value(QStringLiteral("reservations")).toArray(),
                &error)) {
            const bool conflict = error.contains(QStringLiteral("CONFLICT"));
            sendError(socket, protocolVersion, requestId,
                      conflict ? QStringLiteral("SERIAL_HISTORY_CONFLICT")
                               : QStringLiteral("INVALID_MESSAGE"),
                      error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("serial_history_imported"),
                    QJsonObject{
                        {QStringLiteral("log_uuid"), logUuid},
                        {QStringLiteral("count"),
                         payload.value(QStringLiteral("reservations")).toArray().size()}
                    });
        return;
    }

    if (type == QStringLiteral("select_log")) {
        const QStringList expectedKeys{
            QStringLiteral("contest_definition_sha256"), QStringLiteral("log_uuid")
        };
        const QJsonValue definitionHash =
            payload.value(QStringLiteral("contest_definition_sha256"));
        const QJsonValue logValue = payload.value(QStringLiteral("log_uuid"));
        if (payload.keys() != expectedKeys
            || !(logValue.isNull() || logValue.isString())
            || !(definitionHash.isNull() || definitionHash.isString())) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"), QStringLiteral("select_log is invalid."));
            return;
        }
        if (!session->permissions.contains(QStringLiteral("read"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Read permission is required."));
            return;
        }
        if (logValue.isNull()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNKNOWN_LOG"),
                      QStringLiteral("Creating a central log is not available yet."));
            return;
        }
        const QString logUuid = logValue.toString();
        if (!isUuid(logUuid)) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNKNOWN_LOG"), QStringLiteral("Log UUID is invalid."));
            return;
        }
        if (!stationCanAccessLog(session, logUuid)) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Station is not authorized for this log."));
            return;
        }
        QString error;
        const QVector<Contest> logs = m_repository.repository()->contests(&error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), QStringLiteral("Could not read logs."));
            return;
        }
        const auto found = std::find_if(logs.cbegin(), logs.cend(),
            [&logUuid](const Contest &log) { return log.logUuid == logUuid; });
        if (found == logs.cend()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("UNKNOWN_LOG"), QStringLiteral("Central log does not exist."));
            return;
        }
        const QString yaml =
            m_repository.repository()->contestDefinitionYaml(found->id, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read contest definition."));
            return;
        }
        const QByteArray definitionHashBytes = QCryptographicHash::hash(
            yaml.toUtf8(), QCryptographicHash::Sha256).toHex();
        if (!definitionHash.isNull()
            && definitionHash.toString().toLatin1() != definitionHashBytes) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("CONTEST_MISMATCH"),
                      QStringLiteral("Contest definition hash does not match."));
            return;
        }
        session->activeLogUuid = logUuid;
        const qint64 revision = m_repository.currentRevision(logUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read current revision."));
            return;
        }
        const QString serverUuid = m_repository.serverUuid(&error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read server identity."));
            return;
        }
        const qint64 earliestRevision =
            m_repository.earliestRevision(logUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"),
                      QStringLiteral("Could not read log revision range."));
            return;
        }
        session->lastNotifiedRevision = revision;
        sendMessage(socket, protocolVersion, requestId, QStringLiteral("welcome"),
                    QJsonObject{
                        {QStringLiteral("server_uuid"), serverUuid},
                        {QStringLiteral("server_version"), QStringLiteral("0.1.0")},
                        {QStringLiteral("log_uuid"), logUuid},
                        {QStringLiteral("current_revision"), revision},
                        {QStringLiteral("earliest_revision"), earliestRevision},
                        {QStringLiteral("contest_definition_sha256"),
                         QString::fromLatin1(definitionHashBytes)},
                        {QStringLiteral("definition_yaml"), definitionHash.isNull()
                             ? QJsonValue(yaml) : QJsonValue(QJsonValue::Null)},
                        {QStringLiteral("capabilities"), QJsonArray{
                            QStringLiteral("submit_batch"), QStringLiteral("get_changes"),
                            QStringLiteral("ack_revision"), QStringLiteral("heartbeat"),
                            QStringLiteral("get_status"), QStringLiteral("get_metrics"),
                            QStringLiteral("event_notifications")}}
                    });
        return;
    }

    if (session->activeLogUuid.isEmpty()) {
        sendError(socket, protocolVersion, requestId,
                  QStringLiteral("UNKNOWN_LOG"),
                  QStringLiteral("Select a log before synchronizing."));
        return;
    }
    if (type == QStringLiteral("submit_batch")
        || type == QStringLiteral("import_log_batch")) {
        const bool historicalImport = type == QStringLiteral("import_log_batch");
        if (historicalImport
            ? !session->permissions.contains(QStringLiteral("admin"))
            : !session->permissions.contains(QStringLiteral("write"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      historicalImport
                          ? QStringLiteral("Administrator permission is required to merge historical log data.")
                          : QStringLiteral("Write permission is required."));
            return;
        }
        if (payload.keys() != QStringList{
                QStringLiteral("atomic"), QStringLiteral("log_uuid"),
                QStringLiteral("operations")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !payload.value(QStringLiteral("atomic")).isBool()
            || !payload.value(QStringLiteral("atomic")).toBool()
            || !payload.value(QStringLiteral("operations")).isArray()
            || payload.value(QStringLiteral("operations")).toArray().isEmpty()
            || payload.value(QStringLiteral("operations")).toArray().size()
                   > MaximumBatchSize) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"), QStringLiteral("submit_batch is invalid."));
            return;
        }
        const QJsonArray operations = payload.value(QStringLiteral("operations")).toArray();
        if (!historicalImport) {
            for (const QJsonValue &operationValue : operations) {
                if (!operationValue.isObject()
                    || operationValue.toObject().value(QStringLiteral("station_uuid")).toString()
                           != session->stationUuid) {
                    sendError(socket, protocolVersion, requestId,
                              QStringLiteral("FORBIDDEN"),
                              QStringLiteral("Operation station does not match the authenticated station."));
                    return;
                }
            }
        }
        QJsonArray results;
        QString error;
        QElapsedTimer transactionTimer;
        transactionTimer.start();
        const bool submitted = m_repository.submitBatch(
            session->activeLogUuid, operations, &results, &error);
        if (!historicalImport) {
            const QString outcome = submitted
                ? QStringLiteral("success")
                : error.startsWith(QStringLiteral("VERSION_CONFLICT"))
                    ? QStringLiteral("conflict") : QStringLiteral("error");
            QString metricError;
            if (!m_repository.recordSyncTransactionMetric(
                    session->activeLogUuid, session->stationUuid,
                    transactionTimer.elapsed(), outcome, operations.size(),
                    &metricError)) {
                qWarning().noquote() << "Could not persist sync transaction metric:"
                                     << metricError;
            }
        }
        if (!submitted) {
            QString code = QStringLiteral("VALIDATION_FAILED");
            QJsonObject context;
            if (error.startsWith(QStringLiteral("UNKNOWN_LOG"))) {
                code = QStringLiteral("UNKNOWN_LOG");
            } else if (error.startsWith(QStringLiteral("IDEMPOTENCY_KEY_REUSED"))) {
                code = QStringLiteral("IDEMPOTENCY_KEY_REUSED");
            } else if (error.startsWith(QStringLiteral("VERSION_CONFLICT"))) {
                code = QStringLiteral("VERSION_CONFLICT");
                for (const QJsonValue &operationValue : operations) {
                    const QJsonObject operation = operationValue.toObject();
                    const QJsonObject qso = operation.value(QStringLiteral("qso")).toObject();
                    const QString qsoUuid = qso.value(QStringLiteral("qso_uuid")).toString();
                    const int expectedVersion =
                        operation.value(QStringLiteral("expected_version")).toInt(-1);
                    QString snapshotError;
                    const QJsonObject current =
                        m_repository.currentQso(session->activeLogUuid,
                                                qsoUuid, &snapshotError);
                    if (snapshotError.isEmpty()
                        && current.value(QStringLiteral("qso_version")).toInt()
                               != expectedVersion) {
                        context = QJsonObject{
                            {QStringLiteral("operation_uuid"),
                             operation.value(QStringLiteral("operation_uuid"))},
                            {QStringLiteral("qso_uuid"), qsoUuid},
                            {QStringLiteral("expected_version"), expectedVersion},
                            {QStringLiteral("current_qso"), current}
                        };
                        break;
                    }
                }
            } else if (error.startsWith(QStringLiteral("DUPLICATE_QSO"))) {
                code = QStringLiteral("DUPLICATE_QSO");
            } else if (error.startsWith(QStringLiteral("CONTEST_DATA_UNAVAILABLE"))) {
                code = QStringLiteral("CONTEST_DATA_UNAVAILABLE");
            } else if (error.contains(QStringLiteral("SQLite"))) {
                code = QStringLiteral("INTERNAL_ERROR");
            }
            sendError(socket, protocolVersion, requestId, code, error, false, context);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("batch_result"),
                    QJsonObject{{QStringLiteral("results"), results}});
        const qint64 currentRevision =
            m_repository.currentRevision(session->activeLogUuid, &error);
        if (!error.isEmpty()) {
            qWarning().noquote() << "Could not read committed sync revision:"
                                 << error;
            return;
        }
        notifyLogRevision(session->activeLogUuid, currentRevision);
        return;
    }

    if (type == QStringLiteral("get_changes")) {
        if (!session->permissions.contains(QStringLiteral("read"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Read permission is required."));
            return;
        }
        const QStringList changesKeys = payload.keys();
        const QStringList requiredChangesKeys{
            QStringLiteral("after_revision"), QStringLiteral("log_uuid")
        };
        const QStringList fullChangesKeys{
            QStringLiteral("after_revision"), QStringLiteral("limit"),
            QStringLiteral("log_uuid")
        };
        if ((changesKeys != requiredChangesKeys && changesKeys != fullChangesKeys)
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !isNonNegativeInteger(payload.value(QStringLiteral("after_revision")))
            || (payload.contains(QStringLiteral("limit"))
                && (!isNonNegativeInteger(payload.value(QStringLiteral("limit")))
                    || payload.value(QStringLiteral("limit")).toInt() < 1
                    || payload.value(QStringLiteral("limit")).toInt() > MaximumPageSize))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"), QStringLiteral("get_changes is invalid."));
            return;
        }
        const qint64 afterRevision =
            payload.value(QStringLiteral("after_revision")).toInteger();
        const int limit = payload.contains(QStringLiteral("limit"))
            ? payload.value(QStringLiteral("limit")).toInt() : MaximumPageSize;
        QString error;
        const qint64 earliestRevision =
            m_repository.earliestRevision(session->activeLogUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), error);
            return;
        }
        if (afterRevision < earliestRevision - 1) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("SNAPSHOT_REQUIRED"),
                      QStringLiteral("The requested event range is no longer retained."));
            return;
        }
        const qint64 revision =
            m_repository.currentRevision(session->activeLogUuid, &error);
        const QJsonArray events = m_repository.eventsSince(
            session->activeLogUuid, afterRevision, limit, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), error);
            return;
        }
        QJsonArray protocolEvents;
        qint64 toRevision = afterRevision;
        for (const QJsonValue &eventValue : events) {
            QJsonObject event = eventValue.toObject();
            const QJsonObject snapshot = event.take(QStringLiteral("payload")).toObject();
            const QString storedKind = event.value(QStringLiteral("kind")).toString();
            event.insert(QStringLiteral(
                "kind"), storedKind == QStringLiteral("qso.delete")
                            ? QStringLiteral("qso.delete")
                            : QStringLiteral("qso.upsert"));
            event.insert(QStringLiteral("qso_uuid"),
                         event.value(QStringLiteral("entity_uuid")));
            event.insert(QStringLiteral("qso_version"),
                         snapshot.value(QStringLiteral("qso_version")));
            event.insert(QStringLiteral("station_uuid"),
                         snapshot.value(QStringLiteral("station_uuid")));
            event.insert(QStringLiteral("committed_at_utc"),
                         event.value(QStringLiteral("created_at_utc")));
            event.insert(QStringLiteral("payload"), snapshot);
            protocolEvents.append(event);
            toRevision = event.value(QStringLiteral("revision")).toInteger();
        }
        sendMessage(socket, protocolVersion, requestId, QStringLiteral("changes"),
                    QJsonObject{
                        {QStringLiteral("from_revision"), afterRevision + 1},
                        {QStringLiteral("to_revision"), toRevision},
                        {QStringLiteral("current_revision"), revision},
                        {QStringLiteral("has_more"), toRevision < revision
                             && protocolEvents.size() == limit},
                        {QStringLiteral("events"), protocolEvents}
                    });
        return;
    }

    if (type == QStringLiteral("get_snapshot")) {
        if (!session->permissions.contains(QStringLiteral("read"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Read permission is required."));
            return;
        }
        const QStringList keys = payload.keys();
        const QStringList requiredKeys{
            QStringLiteral("limit"), QStringLiteral("log_uuid"),
            QStringLiteral("offset")
        };
        const QStringList pinnedKeys{
            QStringLiteral("limit"), QStringLiteral("log_uuid"),
            QStringLiteral("offset"), QStringLiteral("snapshot_revision")
        };
        if ((keys != requiredKeys && keys != pinnedKeys)
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !isNonNegativeInteger(payload.value(QStringLiteral("offset")))
            || payload.value(QStringLiteral("offset")).toInteger()
                   > std::numeric_limits<int>::max()
            || !isNonNegativeInteger(payload.value(QStringLiteral("limit")))
            || payload.value(QStringLiteral("limit")).toInt() < 1
            || payload.value(QStringLiteral("limit")).toInt() > 100
            || (payload.contains(QStringLiteral("snapshot_revision"))
                && !isNonNegativeInteger(
                    payload.value(QStringLiteral("snapshot_revision"))))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("get_snapshot is invalid."));
            return;
        }
        QString error;
        const qint64 revision = payload.contains(QStringLiteral("snapshot_revision"))
            ? payload.value(QStringLiteral("snapshot_revision")).toInteger()
            : m_repository.currentRevision(session->activeLogUuid, &error);
        const QJsonObject page = error.isEmpty()
            ? m_repository.snapshotPage(
                  session->activeLogUuid, revision,
                  payload.value(QStringLiteral("offset")).toInt(),
                  payload.value(QStringLiteral("limit")).toInt(), &error)
            : QJsonObject{};
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("snapshot_page"), page);
        return;
    }

    if (type == QStringLiteral("reserve_serial")) {
        if (!session->permissions.contains(QStringLiteral("write"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Write permission is required."));
            return;
        }
        if (payload.keys() != QStringList{
                QStringLiteral("band"), QStringLiteral("log_uuid"),
                QStringLiteral("mode"), QStringLiteral("reservation_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !isUuid(payload.value(QStringLiteral("reservation_uuid")).toString())
            || !payload.value(QStringLiteral("band")).isString()
            || !payload.value(QStringLiteral("mode")).isString()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("reserve_serial is invalid."));
            return;
        }
        QString error;
        const QJsonObject reservation = m_repository.reserveSerial(
            session->activeLogUuid, session->stationUuid,
            payload.value(QStringLiteral("reservation_uuid")).toString(),
            payload.value(QStringLiteral("band")).toString(),
            payload.value(QStringLiteral("mode")).toString(), &error);
        if (!error.isEmpty()) {
            const QString code = error.startsWith(QStringLiteral("UNKNOWN_LOG"))
                ? QStringLiteral("UNKNOWN_LOG")
                : error.startsWith(QStringLiteral("IDEMPOTENCY_KEY_REUSED"))
                    ? QStringLiteral("IDEMPOTENCY_KEY_REUSED")
                    : error.startsWith(QStringLiteral("SERIAL_UNAVAILABLE"))
                        ? QStringLiteral("SERIAL_UNAVAILABLE")
                        : QStringLiteral("INTERNAL_ERROR");
            sendError(socket, protocolVersion, requestId, code, error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("serial_reserved"), reservation);
        return;
    }

    if (type == QStringLiteral("cancel_serial")) {
        if (!session->permissions.contains(QStringLiteral("write"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Write permission is required."));
            return;
        }
        if (payload.keys() != QStringList{
                QStringLiteral("log_uuid"), QStringLiteral("reservation_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !isUuid(payload.value(QStringLiteral("reservation_uuid")).toString())) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("cancel_serial is invalid."));
            return;
        }
        QString error;
        if (!m_repository.cancelSerialReservation(
                session->activeLogUuid, session->stationUuid,
                payload.value(QStringLiteral("reservation_uuid")).toString(),
                session->permissions.contains(QStringLiteral("admin")), &error)) {
            const QString code = error.startsWith(QStringLiteral("FORBIDDEN"))
                ? QStringLiteral("FORBIDDEN")
                : error.startsWith(QStringLiteral("SERIAL_UNAVAILABLE"))
                    ? QStringLiteral("SERIAL_UNAVAILABLE")
                    : QStringLiteral("INTERNAL_ERROR");
            sendError(socket, protocolVersion, requestId, code, error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("serial_cancelled"),
                    QJsonObject{{QStringLiteral("reservation_uuid"),
                                 payload.value(QStringLiteral("reservation_uuid"))}});
        return;
    }

    if (type == QStringLiteral("get_serial_reservations")) {
        if (!session->permissions.contains(QStringLiteral("admin"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Administrator permission is required."));
            return;
        }
        if (payload.keys() != QStringList{QStringLiteral("log_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("get_serial_reservations is invalid."));
            return;
        }
        QString error;
        const QJsonArray reservations =
            m_repository.serialReservations(session->activeLogUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("serial_reservations"),
                    QJsonObject{{QStringLiteral("reservations"), reservations}});
        return;
    }

    if (type == QStringLiteral("adjust_serial")) {
        if (!session->permissions.contains(QStringLiteral("admin"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"),
                      QStringLiteral("Administrator permission is required."));
            return;
        }
        const QJsonValue expectedValue =
            payload.value(QStringLiteral("expected_serial"));
        const QJsonValue correctedValue =
            payload.value(QStringLiteral("corrected_serial"));
        const QString reason = payload.value(QStringLiteral("reason")).toString();
        if (payload.keys() != QStringList{
                QStringLiteral("corrected_serial"), QStringLiteral("expected_serial"),
                QStringLiteral("log_uuid"), QStringLiteral("reason"),
                QStringLiteral("reservation_uuid")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !isUuid(payload.value(QStringLiteral("reservation_uuid")).toString())
            || !isNonNegativeInteger(expectedValue)
            || expectedValue.toInteger() < 1
            || expectedValue.toInteger() > std::numeric_limits<int>::max()
            || !isNonNegativeInteger(correctedValue)
            || correctedValue.toInteger() < 1
            || correctedValue.toInteger() > std::numeric_limits<int>::max()
            || !payload.value(QStringLiteral("reason")).isString()
            || reason.trimmed().isEmpty() || reason.size() > 500) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"),
                      QStringLiteral("adjust_serial payload is invalid."));
            return;
        }
        QString error;
        if (!m_repository.adjustSerialReservation(
                session->activeLogUuid, session->stationUuid,
                payload.value(QStringLiteral("reservation_uuid")).toString(),
                static_cast<int>(expectedValue.toInteger()),
                static_cast<int>(correctedValue.toInteger()), reason, &error)) {
            const QString code = error.startsWith(QStringLiteral("SERIAL_CHANGED"))
                ? QStringLiteral("SERIAL_CHANGED")
                : error.startsWith(QStringLiteral("SERIAL_UNAVAILABLE"))
                    ? QStringLiteral("SERIAL_UNAVAILABLE")
                    : error.startsWith(QStringLiteral("UNKNOWN_LOG"))
                        ? QStringLiteral("UNKNOWN_LOG")
                        : QStringLiteral("INTERNAL_ERROR");
            sendError(socket, protocolVersion, requestId, code, error);
            return;
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("serial_adjusted"),
                    QJsonObject{
                        {QStringLiteral("log_uuid"), session->activeLogUuid},
                        {QStringLiteral("reservation_uuid"),
                         payload.value(QStringLiteral("reservation_uuid"))},
                        {QStringLiteral("previous_serial"), expectedValue},
                        {QStringLiteral("corrected_serial"), correctedValue},
                        {QStringLiteral("reason"), reason.trimmed()}
                    });
        return;
    }

    if (type == QStringLiteral("ack_revision")) {
        if (!session->permissions.contains(QStringLiteral("read"))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("FORBIDDEN"), QStringLiteral("Read permission is required."));
            return;
        }
        if (payload.keys() != QStringList{
                QStringLiteral("log_uuid"), QStringLiteral("revision")}
            || payload.value(QStringLiteral("log_uuid")).toString()
                   != session->activeLogUuid
            || !isNonNegativeInteger(payload.value(QStringLiteral("revision")))) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_MESSAGE"), QStringLiteral("ack_revision is invalid."));
            return;
        }
        const qint64 revision = payload.value(QStringLiteral("revision")).toInteger();
        QString error;
        const qint64 current =
            m_repository.currentRevision(session->activeLogUuid, &error);
        if (!error.isEmpty()) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INTERNAL_ERROR"), error);
            return;
        }
        if (revision > current) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("INVALID_REVISION"),
                      QStringLiteral("Acknowledged revision exceeds the server revision."));
            return;
        }
        const qint64 previous = m_repository.clientCursor(
            session->activeLogUuid, session->stationUuid, &error);
        if (!error.isEmpty() || revision >= previous) {
            if (!m_repository.recordClientCursor(
                    session->activeLogUuid, session->stationUuid, revision, &error)) {
                sendError(socket, protocolVersion, requestId,
                          QStringLiteral("INVALID_REVISION"), error);
                return;
            }
        }
        sendMessage(socket, protocolVersion, requestId,
                    QStringLiteral("acknowledged"),
                    QJsonObject{{QStringLiteral("revision"),
                                 qMax(previous, revision)}});
        return;
    }

    sendError(socket, protocolVersion, requestId,
              QStringLiteral("INVALID_MESSAGE"),
              QStringLiteral("Message type is not supported."));
}

void SyncServer::sendMessage(QSslSocket *socket, int protocolVersion,
                             const QString &requestId, const QString &type,
                             const QJsonObject &payload)
{
    if (socket == nullptr || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const QJsonValue requestValue = requestId.isEmpty()
        ? QJsonValue(QJsonValue::Null) : QJsonValue(requestId);
    const QJsonObject response{
        {QStringLiteral("protocol_version"), protocolVersion},
        {QStringLiteral("request_id"), requestValue},
        {QStringLiteral("type"), type},
        {QStringLiteral("payload"), payload}
    };
    const QByteArray frame = frameBytes(response);
    if (frame.size() > MaximumFrameSize + 4) {
        if (type != QStringLiteral("error")) {
            sendError(socket, protocolVersion, requestId,
                      QStringLiteral("MESSAGE_TOO_LARGE"),
                      QStringLiteral("Response exceeds the maximum frame size."));
        }
        return;
    }
    socket->write(frame);
}

void SyncServer::notifyLogRevision(const QString &logUuid, qint64 revision)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        ClientSession *session = it.value();
        QSslSocket *socket = it.key();
        if (!session->authenticated || !session->tlsReady
            || session->activeLogUuid != logUuid
            || !session->permissions.contains(QStringLiteral("read"))
            || !session->capabilities.contains(
                QStringLiteral("event_notifications"))
            || session->lastNotifiedRevision >= revision
            || socket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }
        sendMessage(socket, session->protocolVersion, QString(),
                    QStringLiteral("event_available"),
                    QJsonObject{
                        {QStringLiteral("log_uuid"), logUuid},
                        {QStringLiteral("current_revision"), revision}
                    });
        session->lastNotifiedRevision = revision;
    }
}

void SyncServer::sendError(QSslSocket *socket, int protocolVersion,
                           const QString &requestId, const QString &code,
                           const QString &message, bool retryable,
                           const QJsonObject &context)
{
    QJsonObject payload{
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message},
        {QStringLiteral("retryable"), retryable}
    };
    if (!context.isEmpty()) {
        payload.insert(QStringLiteral("context"), context);
    }
    sendMessage(socket, qMax(0, protocolVersion), requestId, QStringLiteral("error"),
                payload);
}

void SyncServer::closeSession(QSslSocket *socket)
{
    ClientSession *session = m_sessions.take(socket);
    if (session == nullptr) {
        return;
    }
    delete session;
    QObject::disconnect(socket, nullptr, this, nullptr);
    socket->disconnectFromHost();
    if (socket->state() == QAbstractSocket::UnconnectedState) {
        socket->deleteLater();
    }
}

} // namespace logger
