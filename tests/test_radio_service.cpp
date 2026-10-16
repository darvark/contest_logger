#include "logger/radio/radio_service.h"

#include <QSignalSpy>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <algorithm>

class RadioServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void connectsMockRadioOnWorkerThread();
    void disconnectsMockRadio();
    void tunesMockRadio();
    void managesIndependentVfosAndFocus();
    void oneRadioFailureDoesNotInterruptTheOther();
    void pollingResumesAfterExtendedPause();
    void mockProbeReportsCapabilities();
#ifdef LOGGER_WITH_HAMLIB
    void rejectsInvalidModelWithReadableError();
    void rejectsMissingSerialDevicePath();
    void rejectsInvalidSerialBaudRate();
    void rejectsIncompatibleTransportModel();
#endif
    void connectsAndTunesHamlibDummy();
    void connectsAndTunesViaRigctld();
};

void RadioServiceTest::connectsMockRadioOnWorkerThread()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());

    logger::RadioConfig config;
    config.backend = logger::RadioBackendType::Mock;
    controller.connectRadio(config);

    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            const auto snapshot = qvariant_cast<logger::RadioSnapshot>(args.first());
            return snapshot.state == logger::ConnectionState::Connected
                && snapshot.frequencyHz == 14'025'000
                && snapshot.mode == QStringLiteral("CW")
                && snapshot.dualReceiveSupported;
        }),
        3000);
}

void RadioServiceTest::disconnectsMockRadio()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());

    logger::RadioConfig config;
    config.backend = logger::RadioBackendType::Mock;
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            return qvariant_cast<logger::RadioSnapshot>(args.first()).state
                == logger::ConnectionState::Connected;
        }),
        3000);

    controller.disconnectRadio();
    QTRY_VERIFY_WITH_TIMEOUT(
        qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first()).state
            == logger::ConnectionState::Disconnected,
        3000);
}

void RadioServiceTest::tunesMockRadio()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());
    logger::RadioConfig config;
    config.backend = logger::RadioBackendType::Mock;
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            return qvariant_cast<logger::RadioSnapshot>(args.first()).state
                == logger::ConnectionState::Connected;
        }),
        3000);

    controller.setFrequency(7'025'000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first())
                .frequencyHz == 7'025'000,
        3000);
}

void RadioServiceTest::managesIndependentVfosAndFocus()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());

    logger::RadioConfig config;
    config.backend = logger::RadioBackendType::Mock;
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            const auto snapshot = qvariant_cast<logger::RadioSnapshot>(args.first());
            return snapshot.state == logger::ConnectionState::Connected
                && snapshot.dualVfoSupported && snapshot.vfos.size() == 2;
        }),
        3000);

    controller.setFrequency(7'100'000, 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            const auto snapshot = qvariant_cast<logger::RadioSnapshot>(args.first());
            return snapshot.vfos.size() == 2
                && snapshot.vfos.at(0).frequencyHz == 14'025'000
                && snapshot.vfos.at(1).frequencyHz == 7'100'000;
        }),
        3000);

    controller.setVfoFocus(1);
    QTRY_VERIFY_WITH_TIMEOUT(
        !snapshots.isEmpty()
            && qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first())
                   .focusVfo == 1
            && qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first())
                   .frequencyHz == 7'100'000,
        3000);

    controller.setVfoFocus(0);
    QTRY_VERIFY_WITH_TIMEOUT(
        !snapshots.isEmpty()
            && qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first())
                   .focusVfo == 0
            && qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first())
                   .frequencyHz == 14'025'000,
        3000);
}

