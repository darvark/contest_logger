#pragma once

#include "logger/domain/types.h"

#include <QJsonArray>
#include <QString>

namespace logger {

class QtcService final {
public:
    static QtcTransfer createTransfer(const Qso &partnerQso,
                                     QtcDirection direction,
                                     const QVector<Qso> &logQsos);
    static bool validate(const Qso &partnerQso, const QtcTransfer &transfer,
                         const QVector<QtcTransfer> &existing,
                         QString *error = nullptr);
    static bool validateJson(const Qso &partnerQso, const QJsonValue &value,
                             QString *error = nullptr);
    static QJsonArray toJson(const QVector<QtcTransfer> &transfers);
    static QVector<QtcTransfer> fromJson(const QJsonArray &array);
    static QString directionName(QtcDirection direction);
    static bool parseDirection(const QString &value, QtcDirection *direction);
};

} // namespace logger
