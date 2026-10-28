#include "logger/data/central_repository.h"
#include "logger/contest/contest_definition_loader.h"
#include "logger/network/sync_client.h"
#include "logger/network/sync_server.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSet>
#include <QTcpServer>
#include <QThread>
#include <QUuid>
#include <QtTest>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace {

constexpr auto StationUuid = "1c2cf9e8-6107-4e17-9c6c-9751fc4980c8";
constexpr auto ReadWriteStationUuid = "6ad1884f-0712-4710-92e4-2bbcf5a80d26";
constexpr auto ThirdStationUuid = "73c06301-35cc-43a4-8ce1-d15ae9438f87";
constexpr auto ReadOnlyStationUuid = "c87d4108-1f9e-49ed-90a2-8e276af2d1a7";
constexpr auto SecondAdminStationUuid = "c433dc8e-47f2-4db1-b67b-af236d196bac";
constexpr auto StationToken =
    "0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr auto ReadWriteStationToken =
    "abcdef0123456789abcdef0123456789abcdef0123456789";
constexpr auto ThirdStationToken =
    "9876543210abcdef9876543210abcdef9876543210abcdef";
constexpr auto ReadOnlyStationToken =
    "fed0123456789abcdeffed0123456789abcdeffed0123456789";

QByteArray framed(const QJsonObject &message)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    const quint32 size = static_cast<quint32>(payload.size());
    QByteArray result;
    result.reserve(payload.size() + 4);
    result.append(static_cast<char>((size >> 24) & 0xff));
    result.append(static_cast<char>((size >> 16) & 0xff));
    result.append(static_cast<char>((size >> 8) & 0xff));
    result.append(static_cast<char>(size & 0xff));
    result.append(payload);
    return result;
}

bool waitForBytes(QSslSocket &socket, qsizetype size, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (socket.bytesAvailable() >= size) {
            return true;
        }
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            return false;
        }
        QThread::msleep(1);
    }
    return socket.bytesAvailable() >= size;
}

bool readFrame(QSslSocket &socket, QJsonObject *message, int timeoutMs = 5000)
{
    if (!waitForBytes(socket, 4, timeoutMs)) {
        return false;
    }
    const QByteArray header = socket.read(4);
    if (header.size() != 4) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const uchar *>(header.constData());
    const quint32 size = (static_cast<quint32>(bytes[0]) << 24)
        | (static_cast<quint32>(bytes[1]) << 16)
        | (static_cast<quint32>(bytes[2]) << 8)
        | static_cast<quint32>(bytes[3]);
    if (size == 0 || size > 1024 * 1024
        || !waitForBytes(socket, static_cast<qsizetype>(size), timeoutMs)) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(socket.read(size));
    if (!document.isObject()) {
        return false;
    }
    *message = document.object();
    return true;
}

QJsonObject request(QSslSocket &socket, const QString &type,
                    const QJsonObject &payload, int version = 2,
                    bool splitFrame = false)
{
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces)
                                  .toLower();
    const QByteArray bytes = framed(QJsonObject{
        {QStringLiteral("protocol_version"), version},
        {QStringLiteral("request_id"), requestId},
        {QStringLiteral("type"), type},
        {QStringLiteral("payload"), payload}
    });
    if (splitFrame) {
        const qsizetype middle = bytes.size() / 2;
        socket.write(bytes.left(middle));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
        socket.write(bytes.mid(middle));
    } else {
        socket.write(bytes);
    }

    QJsonObject response;
    if (!readFrame(socket, &response)
        || response.value(QStringLiteral("request_id")).toString() != requestId) {
        return {};
    }
    return response;
}

bool connectTls(QSslSocket &socket, quint16 port, const QSslCertificate &certificate)
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setCaCertificates({certificate});
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket.setSslConfiguration(configuration);
    socket.setPeerVerifyName(QStringLiteral("localhost"));
    socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port);

    QElapsedTimer timer;
    timer.start();
    while (!socket.isEncrypted() && socket.state() != QAbstractSocket::UnconnectedState
           && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return socket.isEncrypted();
}

} // namespace

class SyncServerTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void servesTlsProtocolWithFragmentedFrames();
    void rejectsUnsupportedProtocolVersion();
    void enforcesConcurrentConnectionLimit();
    void trustedServerEnforcesConfiguredConnectionLimit();
    void connectsWithVerifiedTlsClient();
    void registersTrustedNetworkStationWithoutCredentials();
    void hostsOpenLogAndBindsIndependentClientLog();
    void rejectsDuplicateStationNames();
    void rejectsDifferentOpenLogType();
    void trustedClientReconnectsAfterNetworkLoss();
    void trustedNetworkThreeStationPilotRecoversAfterHostRestart();
    void reportsConnectedStationsForSelectedLog();
    void discoversAndSelectsRemoteLogForJoining();
    void rejectsUntrustedServerCertificate();
    void synchronizesOutboxAndChangesAtomically();
    void synchronizesThreeIndependentClients();
    void synchronizesUnknownFrequencyPastFailedIndependentQso();
    void rejectsConcurrentQsoEditsWithoutOverwriting();
    void retriesCommittedQsoAfterLostResponse();
    void resumesPendingClientOutboxAfterClientAndServerRestart();
    void reservesAndAuditsSerialsOverTls();
    void persistsSerialReservationsInSyncClient();
    void createsLogsAndImportsSerialHistoryWithAdminOnly();

private:
    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<logger::CentralRepository> m_repository;
    std::unique_ptr<logger::SyncServer> m_server;
    QSslCertificate m_certificate;
};

void SyncServerTest::init()
{
    m_directory = std::make_unique<QTemporaryDir>();
    m_repository = std::make_unique<logger::CentralRepository>();
    QVERIFY(m_directory->isValid());
    QString error;
    QVERIFY2(m_repository->initialize(m_directory->path(), &error), qPrintable(error));
    const QVector<logger::Contest> initialLogs = m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!initialLogs.isEmpty());

    const QDir fixtures(QStringLiteral(SYNC_SERVER_TEST_SOURCE_DIR)
                        + QStringLiteral("/tests/data"));
    QFile certificateFile(fixtures.filePath(QStringLiteral("sync_server_test_cert.pem")));
    QVERIFY(certificateFile.open(QIODevice::ReadOnly));
    m_certificate = QSslCertificate(&certificateFile, QSsl::Pem);
    QVERIFY(!m_certificate.isNull());

    QFile tokenFile(m_directory->filePath(QStringLiteral("tokens.json")));
    QVERIFY(tokenFile.open(QIODevice::WriteOnly));
    const QJsonObject credential{
        {QStringLiteral("station_uuid"), QString::fromLatin1(StationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(StationToken)},
        {QStringLiteral("permissions"),
         QJsonArray{QStringLiteral("read"), QStringLiteral("write"),
                    QStringLiteral("admin")}},
        {QStringLiteral("log_uuids"), QJsonArray{}}
    };
    const QJsonObject readWriteCredential{
        {QStringLiteral("station_uuid"), QString::fromLatin1(ReadWriteStationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(ReadWriteStationToken)},
        {QStringLiteral("permissions"),
         QJsonArray{QStringLiteral("read"), QStringLiteral("write")}},
        {QStringLiteral("log_uuids"),
         QJsonArray{initialLogs.first().logUuid}}
    };
    const QJsonObject thirdStationCredential{
        {QStringLiteral("station_uuid"), QString::fromLatin1(ThirdStationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(ThirdStationToken)},
        {QStringLiteral("permissions"),
        QJsonArray{QStringLiteral("read"), QStringLiteral("write")}},
        {QStringLiteral("log_uuids"),
        QJsonArray{initialLogs.first().logUuid}}
    };
    const QJsonObject readOnlyCredential{
        {QStringLiteral("station_uuid"), QString::fromLatin1(ReadOnlyStationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(ReadOnlyStationToken)},
        {QStringLiteral("permissions"), QJsonArray{QStringLiteral("read")}},
        {QStringLiteral("log_uuids"),
        QJsonArray{initialLogs.first().logUuid}}
    };
    const QJsonObject secondAdminCredential{
        {QStringLiteral("station_uuid"),
         QString::fromLatin1(SecondAdminStationUuid)},
        {QStringLiteral("token"),
         QStringLiteral("fedcba9876543210fedcba9876543210fedcba9876543210")},
        {QStringLiteral("permissions"),
         QJsonArray{QStringLiteral("read"), QStringLiteral("write"),
                    QStringLiteral("admin")}},
        {QStringLiteral("log_uuids"), QJsonArray{}}
    };
    QVERIFY(tokenFile.write(QJsonDocument(QJsonObject{
        {QStringLiteral("stations"),
         QJsonArray{credential, readWriteCredential, thirdStationCredential,
                    readOnlyCredential, secondAdminCredential}}
    }).toJson(QJsonDocument::Compact)) > 0);
    tokenFile.close();

    m_server = std::make_unique<logger::SyncServer>(*m_repository);
    QVERIFY2(m_server->listen(
        QStringLiteral("127.0.0.1"), 0,
        fixtures.filePath(QStringLiteral("sync_server_test_cert.pem")),
        fixtures.filePath(QStringLiteral("sync_server_test_key.pem")),
        tokenFile.fileName(), 4, &error), qPrintable(error));
}

void SyncServerTest::cleanup()
{
    if (m_server) {
        m_server->close();
        m_server.reset();
    }
    m_repository.reset();
    m_directory.reset();
}

void SyncServerTest::servesTlsProtocolWithFragmentedFrames()
{
    QSslSocket socket;
    QVERIFY(connectTls(socket, m_server->serverPort(), m_certificate));

    QJsonObject response = request(socket, QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("client_version"), QStringLiteral("test")},
        {QStringLiteral("min_protocol_version"), 2},
        {QStringLiteral("max_protocol_version"), 2},
        {QStringLiteral("capabilities"),
         QJsonArray{QStringLiteral("event_notifications")}}
    }, 2, true);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("hello_ack"));
    const QJsonArray capabilities = response.value(QStringLiteral("payload"))
        .toObject().value(QStringLiteral("capabilities")).toArray();
    QVERIFY(capabilities.contains(QStringLiteral("list_logs")));
    QVERIFY(capabilities.contains(QStringLiteral("get_serial_reservations")));
    QVERIFY(capabilities.contains(QStringLiteral("event_notifications")));

    response = request(socket, QStringLiteral("authenticate"), QJsonObject{
        {QStringLiteral("station_uuid"), QString::fromLatin1(StationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(StationToken)}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("authenticated"));
    const QString sessionUuid = response.value(QStringLiteral("payload"))
        .toObject().value(QStringLiteral("session_uuid")).toString();

    response = request(socket, QStringLiteral("list_logs"), {});
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("logs"));
    const QJsonArray logs =
        response.value(QStringLiteral("payload")).toObject()
            .value(QStringLiteral("logs")).toArray();
    QCOMPARE(logs.size(), 1);
    const QJsonObject listedLog = logs.first().toObject();
    QVERIFY(listedLog.value(QStringLiteral("name")).isString());
    QVERIFY(listedLog.value(QStringLiteral("station_callsign")).isString());
    QVERIFY(listedLog.value(QStringLiteral("starts_at_utc")).isNull()
            || listedLog.value(QStringLiteral("starts_at_utc")).isString());
    QVERIFY(listedLog.value(QStringLiteral("properties")).isObject());
    QVERIFY(listedLog.value(QStringLiteral("definition_yaml")).isString());
    const QString logUuid =
        listedLog.value(QStringLiteral("log_uuid")).toString();

    response = request(socket, QStringLiteral("select_log"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("contest_definition_sha256"), QJsonValue::Null}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("welcome"));

    response = request(socket, QStringLiteral("get_metrics"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("metrics"));
    const QJsonObject metrics = response.value(QStringLiteral("payload")).toObject();
    QCOMPARE(metrics.value(QStringLiteral("log_uuid")).toString(), logUuid);
    QCOMPARE(metrics.value(QStringLiteral("current_revision")).toInteger(), qint64(0));
    QCOMPARE(metrics.value(QStringLiteral("online_station_count")).toInt(), 1);

    response = request(socket, QStringLiteral("heartbeat"), QJsonObject{
        {QStringLiteral("session_uuid"), sessionUuid},
        {QStringLiteral("last_applied_revision"), 0},
        {QStringLiteral("pending_count"), 2},
        {QStringLiteral("error_count"), 1},
        {QStringLiteral("conflict_count"), 3}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("heartbeat_ack"));

    response = request(socket, QStringLiteral("get_snapshot"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("limit"), 25}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("snapshot_page"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("total_qsos")).toInt(), 0);

    response = request(socket, QStringLiteral("get_status"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("status"));
    const QJsonObject status = response.value(QStringLiteral("payload")).toObject();
    QCOMPARE(status.value(QStringLiteral("current_revision")).toInteger(), qint64(0));
    QCOMPARE(status.value(QStringLiteral("station_cursor")).toInteger(), qint64(0));
    QCOMPARE(status.value(QStringLiteral("connected_stations")).toArray().size(), 1);
    const QJsonObject legacyStation =
        status.value(QStringLiteral("connected_stations")).toArray().first().toObject();
    QCOMPARE(legacyStation.value(QStringLiteral("station_uuid")).toString(),
             QString::fromLatin1(StationUuid));
    QVERIFY(legacyStation.value(QStringLiteral("computer_name")).toString().isEmpty());
    QVERIFY(legacyStation.value(QStringLiteral("operator_name")).toString().isEmpty());

    QSslSocket legacySocket;
    QVERIFY(connectTls(legacySocket, m_server->serverPort(), m_certificate));
    response = request(legacySocket, QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("client_version"), QStringLiteral("test")},
        {QStringLiteral("min_protocol_version"), 2},
        {QStringLiteral("max_protocol_version"), 2},
        {QStringLiteral("capabilities"), QJsonArray{}}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("hello_ack"));
    response = request(legacySocket, QStringLiteral("authenticate"), QJsonObject{
        {QStringLiteral("station_uuid"), QString::fromLatin1(ReadOnlyStationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(ReadOnlyStationToken)}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("authenticated"));
    response = request(legacySocket, QStringLiteral("select_log"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("contest_definition_sha256"), QJsonValue::Null}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("welcome"));

    const QString stationUuid = QString::fromLatin1(StationUuid);
    const QString operationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString qsoUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString timestamp = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs);
    const QJsonObject qso{
        {QStringLiteral("qso_uuid"), qsoUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("callsign"), QStringLiteral("W1AW")},
        {QStringLiteral("timestamp_utc"), timestamp},
        {QStringLiteral("frequency_hz"), 14025000},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")},
        {QStringLiteral("sent_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("received_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("sent_exchange"), QStringLiteral("599")},
        {QStringLiteral("received_exchange"), QStringLiteral("599")},
        {QStringLiteral("points"), 1},
        {QStringLiteral("is_invalid"), false}
    };
    const QJsonObject operation{
        {QStringLiteral("operation_uuid"), operationUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("created_at_utc"), timestamp},
        {QStringLiteral("kind"), QStringLiteral("qso.create")},
        {QStringLiteral("expected_version"), 0},
        {QStringLiteral("qso"), qso}
    };
    const QJsonObject batchPayload{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("atomic"), true},
        {QStringLiteral("operations"), QJsonArray{operation}}
    };
    response = request(socket, QStringLiteral("submit_batch"), batchPayload);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("batch_result"));
    const QJsonObject batchResult = response.value(QStringLiteral("payload")).toObject();
    const QJsonArray operationResults =
        batchResult.value(QStringLiteral("results")).toArray();
    QCOMPARE(operationResults.size(), 1);
    QCOMPARE(operationResults.first().toObject()
                 .value(QStringLiteral("revision")).toInteger(), qint64(1));
    QJsonObject notification;
    QVERIFY(readFrame(socket, &notification));
    QVERIFY(notification.value(QStringLiteral("request_id")).isNull());
    QCOMPARE(notification.value(QStringLiteral("type")).toString(),
             QStringLiteral("event_available"));
    const QJsonObject notificationPayload =
        notification.value(QStringLiteral("payload")).toObject();
    QCOMPARE(notificationPayload.value(QStringLiteral("log_uuid")).toString(),
             logUuid);
    QCOMPARE(notificationPayload.value(QStringLiteral("current_revision"))
                 .toInteger(), qint64(1));
    QVERIFY(!waitForBytes(legacySocket, 1, 100));

    response = request(socket, QStringLiteral("get_snapshot"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("limit"), 25},
        {QStringLiteral("snapshot_revision"), 1}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("snapshot_page"));
    const QJsonObject snapshot = response.value(QStringLiteral("payload")).toObject();
    QCOMPARE(snapshot.value(QStringLiteral("snapshot_revision")).toInteger(), qint64(1));
    QCOMPARE(snapshot.value(QStringLiteral("total_qsos")).toInt(), 1);
    QCOMPARE(snapshot.value(QStringLiteral("qsos")).toArray().first().toObject()
                 .value(QStringLiteral("qso_uuid")).toString(), qsoUuid);
    QCOMPARE(snapshot.value(QStringLiteral("sha256")).toString().size(), 64);

    const QJsonObject retryResponse =
        request(socket, QStringLiteral("submit_batch"), batchPayload);
    QCOMPARE(retryResponse.value(QStringLiteral("type")).toString(),
             QStringLiteral("batch_result"));
    QCOMPARE(retryResponse.value(QStringLiteral("payload")).toObject(), batchResult);

    QJsonObject conflictingOperation = operation;
    conflictingOperation.insert(
        QStringLiteral("operation_uuid"),
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
    conflictingOperation.insert(QStringLiteral("kind"), QStringLiteral("qso.update"));
    const QJsonObject conflictResponse =
        request(socket, QStringLiteral("submit_batch"), QJsonObject{
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("atomic"), true},
            {QStringLiteral("operations"), QJsonArray{conflictingOperation}}
        });
    QCOMPARE(conflictResponse.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    const QJsonObject conflictError =
        conflictResponse.value(QStringLiteral("payload")).toObject();
    QCOMPARE(conflictError.value(QStringLiteral("code")).toString(),
             QStringLiteral("VERSION_CONFLICT"));
    const QJsonObject conflictContext =
        conflictError.value(QStringLiteral("context")).toObject();
    QCOMPARE(conflictContext.value(QStringLiteral("qso_uuid")).toString(), qsoUuid);
    QCOMPARE(conflictContext.value(QStringLiteral("expected_version")).toInt(), 0);
    QCOMPARE(conflictContext.value(QStringLiteral("current_qso")).toObject()
                 .value(QStringLiteral("qso_version")).toInt(), 1);

    const QString rolledBackQsoUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString rolledBackOperationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QJsonObject rolledBackQso = qso;
    rolledBackQso.insert(QStringLiteral("qso_uuid"), rolledBackQsoUuid);
    rolledBackQso.insert(QStringLiteral("callsign"), QStringLiteral("K2ROLLBACK"));
    QJsonObject rolledBackCreate = operation;
    rolledBackCreate.insert(QStringLiteral("operation_uuid"), rolledBackOperationUuid);
    rolledBackCreate.insert(QStringLiteral("qso"), rolledBackQso);
    QJsonObject invalidSecondOperation = conflictingOperation;
    invalidSecondOperation.insert(
        QStringLiteral("operation_uuid"),
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
    const QJsonObject failedBatchResponse =
        request(socket, QStringLiteral("submit_batch"), QJsonObject{
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("atomic"), true},
            {QStringLiteral("operations"),
             QJsonArray{rolledBackCreate, invalidSecondOperation}}
        });
    QCOMPARE(failedBatchResponse.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(failedBatchResponse.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("VERSION_CONFLICT"));
    response = request(socket, QStringLiteral("get_metrics"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid}
    });
    const QJsonObject detailedMetrics =
        response.value(QStringLiteral("payload")).toObject();
    const QJsonArray clientMetrics =
        detailedMetrics.value(QStringLiteral("clients")).toArray();
    QCOMPARE(clientMetrics.size(), 1);
    QCOMPARE(clientMetrics.first().toObject()
                 .value(QStringLiteral("pending_count")).toInt(), 2);
    QCOMPARE(clientMetrics.first().toObject()
                 .value(QStringLiteral("error_count")).toInt(), 1);
    QCOMPARE(clientMetrics.first().toObject()
                 .value(QStringLiteral("conflict_count")).toInt(), 3);
    QCOMPARE(detailedMetrics.value(QStringLiteral("transaction_count")).toInt(), 4);
    QCOMPARE(detailedMetrics.value(QStringLiteral("transaction_conflict_count")).toInt(),
             2);
    QCOMPARE(detailedMetrics.value(QStringLiteral("transaction_history"))
                 .toArray().size(), 4);
    response = request(socket, QStringLiteral("get_status"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid}
    });
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("current_revision")).toInteger(), qint64(1));
    response = request(socket, QStringLiteral("get_changes"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("after_revision"), 1}
    });
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("to_revision")).toInteger(), qint64(1));
    QVERIFY(response.value(QStringLiteral("payload")).toObject()
                .value(QStringLiteral("events")).toArray().isEmpty());

    response = request(socket, QStringLiteral("get_changes"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("after_revision"), 0}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("changes"));
    const QJsonObject changes = response.value(QStringLiteral("payload")).toObject();
    QCOMPARE(changes.value(QStringLiteral("from_revision")).toInteger(), qint64(1));
    QCOMPARE(changes.value(QStringLiteral("to_revision")).toInteger(), qint64(1));
    const QJsonArray events = changes.value(QStringLiteral("events")).toArray();
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().toObject().value(QStringLiteral("qso_uuid")).toString(),
             qsoUuid);

    response = request(socket, QStringLiteral("ack_revision"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("revision"), 1}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("acknowledged"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("revision")).toInteger(), qint64(1));

    int removedEvents = 0;
    QString retentionError;
    QVERIFY2(m_repository->compactSyncEvents(
                 logUuid, QDateTime::currentDateTimeUtc().addDays(1),
                 &removedEvents, &retentionError), qPrintable(retentionError));
    QCOMPARE(removedEvents, 1);
    response = request(socket, QStringLiteral("get_changes"), QJsonObject{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("after_revision"), 0}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("SNAPSHOT_REQUIRED"));
    response = request(socket, QStringLiteral("get_snapshot"), QJsonObject{
        {QStringLiteral("limit"), 25},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("snapshot_revision"), 1}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("snapshot_page"));
    const QJsonArray snapshotQsos = response.value(QStringLiteral("payload"))
                                        .toObject()
                                        .value(QStringLiteral("qsos")).toArray();
    QCOMPARE(snapshotQsos.size(), 1);
    QCOMPARE(snapshotQsos.first().toObject()
                 .value(QStringLiteral("qso_uuid")).toString(), qsoUuid);
}

