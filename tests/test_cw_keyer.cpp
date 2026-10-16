#include "logger/cw/cw_keyer_service.h"

#include <QSignalSpy>
#include <QUdpSocket>
#include <QtTest>

#include <algorithm>
#include <functional>

class CwKeyerTest final : public QObject {
    Q_OBJECT

private slots:
    void mockKeyerQueuesAndAborts();
    void queuedMessagesKeepTheirRequestedVfo();
    void changesSpeedOnMockKeyer();
    void cwdaemonSendsTextSpeedAndAbortCommands();
    void cwdaemonRejectsVfoBWithoutSending();
    void mockCompletionDrivesPacing();
    void estimatedPacingIsReportedWithoutCompletionSupport();
    void missingCompletionStopsQueueAndWarns();
    void abortClearsPendingCompletion();
    void cwdaemonPacesQueueByUdpReply();
    void cwdaemonWithoutReplyTimesOutAndAborts();
    void detectsHamlibModelWithoutCwSupport();
    void reportsUnsupportedHamlibModelBeforeOpeningPort();
};

void CwKeyerTest::mockKeyerQueuesAndAborts()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    QVERIFY(statuses.isValid());

    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    config.wpm = 25;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(statuses.cbegin(), statuses.cend(), [](const QList<QVariant> &args) {
            return qvariant_cast<logger::CwKeyerStatus>(args.first()).state
                == logger::ConnectionState::Connected;
        }),
        3000);

    controller.enqueueMessage(QStringLiteral("CQ TEST"));
    controller.enqueueMessage(QStringLiteral("N0CALL 599 001"));
    QTRY_VERIFY_WITH_TIMEOUT(
        qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first())
                .queuedMessages == 1,
        3000);
    controller.abortTransmission();
    QTRY_VERIFY_WITH_TIMEOUT(
        [&statuses] {
            const auto status = qvariant_cast<logger::CwKeyerStatus>(
                statuses.constLast().first());
            return !status.transmitting && status.queuedMessages == 0
                && status.message.contains(QStringLiteral("aborted"));
        }(),
        3000);
}

void CwKeyerTest::queuedMessagesKeepTheirRequestedVfo()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    const auto currentStatus = [&statuses] {
        return qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first());
    };
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        !statuses.isEmpty()
            && currentStatus().state == logger::ConnectionState::Connected,
        3000);

    controller.enqueueMessage(QStringLiteral("CQ TEST"), 1);
    controller.enqueueMessage(QStringLiteral("TU"), 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        currentStatus().transmitting
            && currentStatus().transmitVfo == 1
            && currentStatus().queuedMessages == 1,
        3000);
    controller.abortTransmission();
    QTRY_VERIFY_WITH_TIMEOUT(!currentStatus().transmitting, 3000);
}

void CwKeyerTest::changesSpeedOnMockKeyer()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        !statuses.isEmpty()
            && qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first()).state
                   == logger::ConnectionState::Connected,
        3000);
    controller.setWpm(32);
    QTRY_COMPARE_WITH_TIMEOUT(
        qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first()).wpm,
        32, 3000);
}

void CwKeyerTest::cwdaemonSendsTextSpeedAndAbortCommands()
{
    QUdpSocket receiver;
    QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Cwdaemon;
    config.host = QStringLiteral("127.0.0.1");
    config.port = receiver.localPort();
    config.wpm = 25;
    config.cwdaemonCompletionReply = false;
    controller.connectKeyer(config);

    auto waitDatagram = [&receiver]() -> QByteArray {
        if (!receiver.hasPendingDatagrams()) {
            receiver.waitForReadyRead(1000);
        }
        if (!receiver.hasPendingDatagrams()) {
            return {};
        }
        QByteArray datagram;
        datagram.resize(static_cast<qsizetype>(receiver.pendingDatagramSize()));
        receiver.readDatagram(datagram.data(), datagram.size());
        return datagram;
    };
    QCOMPARE(waitDatagram(), QByteArray::fromHex("1b323235"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !statuses.isEmpty()
            && qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first()).state
                   == logger::ConnectionState::Connected,
        3000);

    controller.setWpm(31);
    QCOMPARE(waitDatagram(), QByteArray::fromHex("1b323331"));
    controller.enqueueMessage(QStringLiteral("CQ TEST"));
    QCOMPARE(waitDatagram(), QByteArrayLiteral("CQ TEST"));
    controller.abortTransmission();
    QCOMPARE(waitDatagram(), QByteArray::fromHex("1b37"));
}

