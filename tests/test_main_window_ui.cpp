#include "logger/data/sqlite_repository.h"
#include "logger/data/application_data.h"
#include "logger/contest/contest_definition_loader.h"
#include "logger/ui/main_window.h"
#include "logger/ui/new_log_dialog.h"
#include "logger/network/sync_client.h"
#include "logger/network/sync_server.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEvent>
#include <QJsonDocument>
#include <QFrame>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QGroupBox>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <memory>

using namespace logger;

class MainWindowUiTest final : public QObject {
    Q_OBJECT

signals:
    void dxClusterStatusChanged(const logger::DxClusterStatus &status);

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void contestDefinitionConstrainsBandAndModeSelectors();
    void connectionMenuReconnectsAndDisconnects();
    void remoteCentralLogJoinOptionIsAvailable();
    void hostImportRequiresServerMode();
    void networkStatusShowsQueueAndOfflineState();
    void networkStatusListsConnectedStations();
    void dxClusterDialogSelectsDirectoryOrCustomServer();
    void radioConfigDialogPersistsHamlibAndCatTimingOptions();
    void cwKeyerDialogPersistsSerialDeviceAndOutputLine();
    void functionKeyMessagesAreManuallyConfigurable();
    void esmSendsCallAndLogsOnExchangeEnter();
    void esmMessagesAreConfiguredPerContest();
    void autoCqRequiresRunAndStopsWhenEnteringCallsign();
    void frequencyChangeWithinBandSwitchesRunToSearchAndPounce();
    void runFrequencyThreshold_data();
    void runFrequencyThreshold();
    void runFrequencyReferenceResetsOnRunAndCatReconnect();
    void so2vCwTransmissionFollowsRxFocus();
    void cwSpeedShortcutsUpdateTopStatus();
    void liveCwInputSendsImmediatelyAndEscapeStopsThenHides();
    void liveCwInputFollowsActiveRadioAndVfo();
    void bandmapSortAndKeyboardNavigation();
    void bandmapFiltersSpotsByMode();
    void bandmapSpotLifetimeIsConfigurable();
    void clusterAnnouncementsShowOnlyLiveSpots();
    void ctrlWipeClearsCallAndReceivedExchange();
    void supportDataDownloadActionsExist();
    void enterLogsQsoAndRefreshesTable();
    void invalidExchangeIsRejectedWithMessage();
    void duplicateIsRejected();
    void duplicateCanBeLoggedWhenEnabled();
    void duplicateCanBeCreatedByEditingWhenEnabled();
    void editDialogValidatesAndSaves();
    void editStaysOpenWhenOutboxWriteFails();
    void deleteQsoUsesConfirmedTombstone();
    void serialFollowsStoredMaximum();
    void serverSerialReservationRequiresConnectionAndReservation();
    void newLogDialogRequiresCallsignAndContest();
    void newLogActionCreatesLogWithStationCallsign();
    void newLogReadsDefinitionsFromApplicationDirectory();
    void importsAdifToCurrentOrNewLog_data();
    void importsAdifToCurrentOrNewLog();

private:
    QLineEdit *callEdit() const;
    QWidget *panelA() const;
    QLineEdit *exchangeEdit(const QString &name) const;
    QLineEdit *sentEdit(const QString &name) const;
    QTableWidget *table() const;
    QString message() const;
    void enterQso(const QString &call, const QString &report, const QString &serial);
    void runWhenModalAppears(const std::function<void(QDialog *)> &action);

    QTemporaryDir m_settingsDir;
    QTemporaryDir m_dataDir;
    int m_connection = 0;
    QString m_connectionName;
    std::unique_ptr<SqliteRepository> m_repository;
    std::unique_ptr<MainWindow> m_window;
};

void MainWindowUiTest::initTestCase()
{
    Q_INIT_RESOURCE(contests);
    QVERIFY(m_settingsDir.isValid());
    QVERIFY(m_dataDir.isValid());
#ifdef Q_OS_LINUX
    qputenv("XDG_DATA_HOME", m_dataDir.path().toUtf8());
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("ContestLoggerUiTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ContestLoggerUiTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
}

void MainWindowUiTest::newLogReadsDefinitionsFromApplicationDirectory()
{
    QFile source(QStringLiteral(":/contests/serial-sprint.yaml"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QString yaml = QString::fromUtf8(source.readAll());
    yaml.replace(QStringLiteral("name: Serial Sprint"),
                 QStringLiteral("name: Edited disk definition"));
    const QString path = QDir(contestDefinitionsDirectory()).filePath(
        QStringLiteral("serial-sprint.yaml"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(yaml.toUtf8()), yaml.toUtf8().size());
    }
    runWhenModalAppears([this, &yaml](QDialog *dialog) {
        dialog->reject();
        auto *newLog = qobject_cast<NewLogDialog *>(dialog);
        QVERIFY(newLog != nullptr);
        QVERIFY(newLog->selectContest(QStringLiteral("serial-sprint")));
        QVERIFY(newLog->selectedOption() != nullptr);
        QCOMPARE(newLog->selectedOption()->definition.name,
                 QStringLiteral("Edited disk definition (example)"));
        QCOMPARE(newLog->selectedOption()->yaml, yaml);
    });
    auto *action = m_window->findChild<QAction *>(QStringLiteral("newLogAction"));
    QVERIFY(action != nullptr);
    action->trigger();

    QFile restore(path);
    QVERIFY(restore.open(QIODevice::WriteOnly));
    QVERIFY(source.seek(0));
    const QByteArray original = source.readAll();
    QCOMPARE(restore.write(original), original.size());
    const auto selector = m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    const QString stored = m_repository->contestDefinitionYaml(selector->currentData().toLongLong());
    QVERIFY(!stored.contains(QStringLiteral("Edited disk definition")));
}

void MainWindowUiTest::init()
{
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("station/callsign"), QStringLiteral("SP9TEST"));
    settings.sync();

    m_connectionName = QStringLiteral("ui-test-%1").arg(++m_connection);
    QSqlDatabase database =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(m_dataDir.filePath(m_connectionName + QStringLiteral(".sqlite")));
    QVERIFY(database.open());
    m_repository = std::make_unique<SqliteRepository>(database);
    QString error;
    QVERIFY2(m_repository->initialize(&error), qPrintable(error));

    m_window = std::make_unique<MainWindow>(*m_repository);
    m_window->show();
    auto *selector = m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    QVERIFY(selector != nullptr);
    const int sprint = selector->findText(QStringLiteral("Serial Sprint"), Qt::MatchContains);
    QVERIFY2(sprint >= 0, "Serial Sprint log not installed");
    selector->setCurrentIndex(sprint);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(callEdit() != nullptr);
    QVERIFY(exchangeEdit(QStringLiteral("report")) == nullptr);
    auto *band = panelA()->findChildren<QComboBox *>().first();
    band->setCurrentText(QStringLiteral("40m"));
}

void MainWindowUiTest::cleanup()
{
    m_window.reset();
    m_repository.reset();
    QSqlDatabase::database(m_connectionName, false).close();
    QSqlDatabase::removeDatabase(m_connectionName);
}

QLineEdit *MainWindowUiTest::callEdit() const
{
    return m_window->findChild<QLineEdit *>(QStringLiteral("callsignInputA"));
}

QWidget *MainWindowUiTest::panelA() const
{
    QWidget *widget = callEdit();
    while (widget != nullptr && widget->objectName() != QStringLiteral("entryPanel")) {
        widget = widget->parentWidget();
    }
    return widget;
}

QLineEdit *MainWindowUiTest::exchangeEdit(const QString &name) const
{
    return panelA()->findChild<QLineEdit *>(QStringLiteral("exchange_%1").arg(name));
}

QLineEdit *MainWindowUiTest::sentEdit(const QString &name) const
{
    return panelA()->findChild<QLineEdit *>(QStringLiteral("sentExchange_%1").arg(name));
}

QTableWidget *MainWindowUiTest::table() const
{
    return m_window->findChild<QTableWidget *>(QStringLiteral("recentQsos"));
}

QString MainWindowUiTest::message() const
{
    return m_window->findChild<QLabel *>(QStringLiteral("messageLine"))->text();
}

void MainWindowUiTest::contestDefinitionConstrainsBandAndModeSelectors()
{
    auto *bands = m_window->findChild<QComboBox *>(QStringLiteral("bandSelectorA"));
    auto *modes = m_window->findChild<QComboBox *>(QStringLiteral("modeSelectorA"));
    QVERIFY(bands != nullptr);
    QVERIFY(modes != nullptr);
    QCOMPARE(bands->isEditable(), false);
    QCOMPARE(modes->isEditable(), false);
    QCOMPARE(bands->count(), 3);
    QCOMPARE(bands->itemText(0), QStringLiteral("80m"));
    QCOMPARE(bands->itemText(1), QStringLiteral("40m"));
    QCOMPARE(bands->itemText(2), QStringLiteral("20m"));
    QCOMPARE(modes->count(), 1);
    QCOMPARE(modes->itemText(0), QStringLiteral("CW"));
}

void MainWindowUiTest::connectionMenuReconnectsAndDisconnects()
{
    const auto action = [this](const QString &name) {
        return m_window->findChild<QAction *>(name);
    };
    auto *reconnectRadioA = action(QStringLiteral("reconnectRadioAAction"));
    auto *disconnectRadioA = action(QStringLiteral("disconnectRadioAAction"));
    auto *disconnectCluster = action(QStringLiteral("disconnectDxClusterAction"));
    auto *reconnectCw = action(QStringLiteral("reconnectCwKeyerAction"));
    auto *disconnectCw = action(QStringLiteral("disconnectCwKeyerAction"));
    QVERIFY(reconnectRadioA != nullptr);
    QVERIFY(disconnectRadioA != nullptr);
    QVERIFY(action(QStringLiteral("reconnectRadioBAction")) != nullptr);
    QVERIFY(action(QStringLiteral("disconnectRadioBAction")) != nullptr);
    QVERIFY(disconnectCluster != nullptr);
    QVERIFY(reconnectCw != nullptr);
    QVERIFY(disconnectCw != nullptr);
    QVERIFY(m_window->findChild<QAction *>(
                QStringLiteral("configureNetworkConnectionAction")) != nullptr);
    auto *disconnectNetwork = m_window->findChild<QAction *>(
        QStringLiteral("disconnectNetworkConnectionAction"));
    QVERIFY(disconnectNetwork != nullptr);
    QString repositoryError;
    const int localLogCountBefore = m_repository->contests(&repositoryError).size();
    QVERIFY2(repositoryError.isEmpty(), qPrintable(repositoryError));
    disconnectNetwork->trigger();
    QCOMPARE(m_repository->contests(&repositoryError).size(), localLogCountBefore);
    QVERIFY2(repositoryError.isEmpty(), qPrintable(repositoryError));

    disconnectCluster->trigger();
    auto *clusterStatus = m_window->findChild<QLabel *>(QStringLiteral("clusterStatus"));
    QTRY_VERIFY(clusterStatus->text().contains(QStringLiteral("DX cluster: Disconnected")));

    QSettings settings;
    settings.setValue(QStringLiteral("radio/A/backend"),
                      static_cast<int>(RadioBackendType::Mock));
    reconnectRadioA->trigger();
    auto *radioStatus = m_window->findChild<QLabel *>(QStringLiteral("radioStatus"));
    QTRY_VERIFY(radioStatus->text().contains(QStringLiteral("Radio A: "))
                && radioStatus->text().contains(QStringLiteral(" MHz ")));
    disconnectRadioA->trigger();
    QTRY_VERIFY(radioStatus->text().contains(QStringLiteral("Radio A: Radio disconnected.")));

    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    reconnectCw->trigger();
    auto *cwStatus = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("CW: Ready")));
    disconnectCw->trigger();
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("CW: Disconnected")));
}