void SyncServerTest::hostsOpenLogAndBindsIndependentClientLog()
{
    QString error;
    auto &hostRepository = *m_repository->repository();
    const auto hostLog = hostRepository.contests().first();
    logger::Qso hostQso;
    hostQso.contestId = hostLog.id;
    hostQso.callsign = QStringLiteral("W1AW");
    hostQso.stationCallsign = QStringLiteral("SP1HOST");
    hostQso.band = QStringLiteral("20m");
    hostQso.timestampUtc = QDateTime::currentDateTimeUtc();
    hostQso.frequencyHz = 14025000;
    hostQso.mode = QStringLiteral("CW");
    hostQso.sentExchange = QStringLiteral("599");
    hostQso.receivedExchange = QStringLiteral("599");
    hostQso.points = 1;
    QVERIFY2(hostRepository.saveQso(hostQso, &error), qPrintable(error));
    QVERIFY2(hostRepository.setHostedLog(hostLog.logUuid, &error), qPrintable(error));

    {
        logger::CentralRepository attached;
        QVERIFY2(attached.attach(hostRepository, &error), qPrintable(error));
        QCOMPARE(attached.databasePath(), m_repository->databasePath());
        logger::SyncServer server(attached);
        QVERIFY2(server.hostOpenLog(hostLog.logUuid, QStringLiteral("SERVER"), &error),
                 qPrintable(error));
        QVERIFY2(server.listenTrustedNetwork(QStringLiteral("127.0.0.1"), 0, 8, &error),
                 qPrintable(error));

        const QString connection = QStringLiteral("independent-open-log");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(m_directory->filePath(QStringLiteral("independent.sqlite")));
        QVERIFY(database.open());
        {
            logger::SqliteRepository local(database);
            QVERIFY2(local.initialize(&error), qPrintable(error));
            QVERIFY2(local.ensureGeneralLog(&error), qPrintable(error));
            const auto localLog = local.contests().first();
            QVERIFY(localLog.logUuid != hostLog.logUuid);
            logger::Qso localQso = hostQso;
            localQso.id = 0;
            localQso.contestId = localLog.id;
            localQso.qsoUuid.clear();
            localQso.logUuid.clear();
            localQso.stationUuid.clear();
            localQso.callsign = QStringLiteral("K2CLIENT");
            QVERIFY2(local.saveQso(localQso, &error), qPrintable(error));
            QVERIFY2(local.setSynchronizationEnabled(localLog.logUuid, true, &error),
                     qPrintable(error));

            logger::SyncClientConfiguration config;
            config.host = QStringLiteral("127.0.0.1");
            config.port = server.serverPort();
            config.stationUuid = local.localStationUuid(&error);
            config.computerName = QStringLiteral("CLIENT");
            config.logUuid = localLog.logUuid;
            config.trustedNetworkMode = true;
            config.useServerActiveLog = true;
            config.contestDefinitionSha256 = QString::fromLatin1(
                QCryptographicHash::hash(
                    local.contestDefinitionYaml(localLog.id).toUtf8(),
                    QCryptographicHash::Sha256).toHex());
            logger::SyncClient client;
            QSignalSpy states(&client, &logger::SyncClient::stateChanged);
            QSignalSpy bound(&client, &logger::SyncClient::logBound);
            client.connectToServer(config);
            QVERIFY2(client.startSynchronization(local, &error), qPrintable(error));
            QTRY_VERIFY2_WITH_TIMEOUT(client.state() == logger::SyncClient::State::Ready,
                qPrintable(states.isEmpty() ? QStringLiteral("No state")
                                            : states.last().at(1).toString()), 5000);
            QCOMPARE(bound.size(), 1);
            QCOMPARE(local.contests().first().id, localLog.id);
            QCOMPARE(local.contests().first().logUuid, hostLog.logUuid);
            QTRY_COMPARE_WITH_TIMEOUT(hostRepository.qsosForContest(hostLog.id).size(), 2, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(local.qsosForContest(localLog.id).size(), 2, 5000);
            QCOMPARE(server.connectedStations(hostLog.logUuid).size(), 2);

            auto clientEdit = local.qsoById(localQso.id, &error);
            QVERIFY(clientEdit.has_value());
            clientEdit->callsign = QStringLiteral("K2CLIENT/P");
            QVERIFY2(local.updateQso(*clientEdit, &error), qPrintable(error));
            client.flushPendingChanges();
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                const auto qsos = hostRepository.qsosForContest(hostLog.id);
                return std::any_of(qsos.cbegin(), qsos.cend(), [](const logger::Qso &qso) {
                    return qso.callsign == QStringLiteral("K2CLIENT/P");
                });
            }(), 2000);

            auto edited = hostRepository.qsoById(hostQso.id, &error);
            QVERIFY(edited.has_value());
            edited->callsign = QStringLiteral("W1AW/P");
            QVERIFY2(hostRepository.updateQso(*edited, &error), qPrintable(error));
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                const auto qsos = local.qsosForContest(localLog.id);
                return std::any_of(qsos.cbegin(), qsos.cend(), [](const logger::Qso &qso) {
                    return qso.callsign == QStringLiteral("W1AW/P");
                });
            }(), 5000);
            QVERIFY2(hostRepository.deleteQso(hostQso.id, &error), qPrintable(error));
            QTRY_COMPARE_WITH_TIMEOUT(local.qsosForContest(localLog.id).size(), 1, 5000);
            QVERIFY2(hostRepository.undoLastQsoChange(hostLog.id, &error), qPrintable(error));
            QTRY_COMPARE_WITH_TIMEOUT(local.qsosForContest(localLog.id).size(), 2, 5000);
            client.disconnectFromServer();
        }
        database.close();
        database = {};
        QSqlDatabase::removeDatabase(connection);
    }
    // Destroying the server adapter must not close the application's database.
    QVERIFY2(!hostRepository.contests(&error).isEmpty(), qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(hostRepository.setHostedLog(QString(), &error), qPrintable(error));
}

void SyncServerTest::rejectsDuplicateStationNames()
{
    QString error;
    const auto log = m_repository->repository()->contests().first();
    logger::SyncServer server(*m_repository);
    QVERIFY2(server.hostOpenLog(log.logUuid, QStringLiteral("SERVER"), &error), qPrintable(error));
    QVERIFY2(server.listenTrustedNetwork(QStringLiteral("127.0.0.1"), 0, 8, &error),
             qPrintable(error));
    logger::SyncClientConfiguration config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = server.serverPort();
    config.stationUuid = QString::fromLatin1(StationUuid);
    config.computerName = QStringLiteral("Radio-A");
    config.logUuid = log.logUuid;
    config.contestDefinitionSha256 = QString::fromLatin1(QCryptographicHash::hash(
        m_repository->repository()->contestDefinitionYaml(log.id).toUtf8(),
        QCryptographicHash::Sha256).toHex());
    config.trustedNetworkMode = true;
    logger::SyncClient first;
    first.connectToServer(config);
    QTRY_COMPARE(first.state(), logger::SyncClient::State::Ready);
    logger::SyncClient duplicate;
    config.stationUuid = QString::fromLatin1(ThirdStationUuid);
    config.computerName = QStringLiteral("  RADIO-a ");
    QSignalSpy failures(&duplicate, &logger::SyncClient::requestFailed);
    duplicate.connectToServer(config);
    QTRY_COMPARE(duplicate.state(), logger::SyncClient::State::Error);
    QCOMPARE(failures.first().at(1).toString(), QStringLiteral("STATION_ALREADY_CONNECTED"));
    config.computerName = QStringLiteral("server");
    duplicate.connectToServer(config);
    QTRY_COMPARE(duplicate.state(), logger::SyncClient::State::Error);
    QCOMPARE(server.connectedStations(log.logUuid).size(), 2);
    config.computerName = QStringLiteral("ANOTHER-NAME");
    config.stationUuid = m_repository->repository()->localStationUuid(&error);
    duplicate.connectToServer(config);
    QTRY_COMPARE(duplicate.state(), logger::SyncClient::State::Error);
    first.disconnectFromServer();
    QTRY_COMPARE(server.activeConnections(), 0);
    config.stationUuid = QString::fromLatin1(ThirdStationUuid);
    config.computerName = QStringLiteral("RADIO-A");
    duplicate.connectToServer(config);
    QTRY_COMPARE(duplicate.state(), logger::SyncClient::State::Ready);
}

void SyncServerTest::rejectsDifferentOpenLogType()
{
    QString error;
    const auto log = m_repository->repository()->contests().first();
    logger::SyncServer server(*m_repository);
    QVERIFY2(server.hostOpenLog(log.logUuid, QStringLiteral("SERVER"), &error), qPrintable(error));
    QVERIFY2(server.listenTrustedNetwork(QStringLiteral("127.0.0.1"), 0, 8, &error),
             qPrintable(error));
    logger::SyncClientConfiguration config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = server.serverPort();
    config.stationUuid = QString::fromLatin1(StationUuid);
    config.computerName = QStringLiteral("CLIENT");
    config.logUuid = log.logUuid;
    config.trustedNetworkMode = true;
    config.useServerActiveLog = true;
    config.contestDefinitionSha256 = QString(64, QLatin1Char('0'));
    logger::SyncClient client;
    QSignalSpy failures(&client, &logger::SyncClient::requestFailed);
    client.connectToServer(config);
    QTRY_COMPARE(client.state(), logger::SyncClient::State::Error);
    QCOMPARE(failures.first().at(1).toString(), QStringLiteral("CONTEST_MISMATCH"));
    QCOMPARE(server.connectedStations(log.logUuid).size(), 1);
}

