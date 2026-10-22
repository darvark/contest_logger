#include "logger/contest/qtc_service.h"
#include "logger/network/n1mm_udp_broadcaster.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QUdpSocket>
#include <QtTest>

namespace {

logger::Qso partnerQso()
{
    logger::Qso qso;
    qso.qsoUuid = QStringLiteral("11111111-1111-4111-8111-111111111111");
    qso.callsign = QStringLiteral("DL1ABC");
    qso.stationCallsign = QStringLiteral("K1TEST");
    qso.band = QStringLiteral("20m");
    qso.mode = QStringLiteral("CW");
    qso.timestampUtc = QDateTime::fromString(
        QStringLiteral("2026-10-15T10:00:00.000Z"), Qt::ISODateWithMs);
    qso.frequencyHz = 14025000;
    qso.sentExchange = QStringLiteral("001");
    qso.receivedExchange = QStringLiteral("027");
    qso.sentExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("001"));
    qso.receivedExchangeFields.insert(QStringLiteral("serial"), QStringLiteral("027"));
    return qso;
}

logger::QtcTransfer qtcTransfer()
{
    logger::QtcTransfer transfer;
    transfer.qtcUuid = QStringLiteral("22222222-2222-4222-8222-222222222222");
    transfer.partnerQsoUuid = partnerQso().qsoUuid;
    transfer.partnerCallsign = QStringLiteral("DL1ABC");
    transfer.direction = logger::QtcDirection::Received;
    transfer.groupNumber = 1;
    transfer.messages.append({
        QDateTime::fromString(QStringLiteral("2026-10-15T09:40:00.000Z"),
                              Qt::ISODateWithMs),
        QStringLiteral("DL2XYZ"), 18
    });
    return transfer;
}

} // namespace

class QtcN1mmTest final : public QObject {
    Q_OBJECT

private slots:
    void assignsQtcGroupNumberPerPartnerAndDirection();
    void validatesWaeQtcMessagesAndSerializesForSync();
    void formatsN1mmContactXml();
    void broadcastsDatagramsToConfiguredReceiver();
    void udpSendDoesNotRequireAListeningReceiver();
};

void QtcN1mmTest::assignsQtcGroupNumberPerPartnerAndDirection()
{
    logger::Qso qso = partnerQso();
    qso.qtcTransfers.append(qtcTransfer());
    logger::Qso nextPartnerQso = qso;
    nextPartnerQso.qsoUuid =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    const auto next = logger::QtcService::createTransfer(
        nextPartnerQso, logger::QtcDirection::Received, {qso});
    QCOMPARE(next.groupNumber, 2);
    const auto sent = logger::QtcService::createTransfer(
        nextPartnerQso, logger::QtcDirection::Sent, {qso});
    QCOMPARE(sent.groupNumber, 1);
}

void QtcN1mmTest::validatesWaeQtcMessagesAndSerializesForSync()
{
    logger::Qso qso = partnerQso();
    const logger::QtcTransfer transfer = qtcTransfer();
    QString error;
    QVERIFY2(logger::QtcService::validate(qso, transfer, {}, &error),
             qPrintable(error));
    const QJsonArray wire = logger::QtcService::toJson({transfer});
    QVERIFY(logger::QtcService::validateJson(qso, wire, &error));
    const QVector<logger::QtcTransfer> restored =
        logger::QtcService::fromJson(wire);
    QCOMPARE(restored.size(), 1);
    QCOMPARE(restored.first().qtcUuid, transfer.qtcUuid);
    QCOMPARE(restored.first().messages.first().timestampUtc,
             transfer.messages.first().timestampUtc);

    logger::QtcTransfer invalid = transfer;
    invalid.messages.clear();
    QVERIFY(!logger::QtcService::validate(qso, invalid, {}, &error));
    QVERIFY(!error.isEmpty());
    invalid = transfer;
    invalid.messages[0].serialNumber = 0;
    QVERIFY(!logger::QtcService::validate(qso, invalid, {}, &error));

    QJsonArray malformed = wire;
    QJsonObject malformedTransfer = malformed.first().toObject();
    malformedTransfer.insert(QStringLiteral("direction"), QStringLiteral("other"));
    malformed[0] = malformedTransfer;
    QVERIFY(!logger::QtcService::validateJson(qso, malformed, &error));

    malformed = wire;
    malformedTransfer = malformed.first().toObject();
    malformedTransfer.insert(QStringLiteral("group_number"), 1.5);
    malformed[0] = malformedTransfer;
    QVERIFY(!logger::QtcService::validateJson(qso, malformed, &error));

    malformed = wire;
    malformedTransfer = malformed.first().toObject();
    QJsonArray messages = malformedTransfer.value(QStringLiteral("messages")).toArray();
    QJsonObject message = messages.first().toObject();
    message.insert(QStringLiteral("timestamp_utc"),
                   QStringLiteral("2026-10-15T09:40:00"));
    messages[0] = message;
    malformedTransfer.insert(QStringLiteral("messages"), messages);
    malformed[0] = malformedTransfer;
    QVERIFY(!logger::QtcService::validateJson(qso, malformed, &error));
}