void MainWindowUiTest::remoteCentralLogJoinOptionIsAvailable()
{
    auto *configure = m_window->findChild<QAction *>(
        QStringLiteral("configureNetworkConnectionAction"));
    QVERIFY(configure != nullptr);
    runWhenModalAppears([this](QDialog *dialog) {
        auto *logSelector =
            dialog->findChild<QComboBox *>(QStringLiteral("syncLogSelector"));
        QVERIFY(logSelector != nullptr);
        QVERIFY(logSelector->findData(QStringLiteral("@remote")) >= 0);
        QVERIFY(dialog->findChild<QLineEdit *>(
                    QStringLiteral("syncCustomCaPath")) != nullptr);
        auto *autoStart = dialog->findChild<QCheckBox *>(
            QStringLiteral("autoStartSyncClientCheck"));
        QVERIFY(autoStart != nullptr);
        QVERIFY(!autoStart->isChecked());
        QVERIFY(dialog->findChild<QCheckBox *>(
                    QStringLiteral("saveSyncTokenCheck")) != nullptr);
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons != nullptr);
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    configure->trigger();
}

void MainWindowUiTest::hostImportRequiresServerMode()
{
    auto *hostAction = m_window->findChild<QAction *>(
        QStringLiteral("configureHostModeAction"));
    const auto *importAction = m_window->findChild<QAction *>(
        QStringLiteral("hostImportLogAction"));
    QVERIFY(hostAction != nullptr);
    QVERIFY(importAction != nullptr);
    QVERIFY(!importAction->isEnabled());
    runWhenModalAppears([](QDialog *dialog) {
        auto *autoStart = dialog->findChild<QCheckBox *>(
            QStringLiteral("autoStartHostServerCheck"));
        QVERIFY(autoStart != nullptr);
        QVERIFY(!autoStart->isChecked());
        dialog->findChild<QDialogButtonBox *>()->button(
            QDialogButtonBox::Cancel)->click();
    });
    hostAction->trigger();
}

void MainWindowUiTest::networkStatusShowsQueueAndOfflineState()
{
    auto *selector =
        m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    QVERIFY(selector != nullptr);
    const qint64 contestId = selector->currentData().toLongLong();
    QString error;
    const QVector<Contest> contests = m_repository->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto contest = std::find_if(
        contests.cbegin(), contests.cend(),
        [contestId](const Contest &item) { return item.id == contestId; });
    QVERIFY(contest != contests.cend());
    QVERIFY2(m_repository->setSynchronizationEnabled(contest->logUuid, true, &error),
             qPrintable(error));
    const QString localStationUuid = m_repository->localStationUuid(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const SerialReservationRequest offlineRequest{
        contest->logUuid, localStationUuid,
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(),
        QStringLiteral("reserve_serial"), QStringLiteral("40m"),
        QStringLiteral("CW")
    };
    QVERIFY2(m_repository->enqueueSerialReservationRequest(
                 offlineRequest, &error), qPrintable(error));
    Qso conflictQso;
    conflictQso.contestId = contestId;
    conflictQso.callsign = QStringLiteral("K9CONFLICT");
    conflictQso.stationCallsign = QStringLiteral("SP9TEST");
    conflictQso.band = QStringLiteral("40m");
    conflictQso.timestampUtc = QDateTime::currentDateTimeUtc();
    conflictQso.frequencyHz = 7100000;
    conflictQso.mode = QStringLiteral("CW");
    conflictQso.sentExchange = QStringLiteral("");
    conflictQso.receivedExchange = QStringLiteral("");
    QVERIFY2(m_repository->saveQso(conflictQso, &error), qPrintable(error));
    const QVector<SyncOutboxOperation> pending =
        m_repository->syncOutbox(contest->logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!pending.isEmpty());
    QJsonObject remote = QJsonDocument::fromJson(
        pending.last().payloadJson.toUtf8()).object()
            .value(QStringLiteral("qso")).toObject();
    remote.insert(QStringLiteral("callsign"), QStringLiteral("K9REMOTE"));
    remote.insert(QStringLiteral("qso_version"), 2);
    remote.insert(QStringLiteral("is_deleted"), false);
    QVERIFY2(m_repository->recordSyncConflict(
                 pending.last().operationUuid,
                 QString::fromUtf8(QJsonDocument(remote)
                                       .toJson(QJsonDocument::Compact)),
                 &error),
             qPrintable(error));

    QAction *statusAction =
        m_window->findChild<QAction *>(QStringLiteral("networkStatusAction"));
    QVERIFY(statusAction != nullptr);
    bool dialogSeen = false;
    runWhenModalAppears([&dialogSeen](QDialog *dialog) {
        if (dialog->objectName() != QStringLiteral("networkStatusDialog")) {
            return;
        }
        dialogSeen = true;
        auto *summary =
            dialog->findChild<QLabel *>(QStringLiteral("networkStatusSummary"));
        auto *queue =
            dialog->findChild<QTableWidget *>(QStringLiteral("networkOutboxTable"));
        auto *serialStatus = dialog->findChild<QTableWidget *>(
            QStringLiteral("serialReservationStatusTable"));
        QVERIFY(summary != nullptr);
        QVERIFY(queue != nullptr);
        QVERIFY(serialStatus != nullptr);
        auto *stations = dialog->findChild<QTableWidget *>(
            QStringLiteral("connectedStationsTable"));
        auto *stationsStatus = dialog->findChild<QLabel *>(
            QStringLiteral("connectedStationsStatus"));
        QVERIFY(stations != nullptr);
        QVERIFY(stationsStatus != nullptr);
        QCOMPARE(stations->rowCount(), 0);
        QVERIFY(stationsStatus->text().contains(QStringLiteral("not connected")));
        QVERIFY(summary->text().contains(QStringLiteral("Conflict")));
        QVERIFY(summary->text().contains(QStringLiteral("Log:")));
        QCOMPARE(queue->rowCount(), 1);
        QCOMPARE(serialStatus->rowCount(), 1);
        QCOMPARE(serialStatus->item(0, 5)->text(),
                 QStringLiteral("Queued offline"));
        auto *compare = dialog->findChild<QPushButton *>(
            QStringLiteral("compareSyncConflictButton"));
        QVERIFY(compare != nullptr);
        QVERIFY(!compare->isEnabled());
        auto *metrics = dialog->findChild<QPushButton *>(
            QStringLiteral("serverMetricsButton"));
        QVERIFY(metrics != nullptr);
        QVERIFY(!metrics->isEnabled());
        queue->selectRow(0);
        QVERIFY(compare->isEnabled());
        QTimer::singleShot(50, dialog, [] {
            auto *comparison =
                qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (comparison != nullptr
                && comparison->objectName()
                    == QStringLiteral("syncConflictComparisonDialog")) {
                auto *table = comparison->findChild<QTableWidget *>(
                    QStringLiteral("syncConflictComparison"));
                QVERIFY(table != nullptr);
                QVERIFY(table->rowCount() > 0);
                QVERIFY(comparison->findChild<QPushButton *>(
                    QStringLiteral("acceptServerConflictButton")) != nullptr);
                QVERIFY(comparison->findChild<QPushButton *>(
                    QStringLiteral("keepLocalConflictButton")) != nullptr);
                QVERIFY(comparison->findChild<QPushButton *>(
                    QStringLiteral("mergeSelectedConflictButton")) != nullptr);
                QVERIFY(comparison->findChild<QComboBox *>(
                    QStringLiteral("syncConflictChoice_callsign")) != nullptr);
                auto *callChoice = comparison->findChild<QComboBox *>(
                    QStringLiteral("syncConflictChoice_callsign"));
                callChoice->setCurrentIndex(1);
                QTimer::singleShot(50, comparison, [] {
                    auto *confirmation =
                        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    if (confirmation != nullptr) {
                        confirmation->button(QMessageBox::Yes)->click();
                    }
                });
                comparison->findChild<QPushButton *>(
                    QStringLiteral("mergeSelectedConflictButton"))->click();
            }
        });
        compare->click();
        dialog->accept();
    });
    statusAction->trigger();
    QVERIFY(dialogSeen);
    const auto mergedQso = m_repository->qsoById(conflictQso.id, &error);
    QVERIFY2(mergedQso.has_value(), qPrintable(error));
    QCOMPARE(mergedQso->callsign, QStringLiteral("K9REMOTE"));
    QCOMPARE(mergedQso->qsoVersion, 3);
    const QVector<SyncOutboxOperation> mergedOperations =
        m_repository->syncOutbox(contest->logUuid, false, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(mergedOperations.size(), 1);
    QCOMPARE(QJsonDocument::fromJson(
                 mergedOperations.first().payloadJson.toUtf8()).object()
                 .value(QStringLiteral("expected_version")).toInt(), 2);
}

void MainWindowUiTest::networkStatusListsConnectedStations()
{
    auto *selector = m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    QVERIFY(selector != nullptr);
    QString error;
    const auto logs = m_repository->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto log = std::find_if(logs.cbegin(), logs.cend(), [selector](const Contest &item) {
        return item.id == selector->currentData().toLongLong();
    });
    QVERIFY(log != logs.cend());
    const QString yaml = m_repository->contestDefinitionYaml(log->id, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    ContestDefinition definition;
    QVERIFY2(ContestDefinitionLoader().loadText(
        yaml, QStringLiteral("UI network test"), definition, &error), qPrintable(error));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    CentralRepository central;
    QVERIFY2(central.initialize(directory.path(), &error), qPrintable(error));
    QVERIFY2(central.createLog(definition, yaml, *log, &error) > 0, qPrintable(error));
    const QString stationUuid = m_repository->localStationUuid(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString peerUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString token = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef");
    const auto credential = [&token, &log](const QString &uuid) {
        return QJsonObject{
            {QStringLiteral("station_uuid"), uuid},
            {QStringLiteral("token"), token},
            {QStringLiteral("permissions"), QJsonArray{QStringLiteral("read")}},
            {QStringLiteral("log_uuids"), QJsonArray{log->logUuid}}
        };
    };
    QFile tokens(directory.filePath(QStringLiteral("tokens.json")));
    QVERIFY(tokens.open(QIODevice::WriteOnly));
    QVERIFY(tokens.write(QJsonDocument(QJsonObject{
        {QStringLiteral("stations"), QJsonArray{credential(stationUuid), credential(peerUuid)}}
    }).toJson()) > 0);
    tokens.close();
    const QDir fixtures(QStringLiteral(CONTEST_LOGGER_TEST_SOURCE_DIR)
                        + QStringLiteral("/tests/data"));
    const QString certificatePath =
        fixtures.filePath(QStringLiteral("sync_server_test_cert.pem"));
    SyncServer server(central);
    QVERIFY2(server.listen(
        QStringLiteral("127.0.0.1"), 0, certificatePath,
        fixtures.filePath(QStringLiteral("sync_server_test_key.pem")),
        tokens.fileName(), 4, &error), qPrintable(error));
    SyncClientConfiguration configuration;
    configuration.host = QStringLiteral("localhost");
    configuration.peerName = QStringLiteral("localhost");
    configuration.port = server.serverPort();
    configuration.stationUuid = peerUuid;
    configuration.computerName = QStringLiteral("RADIO-B");
    configuration.stationCallsign = QStringLiteral("SP9PEER");
    configuration.operatorName = QStringLiteral("Anna SP9ABC");
    configuration.token = token;
    configuration.logUuid = log->logUuid;
    configuration.caCertificates = QSslCertificate::fromPath(certificatePath);
    SyncClient peer;
    peer.connectToServer(configuration);
    QTRY_COMPARE(peer.state(), SyncClient::State::Ready);

    auto *configure = m_window->findChild<QAction *>(
        QStringLiteral("configureNetworkConnectionAction"));
    QVERIFY(configure != nullptr);
    runWhenModalAppears([&](QDialog *dialog) {
        dialog->findChild<QLineEdit *>(QStringLiteral("syncServerHost"))
            ->setText(QStringLiteral("localhost"));
        dialog->findChild<QSpinBox *>(QStringLiteral("syncServerPort"))
            ->setValue(server.serverPort());
        dialog->findChild<QLineEdit *>(QStringLiteral("syncCustomCaPath"))
            ->setText(certificatePath);
        dialog->findChild<QLineEdit *>(QStringLiteral("syncAccessToken"))->setText(token);
        auto *logSelector = dialog->findChild<QComboBox *>(QStringLiteral("syncLogSelector"));
        logSelector->setCurrentIndex(logSelector->findData(log->logUuid));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    configure->trigger();
    QTRY_COMPARE(server.connectedStations(log->logUuid).size(), 2);

    auto *statusAction = m_window->findChild<QAction *>(QStringLiteral("networkStatusAction"));
    QVERIFY(statusAction != nullptr);
    bool dialogSeen = false;
    runWhenModalAppears([&](QDialog *dialog) {
        QTimer::singleShot(15000, dialog, &QDialog::reject);
        QCOMPARE(dialog->objectName(), QStringLiteral("networkStatusDialog"));
        dialogSeen = true;
        auto *stations = dialog->findChild<QTableWidget *>(
            QStringLiteral("connectedStationsTable"));
        auto *label = dialog->findChild<QLabel *>(QStringLiteral("connectedStationsStatus"));
        auto *timer = dialog->findChild<QTimer *>(
            QStringLiteral("connectedStationsRefreshTimer"));
        QVERIFY(stations != nullptr);
        QVERIFY(label != nullptr);
        QVERIFY(timer != nullptr);
        QCOMPARE(timer->interval(), 5000);
        QTRY_COMPARE(stations->rowCount(), 2);
        QCOMPARE(label->text(), QStringLiteral("Connected stations for this log: 2"));
        QSet<QString> computers;
        for (int row = 0; row < stations->rowCount(); ++row) {
            computers.insert(stations->item(row, 0)->text());
            if (stations->item(row, 0)->text() == QHostInfo::localHostName()) {
                QCOMPARE(stations->item(row, 3)->text(),
                         QStringLiteral("Connected (this station)"));
                QCOMPARE(stations->item(row, 1)->text(), QStringLiteral("SP9TEST"));
                QCOMPARE(stations->item(row, 2)->text(), QStringLiteral("Not provided"));
            } else {
                QCOMPARE(stations->item(row, 1)->text(), QStringLiteral("SP9PEER"));
                QCOMPARE(stations->item(row, 2)->text(), QStringLiteral("Anna SP9ABC"));
            }
            for (int column = 0; column < stations->columnCount(); ++column) {
                QVERIFY(QUuid(stations->item(row, column)->text()).isNull());
            }
        }
        QCOMPARE(computers, (QSet<QString>{QHostInfo::localHostName(),
                                          QStringLiteral("RADIO-B")}));
        peer.setStationIdentity(QStringLiteral("SP9NEW"), QStringLiteral("Jan SP9XYZ"));
        dialog->findChild<QPushButton *>(QStringLiteral("refreshNetworkStatusButton"))->click();
        QTRY_VERIFY([&] {
            for (int row = 0; row < stations->rowCount(); ++row) {
                if (stations->item(row, 0)->text() == QStringLiteral("RADIO-B")) {
                    return stations->item(row, 2)->text() == QStringLiteral("Jan SP9XYZ");
                }
            }
            return false;
        }());
        timer->setInterval(20);
        peer.disconnectFromServer();
        QTRY_COMPARE(stations->rowCount(), 1);
        QCOMPARE(stations->item(0, 0)->text(), QHostInfo::localHostName());

        timer->stop();
        peer.connectToServer(configuration);
        QTRY_COMPARE(peer.state(), SyncClient::State::Ready);
        dialog->findChild<QPushButton *>(QStringLiteral("refreshNetworkStatusButton"))->click();
        QTRY_COMPARE(stations->rowCount(), 2);
        auto *disconnect = m_window->findChild<QAction *>(
            QStringLiteral("disconnectNetworkConnectionAction"));
        QVERIFY(disconnect != nullptr);
        disconnect->trigger();
        QTRY_COMPARE(stations->rowCount(), 0);
        QVERIFY(label->text().contains(QStringLiteral("not connected")));
        dialog->accept();
    });
    statusAction->trigger();
    QVERIFY(dialogSeen);
}

void MainWindowUiTest::dxClusterDialogSelectsDirectoryOrCustomServer()
{
    QSettings settings;
    settings.setValue(QStringLiteral("dxCluster/host"), QStringLiteral("my.cluster.test"));
    settings.setValue(QStringLiteral("dxCluster/port"), 7777);
    QAction *configureCluster = nullptr;
    for (QAction *action : m_window->findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("&DX cluster...")) {
            configureCluster = action;
            break;
        }
    }
    QVERIFY(configureCluster != nullptr);

    bool dialogSeen = false;
    runWhenModalAppears([&dialogSeen](QDialog *dialog) {
        dialogSeen = true;
        auto *server = dialog->findChild<QComboBox *>(QStringLiteral("dxClusterServer"));
        auto *host = dialog->findChild<QLineEdit *>(QStringLiteral("dxClusterHost"));
        auto *port = dialog->findChild<QSpinBox *>(QStringLiteral("dxClusterPort"));
        QVERIFY(server != nullptr);
        QVERIFY(host != nullptr);
        QVERIFY(port != nullptr);
        QCOMPARE(server->count(), 28);
        QCOMPARE(server->currentIndex(), 0);
        QCOMPARE(host->text(), QStringLiteral("my.cluster.test"));
        QCOMPARE(port->value(), 7777);

        const int cluster = server->findText(QStringLiteral("SR4DXCW"),
                                             Qt::MatchStartsWith);
        QVERIFY(cluster > 0);
        server->setCurrentIndex(cluster);
        QCOMPARE(host->text(), QStringLiteral("dx.jestok.com"));
        QCOMPARE(port->value(), 7300);
        QVERIFY(!host->isEnabled());
        QVERIFY(!port->isEnabled());

        server->setCurrentIndex(0);
        QVERIFY(host->isEnabled());
        QVERIFY(port->isEnabled());
        host->setText(QStringLiteral("custom.cluster.test"));
        port->setValue(7373);
        QCOMPARE(host->text(), QStringLiteral("custom.cluster.test"));
        QCOMPARE(port->value(), 7373);
        dialog->reject();
    });
    configureCluster->trigger();
    QVERIFY(dialogSeen);
}

void MainWindowUiTest::radioConfigDialogPersistsHamlibAndCatTimingOptions()
{
    QAction *configureRadioB = nullptr;
    for (QAction *action : m_window->findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("Radio B CAT settings...")) {
            configureRadioB = action;
            break;
        }
    }
    QVERIFY(configureRadioB != nullptr);

    bool dialogSeen = false;
    int selectedModelId = 1;
    const QVector<RadioModel> models = availableRadioModels();
#ifdef LOGGER_WITH_HAMLIB
    QVERIFY2(!models.isEmpty(), "Hamlib returned no radio models");
#endif
    if (!models.isEmpty()) {
        selectedModelId = models.constFirst().id;
    }
    runWhenModalAppears([&dialogSeen, &selectedModelId](QDialog *dialog) {
        dialogSeen = true;
        auto *backend = dialog->findChild<QComboBox *>(QStringLiteral("radioBackend"));
        auto *model = dialog->findChild<QComboBox *>(QStringLiteral("radioHamlibModel"));
        auto *transport = dialog->findChild<QComboBox *>(QStringLiteral("radioTransport"));
        auto *host = dialog->findChild<QLineEdit *>(QStringLiteral("radioTcpHost"));
        auto *port = dialog->findChild<QSpinBox *>(QStringLiteral("radioTcpPort"));
        auto *pollInterval =
            dialog->findChild<QComboBox *>(QStringLiteral("radioPollInterval"));
        auto *timeout = dialog->findChild<QSpinBox *>(QStringLiteral("radioTimeout"));
        auto *rtsState = dialog->findChild<QComboBox *>(QStringLiteral("radioRtsState"));
        auto *dtrState = dialog->findChild<QComboBox *>(QStringLiteral("radioDtrState"));
        auto *dataBits = dialog->findChild<QComboBox *>(QStringLiteral("radioDataBits"));
        auto *stopBits = dialog->findChild<QComboBox *>(QStringLiteral("radioStopBits"));
        auto *parity = dialog->findChild<QComboBox *>(QStringLiteral("radioParity"));
        auto *handshake = dialog->findChild<QComboBox *>(QStringLiteral("radioHandshake"));
        QVERIFY(backend != nullptr);
        QVERIFY(model != nullptr);
        QVERIFY(transport != nullptr);
        QVERIFY(host != nullptr);
        QVERIFY(port != nullptr);
        QVERIFY(pollInterval != nullptr);
        QVERIFY(timeout != nullptr);
        QVERIFY(rtsState != nullptr);
        QVERIFY(dtrState != nullptr);
        QVERIFY(dataBits != nullptr);
        QVERIFY(stopBits != nullptr);
        QVERIFY(parity != nullptr);
        QVERIFY(handshake != nullptr);

        backend->setCurrentIndex(backend->findData(static_cast<int>(RadioBackendType::Hamlib)));
        model->setCurrentIndex(model->findData(selectedModelId));
        QCOMPARE(model->currentData().toInt(), selectedModelId);
        QVERIFY(dialog->findChild<QGroupBox *>(QStringLiteral("radioSerialGroup"))->isVisible());
        rtsState->setCurrentIndex(
            rtsState->findData(static_cast<int>(SerialControlState::On)));
        dtrState->setCurrentIndex(
            dtrState->findData(static_cast<int>(SerialControlState::Off)));
        dataBits->setCurrentIndex(dataBits->findData(7));
        stopBits->setCurrentIndex(stopBits->findData(2));
        parity->setCurrentIndex(parity->findData(static_cast<int>(SerialParity::Even)));
        handshake->setCurrentIndex(
            handshake->findData(static_cast<int>(SerialHandshake::Hardware)));
        transport->setCurrentIndex(
            transport->findData(static_cast<int>(RadioTransport::Tcp)));
        QVERIFY(!dialog->findChild<QGroupBox *>(QStringLiteral("radioSerialGroup"))->isVisible());
        QVERIFY(dialog->findChild<QGroupBox *>(QStringLiteral("radioTcpGroup"))->isVisible());
        host->setText(QStringLiteral("localhost"));
        port->setValue(4533);
        pollInterval->setCurrentIndex(pollInterval->findData(500));
        timeout->setValue(4200);
        dialog->findChild<QPushButton *>(QStringLiteral("radioSaveButton"))->click();
    });
    configureRadioB->trigger();
    QVERIFY(dialogSeen);

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("radio/B/backend")).toInt(),
             static_cast<int>(RadioBackendType::Hamlib));
    QCOMPARE(settings.value(QStringLiteral("radio/B/modelId")).toInt(), selectedModelId);
    QCOMPARE(settings.value(QStringLiteral("radio/B/transport")).toInt(),
             static_cast<int>(RadioTransport::Tcp));
    QCOMPARE(settings.value(QStringLiteral("radio/B/host")).toString(),
             QStringLiteral("localhost"));
    QCOMPARE(settings.value(QStringLiteral("radio/B/port")).toInt(), 4533);
    QCOMPARE(settings.value(QStringLiteral("radio/B/pollIntervalMs")).toInt(), 500);
    QCOMPARE(settings.value(QStringLiteral("radio/B/timeoutMs")).toInt(), 4200);
    QCOMPARE(settings.value(QStringLiteral("radio/B/rtsState")).toInt(),
             static_cast<int>(SerialControlState::On));
    QCOMPARE(settings.value(QStringLiteral("radio/B/dtrState")).toInt(),
             static_cast<int>(SerialControlState::Off));
    QCOMPARE(settings.value(QStringLiteral("radio/B/dataBits")).toInt(), 7);
    QCOMPARE(settings.value(QStringLiteral("radio/B/stopBits")).toInt(), 2);
    QCOMPARE(settings.value(QStringLiteral("radio/B/parity")).toInt(),
             static_cast<int>(SerialParity::Even));
    QCOMPARE(settings.value(QStringLiteral("radio/B/handshake")).toInt(),
             static_cast<int>(SerialHandshake::Hardware));
    QVERIFY(!settings.contains(QStringLiteral("radio/A/modelId")));
}

void MainWindowUiTest::cwKeyerDialogPersistsSerialDeviceAndOutputLine()
{
    QAction *configureCw = nullptr;
    for (QAction *action : m_window->findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("CW keyer...")) {
            configureCw = action;
            break;
        }
    }
    QVERIFY(configureCw != nullptr);

    bool dialogSeen = false;
    runWhenModalAppears([&dialogSeen](QDialog *dialog) {
        dialogSeen = true;
        auto *device = dialog->findChild<QLineEdit *>(QStringLiteral("cwSerialDevice"));
        auto *line = dialog->findChild<QComboBox *>(QStringLiteral("cwSerialLine"));
        QVERIFY(device != nullptr);
        QVERIFY(line != nullptr);
        QVERIFY(dialog->findChild<QComboBox *>(QStringLiteral("cwKeyerProvider"))
                == nullptr);
        device->setText(QStringLiteral("/dev/ttyUSB0"));
        line->setCurrentIndex(line->findData(static_cast<int>(CwKeyerSerialLine::Rts)));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    configureCw->trigger();
    QVERIFY(dialogSeen);

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("cw/serial/devicePath")).toString(),
             QStringLiteral("/dev/ttyUSB0"));
    QCOMPARE(settings.value(QStringLiteral("cw/serial/line")).toInt(),
             static_cast<int>(CwKeyerSerialLine::Rts));
    QCOMPARE(settings.value(QStringLiteral("cw/type")).toInt(),
             static_cast<int>(CwKeyerType::Serial));
}

void MainWindowUiTest::functionKeyMessagesAreManuallyConfigurable()
{
    auto *configure = m_window->findChild<QAction *>(
        QStringLiteral("configureFunctionKeysAction"));
    QVERIFY(configure != nullptr);

    bool dialogSeen = false;
    runWhenModalAppears([&dialogSeen](QDialog *dialog) {
        dialogSeen = true;
        auto *label = dialog->findChild<QLineEdit *>(
            QStringLiteral("functionKeyLabel2"));
        auto *text = dialog->findChild<QPlainTextEdit *>(
            QStringLiteral("functionKeyMessage2"));
        QVERIFY(label != nullptr);
        QVERIFY(text != nullptr);
        label->setText(QStringLiteral("Exchange"));
        text->setPlainText(QStringLiteral("{CALL} {MYCALL}\n{SERIAL}"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    configure->trigger();
    QVERIFY(dialogSeen);

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("cw/functionKeys/F2/label")).toString(),
             QStringLiteral("Exchange"));
    QCOMPARE(settings.value(QStringLiteral("cw/functionKeys/F2/text")).toString(),
             QStringLiteral("{CALL} {MYCALL}\n{SERIAL}"));
    auto *button = m_window->findChild<QPushButton *>(QStringLiteral("functionKey2"));
    QVERIFY(button != nullptr);
    QCOMPARE(button->text(), QStringLiteral("F2  Exchange"));
    QVERIFY(button->toolTip().contains(QStringLiteral("{CALL} {MYCALL}")));
}

void MainWindowUiTest::esmSendsCallAndLogsOnExchangeEnter()
{
    QSettings settings;
    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    settings.setValue(QStringLiteral("cw/radioIndex"), 0);
    m_window->findChild<QAction *>(QStringLiteral("reconnectCwKeyerAction"))->trigger();
    auto *cwStatus = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("CW: Ready")));

    auto *esm = m_window->findChild<QAction *>(QStringLiteral("esmAction"));
    QVERIFY(esm != nullptr);
    esm->setChecked(true);

    QLineEdit *callsign = callEdit();
    callsign->setText(QStringLiteral("W1AW"));
    QTest::keyClick(callsign, Qt::Key_Return);
    auto *serial = exchangeEdit(QStringLiteral("serial"));
    QVERIFY(serial != nullptr);
    QTRY_VERIFY(serial->hasFocus());
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("Sending CW.")));

    serial->setText(QStringLiteral("27"));
    QTest::keyClick(serial, Qt::Key_Return);
    QTRY_COMPARE(table()->rowCount(), 1);
    QVERIFY(callsign->text().isEmpty());
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("queue 1")));
}