void SyncServerTest::registersTrustedNetworkStationWithoutCredentials()
{
    QString error;
    const QVector<logger::Contest> logs = m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!logs.isEmpty());

    logger::SyncServer trustedServer(*m_repository);
    QVERIFY2(trustedServer.listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), 0, 4, &error), qPrintable(error));
    QCOMPARE(error, QString());
    logger::SyncServer occupiedPortServer(*m_repository);
    QVERIFY(!occupiedPortServer.listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), trustedServer.serverPort(), 4, &error));
    QVERIFY(!error.isEmpty());
    logger::SyncServer invalidAddressServer(*m_repository);
    QVERIFY(!invalidAddressServer.listenTrustedNetwork(
        QStringLiteral("localhost"), 0, 4, &error));
    QVERIFY(error.contains(QStringLiteral("numeric IP")));

    logger::SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("127.0.0.1");
    configuration.port = trustedServer.serverPort();
    configuration.trustedNetworkMode = true;
    configuration.stationUuid =
        QStringLiteral("68a7e24a-4c0d-48ca-9d52-35be764a91c0");
    configuration.computerName = QStringLiteral("RADIO-TRUSTED");
    configuration.stationCallsign = QStringLiteral("SP9TEST");
    configuration.operatorName = QStringLiteral("Operator One");
    configuration.logUuid = logs.first().logUuid;

    logger::SyncClient client;
    client.connectToServer(configuration);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Ready, 5000);
    QCOMPARE(trustedServer.activeConnections(), 1);

    logger::SyncClient duplicateClient;
    QSignalSpy duplicateFailures(
        &duplicateClient, &logger::SyncClient::requestFailed);
    duplicateClient.connectToServer(configuration);
    QTRY_COMPARE_WITH_TIMEOUT(duplicateClient.state(),
                              logger::SyncClient::State::Error, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!duplicateFailures.isEmpty(), 5000);
    QCOMPARE(duplicateFailures.first().at(1).toString(),
             QStringLiteral("STATION_ALREADY_CONNECTED"));
    QCOMPARE(trustedServer.activeConnections(), 1);

    client.setOperatingState(QStringLiteral("20m"), QStringLiteral("CW"));
    QTRY_COMPARE_WITH_TIMEOUT(
        trustedServer.connectedStations(configuration.logUuid).size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(([&trustedServer, &configuration] {
        const QJsonArray stations =
            trustedServer.connectedStations(configuration.logUuid);
        return !stations.isEmpty()
            && stations.first().toObject()
                   .value(QStringLiteral("operating_band")).toString()
                == QStringLiteral("20m");
    }()), 5000);
    const QJsonObject station = trustedServer
        .connectedStations(configuration.logUuid).first().toObject();
    QCOMPARE(station.value(QStringLiteral("computer_name")).toString(),
             QStringLiteral("RADIO-TRUSTED"));
    QCOMPARE(station.value(QStringLiteral("station_callsign")).toString(),
             QStringLiteral("SP9TEST"));
    QCOMPARE(station.value(QStringLiteral("operator_name")).toString(),
             QStringLiteral("Operator One"));
    QCOMPARE(station.value(QStringLiteral("operating_mode")).toString(),
             QStringLiteral("CW"));

    QSignalSpy responses(&client, &logger::SyncClient::responseReceived);
    const QString requestId = client.getStatus();
    QVERIFY(!requestId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!responses.isEmpty(), 5000);
    QCOMPARE(responses.first().at(0).toString(), requestId);
    QCOMPARE(responses.first().at(1).toString(), QStringLiteral("status"));

    QSignalSpy failures(&client, &logger::SyncClient::requestFailed);
    const QString forgedRequestId = client.submitBatch(QJsonArray{
        QJsonObject{{QStringLiteral("station_uuid"),
                     QStringLiteral("c87d4108-1f9e-49ed-90a2-8e276af2d1a7")}}
    });
    QVERIFY(!forgedRequestId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!failures.isEmpty(), 5000);
    QCOMPARE(failures.first().at(0).toString(), forgedRequestId);
    QCOMPARE(failures.first().at(1).toString(), QStringLiteral("FORBIDDEN"));

    client.disconnectFromServer();
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Disconnected,
                              5000);
}

void SyncServerTest::trustedClientReconnectsAfterNetworkLoss()
{
    QString error;
    const QVector<logger::Contest> logs = m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!logs.isEmpty());
    const QString stationUuid =
        QStringLiteral("68a7e24a-4c0d-48ca-9d52-35be764a91c0");

    QTcpServer unavailablePortProbe;
    QVERIFY(unavailablePortProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 unavailablePort = unavailablePortProbe.serverPort();
    unavailablePortProbe.close();
    logger::SyncClientConfiguration unavailableConfiguration;
    unavailableConfiguration.host = QStringLiteral("127.0.0.1");
    unavailableConfiguration.port = unavailablePort;
    unavailableConfiguration.trustedNetworkMode = true;
    unavailableConfiguration.stationUuid = stationUuid;
    unavailableConfiguration.logUuid = logs.first().logUuid;
    logger::SyncClient unavailableClient;
    unavailableClient.connectToServer(unavailableConfiguration);
    QTRY_COMPARE_WITH_TIMEOUT(unavailableClient.state(),
                              logger::SyncClient::State::Backoff, 5000);
    unavailableClient.disconnectFromServer();

    logger::SyncServer server(*m_repository);
    QVERIFY2(server.listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), 0, 4, &error), qPrintable(error));
    const quint16 serverPort = server.serverPort();
    logger::SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("127.0.0.1");
    configuration.port = serverPort;
    configuration.trustedNetworkMode = true;
    configuration.stationUuid = stationUuid;
    configuration.computerName = QStringLiteral("RADIO-RECONNECT");
    configuration.logUuid = logs.first().logUuid;

    logger::SyncClient client;
    client.connectToServer(configuration);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Ready, 5000);
    QCOMPARE(server.connectedStations(configuration.logUuid).size(), 1);

    server.close();
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Backoff,
                              5000);
    QVERIFY2(server.listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), serverPort, 4, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Ready, 5000);
    QCOMPARE(server.connectedStations(configuration.logUuid).size(), 1);
    client.disconnectFromServer();
}

void SyncServerTest::trustedNetworkThreeStationPilotRecoversAfterHostRestart()
{
    QString error;
    const QVector<logger::Contest> sourceLogs =
        m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(sourceLogs.size(), 1);
    const logger::Contest sourceLog = sourceLogs.first();
    const QString definitionYaml = QStringLiteral(R"yaml(
schema_version: 1
id: trusted-network-pilot
name: Trusted Network Pilot
bands: [20m, 40m]
modes: [CW, SSB]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
  received:
    - {name: report, type: rst}
scoring:
  serial_start: 1
  serial_scope: global
dupe_key: [callsign, band, mode]
)yaml");
    logger::ContestDefinition definition;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(
        definitionYaml, QStringLiteral("Trusted-network pilot"), definition, &error),
        qPrintable(error));

    const QString centralDirectory =
        m_directory->filePath(QStringLiteral("trusted-pilot-central"));
    auto centralRepository = std::make_unique<logger::CentralRepository>();
    QVERIFY2(centralRepository->initialize(centralDirectory, &error), qPrintable(error));
    QVERIFY2(centralRepository->createLog(
        definition, definitionYaml, sourceLog, &error) > 0, qPrintable(error));
    auto server = std::make_unique<logger::SyncServer>(*centralRepository);
    QVERIFY2(server->listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), 0, 8, &error), qPrintable(error));
    const quint16 port = server->serverPort();
    const QString logUuid = sourceLog.logUuid;

    const std::array<QString, 3> stationUuids{
        QStringLiteral("68000000-0000-4000-8000-000000000001"),
        QStringLiteral("68000000-0000-4000-8000-000000000002"),
        QStringLiteral("68000000-0000-4000-8000-000000000003")
    };
    struct PilotStation {
        QSqlDatabase database;
        std::unique_ptr<logger::SqliteRepository> repository;
        std::unique_ptr<logger::SyncClient> client;
        QString connectionName;
    };
    std::array<std::unique_ptr<PilotStation>, 3> stations;

    for (std::size_t index = 0; index < stations.size(); ++index) {
        auto station = std::make_unique<PilotStation>();
        station->connectionName = QStringLiteral("trusted-pilot-%1-%2")
            .arg(index)
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        station->database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), station->connectionName);
        station->database.setDatabaseName(m_directory->filePath(
            QStringLiteral("trusted-pilot-client-%1.sqlite").arg(index)));
        QVERIFY(station->database.open());
        station->repository =
            std::make_unique<logger::SqliteRepository>(station->database);
        QVERIFY2(station->repository->initialize(&error), qPrintable(error));
        QVERIFY2(station->repository->ensureGeneralLog(&error), qPrintable(error));

        QSqlQuery setLog(station->database);
        setLog.prepare(QStringLiteral(
            "UPDATE contests SET log_uuid = ? WHERE id = 1"));
        setLog.addBindValue(logUuid);
        QVERIFY2(setLog.exec(), qPrintable(setLog.lastError().text()));
        QSqlQuery setStation(station->database);
        setStation.prepare(QStringLiteral(
            "UPDATE sync_metadata SET value = ? "
            "WHERE key = 'local_station_uuid'"));
        setStation.addBindValue(stationUuids.at(index));
        QVERIFY2(setStation.exec(), qPrintable(setStation.lastError().text()));
        QVERIFY2(station->repository->setSynchronizationEnabled(
            logUuid, true, &error), qPrintable(error));

        logger::Qso qso;
        qso.contestId = 1;
        qso.callsign = QStringLiteral("PILOT%1").arg(index + 1);
        qso.stationCallsign = QStringLiteral("N%1TEST").arg(index + 1);
        qso.band = QStringLiteral("20m");
        qso.timestampUtc = QDateTime::currentDateTimeUtc();
        qso.frequencyHz = 14025000 + static_cast<qint64>(index);
        qso.mode = QStringLiteral("CW");
        qso.sentExchange = QStringLiteral("599");
        qso.receivedExchange = QStringLiteral("599");
        qso.sentExchangeFields.insert(
            QStringLiteral("report"), QStringLiteral("599"));
        qso.receivedExchangeFields.insert(
            QStringLiteral("report"), QStringLiteral("599"));
        qso.points = 1;
        QVERIFY2(station->repository->saveQso(qso, &error), qPrintable(error));

        station->client = std::make_unique<logger::SyncClient>();
        stations.at(index) = std::move(station);
    }

    const auto connectStation = [&stations, &stationUuids, port, &logUuid](
                                    std::size_t index) {
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = port;
        configuration.trustedNetworkMode = true;
        configuration.stationUuid = stationUuids.at(index);
        configuration.computerName = QStringLiteral("PILOT-%1").arg(index + 1);
        configuration.stationCallsign = QStringLiteral("N%1TEST").arg(index + 1);
        configuration.logUuid = logUuid;
        stations.at(index)->client->connectToServer(configuration);
        QString startError;
        return stations.at(index)->client->startSynchronization(
            *stations.at(index)->repository, &startError)
            ? QString() : startError;
    };
    QVERIFY2(connectStation(0).isEmpty(), "Could not start first station synchronization.");
    QVERIFY2(connectStation(1).isEmpty(), "Could not start second station synchronization.");

    const auto clientsAtRevision = [&stations, &logUuid](qint64 revision,
                                                         std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            QString cursorError;
            if (stations.at(index)->repository->syncCursor(logUuid, &cursorError)
                    != revision
                || !cursorError.isEmpty()) {
                return false;
            }
        }
        return true;
    };
    QTRY_VERIFY_WITH_TIMEOUT(clientsAtRevision(2, 2), 10000);

    QVERIFY2(connectStation(2).isEmpty(), "Could not start third station synchronization.");
    QTRY_VERIFY_WITH_TIMEOUT(clientsAtRevision(3, 3), 10000);
    QCOMPARE(centralRepository->currentRevision(logUuid, &error), qint64(3));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    server->close();
    for (const auto &station : stations) {
        QTRY_COMPARE_WITH_TIMEOUT(station->client->state(),
                                  logger::SyncClient::State::Backoff, 5000);
    }

    logger::Qso queuedQso;
    queuedQso.contestId = 1;
    queuedQso.callsign = QStringLiteral("N1OFFL");
    queuedQso.stationCallsign = QStringLiteral("N1TEST");
    queuedQso.band = QStringLiteral("20m");
    queuedQso.timestampUtc = QDateTime::currentDateTimeUtc();
    queuedQso.frequencyHz = 14026000;
    queuedQso.mode = QStringLiteral("CW");
    queuedQso.sentExchange = QStringLiteral("599");
    queuedQso.receivedExchange = QStringLiteral("599");
    queuedQso.sentExchangeFields.insert(
        QStringLiteral("report"), QStringLiteral("599"));
    queuedQso.receivedExchangeFields.insert(
        QStringLiteral("report"), QStringLiteral("599"));
    queuedQso.points = 1;
    QVERIFY2(stations.front()->repository->saveQso(queuedQso, &error),
             qPrintable(error));
    const QString queuedQsoUuid = queuedQso.qsoUuid;
    const QVector<logger::SyncOutboxOperation> pendingOperations =
        stations.front()->repository->syncOutbox(logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(std::any_of(
        pendingOperations.cbegin(), pendingOperations.cend(),
        [&queuedQsoUuid](const logger::SyncOutboxOperation &operation) {
            return operation.entityUuid == queuedQsoUuid
                && operation.status == QStringLiteral("pending");
        }));

    server.reset();
    centralRepository.reset();
    centralRepository = std::make_unique<logger::CentralRepository>();
    QVERIFY2(centralRepository->initialize(centralDirectory, &error), qPrintable(error));
    server = std::make_unique<logger::SyncServer>(*centralRepository);
    QVERIFY2(server->listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), port, 8, &error), qPrintable(error));

    QTRY_VERIFY_WITH_TIMEOUT(clientsAtRevision(4, 3), 20000);
    QCOMPARE(centralRepository->currentRevision(logUuid, &error), qint64(4));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QSet<QString> expectedQsoUuids;
    for (const auto &station : stations) {
        const QVector<logger::Qso> qsos =
            station->repository->qsosForContest(1, 0, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(qsos.size(), 4);
        QSet<QString> observedQsoUuids;
        for (const logger::Qso &qso : qsos) {
            observedQsoUuids.insert(qso.qsoUuid);
            QCOMPARE(qso.syncStatus, logger::QsoSyncStatus::Synced);
        }
        QVERIFY(observedQsoUuids.contains(queuedQsoUuid));
        if (expectedQsoUuids.isEmpty()) {
            expectedQsoUuids = observedQsoUuids;
        } else {
            QCOMPARE(observedQsoUuids, expectedQsoUuids);
        }
    }
    QCOMPARE(expectedQsoUuids.size(), 4);

    for (auto &station : stations) {
        station->client->disconnectFromServer();
        station->repository.reset();
        station->database.close();
        station->database = QSqlDatabase();
        QSqlDatabase::removeDatabase(station->connectionName);
    }
}

void SyncServerTest::reportsConnectedStationsForSelectedLog()
{
    QString error;
    const auto logs = m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!logs.isEmpty());
    const QString logUuid = logs.first().logUuid;
    logger::SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("127.0.0.1");
    configuration.peerName = QStringLiteral("localhost");
    configuration.port = m_server->serverPort();
    configuration.stationUuid = QString::fromLatin1(StationUuid);
    configuration.computerName = QStringLiteral("RADIO-A");
    configuration.stationCallsign = QStringLiteral("SP9CLUB");
    configuration.operatorName = QStringLiteral("Jan SP9ABC");
    configuration.token = QString::fromLatin1(StationToken);
    configuration.logUuid = logUuid;
    configuration.caCertificates = {m_certificate};
    logger::SyncClient admin;
    admin.connectToServer(configuration);
    QTRY_COMPARE(admin.state(), logger::SyncClient::State::Ready);
    configuration.stationUuid = QString::fromLatin1(ReadOnlyStationUuid);
    configuration.token = QString::fromLatin1(ReadOnlyStationToken);
    logger::SyncClient reader;
    reader.connectToServer(configuration);
    QTRY_COMPARE(reader.state(), logger::SyncClient::State::Ready);

    QSslSocket unauthenticated;
    QVERIFY(connectTls(unauthenticated, m_server->serverPort(), m_certificate));
    QTRY_COMPARE(m_server->activeConnections(), 3);
    QCOMPARE(m_server->connectedStations(logUuid).size(), 2);
    QVERIFY(m_server->connectedStations({}).isEmpty());
    QVERIFY(m_server->connectedStations(
        QUuid::createUuid().toString(QUuid::WithoutBraces)).isEmpty());
    QSignalSpy responses(&reader, &logger::SyncClient::responseReceived);
    const QString requestId = reader.getStatus();
    QVERIFY(!requestId.isEmpty());
    QTRY_VERIFY(!responses.isEmpty());
    const QList<QVariant> response = responses.takeFirst();
    QCOMPARE(response.at(0).toString(), requestId);
    QCOMPARE(response.at(1).toString(), QStringLiteral("status"));
    const QJsonArray stations = response.at(2).toJsonObject()
        .value(QStringLiteral("connected_stations")).toArray();
    QCOMPARE(stations.size(), 2);
    QSet<QString> uuids;
    for (const QJsonValue &value : stations) {
        const QJsonObject station = value.toObject();
        uuids.insert(station.value(QStringLiteral("station_uuid")).toString());
        QVERIFY(!QUuid(station.value(QStringLiteral("session_uuid")).toString()).isNull());
        QCOMPARE(station.size(), 8);
        QCOMPARE(station.value(QStringLiteral("computer_name")).toString(),
                 QStringLiteral("RADIO-A"));
        QCOMPARE(station.value(QStringLiteral("station_callsign")).toString(),
                 QStringLiteral("SP9CLUB"));
        QCOMPARE(station.value(QStringLiteral("operator_name")).toString(),
                 QStringLiteral("Jan SP9ABC"));
        QVERIFY(station.value(QStringLiteral("operating_band")).toString().isEmpty());
        QVERIFY(station.value(QStringLiteral("operating_mode")).toString().isEmpty());
    }
    QCOMPARE(uuids, (QSet<QString>{QString::fromLatin1(StationUuid),
                                 QString::fromLatin1(ReadOnlyStationUuid)}));
    reader.setOperatingState(QStringLiteral("20m"), QStringLiteral("CW"));
    QTRY_VERIFY([&] {
        for (const QJsonValue &value : m_server->connectedStations(logUuid)) {
            const QJsonObject station = value.toObject();
            if (station.value(QStringLiteral("station_uuid")).toString()
                == QString::fromLatin1(ReadOnlyStationUuid)) {
                return station.value(QStringLiteral("operating_band")).toString()
                           == QStringLiteral("20m")
                    && station.value(QStringLiteral("operating_mode")).toString()
                           == QStringLiteral("CW")
                    && !station.value(
                            QStringLiteral("operating_state_updated_utc"))
                            .toString().isEmpty();
            }
        }
        return false;
    }());
    reader.setStationIdentity(QStringLiteral("SP9NEW"), QStringLiteral("Anna SP9XYZ"));
    QTRY_VERIFY([&] {
        for (const QJsonValue &value : m_server->connectedStations(logUuid)) {
            const QJsonObject station = value.toObject();
            if (station.value(QStringLiteral("station_uuid")).toString()
                == QString::fromLatin1(ReadOnlyStationUuid)) {
                return station.value(QStringLiteral("operator_name")).toString()
                    == QStringLiteral("Anna SP9XYZ");
            }
        }
        return false;
    }());
    QSignalSpy failures(&reader, &logger::SyncClient::requestFailed);
    reader.setStationIdentity(QStringLiteral("SP9NEW"), QString(256, QLatin1Char('x')));
    QTRY_COMPARE(failures.size(), 1);
    QCOMPARE(failures.last().at(1).toString(), QStringLiteral("INVALID_MESSAGE"));
    reader.setStationIdentity(QStringLiteral("SP9NEW"), QStringLiteral("Invalid\noperator"));
    QTRY_COMPARE(failures.size(), 2);
    QCOMPARE(failures.last().at(1).toString(), QStringLiteral("INVALID_MESSAGE"));
    for (const QJsonValue &value : m_server->connectedStations(logUuid)) {
        const QJsonObject station = value.toObject();
        if (station.value(QStringLiteral("station_uuid")).toString()
            == QString::fromLatin1(ReadOnlyStationUuid)) {
            QCOMPARE(station.value(QStringLiteral("operator_name")).toString(),
                     QStringLiteral("Anna SP9XYZ"));
        }
    }
    admin.disconnectFromServer();
    QTRY_COMPARE(m_server->connectedStations(logUuid).size(), 1);
    QCOMPARE(m_server->connectedStations(logUuid).first().toObject()
                 .value(QStringLiteral("station_uuid")).toString(),
             QString::fromLatin1(ReadOnlyStationUuid));
    reader.disconnectFromServer();
    QTRY_VERIFY(m_server->connectedStations(logUuid).isEmpty());
}

