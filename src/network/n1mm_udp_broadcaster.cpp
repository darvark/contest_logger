#include "logger/network/n1mm_udp_broadcaster.h"

#include <QXmlStreamWriter>
#include <QRegularExpression>

#include <utility>

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

QString modeForN1mm(const QString &mode)
{
    const QString normalized = mode.trimmed().toUpper();
    if (normalized == QStringLiteral("USB")
        || normalized == QStringLiteral("LSB")
        || normalized == QStringLiteral("SSB")) {
        return QStringLiteral("SSB");
    }
    if (normalized == QStringLiteral("FT8")
        || normalized == QStringLiteral("FT4")) {
        return QStringLiteral("FT8");
    }
    return normalized;
}

QString reportValue(const Qso &qso, bool sent)
{
    const QMap<QString, QString> &fields =
        sent ? qso.sentExchangeFields : qso.receivedExchangeFields;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (it.key().compare(QStringLiteral("rst"), Qt::CaseInsensitive) == 0
            || it.key().compare(QStringLiteral("report"), Qt::CaseInsensitive) == 0) {
            return it.value().trimmed().toUpper();
        }
    }
    const QString legacy = sent ? qso.sentExchange : qso.receivedExchange;
    const QStringList tokens = legacy.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
    if (!tokens.isEmpty()) {
        return tokens.first().toUpper();
    }
    return qso.mode.compare(QStringLiteral("CW"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("599") : QStringLiteral("59");
}

QString serialValue(const Qso &qso, bool sent)
{
    const QMap<QString, QString> &fields =
        sent ? qso.sentExchangeFields : qso.receivedExchangeFields;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (it.key().compare(QStringLiteral("serial"), Qt::CaseInsensitive) == 0) {
            return it.value().trimmed();
        }
    }
    const QString exchange = sent ? qso.sentExchange : qso.receivedExchange;
    const QStringList tokens = exchange.split(QRegularExpression(QStringLiteral("\\s+")),
                                               Qt::SkipEmptyParts);
    return tokens.size() > 1 ? tokens.at(1) : QString();
}

} // namespace

N1mmUdpBroadcaster::N1mmUdpBroadcaster(N1mmUdpConfiguration configuration)
    : m_configuration(std::move(configuration))
{
}

const N1mmUdpConfiguration &N1mmUdpBroadcaster::configuration() const
{
    return m_configuration;
}

bool N1mmUdpBroadcaster::setConfiguration(
    const N1mmUdpConfiguration &configuration, QString *error)
{
    if (configuration.enabled
        && (configuration.address.isNull() || configuration.port == 0)) {
        setError(error, QStringLiteral(
            "An IP address and UDP port are required when N1MM broadcast is enabled."));
        return false;
    }
    m_configuration = configuration;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

QByteArray N1mmUdpBroadcaster::serializeQso(
    const Qso &qso, const QString &contestName, const QString &operatorName)
{
    QByteArray xml;
    QXmlStreamWriter writer(&xml);
    writer.setAutoFormatting(false);
    writer.writeStartDocument();
    writer.writeStartElement(QStringLiteral("contactinfo"));
    const auto element = [&writer](const QString &name, const QString &value) {
        writer.writeTextElement(name, value);
    };
    element(QStringLiteral("contestname"), contestName.trimmed());
    element(QStringLiteral("timestamp"),
            qso.timestampUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    element(QStringLiteral("mycall"), qso.stationCallsign.trimmed().toUpper());
    element(QStringLiteral("band"), qso.band.trimmed().toLower().remove(QLatin1Char('m')));
    element(QStringLiteral("rxfreq"),
            qso.frequencyHz > 0
                ? QString::number(static_cast<double>(qso.frequencyHz) / 1000.0, 'f', 1)
                : QString());
    element(QStringLiteral("mode"), modeForN1mm(qso.mode));
    element(QStringLiteral("call"), qso.callsign.trimmed().toUpper());
    element(QStringLiteral("operator"), operatorName.trimmed());
    element(QStringLiteral("snt"), reportValue(qso, true));
    element(QStringLiteral("sntnr"), serialValue(qso, true));
    element(QStringLiteral("rcv"), reportValue(qso, false));
    element(QStringLiteral("rcvnr"), serialValue(qso, false));
    element(QStringLiteral("exchange1"), qso.sentExchange.trimmed());
    element(QStringLiteral("exchange2"), qso.receivedExchange.trimmed());
    element(QStringLiteral("points"), QString::number(qso.points));
    element(QStringLiteral("isMultiplier1"), QStringLiteral("0"));
    element(QStringLiteral("isMultiplier2"), QStringLiteral("0"));
    element(QStringLiteral("isRunQSO"), QStringLiteral("1"));
    writer.writeEndElement();
    writer.writeEndDocument();
    return xml;
}

bool N1mmUdpBroadcaster::broadcastQso(
    const Qso &qso, const QString &contestName, const QString &operatorName,
    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (!m_configuration.enabled) {
        return true;
    }
    const QByteArray message = serializeQso(qso, contestName, operatorName);
    if (m_socket.writeDatagram(message, m_configuration.address,
                               m_configuration.port) != message.size()) {
        setError(error, m_socket.errorString());
        return false;
    }
    return true;
}

bool N1mmUdpBroadcaster::broadcastLog(
    const QVector<Qso> &qsos, const QString &contestName,
    const QString &operatorName, int *sentCount, QString *error)
{
    if (sentCount != nullptr) {
        *sentCount = 0;
    }
    if (error != nullptr) {
        error->clear();
    }
    if (!m_configuration.enabled) {
        setError(error, QStringLiteral("Enable N1MM UDP broadcast before sending a log."));
        return false;
    }
    for (const Qso &qso : qsos) {
        if (!broadcastQso(qso, contestName, operatorName, error)) {
            return false;
        }
        if (sentCount != nullptr) {
            ++*sentCount;
        }
    }
    return true;
}

} // namespace logger
