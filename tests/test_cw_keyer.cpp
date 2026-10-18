#include "logger/cw/cw_keyer_service.h"

#include <QSignalSpy>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtTest>

#include <algorithm>
#include <functional>

class CwKeyerTest final : public QObject {
    Q_OBJECT

private slots:
    void expandsCwMacrosCaseInsensitively();
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
    void hamlibRigctldFallbackCanAbortAndDisconnect();
};

void CwKeyerTest::expandsCwMacrosCaseInsensitively()
{
    const QMap<QString, QString> values{
        {QStringLiteral("CALL"), QStringLiteral("W1AW")},
        {QStringLiteral("NR"), QStringLiteral("007")},
        {QStringLiteral("OPERATOR"), QStringLiteral("Ada")},
        {QStringLiteral("WPM"), QStringLiteral("28")},
    };
    QCOMPARE(logger::expandCwMacros(
                 QStringLiteral("{call} {NR} {operator} {WPM} {UNKNOWN}"), values),
             QStringLiteral("W1AW 007 Ada 28 {UNKNOWN}"));
}

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

void CwKeyerTest::hamlibRigctldFallbackCanAbortAndDisconnect()
{
#ifndef LOGGER_WITH_HAMLIB
    QSKIP("Hamlib support is not compiled in");
#else
    const QString rigctldPath =
        QStandardPaths::findExecutable(QStringLiteral("rigctld"));
    const QString rigctlPath = QStandardPaths::findExecutable(QStringLiteral("rigctl"));
    if (rigctldPath.isEmpty() || rigctlPath.isEmpty()) {
        QSKIP("rigctld and rigctl are required for the Hamlib CW integration test");
    }

    const auto capabilities = logger::inspectHamlibCwCapabilities(1);
    QVERIFY(capabilities.modelAvailable);
    QVERIFY(capabilities.sendText);

    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const int port = reservation.serverPort();
    reservation.close();

    QProcess rigctld;
    rigctld.start(rigctldPath, {QStringLiteral("-m"), QStringLiteral("1"),
                                QStringLiteral("-t"), QString::number(port),
                                QStringLiteral("-T"), QStringLiteral("127.0.0.1")});
    if (!rigctld.waitForStarted(3000)) {
        QFAIL(qPrintable(QStringLiteral("Unable to start rigctld: %1")
                             .arg(rigctld.errorString())));
    }

    bool listening = false;
    QElapsedTimer startupTimer;
    startupTimer.start();
    while (startupTimer.elapsed() < 5000 && rigctld.state() == QProcess::Running) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
        if (socket.waitForConnected(100)) {
            socket.disconnectFromHost();
            listening = true;
            break;
        }
        QTest::qWait(50);
    }
    QVERIFY2(listening, qPrintable(QStringLiteral("rigctld did not start: %1 %2")
                                       .arg(QString::fromLocal8Bit(rigctld.readAllStandardError()),
                                            QString::fromLocal8Bit(rigctld.readAllStandardOutput()))));

    QProcess modeSetter;
    modeSetter.start(rigctlPath,
                     {QStringLiteral("-m"), QStringLiteral("2"), QStringLiteral("-r"),
                      QStringLiteral("127.0.0.1:%1").arg(port), QStringLiteral("M"),
                      QStringLiteral("CW"), QStringLiteral("500")});
    QVERIFY(modeSetter.waitForStarted(3000));
    QVERIFY(modeSetter.waitForFinished(3000));
    QCOMPARE(modeSetter.exitStatus(), QProcess::NormalExit);
    QCOMPARE(modeSetter.exitCode(), 0);

    logger::CwKeyerController controller;
    QSignalSpy statuses(&controller, &logger::CwKeyerController::statusChanged);
    QVERIFY(statuses.isValid());
    logger::CwKeyerConfig config;
    config.type = logger::CwKeyerType::Hamlib;
    config.radio.modelId = 2;
    config.radio.transport = logger::RadioTransport::Tcp;
    config.radio.host = QStringLiteral("127.0.0.1");
    config.radio.port = port;
    controller.connectKeyer(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        !statuses.isEmpty()
            && lastStatus(statuses).state == logger::ConnectionState::Connected
            && !lastStatus(statuses).completionConfirmed,
        5000);
    QCOMPARE(lastStatus(statuses).pacing, QStringLiteral("estimated timing"));

    controller.enqueueMessage(QStringLiteral("TEST"));
    controller.enqueueMessage(QStringLiteral("73"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !lastStatus(statuses).transmitting
            && lastStatus(statuses).queuedMessages == 0,
        5000);
    QVERIFY(!anyStatus(statuses, [](const auto &status) {
        return status.completionTimedOut;
    }));

    controller.enqueueMessage(QString(256, QLatin1Char('E')));
    controller.enqueueMessage(QStringLiteral("SHOULD NOT SEND"));
    controller.abortTransmission();
    QTRY_VERIFY_WITH_TIMEOUT(
        lastStatus(statuses).state == logger::ConnectionState::Connected
            && !lastStatus(statuses).transmitting
            && lastStatus(statuses).queuedMessages == 0
            && lastStatus(statuses).message.contains(QStringLiteral("aborted")),
        5000);

    controller.enqueueMessage(QString(256, QLatin1Char('E')));
    controller.disconnectKeyer();
    QTRY_VERIFY_WITH_TIMEOUT(
        lastStatus(statuses).state == logger::ConnectionState::Disconnected
            && !lastStatus(statuses).transmitting,
        5000);

    rigctld.terminate();
    if (!rigctld.waitForFinished(3000)) {
        rigctld.kill();
        rigctld.waitForFinished(3000);
    }
#endif
}

QTEST_GUILESS_MAIN(CwKeyerTest)
#include "test_cw_keyer.moc"
