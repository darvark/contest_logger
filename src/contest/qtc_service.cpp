#include "logger/contest/qtc_service.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool validUuid(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
                       "[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch();
}

QJsonObject messageToJson(const QtcMessage &message)
{
    return {
        {QStringLiteral("timestamp_utc"),
         message.timestampUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("callsign"), message.callsign.trimmed().toUpper()},
        {QStringLiteral("serial_number"), message.serialNumber}
    };
}

} // namespace

QtcTransfer QtcService::createTransfer(
    const Qso &partnerQso, QtcDirection direction, const QVector<Qso> &logQsos)
{
    QtcTransfer transfer;
    transfer.qtcUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    transfer.partnerQsoUuid = partnerQso.qsoUuid;
    transfer.partnerCallsign = partnerQso.callsign.trimmed().toUpper();
    transfer.direction = direction;
    int nextNumber = 1;
    for (const Qso &qso : logQsos) {
        for (const QtcTransfer &existing : qso.qtcTransfers) {
            if (existing.direction == direction
                && existing.partnerCallsign.compare(
                       transfer.partnerCallsign, Qt::CaseInsensitive) == 0) {
                nextNumber = qMax(nextNumber, existing.groupNumber + 1);
            }
        }
    }
    transfer.groupNumber = nextNumber;
    return transfer;
}

bool QtcService::validate(
    const Qso &partnerQso, const QtcTransfer &transfer,
    const QVector<QtcTransfer> &existing, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    static const QRegularExpression callsignPattern(
        QStringLiteral("^[A-Z0-9/]{3,20}$"));
    if (!validUuid(transfer.qtcUuid)
        || transfer.partnerQsoUuid != partnerQso.qsoUuid
        || transfer.partnerCallsign.trimmed().compare(
               partnerQso.callsign.trimmed(), Qt::CaseInsensitive) != 0
        || !callsignPattern.match(transfer.partnerCallsign.trimmed().toUpper()).hasMatch()) {
        setError(error, QStringLiteral("QTC partner identity is invalid."));
        return false;
    }
    if (transfer.groupNumber < 1 || transfer.groupNumber > 10) {
        setError(error, QStringLiteral("WAE QTC group number must be between 1 and 10."));
        return false;
    }
    if (transfer.messages.isEmpty() || transfer.messages.size() > 10) {
        setError(error, QStringLiteral("A WAE QTC group must contain 1 to 10 messages."));
        return false;
    }
    for (qsizetype index = 0; index < transfer.messages.size(); ++index) {
        const QtcMessage &message = transfer.messages.at(index);
        if (!message.timestampUtc.isValid()
            || message.timestampUtc.timeSpec() == Qt::LocalTime
            || !callsignPattern.match(message.callsign.trimmed().toUpper()).hasMatch()
            || message.serialNumber < 1) {
            setError(error, QStringLiteral("QTC message %1 has invalid time, callsign, or serial.")
                                .arg(index + 1));
            return false;
        }
    }
    for (const QtcTransfer &other : existing) {
        if (other.qtcUuid != transfer.qtcUuid
            && other.direction == transfer.direction
            && other.partnerCallsign.compare(transfer.partnerCallsign,
                                             Qt::CaseInsensitive) == 0
            && other.groupNumber == transfer.groupNumber) {
            setError(error, QStringLiteral(
                "This QTC group number is already used for the partner and direction."));
            return false;
        }
    }
    return true;
}