void MainWindowUiTest::esmMessagesAreConfiguredPerContest()
{
    const qint64 contestId = m_window->findChild<QComboBox *>(
        QStringLiteral("contestSelector"))->currentData().toLongLong();
    auto *configure = m_window->findChild<QAction *>(
        QStringLiteral("configureEsmMessagesAction"));
    QVERIFY(configure != nullptr);

    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->findChild<QPlainTextEdit *>(QStringLiteral("esmCallMessages"))
            ->setPlainText(QStringLiteral("CALL {CALL}\nNR {NR}"));
        dialog->findChild<QPlainTextEdit *>(QStringLiteral("esmLoggedMessages"))
            ->setPlainText(QStringLiteral("TU {MYCALL}"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    configure->trigger();

    const QString prefix = QStringLiteral("cw/esm/%1/").arg(contestId);
    QSettings settings;
    QCOMPARE(settings.value(prefix + QStringLiteral("call")).toString(),
             QStringLiteral("CALL {CALL}\nNR {NR}"));
    QCOMPARE(settings.value(prefix + QStringLiteral("logged")).toString(),
             QStringLiteral("TU {MYCALL}"));

    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    settings.setValue(QStringLiteral("cw/radioIndex"), 0);
    m_window->findChild<QAction *>(QStringLiteral("reconnectCwKeyerAction"))->trigger();
    auto *cwStatus = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("CW: Ready")));
    m_window->findChild<QAction *>(QStringLiteral("esmAction"))->setChecked(true);
    callEdit()->setText(QStringLiteral("W1AW"));
    QTest::keyClick(callEdit(), Qt::Key_Return);
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("queue 1")));
}