void RadioServiceTest::oneRadioFailureDoesNotInterruptTheOther()
{
    logger::RadioController radioA;
    logger::RadioController radioB;
    QSignalSpy snapshotsA(&radioA, &logger::RadioController::snapshotChanged);
    QSignalSpy snapshotsB(&radioB, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshotsA.isValid());
    QVERIFY(snapshotsB.isValid());

    logger::RadioConfig workingConfig;
    workingConfig.backend = logger::RadioBackendType::Mock;
    radioA.connectRadio(workingConfig);

    logger::RadioConfig failingConfig;
    failingConfig.backend = logger::RadioBackendType::Hamlib;
    failingConfig.modelId = 100000;
    radioB.connectRadio(failingConfig);

    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshotsA.cbegin(), snapshotsA.cend(),
                    [](const QList<QVariant> &args) {
                        return qvariant_cast<logger::RadioSnapshot>(args.first()).state
                            == logger::ConnectionState::Connected;
                    }),
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshotsB.cbegin(), snapshotsB.cend(),
                    [](const QList<QVariant> &args) {
                        const auto state =
                            qvariant_cast<logger::RadioSnapshot>(args.first()).state;
                        return state == logger::ConnectionState::Error
                            || state == logger::ConnectionState::Unsupported;
                    }),
        3000);

    radioA.setFrequency(14'100'000, 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        qvariant_cast<logger::RadioSnapshot>(snapshotsA.constLast().first())
                .frequencyHz == 14'100'000,
        3000);
    const auto radioBState =
        qvariant_cast<logger::RadioSnapshot>(snapshotsB.constLast().first()).state;
    QVERIFY(radioBState == logger::ConnectionState::Error
            || radioBState == logger::ConnectionState::Unsupported);
}

void RadioServiceTest::pollingResumesAfterExtendedPause()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());
    logger::RadioConfig config;
    config.backend = logger::RadioBackendType::Mock;
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(),
                    [](const QList<QVariant> &args) {
                        return qvariant_cast<logger::RadioSnapshot>(args.first()).state
                            == logger::ConnectionState::Connected;
                    }),
        3000);
    const qsizetype beforePause = snapshots.size();
    QTest::qWait(1300);
    QTRY_VERIFY_WITH_TIMEOUT(snapshots.size() > beforePause, 3000);
    QCOMPARE(qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first()).state,
             logger::ConnectionState::Connected);
}

void RadioServiceTest::mockProbeReportsCapabilities()
{
    logger::RadioConfig config;
    config.backend = logger::RadioBackendType::Mock;
    auto probe = logger::probeRadio(config);
    QVERIFY2(probe.ok, qPrintable(probe.message));
    QCOMPARE(probe.frequencyHz, qint64(14'025'000));
    QCOMPARE(probe.mode, QStringLiteral("CW"));
    QCOMPARE(probe.vfoCount, 2);
    QVERIFY(probe.dualVfo);
    QVERIFY(probe.dualReceive);

    config.mockDualReceiveSupported = false;
    probe = logger::probeRadio(config);
    QVERIFY2(probe.ok, qPrintable(probe.message));
    QVERIFY(probe.dualVfo);
    QVERIFY(!probe.dualReceive);
}

#ifdef LOGGER_WITH_HAMLIB
void RadioServiceTest::rejectsInvalidModelWithReadableError()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());
    logger::RadioConfig config;
    config.modelId = 100000;
    config.devicePath = QStringLiteral("/dev/null");
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty()
                                 && qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first()).state
                                     == logger::ConnectionState::Error,
                             3000);
    const auto snapshot = qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first());
    QVERIFY(snapshot.message.contains(QStringLiteral("model ID"), Qt::CaseInsensitive));
    QVERIFY(!snapshot.message.isEmpty());
}

void RadioServiceTest::rejectsMissingSerialDevicePath()
{
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());
    logger::RadioConfig config;
    config.modelId = 1;
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty()
                                 && qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first()).state
                                     == logger::ConnectionState::Error,
                             3000);
    const auto snapshot = qvariant_cast<logger::RadioSnapshot>(snapshots.constLast().first());
    QVERIFY(snapshot.message.contains(QStringLiteral("device path"), Qt::CaseInsensitive));
}