bool QtcService::validateJson(
    const Qso &partnerQso, const QJsonValue &value, QString *error)
{
    if (!value.isArray()) {
        setError(error, QStringLiteral("QTC data must be an array."));
        return false;
    }
    const QJsonArray array = value.toArray();
    const QVector<QtcTransfer> transfers = fromJson(array);
    if (transfers.size() != array.size()) {
        setError(error, QStringLiteral("QTC data contains a malformed transfer."));
        return false;
    }
    QSet<QString> transferUuids;
    for (qsizetype index = 0; index < array.size(); ++index) {
        const QJsonValue value = array.at(index);
        if (!value.isObject()) {
            setError(error, QStringLiteral("QTC transfer must be an object."));
            return false;
        }
        const QJsonObject object = value.toObject();
        const QJsonValue groupNumber = object.value(QStringLiteral("group_number"));
        if (object.keys() != QStringList{
                QStringLiteral("direction"), QStringLiteral("group_number"),
                QStringLiteral("messages"), QStringLiteral("partner_callsign"),
                QStringLiteral("partner_qso_uuid"), QStringLiteral("qtc_uuid")}
            || !groupNumber.isDouble()
            || groupNumber.toDouble(-1) != groupNumber.toInt(-1)
            || transferUuids.contains(
                object.value(QStringLiteral("qtc_uuid")).toString())) {
            setError(error, QStringLiteral("QTC transfer fields are invalid."));
            return false;
        }
        transferUuids.insert(object.value(QStringLiteral("qtc_uuid")).toString());
        const QJsonArray messages = object.value(QStringLiteral("messages")).toArray();
        if (!object.value(QStringLiteral("messages")).isArray()) {
            setError(error, QStringLiteral("QTC messages must be an array."));
            return false;
        }
        for (const QJsonValue &message : messages) {
            if (!message.isObject()
                || message.toObject().keys() != QStringList{
                    QStringLiteral("callsign"), QStringLiteral("serial_number"),
                    QStringLiteral("timestamp_utc")}
                || !message.toObject().value(QStringLiteral("callsign")).isString()
                || !message.toObject().value(QStringLiteral("timestamp_utc")).isString()
                || !message.toObject().value(QStringLiteral("timestamp_utc"))
                        .toString().endsWith(QLatin1Char('Z'))
                || !message.toObject().value(QStringLiteral("serial_number")).isDouble()
                || message.toObject().value(QStringLiteral("serial_number")).toDouble(-1)
                    != message.toObject().value(QStringLiteral("serial_number")).toInt(-1)) {
                setError(error, QStringLiteral("QTC message fields are invalid."));
                return false;
            }
        }
        QVector<QtcTransfer> otherTransfers;
        for (qsizetype other = 0; other < transfers.size(); ++other) {
            if (other != index) {
                otherTransfers.append(transfers.at(other));
            }
        }
        if (!validate(partnerQso, transfers.at(index), otherTransfers, error)) {
            return false;
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

QJsonArray QtcService::toJson(const QVector<QtcTransfer> &transfers)
{
    QJsonArray result;
    for (const QtcTransfer &transfer : transfers) {
        QJsonArray messages;
        for (const QtcMessage &message : transfer.messages) {
            messages.append(messageToJson(message));
        }
        result.append(QJsonObject{
            {QStringLiteral("qtc_uuid"), transfer.qtcUuid},
            {QStringLiteral("partner_qso_uuid"), transfer.partnerQsoUuid},
            {QStringLiteral("partner_callsign"), transfer.partnerCallsign.trimmed().toUpper()},
            {QStringLiteral("direction"), directionName(transfer.direction)},
            {QStringLiteral("group_number"), transfer.groupNumber},
            {QStringLiteral("messages"), messages}
        });
    }
    return result;
}

QVector<QtcTransfer> QtcService::fromJson(const QJsonArray &array)
{
    QVector<QtcTransfer> result;
    result.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        QtcTransfer transfer;
        transfer.qtcUuid = object.value(QStringLiteral("qtc_uuid")).toString();
        transfer.partnerQsoUuid =
            object.value(QStringLiteral("partner_qso_uuid")).toString();
        transfer.partnerCallsign =
            object.value(QStringLiteral("partner_callsign")).toString();
        if (!parseDirection(object.value(QStringLiteral("direction")).toString(),
                            &transfer.direction)) {
            continue;
        }
        transfer.groupNumber = object.value(QStringLiteral("group_number")).toInt();
        for (const QJsonValue &messageValue :
             object.value(QStringLiteral("messages")).toArray()) {
            const QJsonObject messageObject = messageValue.toObject();
            QtcMessage message;
            message.timestampUtc = QDateTime::fromString(
                messageObject.value(QStringLiteral("timestamp_utc")).toString(),
                Qt::ISODateWithMs);
            if (!message.timestampUtc.isValid()) {
                message.timestampUtc = QDateTime::fromString(
                    messageObject.value(QStringLiteral("timestamp_utc")).toString(),
                    Qt::ISODate);
            }
            message.timestampUtc = message.timestampUtc.toUTC();
            message.callsign =
                messageObject.value(QStringLiteral("callsign")).toString();
            message.serialNumber =
                messageObject.value(QStringLiteral("serial_number")).toInt();
            transfer.messages.append(std::move(message));
        }
        result.append(std::move(transfer));
    }
    return result;
}

QString QtcService::directionName(QtcDirection direction)
{
    return direction == QtcDirection::Sent
        ? QStringLiteral("sent") : QStringLiteral("received");
}

bool QtcService::parseDirection(const QString &value, QtcDirection *direction)
{
    if (direction == nullptr) {
        return false;
    }
    if (value == QStringLiteral("sent")) {
        *direction = QtcDirection::Sent;
        return true;
    }
    if (value == QStringLiteral("received")) {
        *direction = QtcDirection::Received;
        return true;
    }
    return false;
}

} // namespace logger