void MainWindowUiTest::autoCqRequiresRunAndStopsWhenEnteringCallsign()
{
    QSettings settings;
    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    settings.setValue(QStringLiteral("cw/radioIndex"), 0);
    m_window->findChild<QAction *>(QStringLiteral("reconnectCwKeyerAction"))->trigger();
    auto *cwStatus = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("CW: Ready")));

    auto *mode = m_window->findChild<QComboBox *>(QStringLiteral("runModeSelector"));
    auto *autoCq = m_window->findChild<QAction *>(QStringLiteral("autoCqAction"));
    QVERIFY(mode != nullptr);
    QVERIFY(autoCq != nullptr);
    mode->setCurrentIndex(mode->findData(1));
    autoCq->setChecked(true);
    QVERIFY(!autoCq->isChecked());
    mode->setCurrentIndex(mode->findData(0));
    autoCq->setChecked(true);
    QVERIFY(autoCq->isChecked());
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("Sending CW.")));

    callEdit()->setText(QStringLiteral("K1ABC"));
    QVERIFY(!autoCq->isChecked());
}

void MainWindowUiTest::frequencyChangeWithinBandSwitchesRunToSearchAndPounce()
{
    auto *mode = m_window->findChild<QComboBox *>(QStringLiteral("runModeSelector"));
    QVERIFY(mode != nullptr);

    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Connected;
    snapshot.frequencyHz = 7'030'000;
    snapshot.vfos = {{QStringLiteral("VFOA"), 0, 7'030'000, QStringLiteral("CW"),
                      ConnectionState::Connected}};
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "updateRadioSnapshotForRadio", Qt::DirectConnection,
        Q_ARG(int, 0), Q_ARG(logger::RadioSnapshot, snapshot)));

    mode->setCurrentIndex(mode->findData(0));
    snapshot.frequencyHz = 7'040'000;
    snapshot.vfos[0].frequencyHz = 7'040'000;
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "updateRadioSnapshotForRadio", Qt::DirectConnection,
        Q_ARG(int, 0), Q_ARG(logger::RadioSnapshot, snapshot)));

    QCOMPARE(mode->currentData().toInt(), 1);
    QCOMPARE(QSettings().value(QStringLiteral("operating/runMode")).toInt(), 1);
}

void MainWindowUiTest::runFrequencyThreshold_data()
{
    QTest::addColumn<int>("radio");
    QTest::addColumn<bool>("withVfos");
    QTest::addColumn<int>("direction");
    for (int radio = 0; radio < 2; ++radio) {
        for (bool withVfos : {false, true}) {
            for (int direction : {-1, 1}) {
                const QByteArray name = QStringLiteral("radio%1-vfos%2-direction%3")
                    .arg(radio).arg(withVfos).arg(direction).toLatin1();
                QTest::newRow(name.constData()) << radio << withVfos << direction;
            }
        }
    }
}

void MainWindowUiTest::runFrequencyThreshold()
{
    QFETCH(int, radio);
    QFETCH(bool, withVfos);
    QFETCH(int, direction);
    auto *mode = m_window->findChild<QComboBox *>(QStringLiteral("runModeSelector"));
    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Connected;
    snapshot.frequencyHz = 7'030'000;
    snapshot.mode = QStringLiteral("CW");
    snapshot.focusVfo = 1;
    if (withVfos) {
        snapshot.vfos = {
            {QStringLiteral("A"), 0, 7'020'000, QStringLiteral("CW"), ConnectionState::Connected},
            {QStringLiteral("B"), 1, snapshot.frequencyHz, QStringLiteral("CW"), ConnectionState::Connected}
        };
    }
    const auto publish = [&] {
        return QMetaObject::invokeMethod(
            m_window.get(), "updateRadioSnapshotForRadio", Qt::DirectConnection,
            Q_ARG(int, radio), Q_ARG(logger::RadioSnapshot, snapshot));
    };
    QVERIFY(publish());
    m_window->findChild<QAction *>(QStringLiteral("runModeAction"))->trigger();
    // Small CAT poll-to-poll steps must accumulate relative to the RUN frequency.
    for (int offset : {100, 1000, 2000, 3000, 3001}) {
        snapshot.frequencyHz = 7'030'000 + direction * offset;
        if (withVfos) {
            snapshot.vfos[1].frequencyHz = snapshot.frequencyHz;
        }
        QVERIFY(publish());
        QCOMPARE(mode->currentData().toInt(), offset > 3000 ? 1 : 0);
    }
    QVERIFY(m_window->findChild<QAction *>(QStringLiteral("searchAndPounceAction"))->isChecked());
    QCOMPARE(QSettings().value(QStringLiteral("operating/runMode")).toInt(), 1);
}

