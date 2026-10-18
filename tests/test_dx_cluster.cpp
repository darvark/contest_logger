#include "logger/cluster/dx_cluster_service.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QtTest>

class DxClusterTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesDxSpotLine();
    void rejectsMalformedSpotLine();
    void rejectsInvalidClusterCallsign();
    void receivesSpotsAndReconnects();
    void rejectsInvalidSpotTimes();
    void assignsSpotDateAcrossMidnight();
    void handlesTelnetNegotiationAndLoginPrompt();
    void answersCallPromptWithoutFallbackDelay();
    void reassemblesSpotsSplitAcrossPackets();
    void reconnectsAndRelogsInAfterServerDisconnect();
};

namespace {

QDateTime utc(const char *iso)
{
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODate);
}

logger::DxClusterConfig testConfig(quint16 port)
{
    logger::DxClusterConfig config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.callsign = QStringLiteral("N0CALL");
    config.reconnectInitialMs = 50;
    config.loginFallbackMs = 5000;
    return config;
}

void sendPiece(QTcpSocket *socket, const QByteArray &bytes)
{
    socket->write(bytes);
    socket->flush();
    QTest::qWait(25);
}

QList<logger::ConnectionState> states(const QSignalSpy &spy)
{
    QList<logger::ConnectionState> result;
    for (const auto &args : spy) {
        result.append(qvariant_cast<logger::DxClusterStatus>(args.first()).state);
    }
    return result;
}

} // namespace

void DxClusterTest::rejectsInvalidSpotTimes()
{
    const QDateTime now = utc("2026-10-15T13:17:00Z");
    for (const char *stamp : {"2400Z", "2575Z", "2959Z", "1260Z", "1299Z", "9999Z"}) {
        QString error;
        const auto spot = logger::DxSpotParser::parseLine(
            QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW CW CQ ") + stamp, now, &error);
        QVERIFY2(!spot.has_value(), stamp);
        QVERIFY(error.contains(QStringLiteral("time")));
    }
    for (const char *stamp : {"0000Z", "2359Z", "1259Z"}) {
        QVERIFY2(logger::DxSpotParser::parseLine(
                     QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW CW ") + stamp, now)
                     .has_value(), stamp);
    }
    // Spots without a time token keep the receive time.
    const auto noTime = logger::DxSpotParser::parseLine(
        QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW CW"), now);
    QVERIFY(noTime.has_value());
    QCOMPARE(noTime->timestampUtc, now);
    // The last time token wins.
    const auto last = logger::DxSpotParser::parseLine(
        QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW heard 1200Z again 1310Z"), now);
    QVERIFY(last.has_value());
    QCOMPARE(last->timestampUtc.time(), QTime(13, 10));
}

void DxClusterTest::assignsSpotDateAcrossMidnight()
{
    struct Row { const char *received; const char *stamp; const char *expected; };
    // Policy: the date whose timestamp is nearest to the receive time.
    const Row rows[] = {
        {"2026-10-16T00:01:00Z", "2359Z", "2026-10-15T23:59:00Z"},
        {"2026-10-15T23:59:00Z", "0001Z", "2026-10-16T00:01:00Z"},
        {"2026-10-15T13:17:00Z", "1315Z", "2026-10-15T13:15:00Z"},
        {"2026-10-15T13:17:00Z", "1319Z", "2026-10-15T13:19:00Z"},
        {"2026-10-15T00:00:00Z", "0000Z", "2026-10-15T00:00:00Z"},
        {"2026-12-31T23:59:30Z", "0000Z", "2027-01-01T00:00:00Z"},
        {"2027-01-01T00:00:30Z", "2358Z", "2026-12-31T23:58:00Z"},
        {"2026-10-15T06:00:00Z", "2000Z", "2026-10-14T20:00:00Z"},
    };
    for (const Row &row : rows) {
        const auto spot = logger::DxSpotParser::parseLine(
            QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW CW ") + row.stamp,
            utc(row.received));
        QVERIFY2(spot.has_value(), row.stamp);
        QCOMPARE(spot->timestampUtc, utc(row.expected));
        QCOMPARE(spot->timestampUtc.timeSpec(), Qt::UTC);
    }
}

void DxClusterTest::handlesTelnetNegotiationAndLoginPrompt()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QByteArray received;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket,
                [socket, &received] { received.append(socket->readAll()); });
        QTimer::singleShot(0, socket, [socket] {
            // DO ECHO, WILL SGA, subnegotiation (TTYPE SEND) then prompt without newline.
            socket->write(QByteArray::fromHex("fffd01" "fffb03" "fffa1801fff0")
                          + QByteArrayLiteral("Welcome\r\nlogin: "));
            socket->flush();
        });
    });
    logger::DxClusterController controller;
    QSignalSpy spots(&controller, &logger::DxClusterController::spotReceived);
    controller.start(testConfig(server.serverPort()));

    QTRY_VERIFY_WITH_TIMEOUT(received.contains("N0CALL\n"), 3000);
    QVERIFY(received.contains(QByteArray::fromHex("fffc01")));
    QVERIFY(received.contains(QByteArray::fromHex("fffe03")));
    // The subnegotiation payload must not leak into the text stream or elicit replies.
    QVERIFY(!received.contains(QByteArray::fromHex("fffa")));

    QTcpSocket *socket = server.findChild<QTcpSocket *>();
    QVERIFY(socket != nullptr);
    // Option commands interleaved inside a spot line, DONT/WONT (no reply), escaped 0xFF.
    socket->write(QByteArray::fromHex("fffe01")
                  + QByteArrayLiteral("DX de K1ABC: 14025.0 ")
                  + QByteArray::fromHex("fffd05fffc18")
                  + QByteArrayLiteral("W1AW CW CQ 1315Z\r\n"));
    socket->flush();
    QTRY_COMPARE_WITH_TIMEOUT(spots.count(), 1, 3000);
    QCOMPARE(qvariant_cast<logger::Spot>(spots.first().first()).callsign,
             QStringLiteral("W1AW"));
    QTRY_VERIFY_WITH_TIMEOUT(received.contains(QByteArray::fromHex("fffc05")), 3000);
    QTest::qWait(200);
    QCOMPARE(received.count("N0CALL\n"), 1);
    QCOMPARE(spots.count(), 1);
    controller.stop();
}

