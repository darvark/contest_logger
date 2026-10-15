#pragma once

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QVector>

#include <optional>

namespace logger {

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error,
    Unsupported
};

struct Qso {
    qint64 id = 0;
    qint64 contestId = 0;
    QString callsign;
    QString stationCallsign;
    QString band;
    QDateTime timestampUtc;
    qint64 frequencyHz = 0;
    QString mode;
    QString sentExchange;
    QString receivedExchange;
    QMap<QString, QString> sentExchangeFields;
    QMap<QString, QString> receivedExchangeFields;
    int points = 0;
};

struct Contest {
    qint64 id = 0;
    QString code;
    QString name;
    QString description;
    std::optional<QDateTime> startsAtUtc;
    std::optional<QDateTime> endsAtUtc;
};

struct Station {
    QString callsign;
    QString operatorName;
    QString grid;
    QString powerClass;
};

struct Vfo {
    QString id;
    int number = 0;
    qint64 frequencyHz = 0;
    QString mode;
    ConnectionState state = ConnectionState::Disconnected;
};

struct Radio {
    QString id;
    QString name;
    QString model;
    qint64 frequencyHz = 0;
    QString mode;
    ConnectionState state = ConnectionState::Disconnected;
    QVector<Vfo> vfos;
};

struct Spot {
    qint64 id = 0;
    QString callsign;
    qint64 frequencyHz = 0;
    QString band;
    QString mode;
    QString spotter;
    QString comment;
    QDateTime timestampUtc;
    QString source;
};

} // namespace logger

Q_DECLARE_METATYPE(logger::Spot)
