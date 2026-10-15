#include "logger/data/sqlite_repository.h"
#include "logger/ui/main_window.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void contestDefinitionConstrainsBandAndModeSelectors();
    void connectionMenuReconnectsAndDisconnects();
    void bandmapSortAndKeyboardNavigation();
    void ctrlWipeClearsCallAndReceivedExchange();
    void supportDataDownloadActionsExist();
    void enterLogsQsoAndRefreshesTable();
    void invalidExchangeIsRejectedWithMessage();
    void duplicateIsRejected();
    void editDialogValidatesAndSaves();
    void serialFollowsStoredMaximum();

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

QTEST_MAIN(MainWindowUiTest)
#include "test_main_window_ui.moc"
