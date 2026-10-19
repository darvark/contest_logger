#pragma once

#include "logger/data/central_repository.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QSet>
#include <QString>

#include <memory>

class QSslSocket;
class QTimer;

namespace logger {

class TlsTcpServer;

class SyncServer final : public QObject {
    Q_OBJECT

public:
    explicit SyncServer(CentralRepository &repository, QObject *parent = nullptr);
    ~SyncServer() override;

    bool listen(const QString &address, quint16 port,
                const QString &certificatePath, const QString &privateKeyPath,
                const QString &tokenFilePath, int maximumConnections,
                QString *error = nullptr);
    void close();
    quint16 serverPort() const;
    int activeConnections() const;

private:
    struct ClientSession;

    void beginTls(QSslSocket *socket);
    bool stationCanAccessLog(const ClientSession *session,
                             const QString &logUuid) const;
    void processFrames(QSslSocket *socket);
    void processMessage(QSslSocket *socket, const QByteArray &frame);
    void sendMessage(QSslSocket *socket, int protocolVersion,
                     const QString &requestId, const QString &type,
                     const QJsonObject &payload);
    void sendError(QSslSocket *socket, int protocolVersion,
                   const QString &requestId, const QString &code,
                   const QString &message, bool retryable = false,
                   const QJsonObject &context = {});
    void closeSession(QSslSocket *socket);
    void touchSession(QSslSocket *socket);

    CentralRepository &m_repository;
    std::unique_ptr<TlsTcpServer> m_server;
    QHash<QSslSocket *, ClientSession *> m_sessions;
    QHash<QString, QJsonObject> m_credentials;
    QSslCertificate m_certificate;
    QSslKey m_privateKey;
    int m_maximumConnections = 0;
};

} // namespace logger