void DxClusterTest::answersCallPromptWithoutFallbackDelay()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QByteArray received;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket,
                [socket, &received] { received.append(socket->readAll()); });
        socket->write("Please enter your call: ");
        socket->flush();
    });
    logger::DxClusterController controller;
    controller.start(testConfig(server.serverPort()));
    // Fallback is 5 s, so a quick reply proves the prompt triggered the login.
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("N0CALL\n"), 2000);
    controller.stop();
}

void DxClusterTest::reassemblesSpotsSplitAcrossPackets()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QTcpSocket *client = nullptr;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        client = server.nextPendingConnection();
        client->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    });
    logger::DxClusterController controller;
    QSignalSpy spots(&controller, &logger::DxClusterController::spotReceived);
    controller.start(testConfig(server.serverPort()));
    QTRY_VERIFY_WITH_TIMEOUT(client != nullptr, 3000);

    sendPiece(client, "DX de K1A");
    sendPiece(client, "BC: 14025");
    sendPiece(client, ".0 W1");
    sendPiece(client, "AW CW 13");
    sendPiece(client, "15Z\r");
    QCOMPARE(spots.count(), 0);
    sendPiece(client, "\nDX de K2XYZ: 7010.0 DL1ABC CW ");
    // IAC DO <opt> split byte by byte in the middle of a line.
    sendPiece(client, QByteArray::fromHex("ff"));
    sendPiece(client, QByteArray::fromHex("fd"));
    sendPiece(client, QByteArray::fromHex("01"));
    sendPiece(client, "1316Z\r\n");
    QTRY_COMPARE_WITH_TIMEOUT(spots.count(), 2, 3000);
    QTest::qWait(150);
    QCOMPARE(spots.count(), 2);
    const auto first = qvariant_cast<logger::Spot>(spots.at(0).first());
    const auto second = qvariant_cast<logger::Spot>(spots.at(1).first());
    QCOMPARE(first.callsign, QStringLiteral("W1AW"));
    QCOMPARE(first.spotter, QStringLiteral("K1ABC"));
    QCOMPARE(first.frequencyHz, qint64(14'025'000));
    QCOMPARE(second.callsign, QStringLiteral("DL1ABC"));
    QCOMPARE(second.frequencyHz, qint64(7'010'000));
    controller.stop();
}

void DxClusterTest::reconnectsAndRelogsInAfterServerDisconnect()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QList<QByteArray> logins;
    int connections = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        const int index = connections++;
        logins.append(QByteArray());
        connect(socket, &QTcpSocket::readyRead, socket, [socket, &logins, index] {
            const bool first = logins[index].isEmpty();
            logins[index].append(socket->readAll());
            if (!first) {
                return;
            }
            const QByteArray spot = index == 0
                ? QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW CW 1315Z\r\n")
                : QByteArrayLiteral("DX de K2XYZ: 7010.0 DL1ABC CW 1316Z\r\n");
            socket->write(spot);
            socket->flush();
            if (index == 0) {
                QTimer::singleShot(50, socket, &QTcpSocket::disconnectFromHost);
            }
        });
        socket->write("login: ");
        socket->flush();
    });
    logger::DxClusterController controller;
    QSignalSpy spots(&controller, &logger::DxClusterController::spotReceived);
    QSignalSpy statuses(&controller, &logger::DxClusterController::statusChanged);
    controller.start(testConfig(server.serverPort()));

    QTRY_COMPARE_WITH_TIMEOUT(spots.count(), 2, 5000);
    QTest::qWait(200);
    QCOMPARE(spots.count(), 2);
    QCOMPARE(connections, 2);
    QCOMPARE(logins.at(0), QByteArrayLiteral("N0CALL\n"));
    QCOMPARE(logins.at(1), QByteArrayLiteral("N0CALL\n"));
    QCOMPARE(qvariant_cast<logger::Spot>(spots.at(0).first()).callsign,
             QStringLiteral("W1AW"));
    QCOMPARE(qvariant_cast<logger::Spot>(spots.at(1).first()).callsign,
             QStringLiteral("DL1ABC"));

    using S = logger::ConnectionState;
    // start() first emits Disconnected (it calls stop()).
    const QList<S> expected{S::Disconnected, S::Connecting, S::Connected, S::Error,
                            S::Connecting, S::Connected};
    QCOMPARE(states(statuses), expected);
    const auto error = qvariant_cast<logger::DxClusterStatus>(statuses.at(3).first());
    QVERIFY(error.message.contains(QStringLiteral("Reconnecting")));
    controller.stop();
}