void MainWindowUiTest::runFrequencyReferenceResetsOnRunAndCatReconnect()
{
    auto *mode = m_window->findChild<QComboBox *>(QStringLiteral("runModeSelector"));
    auto *run = m_window->findChild<QAction *>(QStringLiteral("runModeAction"));
    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Connected;
    snapshot.mode = QStringLiteral("CW");
    snapshot.frequencyHz = 7'030'000;
    const auto publish = [&] {
        return QMetaObject::invokeMethod(
            m_window.get(), "updateRadioSnapshotForRadio", Qt::DirectConnection,
            Q_ARG(int, 0), Q_ARG(logger::RadioSnapshot, snapshot));
    };
    QVERIFY(publish());
    run->trigger();
    snapshot.frequencyHz += 3001;
    QVERIFY(publish());
    QCOMPARE(mode->currentData().toInt(), 1);
    run->trigger();
    snapshot.frequencyHz += 100;
    QVERIFY(publish());
    QCOMPARE(mode->currentData().toInt(), 0);
    snapshot.state = ConnectionState::Disconnected;
    snapshot.frequencyHz = 7'100'000;
    QVERIFY(publish());
    QCOMPARE(mode->currentData().toInt(), 0);
    snapshot.state = ConnectionState::Connected;
    QVERIFY(publish());
    QCOMPARE(mode->currentData().toInt(), 0);
    snapshot.frequencyHz += 3000;
    QVERIFY(publish());
    QCOMPARE(mode->currentData().toInt(), 0);
    snapshot.frequencyHz += 1;
    QVERIFY(publish());
    QCOMPARE(mode->currentData().toInt(), 1);
}

void MainWindowUiTest::so2vCwTransmissionFollowsRxFocus()
{
    QSettings settings;
    settings.setValue(QStringLiteral("radio/A/backend"),
                      static_cast<int>(RadioBackendType::Mock));
    settings.setValue(QStringLiteral("radio/B/backend"),
                      static_cast<int>(RadioBackendType::Mock));
    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    settings.setValue(QStringLiteral("cw/radioIndex"), 0);
    m_window->findChild<QAction *>(QStringLiteral("reconnectRadioAAction"))->trigger();
    m_window->findChild<QAction *>(QStringLiteral("reconnectRadioBAction"))->trigger();
    auto *radioStatus = m_window->findChild<QLabel *>(QStringLiteral("radioStatus"));
    QTRY_VERIFY(radioStatus->text().contains(QStringLiteral("Radio A: "))
                && radioStatus->text().contains(QStringLiteral(" MHz ")));
    QTRY_VERIFY(radioStatus->text().contains(QStringLiteral("Radio B: "))
                && radioStatus->text().contains(QStringLiteral(" MHz ")));

    auto *so2v = m_window->findChild<QCheckBox *>(QStringLiteral("so2vToggle"));
    auto *callsignB = m_window->findChild<QLineEdit *>(QStringLiteral("callsignInputB"));
    auto *focusVfoB = m_window->findChild<QPushButton *>(QStringLiteral("focusVfoB"));
    auto *activeRadio = m_window->findChild<QComboBox *>(QStringLiteral("activeRadioSelector"));
    auto *radioABandmap = m_window->findChild<QTableWidget *>(
        QStringLiteral("dxBandmapRadioA"));
    auto *radioABandmapDock = m_window->findChild<QDockWidget *>(
        QStringLiteral("radioABandmapDock"));
    auto *radioBBandmapDock = m_window->findChild<QDockWidget *>(
        QStringLiteral("dxBandmapDock"));
    QVERIFY(so2v != nullptr);
    QVERIFY(callsignB != nullptr);
    QVERIFY(focusVfoB != nullptr);
    QVERIFY(activeRadio != nullptr);
    QVERIFY(radioABandmap != nullptr);
    QVERIFY(radioABandmapDock != nullptr);
    QVERIFY(radioBBandmapDock != nullptr);
    QCOMPARE(m_window->dockWidgetArea(radioABandmapDock), Qt::LeftDockWidgetArea);
    QCOMPARE(m_window->dockWidgetArea(radioBBandmapDock), Qt::RightDockWidgetArea);
    QVERIFY(!radioABandmapDock->isVisible());
    so2v->setChecked(true);
    QTRY_VERIFY(radioABandmapDock->isVisible());
    QVERIFY(radioBBandmapDock->windowTitle().contains(QStringLiteral("Radio B")));

    RadioSnapshot savedContextSnapshot;
    savedContextSnapshot.state = ConnectionState::Connected;
    savedContextSnapshot.frequencyHz = 14'025'000;
    savedContextSnapshot.mode = QStringLiteral("CW");
    savedContextSnapshot.focusVfo = 0;
    savedContextSnapshot.dualVfoSupported = true;
    savedContextSnapshot.vfos = {
        {QStringLiteral("A"), 0, 14'025'000, QStringLiteral("CW"),
         ConnectionState::Connected},
        {QStringLiteral("B"), 1, 7'123'000, QStringLiteral("CW"),
         ConnectionState::Connected}
    };
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "updateRadioSnapshotForRadio", Qt::DirectConnection,
        Q_ARG(int, 0), Q_ARG(logger::RadioSnapshot, savedContextSnapshot)));

    const auto headers = m_window->findChildren<QLabel *>(QStringLiteral("entryHeader"));
    QCOMPARE(headers.size(), 2);
    QTest::mouseClick(focusVfoB, Qt::LeftButton);
    QTRY_VERIFY(headers.at(1)->text().contains(QStringLiteral("◀ TX")));
    QCOMPARE(activeRadio->currentData().toInt(), 0);
    QVERIFY(headers.at(1)->parentWidget()->property("active").toBool());
    QTRY_VERIFY(radioStatus->text().contains(QStringLiteral("B 7.123")));
    QVERIFY(radioStatus->text().contains(QStringLiteral("active: A")));
    QTest::mouseClick(callsignB, Qt::LeftButton);

    m_window->findChild<QAction *>(QStringLiteral("reconnectCwKeyerAction"))->trigger();
    auto *cwStatus = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("CW: Ready")));
    QTRY_VERIFY(headers.at(1)->text().contains(QStringLiteral("◀ TX")));

    QTest::mouseClick(callEdit(), Qt::LeftButton);
    QTest::keyClick(m_window.get(), Qt::Key_F1);
    QTRY_VERIFY(cwStatus->text().contains(QStringLiteral("TX VFO B")));
}

void MainWindowUiTest::cwSpeedShortcutsUpdateTopStatus()
{
    auto *top = m_window->findChild<QLabel *>(QStringLiteral("topStatus"));
    QVERIFY(top != nullptr);
    QVERIFY(top->text().contains(QStringLiteral("25 WPM")));
    callEdit()->setFocus();
    QTest::keyClick(callEdit(), Qt::Key_PageUp);
    QCOMPARE(QSettings().value(QStringLiteral("cw/wpm")).toInt(), 26);
    QVERIFY(top->text().contains(QStringLiteral("26 WPM")));
    QTest::keyClick(callEdit(), Qt::Key_PageDown);
    QCOMPARE(QSettings().value(QStringLiteral("cw/wpm")).toInt(), 25);
    QVERIFY(top->text().contains(QStringLiteral("25 WPM")));

    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "setCwWpm", Q_ARG(int, 80)));
    QTest::keyClick(callEdit(), Qt::Key_PageUp);
    QCOMPARE(QSettings().value(QStringLiteral("cw/wpm")).toInt(), 80);
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "setCwWpm", Q_ARG(int, 5)));
    QTest::keyClick(callEdit(), Qt::Key_PageDown);
    QCOMPARE(QSettings().value(QStringLiteral("cw/wpm")).toInt(), 5);

    auto *mode = m_window->findChild<QComboBox *>(QStringLiteral("modeSelectorA"));
    mode->addItem(QStringLiteral("SSB"));
    mode->setCurrentText(QStringLiteral("SSB"));
    QTRY_VERIFY(!top->text().contains(QStringLiteral("WPM")));
}

void MainWindowUiTest::liveCwInputSendsImmediatelyAndEscapeStopsThenHides()
{
    QVERIFY(m_window->findChild<QDockWidget *>(QStringLiteral("cwKeyerDock")) == nullptr);
    QVERIFY(m_window->findChild<QComboBox *>(QStringLiteral("cwRadioSelector")) == nullptr);
    QSettings().setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    m_window->findChild<QAction *>(QStringLiteral("reconnectCwKeyerAction"))->trigger();
    auto *status = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(status->text().contains(QStringLiteral("CW: Ready")));

    QTest::keyClick(callEdit(), Qt::Key_K, Qt::ControlModifier);
    auto *inputWindow = m_window->findChild<QWidget *>(QStringLiteral("cwInputWindow"));
    auto *input = m_window->findChild<QLineEdit *>(QStringLiteral("cwMessageInput"));
    QVERIFY(inputWindow != nullptr);
    QVERIFY(input != nullptr);
    QTRY_VERIFY(inputWindow->isVisible());
    QTRY_VERIFY(input->hasFocus());
    QTest::keyClicks(input, "CQ TEST");
    QTRY_VERIFY(status->text().contains(QStringLiteral("TX VFO A")));
    QCOMPARE(input->text(), QStringLiteral("CQ TEST"));
    QTest::keyClick(input, Qt::Key_PageUp);
    QCOMPARE(QSettings().value(QStringLiteral("cw/wpm")).toInt(), 26);
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(status->text().contains(QStringLiteral("aborted")));
    QVERIFY(inputWindow->isVisible());
    QVERIFY(input->text().isEmpty());
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(!inputWindow->isVisible());
    QVERIFY(!status->text().contains(QStringLiteral("TX VFO")));

    QTest::keyClick(callEdit(), Qt::Key_K, Qt::ControlModifier);
    QTRY_VERIFY(inputWindow->isVisible());
    QTest::keyClick(input, Qt::Key_T);
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(status->text().contains(QStringLiteral("aborted")));
    QVERIFY(inputWindow->isVisible());
}

void MainWindowUiTest::liveCwInputFollowsActiveRadioAndVfo()
{
    auto *activeRadio = m_window->findChild<QComboBox *>(QStringLiteral("activeRadioSelector"));
    activeRadio->setCurrentIndex(activeRadio->findData(1));
    QSettings().setValue(QStringLiteral("cw/type"), static_cast<int>(CwKeyerType::Mock));
    m_window->findChild<QAction *>(QStringLiteral("reconnectCwKeyerAction"))->trigger();
    auto *status = m_window->findChild<QLabel *>(QStringLiteral("cwKeyerStatus"));
    QTRY_VERIFY(status->text().contains(QStringLiteral("CW: Ready")));
    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Connected;
    snapshot.mode = QStringLiteral("CW");
    snapshot.frequencyHz = 7'025'000;
    snapshot.focusVfo = 1;
    snapshot.dualVfoSupported = true;
    snapshot.vfos = {
        {QStringLiteral("A"), 0, 7'025'000, QStringLiteral("CW"), ConnectionState::Connected},
        {QStringLiteral("B"), 1, 7'030'000, QStringLiteral("CW"), ConnectionState::Connected}
    };
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "updateRadioSnapshotForRadio", Qt::DirectConnection,
        Q_ARG(int, 1), Q_ARG(logger::RadioSnapshot, snapshot)));
    m_window->findChild<QAction *>(QStringLiteral("cwInputAction"))->trigger();
    auto *input = m_window->findChild<QLineEdit *>(QStringLiteral("cwMessageInput"));
    QTest::keyClicks(input, "TEST");
    QTRY_VERIFY(status->text().contains(QStringLiteral("TX VFO B")));
    m_window->findChild<QAction *>(QStringLiteral("abortCwAction"))->trigger();
    QTRY_VERIFY(status->text().contains(QStringLiteral("aborted")));

    activeRadio->setCurrentIndex(activeRadio->findData(0));
    QTest::keyClicks(input, "CQ");
    QTRY_VERIFY(status->text().contains(QStringLiteral("TX VFO A")));
}