void SyncServerTest::rejectsUnsupportedProtocolVersion()
{
    QSslSocket socket;
    QVERIFY(connectTls(socket, m_server->serverPort(), m_certificate));
    const QJsonObject response = request(socket, QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("client_version"), QStringLiteral("test")},
        {QStringLiteral("min_protocol_version"), 1},
        {QStringLiteral("max_protocol_version"), 1},
        {QStringLiteral("capabilities"), QJsonArray{}}
    }, 1);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("UNSUPPORTED_PROTOCOL"));
}

void SyncServerTest::enforcesConcurrentConnectionLimit()
{
    QSslSocket first;
    QSslSocket second;
    QSslSocket third;
    QSslSocket fourth;
    QSslSocket excess;
    QVERIFY(connectTls(first, m_server->serverPort(), m_certificate));
    QVERIFY(connectTls(second, m_server->serverPort(), m_certificate));
    QVERIFY(connectTls(third, m_server->serverPort(), m_certificate));
    QVERIFY(connectTls(fourth, m_server->serverPort(), m_certificate));
    QCOMPARE(m_server->activeConnections(), 4);

    QVERIFY(!connectTls(excess, m_server->serverPort(), m_certificate));
    QCOMPARE(m_server->activeConnections(), 4);
}

void SyncServerTest::trustedServerEnforcesConfiguredConnectionLimit()
{
    logger::SyncServer server(*m_repository);
    QString error;
    QVERIFY2(server.listenTrustedNetwork(
                 QStringLiteral("127.0.0.1"), 0, 2, &error), qPrintable(error));

    QTcpSocket first;
    QTcpSocket second;
    QTcpSocket excess;
    first.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort());
    second.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 2, 5000);
    excess.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(excess.state() == QAbstractSocket::UnconnectedState, 5000);
    QCOMPARE(server.activeConnections(), 2);

    first.disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 1, 5000);
    QTcpSocket replacement;
    replacement.connectToHost(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 2, 5000);
    server.close();
}