void CwKeyerTest::cwdaemonRejectsVfoBWithoutSending()
{
    QUdpSocket receiver;
    QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Cwdaemon;
    config.host = QStringLiteral("127.0.0.1");
    config.port = receiver.localPort();
    config.cwdaemonCompletionReply = false;
    controller.connectKeyer(config);
    QVERIFY(receiver.waitForReadyRead(1000));
    QByteArray speedCommand;
    speedCommand.resize(static_cast<qsizetype>(receiver.pendingDatagramSize()));
    receiver.readDatagram(speedCommand.data(), speedCommand.size());
    QCOMPARE(speedCommand, QByteArray::fromHex("1b323235"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !statuses.isEmpty()
            && qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first()).state
                   == logger::ConnectionState::Connected,
        3000);

    controller.enqueueMessage(QStringLiteral("CQ TEST"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        qvariant_cast<logger::CwKeyerStatus>(statuses.constLast().first())
            .message.contains(QStringLiteral("cannot select a transmit VFO")),
        3000);
    QVERIFY(!receiver.hasPendingDatagrams());
}

namespace {

logger::CwKeyerStatus lastStatus(const QSignalSpy &spy)
{
    return qvariant_cast<logger::CwKeyerStatus>(spy.constLast().first());
}

bool anyStatus(const QSignalSpy &spy,
               const std::function<bool(const logger::CwKeyerStatus &)> &predicate)
{
    for (const auto &args : spy) {
        if (predicate(qvariant_cast<logger::CwKeyerStatus>(args.first()))) {
            return true;
        }
    }
    return false;
}

QByteArray readDatagram(QUdpSocket &socket, QHostAddress *sender = nullptr,
                        quint16 *port = nullptr, int timeoutMs = 1000)
{
    if (!socket.hasPendingDatagrams()) {
        socket.waitForReadyRead(timeoutMs);
    }
    if (!socket.hasPendingDatagrams()) {
        return {};
    }
    QByteArray datagram;
    datagram.resize(static_cast<qsizetype>(socket.pendingDatagramSize()));
    socket.readDatagram(datagram.data(), datagram.size(), sender, port);
    return datagram;
}

} // namespace

void CwKeyerTest::mockCompletionDrivesPacing()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    config.mockCompletionDelayMs = 40;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        !statuses.isEmpty() && lastStatus(statuses).completionConfirmed, 3000);
    QCOMPARE(lastStatus(statuses).pacing, QStringLiteral("keyer confirmation"));

    QElapsedTimer elapsed;
    elapsed.start();
    controller.enqueueMessage(QStringLiteral("CQ TEST DE N0CALL"));
    controller.enqueueMessage(QStringLiteral("N0CALL 599 001"));
    // Estimate-based pacing would need several seconds for these messages.
    QTRY_VERIFY_WITH_TIMEOUT(
        anyStatus(statuses, [](const auto &s) { return s.queuedMessages == 1 && s.transmitting; })
            && !lastStatus(statuses).transmitting && lastStatus(statuses).queuedMessages == 0,
        2000);
    QVERIFY2(elapsed.elapsed() < 1500, "queue was not paced by completion");
    QVERIFY(!anyStatus(statuses, [](const auto &s) { return s.completionTimedOut; }));
}

void CwKeyerTest::estimatedPacingIsReportedWithoutCompletionSupport()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty()
                                 && lastStatus(statuses).state
                                     == logger::ConnectionState::Connected,
                             3000);
    QVERIFY(!lastStatus(statuses).completionConfirmed);
    QCOMPARE(lastStatus(statuses).pacing, QStringLiteral("estimated timing"));
}

void CwKeyerTest::missingCompletionStopsQueueAndWarns()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    config.mockCompletionNever = true;
    config.completionTimeoutFactor = 0.01;
    config.completionTimeoutMarginMs = 150;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty(), 3000);
    controller.enqueueMessage(QStringLiteral("CQ TEST"));
    controller.enqueueMessage(QStringLiteral("SECOND"));
    QTRY_VERIFY_WITH_TIMEOUT(lastStatus(statuses).completionTimedOut, 3000);
    const auto status = lastStatus(statuses);
    QVERIFY(status.message.contains(QStringLiteral("No completion confirmation")));
    QCOMPARE(status.queuedMessages, 0);
    QVERIFY(!status.transmitting);
    const int count = statuses.count();
    QTest::qWait(300);
    QCOMPARE(statuses.count(), count);
}

void CwKeyerTest::abortClearsPendingCompletion()
{
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Mock;
    config.mockCompletionNever = true;
    config.completionTimeoutFactor = 0.01;
    config.completionTimeoutMarginMs = 300;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty(), 3000);
    controller.enqueueMessage(QStringLiteral("CQ TEST"));
    controller.enqueueMessage(QStringLiteral("SECOND"));
    QTRY_VERIFY_WITH_TIMEOUT(
        anyStatus(statuses, [](const auto &s) { return s.transmitting && s.queuedMessages == 1; }),
        3000);
    controller.abortTransmission();
    QTRY_VERIFY_WITH_TIMEOUT(
        lastStatus(statuses).message.contains(QStringLiteral("aborted")), 3000);
    QVERIFY(!lastStatus(statuses).transmitting);
    QCOMPARE(lastStatus(statuses).queuedMessages, 0);
    // No timeout warning may fire for the aborted message.
    QTest::qWait(600);
    QVERIFY(!anyStatus(statuses, [](const auto &s) { return s.completionTimedOut; }));
}