void MainWindowUiTest::bandmapSortAndKeyboardNavigation()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto addSpot = [this, &now](const QString &callsign, qint64 frequency,
                                      int ageSeconds) {
        Spot spot;
        spot.callsign = callsign;
        spot.frequencyHz = frequency;
        spot.band = QStringLiteral("40m");
        spot.mode = QStringLiteral("CW");
        spot.spotter = QStringLiteral("K1ABC");
        spot.comment = QStringLiteral("CQ TEST");
        spot.timestampUtc = now.addSecs(-ageSeconds);
        spot.source = QStringLiteral("test");
        return QMetaObject::invokeMethod(
            m_window.get(), "receiveSpot", Qt::DirectConnection,
            Q_ARG(logger::Spot, spot));
    };
    QVERIFY(addSpot(QStringLiteral("K1NEW"), 7'040'000, 10));
    QVERIFY(addSpot(QStringLiteral("K1OLD"), 7'020'000, 60));
    QVERIFY(addSpot(QStringLiteral("K1MID"), 7'030'000, 30));

    auto *bandmap = m_window->findChild<QTableWidget *>(QStringLiteral("dxBandmap"));
    auto *sortSelector = m_window->findChild<QComboBox *>(
        QStringLiteral("bandmapSortSelector"));
    auto *previousAction = m_window->findChild<QAction *>(
        QStringLiteral("previousBandmapSpotAction"));
    auto *nextAction = m_window->findChild<QAction *>(
        QStringLiteral("nextBandmapSpotAction"));
    QVERIFY(bandmap != nullptr);
    QVERIFY(sortSelector != nullptr);
    QVERIFY(previousAction != nullptr);
    QVERIFY(nextAction != nullptr);
    QCOMPARE(bandmap->rowCount(), 3);
    QCOMPARE(bandmap->item(0, 1)->text(), QStringLiteral("K1OLD"));
    QCOMPARE(bandmap->item(1, 1)->text(), QStringLiteral("K1MID"));
    QCOMPARE(bandmap->item(2, 1)->text(), QStringLiteral("K1NEW"));

    sortSelector->setCurrentIndex(sortSelector->findData(QStringLiteral("time")));
    QCOMPARE(bandmap->item(0, 1)->text(), QStringLiteral("K1NEW"));
    QCOMPARE(bandmap->item(1, 1)->text(), QStringLiteral("K1MID"));
    QCOMPARE(bandmap->item(2, 1)->text(), QStringLiteral("K1OLD"));

    callEdit()->setFocus();
    QTest::keyClick(callEdit(), Qt::Key_Down, Qt::ControlModifier);
    QCOMPARE(bandmap->currentRow(), 0);
    QTest::keyClick(callEdit(), Qt::Key_Down, Qt::ControlModifier);
    QCOMPARE(bandmap->currentRow(), 1);
    QTest::keyClick(callEdit(), Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(bandmap->currentRow(), 0);
    QTest::keyClick(callEdit(), Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(bandmap->currentRow(), 2);
}

void MainWindowUiTest::bandmapSpotLifetimeIsConfigurable()
{
    m_window.reset();
    Spot spot;
    spot.callsign = QStringLiteral("K1OLD");
    spot.frequencyHz = 7'040'000;
    spot.band = QStringLiteral("40m");
    spot.mode = QStringLiteral("CW");
    spot.spotter = QStringLiteral("K1ABC");
    spot.comment = QStringLiteral("CQ TEST");
    spot.timestampUtc = QDateTime::currentDateTimeUtc().addSecs(-120);
    spot.source = QStringLiteral("test");
    QString error;
    QVERIFY2(m_repository->saveSpot(spot, &error), qPrintable(error));
    m_window = std::make_unique<MainWindow>(*m_repository);
    m_window->show();

    auto *bandmap = m_window->findChild<QTableWidget *>(QStringLiteral("dxBandmap"));
    auto *bandFilter = m_window->findChild<QComboBox *>(
        QStringLiteral("bandmapBandFilter"));
    QVERIFY(bandmap != nullptr);
    QVERIFY(bandFilter != nullptr);
    bandFilter->setCurrentIndex(bandFilter->findData(QString()));
    QCOMPARE(bandmap->rowCount(), 1);

    QSettings().setValue(QStringLiteral("dxCluster/spotLifetimeMinutes"), 1);
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "refreshBandmap"));
    QCOMPARE(bandmap->rowCount(), 0);
}

void MainWindowUiTest::bandmapFiltersSpotsByMode()
{
    const auto addSpot = [this](const QString &callsign, const QString &mode) {
        Spot spot;
        spot.callsign = callsign;
        spot.frequencyHz = 7'040'000;
        spot.band = QStringLiteral("40m");
        spot.mode = mode;
        spot.spotter = QStringLiteral("K1ABC");
        spot.comment = QStringLiteral("CQ TEST");
        spot.timestampUtc = QDateTime::currentDateTimeUtc();
        spot.source = QStringLiteral("test");
        return QMetaObject::invokeMethod(
            m_window.get(), "receiveSpot", Qt::DirectConnection,
            Q_ARG(logger::Spot, spot));
    };
    QVERIFY(addSpot(QStringLiteral("K1CW"), QStringLiteral("CW")));
    QVERIFY(addSpot(QStringLiteral("K1SSB"), QStringLiteral("SSB")));
    QVERIFY(addSpot(QStringLiteral("K1UNKNOWN"), QString()));

    auto *bandmap = m_window->findChild<QTableWidget *>(QStringLiteral("dxBandmap"));
    auto *modeFilter = m_window->findChild<QComboBox *>(
        QStringLiteral("bandmapModeFilter"));
    QVERIFY(bandmap != nullptr);
    QVERIFY(modeFilter != nullptr);
    QCOMPARE(bandmap->rowCount(), 3);

    modeFilter->setCurrentIndex(modeFilter->findData(QStringLiteral("CW")));
    QCOMPARE(bandmap->rowCount(), 1);
    QCOMPARE(bandmap->item(0, 1)->text(), QStringLiteral("K1CW"));

    modeFilter->setCurrentIndex(modeFilter->findData(QString()));
    QCOMPARE(bandmap->rowCount(), 3);

    auto *continentFilter = m_window->findChild<QComboBox *>(
        QStringLiteral("bandmapContinentFilter"));
    QVERIFY(continentFilter != nullptr);
    continentFilter->setCurrentIndex(
        continentFilter->findData(QStringLiteral("@unknown")));
    QCOMPARE(bandmap->rowCount(), 3);
    continentFilter->setCurrentIndex(
        continentFilter->findData(QStringLiteral("EU")));
    QCOMPARE(bandmap->rowCount(), 0);
    continentFilter->setCurrentIndex(continentFilter->findData(QString()));
    QCOMPARE(bandmap->rowCount(), 3);
}

void MainWindowUiTest::clusterAnnouncementsShowOnlyLiveSpots()
{
    auto *announcements = m_window->findChild<QTableWidget *>(
        QStringLiteral("clusterLog"));
    QVERIFY(announcements != nullptr);
    QCOMPARE(announcements->rowCount(), 0);

    QVERIFY(QObject::connect(
        this, SIGNAL(dxClusterStatusChanged(logger::DxClusterStatus)),
        m_window.get(), SLOT(updateDxClusterStatus(logger::DxClusterStatus)),
        Qt::DirectConnection));
    emit dxClusterStatusChanged(
        {ConnectionState::Connected, QStringLiteral("test"), 0});

    Spot spot;
    spot.callsign = QStringLiteral("K1LIVE");
    spot.frequencyHz = 7'040'000;
    spot.band = QStringLiteral("40m");
    spot.mode = QStringLiteral("CW");
    spot.spotter = QStringLiteral("K1ABC");
    spot.comment = QStringLiteral("CQ TEST");
    spot.timestampUtc = QDateTime::currentDateTimeUtc();
    spot.source = QStringLiteral("test");
    QVERIFY(QMetaObject::invokeMethod(
        m_window.get(), "receiveSpot", Qt::DirectConnection,
        Q_ARG(logger::Spot, spot)));
    QCOMPARE(announcements->rowCount(), 1);

    emit dxClusterStatusChanged(
        {ConnectionState::Disconnected, QStringLiteral("test"), 0});
    QCOMPARE(announcements->rowCount(), 0);
}

void MainWindowUiTest::ctrlWipeClearsCallAndReceivedExchange()
{
    callEdit()->setText(QStringLiteral("SP2ABC"));
    exchangeEdit(QStringLiteral("serial"))->setText(QStringLiteral("123"));
    QTest::keyClick(callEdit(), Qt::Key_W, Qt::ControlModifier);
    QVERIFY(callEdit()->text().isEmpty());
    QVERIFY(exchangeEdit(QStringLiteral("serial"))->text().isEmpty());
    QCOMPARE(sentEdit(QStringLiteral("serial"))->text(), QStringLiteral("001"));
}

void MainWindowUiTest::supportDataDownloadActionsExist()
{
    QVERIFY(m_window->findChild<QAction *>(QStringLiteral("downloadMasterScpAction"))
            != nullptr);
    QVERIFY(m_window->findChild<QAction *>(QStringLiteral("downloadWlCtyAction"))
            != nullptr);
}

void MainWindowUiTest::enterQso(const QString &call, const QString &report,
                                const QString &serial)
{
    callEdit()->setText(call);
    Q_UNUSED(report)
    exchangeEdit(QStringLiteral("serial"))->setText(serial);
    QTest::keyClick(callEdit(), Qt::Key_Return);
    QCoreApplication::processEvents();
}

void MainWindowUiTest::runWhenModalAppears(const std::function<void(QDialog *)> &action)
{
    auto *timer = new QTimer(this);
    timer->setInterval(20);
    connect(timer, &QTimer::timeout, this, [timer, action] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        action(dialog);
    });
    timer->start();
}

void MainWindowUiTest::enterLogsQsoAndRefreshesTable()
{
    QCOMPARE(table()->rowCount(), 0);
    QCOMPARE(sentEdit(QStringLiteral("serial"))->text(), QStringLiteral("001"));
    enterQso(QStringLiteral("dl1abc"), QStringLiteral("599"), QStringLiteral("12"));
    QVERIFY2(table()->rowCount() == 1, qPrintable(message()));
    QCOMPARE(table()->item(0, 5)->text(), QStringLiteral("DL1ABC"));
    // First QSO with prefix DL1 is a new multiplier.
    QCOMPARE(table()->item(0, 8)->text(), QStringLiteral("1"));
    QVERIFY(callEdit()->text().isEmpty());
    QCOMPARE(sentEdit(QStringLiteral("serial"))->text(), QStringLiteral("002"));

    enterQso(QStringLiteral("DL1XYZ"), QStringLiteral("599"), QStringLiteral("3"));
    QCOMPARE(table()->rowCount(), 2);
    QVERIFY(table()->item(1, 8)->text().isEmpty());
}

void MainWindowUiTest::invalidExchangeIsRejectedWithMessage()
{
    enterQso(QStringLiteral("OK1AA"), QString(), QStringLiteral("x"));
    QCOMPARE(table()->rowCount(), 0);
    QVERIFY2(message().contains(QStringLiteral("Not logged")), qPrintable(message()));
    QVERIFY(message().contains(QStringLiteral("serial")));
    QCOMPARE(callEdit()->text(), QStringLiteral("OK1AA"));

    enterQso(QStringLiteral("##"), QStringLiteral("599"), QStringLiteral("1"));
    QCOMPARE(table()->rowCount(), 0);
    QVERIFY(message().contains(QStringLiteral("callsign")));
}

void MainWindowUiTest::duplicateIsRejected()
{
    enterQso(QStringLiteral("G4AAA"), QStringLiteral("599"), QStringLiteral("1"));
    QCOMPARE(table()->rowCount(), 1);
    callEdit()->setText(QStringLiteral("G4AAA"));
    QCoreApplication::processEvents();
    auto *badge = panelA()->findChild<QLabel *>(QStringLiteral("dupeBadge"));
    QVERIFY(badge != nullptr);
    QCOMPARE(badge->text(), QStringLiteral("DUPE"));
    enterQso(QStringLiteral("G4AAA"), QStringLiteral("599"), QStringLiteral("2"));
    QCOMPARE(table()->rowCount(), 1);
    QVERIFY2(message().contains(QStringLiteral("DUPE")), qPrintable(message()));
}