void QtcN1mmTest::formatsN1mmContactXml()
{
    logger::Qso qso = partnerQso();
    qso.callsign = QStringLiteral("DL1&ABC");
    qso.sentExchangeFields.insert(QStringLiteral("report"), QStringLiteral("599"));
    qso.receivedExchangeFields.insert(QStringLiteral("report"), QStringLiteral("579"));
    const QByteArray xml = logger::N1mmUdpBroadcaster::serializeQso(
        qso, QStringLiteral("WAE <CW>"), QStringLiteral("Operator & One"));
    QVERIFY(xml.startsWith("<?xml"));
    QVERIFY(xml.contains("<contestname>WAE &lt;CW&gt;</contestname>"));
    QVERIFY(xml.contains("<call>DL1&amp;ABC</call>"));
    QVERIFY(xml.contains("<operator>Operator &amp; One</operator>"));
    QVERIFY(xml.contains("<rxfreq>14025.0</rxfreq>"));
    QVERIFY(xml.contains("<sntnr>001</sntnr>"));
}

void QtcN1mmTest::broadcastsDatagramsToConfiguredReceiver()
{
    QUdpSocket receiver;
    QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
    logger::N1mmUdpConfiguration configuration;
    configuration.enabled = true;
    configuration.address = QHostAddress::LocalHost;
    configuration.port = receiver.localPort();
    logger::N1mmUdpBroadcaster broadcaster;
    QString error;
    QVERIFY2(broadcaster.setConfiguration(configuration, &error), qPrintable(error));

    int sent = 0;
    const QVector<logger::Qso> qsos{partnerQso(), partnerQso()};
    QVERIFY2(broadcaster.broadcastLog(
                 qsos, QStringLiteral("WAE"), QStringLiteral("K1TEST"),
                 &sent, &error), qPrintable(error));
    QCOMPARE(sent, 2);
    QTRY_VERIFY(receiver.hasPendingDatagrams());
    int received = 0;
    while (receiver.hasPendingDatagrams()) {
        const QByteArray datagram(
            static_cast<qsizetype>(receiver.pendingDatagramSize()), Qt::Uninitialized);
        QByteArray buffer = datagram;
        QCOMPARE(receiver.readDatagram(buffer.data(), buffer.size()), qint64(buffer.size()));
        QVERIFY(buffer.contains("<contactinfo>"));
        ++received;
    }
    QCOMPARE(received, 2);
}

void QtcN1mmTest::udpSendDoesNotRequireAListeningReceiver()
{
    QUdpSocket unusedPort;
    QVERIFY(unusedPort.bind(QHostAddress::LocalHost, 0));
    const quint16 port = unusedPort.localPort();
    unusedPort.close();
    logger::N1mmUdpConfiguration configuration;
    configuration.enabled = true;
    configuration.address = QHostAddress::LocalHost;
    configuration.port = port;
    logger::N1mmUdpBroadcaster broadcaster(configuration);
    QString error;
    QVERIFY2(broadcaster.broadcastQso(
                 partnerQso(), QStringLiteral("WAE"), QStringLiteral("K1TEST"),
                 &error), qPrintable(error));
}

QTEST_MAIN(QtcN1mmTest)
#include "test_qtc_n1mm.moc"
