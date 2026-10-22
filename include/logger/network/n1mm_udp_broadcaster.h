#pragma once

#include "logger/domain/types.h"

#include <QHostAddress>
#include <QString>
#include <QVector>
#include <QUdpSocket>

namespace logger {

struct N1mmUdpConfiguration {
    bool enabled = false;
    QHostAddress address;
    quint16 port = 12060;
};

class N1mmUdpBroadcaster final {
public:
    explicit N1mmUdpBroadcaster(N1mmUdpConfiguration configuration = {});

    const N1mmUdpConfiguration &configuration() const;
    bool setConfiguration(const N1mmUdpConfiguration &configuration,
                          QString *error = nullptr);
    static QByteArray serializeQso(const Qso &qso, const QString &contestName,
                                   const QString &operatorName);
    bool broadcastQso(const Qso &qso, const QString &contestName,
                      const QString &operatorName, QString *error = nullptr);
    bool broadcastLog(const QVector<Qso> &qsos, const QString &contestName,
                      const QString &operatorName, int *sentCount = nullptr,
                      QString *error = nullptr);

private:
    N1mmUdpConfiguration m_configuration;
    QUdpSocket m_socket;
};

} // namespace logger