void MainWindowUiTest::duplicateCanBeLoggedWhenEnabled()
{
    auto *allowDupes = m_window->findChild<QAction *>(
        QStringLiteral("allowDupeLoggingAction"));
    QVERIFY(allowDupes != nullptr);
    allowDupes->setChecked(true);

    enterQso(QStringLiteral("G4AAA"), QStringLiteral("599"), QStringLiteral("1"));
    enterQso(QStringLiteral("G4AAA"), QStringLiteral("599"), QStringLiteral("2"));

    QCOMPARE(table()->rowCount(), 2);
    QCOMPARE(QSettings().value(QStringLiteral("operating/allowDupeLogging")).toBool(), true);
    QString error;
    const qint64 contestId = m_window->findChild<QComboBox *>(
        QStringLiteral("contestSelector"))->currentData().toLongLong();
    const QVector<Qso> logged = m_repository->qsosForContest(contestId, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logged.size(), 2);
    QVERIFY((logged.at(0).points == 0 && logged.at(1).points > 0)
            || (logged.at(1).points == 0 && logged.at(0).points > 0));
    QVERIFY(message().contains(QStringLiteral("DUPE logged")));
}

void MainWindowUiTest::duplicateCanBeCreatedByEditingWhenEnabled()
{
    auto *allowDupes = m_window->findChild<QAction *>(
        QStringLiteral("allowDupeLoggingAction"));
    QVERIFY(allowDupes != nullptr);
    allowDupes->setChecked(true);
    enterQso(QStringLiteral("G4AAA"), QStringLiteral("599"), QStringLiteral("1"));
    enterQso(QStringLiteral("G4BBB"), QStringLiteral("599"), QStringLiteral("2"));

    const qint64 selectedId = table()->item(0, 0)->data(Qt::UserRole).toLongLong();
    QString error;
    const auto selected = m_repository->qsoById(selectedId, &error);
    QVERIFY2(selected.has_value(), qPrintable(error));
    const QString otherCall = selected->callsign == QStringLiteral("G4AAA")
        ? QStringLiteral("G4BBB") : QStringLiteral("G4AAA");

    runWhenModalAppears([&otherCall](QDialog *dialog) {
        dialog->findChild<QLineEdit *>(QStringLiteral("editCallsign"))
            ->setText(otherCall);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "editSelectedQso",
                                      Q_ARG(int, 0), Q_ARG(int, 0)));

    const qint64 contestId = m_window->findChild<QComboBox *>(
        QStringLiteral("contestSelector"))->currentData().toLongLong();
    const QVector<Qso> logged = m_repository->qsosForContest(contestId, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(logged.size(), 2);
    QVERIFY(std::any_of(logged.cbegin(), logged.cend(), [](const Qso &qso) {
        return qso.points == 0;
    }));
    QVERIFY(message().contains(QStringLiteral("updated")));
}

void MainWindowUiTest::editDialogValidatesAndSaves()
{
    enterQso(QStringLiteral("SP5AA"), QStringLiteral("599"), QStringLiteral("7"));
    enterQso(QStringLiteral("SP6BB"), QStringLiteral("599"), QStringLiteral("8"));
    QCOMPARE(table()->rowCount(), 2);

    bool sawError = false;
    runWhenModalAppears([&sawError, this](QDialog *dialog) {
        QCOMPARE(dialog->objectName(), QStringLiteral("editQsoDialog"));
        auto *call = dialog->findChild<QLineEdit *>(QStringLiteral("editCallsign"));
        auto *serial = dialog->findChild<QLineEdit *>(QStringLiteral("editRcvd_serial"));
        auto *errors = dialog->findChild<QLabel *>(QStringLiteral("editErrors"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(call && serial && errors && buttons);
        // Changing SP5AA into SP6BB on the same band/mode must be refused as a dupe.
        call->setText(QStringLiteral("SP6BB"));
        buttons->button(QDialogButtonBox::Save)->click();
        QCoreApplication::processEvents();
        sawError = dialog->isVisible() && !errors->text().isEmpty();
        call->setText(QStringLiteral("SP5ZZ"));
        serial->setText(QStringLiteral("42"));
        buttons->button(QDialogButtonBox::Save)->click();
    });
    QMetaObject::invokeMethod(m_window.get(), "editSelectedQso", Q_ARG(int, 0), Q_ARG(int, 0));
    QVERIFY(sawError);
    QCOMPARE(table()->item(0, 5)->text(), QStringLiteral("SP5ZZ"));
    QVERIFY(table()->item(0, 7)->text().contains(QStringLiteral("42")));
    QVERIFY(message().contains(QStringLiteral("updated")));
}

void MainWindowUiTest::deleteQsoUsesConfirmedTombstone()
{
    enterQso(QStringLiteral("W1AW"), QStringLiteral("599"), QStringLiteral("1"));
    QCOMPARE(table()->rowCount(), 1);
    const qint64 qsoId = table()->item(0, 0)->data(Qt::UserRole).toLongLong();

    runWhenModalAppears([](QDialog *dialog) {
        auto *confirmation = qobject_cast<QMessageBox *>(dialog);
        QVERIFY(confirmation != nullptr);
        confirmation->button(QMessageBox::Yes)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "deleteSelectedQso",
                                      Q_ARG(int, 0)));
    QCOMPARE(table()->rowCount(), 0);
    QString error;
    QVERIFY(!m_repository->qsoById(qsoId, &error).has_value());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(message().contains(QStringLiteral("deleted")));
}

void MainWindowUiTest::editStaysOpenWhenOutboxWriteFails()
{
    enterQso(QStringLiteral("W1AW"), QStringLiteral("599"), QStringLiteral("1"));
    const qint64 qsoId = table()->item(0, 0)->data(Qt::UserRole).toLongLong();
    QString error;
    const QVector<Contest> contests = m_repository->contests(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto selectedLog = std::find_if(
        contests.cbegin(), contests.cend(), [this](const Contest &contest) {
            return contest.id == m_window->findChild<QComboBox *>(
                QStringLiteral("contestSelector"))->currentData().toLongLong();
        });
    QVERIFY(selectedLog != contests.cend());
    QVERIFY2(m_repository->setSynchronizationEnabled(selectedLog->logUuid, true, &error),
             qPrintable(error));
    QSqlQuery trigger(QSqlDatabase::database(m_connectionName));
    QVERIFY2(trigger.exec(QStringLiteral(
        "CREATE TRIGGER fail_sync_outbox BEFORE INSERT ON sync_outbox "
        "BEGIN SELECT RAISE(ABORT, 'simulated outbox failure'); END")),
        qPrintable(trigger.lastError().text()));

    bool stayedOpenWithError = false;
    runWhenModalAppears([&stayedOpenWithError](QDialog *dialog) {
        auto *call = dialog->findChild<QLineEdit *>(QStringLiteral("editCallsign"));
        auto *errors = dialog->findChild<QLabel *>(QStringLiteral("editErrors"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(call && errors && buttons);
        call->setText(QStringLiteral("K2NEW"));
        buttons->button(QDialogButtonBox::Save)->click();
        QCoreApplication::processEvents();
        stayedOpenWithError = dialog->isVisible() && !errors->text().isEmpty();
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "editSelectedQso",
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QVERIFY(stayedOpenWithError);
    QCOMPARE(table()->item(0, 5)->text(), QStringLiteral("W1AW"));
    const auto unchanged = m_repository->qsoById(qsoId, &error);
    QVERIFY2(unchanged.has_value(), qPrintable(error));
    QCOMPARE(unchanged->callsign, QStringLiteral("W1AW"));
}

void MainWindowUiTest::serialFollowsStoredMaximum()
{
    enterQso(QStringLiteral("F5AA"), QStringLiteral("599"), QStringLiteral("1"));
    enterQso(QStringLiteral("F5BB"), QStringLiteral("599"), QStringLiteral("2"));
    QCOMPARE(sentEdit(QStringLiteral("serial"))->text(), QStringLiteral("003"));

    runWhenModalAppears([](QDialog *dialog) {
        auto *serial = dialog->findChild<QLineEdit *>(QStringLiteral("editSent_serial"));
        QVERIFY(serial != nullptr);
        serial->setText(QStringLiteral("10"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    QMetaObject::invokeMethod(m_window.get(), "editSelectedQso", Q_ARG(int, 1), Q_ARG(int, 0));
    QCOMPARE(sentEdit(QStringLiteral("serial"))->text(), QStringLiteral("011"));
}

void MainWindowUiTest::serverSerialReservationRequiresConnectionAndReservation()
{
    const QString yaml = QStringLiteral(R"(
schema_version: 1
id: ui-server-serial
name: UI server serial test
description: Server-allocated serial exchange for UI testing.
bands: [20m]
modes: [CW]
exchange:
  sent:
    - name: serial
      label: Serial
      type: serial
      generated: true
      width: 3
  received:
    - name: serial
      label: Serial
      type: integer
      min: 1
      max: 999
scoring:
  qso_points: 1
  serial_start: 1
  serial_server_required: true
dupe_key: [callsign, band, mode]
)");
    ContestDefinition definition;
    QString error;
    QVERIFY2(ContestDefinitionLoader().loadText(
                 yaml, QStringLiteral("ui-server-serial"), definition, &error),
             qPrintable(error));
    QVERIFY2(m_repository->saveContestDefinition(definition, yaml, &error),
             qPrintable(error));
    const qint64 logId = m_repository->createContestLog(
        definition, yaml, QStringLiteral("UI server serial test"), &error);
    QVERIFY2(logId > 0, qPrintable(error));

    m_window.reset();
    m_window = std::make_unique<MainWindow>(*m_repository);
    m_window->show();
    auto *selector = m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    QVERIFY(selector != nullptr);
    const int index = selector->findData(logId);
    QVERIFY(index >= 0);
    selector->setCurrentIndex(index);
    QCoreApplication::processEvents();

    auto *reserve = m_window->findChild<QPushButton *>(
        QStringLiteral("reserveSerialButtonV0"));
    auto *cancel = m_window->findChild<QPushButton *>(
        QStringLiteral("cancelSerialButtonV0"));
    QVERIFY(reserve != nullptr);
    QVERIFY(cancel != nullptr);
    QVERIFY(!cancel->isEnabled());
    QCOMPARE(sentEdit(QStringLiteral("serial"))->text(), QString());
    QTest::mouseClick(reserve, Qt::LeftButton);
    QVERIFY(message().contains(QStringLiteral("central server")));

    callEdit()->setText(QStringLiteral("W1AW"));
    QVERIFY(QMetaObject::invokeMethod(m_window.get(), "logQso",
                                      Qt::DirectConnection));
    QVERIFY(message().contains(QStringLiteral("central connection")));
    QCOMPARE(table()->rowCount(), 0);
    QCOMPARE(callEdit()->text(), QStringLiteral("W1AW"));
}

void MainWindowUiTest::newLogDialogRequiresCallsignAndContest()
{
    NewLogContestOption option;
    option.definition.id = QStringLiteral("zone-test");
    option.definition.name = QStringLiteral("Zone Test");
    option.definition.bands = {QStringLiteral("20m"), QStringLiteral("40m")};
    option.definition.modes = {QStringLiteral("CW"), QStringLiteral("SSB")};
    option.definition.sentExchange = {
        {QStringLiteral("report"), QStringLiteral("RST"), ExchangeFieldType::Rst,
         true, false, QStringLiteral("599")},
        {QStringLiteral("zone"), QStringLiteral("Zone"), ExchangeFieldType::Integer,
         true, false, {}, {}, 0, 1, 40}};
    option.source = QStringLiteral("test");
    Station station;
    NewLogDialog dialog({option}, station, {QStringLiteral("SP9ABC Zone Test 2026")});
    auto *callsign = dialog.findChild<QLineEdit *>(QStringLiteral("newLogCallsign"));
    auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("newLogName"));
    auto *ok = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    auto *startDate = dialog.findChild<QDateEdit *>(QStringLiteral("newLogStartDate"));
    QVERIFY(callsign != nullptr && name != nullptr && startDate != nullptr);
    QVERIFY(dialog.selectedOption() != nullptr);
    QVERIFY(!ok->isEnabled());
    QVERIFY(dialog.findChild<QLineEdit *>(QStringLiteral("newLogSent_report")) == nullptr);
    QVERIFY(dialog.findChild<QLineEdit *>(QStringLiteral("newLogSent_zone")) != nullptr);
    auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("newLogCategoryMode"));
    QCOMPARE(mode->itemText(0), QStringLiteral("MIXED"));
    auto *transmitter =
        dialog.findChild<QComboBox *>(QStringLiteral("newLogCategoryTransmitter"));
    QVERIFY(!transmitter->isEnabled());

    startDate->setDate(QDate(2026, 3, 1));
    callsign->setText(QStringLiteral("SP9ABC"));
    QVERIFY(ok->isEnabled());
    QCOMPARE(name->text(), QStringLiteral("SP9ABC Zone Test 2026 (2)"));
    dialog.findChild<QComboBox *>(QStringLiteral("newLogCategoryOperator"))
        ->setCurrentText(QStringLiteral("MULTI-OP"));
    QVERIFY(transmitter->isEnabled());
    dialog.findChild<QLineEdit *>(QStringLiteral("newLogSent_zone"))
        ->setText(QStringLiteral("15"));
    const Contest log = dialog.log();
    QCOMPARE(log.stationCallsign, QStringLiteral("SP9ABC"));
    QCOMPARE(log.properties.value(QStringLiteral("CATEGORY-OPERATOR")),
             QStringLiteral("MULTI-OP"));
    QCOMPARE(log.startsAtUtc->date(), QDate(2026, 3, 1));
    QCOMPARE(dialog.sentExchange().value(QStringLiteral("zone")), QStringLiteral("15"));
}

void MainWindowUiTest::importsAdifToCurrentOrNewLog_data()
{
    QTest::addColumn<bool>("newLog");
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("cabrillo");
    QTest::newRow("current") << false << QStringLiteral("success") << false;
    QTest::newRow("new") << true << QStringLiteral("success") << false;
    QTest::newRow("current-cabrillo") << false << QStringLiteral("success") << true;
    QTest::newRow("new-cabrillo") << true << QStringLiteral("success") << true;
    QTest::newRow("cancel-new") << true << QStringLiteral("cancel") << false;
    QTest::newRow("invalid-record") << false << QStringLiteral("invalid") << false;
    QTest::newRow("rollback-current") << false << QStringLiteral("database-error") << false;
    QTest::newRow("rollback-new") << true << QStringLiteral("database-error") << false;
}

void MainWindowUiTest::importsAdifToCurrentOrNewLog()
{
    QFETCH(bool, newLog);
    QFETCH(QString, scenario);
    QFETCH(bool, cabrillo);
    const QString path = m_dataDir.filePath(
        cabrillo ? QStringLiteral("import.log") : QStringLiteral("import.adi"));
    QByteArray first("<QSO_DATE:8>20261020<TIME_ON:6>101100<CALL:4>W1AW"
        "<BAND:3>40m<MODE:2>CW<RST_SENT:3>599<STX:1>7"
        "<RST_RCVD:3>599<SRX:2>12<EOR>\n");
    QByteArray second = first;
    second.replace("<CALL:4>W1AW", "<CALL:5>W2XYZ");
    second.replace("101100", "101200");
    if (scenario == QLatin1String("invalid")) {
        second.replace("<SRX:2>12", "<SRX:2>XX");
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray contents = QByteArray("<CONTEST_ID:13>serial-sprint<EOH>\n")
        + first + first + second;
    if (cabrillo) {
        const QByteArray row("QSO: 7025 CW 2026-10-20 1011 SP9TEST 599 007 W1AW 599 12\n");
        contents = QByteArray("START-OF-LOG: 3.0\nCONTEST: SERIAL-SPRINT\nCALLSIGN: SP9TEST\n")
            + row + row
            + QByteArray("QSO: 7025 CW 2026-10-20 1012 SP9TEST 599 008 W2XYZ 599 12\nEND-OF-LOG:\n");
    }
    QCOMPARE(file.write(contents), contents.size());
    file.close();
    const int originalLogs = m_repository->contests().size();
    auto *selector = m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    const qint64 originalId = selector->currentData().toLongLong();
    QString error;
    if (scenario == QLatin1String("database-error")) {
        QSqlQuery query(QSqlDatabase::database(m_connectionName));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_import BEFORE INSERT ON qsos "
            "WHEN NEW.callsign = 'W2XYZ' BEGIN SELECT RAISE(ABORT, 'import test failure'); END")));
    } else if (!newLog) {
        const auto contests = m_repository->contests();
        const auto current = std::find_if(contests.cbegin(), contests.cend(),
            [originalId](const Contest &contest) { return contest.id == originalId; });
        QVERIFY(current != contests.cend());
        QVERIFY(m_repository->setSynchronizationEnabled(current->logUuid, true, &error));
    }

    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    bool destinationSeen = false;
    bool propertiesSeen = false;
    bool confirmationSeen = false;
    QString failure;
    QTimer driver;
    driver.setInterval(20);
    connect(&driver, &QTimer::timeout, this, [&] {
        auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (modal == nullptr) {
            return;
        }
        if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
            if (!picker->property("importTestSelected").toBool()) {
                picker->setProperty("importTestSelected", true);
                picker->setDirectory(QFileInfo(path).absolutePath());
                QTimer::singleShot(150, picker, [picker, path] {
                    picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))->setText(path);
                    QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                });
            }
        } else if (modal->objectName() == QLatin1String("logImportDestinationDialog")) {
            destinationSeen = true;
            auto *choice = modal->findChild<QComboBox *>(QStringLiteral("logImportDestination"));
            choice->setCurrentIndex(choice->findData(newLog));
            modal->accept();
        } else if (auto *properties = qobject_cast<NewLogDialog *>(modal)) {
            propertiesSeen = true;
            properties->selectContest(QStringLiteral("serial-sprint"));
            properties->findChild<QLineEdit *>(QStringLiteral("newLogName"))
                ->setText(QStringLiteral("Imported test log"));
            properties->accept();
        } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            if (box->windowTitle() == QLatin1String("Confirm log import")) {
                confirmationSeen = true;
                box->button(scenario == QLatin1String("cancel") ? QMessageBox::No
                                                              : QMessageBox::Yes)->click();
            } else {
                failure = box->text();
                box->accept();
            }
        } else {
            failure = QStringLiteral("Unexpected modal dialog");
            modal->reject();
        }
    });
    driver.start();
    auto *action = m_window->findChild<QAction *>(QStringLiteral("importLogAction"));
    QVERIFY(action != nullptr);
    action->trigger();
    driver.stop();
    QVERIFY(destinationSeen);
    QCOMPARE(propertiesSeen, newLog);
    if (scenario == QLatin1String("success")) {
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(confirmationSeen);
        const qint64 targetId = selector->currentData().toLongLong();
        QCOMPARE(targetId != originalId, newLog);
        const auto qsos = m_repository->qsosForContest(targetId, 0, &error);
        QCOMPARE(qsos.size(), 2);
        QCOMPARE(table()->rowCount(), 2);
        QVERIFY(message().contains(QStringLiteral("skipped 1")));
        QVERIFY(qsos.first().timestampUtc.date() == QDate(2026, 10, 20));
        if (!newLog) {
            QCOMPARE(m_repository->syncOutbox(qsos.first().logUuid, false, &error).size(), 2);
        }
        // A second import must not add the same records again.
        newLog = false;
        failure.clear();
        driver.start();
        action->trigger();
        driver.stop();
        QVERIFY(failure.contains(QStringLiteral("already exist")));
        QCOMPARE(m_repository->qsosForContest(targetId).size(), 2);
    } else {
        QCOMPARE(selector->currentData().toLongLong(), originalId);
        QCOMPARE(m_repository->contests().size(), originalLogs);
        QVERIFY(m_repository->qsosForContest(originalId).isEmpty());
        if (scenario == QLatin1String("database-error")) {
            QVERIFY2(failure.contains(QStringLiteral("import test failure")), qPrintable(failure));
        } else if (scenario == QLatin1String("invalid")) {
            QVERIFY(!failure.isEmpty());
            QVERIFY(!confirmationSeen);
        } else {
            QVERIFY(failure.isEmpty());
            QVERIFY(confirmationSeen);
        }
    }
}

void MainWindowUiTest::newLogActionCreatesLogWithStationCallsign()
{
    auto *action = m_window->findChild<QAction *>(QStringLiteral("newLogAction"));
    QVERIFY(action != nullptr);
    bool dialogSeen = false;
    runWhenModalAppears([&dialogSeen](QDialog *dialog) {
        auto *newLog = qobject_cast<NewLogDialog *>(dialog);
        if (newLog == nullptr) {
            dialog->reject();
            return;
        }
        dialogSeen = true;
        auto *callsign = dialog->findChild<QLineEdit *>(QStringLiteral("newLogCallsign"));
        QCOMPARE(callsign->text(), QStringLiteral("SP9TEST"));
        auto *filter = dialog->findChild<QLineEdit *>(QStringLiteral("newLogContestFilter"));
        filter->setText(QStringLiteral("CQ WW"));
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("newLogContestList"));
        QVERIFY(list->count() >= 2);
        QVERIFY(newLog->selectContest(QStringLiteral("cqww-cw")));
        callsign->setText(QStringLiteral("SN9X"));
        dialog->findChild<QLineEdit *>(QStringLiteral("newLogSent_zone"))
            ->setText(QStringLiteral("15"));
        dialog->findChild<QComboBox *>(QStringLiteral("newLogCategoryPower"))
            ->setCurrentText(QStringLiteral("LOW"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    action->trigger();
    QVERIFY(dialogSeen);

    auto *selector = m_window->findChild<QComboBox *>(QStringLiteral("contestSelector"));
    QVERIFY2(selector->currentText().startsWith(QStringLiteral("SN9X CQ WW DX Contest")),
             qPrintable(selector->currentText()));
    QCOMPARE(sentEdit(QStringLiteral("zone"))->text(), QStringLiteral("15"));
    auto *topStatus = m_window->findChild<QLabel *>(QStringLiteral("topStatus"));
    QVERIFY2(topStatus->text().contains(QStringLiteral("SN9X")),
             qPrintable(topStatus->text()));
    const QVector<Contest> contests = m_repository->contests();
    const auto created = std::find_if(
        contests.cbegin(), contests.cend(), [](const Contest &contest) {
            return contest.stationCallsign == QStringLiteral("SN9X");
        });
    QVERIFY(created != contests.cend());
    QCOMPARE(created->properties.value(QStringLiteral("CATEGORY-POWER")),
             QStringLiteral("LOW"));
    QCOMPARE(created->properties.value(QStringLiteral("CATEGORY-MODE")),
             QStringLiteral("CW"));

    // Switching back to a log without its own callsign restores the station default.
    selector->setCurrentIndex(selector->findText(QStringLiteral("Serial Sprint"),
                                                 Qt::MatchContains));
    QVERIFY(topStatus->text().contains(QStringLiteral("SP9TEST")));
}

QTEST_MAIN(MainWindowUiTest)
#include "test_main_window_ui.moc"
