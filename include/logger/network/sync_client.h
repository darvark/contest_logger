#pragma once

#include "logger/data/sqlite_repository.h"

#include <QHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSslCertificate>
#include <QString>
#include <QStringList>

#include <memory>

class QSslSocket;
class QTimer;

namespace logger {

struct SyncClientConfiguration {
    QString host;
    quint16 port = 0;
    QString peerName;
    QString stationUuid;
    QString computerName;
    QString stationCallsign;
    QString operatorName;
    QString operatingBand;
    QString operatingMode;
    bool trustedNetworkMode = false;
    bool useServerActiveLog = false;
    QString token;
    QString logUuid;
    QString contestDefinitionSha256;
    QList<QSslCertificate> caCertificates;
};

class SyncClient final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Handshaking,
        Authenticating,
        SelectingLog,
        Ready,
        Backoff,
        Error
    };
    Q_ENUM(State)

    explicit SyncClient(QObject *parent = nullptr);
    ~SyncClient() override;

    State state() const;
    QString serverUuid() const;
    qint64 currentRevision() const;
    QString contestDefinitionYaml() const;
    bool isReadOnly() const;
    void setLastAppliedRevision(qint64 revision);
    bool startSynchronization(SqliteRepository &repository,
                              QString *error = nullptr);
    void stopSynchronization();
    bool synchronizeNow();
    void flushPendingChanges();
    bool selectRemoteLog(const QString &logUuid,
                         const QString &contestDefinitionSha256);
    QString createRemoteLog(const Contest &log, const QString &definitionYaml);
    QString importSerialHistory(const QString &logUuid,
                                const QJsonArray &reservations);
    QString importLogBatch(const QJsonArray &operations);

public slots:
    void connectToServer(const logger::SyncClientConfiguration &configuration);
    void disconnectFromServer();
    QString submitBatch(const QJsonArray &operations);
    QString getChanges(qint64 afterRevision, int limit = 500);
    QString acknowledgeRevision(qint64 revision);
    QString getStatus();
    QString checkConsistency();
    void setStationIdentity(const QString &stationCallsign, const QString &operatorName);
    void setOperatingState(const QString &band, const QString &mode);
    QString getMetrics();
    QString reserveSerial(const QString &reservationUuid, const QString &band,
                          const QString &mode);
    QString cancelSerial(const QString &reservationUuid);
    QString getSerialReservations();
    QString adjustSerialReservation(const QString &reservationUuid,
                                    int expectedSerial, int correctedSerial,
                                    const QString &reason);

signals:
    void stateChanged(logger::SyncClient::State state, const QString &message);
    void readOnlyChanged(bool readOnly);
    void ready(const QString &serverUuid, qint64 currentRevision,
               const QString &contestDefinitionYaml);
    void disconnected(const QString &message);
    void responseReceived(const QString &requestId, const QString &type,
                          const QJsonObject &payload);
    void remoteChangesAvailable(qint64 currentRevision);
    void requestFailed(const QString &requestId, const QString &code,
                       const QString &message, bool retryable);
    void logsDiscovered(const QJsonArray &logs);
    void logBound(const QString &logUuid);
    void synchronizationStarted();
    void synchronizationFinished(int uploadedOperations, int downloadedEvents,
                                 qint64 elapsedMs, bool success,
                                 const QString &detail);

private:
    struct PendingRequest {
        QString type;
        QDateTime deadline;
        bool handshake = false;
        QJsonObject payload;
    };

    QString sendRequest(const QString &type, const QJsonObject &payload,
                        bool handshakeRequest = false);
    void beginHandshake();
    void beginAuthentication();
    void selectLog();
    void readFrames();
    void processResponse(const QJsonObject &message);
    void handleSocketFailure(const QString &message);
    void scheduleReconnect(const QString &message);
    void setState(State state, const QString &message = {});
    void heartbeat();
    void expireRequests();
    void closeSocket();
    void finishManualSynchronization(bool success, const QString &detail);
    void pumpSynchronization();
    QString pumpSerialReservationRequests(const QString &operation = {},
                                         const QString &reservationUuid = {});
    void beginSnapshot();
    void requestSnapshotPage();

    std::unique_ptr<QSslSocket> m_socket;
    std::unique_ptr<QTimer> m_reconnectTimer;
    std::unique_ptr<QTimer> m_heartbeatTimer;
    std::unique_ptr<QTimer> m_requestTimer;
    std::unique_ptr<QTimer> m_syncTimer;
    SyncClientConfiguration m_configuration;
    SqliteRepository *m_repository = nullptr;
    QByteArray m_input;
    QHash<QString, PendingRequest> m_pendingRequests;
    QString m_handshakeRequestId;
    QString m_outboxRequestId;
    QStringList m_outboxOperationUuids;
    QString m_changesRequestId;
    QString m_snapshotRequestId;
    QString m_sessionUuid;
    QString m_serverUuid;
    QString m_definitionYaml;
    QJsonArray m_snapshotQsos;
    QString m_snapshotSha256;
    qint64 m_currentRevision = 0;
    qint64 m_lastAppliedRevision = 0;
    qint64 m_snapshotRevision = -1;
    int m_snapshotOffset = 0;
    int m_snapshotTotal = -1;
    int m_reconnectDelayMs = 1000;
    bool m_hasConfiguration = false;
    bool m_manualDisconnect = false;
    bool m_socketFailureHandled = false;
    bool m_synchronizationEnabled = false;
    bool m_syncPending = false;
    bool m_readOnly = false;
    bool m_stationIdentitySupported = false;
    bool m_operatingStateSupported = false;
    bool m_trustedNetworkSupported = false;
    int m_outboxBatchSize = 16;
    int m_manualUploadedOperations = 0;
    int m_manualDownloadedEvents = 0;
    QElapsedTimer m_manualSyncTimer;
    bool m_manualSyncActive = false;
    State m_state = State::Disconnected;
};

} // namespace logger

Q_DECLARE_METATYPE(logger::SyncClient::State)
Q_DECLARE_METATYPE(logger::SyncClientConfiguration)