void DxClusterTest::parsesDxSpotLine()
{
    const QDateTime receivedAt =
        QDateTime::fromString(QStringLiteral("2026-10-15T13:17:00Z"), Qt::ISODate);
    QString error;
    const auto spot = logger::DxSpotParser::parseLine(
        QByteArrayLiteral("DX de K1ABC: 14025.0 W1AW CW CQ TEST 1315Z"),
        receivedAt, &error);

    QVERIFY2(spot.has_value(), qPrintable(error));
    QCOMPARE(spot->callsign, QStringLiteral("W1AW"));
    QCOMPARE(spot->spotter, QStringLiteral("K1ABC"));
    QCOMPARE(spot->frequencyHz, qint64(14'025'000));
    QCOMPARE(spot->band, QStringLiteral("20m"));
    QCOMPARE(spot->mode, QStringLiteral("CW"));
    QCOMPARE(spot->comment, QStringLiteral("CW CQ TEST 1315Z"));
    QCOMPARE(spot->timestampUtc.time(), QTime(13, 15));
}

void DxClusterTest::rejectsMalformedSpotLine()
{
    QString error;
    QVERIFY(!logger::DxSpotParser::parseLine(
                 QByteArrayLiteral("DX de K1ABC: nonsense W1AW"),
                 QDateTime::currentDateTimeUtc(), &error)
                 .has_value());
    QVERIFY(!error.isEmpty());

    QVERIFY(!logger::DxSpotParser::parseLine(
                 QByteArrayLiteral("DX de K1ABC: 0 W1AW CW"),
                 QDateTime::currentDateTimeUtc(), &error)
                 .has_value());
    QVERIFY(error.contains(QStringLiteral("frequency")));
}

void DxClusterTest::rejectsInvalidClusterCallsign()
{
    logger::DxClusterController controller;
    QSignalSpy statuses(&controller, &logger::DxClusterController::statusChanged);
    QVERIFY(statuses.isValid());
    logger::DxClusterConfig config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = 7300;
    config.callsign = QStringLiteral("N0CALL\r\nSHUTDOWN");
    controller.start(config);
    QTRY_VERIFY_WITH_TIMEOUT(statuses.count() >= 2, 3000);
    const auto status =
        qvariant_cast<logger::DxClusterStatus>(statuses.constLast().first());
    QCOMPARE(status.state, logger::ConnectionState::Error);
    QVERIFY(status.message.contains(QStringLiteral("valid station callsign")));
}

void DxClusterTest::receivesSpotsAndReconnects()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    int connections = 0;
    QByteArray clientReplies;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        ++connections;
        connect(socket, &QTcpSocket::readyRead, socket, [socket, &clientReplies] {
            clientReplies.append(socket->readAll());
        });
        QTimer::singleShot(50, socket, [socket, connections] {
            socket->write(QByteArray::fromHex("fffb01")
                          + QByteArrayLiteral(
                              "DX de K1ABC: 14025.0 W1AW CW CQ TEST 1315Z\r\n"));
            socket->flush();
            if (connections == 1) {
                QTimer::singleShot(50, socket, &QTcpSocket::disconnectFromHost);
            }
        });
    });

    logger::DxClusterController controller;
    QSignalSpy spots(&controller, &logger::DxClusterController::spotReceived);
    QSignalSpy statuses(&controller, &logger::DxClusterController::statusChanged);
    QVERIFY(spots.isValid());
    QVERIFY(statuses.isValid());

    logger::DxClusterConfig config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = server.serverPort();
    config.callsign = QStringLiteral("N0CALL");
    controller.start(config);

    QTRY_VERIFY_WITH_TIMEOUT(spots.count() >= 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(connections >= 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(spots.count() >= 2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(clientReplies.contains("N0CALL\n"), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(clientReplies.contains(QByteArray::fromHex("fffe01")), 3000);
    QCOMPARE(qvariant_cast<logger::Spot>(spots.first().first()).callsign,
             QStringLiteral("W1AW"));
    controller.stop();
}

QTEST_GUILESS_MAIN(DxClusterTest)
#include "test_dx_cluster.moc"