void SyncServerTest::connectsWithVerifiedTlsClient()
{
    const QVector<logger::Contest> logs =
        m_repository->repository()->contests();
    QCOMPARE(logs.size(), 1);

    logger::SyncClient client;
    QSignalSpy readySpy(&client, &logger::SyncClient::ready);
    QSignalSpy responseSpy(&client, &logger::SyncClient::responseReceived);
    QVERIFY(readySpy.isValid());
    QVERIFY(responseSpy.isValid());

    logger::SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("127.0.0.1");
    configuration.port = m_server->serverPort();
    configuration.peerName = QStringLiteral("localhost");
    configuration.stationUuid = QString::fromLatin1(StationUuid);
    configuration.token = QString::fromLatin1(StationToken);
    configuration.logUuid = logs.first().logUuid;
    configuration.caCertificates = {m_certificate};
    client.connectToServer(configuration);

    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
    QCOMPARE(client.state(), logger::SyncClient::State::Ready);
    QCOMPARE(readySpy.first().at(0).toString().size(), 36);
    QCOMPARE(readySpy.first().at(1).toLongLong(), qint64(0));
    QCOMPARE(readySpy.first().at(2).toString(), client.contestDefinitionYaml());

    const QString requestId = client.getStatus();
    QVERIFY(!requestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(responseSpy.count(), 1, 5000);
    QCOMPARE(responseSpy.first().at(0).toString(), requestId);
    QCOMPARE(responseSpy.first().at(1).toString(), QStringLiteral("status"));

    const QString changesRequestId = client.getChanges(0);
    QVERIFY(!changesRequestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(responseSpy.count(), 2, 5000);
    QCOMPARE(responseSpy.constLast().at(0).toString(), changesRequestId);
    QCOMPARE(responseSpy.constLast().at(1).toString(), QStringLiteral("changes"));
    const QJsonObject changes =
        responseSpy.constLast().at(2).toJsonObject();
    QCOMPARE(changes.value(QStringLiteral("from_revision")).toInteger(), qint64(1));
    QCOMPARE(changes.value(QStringLiteral("to_revision")).toInteger(), qint64(0));

    client.disconnectFromServer();
    QCOMPARE(client.state(), logger::SyncClient::State::Disconnected);
}

void SyncServerTest::discoversAndSelectsRemoteLogForJoining()
{
    logger::SyncClient client;
    QSignalSpy logsSpy(&client, &logger::SyncClient::logsDiscovered);
    QSignalSpy readySpy(&client, &logger::SyncClient::ready);
    connect(&client, &logger::SyncClient::logsDiscovered,
            &client, [&client](const QJsonArray &logs) {
                if (logs.isEmpty()) {
                    return;
                }
                const QJsonObject log = logs.first().toObject();
                const QByteArray hash = QCryptographicHash::hash(
                    log.value(QStringLiteral("definition_yaml")).toString().toUtf8(),
                    QCryptographicHash::Sha256).toHex();
                QVERIFY(client.selectRemoteLog(
                    log.value(QStringLiteral("log_uuid")).toString(),
                    QString::fromLatin1(hash)));
            });

    logger::SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("127.0.0.1");
    configuration.port = m_server->serverPort();
    configuration.peerName = QStringLiteral("localhost");
    configuration.stationUuid = QString::fromLatin1(StationUuid);
    configuration.token = QString::fromLatin1(StationToken);
    configuration.caCertificates = {m_certificate};
    client.connectToServer(configuration);

    QTRY_COMPARE_WITH_TIMEOUT(logsSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);
    QCOMPARE(client.state(), logger::SyncClient::State::Ready);
    QCOMPARE(readySpy.first().at(1).toLongLong(), qint64(0));
    client.disconnectFromServer();
}

void SyncServerTest::rejectsUntrustedServerCertificate()
{
    const QVector<logger::Contest> logs =
        m_repository->repository()->contests();
    QCOMPARE(logs.size(), 1);
    logger::SyncClient client;
    QSignalSpy readySpy(&client, &logger::SyncClient::ready);
    QVERIFY(readySpy.isValid());

    logger::SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("127.0.0.1");
    configuration.port = m_server->serverPort();
    configuration.peerName = QStringLiteral("localhost");
    configuration.stationUuid = QString::fromLatin1(StationUuid);
    configuration.token = QString::fromLatin1(StationToken);
    configuration.logUuid = logs.first().logUuid;
    client.connectToServer(configuration);

    QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Backoff, 5000);
    QCOMPARE(readySpy.count(), 0);
    client.disconnectFromServer();
}

void SyncServerTest::synchronizesOutboxAndChangesAtomically()
{
    const QVector<logger::Contest> serverLogs =
        m_repository->repository()->contests();
    QCOMPARE(serverLogs.size(), 1);
    const QString logUuid = serverLogs.first().logUuid;
    const QString connectionName = QStringLiteral("sync-client-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(m_directory->filePath(QStringLiteral("client.sqlite")));
    QVERIFY(database.open());
    {
        logger::SqliteRepository localRepository(database);
        QString error;
        QVERIFY2(localRepository.initialize(&error), qPrintable(error));
        QVERIFY2(localRepository.ensureGeneralLog(&error), qPrintable(error));
        QSqlQuery setLog(database);
        setLog.prepare(QStringLiteral(
            "UPDATE contests SET log_uuid = ? WHERE id = 1"));
        setLog.addBindValue(logUuid);
        QVERIFY2(setLog.exec(), qPrintable(setLog.lastError().text()));
        QSqlQuery setStation(database);
        setStation.prepare(QStringLiteral(
            "UPDATE sync_metadata SET value = ? "
            "WHERE key = 'local_station_uuid'"));
        setStation.addBindValue(QString::fromLatin1(StationUuid));
        QVERIFY2(setStation.exec(), qPrintable(setStation.lastError().text()));
        QVERIFY2(localRepository.setSynchronizationEnabled(logUuid, true, &error),
                 qPrintable(error));

        logger::Qso qso;
        qso.contestId = 1;
        qso.callsign = QStringLiteral("W1AW");
        qso.stationCallsign = QStringLiteral("K1TEST");
        qso.band = QStringLiteral("20m");
        qso.timestampUtc = QDateTime::currentDateTimeUtc();
        qso.frequencyHz = 14025000;
        qso.mode = QStringLiteral("CW");
        qso.sentExchange = QStringLiteral("599");
        qso.receivedExchange = QStringLiteral("599");
        qso.sentExchangeFields.insert(QStringLiteral("report"), QStringLiteral("599"));
        qso.receivedExchangeFields.insert(QStringLiteral("report"), QStringLiteral("599"));
        qso.points = 1;
        QVERIFY2(localRepository.saveQso(qso, &error), qPrintable(error));
        auto locallySaved = localRepository.qsoById(qso.id, &error);
        QVERIFY2(locallySaved.has_value(), qPrintable(error));
        locallySaved->callsign = QStringLiteral("W1AW/P");
        QVERIFY2(localRepository.updateQso(*locallySaved, &error), qPrintable(error));

        logger::SyncClient client;
        QSignalSpy responseSpy(&client, &logger::SyncClient::responseReceived);
        QSignalSpy syncFinishedSpy(
            &client, &logger::SyncClient::synchronizationFinished);
        QVERIFY(responseSpy.isValid());
        QVERIFY(syncFinishedSpy.isValid());
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = m_server->serverPort();
        configuration.peerName = QStringLiteral("localhost");
        configuration.stationUuid = QString::fromLatin1(StationUuid);
        configuration.token = QString::fromLatin1(StationToken);
        configuration.logUuid = logUuid;
        configuration.caCertificates = {m_certificate};
        client.connectToServer(configuration);
        QElapsedTimer queueTimer;
        queueTimer.start();
        QVERIFY2(client.startSynchronization(localRepository, &error),
                 qPrintable(error));

        QTRY_COMPARE_WITH_TIMEOUT(
            localRepository.syncCursor(logUuid, &error), qint64(2), 5000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        bool sawTwoOperationBatch = false;
        for (const QList<QVariant> &response : responseSpy) {
            if (response.value(1).toString() == QStringLiteral("batch_result")
                && response.value(2).toJsonObject()
                       .value(QStringLiteral("results")).toArray().size() == 2) {
                sawTwoOperationBatch = true;
                break;
            }
        }
        QVERIFY(sawTwoOperationBatch);
        qInfo().noquote() << "sync_queue_measurement operations=2"
                          << "requests=1 elapsed_ms=" << queueTimer.elapsed();
        const QVector<logger::Qso> localQsos =
            localRepository.qsosForContest(1, 0, &error);
        QCOMPARE(localQsos.size(), 1);
        QCOMPARE(localQsos.first().qsoUuid, qso.qsoUuid);
        QCOMPARE(localQsos.first().callsign, QStringLiteral("W1AW/P"));
        QCOMPARE(localQsos.first().qsoVersion, 2);
        QCOMPARE(localQsos.first().syncStatus, logger::QsoSyncStatus::Synced);
        QVERIFY(client.synchronizeNow());
        QTRY_COMPARE_WITH_TIMEOUT(syncFinishedSpy.count(), 1, 5000);
        QCOMPARE(syncFinishedSpy.first().at(0).toInt(), 0);
        QCOMPARE(syncFinishedSpy.first().at(1).toInt(), 0);
        QVERIFY(syncFinishedSpy.first().at(2).toLongLong() >= 0);
        QVERIFY(syncFinishedSpy.first().at(3).toBool());
        const QString consistencyRequest = client.checkConsistency();
        QVERIFY(!consistencyRequest.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            responseSpy.cbegin(), responseSpy.cend(),
            [&consistencyRequest](const QList<QVariant> &response) {
                return response.value(0).toString() == consistencyRequest
                    && response.value(1).toString()
                        == QStringLiteral("consistency_digest");
            }), 5000);
        const auto digestResponse = std::find_if(
            responseSpy.cbegin(), responseSpy.cend(),
            [&consistencyRequest](const QList<QVariant> &response) {
                return response.value(0).toString() == consistencyRequest;
            });
        QVERIFY(digestResponse != responseSpy.cend());
        QJsonObject serverDigest = digestResponse->at(2).toJsonObject();
        QCOMPARE(serverDigest.take(QStringLiteral("revision")).toInteger(), qint64(2));
        QCOMPARE(serverDigest,
                 localRepository.syncConsistencyDigest(logUuid, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        const QString remoteStation = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const QString remoteOperationUuid = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const QString remoteQsoUuid = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const QString timestamp = QDateTime::currentDateTimeUtc()
            .toString(Qt::ISODateWithMs);
        const QJsonObject remoteQso{
            {QStringLiteral("qso_uuid"), remoteQsoUuid},
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("station_uuid"), remoteStation},
            {QStringLiteral("station_callsign"), QStringLiteral("K2TEST")},
            {QStringLiteral("callsign"), QStringLiteral("DL1XYZ")},
            {QStringLiteral("timestamp_utc"), timestamp},
            {QStringLiteral("frequency_hz"), 14026000},
            {QStringLiteral("band"), QStringLiteral("20m")},
            {QStringLiteral("mode"), QStringLiteral("CW")},
            {QStringLiteral("sent_exchange_fields"), QJsonObject{
                {QStringLiteral("report"), QStringLiteral("599")}}},
            {QStringLiteral("received_exchange_fields"), QJsonObject{
                {QStringLiteral("report"), QStringLiteral("599")}}},
            {QStringLiteral("sent_exchange"), QStringLiteral("599")},
            {QStringLiteral("received_exchange"), QStringLiteral("599")},
            {QStringLiteral("points"), 1},
            {QStringLiteral("is_invalid"), false}
        };
        const QJsonObject remoteOperation{
            {QStringLiteral("operation_uuid"), remoteOperationUuid},
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("station_uuid"), remoteStation},
            {QStringLiteral("created_at_utc"), timestamp},
            {QStringLiteral("kind"), QStringLiteral("qso.create")},
            {QStringLiteral("expected_version"), 0},
            {QStringLiteral("qso"), remoteQso}
        };
        QJsonArray remoteResults;
        QVERIFY2(m_repository->submitBatch(
                     logUuid, QJsonArray{remoteOperation}, &remoteResults, &error),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            localRepository.syncCursor(logUuid, &error), qint64(3), 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QVector<logger::Qso> updatedQsos =
            localRepository.qsosForContest(1, 0, &error);
        QCOMPARE(updatedQsos.size(), 2);
        QVERIFY(std::any_of(updatedQsos.cbegin(), updatedQsos.cend(),
            [&remoteQsoUuid](const logger::Qso &item) {
                return item.qsoUuid == remoteQsoUuid
                    && item.stationUuid != QString::fromLatin1(StationUuid);
            }));

        const QString statusRequest = client.getStatus();
        QVERIFY(!statusRequest.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            responseSpy.cbegin(), responseSpy.cend(),
            [&statusRequest](const QList<QVariant> &arguments) {
                return arguments.size() == 3
                    && arguments.at(0).toString() == statusRequest
                    && arguments.at(1).toString() == QStringLiteral("status");
            }), 5000);
        const auto statusIt = std::find_if(
            responseSpy.cbegin(), responseSpy.cend(),
            [&statusRequest](const QList<QVariant> &arguments) {
                return arguments.size() == 3
                    && arguments.at(0).toString() == statusRequest
                    && arguments.at(1).toString() == QStringLiteral("status");
            });
        QCOMPARE(statusIt->at(2).toJsonObject()
                     .value(QStringLiteral("current_revision")).toInteger(), qint64(3));
        QCOMPARE(statusIt->at(2).toJsonObject()
                     .value(QStringLiteral("station_cursor")).toInteger(), qint64(3));

        const auto remoteLocal = std::find_if(
            updatedQsos.cbegin(), updatedQsos.cend(),
            [&remoteQsoUuid](const logger::Qso &item) {
                return item.qsoUuid == remoteQsoUuid;
            });
        QVERIFY(remoteLocal != updatedQsos.cend());
        QVERIFY2(localRepository.deleteQso(remoteLocal->id, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            localRepository.syncCursor(logUuid, &error), qint64(4), 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(localRepository.qsosForContest(1, 0, &error).size(), 1);
        QCOMPARE(m_repository->currentRevision(logUuid, &error), qint64(4));

        const auto savePilotQso = [&localRepository, &error](const QString &call) {
            logger::Qso added;
            added.contestId = 1;
            added.callsign = call;
            added.stationCallsign = QStringLiteral("K1TEST");
            added.band = QStringLiteral("20m");
            added.timestampUtc = QDateTime::currentDateTimeUtc();
            added.frequencyHz = 14027000;
            added.mode = QStringLiteral("CW");
            added.sentExchange = QStringLiteral("599");
            added.receivedExchange = QStringLiteral("599");
            added.sentExchangeFields.insert(
                QStringLiteral("report"), QStringLiteral("599"));
            added.receivedExchangeFields.insert(
                QStringLiteral("report"), QStringLiteral("599"));
            added.points = 1;
            if (!localRepository.saveQso(added, &error)) {
                return QString();
            }
            return added.qsoUuid;
        };
        const QString transientQsoUuid = savePilotQso(QStringLiteral("N1TEMP"));
        QVERIFY(!transientQsoUuid.isEmpty());
        const QVector<logger::Qso> beforeTransientEdit =
            localRepository.qsosForContest(1, 0, &error);
        const auto transientQso = std::find_if(
            beforeTransientEdit.cbegin(), beforeTransientEdit.cend(),
            [&transientQsoUuid](const logger::Qso &item) {
                return item.qsoUuid == transientQsoUuid;
            });
        QVERIFY(transientQso != beforeTransientEdit.cend());
        logger::Qso editedTransient = *transientQso;
        editedTransient.callsign = QStringLiteral("N1TEM2");
        QVERIFY2(localRepository.updateQso(editedTransient, &error), qPrintable(error));
        QVERIFY2(localRepository.deleteQso(editedTransient.id, &error), qPrintable(error));
        QVERIFY(client.synchronizeNow());
        QTRY_COMPARE_WITH_TIMEOUT(syncFinishedSpy.count(), 2, 10000);
        QVERIFY(syncFinishedSpy.constLast().at(3).toBool());
        QCOMPARE(localRepository.syncCursor(logUuid, &error), qint64(7));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(m_repository->currentRevision(logUuid, &error), qint64(7));
        const QString tombstoneDigestRequest = client.checkConsistency();
        QVERIFY(!tombstoneDigestRequest.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            responseSpy.cbegin(), responseSpy.cend(),
            [&tombstoneDigestRequest](const QList<QVariant> &response) {
                return response.value(0).toString() == tombstoneDigestRequest;
            }), 5000);
        const auto tombstoneDigestResponse = std::find_if(
            responseSpy.cbegin(), responseSpy.cend(),
            [&tombstoneDigestRequest](const QList<QVariant> &response) {
                return response.value(0).toString() == tombstoneDigestRequest;
            });
        QVERIFY(tombstoneDigestResponse != responseSpy.cend());
        QJsonObject tombstoneServerDigest =
            tombstoneDigestResponse->at(2).toJsonObject();
        QCOMPARE(tombstoneServerDigest.take(
                     QStringLiteral("revision")).toInteger(), qint64(7));
        QCOMPARE(tombstoneServerDigest,
                 localRepository.syncConsistencyDigest(logUuid, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QVERIFY(!savePilotQso(QStringLiteral("N1GOOD")).isEmpty());
        const QString invalidQsoUuid = savePilotQso(QStringLiteral("N1BAD"));
        QVERIFY(!invalidQsoUuid.isEmpty());
        QVector<logger::SyncOutboxOperation> queued =
            localRepository.syncOutbox(logUuid, false, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto invalidOperation = std::find_if(
            queued.cbegin(), queued.cend(),
            [&invalidQsoUuid](const logger::SyncOutboxOperation &operation) {
                return operation.entityUuid == invalidQsoUuid;
            });
        QVERIFY(invalidOperation != queued.cend());
        const QString validPayload = invalidOperation->payloadJson;
        QJsonObject invalidPayload = QJsonDocument::fromJson(
            validPayload.toUtf8()).object();
        invalidPayload.insert(QStringLiteral(
            "station_uuid"), QStringLiteral("68000000-0000-4000-8000-000000000099"));
        QSqlQuery corruptQueuedOperation(database);
        corruptQueuedOperation.prepare(QStringLiteral(
            "UPDATE sync_outbox SET payload_json = ? WHERE operation_uuid = ?"));
        corruptQueuedOperation.addBindValue(QString::fromUtf8(
            QJsonDocument(invalidPayload).toJson(QJsonDocument::Compact)));
        corruptQueuedOperation.addBindValue(invalidOperation->operationUuid);
        QVERIFY2(corruptQueuedOperation.exec(),
                 qPrintable(corruptQueuedOperation.lastError().text()));
        QVERIFY(client.synchronizeNow());
        QTRY_COMPARE_WITH_TIMEOUT(syncFinishedSpy.count(), 3, 10000);
        QVERIFY(!syncFinishedSpy.constLast().at(3).toBool());
        QCOMPARE(localRepository.syncCursor(logUuid, &error), qint64(8));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        queued = localRepository.syncOutbox(logUuid, false, &error);
        const auto failedOperation = std::find_if(
            queued.cbegin(), queued.cend(),
            [&invalidQsoUuid](const logger::SyncOutboxOperation &operation) {
                return operation.entityUuid == invalidQsoUuid
                    && operation.status == QStringLiteral("failed");
            });
        QVERIFY(failedOperation != queued.cend());
        QSqlQuery restoreQueuedOperation(database);
        restoreQueuedOperation.prepare(QStringLiteral(
            "UPDATE sync_outbox SET payload_json = ? WHERE operation_uuid = ?"));
        restoreQueuedOperation.addBindValue(validPayload);
        restoreQueuedOperation.addBindValue(failedOperation->operationUuid);
        QVERIFY2(restoreQueuedOperation.exec(),
                 qPrintable(restoreQueuedOperation.lastError().text()));
        QVERIFY2(localRepository.retrySyncOperation(
                     failedOperation->operationUuid, &error), qPrintable(error));
        QVERIFY(client.synchronizeNow());
        QTRY_COMPARE_WITH_TIMEOUT(syncFinishedSpy.count(), 4, 10000);
        QVERIFY(syncFinishedSpy.constLast().at(3).toBool());
        QCOMPARE(localRepository.syncCursor(logUuid, &error), qint64(9));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QSqlQuery corruptReplica(database);
        QVERIFY2(corruptReplica.exec(QStringLiteral(
            "UPDATE qsos SET points = points + 1 WHERE callsign = 'N1GOOD'")),
            qPrintable(corruptReplica.lastError().text()));
        const QString mismatchRequest = client.checkConsistency();
        QVERIFY(!mismatchRequest.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            responseSpy.cbegin(), responseSpy.cend(),
            [&mismatchRequest](const QList<QVariant> &response) {
                return response.value(0).toString() == mismatchRequest;
            }), 5000);
        const auto mismatchResponse = std::find_if(
            responseSpy.cbegin(), responseSpy.cend(),
            [&mismatchRequest](const QList<QVariant> &response) {
                return response.value(0).toString() == mismatchRequest;
            });
        QVERIFY(mismatchResponse != responseSpy.cend());
        QJsonObject expectedDigest = mismatchResponse->at(2).toJsonObject();
        expectedDigest.remove(QStringLiteral("revision"));
        QVERIFY(expectedDigest !=
                localRepository.syncConsistencyDigest(logUuid, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        client.disconnectFromServer();
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SyncServerTest::synchronizesThreeIndependentClients()
{
    const QVector<logger::Contest> logs = m_repository->repository()->contests();
    QCOMPARE(logs.size(), 1);
    const QString logUuid = logs.first().logUuid;
    const std::array<QString, 3> stationUuids{
        QString::fromLatin1(StationUuid),
        QString::fromLatin1(ReadWriteStationUuid),
        QString::fromLatin1(ThirdStationUuid)
    };
    const std::array<QString, 3> tokens{
        QString::fromLatin1(StationToken),
        QString::fromLatin1(ReadWriteStationToken),
        QString::fromLatin1(ThirdStationToken)
    };
    struct ClientNode {
        QSqlDatabase database;
        std::unique_ptr<logger::SqliteRepository> repository;
        std::unique_ptr<logger::SyncClient> client;
        std::unique_ptr<QSignalSpy> notificationSpy;
        QString connectionName;
    };
    std::vector<std::unique_ptr<ClientNode>> clients;
    clients.reserve(stationUuids.size());

    for (std::size_t index = 0; index < stationUuids.size(); ++index) {
        auto node = std::make_unique<ClientNode>();
        node->connectionName = QStringLiteral("three-client-%1-%2")
            .arg(index)
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        node->database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), node->connectionName);
        node->database.setDatabaseName(m_directory->filePath(
            QStringLiteral("client-%1.sqlite").arg(index)));
        QVERIFY(node->database.open());
        node->repository =
            std::make_unique<logger::SqliteRepository>(node->database);
        QString error;
        QVERIFY2(node->repository->initialize(&error), qPrintable(error));
        QVERIFY2(node->repository->ensureGeneralLog(&error), qPrintable(error));

        QSqlQuery setLog(node->database);
        setLog.prepare(QStringLiteral(
            "UPDATE contests SET log_uuid = ? WHERE id = 1"));
        setLog.addBindValue(logUuid);
        QVERIFY2(setLog.exec(), qPrintable(setLog.lastError().text()));
        QSqlQuery setStation(node->database);
        setStation.prepare(QStringLiteral(
            "UPDATE sync_metadata SET value = ? "
            "WHERE key = 'local_station_uuid'"));
        setStation.addBindValue(stationUuids.at(index));
        QVERIFY2(setStation.exec(), qPrintable(setStation.lastError().text()));
        QVERIFY2(node->repository->setSynchronizationEnabled(
                     logUuid, true, &error), qPrintable(error));

        logger::Qso qso;
        qso.contestId = 1;
        qso.callsign = QStringLiteral("K%1TEST").arg(index + 1);
        qso.stationCallsign = QStringLiteral("N%1STATION").arg(index + 1);
        qso.band = QStringLiteral("20m");
        qso.timestampUtc = QDateTime::currentDateTimeUtc();
        qso.frequencyHz = 14025000 + static_cast<qint64>(index);
        qso.mode = QStringLiteral("CW");
        qso.sentExchange = QStringLiteral("599");
        qso.receivedExchange = QStringLiteral("599");
        qso.sentExchangeFields.insert(
            QStringLiteral("report"), QStringLiteral("599"));
        qso.receivedExchangeFields.insert(
            QStringLiteral("report"), QStringLiteral("599"));
        qso.points = 1;
        QVERIFY2(node->repository->saveQso(qso, &error), qPrintable(error));

        node->client = std::make_unique<logger::SyncClient>();
        if (index == 0) {
            node->notificationSpy = std::make_unique<QSignalSpy>(
                node->client.get(),
                &logger::SyncClient::remoteChangesAvailable);
            QVERIFY(node->notificationSpy->isValid());
        }
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = m_server->serverPort();
        configuration.peerName = QStringLiteral("localhost");
        configuration.stationUuid = stationUuids.at(index);
        configuration.token = tokens.at(index);
        configuration.logUuid = logUuid;
        configuration.caCertificates = {m_certificate};
        node->client->connectToServer(configuration);
        QVERIFY2(node->client->startSynchronization(
                     *node->repository, &error), qPrintable(error));
        clients.push_back(std::move(node));
    }

    const auto allClientsSynced = [&clients, &logUuid] {
        for (const auto &node : clients) {
            QString error;
            if (node->repository->syncCursor(logUuid, &error) != 3
                || !error.isEmpty()) {
                return false;
            }
        }
        return true;
    };
    QTRY_VERIFY_WITH_TIMEOUT(allClientsSynced(), 10000);
    QVERIFY(clients.front()->notificationSpy->count() > 0);
    QCOMPARE(m_repository->currentRevision(logUuid), qint64(3));

    QSet<QString> expectedQsoUuids;
    for (const auto &node : clients) {
        QString error;
        const QVector<logger::Qso> qsos =
            node->repository->qsosForContest(1, 0, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(qsos.size(), 3);
        QSet<QString> observedQsoUuids;
        for (const logger::Qso &qso : qsos) {
            observedQsoUuids.insert(qso.qsoUuid);
            QCOMPARE(qso.syncStatus, logger::QsoSyncStatus::Synced);
        }
        if (expectedQsoUuids.isEmpty()) {
            expectedQsoUuids = observedQsoUuids;
        } else {
            QCOMPARE(observedQsoUuids, expectedQsoUuids);
        }
    }
    QCOMPARE(expectedQsoUuids.size(), 3);

    const QString readOnlyConnectionName = QStringLiteral("read-only-client-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase readOnlyDatabase = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), readOnlyConnectionName);
    readOnlyDatabase.setDatabaseName(
        m_directory->filePath(QStringLiteral("read-only-client.sqlite")));
    QVERIFY(readOnlyDatabase.open());
    {
        logger::SqliteRepository readOnlyRepository(readOnlyDatabase);
        QString error;
        QVERIFY2(readOnlyRepository.initialize(&error), qPrintable(error));
        QVERIFY2(readOnlyRepository.ensureGeneralLog(&error), qPrintable(error));
        QSqlQuery setLog(readOnlyDatabase);
        setLog.prepare(QStringLiteral(
            "UPDATE contests SET log_uuid = ? WHERE id = 1"));
        setLog.addBindValue(logUuid);
        QVERIFY2(setLog.exec(), qPrintable(setLog.lastError().text()));
        QSqlQuery setStation(readOnlyDatabase);
        setStation.prepare(QStringLiteral(
            "UPDATE sync_metadata SET value = ? "
            "WHERE key = 'local_station_uuid'"));
        setStation.addBindValue(QString::fromLatin1(ReadOnlyStationUuid));
        QVERIFY2(setStation.exec(), qPrintable(setStation.lastError().text()));
        QVERIFY2(readOnlyRepository.setSynchronizationEnabled(
                     logUuid, true, &error), qPrintable(error));
        logger::Qso localQso;
        localQso.contestId = 1;
        localQso.callsign = QStringLiteral("K9READONLY");
        localQso.stationCallsign = QStringLiteral("N9READONLY");
        localQso.band = QStringLiteral("20m");
        localQso.timestampUtc = QDateTime::currentDateTimeUtc();
        localQso.frequencyHz = 14027000;
        localQso.mode = QStringLiteral("CW");
        localQso.sentExchange = QStringLiteral("599");
        localQso.receivedExchange = QStringLiteral("599");
        localQso.points = 1;
        QVERIFY2(readOnlyRepository.saveQso(localQso, &error), qPrintable(error));

        logger::SyncClient readOnlyClient;
        QSignalSpy failureSpy(&readOnlyClient, &logger::SyncClient::requestFailed);
        QVERIFY(failureSpy.isValid());
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = m_server->serverPort();
        configuration.peerName = QStringLiteral("localhost");
        configuration.stationUuid = QString::fromLatin1(ReadOnlyStationUuid);
        configuration.token = QString::fromLatin1(ReadOnlyStationToken);
        configuration.logUuid = logUuid;
        configuration.caCertificates = {m_certificate};
        readOnlyClient.connectToServer(configuration);
        QVERIFY2(readOnlyClient.startSynchronization(
                     readOnlyRepository, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(readOnlyClient.isReadOnly(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            readOnlyRepository.syncCursor(logUuid, &error), qint64(3), 5000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QVector<logger::SyncOutboxOperation> queued =
            readOnlyRepository.syncOutbox(logUuid, false, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(queued.size(), 1);
        QCOMPARE(queued.first().status, QStringLiteral("pending"));
        QVERIFY(readOnlyClient.submitBatch(QJsonArray{}).isEmpty());
        QVERIFY(readOnlyClient.reserveSerial(
            QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
            QStringLiteral("20m"), QStringLiteral("CW")).isEmpty());
        const QString metricsRequest = readOnlyClient.getMetrics();
        QVERIFY(!metricsRequest.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            failureSpy.cbegin(), failureSpy.cend(),
            [&metricsRequest](const QList<QVariant> &arguments) {
                return arguments.size() == 4
                    && arguments.at(0).toString() == metricsRequest
                    && arguments.at(1).toString() == QStringLiteral("FORBIDDEN");
            }), 5000);
        QCOMPARE(m_repository->currentRevision(logUuid), qint64(3));
        readOnlyClient.disconnectFromServer();
    }
    readOnlyDatabase.close();
    readOnlyDatabase = QSqlDatabase();
    QSqlDatabase::removeDatabase(readOnlyConnectionName);

    for (auto &node : clients) {
        node->client->disconnectFromServer();
        node->repository.reset();
        node->database.close();
        node->database = QSqlDatabase();
        QSqlDatabase::removeDatabase(node->connectionName);
    }
}

void SyncServerTest::synchronizesUnknownFrequencyPastFailedIndependentQso()
{
    QString error;
    const QString logUuid = m_repository->repository()->contests().first().logUuid;
    m_server->close();
    QVERIFY2(m_server->listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), 0, 4, &error), qPrintable(error));

    struct Station {
        QString connectionName;
        QSqlDatabase database;
        std::unique_ptr<logger::SqliteRepository> repository;
        logger::SyncClient client;
        ~Station()
        {
            client.disconnectFromServer();
            repository.reset();
            database.close();
            database = {};
            QSqlDatabase::removeDatabase(connectionName);
        }
    };
    std::array<Station, 2> stations;
    for (std::size_t index = 0; index < stations.size(); ++index) {
        auto &station = stations.at(index);
        station.connectionName = QStringLiteral("unknown-frequency-%1-%2")
            .arg(index).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        station.database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), station.connectionName);
        station.database.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(station.database.open());
        station.repository = std::make_unique<logger::SqliteRepository>(station.database);
        QVERIFY2(station.repository->initialize(&error), qPrintable(error));
        QVERIFY2(station.repository->ensureGeneralLog(&error), qPrintable(error));
        QSqlQuery setLog(station.database);
        setLog.prepare(QStringLiteral("UPDATE contests SET log_uuid = ? WHERE id = 1"));
        setLog.addBindValue(logUuid);
        QVERIFY2(setLog.exec(), qPrintable(setLog.lastError().text()));
        QVERIFY2(station.repository->setSynchronizationEnabled(logUuid, true, &error),
                 qPrintable(error));
    }

    auto &source = *stations.front().repository;
    logger::Qso rejected;
    rejected.contestId = 1;
    rejected.callsign = QStringLiteral("W1BAD");
    rejected.stationCallsign = QStringLiteral("K1TEST");
    rejected.band = QStringLiteral("160m");
    rejected.timestampUtc = QDateTime::currentDateTimeUtc();
    rejected.frequencyHz = 0;
    rejected.mode = QStringLiteral("CW");
    rejected.sentExchange = QStringLiteral("599");
    rejected.receivedExchange = QStringLiteral("599");
    rejected.points = -1;
    QVERIFY2(source.saveQso(rejected, &error), qPrintable(error));
    const QString rejectedOperation =
        source.syncOutbox(logUuid, false, &error).first().operationUuid;
    rejected.callsign = QStringLiteral("W1BAD/P");
    QVERIFY2(source.updateQso(rejected, &error), qPrintable(error));
    logger::Qso valid = rejected;
    valid.id = 0;
    valid.qsoUuid.clear();
    valid.callsign = QStringLiteral("SP6MI/M");
    valid.points = 1;
    QVERIFY2(source.saveQso(valid, &error), qPrintable(error));
    QSignalSpy failures(&stations.front().client, &logger::SyncClient::requestFailed);
    for (std::size_t index = 0; index < stations.size(); ++index) {
        auto &station = stations.at(index);
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = m_server->serverPort();
        configuration.trustedNetworkMode = true;
        configuration.stationUuid = station.repository->localStationUuid(&error);
        configuration.computerName = QStringLiteral("NO-CAT-%1").arg(index);
        configuration.logUuid = logUuid;
        station.client.connectToServer(configuration);
        QVERIFY2(station.client.startSynchronization(*station.repository, &error),
                 qPrintable(error));
    }

    QTRY_COMPARE_WITH_TIMEOUT(m_repository->currentRevision(logUuid), qint64(1), 8000);
    for (auto &station : stations) {
        QTRY_COMPARE_WITH_TIMEOUT(
            station.repository->syncCursor(logUuid), qint64(1), 8000);
        const auto qsos = station.repository->qsosForContest(1);
        const auto found = std::find_if(qsos.cbegin(), qsos.cend(),
            [&valid](const logger::Qso &qso) { return qso.qsoUuid == valid.qsoUuid; });
        QVERIFY(found != qsos.cend());
        QCOMPARE(found->frequencyHz, qint64(0));
        QCOMPARE(found->points, 1);
        QCOMPARE(found->syncStatus, logger::QsoSyncStatus::Synced);
    }
    const auto hostQsos = m_repository->repository()->qsosForContest(1);
    QCOMPARE(hostQsos.size(), 1);
    QCOMPARE(hostQsos.first().frequencyHz, qint64(0));
    auto queued = source.syncOutbox(logUuid, false, &error);
    QCOMPARE(queued.size(), 2);
    QCOMPARE(queued.first().status, QStringLiteral("failed"));
    QVERIFY(queued.first().lastError.contains(QStringLiteral("points")));
    QVERIFY(queued.first().lastError.contains(rejectedOperation));
    QCOMPARE(queued.last().operation, QStringLiteral("qso.update"));
    QCOMPARE(queued.last().status, QStringLiteral("pending"));
    // The dependent update was in the original atomic batch, which rolled back.
    QCOMPARE(queued.last().attempts, 1);
    QVERIFY(std::any_of(failures.cbegin(), failures.cend(),
        [](const QList<QVariant> &failure) {
            return failure.at(1).toString() == QStringLiteral("INVALID_MESSAGE")
                && failure.at(2).toString().contains(QStringLiteral("points"));
        }));

    auto edit = source.qsoById(valid.id, &error);
    QVERIFY(edit.has_value());
    edit->callsign = QStringLiteral("SP6MI/P");
    QVERIFY2(source.updateQso(*edit, &error), qPrintable(error));
    stations.front().client.flushPendingChanges();
    QTRY_COMPARE_WITH_TIMEOUT(m_repository->currentRevision(logUuid), qint64(2), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(stations.back().repository->syncCursor(logUuid), qint64(2), 8000);
    const auto replicated = stations.back().repository->qsosForContest(1);
    QCOMPARE(replicated.size(), 1);
    QCOMPARE(replicated.first().callsign, QStringLiteral("SP6MI/P"));
    QCOMPARE(replicated.first().frequencyHz, qint64(0));
    queued = source.syncOutbox(logUuid, false, &error);
    QCOMPARE(queued.size(), 2);
    QCOMPARE(queued.last().attempts, 1);
}

void SyncServerTest::rejectsConcurrentQsoEditsWithoutOverwriting()
{
    QString error;
    const QVector<logger::Contest> logs =
        m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logs.size(), 1);
    const QString logUuid = logs.first().logUuid;
    const QString originalStation = QString::fromLatin1(StationUuid);
    const QString qsoUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString timestamp = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs);
    const QJsonObject originalQso{
        {QStringLiteral("qso_uuid"), qsoUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), originalStation},
        {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("callsign"), QStringLiteral("W1AW")},
        {QStringLiteral("timestamp_utc"), timestamp},
        {QStringLiteral("frequency_hz"), 14025000},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")},
        {QStringLiteral("sent_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("received_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("sent_exchange"), QStringLiteral("599")},
        {QStringLiteral("received_exchange"), QStringLiteral("599")},
        {QStringLiteral("points"), 1},
        {QStringLiteral("is_invalid"), false}
    };
    const QJsonObject create{
        {QStringLiteral("operation_uuid"),
         QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), originalStation},
        {QStringLiteral("created_at_utc"), timestamp},
        {QStringLiteral("kind"), QStringLiteral("qso.create")},
        {QStringLiteral("expected_version"), 0},
        {QStringLiteral("qso"), originalQso}
    };
    QJsonArray results;
    QVERIFY2(m_repository->submitBatch(logUuid, {create}, &results, &error),
             qPrintable(error));

    const auto connectAndSelect = [this, &logUuid](
        QSslSocket &socket, const QString &stationUuid, const QString &token) {
        if (!connectTls(socket, m_server->serverPort(), m_certificate)) {
            return false;
        }
        QJsonObject response = request(socket, QStringLiteral("hello"), QJsonObject{
            {QStringLiteral("client_version"), QStringLiteral("test")},
            {QStringLiteral("min_protocol_version"), 2},
            {QStringLiteral("max_protocol_version"), 2},
            {QStringLiteral("capabilities"), QJsonArray{}}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("hello_ack")) {
            return false;
        }
        response = request(socket, QStringLiteral("authenticate"), QJsonObject{
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("token"), token}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("authenticated")) {
            return false;
        }
        response = request(socket, QStringLiteral("select_log"), QJsonObject{
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("contest_definition_sha256"), QJsonValue::Null}
        });
        return response.value(QStringLiteral("type")).toString()
            == QStringLiteral("welcome");
    };

    QSslSocket firstSocket;
    QSslSocket secondSocket;
    QVERIFY(connectAndSelect(firstSocket, QString::fromLatin1(StationUuid),
                             QString::fromLatin1(StationToken)));
    QVERIFY(connectAndSelect(secondSocket,
                             QString::fromLatin1(ReadWriteStationUuid),
                             QString::fromLatin1(ReadWriteStationToken)));

    const auto updateRequest = [&logUuid, &qsoUuid, &originalQso](
        const QString &stationUuid, const QString &callsign) {
        QJsonObject qso = originalQso;
        qso.insert(QStringLiteral("callsign"), callsign);
        qso.insert(QStringLiteral("timestamp_utc"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        const QString createdAt =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QString requestId = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const QJsonObject operation{
            {QStringLiteral("operation_uuid"),
             QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("created_at_utc"), createdAt},
            {QStringLiteral("kind"), QStringLiteral("qso.update")},
            {QStringLiteral("expected_version"), 1},
            {QStringLiteral("qso"), qso}
        };
        return qMakePair(requestId, framed(QJsonObject{
            {QStringLiteral("protocol_version"), 2},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("type"), QStringLiteral("submit_batch")},
            {QStringLiteral("payload"), QJsonObject{
                {QStringLiteral("log_uuid"), logUuid},
                {QStringLiteral("atomic"), true},
                {QStringLiteral("operations"), QJsonArray{operation}}
            }}
        }));
    };
    const auto firstUpdate = updateRequest(
        QString::fromLatin1(StationUuid), QStringLiteral("W1AW/P"));
    const auto secondUpdate = updateRequest(
        QString::fromLatin1(ReadWriteStationUuid), QStringLiteral("W1AW/M"));
    QCOMPARE(firstSocket.write(firstUpdate.second), firstUpdate.second.size());
    QCOMPARE(secondSocket.write(secondUpdate.second), secondUpdate.second.size());

    QJsonObject firstResponse;
    QJsonObject secondResponse;
    QVERIFY(readFrame(firstSocket, &firstResponse));
    QVERIFY(readFrame(secondSocket, &secondResponse));
    QCOMPARE(firstResponse.value(QStringLiteral("request_id")).toString(),
             firstUpdate.first);
    QCOMPARE(secondResponse.value(QStringLiteral("request_id")).toString(),
             secondUpdate.first);
    const bool firstAccepted =
        firstResponse.value(QStringLiteral("type")).toString()
            == QStringLiteral("batch_result");
    const bool secondAccepted =
        secondResponse.value(QStringLiteral("type")).toString()
            == QStringLiteral("batch_result");
    QVERIFY(firstAccepted != secondAccepted);
    const QJsonObject conflict = firstAccepted
        ? secondResponse.value(QStringLiteral("payload")).toObject()
        : firstResponse.value(QStringLiteral("payload")).toObject();
    QCOMPARE(conflict.value(QStringLiteral("code")).toString(),
             QStringLiteral("VERSION_CONFLICT"));
    QCOMPARE(m_repository->currentRevision(logUuid, &error), qint64(2));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonObject committed = m_repository->currentQso(logUuid, qsoUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(committed.value(QStringLiteral("callsign")).toString()
                == QStringLiteral("W1AW/P")
            || committed.value(QStringLiteral("callsign")).toString()
                == QStringLiteral("W1AW/M"));
}

void SyncServerTest::retriesCommittedQsoAfterLostResponse()
{
    QString error;
    const QVector<logger::Contest> logs =
        m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logs.size(), 1);
    const QString logUuid = logs.first().logUuid;
    const QString stationUuid = QString::fromLatin1(StationUuid);
    const QString timestamp = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs);
    const QJsonObject qso{
        {QStringLiteral("qso_uuid"),
         QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("callsign"), QStringLiteral("W1LOST")},
        {QStringLiteral("timestamp_utc"), timestamp},
        {QStringLiteral("frequency_hz"), 14025000},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")},
        {QStringLiteral("sent_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("received_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("sent_exchange"), QStringLiteral("599")},
        {QStringLiteral("received_exchange"), QStringLiteral("599")},
        {QStringLiteral("points"), 1},
        {QStringLiteral("is_invalid"), false}
    };
    const QJsonObject operation{
        {QStringLiteral("operation_uuid"),
         QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("created_at_utc"), timestamp},
        {QStringLiteral("kind"), QStringLiteral("qso.create")},
        {QStringLiteral("expected_version"), 0},
        {QStringLiteral("qso"), qso}
    };
    const auto connectAndSelect = [this, &logUuid](QSslSocket &socket) {
        if (!connectTls(socket, m_server->serverPort(), m_certificate)) {
            return false;
        }
        QJsonObject response = request(socket, QStringLiteral("hello"), QJsonObject{
            {QStringLiteral("client_version"), QStringLiteral("test")},
            {QStringLiteral("min_protocol_version"), 2},
            {QStringLiteral("max_protocol_version"), 2},
            {QStringLiteral("capabilities"), QJsonArray{}}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("hello_ack")) {
            return false;
        }
        response = request(socket, QStringLiteral("authenticate"), QJsonObject{
            {QStringLiteral("station_uuid"), QString::fromLatin1(StationUuid)},
            {QStringLiteral("token"), QString::fromLatin1(StationToken)}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("authenticated")) {
            return false;
        }
        response = request(socket, QStringLiteral("select_log"), QJsonObject{
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("contest_definition_sha256"), QJsonValue::Null}
        });
        return response.value(QStringLiteral("type")).toString()
            == QStringLiteral("welcome");
    };
    const auto submitFrame = [&logUuid, &operation](const QString &requestId) {
        return framed(QJsonObject{
            {QStringLiteral("protocol_version"), 2},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("type"), QStringLiteral("submit_batch")},
            {QStringLiteral("payload"), QJsonObject{
                {QStringLiteral("log_uuid"), logUuid},
                {QStringLiteral("atomic"), true},
                {QStringLiteral("operations"), QJsonArray{operation}}
            }}
        });
    };

    QSslSocket lostResponseSocket;
    QVERIFY(connectAndSelect(lostResponseSocket));
    const QString initialRequestId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QByteArray firstFrame = submitFrame(initialRequestId);
    QCOMPARE(lostResponseSocket.write(firstFrame), firstFrame.size());
    QTRY_COMPARE_WITH_TIMEOUT(
        m_repository->currentRevision(logUuid, &error), qint64(1), 5000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    lostResponseSocket.disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(lostResponseSocket.state(),
                              QAbstractSocket::UnconnectedState, 5000);

    QSslSocket retrySocket;
    QVERIFY(connectAndSelect(retrySocket));
    const QString retryRequestId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QByteArray retryFrame = submitFrame(retryRequestId);
    QCOMPARE(retrySocket.write(retryFrame), retryFrame.size());
    QJsonObject response;
    QVERIFY(readFrame(retrySocket, &response));
    QCOMPARE(response.value(QStringLiteral("request_id")).toString(),
             retryRequestId);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("batch_result"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("results")).toArray().first().toObject()
                 .value(QStringLiteral("revision")).toInteger(), qint64(1));
    QCOMPARE(m_repository->currentRevision(logUuid, &error), qint64(1));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(m_repository->eventsSince(logUuid, 0, 10, &error).size(), 1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void SyncServerTest::resumesPendingClientOutboxAfterClientAndServerRestart()
{
    QString error;
    const QVector<logger::Contest> logs =
        m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logs.size(), 1);
    const QString logUuid = logs.first().logUuid;
    const quint16 port = m_server->serverPort();
    m_server->close();
    QVERIFY2(m_server->listenTrustedNetwork(
        QStringLiteral("127.0.0.1"), port, 4, &error), qPrintable(error));
    const QString clientDatabasePath =
        m_directory->filePath(QStringLiteral("restart-client.sqlite"));
    const QString firstConnectionName = QStringLiteral("restart-client-first-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), firstConnectionName);
        database.setDatabaseName(clientDatabasePath);
        QVERIFY(database.open());
        {
            logger::SqliteRepository repository(database);
            QVERIFY2(repository.initialize(&error), qPrintable(error));
            QVERIFY2(repository.ensureGeneralLog(&error), qPrintable(error));
            QSqlQuery updateLog(database);
            updateLog.prepare(QStringLiteral(
                "UPDATE contests SET log_uuid = ? WHERE id = 1"));
            updateLog.addBindValue(logUuid);
            QVERIFY2(updateLog.exec(), qPrintable(updateLog.lastError().text()));
            QSqlQuery updateStation(database);
            updateStation.prepare(QStringLiteral(
                "UPDATE sync_metadata SET value = ? "
                "WHERE key = 'local_station_uuid'"));
            updateStation.addBindValue(QString::fromLatin1(StationUuid));
            QVERIFY2(updateStation.exec(),
                     qPrintable(updateStation.lastError().text()));
            QVERIFY2(repository.setSynchronizationEnabled(logUuid, true, &error),
                     qPrintable(error));

            logger::Qso firstQso;
            firstQso.contestId = 1;
            firstQso.callsign = QStringLiteral("W1FIRST");
            firstQso.stationCallsign = QStringLiteral("K1TEST");
            firstQso.band = QStringLiteral("20m");
            firstQso.timestampUtc = QDateTime::currentDateTimeUtc();
            firstQso.frequencyHz = 14025000;
            firstQso.mode = QStringLiteral("CW");
            firstQso.sentExchange = QStringLiteral("599");
            firstQso.receivedExchange = QStringLiteral("599");
            firstQso.points = 1;
            QVERIFY2(repository.saveQso(firstQso, &error), qPrintable(error));

            logger::SyncClient client;
            logger::SyncClientConfiguration configuration;
            configuration.host = QStringLiteral("127.0.0.1");
            configuration.port = port;
            configuration.stationUuid = QString::fromLatin1(StationUuid);
            configuration.computerName = QStringLiteral("RESTART-STATION");
            configuration.trustedNetworkMode = true;
            configuration.logUuid = logUuid;
            client.connectToServer(configuration);
            QVERIFY2(client.startSynchronization(repository, &error),
                     qPrintable(error));
            QTRY_COMPARE_WITH_TIMEOUT(
                m_repository->currentRevision(logUuid, &error), qint64(1), 8000);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QTRY_COMPARE_WITH_TIMEOUT(
                repository.syncCursor(logUuid, &error), qint64(1), 8000);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            client.disconnectFromServer();

            logger::Qso pendingQso;
            pendingQso.contestId = 1;
            pendingQso.callsign = QStringLiteral("W1PENDING");
            pendingQso.stationCallsign = QStringLiteral("K1TEST");
            pendingQso.band = QStringLiteral("20m");
            pendingQso.timestampUtc = QDateTime::currentDateTimeUtc();
            pendingQso.frequencyHz = 14026000;
            pendingQso.mode = QStringLiteral("CW");
            pendingQso.sentExchange = QStringLiteral("599");
            pendingQso.receivedExchange = QStringLiteral("599");
            pendingQso.points = 1;
            QVERIFY2(repository.saveQso(pendingQso, &error), qPrintable(error));
            const QVector<logger::SyncOutboxOperation> pending =
                repository.syncOutbox(logUuid, false, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(pending.size(), 1);
            QCOMPARE(pending.first().status, QStringLiteral("pending"));
        }
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(firstConnectionName);
    }

    m_server->close();
    m_server.reset();
    m_repository.reset();
    m_repository = std::make_unique<logger::CentralRepository>();
    QVERIFY2(m_repository->initialize(m_directory->path(), &error),
             qPrintable(error));
    QCOMPARE(m_repository->currentRevision(logUuid, &error), qint64(1));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString secondConnectionName = QStringLiteral("restart-client-second-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase restartedDatabase = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), secondConnectionName);
    restartedDatabase.setDatabaseName(clientDatabasePath);
    QVERIFY(restartedDatabase.open());
    {
        logger::SqliteRepository repository(restartedDatabase);
        QVERIFY2(repository.initialize(&error), qPrintable(error));
        logger::SyncClient restartedClient;
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = port;
        configuration.stationUuid = QString::fromLatin1(StationUuid);
        configuration.computerName = QStringLiteral("RESTART-STATION");
        configuration.trustedNetworkMode = true;
        configuration.logUuid = logUuid;
        restartedClient.connectToServer(configuration);
        QVERIFY2(restartedClient.startSynchronization(repository, &error),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            restartedClient.state(), logger::SyncClient::State::Backoff, 5000);

        m_server = std::make_unique<logger::SyncServer>(*m_repository);
        QVERIFY2(m_server->listenTrustedNetwork(
                     QStringLiteral("127.0.0.1"), port, 4, &error),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            restartedClient.state(), logger::SyncClient::State::Ready, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(
            m_repository->currentRevision(logUuid, &error), qint64(2), 15000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            repository.syncCursor(logUuid, &error), qint64(2), 15000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QVector<logger::SyncOutboxOperation> outbox =
            repository.syncOutbox(logUuid, true, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(outbox.size(), 2);
        QVERIFY(std::all_of(outbox.cbegin(), outbox.cend(),
            [](const logger::SyncOutboxOperation &operation) {
                return operation.status == QStringLiteral("accepted");
            }));
        restartedClient.disconnectFromServer();
    }
    restartedDatabase.close();
    restartedDatabase = QSqlDatabase();
    QSqlDatabase::removeDatabase(secondConnectionName);
}

void SyncServerTest::reservesAndAuditsSerialsOverTls()
{
    QString error;
    const QVector<logger::Contest> initialLogs = m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!initialLogs.isEmpty());
    const QString authorizedLogUuid = initialLogs.first().logUuid;
    const QString yaml = QStringLiteral(R"yaml(
schema_version: 1
id: tls-serial-test
name: TLS Serial Test
bands: [20m]
modes: [CW]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
    - {name: serial, type: serial, generated: true, width: 3}
  received:
    - {name: report, type: rst}
scoring:
  serial_start: 7
  serial_scope: global
  serial_server_required: true
dupe_key: [callsign, band, mode]
)yaml");
    logger::ContestDefinition definition;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(
                 yaml, QStringLiteral("TLS serial test"), definition, &error),
             qPrintable(error));
    logger::Contest contest;
    contest.name = QStringLiteral("TLS Serial Test");
    contest.stationCallsign = QStringLiteral("K1TEST");
    const qint64 contestId = m_repository->repository()->createContestLog(
        definition, yaml, contest, &error);
    QVERIFY2(contestId > 0, qPrintable(error));
    const QVector<logger::Contest> logs = m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto log = std::find_if(logs.cbegin(), logs.cend(),
        [contestId](const logger::Contest &item) { return item.id == contestId; });
    QVERIFY(log != logs.cend());

    const auto authenticate = [this](QSslSocket &socket, const QString &stationUuid,
                                     const QString &token, const QString &logUuid) {
        if (!connectTls(socket, m_server->serverPort(), m_certificate)) {
            return false;
        }
        QJsonObject response = request(socket, QStringLiteral("hello"), QJsonObject{
            {QStringLiteral("client_version"), QStringLiteral("test")},
            {QStringLiteral("min_protocol_version"), 2},
            {QStringLiteral("max_protocol_version"), 2},
            {QStringLiteral("capabilities"), QJsonArray{}}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("hello_ack")) {
            qWarning() << "serial test hello failed:" << response;
            return false;
        }
        response = request(socket, QStringLiteral("authenticate"), QJsonObject{
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("token"), token}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("authenticated")) {
            qWarning() << "serial test authentication failed:" << response;
            return false;
        }
        response = request(socket, QStringLiteral("select_log"), QJsonObject{
            {QStringLiteral("log_uuid"), logUuid},
            {QStringLiteral("contest_definition_sha256"), QJsonValue::Null}
        });
        if (response.value(QStringLiteral("type")).toString()
            != QStringLiteral("welcome")) {
            qWarning() << "serial test log selection failed:" << response;
            return false;
        }
        return true;
    };

    QSslSocket operatorSocket;
    QVERIFY(authenticate(operatorSocket,
                         QString::fromLatin1(ReadWriteStationUuid),
                         QString::fromLatin1(ReadWriteStationToken), authorizedLogUuid));
    QJsonObject response = request(
        operatorSocket, QStringLiteral("get_serial_reservations"),
        QJsonObject{{QStringLiteral("log_uuid"), authorizedLogUuid}});
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("FORBIDDEN"));
    response = request(operatorSocket, QStringLiteral("adjust_serial"), QJsonObject{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"),
         QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
        {QStringLiteral("expected_serial"), 7},
        {QStringLiteral("corrected_serial"), 8},
        {QStringLiteral("reason"), QStringLiteral("Unauthorized test")}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("FORBIDDEN"));
    operatorSocket.disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(operatorSocket.state(),
                              QAbstractSocket::UnconnectedState, 5000);

    QSslSocket firstStationSocket;
    QSslSocket secondStationSocket;
    QVERIFY(authenticate(firstStationSocket, QString::fromLatin1(StationUuid),
                         QString::fromLatin1(StationToken), log->logUuid));
    QVERIFY(authenticate(secondStationSocket,
                         QString::fromLatin1(SecondAdminStationUuid),
                         QStringLiteral("fedcba9876543210fedcba9876543210fedcba9876543210"),
                         log->logUuid));
    const QString firstReservationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString secondReservationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QJsonObject firstReservePayload{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), firstReservationUuid},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")}
    };
    const QJsonObject secondReservePayload{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), secondReservationUuid},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")}
    };
    const QString firstRequestId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QString secondRequestId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const auto sendReservationRequest = [](QSslSocket &socket,
                                           const QString &requestId,
                                           const QJsonObject &payload) {
        const QByteArray bytes = framed(QJsonObject{
            {QStringLiteral("protocol_version"), 2},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("type"), QStringLiteral("reserve_serial")},
            {QStringLiteral("payload"), payload}
        });
        return socket.write(bytes) == bytes.size();
    };
    QVERIFY(sendReservationRequest(
        firstStationSocket, firstRequestId, firstReservePayload));
    QVERIFY(sendReservationRequest(
        secondStationSocket, secondRequestId, secondReservePayload));
    QJsonObject firstResponse;
    QJsonObject secondResponse;
    QVERIFY(readFrame(firstStationSocket, &firstResponse));
    QVERIFY(readFrame(secondStationSocket, &secondResponse));
    QCOMPARE(firstResponse.value(QStringLiteral("request_id")).toString(),
             firstRequestId);
    QCOMPARE(secondResponse.value(QStringLiteral("request_id")).toString(),
             secondRequestId);
    QCOMPARE(firstResponse.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reserved"));
    QCOMPARE(secondResponse.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reserved"));
    const QJsonObject firstReservation =
        firstResponse.value(QStringLiteral("payload")).toObject();
    const QJsonObject secondReservation =
        secondResponse.value(QStringLiteral("payload")).toObject();
    const int firstSerial = firstReservation.value(QStringLiteral("serial")).toInt();
    const int secondSerial = secondReservation.value(QStringLiteral("serial")).toInt();
    QVERIFY(firstSerial != secondSerial);
    QCOMPARE(qMin(firstSerial, secondSerial), 7);
    QCOMPARE(qMax(firstSerial, secondSerial), 8);
    const bool firstStationOwnsSerialEight = firstSerial == 8;
    const QString cancelledReservationUuid = firstStationOwnsSerialEight
        ? secondReservationUuid : firstReservationUuid;
    const QString usedReservationUuid = firstStationOwnsSerialEight
        ? firstReservationUuid : secondReservationUuid;
    const QString usedStationUuid = firstStationOwnsSerialEight
        ? QString::fromLatin1(StationUuid)
        : QString::fromLatin1(SecondAdminStationUuid);
    QSslSocket &usedStationSocket = firstStationOwnsSerialEight
        ? firstStationSocket : secondStationSocket;
    const QJsonObject usedReservation = firstStationOwnsSerialEight
        ? firstReservation : secondReservation;
    const QJsonObject usedReservePayload = firstStationOwnsSerialEight
        ? firstReservePayload : secondReservePayload;
    response = request(usedStationSocket, QStringLiteral("reserve_serial"),
                       usedReservePayload);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reserved"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject(), usedReservation);

    response = request(firstStationSocket, QStringLiteral("cancel_serial"), QJsonObject{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), cancelledReservationUuid}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_cancelled"));
    response = request(firstStationSocket, QStringLiteral("get_serial_reservations"),
                       QJsonObject{{QStringLiteral("log_uuid"), log->logUuid}});
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reservations"));
    const QJsonArray audit = response.value(QStringLiteral("payload")).toObject()
                                 .value(QStringLiteral("reservations")).toArray();
    QCOMPARE(audit.size(), 2);
    const auto cancelledEntry = std::find_if(
        audit.cbegin(), audit.cend(),
        [&cancelledReservationUuid](const QJsonValue &value) {
            return value.toObject().value(QStringLiteral("reservation_uuid"))
                .toString() == cancelledReservationUuid;
        });
    QVERIFY(cancelledEntry != audit.cend());
    QCOMPARE(cancelledEntry->toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("cancelled"));
    const QString nextReservationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    response = request(firstStationSocket, QStringLiteral("reserve_serial"), QJsonObject{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), nextReservationUuid},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reserved"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("serial")).toInt(), 9);
    response = request(firstStationSocket, QStringLiteral("adjust_serial"),
                       QJsonObject{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), nextReservationUuid},
        {QStringLiteral("expected_serial"), 9},
        {QStringLiteral("corrected_serial"), 11},
        {QStringLiteral("reason"), QStringLiteral("Verified operator correction")}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_adjusted"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("previous_serial")).toInt(), 9);
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("corrected_serial")).toInt(), 11);
    response = request(firstStationSocket, QStringLiteral("adjust_serial"),
                       QJsonObject{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), nextReservationUuid},
        {QStringLiteral("expected_serial"), 9},
        {QStringLiteral("corrected_serial"), 12},
        {QStringLiteral("reason"), QStringLiteral("Stale correction")}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("SERIAL_CHANGED"));
    const QString timestamp = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs);
    const QString qsoUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QJsonObject qso{
        {QStringLiteral("qso_uuid"), qsoUuid},
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("station_uuid"), usedStationUuid},
        {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("callsign"), QStringLiteral("W1AW")},
        {QStringLiteral("timestamp_utc"), timestamp},
        {QStringLiteral("frequency_hz"), 14025000},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")},
        {QStringLiteral("sent_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")},
            {QStringLiteral("serial"), QStringLiteral("008")}}},
        {QStringLiteral("received_exchange_fields"), QJsonObject{
            {QStringLiteral("report"), QStringLiteral("599")}}},
        {QStringLiteral("sent_exchange"), QStringLiteral("599 008")},
        {QStringLiteral("received_exchange"), QStringLiteral("599")},
        {QStringLiteral("points"), 1},
        {QStringLiteral("is_invalid"), false}
    };
    const QJsonObject operation{
        {QStringLiteral("operation_uuid"),
         QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("station_uuid"), usedStationUuid},
        {QStringLiteral("created_at_utc"), timestamp},
        {QStringLiteral("kind"), QStringLiteral("qso.create")},
        {QStringLiteral("expected_version"), 0},
        {QStringLiteral("reservation_uuid"), usedReservationUuid},
        {QStringLiteral("qso"), qso}
    };
    response = request(usedStationSocket, QStringLiteral("submit_batch"), QJsonObject{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("atomic"), true},
        {QStringLiteral("operations"), QJsonArray{operation}}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("batch_result"));
    response = request(firstStationSocket, QStringLiteral("get_serial_reservations"),
                       QJsonObject{{QStringLiteral("log_uuid"), log->logUuid}});
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reservations"));
    const QJsonArray finalAudit = response.value(QStringLiteral("payload")).toObject()
                                      .value(QStringLiteral("reservations")).toArray();
    QCOMPARE(finalAudit.size(), 3);
    const auto usedReservationEntry = std::find_if(
        finalAudit.cbegin(), finalAudit.cend(),
        [&usedReservationUuid](const QJsonValue &value) {
            return value.toObject().value(QStringLiteral("reservation_uuid"))
                .toString() == usedReservationUuid;
        });
    QVERIFY(usedReservationEntry != finalAudit.cend());
    QCOMPARE(usedReservationEntry->toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("used"));
    QCOMPARE(usedReservationEntry->toObject().value(QStringLiteral("qso_uuid")).toString(),
             qsoUuid);

    const QString lostReplyReservationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QJsonObject lostReplyPayload{
        {QStringLiteral("log_uuid"), log->logUuid},
        {QStringLiteral("reservation_uuid"), lostReplyReservationUuid},
        {QStringLiteral("band"), QStringLiteral("20m")},
        {QStringLiteral("mode"), QStringLiteral("CW")}
    };
    QSslSocket lostReplySocket;
    QVERIFY(authenticate(lostReplySocket, QString::fromLatin1(StationUuid),
                         QString::fromLatin1(StationToken), log->logUuid));
    const QString lostReplyRequestId = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    QVERIFY(sendReservationRequest(
        lostReplySocket, lostReplyRequestId, lostReplyPayload));
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const QJsonArray reservations =
            m_repository->serialReservations(log->logUuid, &error);
        if (!error.isEmpty()) {
            return false;
        }
        return std::any_of(reservations.cbegin(), reservations.cend(),
            [&lostReplyReservationUuid](const QJsonValue &value) {
                return value.toObject()
                           .value(QStringLiteral("reservation_uuid")).toString()
                    == lostReplyReservationUuid;
            });
    }(), 5000);
    lostReplySocket.disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(lostReplySocket.state(),
                              QAbstractSocket::UnconnectedState, 5000);

    QSslSocket retrySocket;
    QVERIFY(authenticate(retrySocket, QString::fromLatin1(StationUuid),
                         QString::fromLatin1(StationToken), log->logUuid));
    response = request(retrySocket, QStringLiteral("reserve_serial"),
                       lostReplyPayload);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_reserved"));
    const QJsonObject retriedReservation =
        response.value(QStringLiteral("payload")).toObject();
    QCOMPARE(retriedReservation.value(QStringLiteral("reservation_uuid")).toString(),
             lostReplyReservationUuid);
    QCOMPARE(retriedReservation.value(QStringLiteral("serial")).toInt(), 12);
    const QJsonArray reservations =
        m_repository->serialReservations(log->logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(reservations.size(), 4);
    const auto retriedEntry = std::find_if(
        reservations.cbegin(), reservations.cend(),
        [&lostReplyReservationUuid](const QJsonValue &value) {
            return value.toObject()
                       .value(QStringLiteral("reservation_uuid")).toString()
                == lostReplyReservationUuid;
        });
    QVERIFY(retriedEntry != reservations.cend());
    QCOMPARE(retriedEntry->toObject().value(QStringLiteral("serial")).toInt(), 12);
}

void SyncServerTest::persistsSerialReservationsInSyncClient()
{
    const QString yaml = QStringLiteral(R"yaml(
schema_version: 1
id: client-serial-test
name: Client Serial Test
bands: [20m]
modes: [CW]
exchange:
  sent:
    - {name: serial, type: serial, generated: true, width: 3}
  received:
    - {name: report, type: rst}
scoring:
  serial_start: 21
  serial_scope: global
  serial_server_required: true
dupe_key: [callsign, band, mode]
)yaml");
    logger::ContestDefinition definition;
    QString error;
    QVERIFY2(logger::ContestDefinitionLoader().loadText(
                 yaml, QStringLiteral("client serial test"), definition, &error),
             qPrintable(error));
    logger::Contest contest;
    contest.name = QStringLiteral("Client Serial Test");
    contest.stationCallsign = QStringLiteral("K1TEST");
    const qint64 contestId = m_repository->repository()->createContestLog(
        definition, yaml, contest, &error);
    QVERIFY2(contestId > 0, qPrintable(error));
    const QVector<logger::Contest> serverLogs =
        m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto serverLog = std::find_if(serverLogs.cbegin(), serverLogs.cend(),
        [contestId](const logger::Contest &item) { return item.id == contestId; });
    QVERIFY(serverLog != serverLogs.cend());

    const QString connectionName = QStringLiteral("serial-client-cache-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(QStringLiteral(":memory:"));
    QVERIFY(database.open());
    {
        logger::SqliteRepository localRepository(database);
        QVERIFY2(localRepository.initialize(&error), qPrintable(error));
        QVERIFY2(localRepository.ensureGeneralLog(&error), qPrintable(error));
        const qint64 localContestId = localRepository.createContestLog(
            definition, yaml, contest, &error);
        QVERIFY2(localContestId > 0, qPrintable(error));
        QSqlQuery updateLog(database);
        updateLog.prepare(QStringLiteral(
            "UPDATE contests SET log_uuid = ? WHERE id = ?"));
        updateLog.addBindValue(serverLog->logUuid);
        updateLog.addBindValue(localContestId);
        QVERIFY2(updateLog.exec(), qPrintable(updateLog.lastError().text()));
        updateLog.finish();
        QSqlQuery updateStation(database);
        updateStation.prepare(QStringLiteral(
            "UPDATE sync_metadata SET value = ? "
            "WHERE key = 'local_station_uuid'"));
        updateStation.addBindValue(QString::fromLatin1(StationUuid));
        QVERIFY2(updateStation.exec(), qPrintable(updateStation.lastError().text()));
        QVERIFY2(localRepository.setSynchronizationEnabled(
                     serverLog->logUuid, true, &error), qPrintable(error));
        const QString reservationUuid = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const logger::SerialReservationRequest queuedReservation{
            serverLog->logUuid, QString::fromLatin1(StationUuid),
            reservationUuid, QStringLiteral("reserve_serial"),
            QStringLiteral("20m"), QStringLiteral("CW")
        };
        QVERIFY2(localRepository.enqueueSerialReservationRequest(
                     queuedReservation, &error), qPrintable(error));

        logger::SyncClient client;
        QSignalSpy responseSpy(&client, &logger::SyncClient::responseReceived);
        QSignalSpy failureSpy(&client, &logger::SyncClient::requestFailed);
        QVERIFY(responseSpy.isValid());
        QVERIFY(failureSpy.isValid());
        logger::SyncClientConfiguration configuration;
        configuration.host = QStringLiteral("127.0.0.1");
        configuration.port = m_server->serverPort();
        configuration.peerName = QStringLiteral("localhost");
        configuration.stationUuid = QString::fromLatin1(StationUuid);
        configuration.token = QString::fromLatin1(StationToken);
        configuration.logUuid = serverLog->logUuid;
        configuration.caCertificates = {m_certificate};
        client.connectToServer(configuration);
        QVERIFY2(client.startSynchronization(localRepository, &error),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), logger::SyncClient::State::Ready,
                                  5000);

        const QString reserveRequestId = client.reserveSerial(
            reservationUuid, QStringLiteral("20m"), QStringLiteral("CW"));
        QVERIFY(!reserveRequestId.isEmpty());
        const auto hasReserveResponse = [&] {
            return std::any_of(responseSpy.cbegin(), responseSpy.cend(),
                [&reserveRequestId](const QList<QVariant> &arguments) {
                    return arguments.size() == 3
                        && arguments.at(0).toString() == reserveRequestId
                        && arguments.at(1).toString()
                            == QStringLiteral("serial_reserved");
                });
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasReserveResponse()
            || std::any_of(failureSpy.cbegin(), failureSpy.cend(),
                [&reserveRequestId](const QList<QVariant> &arguments) {
                    return arguments.size() == 4
                        && arguments.at(0).toString() == reserveRequestId;
                }), 5000);
        QString reserveFailure;
        for (const QList<QVariant> &arguments : failureSpy) {
            if (arguments.size() == 4
                && arguments.at(0).toString() == reserveRequestId) {
                reserveFailure = QStringLiteral("%1: %2")
                    .arg(arguments.at(1).toString(), arguments.at(2).toString());
            }

        }
        QVERIFY2(hasReserveResponse(), qPrintable(reserveFailure));
        const auto reservationResponses = localRepository.serialReservations(
            serverLog->logUuid, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(reservationResponses.size(), 1);
        QCOMPARE(reservationResponses.first().reservationUuid, reservationUuid);
        QCOMPARE(reservationResponses.first().serialNumber, 21);
        QCOMPARE(reservationResponses.first().status, QStringLiteral("reserved"));

        const QString cancelRequestId = client.cancelSerial(reservationUuid);
        QVERIFY(!cancelRequestId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            responseSpy.cbegin(), responseSpy.cend(),
            [&cancelRequestId](const QList<QVariant> &arguments) {
                return arguments.size() == 3
                    && arguments.at(0).toString() == cancelRequestId
                    && arguments.at(1).toString()
                        == QStringLiteral("serial_cancelled");
            }), 5000);
        const std::optional<logger::SerialReservation> cancelled =
            localRepository.serialReservation(
                serverLog->logUuid, reservationUuid, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(cancelled.has_value());
        QCOMPARE(cancelled->status, QStringLiteral("cancelled"));

        const QString usedReservationUuid = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const QString nextReserveRequestId = client.reserveSerial(
            usedReservationUuid, QStringLiteral("20m"), QStringLiteral("CW"));
        QVERIFY(!nextReserveRequestId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(
            responseSpy.cbegin(), responseSpy.cend(),
            [&nextReserveRequestId](const QList<QVariant> &arguments) {
                return arguments.size() == 3
                    && arguments.at(0).toString() == nextReserveRequestId
                    && arguments.at(1).toString()
                        == QStringLiteral("serial_reserved");
            }), 5000);
        const std::optional<logger::SerialReservation> reserved =
            localRepository.serialReservation(
                serverLog->logUuid, usedReservationUuid, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(reserved.has_value());
        QCOMPARE(reserved->serialNumber, 22);

        logger::Qso qso;
        qso.contestId = localContestId;
        qso.callsign = QStringLiteral("W1AW");
        qso.stationCallsign = QStringLiteral("K1TEST");
        qso.band = QStringLiteral("20m");
        qso.timestampUtc = QDateTime::currentDateTimeUtc();
        qso.frequencyHz = 14025000;
        qso.mode = QStringLiteral("CW");
        qso.sentExchange = QStringLiteral("022");
        qso.receivedExchange = QStringLiteral("599");
        qso.sentExchangeFields.insert(
            QStringLiteral("serial"), QStringLiteral("022"));
        qso.receivedExchangeFields.insert(
            QStringLiteral("report"), QStringLiteral("599"));
        qso.reservationUuid = usedReservationUuid;
        qso.points = 1;
        QVERIFY2(localRepository.saveQso(qso, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const QVector<logger::SyncOutboxOperation> operations =
                localRepository.syncOutbox(serverLog->logUuid, true, &error);
            return error.isEmpty() && operations.size() == 1
                && operations.first().status == QStringLiteral("accepted");
        }(), 5000);
        const std::optional<logger::SerialReservation> used =
            localRepository.serialReservation(
                serverLog->logUuid, usedReservationUuid, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(used.has_value());
        QCOMPARE(used->status, QStringLiteral("used"));
        QCOMPARE(used->qsoUuid, qso.qsoUuid);
        const std::optional<logger::Qso> synchronizedQso =
            localRepository.qsoById(qso.id, &error);
        QVERIFY2(synchronizedQso.has_value(), qPrintable(error));
        QCOMPARE(synchronizedQso->reservationUuid, usedReservationUuid);
        QCOMPARE(m_repository->currentRevision(serverLog->logUuid, &error), qint64(1));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        client.disconnectFromServer();
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

void SyncServerTest::createsLogsAndImportsSerialHistoryWithAdminOnly()
{
    const QString yaml = QStringLiteral(R"yaml(
schema_version: 1
id: network-serial-test
name: Network Serial Test
bands: [20m]
modes: [CW]
exchange:
  sent:
    - {name: report, type: rst, default: "599"}
    - {name: serial, type: serial, generated: true, width: 3}
  received:
    - {name: report, type: rst}
scoring:
  serial_start: 7
  serial_scope: global
  serial_server_required: true
dupe_key: [callsign, band, mode]
)yaml");
    const QString logUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    const QJsonObject log{
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("name"), QStringLiteral("Network serial test")},
        {QStringLiteral("station_callsign"), QStringLiteral("K1TEST")},
        {QStringLiteral("starts_at_utc"), QJsonValue::Null},
        {QStringLiteral("properties"), QJsonObject{}}
    };
    const QJsonObject createPayload{
        {QStringLiteral("definition_yaml"), yaml},
        {QStringLiteral("log"), log}
    };

    QSslSocket readWriteSocket;
    QVERIFY(connectTls(readWriteSocket, m_server->serverPort(), m_certificate));
    QJsonObject response = request(readWriteSocket, QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("client_version"), QStringLiteral("test")},
        {QStringLiteral("min_protocol_version"), 2},
        {QStringLiteral("max_protocol_version"), 2},
        {QStringLiteral("capabilities"), QJsonArray{}}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("hello_ack"));
    response = request(readWriteSocket, QStringLiteral("authenticate"), QJsonObject{
        {QStringLiteral("station_uuid"), QString::fromLatin1(ReadWriteStationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(ReadWriteStationToken)}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("authenticated"));
    response = request(readWriteSocket, QStringLiteral("create_log"), createPayload);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("FORBIDDEN"));
    response = request(readWriteSocket,
                       QStringLiteral("import_serial_reservations"),
                       QJsonObject{
                           {QStringLiteral("log_uuid"), logUuid},
                           {QStringLiteral("reservations"), QJsonArray{}}
                       });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("FORBIDDEN"));
    QString error;
    const QVector<logger::Contest> existingLogs =
        m_repository->repository()->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!existingLogs.isEmpty());
    response = request(readWriteSocket, QStringLiteral("select_log"), QJsonObject{
        {QStringLiteral("log_uuid"), existingLogs.first().logUuid},
        {QStringLiteral("contest_definition_sha256"), QJsonValue::Null}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("welcome"));
    response = request(readWriteSocket, QStringLiteral("import_log_batch"),
                       QJsonObject{
                           {QStringLiteral("log_uuid"), existingLogs.first().logUuid},
                           {QStringLiteral("atomic"), true},
                           {QStringLiteral("operations"), QJsonArray{QJsonObject{}}}
                       });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("error"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("FORBIDDEN"));
    readWriteSocket.disconnectFromHost();

    QSslSocket adminSocket;
    QVERIFY(connectTls(adminSocket, m_server->serverPort(), m_certificate));
    response = request(adminSocket, QStringLiteral("hello"), QJsonObject{
        {QStringLiteral("client_version"), QStringLiteral("test")},
        {QStringLiteral("min_protocol_version"), 2},
        {QStringLiteral("max_protocol_version"), 2},
        {QStringLiteral("capabilities"), QJsonArray{}}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("hello_ack"));
    response = request(adminSocket, QStringLiteral("authenticate"), QJsonObject{
        {QStringLiteral("station_uuid"), QString::fromLatin1(StationUuid)},
        {QStringLiteral("token"), QString::fromLatin1(StationToken)}
    });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("authenticated"));
    response = request(adminSocket, QStringLiteral("create_log"), createPayload);
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("log_created"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("log_uuid")).toString(), logUuid);

    const QString stationUuid = QString::fromLatin1(StationUuid);
    const QJsonArray reservations{
        QJsonObject{
            {QStringLiteral("reservation_uuid"),
             QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("scope_key"), QStringLiteral("global")},
            {QStringLiteral("band"), QStringLiteral("20m")},
            {QStringLiteral("mode"), QStringLiteral("CW")},
            {QStringLiteral("serial"), 7},
            {QStringLiteral("status"), QStringLiteral("reserved")},
            {QStringLiteral("qso_uuid"), QString()}
        },
        QJsonObject{
            {QStringLiteral("reservation_uuid"),
             QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()},
            {QStringLiteral("station_uuid"), stationUuid},
            {QStringLiteral("scope_key"), QStringLiteral("global")},
            {QStringLiteral("band"), QStringLiteral("20m")},
            {QStringLiteral("mode"), QStringLiteral("CW")},
            {QStringLiteral("serial"), 8},
            {QStringLiteral("status"), QStringLiteral("cancelled")},
            {QStringLiteral("qso_uuid"), QString()}
        }
    };
    response = request(adminSocket, QStringLiteral("import_serial_reservations"),
                       QJsonObject{
                           {QStringLiteral("log_uuid"), logUuid},
                           {QStringLiteral("reservations"), reservations}
                       });
    QCOMPARE(response.value(QStringLiteral("type")).toString(),
             QStringLiteral("serial_history_imported"));
    QCOMPARE(response.value(QStringLiteral("payload")).toObject()
                 .value(QStringLiteral("count")).toInt(), 2);
    const QJsonArray imported =
        m_repository->serialReservations(logUuid, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(imported.size(), 2);
    QSet<QString> statuses;
    for (const QJsonValue &item : imported) {
        statuses.insert(item.toObject().value(QStringLiteral("status")).toString());
    }
    QVERIFY(statuses.contains(QStringLiteral("reserved")));
    QVERIFY(statuses.contains(QStringLiteral("cancelled")));
}

QTEST_GUILESS_MAIN(SyncServerTest)

#include "test_sync_server.moc"
