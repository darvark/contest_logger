#pragma once

#include "logger/domain/types.h"

#include <QObject>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace logger {

struct DxClusterConfig {
    QString host;
    int port = 7300;
    QString callsign;
    // Initial reconnect backoff (doubles up to 30 s); configurable for tests.
    int reconnectInitialMs = 1000;
    // Callsign is sent when a login prompt is seen; if no prompt arrives within
    // this time it is sent anyway (for servers that do not prompt).
    int loginFallbackMs = 1500;
};

struct DxClusterStatus {
    ConnectionState state = ConnectionState::Disconnected;
    QString message;
    int reconnectDelaySeconds = 0;
};

class DxSpotParser final {
public:
    static std::optional<Spot> parseLine(const QByteArray &line,
                                         const QDateTime &receivedAtUtc,
                                         QString *error = nullptr);
};

class DxClusterWorker final : public QObject {
    Q_OBJECT

public:
    explicit DxClusterWorker(QObject *parent = nullptr);
    ~DxClusterWorker() override;

public slots:
    void start(const logger::DxClusterConfig &config);
    void stop();
    void sendCommand(const QString &command);
    void sendSpot(const logger::Spot &spot);

signals:
    void statusChanged(const logger::DxClusterStatus &status);
    void spotReceived(const logger::Spot &spot);

private slots:
    void connectToCluster();
    void readAvailableData();
    void socketDisconnected();
    void socketError();
    void reconnect();

private:
    void scheduleReconnect(const QString &message);
    void processLine(const QByteArray &line);
    void sendTelnetReply(quint8 command, quint8 option);
    void sendLogin();
    static bool looksLikeLoginPrompt(const QByteArray &text);

    DxClusterConfig m_config;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_loginTimer = nullptr;
    QByteArray m_lineBuffer;
    QByteArray m_telnetBuffer;
    int m_telnetState = 0;
    int m_reconnectDelayMs = 1000;
    bool m_loggedIn = false;
    bool m_requested = false;
};

class DxClusterController final : public QObject {
    Q_OBJECT

public:
    explicit DxClusterController(QObject *parent = nullptr);
    ~DxClusterController() override;

    void start(const DxClusterConfig &config);
    void stop();
    void sendCommand(const QString &command);
    void sendSpot(const logger::Spot &spot);

signals:
    void startRequested(const logger::DxClusterConfig &config);
    void stopRequested();
    void commandRequested(const QString &command);
    void spotRequested(const logger::Spot &spot);
    void statusChanged(const logger::DxClusterStatus &status);
    void spotReceived(const logger::Spot &spot);

private:
    QThread m_thread;
    DxClusterWorker *m_worker = nullptr;
};

} // namespace logger

Q_DECLARE_METATYPE(logger::DxClusterConfig)
Q_DECLARE_METATYPE(logger::DxClusterStatus)