void CwKeyerTest::cwdaemonPacesQueueByUdpReply()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress::LocalHost, 0));
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Cwdaemon;
    config.host = QStringLiteral("127.0.0.1");
    config.port = server.localPort();
    config.completionTimeoutMarginMs = 5000;
    controller.connectKeyer(config);
    QCOMPARE(readDatagram(server), QByteArray::fromHex("1b323235"));
    QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty() && lastStatus(statuses).completionConfirmed, 3000);

    controller.enqueueMessage(QStringLiteral("CQ TEST"));
    controller.enqueueMessage(QStringLiteral("73"));
    QHostAddress clientAddress;
    quint16 clientPort = 0;
    const QByteArray request = readDatagram(server, &clientAddress, &clientPort);
    QVERIFY(request.size() >= 3);
    QCOMPARE(request.left(2), QByteArray::fromHex("1b68"));
    const QByteArray token = request.mid(2);
    QVERIFY(!token.isEmpty());
    QCOMPARE(readDatagram(server), QByteArrayLiteral("CQ TEST"));

    // Nothing more is sent before completion, including after a wrong token.
    QVERIFY(readDatagram(server, nullptr, nullptr, 200).isEmpty());
    server.writeDatagram(QByteArrayLiteral("h99999\r\n"), clientAddress, clientPort);
    QVERIFY(readDatagram(server, nullptr, nullptr, 200).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(lastStatus(statuses).transmitting
                                 && lastStatus(statuses).queuedMessages == 1, 3000);

    server.writeDatagram("h" + token + "\r\n", clientAddress, clientPort);
    const QByteArray request2 = readDatagram(server);
    QCOMPARE(request2.left(2), QByteArray::fromHex("1b68"));
    QVERIFY(request2.mid(2) != token);
    QCOMPARE(readDatagram(server), QByteArrayLiteral("73"));
    server.writeDatagram("h" + request2.mid(2) + "\r\n", clientAddress, clientPort);
    QTRY_VERIFY_WITH_TIMEOUT(!lastStatus(statuses).transmitting
                                 && lastStatus(statuses).queuedMessages == 0, 3000);
}

void CwKeyerTest::cwdaemonWithoutReplyTimesOutAndAborts()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress::LocalHost, 0));
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Cwdaemon;
    config.host = QStringLiteral("127.0.0.1");
    config.port = server.localPort();
    config.completionTimeoutFactor = 0.01;
    config.completionTimeoutMarginMs = 150;
    controller.connectKeyer(config);
    QCOMPARE(readDatagram(server), QByteArray::fromHex("1b323235"));
    controller.enqueueMessage(QStringLiteral("CQ TEST"));
    controller.enqueueMessage(QStringLiteral("SECOND"));
    QCOMPARE(readDatagram(server).left(2), QByteArray::fromHex("1b68"));
    QCOMPARE(readDatagram(server), QByteArrayLiteral("CQ TEST"));
    QTRY_VERIFY_WITH_TIMEOUT(!statuses.isEmpty() && lastStatus(statuses).completionTimedOut, 3000);
    QCOMPARE(readDatagram(server), QByteArray::fromHex("1b37"));
    QVERIFY(readDatagram(server, nullptr, nullptr, 300).isEmpty());
}

void CwKeyerTest::detectsHamlibModelWithoutCwSupport()
{
#ifdef LOGGER_WITH_HAMLIB
    const logger::HamlibCwCapabilities capabilities =
        logger::inspectHamlibCwCapabilities(27005);
    QVERIFY(capabilities.modelAvailable);
    QVERIFY(!capabilities.sendText);
    const logger::HamlibCwCapabilities noSpeed =
        logger::inspectHamlibCwCapabilities(38001);
    QVERIFY(noSpeed.modelAvailable);
    QVERIFY(!noSpeed.sendText);
#else
    QSKIP("Hamlib support was not built.");
#endif
}

void CwKeyerTest::reportsUnsupportedHamlibModelBeforeOpeningPort()
{
#ifdef LOGGER_WITH_HAMLIB
    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    QVERIFY(statuses.isValid());
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Hamlib;
    config.radio.modelId = 27005;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(statuses.cbegin(), statuses.cend(), [](const QList<QVariant> &args) {
            const auto status =
                qvariant_cast<logger::CwKeyerStatus>(args.first());
            return status.state == logger::ConnectionState::Error
                && status.message.contains(QStringLiteral("does not support CW text"));
        }),
        3000);
#else
    QSKIP("Hamlib support was not built.");
#endif
}

QTEST_GUILESS_MAIN(CwKeyerTest)
#include "test_cw_keyer.moc"
