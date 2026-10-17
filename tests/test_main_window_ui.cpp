#include "logger/data/sqlite_repository.h"
#include "logger/ui/main_window.h"
#include "logger/ui/new_log_dialog.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QSqlDatabase>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

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
    void radioConfigDialogPersistsHamlibAndCatTimingOptions();
    void cwKeyerDialogPersistsHamlibSerialControlOptions();
    void functionKeyMessagesAreManuallyConfigurable();
    void esmSendsCallAndLogsOnExchangeEnter();
    void autoCqRequiresRunAndStopsWhenEnteringCallsign();
    void frequencyChangeWithinBandSwitchesRunToSearchAndPounce();
    void so2vCwTransmissionFollowsRxFocus();
    void bandmapSortAndKeyboardNavigation();
    void bandmapFiltersSpotsByMode();
    void bandmapSpotLifetimeIsConfigurable();
    void clusterAnnouncementsShowOnlyLiveSpots();
    void ctrlWipeClearsCallAndReceivedExchange();
    void supportDataDownloadActionsExist();
    void enterLogsQsoAndRefreshesTable();
    void invalidExchangeIsRejectedWithMessage();
    void duplicateIsRejected();
    void editDialogValidatesAndSaves();
    void serialFollowsStoredMaximum();
    void newLogDialogRequiresCallsignAndContest();
    void newLogActionCreatesLogWithStationCallsign();

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
    QCoreApplication::setOrganizationName(QStringLiteral("ContestLoggerUiTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ContestLoggerUiTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
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

void MainWindowUiTest::cwKeyerDialogPersistsHamlibSerialControlOptions()
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
        auto *type = dialog->findChild<QComboBox *>(QStringLiteral("cwKeyerProvider"));
        auto *transport =
            dialog->findChild<QComboBox *>(QStringLiteral("cwHamlibTransport"));
        auto *rtsState =
            dialog->findChild<QComboBox *>(QStringLiteral("cwHamlibRtsState"));
        auto *dtrState =
            dialog->findChild<QComboBox *>(QStringLiteral("cwHamlibDtrState"));
        QVERIFY(type != nullptr);
        QVERIFY(transport != nullptr);
        QVERIFY(rtsState != nullptr);
        QVERIFY(dtrState != nullptr);
        type->setCurrentIndex(type->findData(static_cast<int>(CwKeyerType::Hamlib)));
        transport->setCurrentIndex(
            transport->findData(static_cast<int>(RadioTransport::Serial)));
        QVERIFY(rtsState->isVisible());
        QVERIFY(dtrState->isVisible());
        rtsState->setCurrentIndex(
            rtsState->findData(static_cast<int>(SerialControlState::Off)));
        dtrState->setCurrentIndex(
            dtrState->findData(static_cast<int>(SerialControlState::On)));
        type->setCurrentIndex(type->findData(static_cast<int>(CwKeyerType::Mock)));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    configureCw->trigger();
    QVERIFY(dialogSeen);

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("cw/hamlib/rtsState")).toInt(),
             static_cast<int>(SerialControlState::Off));
    QCOMPARE(settings.value(QStringLiteral("cw/hamlib/dtrState")).toInt(),
             static_cast<int>(SerialControlState::On));
    QCOMPARE(settings.value(QStringLiteral("cw/type")).toInt(),
             static_cast<int>(CwKeyerType::Mock));
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