void RadioServiceTest::rejectsInvalidSerialBaudRate()
{
    logger::RadioConfig config;
    config.modelId = 1001;
    config.devicePath = QStringLiteral("/dev/null");
    config.baudRate = 1;
    auto probe = logger::probeRadio(config);
    QVERIFY(!probe.ok);
    QVERIFY(probe.message.contains(QStringLiteral("supported range"), Qt::CaseInsensitive));

    config.modelId = 1;
    config.baudRate = 0;
    probe = logger::probeRadio(config);
    QVERIFY(!probe.ok);
    QVERIFY(probe.message.contains(QStringLiteral("baud rate"), Qt::CaseInsensitive));
}

void RadioServiceTest::rejectsIncompatibleTransportModel()
{
    logger::RadioConfig config;
    config.transport = logger::RadioTransport::Tcp;
    config.modelId = 1;
    const auto probe = logger::probeRadio(config);
    QVERIFY(!probe.ok);
    QVERIFY(probe.message.contains(QStringLiteral("model 2"), Qt::CaseInsensitive));
}
#endif

void RadioServiceTest::connectsAndTunesHamlibDummy()
{
#ifndef LOGGER_WITH_HAMLIB
    QSKIP("Hamlib support is not compiled in");
#else
    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());
    logger::RadioConfig config;
    config.modelId = 1;
    config.devicePath = QStringLiteral("/dev/null");
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            const auto snapshot = qvariant_cast<logger::RadioSnapshot>(args.first());
            return snapshot.state == logger::ConnectionState::Connected
                && snapshot.frequencyHz > 0;
        }), 5000);

    controller.setVfoFocus(1);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            const auto snapshot = qvariant_cast<logger::RadioSnapshot>(args.first());
            return snapshot.state == logger::ConnectionState::Connected
                && snapshot.focusVfo == 1;
        }), 5000);

    constexpr qint64 tunedFrequency = 7'025'000;
    controller.setFrequency(tunedFrequency, 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            return qvariant_cast<logger::RadioSnapshot>(args.first()).frequencyHz
                == tunedFrequency;
        }), 5000);
#endif
}

void RadioServiceTest::connectsAndTunesViaRigctld()
{
#ifndef LOGGER_WITH_HAMLIB
    QSKIP("Hamlib support is not compiled in");
#else
    const QString rigctldPath = QStandardPaths::findExecutable(QStringLiteral("rigctld"));
    if (rigctldPath.isEmpty()) {
        QSKIP("rigctld is not available on PATH");
    }
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const int port = reservation.serverPort();
    reservation.close();

    QProcess rigctld;
    rigctld.start(rigctldPath, {QStringLiteral("-m"), QStringLiteral("1"),
                               QStringLiteral("-t"), QString::number(port)});
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
    QVERIFY2(listening, qPrintable(QStringLiteral("rigctld did not accept connections: %1 %2")
                                       .arg(QString::fromLocal8Bit(rigctld.readAllStandardError()),
                                            QString::fromLocal8Bit(rigctld.readAllStandardOutput()))));

    logger::RadioController controller;
    QSignalSpy snapshots(&controller, &logger::RadioController::snapshotChanged);
    QVERIFY(snapshots.isValid());
    logger::RadioConfig config;
    config.transport = logger::RadioTransport::Tcp;
    config.modelId = 2;
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    controller.connectRadio(config);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            const auto snapshot = qvariant_cast<logger::RadioSnapshot>(args.first());
            return snapshot.state == logger::ConnectionState::Connected
                && snapshot.frequencyHz > 0;
        }), 7000);

    constexpr qint64 tunedFrequency = 14'025'000;
    controller.setFrequency(tunedFrequency);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(snapshots.cbegin(), snapshots.cend(), [](const QList<QVariant> &args) {
            return qvariant_cast<logger::RadioSnapshot>(args.first()).frequencyHz
                == tunedFrequency;
        }), 7000);
    controller.disconnectRadio();
    rigctld.terminate();
    if (!rigctld.waitForFinished(3000)) {
        rigctld.kill();
        rigctld.waitForFinished(3000);
    }
#endif
}

QTEST_GUILESS_MAIN(RadioServiceTest)
#include "test_radio_service.moc"
