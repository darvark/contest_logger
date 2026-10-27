#include "logger/ui/main_window.h"

#include "logger/contest/contest_engine.h"
#include "logger/data/application_data.h"
#include "logger/data/log_importer.h"
#include "logger/network/station_presence.h"
#include "logger/radio/band_plan.h"
#include "logger/ui/new_log_dialog.h"
#include "logger/ui/operator_dialog.h"
#include "logger/ui/qtc_dialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDateTimeEdit>
#include <QDir>
#include <QFontMetrics>
#include <QFrame>
#include <QInputDialog>
#include <QListWidget>
#include <QLocale>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QCompleter>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHostInfo>
#include <QHostAddress>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPolygonF>
#include <QRegularExpression>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringListModel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTranslator>
#include <QTabWidget>
#include <QTimeZone>
#include <QToolBar>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace logger {
namespace {

bool isLogSetting(const QString &key)
{
    return key.startsWith(QLatin1String("station/"))
        || key.startsWith(QLatin1String("contest/"))
        || key.startsWith(QLatin1String("cw/functionKeys/"))
        || key.startsWith(QLatin1String("cw/esm/"))
        || key.startsWith(QLatin1String("operating/"));
}

QString supportAssetKey(const QString &type, const QString &path)
{
    return type == QLatin1String("support/cty")
        ? type : type + QLatin1Char('/')
            + QString::fromLatin1(QCryptographicHash::hash(
                QFileInfo(path).absoluteFilePath().toUtf8(),
                QCryptographicHash::Sha256).toHex());
}

class LogSettings final : public QSettings {
    Q_DECLARE_TR_FUNCTIONS(logger::MainWindow)
public:
    explicit LogSettings(SqliteRepository &repository) : m_repository(repository) {}

    QVariant value(const QString &key, const QVariant &fallback = {}) const
    {
        const QString fullKey = group().isEmpty() ? key : group() + QLatin1Char('/') + key;
        if (!m_repository.isStandaloneLog() || !isLogSetting(fullKey)) {
            return QSettings::value(key, fallback);
        }
        QString error;
        const auto values = m_repository.logSettings(&error);
        if (!error.isEmpty()) {
            QMessageBox::critical(nullptr, tr("Unable to read log settings"), error);
            return {};
        }
        return values.value(fullKey, fallback);
    }

    void setValue(const QString &key, const QVariant &value)
    {
        const QString fullKey = group().isEmpty() ? key : group() + QLatin1Char('/') + key;
        if (!m_repository.isStandaloneLog() || !isLogSetting(fullKey)) {
            QSettings::setValue(key, value);
            return;
        }
        QString error;
        if (!m_repository.setLogSetting(fullKey, value, &error)) {
            QMessageBox::critical(nullptr, tr("Unable to save log settings"), error);
        }
    }

private:
    SqliteRepository &m_repository;
};

void issueNetworkRequest(QObject *context, QNetworkAccessManager *manager,
                         const QNetworkRequest &request,
                         std::function<void(QNetworkReply *)> finished)
{
    QNetworkReply *reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, context,
                     [reply, finished = std::move(finished)]() mutable {
                         finished(reply);
                         reply->deleteLater();
                     });
}

QNetworkRequest supportDataRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("ContestLogger/0.1"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

std::optional<qint64> frequencyHzFromCallsignInput(const QString &input)
{
    const QString value = input.trimmed();
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9]+(?:[.,][0-9]{1,3})?$"));
    if (!pattern.match(value).hasMatch()) {
        return std::nullopt;
    }

    QString normalized = value;
    normalized.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double frequencyKhz = normalized.toDouble(&ok);
    if (!ok || !std::isfinite(frequencyKhz) || frequencyKhz <= 0.0
        || frequencyKhz > 450'000.0) {
        return std::nullopt;
    }

    const qint64 frequencyHz = qRound64(frequencyKhz * 1000.0);
    return bandForFrequency(frequencyHz).isEmpty()
        ? std::nullopt : std::optional<qint64>(frequencyHz);
}

void populateSerialControlSelector(QComboBox *selector, SerialControlState state)
{
    selector->addItem(QObject::tr("Unchanged"),
                      static_cast<int>(SerialControlState::Unchanged));
    selector->addItem(QObject::tr("On"),
                      static_cast<int>(SerialControlState::On));
    selector->addItem(QObject::tr("Off"),
                      static_cast<int>(SerialControlState::Off));
    const int index = selector->findData(static_cast<int>(state));
    selector->setCurrentIndex(index < 0 ? 0 : index);
}

QString networkReplyError(const QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        return reply->errorString();
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
        return QStringLiteral("HTTP %1").arg(status);
    }
    return {};
}

bool saveSupportDataFile(const QString &fileName, const QByteArray &contents,
                         QString &path, QString &error)
{
    const QString directory = applicationDataDirectory();
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        error = QStringLiteral("Unable to create the application data directory.");
        return false;
    }
    path = QDir(directory).filePath(fileName);
    if (QFileInfo::exists(path)) {
        QFile previousFile(path);
        if (!previousFile.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("%1: %2").arg(path, previousFile.errorString());
            return false;
        }
        const QByteArray previous = previousFile.readAll();
        if (previous != contents) {
            const QString historyDirectory = QDir(directory).filePath(
                QStringLiteral("support-data-versions/%1").arg(fileName));
            if (!QDir().mkpath(historyDirectory)) {
                error = QStringLiteral("Unable to create support-data version directory.");
                return false;
            }
            const QString digest = QString::fromLatin1(
                QCryptographicHash::hash(previous, QCryptographicHash::Sha256)
                    .toHex().left(12));
            const QString versionPath = QDir(historyDirectory).filePath(
                QStringLiteral("%1-%2.bak")
                    .arg(QDateTime::currentDateTimeUtc()
                             .toString(QStringLiteral("yyyyMMdd'T'hhmmsszzz'Z'")),
                         digest));
            QSaveFile versionFile(versionPath);
            if (!versionFile.open(QIODevice::WriteOnly)
                || versionFile.write(previous) != previous.size()
                || !versionFile.commit()) {
                error = QStringLiteral("%1: %2")
                            .arg(versionPath, versionFile.errorString());
                return false;
            }
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.write(contents) != contents.size()) {
        error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}

QString fieldTypeName(ExchangeFieldType type)
{
    switch (type) {
    case ExchangeFieldType::Rst:
        return QStringLiteral("RST");
    case ExchangeFieldType::Serial:
        return QStringLiteral("serial");
    case ExchangeFieldType::Integer:
        return QStringLiteral("integer");
    case ExchangeFieldType::Text:
        return QStringLiteral("text");
    case ExchangeFieldType::Grid:
        return QStringLiteral("grid");
    case ExchangeFieldType::Callsign:
        return QStringLiteral("callsign");
    case ExchangeFieldType::Enumeration:
        return QStringLiteral("enum");
    }
    return {};
}

QString exchangeText(const QMap<QString, QString> &fields)
{
    QStringList values;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        values.append(it.key() + QLatin1Char('=') + it.value());
    }
    return values.join(QLatin1Char(' '));
}


class RadioConfigDialog final : public QDialog {
public:
    explicit RadioConfigDialog(const RadioConfig &config, const QString &radioName,
                               bool autoConnect, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("%1 CAT configuration").arg(radioName));
        auto *layout = new QVBoxLayout(this);
        auto *tabs = new QTabWidget(this);
        auto *connectionTab = new QWidget(tabs);
        auto *connectionLayout = new QVBoxLayout(connectionTab);
        auto *form = new QFormLayout;
        m_backend = new QComboBox(this);
        m_backend->setObjectName(QStringLiteral("radioBackend"));
        m_backend->addItem(tr("Hamlib"), static_cast<int>(RadioBackendType::Hamlib));
        m_backend->addItem(tr("Mock radio (testing)"),
                           static_cast<int>(RadioBackendType::Mock));
        m_backend->setCurrentIndex(m_backend->findData(static_cast<int>(config.backend)));
        form->addRow(tr("Backend:"), m_backend);

        auto *hamlibGroup = new QGroupBox(tr("Hamlib radio"), connectionTab);
        auto *hamlibForm = new QFormLayout(hamlibGroup);
        m_model = new QComboBox(this);
        m_model->setObjectName(QStringLiteral("radioHamlibModel"));
        for (const RadioModel &model : availableRadioModels()) {
            m_model->addItem(model.name, model.id);
        }
        int modelIndex = m_model->findData(config.modelId);
        if (modelIndex < 0) {
            m_model->addItem(tr("Saved model (not in Hamlib catalog)"),
                             config.modelId);
            modelIndex = m_model->count() - 1;
        }
        m_model->setCurrentIndex(modelIndex);
        hamlibForm->addRow(tr("Radio model:"), m_model);

        m_transport = new QComboBox(this);
        m_transport->setObjectName(QStringLiteral("radioTransport"));
        m_transport->addItem(tr("Serial port"),
                             static_cast<int>(RadioTransport::Serial));
        m_transport->addItem(tr("TCP / rigctld"),
                             static_cast<int>(RadioTransport::Tcp));
        m_transport->setCurrentIndex(
            m_transport->findData(static_cast<int>(config.transport)));
        hamlibForm->addRow(tr("Transport:"), m_transport);
        connectionLayout->addLayout(form);
        connectionLayout->addWidget(hamlibGroup);

        m_serialGroup = new QGroupBox(tr("Serial CAT connection"), connectionTab);
        m_serialGroup->setObjectName(QStringLiteral("radioSerialGroup"));
        auto *serialForm = new QFormLayout(m_serialGroup);
        m_devicePath = new QLineEdit(config.devicePath, this);
        m_devicePath->setObjectName(QStringLiteral("radioSerialDevice"));
        serialForm->addRow(tr("Serial device:"), m_devicePath);
        m_baudRate = new QComboBox(this);
        m_baudRate->setObjectName(QStringLiteral("radioBaudRate"));
        for (int rate : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200}) {
            m_baudRate->addItem(QString::number(rate), rate);
        }
        int baudIndex = m_baudRate->findData(config.baudRate);
        m_baudRate->setCurrentIndex(baudIndex < 0 ? 3 : baudIndex);
        serialForm->addRow(tr("Baud rate:"), m_baudRate);
        m_dataBits = new QComboBox(this);
        m_dataBits->setObjectName(QStringLiteral("radioDataBits"));
        for (int bits : {5, 6, 7, 8}) {
            m_dataBits->addItem(QString::number(bits), bits);
        }
        m_dataBits->setCurrentIndex(m_dataBits->findData(config.dataBits));
        serialForm->addRow(tr("Data bits:"), m_dataBits);
        m_stopBits = new QComboBox(this);
        m_stopBits->setObjectName(QStringLiteral("radioStopBits"));
        for (int bits : {1, 2}) {
            m_stopBits->addItem(QString::number(bits), bits);
        }
        m_stopBits->setCurrentIndex(m_stopBits->findData(config.stopBits));
        serialForm->addRow(tr("Stop bits:"), m_stopBits);
        m_parity = new QComboBox(this);
        m_parity->setObjectName(QStringLiteral("radioParity"));
        m_parity->addItem(tr("None"), static_cast<int>(SerialParity::None));
        m_parity->addItem(tr("Odd"), static_cast<int>(SerialParity::Odd));
        m_parity->addItem(tr("Even"), static_cast<int>(SerialParity::Even));
        m_parity->addItem(tr("Mark"), static_cast<int>(SerialParity::Mark));
        m_parity->addItem(tr("Space"), static_cast<int>(SerialParity::Space));
        m_parity->setCurrentIndex(
            m_parity->findData(static_cast<int>(config.parity)));
        serialForm->addRow(tr("Parity:"), m_parity);
        m_handshake = new QComboBox(this);
        m_handshake->setObjectName(QStringLiteral("radioHandshake"));
        m_handshake->addItem(tr("None"), static_cast<int>(SerialHandshake::None));
        m_handshake->addItem(tr("Software (XON/XOFF)"),
                             static_cast<int>(SerialHandshake::XonXoff));
        m_handshake->addItem(tr("Hardware (RTS/CTS)"),
                             static_cast<int>(SerialHandshake::Hardware));
        m_handshake->setCurrentIndex(
            m_handshake->findData(static_cast<int>(config.handshake)));
        serialForm->addRow(tr("Handshake:"), m_handshake);
        m_rtsState = new QComboBox(this);
        m_rtsState->setObjectName(QStringLiteral("radioRtsState"));
        populateSerialControlSelector(m_rtsState, config.rtsState);
        serialForm->addRow(tr("RTS:"), m_rtsState);
        m_dtrState = new QComboBox(this);
        m_dtrState->setObjectName(QStringLiteral("radioDtrState"));
        populateSerialControlSelector(m_dtrState, config.dtrState);
        serialForm->addRow(tr("DTR:"), m_dtrState);
        connectionLayout->addWidget(m_serialGroup);

        m_tcpGroup = new QGroupBox(tr("TCP / rigctld connection"), connectionTab);
        m_tcpGroup->setObjectName(QStringLiteral("radioTcpGroup"));
        auto *tcpForm = new QFormLayout(m_tcpGroup);
        m_host = new QLineEdit(config.host, this);
        m_host->setObjectName(QStringLiteral("radioTcpHost"));
        tcpForm->addRow(tr("Host:"), m_host);
        m_port = new QSpinBox(this);
        m_port->setObjectName(QStringLiteral("radioTcpPort"));
        m_port->setRange(1, 65535);
        m_port->setValue(config.port);
        tcpForm->addRow(tr("TCP port:"), m_port);
        connectionLayout->addWidget(m_tcpGroup);

        m_autoConnect = new QCheckBox(tr("Connect automatically at startup"), this);
        m_autoConnect->setObjectName(QStringLiteral("radioAutoConnect"));
        m_autoConnect->setChecked(autoConnect);
        connectionLayout->addWidget(m_autoConnect);
        connectionLayout->addStretch();
        tabs->addTab(connectionTab, tr("Connection"));

        auto *advancedTab = new QWidget(tabs);
        auto *advancedLayout = new QVBoxLayout(advancedTab);
        auto *advancedGroup = new QGroupBox(tr("CAT communication"), advancedTab);
        auto *advancedForm = new QFormLayout(advancedGroup);
        m_pollInterval = new QComboBox(advancedGroup);
        m_pollInterval->setObjectName(QStringLiteral("radioPollInterval"));
        for (int interval : {250, 500, 1000, 2000, 5000}) {
            m_pollInterval->addItem(
                tr("%1 ms").arg(interval), interval);
        }
        int pollIndex = m_pollInterval->findData(config.pollIntervalMs);
        m_pollInterval->setCurrentIndex(pollIndex < 0 ? 2 : pollIndex);
        advancedForm->addRow(tr("Polling interval:"), m_pollInterval);
        m_timeout = new QSpinBox(advancedGroup);
        m_timeout->setObjectName(QStringLiteral("radioTimeout"));
        m_timeout->setRange(100, 30000);
        m_timeout->setSingleStep(100);
        m_timeout->setSuffix(tr(" ms"));
        m_timeout->setValue(config.timeoutMs);
        advancedForm->addRow(tr("Connection timeout:"), m_timeout);
        advancedLayout->addWidget(advancedGroup);
        advancedLayout->addStretch();
        tabs->addTab(advancedTab, tr("Advanced"));
        layout->addWidget(tabs);

        updateTransportFields();
        connect(m_transport, &QComboBox::currentIndexChanged,
                this, [this] { updateTransportFields(); });

        m_result = new QLabel(tr("Use Test to check the settings without keeping the "
                                 "connection open."), this);
        m_result->setObjectName(QStringLiteral("radioTestResult"));
        m_result->setWordWrap(true);
        m_result->setMinimumWidth(380);
        layout->addWidget(m_result);

        auto *buttons = new QDialogButtonBox(this);
        auto *testButton = buttons->addButton(tr("Test"), QDialogButtonBox::ActionRole);
        testButton->setObjectName(QStringLiteral("radioTestButton"));
        auto *connectButton = buttons->addButton(tr("Save && connect"),
                                                 QDialogButtonBox::AcceptRole);
        connectButton->setObjectName(QStringLiteral("radioConnectButton"));
        auto *disconnectButton = buttons->addButton(tr("Disconnect"),
                                                    QDialogButtonBox::DestructiveRole);
        disconnectButton->setObjectName(QStringLiteral("radioDisconnectButton"));
        auto *saveButton = buttons->addButton(tr("Save only"), QDialogButtonBox::ApplyRole);
        saveButton->setObjectName(QStringLiteral("radioSaveButton"));
        buttons->addButton(QDialogButtonBox::Cancel);
        connect(testButton, &QPushButton::clicked, this, &RadioConfigDialog::runTest);
        connect(connectButton, &QPushButton::clicked, this, [this] {
            m_action = Action::Connect;
            accept();
        });
        connect(disconnectButton, &QPushButton::clicked, this, [this] {
            m_action = Action::Disconnect;
            accept();
        });
        connect(saveButton, &QPushButton::clicked, this, [this] {
            m_action = Action::Save;
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    enum class Action { Save, Connect, Disconnect };

    Action action() const { return m_action; }
    bool autoConnect() const { return m_autoConnect->isChecked(); }

    void runTest()
    {
        m_result->setStyleSheet(QString());
        m_result->setText(tr("Testing…"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QApplication::processEvents();
        const RadioConfig radioConfig = config();
        const RadioProbeResult result =
            probeRadio(radioConfig, radioConfig.timeoutMs);
        QApplication::restoreOverrideCursor();
        if (!result.ok) {
            m_result->setStyleSheet(QStringLiteral("color: #c62828; font-weight: bold;"));
            m_result->setText(tr("Test failed: %1").arg(result.message));
            return;
        }
        m_result->setStyleSheet(QStringLiteral("color: #1b5e20; font-weight: bold;"));
        QString capability = result.dualReceive
            ? tr("dual receive (SO2V available)")
            : result.dualVfo ? tr("two VFOs, single receiver (SO2V without dual watch)")
                             : tr("single VFO");
        m_result->setText(tr("OK — %1 kHz %2, %3 VFO(s), %4. %5")
                              .arg(QString::number(result.frequencyHz / 1000.0, 'f', 1),
                                   result.mode)
                              .arg(result.vfoCount)
                              .arg(capability, result.message));
    }

    RadioConfig config() const
    {
        RadioConfig result;
        result.backend = static_cast<RadioBackendType>(m_backend->currentData().toInt());
        result.transport =
            static_cast<RadioTransport>(m_transport->currentData().toInt());
        result.modelId = m_model->currentData().toInt();
        result.devicePath = m_devicePath->text().trimmed();
        result.host = m_host->text().trimmed();
        result.port = m_port->value();
        result.baudRate = m_baudRate->currentData().toInt();
        result.dataBits = m_dataBits->currentData().toInt();
        result.stopBits = m_stopBits->currentData().toInt();
        result.parity = static_cast<SerialParity>(m_parity->currentData().toInt());
        result.handshake =
            static_cast<SerialHandshake>(m_handshake->currentData().toInt());
        result.rtsState =
            static_cast<SerialControlState>(m_rtsState->currentData().toInt());
        result.dtrState =
            static_cast<SerialControlState>(m_dtrState->currentData().toInt());
        result.timeoutMs = m_timeout->value();
        result.pollIntervalMs = m_pollInterval->currentData().toInt();
        return result;
    }

private:
    void updateTransportFields()
    {
        const bool serial =
            m_transport->currentData().toInt()
            == static_cast<int>(RadioTransport::Serial);
        m_serialGroup->setVisible(serial);
        m_tcpGroup->setVisible(!serial);
    }

    QComboBox *m_backend = nullptr;
    QComboBox *m_model = nullptr;
    QComboBox *m_transport = nullptr;
    QLineEdit *m_devicePath = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QComboBox *m_baudRate = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_handshake = nullptr;
    QComboBox *m_rtsState = nullptr;
    QComboBox *m_dtrState = nullptr;
    QGroupBox *m_serialGroup = nullptr;
    QGroupBox *m_tcpGroup = nullptr;
    QComboBox *m_pollInterval = nullptr;
    QSpinBox *m_timeout = nullptr;
    QCheckBox *m_autoConnect = nullptr;
    QLabel *m_result = nullptr;
    Action m_action = Action::Save;
};

class SyncConnectionDialog final : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(logger::SyncConnectionDialog)
public:
    SyncConnectionDialog(const QVector<Contest> &contests,
                         const QString &savedHost, int savedPort,
                         const QString &savedLogUuid, bool autoStart, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Network client"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_host = new QLineEdit(savedHost, this);
        m_host->setObjectName(QStringLiteral("syncServerHost"));
        m_host->setPlaceholderText(tr("Server IPv4 or IPv6 address"));
        form->addRow(tr("Server IP address:"), m_host);

        m_port = new QSpinBox(this);
        m_port->setObjectName(QStringLiteral("syncServerPort"));
        m_port->setRange(1, 65535);
        m_port->setValue(savedPort > 0 ? savedPort : 7443);
        form->addRow(tr("Port:"), m_port);

        m_log = new QComboBox(this);
        m_log->setObjectName(QStringLiteral("syncLogSelector"));
        for (const Contest &contest : contests) {
            m_log->addItem(QStringLiteral("%1 — %2")
                               .arg(contest.name, contest.stationCallsign),
                           contest.logUuid);
        }
        const int savedLogIndex = m_log->findData(savedLogUuid);
        if (savedLogIndex >= 0) {
            m_log->setCurrentIndex(savedLogIndex);
        } else if (contests.isEmpty()) {
            m_log->setCurrentIndex(m_log->count() - 1);
        }
        form->addRow(tr("Log to synchronize:"), m_log);
        m_log->setEnabled(false);

        m_autoStart = new QCheckBox(
            tr("Connect automatically at application startup"), this);
        m_autoStart->setObjectName(QStringLiteral("autoStartSyncClientCheck"));
        m_autoStart->setChecked(autoStart);
        form->addRow(QString(), m_autoStart);
        layout->addLayout(form);

        auto *notice = new QLabel(
            tr("The currently open log must have the same contest definition "
               "as the server log. QSOs are sent automatically over plain TCP."),
            this);
        notice->setWordWrap(true);
        layout->addWidget(notice);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
        connect(buttons, &QDialogButtonBox::accepted,
                this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    QString host() const { return m_host->text().trimmed(); }
    quint16 port() const { return static_cast<quint16>(m_port->value()); }
    QString logUuid() const
    {
        return joinRemote() ? QString() : m_log->currentData().toString();
    }
    bool joinRemote() const
    {
        return m_log->currentData().toString() == QStringLiteral("@remote");
    }
    bool createRemote() const
    {
        return false;
    }
    bool autoStart() const { return m_autoStart->isChecked(); }

private:
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QComboBox *m_log = nullptr;
    QCheckBox *m_autoStart = nullptr;
};

class DxClusterConfigDialog final : public QDialog {
public:
    explicit DxClusterConfigDialog(const DxClusterConfig &config,
                                   int spotLifetimeMinutes, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("DX cluster connection"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_server = new QComboBox(this);
        m_server->setObjectName(QStringLiteral("dxClusterServer"));
        m_server->addItem(tr("Custom server"));
        struct ClusterServer {
            QString label;
            QString host;
            int port;
        };
        const QList<ClusterServer> servers{
            {QStringLiteral("DA0BCC-7"), QStringLiteral("dx.da0bcc.de"), 7300},
            {QStringLiteral("F4BPO-1"), QStringLiteral("cluster.f4bpo.com"), 7300},
            {QStringLiteral("EA2CW-1"), QStringLiteral("cluster.gautxori.com"), 7300},
            {QStringLiteral("EA4URE-2"), QStringLiteral("ea4ure.com"), 7300},
            {QStringLiteral("PI1LAP-1"), QStringLiteral("dx.packet-radio.nl"), 7300},
            {QStringLiteral("S50CLX"), QStringLiteral("s50clx.si"), 41112},
            {QStringLiteral("OE3XHQ-1"), QStringLiteral("dxcluster.oevsv.at"), 7300},
            {QStringLiteral("OH1AF"), QStringLiteral("oh1af.fi"), 8000},
            {QStringLiteral("VE3CGR-5"), QStringLiteral("ve3cgr.dyndns.org"), 7300},
            {QStringLiteral("N0OJ-7"), QStringLiteral("cluster.n0oj.net"), 7300},
            {QStringLiteral("DA0BCC-7"), QStringLiteral("dx.da0bcc.de"), 7300},
            {QStringLiteral("EA4URE-2"), QStringLiteral("ea4ure.com"), 7300},
            {QStringLiteral("GB7DJK"), QStringLiteral("gb7djk.dxcluster.net"), 7300},
            {QStringLiteral("MX0ONL-4"), QStringLiteral("dx.onlineradioclub.org"), 7300},
            {QStringLiteral("GB7DXG"), QStringLiteral("gb7dxg.lebs.org.uk"), 7373},
            {QStringLiteral("GB7DXG-1"), QStringLiteral("gb7dxg-1.lebs.org.uk"), 7300},
            {QStringLiteral("HA5DXC"), QStringLiteral("ha5dxc.homelinux.net"), 8000},
            {QStringLiteral("K1AX-11"), QStringLiteral("dxdata.io"), 7300},
            {QStringLiteral("ON0LLV-5"), QStringLiteral("on0llv-5.on6ll.be"), 7300},
            {QStringLiteral("ON0LLV-6"), QStringLiteral("on0llv-6.on6ll.be"), 7373},
            {QStringLiteral("ON0NOL-9"), QStringLiteral("nolcluster.on8ar.eu"), 7300},
            {QStringLiteral("ON4KST-2"), QStringLiteral("www.on4kst.info"), 7300},
            {QStringLiteral("ON4LL-2"), QStringLiteral("on4ll.ddns.net"), 7300},
            {QStringLiteral("PI4CC"), QStringLiteral("dxc.pi4cc.nl"), 8000},
            {QStringLiteral("SM7IUN-1"), QStringLiteral("dxc.sm7iun.se"), 7300},
            {QStringLiteral("SR4DXCW"), QStringLiteral("dx.jestok.com"), 7300},
            {QStringLiteral("VE3EID-1"), QStringLiteral("cluster.ve3eid.com"), 9000}

        };
        for (const ClusterServer &server : servers) {
            const int index = m_server->count();
            m_server->addItem(QStringLiteral("%1 (%2:%3)")
                                  .arg(server.label, server.host)
                                  .arg(server.port),
                              server.host);
            m_server->setItemData(index, server.port, Qt::UserRole + 1);
        }
        m_host = new QLineEdit(config.host, this);
        m_host->setObjectName(QStringLiteral("dxClusterHost"));
        m_host->setPlaceholderText(QStringLiteral("cluster.example.net"));
        m_port = new QSpinBox(this);
        m_port->setObjectName(QStringLiteral("dxClusterPort"));
        m_port->setRange(1, 65535);
        m_port->setValue(config.port);
        m_callsign = new QLineEdit(config.callsign, this);
        m_spotLifetime = new QSpinBox(this);
        m_spotLifetime->setObjectName(QStringLiteral("dxSpotLifetimeMinutes"));
        m_spotLifetime->setRange(1, 1440);
        m_spotLifetime->setSuffix(tr(" min"));
        m_spotLifetime->setValue(spotLifetimeMinutes);
        form->addRow(tr("Server:"), m_server);
        form->addRow(tr("Host:"), m_host);
        form->addRow(tr("Telnet port:"), m_port);
        form->addRow(tr("Station callsign:"), m_callsign);
        form->addRow(tr("Spot lifetime:"), m_spotLifetime);
        layout->addLayout(form);
        connect(m_server, &QComboBox::currentIndexChanged, this,
                [this](int index) {
                    const bool custom = index == 0;
                    m_host->setEnabled(custom);
                    m_port->setEnabled(custom);
                    if (!custom) {
                        m_host->setText(m_server->itemData(index).toString());
                        m_port->setValue(
                            m_server->itemData(index, Qt::UserRole + 1).toInt());
                    }
                });
        for (int index = 1; index < m_server->count(); ++index) {
            if (m_server->itemData(index).toString().compare(
                    config.host, Qt::CaseInsensitive) == 0
                && m_server->itemData(index, Qt::UserRole + 1).toInt() == config.port) {
                m_server->setCurrentIndex(index);
                break;
            }
        }
        if (m_server->currentIndex() != 0) {
            m_host->setEnabled(false);
            m_port->setEnabled(false);
        }
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    DxClusterConfig config() const
    {
        return {m_host->text().trimmed(), m_port->value(),
                m_callsign->text().trimmed().toUpper()};
    }

    int spotLifetimeMinutes() const { return m_spotLifetime->value(); }

private:
    QComboBox *m_server = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_callsign = nullptr;
    QSpinBox *m_spotLifetime = nullptr;
};

class CwKeyerConfigDialog final : public QDialog {
public:
    explicit CwKeyerConfigDialog(const CwKeyerConfig &config, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("CW keyer configuration"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_device = new QLineEdit(config.serialDevicePath, this);
        m_device->setObjectName(QStringLiteral("cwSerialDevice"));
        m_device->setPlaceholderText(QStringLiteral("/dev/ttyUSB0"));
        form->addRow(tr("Serial device:"), m_device);
        m_line = new QComboBox(this);
        m_line->setObjectName(QStringLiteral("cwSerialLine"));
        m_line->addItem(tr("DTR"), static_cast<int>(CwKeyerSerialLine::Dtr));
        m_line->addItem(tr("RTS"), static_cast<int>(CwKeyerSerialLine::Rts));
        m_line->setCurrentIndex(m_line->findData(static_cast<int>(config.serialLine)));
        form->addRow(tr("CW output:"), m_line);
        layout->addLayout(form);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    CwKeyerConfig config() const
    {
        CwKeyerConfig result;
        result.type = CwKeyerType::Serial;
        result.serialDevicePath = m_device->text().trimmed();
        result.serialLine =
            static_cast<CwKeyerSerialLine>(m_line->currentData().toInt());
        return result;
    }

private:
    QLineEdit *m_device = nullptr;
    QComboBox *m_line = nullptr;
};

// Day/night map (equirectangular); points are (longitude, latitude) in degrees.
class GreylineWidget final : public QWidget {
public:
    using PointProvider = std::function<std::optional<QPointF>()>;

    GreylineWidget(PointProvider home, PointProvider target, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_home(std::move(home))
        , m_target(std::move(target))
    {
        setMinimumSize(200, 100);
        loadLand();
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
        timer->start(30'000);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(QStringLiteral("#40E0D0")));
        const double width = qMin<double>(this->width(), this->height() * 2.0);
        const QRectF map((this->width() - width) / 2.0,
                         (this->height() - width / 2.0) / 2.0, width, width / 2.0);
        const auto project = [&map](double lon, double lat) {
            return QPointF(map.left() + (lon + 180.0) / 360.0 * map.width(),
                           map.top() + (90.0 - lat) / 180.0 * map.height());
        };
        p.fillRect(map, QColor(QStringLiteral("#BFEFFF")));
        p.save();
        p.setClipRect(map);
        p.setPen(QPen(QColor(QStringLiteral("#3A6B35")), 1));
        p.setBrush(QColor(QStringLiteral("#8FCB7A")));
        for (const QPolygonF &ring : std::as_const(m_land)) {
            QPolygonF projected;
            projected.reserve(ring.size());
            for (const QPointF &point : ring) {
                projected << project(point.x(), point.y());
            }
            p.drawPolygon(projected);
        }
        p.restore();

        // Subsolar point from the UTC time (low-precision solar position).
        constexpr double kRad = M_PI / 180.0;
        const double days = QDateTime::currentMSecsSinceEpoch() / 86'400'000.0 - 10'957.5;
        const double meanLon = 280.460 + 0.9856474 * days;
        const double anomaly = (357.528 + 0.9856003 * days) * kRad;
        const double eclipticLon =
            (meanLon + 1.915 * std::sin(anomaly) + 0.020 * std::sin(2 * anomaly)) * kRad;
        const double obliquity = (23.439 - 0.0000004 * days) * kRad;
        const double declination = std::asin(std::sin(obliquity) * std::sin(eclipticLon));
        const double rightAscension =
            std::atan2(std::cos(obliquity) * std::sin(eclipticLon), std::cos(eclipticLon));
        const double gmst = std::fmod(280.46061837 + 360.98564736629 * days, 360.0);
        double sunLon = std::fmod(rightAscension / kRad - gmst, 360.0);
        if (sunLon < -180.0) {
            sunLon += 360.0;
        } else if (sunLon > 180.0) {
            sunLon -= 360.0;
        }

        const double safeDeclination =
            std::abs(declination) < 0.0002 ? 0.0002 : declination;
        QPolygonF night;
        for (int lon = -180; lon <= 180; ++lon) {
            const double hourAngle = (lon - sunLon) * kRad;
            const double lat =
                std::atan(-std::cos(hourAngle) / std::tan(safeDeclination)) / kRad;
            night << project(lon, lat);
        }
        const double capLat = safeDeclination > 0 ? -90.0 : 90.0;
        night << project(180.0, capLat) << project(-180.0, capLat);
        p.save();
        p.setClipRect(map);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 40, 130));
        p.drawPolygon(night);
        p.restore();

        p.setPen(QPen(QColor(0, 0, 0, 70), 1));
        for (int lon = -180; lon <= 180; lon += 30) {
            p.drawLine(project(lon, 90), project(lon, -90));
        }
        for (int lat = -90; lat <= 90; lat += 30) {
            p.drawLine(project(-180, lat), project(180, lat));
        }
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(map);

        const QPointF sun = project(sunLon, declination / kRad);
        p.setPen(QPen(QColor(QStringLiteral("#B8860B")), 1));
        p.setBrush(QColor(QStringLiteral("#FFD700")));
        p.drawEllipse(sun, 6, 6);

        const auto marker = [&](const std::optional<QPointF> &point, const QColor &color,
                                const QString &label) {
            if (!point.has_value()) {
                return;
            }
            const QPointF position = project(point->x(), point->y());
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(color);
            p.drawEllipse(position, 4, 4);
            p.drawText(position + QPointF(7, -5), label);
        };
        const std::optional<QPointF> home = m_home();
        const std::optional<QPointF> target = m_target();
        if (home.has_value() && target.has_value()) {
            p.setPen(QPen(QColor(QStringLiteral("#CE0000")), 1, Qt::DashLine));
            p.drawLine(project(home->x(), home->y()), project(target->x(), target->y()));
        }
        marker(home, QColor(QStringLiteral("#CE0000")), tr("QTH"));
        marker(target, QColor(QStringLiteral("#0000CD")), tr("DX"));

        p.setPen(Qt::black);
        p.drawText(map.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignBottom,
                   QDateTime::currentDateTimeUtc().toString(
                       QStringLiteral("yyyy-MM-dd HH:mm 'UTC'")));
    }

private:
    void loadLand()
    {
        QFile file(QStringLiteral(":/map/world_land.txt"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return;
        }
        while (!file.atEnd()) {
            const QString line = QString::fromLatin1(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            QPolygonF ring;
            for (const QString &pair : line.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const QStringList parts = pair.split(QLatin1Char(','));
                if (parts.size() == 2) {
                    ring << QPointF(parts.at(0).toDouble(), parts.at(1).toDouble());
                }
            }
            if (ring.size() > 2) {
                m_land.append(ring);
            }
        }
    }

    QList<QPolygonF> m_land;
    PointProvider m_home;
    PointProvider m_target;
};

QStringList defaultBands()
{
    return {QStringLiteral("160m"), QStringLiteral("80m"), QStringLiteral("60m"),
            QStringLiteral("40m"), QStringLiteral("30m"), QStringLiteral("20m"),
            QStringLiteral("17m"), QStringLiteral("15m"), QStringLiteral("12m"),
            QStringLiteral("10m"), QStringLiteral("6m"), QStringLiteral("2m")};
}

QStringList defaultModes()
{
    return {QStringLiteral("CW"), QStringLiteral("SSB"), QStringLiteral("USB"),
            QStringLiteral("LSB"), QStringLiteral("RTTY"), QStringLiteral("DATA"),
            QStringLiteral("FM")};
}

class RatePaceChart final : public QWidget {
public:
    explicit RatePaceChart(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("ratePaceChart"));
        setMinimumHeight(64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setSamples(const QVector<int> &samples)
    {
        m_samples = samples;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());
        const QRect area = rect().adjusted(4, 3, -4, -15);
        if (area.width() <= 0 || area.height() <= 0 || m_samples.isEmpty()) {
            return;
        }
        const int maxValue = qMax(1, *std::max_element(m_samples.cbegin(),
                                                      m_samples.cend()));
        const qreal slotWidth = static_cast<qreal>(area.width()) / m_samples.size();
        painter.setPen(QPen(palette().mid().color(), 1));
        painter.drawLine(area.bottomLeft(), area.bottomRight());
        for (qsizetype index = 0; index < m_samples.size(); ++index) {
            const qreal ratio = static_cast<qreal>(m_samples.at(index)) / maxValue;
            const int height = qRound(ratio * (area.height() - 3));
            const QRectF bar(area.left() + index * slotWidth + 1,
                             area.bottom() - height, qMax<qreal>(1, slotWidth - 2),
                             height);
            painter.fillRect(bar, QColor(QStringLiteral("#20B2AA")));
        }
        painter.setPen(palette().text().color());
        painter.drawText(rect().adjusted(4, 0, -4, 0),
                         Qt::AlignBottom | Qt::AlignLeft,
                         tr("QSO per 5 min — last hour"));
    }

private:
    QVector<int> m_samples;
};

void selectAdjacentBandmapRow(QTableWidget *bandmap, int direction)
{
    if (bandmap == nullptr || bandmap->rowCount() == 0 || direction == 0) {
        return;
    }
    const int rowCount = bandmap->rowCount();
    const int currentRow = bandmap->currentRow();
    const int startRow = currentRow < 0 ? (direction > 0 ? -1 : 0) : currentRow;
    const int nextRow = (startRow + direction + rowCount) % rowCount;
    bandmap->setCurrentCell(nextRow, 0);
    if (QTableWidgetItem *item = bandmap->item(nextRow, 0)) {
        bandmap->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
    bandmap->setFocus(Qt::ShortcutFocusReason);
}

QMap<int, qint64> connectedVfoFrequencies(const RadioSnapshot &snapshot)
{
    QMap<int, qint64> frequencies;
    if (snapshot.state != ConnectionState::Connected) {
        return frequencies;
    }
    if (snapshot.vfos.isEmpty()) {
        if (snapshot.frequencyHz > 0) {
            frequencies.insert(snapshot.focusVfo, snapshot.frequencyHz);
        }
    } else {
        for (const Vfo &vfo : snapshot.vfos) {
            if (vfo.frequencyHz > 0) {
                frequencies.insert(vfo.number, vfo.frequencyHz);
            }
        }
    }
    return frequencies;
}

CwKeyerConfig cwKeyerConfigFromSettings(const QSettings &settings, int radioIndex,
                                       RadioConfig radio)
{
    const int previousRadioIndex =
        settings.value(QStringLiteral("cw/radioIndex"), radioIndex).toInt();
    CwKeyerConfig config;
    config.type = static_cast<CwKeyerType>(
        settings.value(QStringLiteral("cw/type"),
                       static_cast<int>(CwKeyerType::Serial)).toInt());
    config.host = settings.value(QStringLiteral("cw/host"),
                                 QStringLiteral("127.0.0.1")).toString();
    config.port = settings.value(QStringLiteral("cw/port"), 6789).toInt();
    config.wpm = settings.value(QStringLiteral("cw/wpm"), 25).toInt();
    config.radio = std::move(radio);
    config.serialDevicePath = settings.value(
        QStringLiteral("cw/serial/devicePath"),
        settings.value(QStringLiteral("cw/hamlib/devicePath"),
                       config.radio.devicePath)).toString();
    config.serialLine = static_cast<CwKeyerSerialLine>(
        settings.value(QStringLiteral("cw/serial/line"),
                       static_cast<int>(CwKeyerSerialLine::Dtr)).toInt());
    if (previousRadioIndex == radioIndex) {
        config.radio.modelId = settings.value(QStringLiteral("cw/hamlib/modelId"),
                                              config.radio.modelId).toInt();
        config.radio.transport = static_cast<RadioTransport>(
            settings.value(QStringLiteral("cw/hamlib/transport"),
                           static_cast<int>(config.radio.transport)).toInt());
        config.radio.devicePath = settings.value(
            QStringLiteral("cw/hamlib/devicePath"), config.radio.devicePath).toString();
        config.radio.baudRate = settings.value(
            QStringLiteral("cw/hamlib/baudRate"), config.radio.baudRate).toInt();
        config.radio.rtsState = static_cast<SerialControlState>(
            settings.value(QStringLiteral("cw/hamlib/rtsState"),
                          static_cast<int>(config.radio.rtsState)).toInt());
        config.radio.dtrState = static_cast<SerialControlState>(
            settings.value(QStringLiteral("cw/hamlib/dtrState"),
                          static_cast<int>(config.radio.dtrState)).toInt());
        config.radio.host = settings.value(
            QStringLiteral("cw/hamlib/host"), config.radio.host).toString();
        config.radio.port = settings.value(
            QStringLiteral("cw/hamlib/port"), config.radio.port).toInt();
    }
    return config;
}

QFont monoFont(int pointSize, bool bold)
{
    QFont font;
    font.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Courier New"),
                      QStringLiteral("DejaVu Sans Mono"),
                      QStringLiteral("Liberation Mono")});
    font.setStyleHint(QFont::TypeWriter);
    font.setFixedPitch(true);
    font.setPointSize(pointSize);
    font.setBold(bold);
    return font;
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void installUppercase(QLineEdit *edit)
{
    QObject::connect(edit, &QLineEdit::textEdited, edit, [edit](const QString &text) {
        const QString upper = text.toUpper();
        if (upper != text) {
            const int cursor = edit->cursorPosition();
            edit->setText(upper);
            edit->setCursorPosition(cursor);
        }
    });
}

// Pane background frame used as the content of every DXLog-like tool window.
QFrame *createPane(const QString &objectName)
{
    auto *pane = new QFrame;
    pane->setObjectName(objectName);
    pane->setProperty("pane", true);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    return pane;
}

// Small "label above widget" column used on the QSO entry line.
QWidget *fieldColumn(const QString &label, QWidget *widget, QWidget *parent)
{
    auto *column = new QWidget(parent);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    auto *caption = new QLabel(label, column);
    caption->setObjectName(QStringLiteral("fieldCaption"));
    layout->addWidget(caption);
    layout->addWidget(widget);
    return column;
}

struct FunctionKeyDefault {
    const char *label;
    const char *text;
};

constexpr FunctionKeyDefault kFunctionKeyDefaults[8] = {
    {"CQ", "CQ TEST {MYCALL} {MYCALL} TEST"},
    {"Exch", "{SENT}"},
    {"TU", "TU {MYCALL}"},
    {"My call", "{MYCALL}"},
    {"His call", "{CALL}"},
    {"QSO B4", "QSO B4 TU"},
    {"?", "?"},
    {"AGN", "AGN"},
};

QString functionKeyLabel(SqliteRepository &repository, int key)
{
    LogSettings settings(repository);
    return settings.value(QStringLiteral("cw/functionKeys/F%1/label").arg(key + 1),
                          QString::fromLatin1(kFunctionKeyDefaults[key].label))
        .toString();
}

QString functionKeyText(SqliteRepository &repository, int key)
{
    LogSettings settings(repository);
    return settings.value(QStringLiteral("cw/functionKeys/F%1/text").arg(key + 1),
                          QString::fromLatin1(kFunctionKeyDefaults[key].text))
        .toString();
}

QPalette paletteForColorScheme(const QString &scheme)
{
    QPalette palette = QApplication::style()->standardPalette();
    if (scheme != QLatin1String("monokai")) {
        return palette;
    }

    palette.setColor(QPalette::Window, QColor(QStringLiteral("#272822")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#F8F8F2")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#1E1F1C")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#35362F")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#3E3D32")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#F8F8F2")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#F8F8F2")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#3E3D32")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#F8F8F2")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#F92672")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#66D9EF")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#49483E")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#F8F8F2")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#75715E")));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     QColor(QStringLiteral("#75715E")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     QColor(QStringLiteral("#75715E")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     QColor(QStringLiteral("#75715E")));
    return palette;
}

QString dxlogStyleSheet(bool monokai = false)
{
    if (monokai) {
        return QStringLiteral(R"(
QMainWindow { background: #272822; color: #F8F8F2; }
QMainWindow::separator { background: #49483E; width: 4px; height: 4px; }
QMenuBar { background: #1E1F1C; color: #F8F8F2; }
QMenuBar::item { background: transparent; color: #F8F8F2; padding: 4px 8px; }
QMenuBar::item:selected { background: #49483E; color: #F8F8F2; }
QMenu { background: #272822; color: #F8F8F2; border: 1px solid #49483E; }
QMenu::item { background: transparent; color: #F8F8F2; padding: 4px 24px 4px 8px; }
QMenu::item:selected { background: #49483E; color: #F8F8F2; }
QMenu::item:disabled { color: #75715E; }
QMenu::separator { height: 1px; background: #49483E; margin: 4px 8px; }
QToolBar { background: #1E1F1C; color: #F8F8F2; border: none; spacing: 4px; padding: 2px; }
QToolBar QCheckBox, QToolBar QLabel { color: #F8F8F2; }
QComboBox, QLineEdit, QSpinBox, QDateEdit, QPlainTextEdit {
    color: #F8F8F2; background: #1E1F1C; border: 1px solid #49483E;
}
QComboBox QAbstractItemView {
    color: #F8F8F2; background: #272822;
    selection-background-color: #49483E; selection-color: #F8F8F2;
}
QComboBox QAbstractItemView::item:disabled { color: #75715E; }
QStatusBar { background: #1E1F1C; color: #F8F8F2; }
QStatusBar QLabel { color: #F8F8F2; padding: 0 8px; border-left: 1px solid #49483E; }
QDockWidget { color: #F8F8F2; font-weight: bold; }
QDockWidget::title { background: #49483E; padding: 2px 4px; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button {
    background: #3E3D32; border: 1px solid #75715E; padding: 0px;
}
QFrame[pane="true"] { background: #272822; color: #F8F8F2; }
QFrame[pane="true"] QLabel { color: #F8F8F2; }
QFrame[pane="true"] QTableWidget, QFrame[pane="true"] QListWidget {
    background: #272822; color: #F8F8F2; border: none;
    selection-background-color: #49483E; selection-color: #F8F8F2;
}
QFrame[pane="true"] QHeaderView::section {
    background: #35362F; color: #E6DB74; border: none;
    border-bottom: 1px solid #49483E; padding: 1px 4px; font-weight: bold;
}
QFrame[pane="true"] QTableCornerButton::section { background: #35362F; border: none; }
QFrame#bandmapPane, QFrame#bandmapPane QTableWidget,
QFrame#radioABandmapPane, QFrame#radioABandmapPane QTableWidget {
    background: #1E1F1C; color: #F8F8F2;
}
QFrame#bandmapPane QHeaderView, QFrame#radioABandmapPane QHeaderView {
    background: #35362F; border: none;
}
QFrame#bandmapPane QHeaderView::section,
QFrame#radioABandmapPane QHeaderView::section { background: #35362F; color: #E6DB74; }
QFrame#clusterPane, QFrame#clusterPane QTableWidget { background: #1E1F1C; color: #F8F8F2; }
QFrame#clusterPane QHeaderView::section {
    background: #272822; color: #E6DB74; border-bottom: 1px solid #49483E;
}
QFrame#checkPartialPane, QFrame#checkPartialPane QListWidget {
    background: #272822; color: #F8F8F2;
}
QFrame#ratePane { background: #35362F; }
QFrame#ratePane QLabel { color: #F8F8F2; }
QFrame#contestInfoPane, QFrame#contestInfoPane QScrollArea,
QFrame#contestInfoPane QLabel { background: #272822; color: #F8F8F2; }
QLabel#finalScore { color: #66D9EF; background: #35362F; padding: 3px; }
QLabel#checkCallTitle { background: #35362F; color: #F8F8F2; padding: 2px 4px; }
QFrame#logWindow { background: #272822; }
QLabel#topStatus { background: #49483E; color: #E6DB74; padding: 2px 6px; }
QTableWidget#recentQsos {
    background: #272822; color: #F8F8F2; border: 1px solid #49483E;
    selection-background-color: #49483E; selection-color: #F8F8F2;
}
QTableWidget#recentQsos QHeaderView::section {
    background: #35362F; color: #E6DB74; border: none;
    border-bottom: 1px solid #49483E; padding: 1px 4px; font-weight: bold;
}
QFrame#entryPanel { background: #35362F; border: 2px solid #49483E; }
QFrame#entryPanel[active="true"] { border: 2px solid #A6E22E; }
QLabel#entryHeader { background: #49483E; color: #F8F8F2; padding: 2px 6px; }
QFrame#entryPanel[active="true"] QLabel#entryHeader { background: #49483E; }
QLabel#fieldCaption { color: #66D9EF; font-size: 8pt; font-weight: bold; }
QFrame#entryPanel QLineEdit, QFrame#entryPanel QComboBox {
    background: #1E1F1C; color: #F8F8F2; border: 1px solid #49483E; padding: 1px 3px;
}
QFrame#entryPanel QLineEdit:focus { background: #35362F; border: 2px solid #A6E22E; }
QFrame#entryPanel QLineEdit[sentField="true"] { background: #272822; color: #66D9EF; }
QLabel#dupeBadge[state="dupe"] { background: #F92672; color: #F8F8F2; }
QLabel#dupeBadge[state="mult"] { background: #A6E22E; color: #272822; }
QLabel#dupeBadge[state="ok"] { background: transparent; color: #A6E22E; }
QLabel#callInfoA, QLabel#callInfoB { color: #66D9EF; }
QLabel#messageLine { color: #F92672; background: transparent; }
QLabel#messageLine[alert="true"] { background: #A6E22E; color: #272822; }
QFrame#fkeyBar QPushButton {
    background: #3E3D32; border: 1px solid #75715E; padding: 3px 4px; color: #F8F8F2;
}
QFrame#fkeyBar QPushButton:hover { background: #49483E; }
QFrame#fkeyBar QPushButton:pressed { background: #A6E22E; color: #272822; }
)");
    }

    return QStringLiteral(R"(
QMainWindow { background: #20B2AA; }
QMainWindow::separator { background: #178F89; width: 4px; height: 4px; }
QMenuBar { background: #ECECEC; color: #202020; }
QMenuBar::item {
    background: transparent; color: #202020; padding: 4px 8px;
}
QMenuBar::item:selected { background: #C7E7E4; color: #101010; }
QMenu {
    background: #F5F5F5; color: #202020; border: 1px solid #888888;
}
QMenu::item {
    background: transparent; color: #202020; padding: 4px 24px 4px 8px;
}
QMenu::item:selected { background: #4682B4; color: white; }
QMenu::item:disabled { color: #666666; }
QMenu::separator { height: 1px; background: #C0C0C0; margin: 4px 8px; }
QToolBar {
    background: #ECECEC; color: #202020; border: none; spacing: 4px; padding: 2px;
}
QToolBar QCheckBox, QToolBar QLabel { color: #202020; }
QComboBox { color: #202020; background: #FFFFFF; }
QComboBox QAbstractItemView {
    color: #202020; background: #FFFFFF;
    selection-background-color: #C7E7E4; selection-color: #101010;
}
QComboBox QAbstractItemView::item:disabled { color: #666666; }
QStatusBar { background: #D4D0C8; color: #202020; }
QStatusBar QLabel {
    color: #202020; padding: 0 8px; border-left: 1px solid #A0A0A0;
}

QDockWidget { color: white; font-weight: bold; }
QDockWidget::title {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #F04040, stop:0.5 #CE0000, stop:1 #A00000);
    padding: 2px 4px; text-align: left;
}
QDockWidget::close-button, QDockWidget::float-button {
    background: #E8E8E8; border: 1px solid #700000; padding: 0px;
}

QFrame[pane="true"] { background: #40E0D0; color: black; }
QFrame[pane="true"] QLabel { color: black; }
QFrame[pane="true"] QTableWidget, QFrame[pane="true"] QListWidget {
    background: #40E0D0; color: black; border: none;
    selection-background-color: #4682B4; selection-color: white;
}
QFrame[pane="true"] QHeaderView::section {
    background: #40E0D0; color: #000080; border: none;
    border-bottom: 1px solid #178F89; padding: 1px 4px; font-weight: bold;
}
QFrame[pane="true"] QTableCornerButton::section { background: #40E0D0; border: none; }

QFrame#bandmapPane, QFrame#bandmapPane QTableWidget,
QFrame#radioABandmapPane, QFrame#radioABandmapPane QTableWidget {
    background: #AFEEEE;
}
QFrame#bandmapPane QHeaderView, QFrame#radioABandmapPane QHeaderView {
    background: #AFEEEE; border: none;
}
QFrame#bandmapPane QHeaderView::section,
QFrame#radioABandmapPane QHeaderView::section { background: #AFEEEE; }
QFrame#clusterPane, QFrame#clusterPane QTableWidget { background: black; color: white; }
QFrame#clusterPane QHeaderView::section {
    background: #202020; color: #FFFF00; border-bottom: 1px solid #404040;
}
QFrame#checkPartialPane, QFrame#checkPartialPane QListWidget {
    background: #4682B4; color: white;
}
QFrame#ratePane { background: #0000CD; }
QFrame#ratePane QLabel { color: white; }
QFrame#contestInfoPane, QFrame#contestInfoPane QScrollArea,
QFrame#contestInfoPane QLabel { background: #40E0D0; }
QLabel#finalScore { color: #0000CD; background: #C0C0C0; padding: 3px; }
QLabel#checkCallTitle { background: #C3C2C0; color: black; padding: 2px 4px; }

QFrame#logWindow { background: #20B2AA; }
QLabel#topStatus { background: #FFFF00; color: black; padding: 2px 6px; }
QTableWidget#recentQsos {
    background: #20B2AA; color: black; border: 1px solid #178F89;
    selection-background-color: #4682B4; selection-color: white;
}
QTableWidget#recentQsos QHeaderView::section {
    background: #20B2AA; color: #FFFF00; border: none;
    border-bottom: 1px solid #178F89; padding: 1px 4px; font-weight: bold;
}

QFrame#entryPanel { background: #48D1CC; border: 2px solid #20B2AA; }
QFrame#entryPanel[active="true"] { border: 2px solid #CE0000; }
QLabel#entryHeader { background: #20B2AA; color: white; padding: 2px 6px; }
QFrame#entryPanel[active="true"] QLabel#entryHeader { background: #CE0000; }
QLabel#fieldCaption { color: #000080; font-size: 8pt; font-weight: bold; }
QFrame#entryPanel QLineEdit, QFrame#entryPanel QComboBox {
    background: white; color: black; border: 1px solid #178F89; padding: 1px 3px;
}
QFrame#entryPanel QLineEdit:focus {
    background: #FFFFE0; border: 2px solid #CE0000;
}
QFrame#entryPanel QLineEdit[sentField="true"] { background: #E0FFFF; color: #404040; }
QLabel#dupeBadge { border-radius: 2px; padding: 2px 6px; }
QLabel#dupeBadge[state="dupe"] { background: #CE0000; color: white; }
QLabel#dupeBadge[state="mult"] { background: #90EE90; color: #CE0000; }
QLabel#dupeBadge[state="ok"] { background: transparent; color: #006400; }
QLabel#callInfoA, QLabel#callInfoB { color: #000080; }

QLabel#messageLine { color: #CE0000; background: transparent; }
QLabel#messageLine[alert="true"] { background: #90EE90; }

QFrame#fkeyBar QPushButton {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #F4F4F4, stop:1 #C8C8C8);
    border: 1px solid #707070; padding: 3px 4px; color: black;
}
QFrame#fkeyBar QPushButton:hover { background: #FFFFE0; }
QFrame#fkeyBar QPushButton:pressed { background: #FFFF00; }
)");
}

bool sameDupeKey(const QStringList &dupeKey, const Qso &qso, const QString &callsign,
                 const QString &band, const QString &mode)
{
    if (qso.invalid) {
        return false;
    }
    const QStringList key = dupeKey.isEmpty()
        ? QStringList{QStringLiteral("callsign"), QStringLiteral("band"),
                      QStringLiteral("mode")}
        : dupeKey;
    if (qso.callsign.compare(callsign, Qt::CaseInsensitive) != 0) {
        return false;
    }
    if (key.contains(QStringLiteral("band"), Qt::CaseInsensitive)
        && qso.band.compare(band, Qt::CaseInsensitive) != 0) {
        return false;
    }
    if (key.contains(QStringLiteral("mode"), Qt::CaseInsensitive)
        && qso.mode.compare(mode, Qt::CaseInsensitive) != 0) {
        return false;
    }
    return true;
}

QString groupedNumber(qint64 value)
{
    QString digits = QString::number(value);
    for (int position = digits.size() - 3; position > 0; position -= 3) {
        digits.insert(position, QLatin1Char(' '));
    }
    return digits;
}

void appendClusterRow(QTableWidget *table, const Spot &spot)
{
    const int row = table->rowCount();
    table->insertRow(row);
    const QStringList values{
        spot.timestampUtc.toUTC().toString(QStringLiteral("HHmm")),
        QString::number(static_cast<double>(spot.frequencyHz) / 1000.0, 'f', 1),
        spot.callsign, spot.spotter, spot.source, spot.comment};
    for (int column = 0; column < values.size(); ++column) {
        auto *item = new QTableWidgetItem(values.at(column));
        item->setForeground(column == 2 ? QColor(QStringLiteral("#FFFF00"))
                                        : QColor(Qt::white));
        if (column == 0) {
            item->setData(Qt::UserRole, spot.id);
        }
        table->setItem(row, column, item);
    }
    while (table->rowCount() > 500) {
        table->removeRow(0);
    }
}

QString importDupeIdentity(const QStringList &dupeKey, const Qso &qso)
{
    QStringList values{qso.callsign.trimmed().toUpper()};
    if (dupeKey.contains(QStringLiteral("band"), Qt::CaseInsensitive)) {
        values.append(qso.band.trimmed().toLower());
    }
    if (dupeKey.contains(QStringLiteral("mode"), Qt::CaseInsensitive)) {
        values.append(qso.mode.trimmed().toUpper());
    }
    return values.join(QChar(0x1f));
}

bool validImportUuid(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
                       "[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch();
}

bool sameImportedQso(const Qso &left, const Qso &right,
                     const QString &fallbackStationUuid)
{
    const QString leftStationUuid = left.stationUuid.isEmpty()
        ? fallbackStationUuid : left.stationUuid;
    return left.qsoUuid == right.qsoUuid
        && leftStationUuid == right.stationUuid
        && left.callsign.compare(right.callsign, Qt::CaseInsensitive) == 0
        && left.stationCallsign.compare(right.stationCallsign, Qt::CaseInsensitive) == 0
        && left.band.compare(right.band, Qt::CaseInsensitive) == 0
        && left.timestampUtc.toUTC() == right.timestampUtc.toUTC()
        && left.frequencyHz == right.frequencyHz
        && left.mode.compare(right.mode, Qt::CaseInsensitive) == 0
        && left.sentExchange == right.sentExchange
        && left.receivedExchange == right.receivedExchange
        && left.sentExchangeFields == right.sentExchangeFields
        && left.receivedExchangeFields == right.receivedExchangeFields
        && left.reservationUuid == right.reservationUuid
        && left.points == right.points;
}

QJsonObject importExchangeObject(const QMap<QString, QString> &fields)
{
    QJsonObject result;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        result.insert(it.key(), it.value());
    }
    return result;
}

QJsonObject importOperation(const Qso &qso, const QString &logUuid,
                            const QString &stationUuid)
{
    const QString operationUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    const QJsonObject snapshot{
        {QStringLiteral("qso_uuid"), qso.qsoUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("callsign"), qso.callsign.trimmed().toUpper()},
        {QStringLiteral("station_callsign"), qso.stationCallsign.trimmed().toUpper()},
        {QStringLiteral("band"), qso.band.trimmed().toLower()},
        {QStringLiteral("timestamp_utc"),
         qso.timestampUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("frequency_hz"), qso.frequencyHz},
        {QStringLiteral("mode"), qso.mode.trimmed().toUpper()},
        {QStringLiteral("sent_exchange"), qso.sentExchange},
        {QStringLiteral("received_exchange"), qso.receivedExchange},
        {QStringLiteral("sent_exchange_fields"),
         importExchangeObject(qso.sentExchangeFields)},
        {QStringLiteral("received_exchange_fields"),
         importExchangeObject(qso.receivedExchangeFields)},
        {QStringLiteral("points"), qso.points}
    };
    QJsonObject operation{
        {QStringLiteral("operation_uuid"), operationUuid},
        {QStringLiteral("log_uuid"), logUuid},
        {QStringLiteral("station_uuid"), stationUuid},
        {QStringLiteral("created_at_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("kind"), QStringLiteral("qso.create")},
        {QStringLiteral("expected_version"), 0},
        {QStringLiteral("qso"), snapshot}
    };
    if (!qso.reservationUuid.isEmpty()) {
        operation.insert(QStringLiteral("reservation_uuid"), qso.reservationUuid);
    }
    return operation;
}

QString clientSecretKey(const QString &host, quint16 port,
                        const QString &stationUuid)
{
    const QByteArray identity = QStringLiteral("%1:%2:%3")
        .arg(host.trimmed().toLower()).arg(port).arg(stationUuid)
        .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

} // namespace

MainWindow::MainWindow(SqliteRepository &repository, QWidget *parent)
    : QMainWindow(parent)
    , m_repository(repository)
{
    QSettings n1mmSettings;
    N1mmUdpConfiguration n1mmConfiguration;
    n1mmConfiguration.enabled = n1mmSettings.value(
        QStringLiteral("n1mmUdp/enabled"), false).toBool();
    n1mmConfiguration.address.setAddress(n1mmSettings.value(
        QStringLiteral("n1mmUdp/address"), QStringLiteral("127.0.0.1")).toString());
    n1mmConfiguration.port = static_cast<quint16>(n1mmSettings.value(
        QStringLiteral("n1mmUdp/port"), 12060).toUInt());
    QString n1mmConfigurationError;
    m_n1mmBroadcaster.setConfiguration(n1mmConfiguration,
                                       &n1mmConfigurationError);
    m_networkLastContactUtc = QDateTime::fromString(
        QSettings().value(QStringLiteral("network/client/lastContactUtc"))
            .toString(), Qt::ISODateWithMs).toUTC();
    setWindowTitle(tr("Contest Logger"));
    resize(1600, 960);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    QString colorScheme = QSettings().value(
        QStringLiteral("ui/colorPalette"), QStringLiteral("current")).toString();
    if (colorScheme != QLatin1String("current")
        && colorScheme != QLatin1String("monokai")) {
        qWarning().noquote() << "Unsupported saved color palette; using current palette.";
        colorScheme = QStringLiteral("current");
    }
    QApplication::setPalette(paletteForColorScheme(colorScheme));
    setStyleSheet(dxlogStyleSheet(colorScheme == QLatin1String("monokai")));

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *newLogAction = fileMenu->addAction(tr("&New log..."));
    newLogAction->setObjectName(QStringLiteral("newLogAction"));
    newLogAction->setShortcut(QKeySequence::New);
    connect(newLogAction, &QAction::triggered, this, &MainWindow::createNewLog);
    auto *openLogAction = fileMenu->addAction(tr("&Open log..."));
    openLogAction->setObjectName(QStringLiteral("openLogAction"));
    openLogAction->setShortcut(QKeySequence::Open);
    connect(openLogAction, &QAction::triggered, this, &MainWindow::openLog);
    auto *logPropertiesAction = fileMenu->addAction(tr("Log &properties..."));
    logPropertiesAction->setObjectName(QStringLiteral("logPropertiesAction"));
    connect(logPropertiesAction, &QAction::triggered,
            this, &MainWindow::editLogProperties);
    fileMenu->addSeparator();
    auto *contestMenu = menuBar()->addMenu(tr("&Contest"));
    auto *importAction = contestMenu->addAction(tr("&Import YAML definition..."));
    connect(importAction, &QAction::triggered,
            this, &MainWindow::importContestDefinition);
    auto *restoreDefinitionAction =
        contestMenu->addAction(tr("Restore previous YAML definition..."));
    restoreDefinitionAction->setObjectName(
        QStringLiteral("restoreContestDefinitionVersionAction"));
    connect(restoreDefinitionAction, &QAction::triggered,
            this, &MainWindow::restoreContestDefinitionVersion);
    auto *qtcAction = contestMenu->addAction(tr("Manage WAE QTC..."));
    qtcAction->setObjectName(QStringLiteral("manageQtcAction"));
    connect(qtcAction, &QAction::triggered, this, &MainWindow::manageQtc);
    auto *operatingMenu = menuBar()->addMenu(tr("O&perating"));
    auto *runModeMenu = operatingMenu->addMenu(tr("Operating mode"));
    auto *runModeGroup = new QActionGroup(runModeMenu);
    runModeGroup->setExclusive(true);
    for (int mode = 0; mode < 2; ++mode) {
        auto *action = runModeMenu->addAction(mode == 0 ? tr("Run") : tr("Search & Pounce"));
        action->setCheckable(true);
        action->setData(mode);
        action->setObjectName(mode == 0 ? QStringLiteral("runModeAction")
                                        : QStringLiteral("searchAndPounceAction"));
        runModeGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode] { setRunMode(mode); });
    }

    m_esmAction = operatingMenu->addAction(tr("Enter Sends Message (ESM)"));
    m_esmAction->setObjectName(QStringLiteral("esmAction"));
    m_esmAction->setCheckable(true);
    m_esmAction->setChecked(LogSettings(m_repository).value(QStringLiteral("operating/esmEnabled"),
                                               false).toBool());
    connect(m_esmAction, &QAction::toggled, this, [this](bool enabled) {
        LogSettings(m_repository).setValue(QStringLiteral("operating/esmEnabled"), enabled);
    });
    auto *undoQsoAction = operatingMenu->addAction(tr("Undo last QSO change"));
    undoQsoAction->setObjectName(QStringLiteral("undoLastQsoChangeAction"));
    undoQsoAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Z")));
    connect(undoQsoAction, &QAction::triggered,
            this, &MainWindow::undoLastQsoChange);
    auto *allowDupeLoggingAction = operatingMenu->addAction(tr("Allow logging duplicate QSOs"));
    allowDupeLoggingAction->setObjectName(QStringLiteral("allowDupeLoggingAction"));
    allowDupeLoggingAction->setCheckable(true);
    allowDupeLoggingAction->setChecked(LogSettings(m_repository).value(
        QStringLiteral("operating/allowDupeLogging"), false).toBool());
    connect(allowDupeLoggingAction, &QAction::toggled, this, [this](bool enabled) {
        LogSettings(m_repository).setValue(QStringLiteral("operating/allowDupeLogging"), enabled);
    });
    auto *configureEsmAction = operatingMenu->addAction(tr("Configure ESM messages..."));
    configureEsmAction->setObjectName(QStringLiteral("configureEsmMessagesAction"));
    connect(configureEsmAction, &QAction::triggered,
            this, &MainWindow::configureEsmMessages);
    m_autoCqAction = operatingMenu->addAction(tr("Auto CQ (F1 repeat)"));
    m_autoCqAction->setObjectName(QStringLiteral("autoCqAction"));
    m_autoCqAction->setCheckable(true);
    connect(m_autoCqAction, &QAction::toggled,
            this, &MainWindow::setAutoCqEnabled);
    const int savedRunMode =
        qBound(0, LogSettings(m_repository).value(QStringLiteral("operating/runMode"), 0).toInt(), 1);
    runModeGroup->actions().at(savedRunMode)->setChecked(true);
    auto *radioMenu = menuBar()->addMenu(tr("&Radio"));
    auto *settingsMenu = menuBar()->addMenu(tr("Op&tions"));
    auto *networkAction = settingsMenu->addAction(tr("Network operation..."));
    networkAction->setObjectName(QStringLiteral("configureNetworkConnectionAction"));
    connect(networkAction, &QAction::triggered,
            this, &MainWindow::configureNetworkConnection);
    m_hostModeAction =
        settingsMenu->addAction(tr("Start/stop network server..."));
    m_hostModeAction->setObjectName(QStringLiteral("configureHostModeAction"));
    connect(m_hostModeAction, &QAction::triggered,
            this, &MainWindow::configureHostMode);
    m_hostImportAction = settingsMenu->addAction(
        tr("Create central log / import local log..."));
    m_hostImportAction->setObjectName(QStringLiteral("hostImportLogAction"));
    m_hostImportAction->setEnabled(false);
    m_hostImportAction->setVisible(false);
    connect(m_hostImportAction, &QAction::triggered,
            this, &MainWindow::importLocalLogToHost);
    auto *disconnectNetworkAction =
        settingsMenu->addAction(tr("Disconnect network"));
    disconnectNetworkAction->setObjectName(
        QStringLiteral("disconnectNetworkConnectionAction"));
    connect(disconnectNetworkAction, &QAction::triggered,
            this, &MainWindow::disconnectNetworkConnection);
    auto *networkStatusAction =
        settingsMenu->addAction(tr("Network status and queue..."));
    networkStatusAction->setObjectName(QStringLiteral("networkStatusAction"));
    connect(networkStatusAction, &QAction::triggered,
            this, &MainWindow::showNetworkStatus);
    auto *n1mmSettingsAction =
        settingsMenu->addAction(tr("N1MM UDP broadcast settings..."));
    n1mmSettingsAction->setObjectName(QStringLiteral("configureN1mmUdpAction"));
    connect(n1mmSettingsAction, &QAction::triggered,
            this, &MainWindow::configureN1mmBroadcast);
    settingsMenu->addSeparator();
    QMenu *languageMenu = settingsMenu->addMenu(tr("Language"));
    languageMenu->setObjectName(QStringLiteral("languageMenu"));
    auto *languageGroup = new QActionGroup(languageMenu);
    languageGroup->setExclusive(true);
    auto *englishAction = languageMenu->addAction(tr("English"));
    englishAction->setObjectName(QStringLiteral("languageEnglishAction"));
    englishAction->setCheckable(true);
    languageGroup->addAction(englishAction);
    auto *polishAction = languageMenu->addAction(tr("Polski"));
    polishAction->setObjectName(QStringLiteral("languagePolishAction"));
    polishAction->setCheckable(true);
    languageGroup->addAction(polishAction);
    const QString savedLanguage = QSettings().value(
        QStringLiteral("ui/language"), QStringLiteral("en")).toString();
    (savedLanguage == QLatin1String("pl") ? polishAction : englishAction)
        ->setChecked(true);
    const auto saveLanguage = [this, englishAction, polishAction](
                                  const QString &language) {
        auto *translator = qApp->findChild<QTranslator *>(QStringLiteral("uiTranslator"));
        if (translator == nullptr) {
            translator = new QTranslator(qApp);
            translator->setObjectName(QStringLiteral("uiTranslator"));
        }
        if (language == QLatin1String("pl") && translator->isEmpty()
            && !translator->load(QStringLiteral(":/i18n/contest_logger_pl.qm"))) {
            QMessageBox::critical(this, tr("Language settings error"),
                                 tr("The Polish translation catalog could not be loaded."));
            englishAction->setChecked(true);
            return;
        }
        QSettings settings;
        const QString previousLanguage = settings.value(
            QStringLiteral("ui/language"), QStringLiteral("en")).toString();
        settings.setValue(QStringLiteral("ui/language"), language);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::critical(
                this, tr("Language settings error"),
                tr("The language preference could not be saved."));
            (previousLanguage == QLatin1String("pl") ? polishAction : englishAction)
                ->setChecked(true);
            return;
        }
        if (language == QLatin1String("pl")) {
            if (!QCoreApplication::installTranslator(translator)) {
                QMessageBox::critical(this, tr("Language settings error"),
                                     tr("The Polish translation catalog could not be loaded."));
                settings.setValue(QStringLiteral("ui/language"), previousLanguage);
                settings.sync();
                englishAction->setChecked(true);
                return;
            }
            QLocale::setDefault(QLocale(QLocale::Polish, QLocale::Poland));
        } else {
            QCoreApplication::removeTranslator(translator);
            QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        }
        m_retranslateUi();
        statusBar()->showMessage(
            tr("Language changed."), 7000);
    };
    connect(englishAction, &QAction::triggered, this,
            [saveLanguage] { saveLanguage(QStringLiteral("en")); });
    connect(polishAction, &QAction::triggered, this,
            [saveLanguage] { saveLanguage(QStringLiteral("pl")); });
    QMenu *colorPaletteMenu = settingsMenu->addMenu(tr("Color palette"));
    colorPaletteMenu->setObjectName(QStringLiteral("colorPaletteMenu"));
    auto *colorPaletteGroup = new QActionGroup(colorPaletteMenu);
    colorPaletteGroup->setExclusive(true);
    auto *currentPaletteAction = colorPaletteMenu->addAction(tr("Current"));
    currentPaletteAction->setObjectName(QStringLiteral("colorPaletteCurrentAction"));
    currentPaletteAction->setCheckable(true);
    colorPaletteGroup->addAction(currentPaletteAction);
    auto *monokaiPaletteAction = colorPaletteMenu->addAction(tr("Monokai"));
    monokaiPaletteAction->setObjectName(QStringLiteral("colorPaletteMonokaiAction"));
    monokaiPaletteAction->setCheckable(true);
    colorPaletteGroup->addAction(monokaiPaletteAction);
    const QString savedColorScheme = QSettings().value(
        QStringLiteral("ui/colorPalette"), QStringLiteral("current")).toString();
    (savedColorScheme == QLatin1String("monokai")
         ? monokaiPaletteAction : currentPaletteAction)
        ->setChecked(true);
    const auto saveColorScheme = [this, currentPaletteAction,
                                  monokaiPaletteAction](const QString &scheme) {
        QSettings settings;
        const QString previousScheme = settings.value(
            QStringLiteral("ui/colorPalette"), QStringLiteral("current")).toString();
        settings.setValue(QStringLiteral("ui/colorPalette"), scheme);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::critical(
                this, tr("Color palette settings error"),
                tr("The color palette preference could not be saved."));
            (previousScheme == QLatin1String("monokai")
                 ? monokaiPaletteAction : currentPaletteAction)
                ->setChecked(true);
            return;
        }
        QApplication::setPalette(paletteForColorScheme(scheme));
        setStyleSheet(dxlogStyleSheet(scheme == QLatin1String("monokai")));
        statusBar()->showMessage(tr("Color palette changed."), 5000);
    };
    connect(currentPaletteAction, &QAction::triggered, this,
            [saveColorScheme] { saveColorScheme(QStringLiteral("current")); });
    connect(monokaiPaletteAction, &QAction::triggered, this,
            [saveColorScheme] { saveColorScheme(QStringLiteral("monokai")); });
    auto *operatorAction = settingsMenu->addAction(tr("Operator and &station..."));
    connect(operatorAction, &QAction::triggered,
            this, &MainWindow::configureOperator);
    auto *operatorCallsignAction = settingsMenu->addAction(tr("Operator callsign..."));
    operatorCallsignAction->setObjectName(QStringLiteral("operatorCallsignAction"));
    operatorCallsignAction->setShortcut(QKeySequence(QStringLiteral("Alt+O")));
    connect(operatorCallsignAction, &QAction::triggered,
            this, &MainWindow::configureOperatorCallsign);
    auto *functionKeysAction =
        settingsMenu->addAction(tr("CW function key messages..."));
    functionKeysAction->setObjectName(QStringLiteral("configureFunctionKeysAction"));
    connect(functionKeysAction, &QAction::triggered,
            this, &MainWindow::configureFunctionKeys);
    auto *radioAction = radioMenu->addAction(tr("Radio A CAT settings..."));
    connect(radioAction, &QAction::triggered, this, &MainWindow::configureRadio);
    auto *radioBAction = radioMenu->addAction(tr("Radio B CAT settings..."));
    connect(radioBAction, &QAction::triggered, this, &MainWindow::configureRadioB);
    auto *reconnectRadioAAction = radioMenu->addAction(tr("Reconnect Radio A CAT"));
    reconnectRadioAAction->setObjectName(QStringLiteral("reconnectRadioAAction"));
    connect(reconnectRadioAAction, &QAction::triggered, this, [this] {
        m_radioControllers[0].connectRadio(radioConfigFromSettings(0));
    });
    auto *reconnectRadioBAction = radioMenu->addAction(tr("Reconnect Radio B CAT"));
    reconnectRadioBAction->setObjectName(QStringLiteral("reconnectRadioBAction"));
    connect(reconnectRadioBAction, &QAction::triggered, this, [this] {
        m_radioControllers[1].connectRadio(radioConfigFromSettings(1));
    });
    auto *disconnectAction = radioMenu->addAction(tr("Disconnect Radio A CAT"));
    disconnectAction->setObjectName(QStringLiteral("disconnectRadioAAction"));
    connect(disconnectAction, &QAction::triggered,
            &m_radioControllers[0], &RadioController::disconnectRadio);
    auto *disconnectRadioBAction = radioMenu->addAction(tr("Disconnect Radio B CAT"));
    disconnectRadioBAction->setObjectName(QStringLiteral("disconnectRadioBAction"));
    connect(disconnectRadioBAction, &QAction::triggered,
            &m_radioControllers[1], &RadioController::disconnectRadio);
    for (int radio = 0; radio < 2; ++radio) {
        connect(&m_radioControllers[radio], &RadioController::snapshotChanged,
                this, [this, radio](const RadioSnapshot &snapshot) {
                    updateRadioSnapshotForRadio(radio, snapshot);
                });
    }
    connect(&m_cwKeyerController, &CwKeyerController::statusChanged,
            this, &MainWindow::updateCwStatus);

    radioMenu->addSeparator();
    auto *clusterAction = radioMenu->addAction(tr("&DX cluster..."));
    clusterAction->setObjectName(QStringLiteral("configureDxClusterAction"));
    connect(clusterAction, &QAction::triggered,
            this, &MainWindow::configureDxCluster);
    auto *disconnectClusterAction =
        radioMenu->addAction(tr("Disconnect DX cluster"));
    disconnectClusterAction->setObjectName(QStringLiteral("disconnectDxClusterAction"));
    connect(disconnectClusterAction, &QAction::triggered,
            this, &MainWindow::disconnectDxClusters);
    radioMenu->addSeparator();
    auto *cwKeyerAction = radioMenu->addAction(tr("CW keyer..."));
    connect(cwKeyerAction, &QAction::triggered,
            this, &MainWindow::configureCwKeyer);
        auto *reconnectCwAction = radioMenu->addAction(tr("Reconnect CW keyer"));
        reconnectCwAction->setObjectName(QStringLiteral("reconnectCwKeyerAction"));
        connect(reconnectCwAction, &QAction::triggered, this, [this] {
            const int radioIndex = cwRadioForContext(currentEntryContext());
            QSettings settings;
            const CwKeyerConfig config = cwKeyerConfigFromSettings(
                settings, radioIndex, radioConfigFromSettings(radioIndex));
            m_cwWpm = qBound(5, config.wpm, 80);
            updateTopStatus();
            m_cwConfiguredRadio = radioIndex;
            m_cwKeyerController.connectKeyer(config);
        });
    auto *disconnectCwAction = radioMenu->addAction(tr("Disconnect CW keyer"));
        disconnectCwAction->setObjectName(QStringLiteral("disconnectCwKeyerAction"));
    connect(disconnectCwAction, &QAction::triggered,
            &m_cwKeyerController, &CwKeyerController::disconnectKeyer);
    auto *abortCwAction = radioMenu->addAction(tr("Abort CW transmission"));
    abortCwAction->setObjectName(QStringLiteral("abortCwAction"));
    abortCwAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Escape));
    abortCwAction->setShortcutContext(Qt::WindowShortcut);
    connect(abortCwAction, &QAction::triggered, this, &MainWindow::abortCwTransmission);
    auto *escapeCwAction = new QAction(this);
    escapeCwAction->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(escapeCwAction);
    connect(escapeCwAction, &QAction::triggered, this, [this] {
        if (m_cwInputWindow->isVisible()
            && m_cwStatus.processedEnqueues == m_cwEnqueuedRequests
            && !m_cwStatus.transmitting && m_cwStatus.queuedMessages == 0
            && !m_autoCqEnabled) {
            m_cwInputWindow->hide();
            m_callsignEdits[currentEntryContext()]->setFocus();
        } else {
            abortCwTransmission();
        }
    });
    auto *cwInputAction = operatingMenu->addAction(tr("Live CW input"));
    cwInputAction->setObjectName(QStringLiteral("cwInputAction"));
    cwInputAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(cwInputAction, &QAction::triggered, this, &MainWindow::showCwInput);
    auto *cwFasterAction = operatingMenu->addAction(tr("Increase CW speed"));
    cwFasterAction->setObjectName(QStringLiteral("cwFasterAction"));
    cwFasterAction->setShortcut(QKeySequence(Qt::Key_PageUp));
    connect(cwFasterAction, &QAction::triggered, this,
            [this] { setCwWpm(qMin(80, m_cwWpm + 1)); });
    auto *cwSlowerAction = operatingMenu->addAction(tr("Decrease CW speed"));
    cwSlowerAction->setObjectName(QStringLiteral("cwSlowerAction"));
    cwSlowerAction->setShortcut(QKeySequence(Qt::Key_PageDown));
    connect(cwSlowerAction, &QAction::triggered, this,
            [this] { setCwWpm(qMax(5, m_cwWpm - 1)); });

    auto *wipeAction = operatingMenu->addAction(tr("&Wipe QSO entry"));
    wipeAction->setObjectName(QStringLiteral("wipeQsoEntryAction"));
    wipeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    wipeAction->setShortcutContext(Qt::WindowShortcut);
    connect(wipeAction, &QAction::triggered,
            this, [this] { wipeEntry(currentEntryContext()); });
    auto *switchEntryAction = operatingMenu->addAction(tr("Switch entry A/B"));
    switchEntryAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab));
    connect(switchEntryAction, &QAction::triggered, this, [this] {
        const int other = 1 - currentEntryContext();
        if (m_entryGroups[other]->isVisible()) {
            m_callsignEdits[other]->setFocus();
        }
    });
    const auto stepAndActivateSpot = [this](int direction) {
        QTableWidget *bandmap =
            (m_so2vToggle->isChecked() || m_so2rToggle->isChecked())
                && currentEntryContext() == 0
            ? m_radioABandmap : m_bandmap;
        selectAdjacentBandmapRow(bandmap, direction);
        const QTableWidgetItem *item = bandmap->item(bandmap->currentRow(), 0);
        if (item == nullptr) {
            return;
        }
        if (bandmap == m_radioABandmap) {
            const int radio = m_so2rToggle->isChecked()
                ? 0 : m_activeRadioSelector->currentData().toInt();
            activateSpotId(item->data(Qt::UserRole).toLongLong(), radio, 0);
        } else {
            activateSpot(bandmap->currentRow(), 0);
        }
    };
    auto *previousSpotAction = operatingMenu->addAction(tr("Previous bandmap spot"));
    previousSpotAction->setObjectName(QStringLiteral("previousBandmapSpotAction"));
    previousSpotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    previousSpotAction->setShortcutContext(Qt::WindowShortcut);
    connect(previousSpotAction, &QAction::triggered, this,
            [stepAndActivateSpot] { stepAndActivateSpot(-1); });
    auto *nextSpotAction = operatingMenu->addAction(tr("Next bandmap spot"));
    nextSpotAction->setObjectName(QStringLiteral("nextBandmapSpotAction"));
    nextSpotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    nextSpotAction->setShortcutContext(Qt::WindowShortcut);
    connect(nextSpotAction, &QAction::triggered, this,
            [stepAndActivateSpot] { stepAndActivateSpot(1); });
    auto *nextNewMultiplierAction =
        operatingMenu->addAction(tr("Next bandmap spot with a new multiplier"));
    nextNewMultiplierAction->setObjectName(
        QStringLiteral("nextNewMultiplierSpotAction"));
    nextNewMultiplierAction->setShortcut(
        QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Down));
    nextNewMultiplierAction->setShortcutContext(Qt::WindowShortcut);
    connect(nextNewMultiplierAction, &QAction::triggered, this, [this] {
        QTableWidget *bandmap =
            (m_so2vToggle->isChecked() || m_so2rToggle->isChecked())
                && currentEntryContext() == 0
            ? m_radioABandmap : m_bandmap;
        const int rowCount = bandmap->rowCount();
        const int startRow = bandmap->currentRow();
        const ContestDefinition *definition = activeDefinition();
        if (rowCount == 0 || definition == nullptr
            || definition->multipliers.isEmpty()) {
            statusBar()->showMessage(
                tr("No eligible multiplier spot is available."), 3000);
            return;
        }
        const int contextIndex = currentEntryContext();
        const QString currentMode =
            m_modeSelectors[contextIndex]->currentText().trimmed().toUpper();
        const auto resolver = multiplierResolver();
        const ContestEngine engine;
        for (int offset = 1; offset <= rowCount; ++offset) {
            const int row = startRow < 0
                ? offset - 1 : (startRow + offset) % rowCount;
            const QTableWidgetItem *item = bandmap->item(row, 0);
            if (item == nullptr) {
                continue;
            }
            const Spot spot = m_spots.value(item->data(Qt::UserRole).toLongLong());
            Qso candidate;
            candidate.callsign = spot.callsign.trimmed().toUpper();
            candidate.band = spot.band.trimmed().toLower();
            candidate.mode = spot.mode.trimmed().isEmpty()
                ? currentMode : spot.mode.trimmed().toUpper();
            if (candidate.callsign.isEmpty() || candidate.band.isEmpty()) {
                continue;
            }
            const bool duplicate = std::any_of(
                m_logQsos.cbegin(), m_logQsos.cend(),
                [definition, &candidate](const Qso &worked) {
                    return !worked.invalid
                        && sameDupeKey(definition->dupeKey, worked,
                                       candidate.callsign, candidate.band,
                                       candidate.mode);
                });
            if (duplicate) {
                continue;
            }
            const QStringList keys = engine.multiplierKeys(
                *definition, candidate, resolver);
            if (std::none_of(keys.cbegin(), keys.cend(),
                             [this](const QString &key) {
                                 return !key.isEmpty()
                                     && !m_workedMultiplierKeys.contains(key);
                             })) {
                continue;
            }
            bandmap->setCurrentCell(row, 0);
            if (bandmap == m_radioABandmap) {
                const int radio = m_so2rToggle->isChecked()
                    ? 0 : m_activeRadioSelector->currentData().toInt();
                activateSpotId(item->data(Qt::UserRole).toLongLong(), radio, 0);
            } else {
                activateSpot(row, 0);
            }
            return;
        }
        statusBar()->showMessage(
            tr("No unworked new-multiplier spot matches the current filters."), 4000);
    });
    operatingMenu->addAction(abortCwAction);
    operatingMenu->addSeparator();
    auto *editKeysMenu = new QMenu(tr("Edit function key message"), operatingMenu);
    for (int key = 0; key < 8; ++key) {
        m_functionKeyActions[key] = operatingMenu->addAction(QString());
        m_functionKeyActions[key]->setShortcut(QKeySequence(Qt::Key_F1 + key));
        m_functionKeyActions[key]->setShortcutContext(Qt::WindowShortcut);
        connect(m_functionKeyActions[key], &QAction::triggered,
                this, [this, key] { sendFunctionKey(key); });
        auto *editKey = editKeysMenu->addAction(QStringLiteral("F%1...").arg(key + 1));
        connect(editKey, &QAction::triggered, this, [this, key] { editFunctionKey(key); });
    }
    operatingMenu->addSeparator();
    operatingMenu->addMenu(editKeysMenu);

    auto *importLogAction = fileMenu->addAction(tr("Import ADIF / Cabrillo..."));
    importLogAction->setObjectName(QStringLiteral("importLogAction"));
    connect(importLogAction, &QAction::triggered, this, &MainWindow::importLog);
    auto *exportCabrilloAction = fileMenu->addAction(tr("Export Cabrillo..."));
    connect(exportCabrilloAction, &QAction::triggered,
            this, &MainWindow::exportCabrillo);
    auto *exportAdifAction = fileMenu->addAction(tr("Export ADIF..."));
    connect(exportAdifAction, &QAction::triggered,
            this, &MainWindow::exportAdif);
    auto *broadcastLogAction =
        fileMenu->addAction(tr("Broadcast full log to N1MM UDP..."));
    broadcastLogAction->setObjectName(QStringLiteral("broadcastFullLogN1mmAction"));
    connect(broadcastLogAction, &QAction::triggered,
            this, &MainWindow::broadcastFullLogToN1mm);
    fileMenu->addSeparator();
    auto *backupAction = fileMenu->addAction(tr("Back up database..."));
    connect(backupAction, &QAction::triggered,
            this, &MainWindow::backupDatabase);
    auto *automaticBackupAction =
        fileMenu->addAction(tr("Automatic backup settings..."));
    automaticBackupAction->setObjectName(QStringLiteral("automaticBackupSettingsAction"));
    connect(automaticBackupAction, &QAction::triggered,
            this, &MainWindow::configureAutomaticBackups);
    auto *supportDataMenu = fileMenu->addMenu(tr("Support data"));
    auto *callHistoryAction =
        supportDataMenu->addAction(tr("Call history file..."));
    connect(callHistoryAction, &QAction::triggered,
            this, &MainWindow::importCallHistory);
    auto *masterCpyAction =
        supportDataMenu->addAction(tr("Master callsign list from file..."));
    connect(masterCpyAction, &QAction::triggered,
            this, &MainWindow::importMasterCpy);
    auto *downloadMasterScpAction =
        supportDataMenu->addAction(tr("Download/update Master.SCP..."));
    downloadMasterScpAction->setObjectName(QStringLiteral("downloadMasterScpAction"));
    connect(downloadMasterScpAction, &QAction::triggered, this, [this] {
        const QString logPath = m_repository.databasePath();
        auto *manager = new QNetworkAccessManager(this);
        const QUrl catalogUrl(QStringLiteral(
            "https://www.supercheckpartial.com/api/v1/files"));
        const QNetworkRequest catalogRequest = supportDataRequest(catalogUrl);
        issueNetworkRequest(this, manager, catalogRequest,
                            [this, manager, logPath](QNetworkReply *catalogReply) {
            const QString catalogError = networkReplyError(catalogReply);
            if (!catalogError.isEmpty()) {
                manager->deleteLater();
                QMessageBox::warning(this, tr("Master.SCP download failed"), catalogError);
                return;
            }
            QJsonParseError jsonError;
            const QJsonDocument catalog = QJsonDocument::fromJson(
                catalogReply->readAll(), &jsonError);
            if (!catalog.isArray()) {
                manager->deleteLater();
                QMessageBox::warning(
                    this, tr("Master.SCP download failed"),
                    tr("The SCP file catalog is invalid: %1").arg(jsonError.errorString()));
                return;
            }
            QJsonObject masterFile;
            for (const QJsonValue &value : catalog.array()) {
                if (value.isObject()
                    && value.toObject().value(QStringLiteral("name")).toString()
                           .compare(QStringLiteral("MASTER.SCP"), Qt::CaseInsensitive) == 0) {
                    masterFile = value.toObject();
                    break;
                }
            }
            const QString fileName = masterFile.value(QStringLiteral("name")).toString();
            const QString etag = masterFile.value(QStringLiteral("etag")).toString();
            const qint64 expectedSize = masterFile.value(QStringLiteral("size")).toInteger();
            if (fileName.isEmpty() || etag.isEmpty() || expectedSize <= 0) {
                manager->deleteLater();
                QMessageBox::warning(
                    this, tr("Master.SCP download failed"),
                    tr("The SCP catalog does not contain valid Master.SCP metadata."));
                return;
            }

            QSettings settings;
            const QString cachePath = settings.value(
                QStringLiteral("supportData/masterScpPath")).toString();
            const QString cachedEtag = settings.value(
                QStringLiteral("supportData/masterScpEtag")).toString();
            const QString encodedName = QString::fromLatin1(
                QUrl::toPercentEncoding(fileName));
            QNetworkRequest fileRequest = supportDataRequest(QUrl(
                QStringLiteral("https://www.supercheckpartial.com/downloads/%1")
                    .arg(encodedName)));
            if (!m_repository.isStandaloneLog()
                && cachedEtag == etag && QFileInfo::exists(cachePath)) {
                fileRequest.setRawHeader(
                    "If-None-Match", QByteArray("\"") + etag.toUtf8() + QByteArray("\""));
            }
            issueNetworkRequest(this, manager, fileRequest,
                                [this, manager, fileName, etag, expectedSize, logPath]
                                (QNetworkReply *fileReply) {
                if (m_repository.databasePath() != logPath) {
                    manager->deleteLater();
                    QMessageBox::warning(this, tr("Support data download"),
                                         tr("The log changed. Downloaded data was not applied."));
                    return;
                }
                const int status = fileReply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 304) {
                    manager->deleteLater();
                    statusBar()->showMessage(tr("Master.SCP is already up to date."), 5000);
                    return;
                }
                const QString downloadError = networkReplyError(fileReply);
                if (!downloadError.isEmpty()) {
                    manager->deleteLater();
                    QMessageBox::warning(this, tr("Master.SCP download failed"),
                                         downloadError);
                    return;
                }
                const QByteArray contents = fileReply->readAll();
                if (contents.size() != expectedSize) {
                    manager->deleteLater();
                    QMessageBox::warning(
                        this, tr("Master.SCP download failed"),
                        tr("Downloaded file size does not match the catalog."));
                    return;
                }
                QStringList callsigns;
                QString error;
                if (!SupportDataParser::parseMasterCpy(
                        QString::fromUtf8(contents), callsigns, &error)) {
                    manager->deleteLater();
                    QMessageBox::warning(this, tr("Invalid Master.SCP file"), error);
                    return;
                }
                QString path;
                if (!saveSupportDataFile(QStringLiteral("master.scp"), contents,
                                         path, error)) {
                    manager->deleteLater();
                    QMessageBox::warning(this, tr("Unable to save Master.SCP"), error);
                    return;
                }
                QSettings settings;
                settings.setValue(QStringLiteral("supportData/masterScpPath"), path);
                settings.setValue(QStringLiteral("supportData/masterScpEtag"), etag);
                QStringList paths = settings.value(
                    QStringLiteral("supportData/masterCpyFiles")).toStringList();
                if (paths.isEmpty()) {
                    const QString legacyPath = settings.value(
                        QStringLiteral("supportData/masterCpy")).toString();
                    if (!legacyPath.isEmpty()) {
                        paths.append(legacyPath);
                    }
                }
                if (!paths.contains(path)) {
                    paths.append(path);
                }
                settings.setValue(QStringLiteral("supportData/masterCpyFiles"), paths);
                if (!storeSupportAsset(supportAssetKey(QStringLiteral("support/master"), path), contents)) {
                    manager->deleteLater();
                    return;
                }
                loadSupportData();
                manager->deleteLater();
                statusBar()->showMessage(
                    tr("Loaded %1 callsigns from Master.SCP.").arg(callsigns.size()), 6000);
            });
        });
    });
    auto *restoreSupportDataAction =
        supportDataMenu->addAction(tr("Restore previous downloaded support data..."));
    restoreSupportDataAction->setObjectName(
        QStringLiteral("restoreSupportDataVersionAction"));
    connect(restoreSupportDataAction, &QAction::triggered,
            this, &MainWindow::restoreSupportDataVersion);
    auto *ctyDatAction =
        supportDataMenu->addAction(tr("wl_cty.dat from file..."));
    connect(ctyDatAction, &QAction::triggered,
            this, &MainWindow::importCtyDat);
    auto *downloadCtyAction =
        supportDataMenu->addAction(tr("Download/update wl_cty.dat..."));
    downloadCtyAction->setObjectName(QStringLiteral("downloadWlCtyAction"));
    connect(downloadCtyAction, &QAction::triggered, this, [this] {
        const QString logPath = m_repository.databasePath();
        auto *manager = new QNetworkAccessManager(this);
        const QNetworkRequest request = supportDataRequest(QUrl(
            QStringLiteral("https://www.country-files.com/cty/wl_cty.dat")));
        issueNetworkRequest(this, manager, request,
                            [this, manager, logPath](QNetworkReply *reply) {
            if (m_repository.databasePath() != logPath) {
                manager->deleteLater();
                QMessageBox::warning(this, tr("Support data download"),
                                     tr("The log changed. Downloaded data was not applied."));
                return;
            }
            const QString downloadError = networkReplyError(reply);
            if (!downloadError.isEmpty()) {
                manager->deleteLater();
                QMessageBox::warning(this, tr("wl_cty.dat download failed"),
                                     downloadError);
                return;
            }
            const QByteArray contents = reply->readAll();
            QVector<CtyPrefixEntry> prefixes;
            QString error;
            if (!SupportDataParser::parseCtyDat(
                    QString::fromUtf8(contents), prefixes, &error)) {
                manager->deleteLater();
                QMessageBox::warning(this, tr("Invalid wl_cty.dat file"), error);
                return;
            }
            QString path;
            if (!saveSupportDataFile(QStringLiteral("wl_cty.dat"), contents,
                                     path, error)) {
                manager->deleteLater();
                QMessageBox::warning(this, tr("Unable to save wl_cty.dat"), error);
                return;
            }
            QSettings().setValue(QStringLiteral("supportData/ctyDat"), path);
            if (!storeSupportAsset(QStringLiteral("support/cty"), contents)) {
                manager->deleteLater();
                return;
            }
            loadSupportData();
            manager->deleteLater();
            statusBar()->showMessage(
                tr("Loaded %1 callsign prefixes from wl_cty.dat.").arg(prefixes.size()),
                6000);
        });
    });
    fileMenu->addSeparator();
    auto *exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto *toolbar = addToolBar(tr("Contest"));
    toolbar->setObjectName(QStringLiteral("contestToolbar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addWidget(new QLabel(tr(" Log: "), toolbar));
    m_contestSelector = new QComboBox(this);
    m_contestSelector->setObjectName(QStringLiteral("contestSelector"));
    m_contestSelector->hide();
    connect(m_contestSelector, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::selectContest);
    m_openLogLabel = new QLabel(tr("no log"), toolbar);
    m_openLogLabel->setObjectName(QStringLiteral("openLogLabel"));
    m_openLogLabel->setMinimumWidth(260);
    toolbar->addWidget(m_openLogLabel);
    toolbar->addAction(openLogAction);
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr(" Active radio: "), toolbar));
    m_activeRadioSelector = new QComboBox(toolbar);
    m_activeRadioSelector->setObjectName(QStringLiteral("activeRadioSelector"));
    m_activeRadioSelector->addItem(tr("Radio A"), 0);
    m_activeRadioSelector->addItem(tr("Radio B"), 1);
    connect(m_activeRadioSelector, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
                const int radio = m_activeRadioSelector->currentData().toInt();
                updateRadioSnapshotForRadio(radio, m_radioSnapshots[radio]);
            });
    toolbar->addWidget(m_activeRadioSelector);
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr(" Mode: "), toolbar));
    m_runModeSelector = new QComboBox(toolbar);
    m_runModeSelector->setObjectName(QStringLiteral("runModeSelector"));
    m_runModeSelector->addItem(tr("Run"), 0);
    m_runModeSelector->addItem(tr("Search & Pounce"), 1);
    m_runModeSelector->setCurrentIndex(m_runModeSelector->findData(savedRunMode));
    connect(m_runModeSelector, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { setRunMode(m_runModeSelector->currentData().toInt()); });
    toolbar->addWidget(m_runModeSelector);
    m_so2vToggle = new QCheckBox(tr("SO2V (dual VFO)"), toolbar);
    m_so2vToggle->setObjectName(QStringLiteral("so2vToggle"));
    m_so2vToggle->setToolTip(tr("Two VFOs of one connected radio, independent QSO entry."));
    connect(m_so2vToggle, &QCheckBox::toggled,
            this, &MainWindow::setSo2vEnabled);
    m_so2rToggle = new QCheckBox(tr("SO2R (two radios)"), toolbar);
    m_so2rToggle->setObjectName(QStringLiteral("so2rToggle"));
    m_so2rToggle->setToolTip(tr("Two connected radios, one QSO entry per radio."));
    connect(m_so2rToggle, &QCheckBox::toggled,
            this, &MainWindow::setSo2rEnabled);
    toolbar->addSeparator();
    toolbar->addWidget(m_so2vToggle);
    toolbar->addWidget(m_so2rToggle);

    m_stationLabel = new QLabel(this);
    m_stationLabel->setObjectName(QStringLiteral("stationSummary"));
    m_radioLabel = new QLabel(tr("Radio disconnected"), this);
    m_radioLabel->setObjectName(QStringLiteral("radioStatus"));
    m_clusterLabel = new QLabel(tr("DX cluster disconnected"), this);
    m_clusterLabel->setObjectName(QStringLiteral("clusterStatus"));
    statusBar()->addPermanentWidget(m_stationLabel);
    statusBar()->addPermanentWidget(m_radioLabel);
    statusBar()->addPermanentWidget(m_clusterLabel);

    // Central "log window": yellow status line, log grid, entry lines,
    // information line and function-key strip, following the DXLog layout.
    auto *central = new QFrame(this);
    central->setObjectName(QStringLiteral("logWindow"));
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    m_topStatus = new QLabel(central);
    m_topStatus->setObjectName(QStringLiteral("topStatus"));
    m_topStatus->setFont(monoFont(11, true));
    m_topStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(m_topStatus);

    auto *logFilters = new QHBoxLayout;
    logFilters->setContentsMargins(0, 0, 0, 0);
    logFilters->setSpacing(4);
    m_logSearchEdit = new QLineEdit(central);
    m_logSearchEdit->setObjectName(QStringLiteral("logSearchInput"));
    m_logSearchEdit->setClearButtonEnabled(true);
    m_logSearchEdit->setPlaceholderText(
        tr("Search callsign or exchange"));
    m_logBandFilter = new QComboBox(central);
    m_logBandFilter->setObjectName(QStringLiteral("logBandFilter"));
    m_logModeFilter = new QComboBox(central);
    m_logModeFilter->setObjectName(QStringLiteral("logModeFilter"));
    m_logValidityFilter = new QComboBox(central);
    m_logValidityFilter->setObjectName(QStringLiteral("logValidityFilter"));
    m_logValidityFilter->addItem(tr("All QSOs"), QStringLiteral("all"));
    m_logValidityFilter->addItem(tr("Valid"), QStringLiteral("valid"));
    m_logValidityFilter->addItem(tr("Invalid"), QStringLiteral("invalid"));
    m_logFilterResultLabel = new QLabel(central);
    m_logFilterResultLabel->setObjectName(QStringLiteral("logFilterResultCount"));
    m_logFilterResultLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    logFilters->addWidget(m_logSearchEdit, 2);
    logFilters->addWidget(m_logBandFilter);
    logFilters->addWidget(m_logModeFilter);
    logFilters->addWidget(m_logValidityFilter);
    logFilters->addWidget(m_logFilterResultLabel, 1);
    layout->addLayout(logFilters);

    m_logFilterTimer = new QTimer(this);
    m_logFilterTimer->setSingleShot(true);
    m_logFilterTimer->setInterval(150);
    connect(m_logFilterTimer, &QTimer::timeout,
            this, &MainWindow::refreshRecentQsoTable);
    connect(m_logSearchEdit, &QLineEdit::textChanged,
            m_logFilterTimer, qOverload<>(&QTimer::start));
    const auto refreshLogFilters = [this] {
        refreshRecentQsoTable();
    };
    connect(m_logBandFilter, &QComboBox::currentIndexChanged,
            this, refreshLogFilters);
    connect(m_logModeFilter, &QComboBox::currentIndexChanged,
            this, refreshLogFilters);
    connect(m_logValidityFilter, &QComboBox::currentIndexChanged,
            this, refreshLogFilters);

    m_recentQsos = new QTableWidget(0, 10, central);
    m_recentQsos->setObjectName(QStringLiteral("recentQsos"));
    m_recentQsos->setHorizontalHeaderLabels(
        {tr("Nr"), tr("UTC"), tr("Band"), tr("Mode"), tr("kHz"), tr("Call"),
         tr("Snt"), tr("Rcv"), tr("Mult"), tr("Pts")});
    m_recentQsos->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentQsos->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recentQsos->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentQsos->setAlternatingRowColors(false);
    m_recentQsos->setShowGrid(false);
    m_recentQsos->setWordWrap(false);
    m_recentQsos->setFocusPolicy(Qt::ClickFocus);
    m_recentQsos->setFont(monoFont(11, true));
    m_recentQsos->verticalHeader()->hide();
    m_recentQsos->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_recentQsos->verticalHeader()->setDefaultSectionSize(
        QFontMetrics(monoFont(11, true)).height() + 2);
    m_recentQsos->horizontalHeader()->setHighlightSections(false);
    m_recentQsos->horizontalHeader()->setStyleSheet(QStringLiteral(
        "QHeaderView { background: #20B2AA; border: none; }"
        "QHeaderView::section { background: #20B2AA; color: #FFFF00; border: none;"
        " border-bottom: 1px solid #178F89; padding: 1px 4px; font-weight: bold; }"));
    m_recentQsos->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_recentQsos->horizontalHeader()->setStretchLastSection(false);
    m_recentQsos->setToolTip(
        tr("Double-click a QSO to edit it. Right-click to edit, invalidate, or restore."));
    connect(m_recentQsos, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::editSelectedQso);
    m_recentQsos->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_recentQsos, &QTableWidget::customContextMenuRequested,
            this, [this](const QPoint &position) {
        const int row = m_recentQsos->rowAt(position.y());
        if (row < 0 || m_recentQsos->item(row, 0) == nullptr) {
            return;
        }
        m_recentQsos->setCurrentCell(row, 0);
        QString error;
        const auto qso = m_repository.qsoById(
            m_recentQsos->item(row, 0)->data(Qt::UserRole).toLongLong(), &error);
        if (!error.isEmpty() || !qso.has_value()) {
            QMessageBox::critical(
                this, tr("Unable to load QSO"),
                error.isEmpty() ? tr("The QSO no longer exists.") : error);
            return;
        }
        QMenu menu(this);
        QAction *editAction = menu.addAction(tr("Edit QSO..."));
        QAction *invalidAction = menu.addAction(
            qso->invalid ? tr("Restore QSO") : tr("Mark QSO invalid"));
        QAction *deleteAction = menu.addAction(tr("Delete QSO..."));
        QAction *selected = menu.exec(m_recentQsos->viewport()->mapToGlobal(position));
        if (selected == editAction) {
            editSelectedQso(row, 0);
        } else if (selected == invalidAction) {
            setSelectedQsoInvalid(row, !qso->invalid);
        } else if (selected == deleteAction) {
            deleteSelectedQso(row);
        }
    });
    layout->addWidget(m_recentQsos, 1);

    auto *entryContexts = new QHBoxLayout;
    entryContexts->setSpacing(4);
    for (int vfo = 0; vfo < 2; ++vfo) {
        auto *entryPanel = new QFrame(central);
        entryPanel->setObjectName(QStringLiteral("entryPanel"));
        entryPanel->setProperty("active", vfo == 0);
        m_entryGroups[vfo] = entryPanel;
        auto *entryLayout = new QVBoxLayout(entryPanel);
        entryLayout->setContentsMargins(0, 0, 0, 4);
        entryLayout->setSpacing(3);

        auto *headerRow = new QHBoxLayout;
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(0);
        m_entryHeaders[vfo] = new QLabel(entryPanel);
        m_entryHeaders[vfo]->setObjectName(QStringLiteral("entryHeader"));
        m_entryHeaders[vfo]->setFont(monoFont(10, true));
        m_focusButtons[vfo] = new QPushButton(
            vfo == 0 ? tr("RX focus A") : tr("RX focus B"), entryPanel);
        m_focusButtons[vfo]->setObjectName(
            vfo == 0 ? QStringLiteral("focusVfoA") : QStringLiteral("focusVfoB"));
        m_focusButtons[vfo]->setFocusPolicy(Qt::NoFocus);
        headerRow->addWidget(m_entryHeaders[vfo], 1);
        headerRow->addWidget(m_focusButtons[vfo]);
        entryLayout->addLayout(headerRow);

        auto *fieldsRow = new QHBoxLayout;
        fieldsRow->setContentsMargins(6, 0, 6, 0);
        fieldsRow->setSpacing(6);
        m_bandSelectors[vfo] = new QComboBox(entryPanel);
        m_bandSelectors[vfo]->setObjectName(
            vfo == 0 ? QStringLiteral("bandSelectorA")
                     : QStringLiteral("bandSelectorB"));
        m_bandSelectors[vfo]->setEditable(true);
        m_bandSelectors[vfo]->addItems(defaultBands());
        m_bandSelectors[vfo]->setFont(monoFont(11, true));
        m_bandSelectors[vfo]->setFixedWidth(84);
        m_modeSelectors[vfo] = new QComboBox(entryPanel);
        m_modeSelectors[vfo]->setObjectName(
            vfo == 0 ? QStringLiteral("modeSelectorA")
                     : QStringLiteral("modeSelectorB"));
        m_modeSelectors[vfo]->setEditable(true);
        m_modeSelectors[vfo]->addItems(defaultModes());
        m_modeSelectors[vfo]->setFont(monoFont(11, true));
        m_modeSelectors[vfo]->setFixedWidth(84);
        m_callsignEdits[vfo] = new QLineEdit(entryPanel);
        m_callsignEdits[vfo]->setObjectName(
            vfo == 0 ? QStringLiteral("callsignInputA")
                     : QStringLiteral("callsignInputB"));
        m_callsignEdits[vfo]->setMaxLength(20);
        m_callsignEdits[vfo]->setMinimumWidth(190);
        m_callsignEdits[vfo]->setFont(monoFont(18, true));
        m_callsignEdits[vfo]->installEventFilter(this);
        installUppercase(m_callsignEdits[vfo]);
        m_callCompleterModels[vfo] = new QStringListModel(this);
        m_callCompleters[vfo] = new QCompleter(m_callCompleterModels[vfo], this);
        m_callCompleters[vfo]->setCaseSensitivity(Qt::CaseInsensitive);
        m_callCompleters[vfo]->setCompletionMode(QCompleter::PopupCompletion);
        m_callCompleters[vfo]->setFilterMode(Qt::MatchStartsWith);
        m_callCompleters[vfo]->setMaxVisibleItems(12);
        m_callsignEdits[vfo]->setCompleter(m_callCompleters[vfo]);
        fieldsRow->addWidget(fieldColumn(tr("Band"), m_bandSelectors[vfo], entryPanel));
        fieldsRow->addWidget(fieldColumn(tr("Mode"), m_modeSelectors[vfo], entryPanel));
        fieldsRow->addWidget(fieldColumn(tr("Call"), m_callsignEdits[vfo], entryPanel), 3);
        m_sentExchangeLayouts[vfo] = new QHBoxLayout;
        m_sentExchangeLayouts[vfo]->setSpacing(4);
        fieldsRow->addLayout(m_sentExchangeLayouts[vfo]);
        m_exchangeLayouts[vfo] = new QHBoxLayout;
        m_exchangeLayouts[vfo]->setSpacing(4);
        fieldsRow->addLayout(m_exchangeLayouts[vfo], 2);
        m_dupeLabels[vfo] = new QLabel(entryPanel);
        m_dupeLabels[vfo]->setObjectName(QStringLiteral("dupeBadge"));
        m_dupeLabels[vfo]->setFont(monoFont(11, true));
        m_dupeLabels[vfo]->setAlignment(Qt::AlignCenter);
        m_dupeLabels[vfo]->setMinimumWidth(96);
        auto *logButton = new QPushButton(tr("Log ↵"), entryPanel);
        logButton->setObjectName(vfo == 0 ? QStringLiteral("logQsoA")
                                          : QStringLiteral("logQsoB"));
        logButton->setFocusPolicy(Qt::NoFocus);
        fieldsRow->addWidget(fieldColumn(QString(), m_dupeLabels[vfo], entryPanel));
        fieldsRow->addWidget(fieldColumn(QString(), logButton, entryPanel));
        entryLayout->addLayout(fieldsRow);

        auto *infoRow = new QHBoxLayout;
        infoRow->setContentsMargins(6, 0, 6, 0);
        m_callInfoLabels[vfo] = new QLabel(entryPanel);
        m_callInfoLabels[vfo]->setObjectName(
            vfo == 0 ? QStringLiteral("callInfoA") : QStringLiteral("callInfoB"));
        m_callInfoLabels[vfo]->setFont(monoFont(10, true));
        m_callInfoLabels[vfo]->setMinimumHeight(20);
        m_workedBandLabels[vfo] = new QLabel(entryPanel);
        m_workedBandLabels[vfo]->setObjectName(QStringLiteral("workedBands"));
        m_workedBandLabels[vfo]->setFont(monoFont(10, true));
        m_workedBandLabels[vfo]->setTextFormat(Qt::RichText);
        m_workedBandLabels[vfo]->setToolTip(
            tr("Bands on which this callsign is already in the log "
               "(red = dupe on the current band and mode)."));
        infoRow->addWidget(m_callInfoLabels[vfo], 1);
        infoRow->addWidget(m_workedBandLabels[vfo]);
        entryLayout->addLayout(infoRow);
        entryContexts->addWidget(entryPanel, 1);

        connect(m_callsignEdits[vfo], &QLineEdit::returnPressed,
                this, [this, vfo] { handleEntryEnter(vfo, true); });
        connect(logButton, &QPushButton::clicked,
                this, [this, vfo] { logQsoForVfo(vfo); });
        connect(m_callsignEdits[vfo], &QLineEdit::textChanged,
                this, [this, vfo](const QString &text) {
                    if (!text.isEmpty()) {
                        stopAutoCq();
                    }
                    updateCallLookup(vfo);
                });
        connect(m_callsignEdits[vfo], &QLineEdit::textChanged,
                this, [this, vfo] { updateDupeIndicatorForVfo(vfo); });
        connect(m_bandSelectors[vfo], &QComboBox::currentTextChanged,
                this, [this, vfo] {
                    updateDupeIndicatorForVfo(vfo);
                    updateEntryHeaders();
                    updateSerialFields();
                    if (vfo == currentEntryContext() || m_so2vToggle->isChecked()
                        || m_so2rToggle->isChecked()) {
                        refreshBandmap();
                        refreshScoreSummary();
                    }
                    if (vfo == currentEntryContext()) {
                        updateNetworkOperatingState();
                    }
                });
        connect(m_modeSelectors[vfo], &QComboBox::currentTextChanged,
                this, [this, vfo] {
                    if (m_modeSelectors[vfo]->currentText().compare(
                            QStringLiteral("CW"), Qt::CaseInsensitive) != 0) {
                        stopAutoCq();
                    }
                    updateDupeIndicatorForVfo(vfo);
                    updateSentReportForMode(vfo);
                    updateEntryHeaders();
                    updateSerialFields();
                    if (vfo == currentEntryContext()) {
                        updateNetworkOperatingState();
                    }
                });
        connect(m_focusButtons[vfo], &QPushButton::clicked,
                this, [this, vfo] { focusVfo(vfo); });
    }
    m_entryGroups[1]->hide();
    layout->addLayout(entryContexts);

    m_messageLine = new QLabel(central);
    m_messageLine->setObjectName(QStringLiteral("messageLine"));
    m_messageLine->setFont(monoFont(10, true));
    m_messageLine->setAlignment(Qt::AlignCenter);
    m_messageLine->setMinimumHeight(QFontMetrics(monoFont(10, true)).height() + 6);
    layout->addWidget(m_messageLine);
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    connect(m_messageTimer, &QTimer::timeout, this, [this] {
        m_messageLine->clear();
        m_messageLine->setProperty("alert", false);
        repolish(m_messageLine);
    });
    m_autoCqTimer = new QTimer(this);
    m_autoCqTimer->setSingleShot(true);
    m_autoCqTimer->setInterval(5000);
    connect(m_autoCqTimer, &QTimer::timeout, this, [this] {
        const int context = currentEntryContext();
        if (!m_autoCqEnabled || m_runModeSelector->currentData().toInt() != 0
            || m_modeSelectors[context]->currentText().compare(
                   QStringLiteral("CW"), Qt::CaseInsensitive) != 0
            || !m_callsignEdits[context]->text().trimmed().isEmpty()
            || m_cwStatus.state != ConnectionState::Connected
            || m_cwStatus.transmitting || m_cwStatus.queuedMessages != 0) {
            return;
        }
        sendFunctionKeyForContext(0, context);
    });

    auto *functionKeyBar = new QFrame(central);
    functionKeyBar->setObjectName(QStringLiteral("fkeyBar"));
    auto *functionKeyLayout = new QHBoxLayout(functionKeyBar);
    functionKeyLayout->setContentsMargins(0, 0, 0, 0);
    functionKeyLayout->setSpacing(3);
    for (int key = 0; key < 8; ++key) {
        auto *button = new QPushButton(functionKeyBar);
        button->setObjectName(QStringLiteral("functionKey%1").arg(key + 1));
        button->setFocusPolicy(Qt::NoFocus);
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::clicked, this, [this, key] { sendFunctionKey(key); });
        connect(button, &QPushButton::customContextMenuRequested,
                this, [this, key] { editFunctionKey(key); });
        m_functionKeyButtons[key] = button;
        functionKeyLayout->addWidget(button, 1);
    }
    layout->addWidget(functionKeyBar);
    setCentralWidget(central);

    // Contest information.
    m_activeContestLabel = new QLabel(tr("Select a log to begin."));
    m_activeContestLabel->setObjectName(QStringLiteral("activeContestSummary"));
    m_activeContestLabel->setWordWrap(true);
    m_activeContestLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_activeContestLabel->setMargin(6);
    auto *contestInfoScroll = new QScrollArea;
    contestInfoScroll->setObjectName(QStringLiteral("contestInfoScroll"));
    contestInfoScroll->setWidgetResizable(true);
    contestInfoScroll->setFrameShape(QFrame::NoFrame);
    contestInfoScroll->setWidget(m_activeContestLabel);
    auto *contestInfoPane = createPane(QStringLiteral("contestInfoPane"));
    contestInfoPane->layout()->addWidget(contestInfoScroll);
    m_contestInfoDock = createPaneDock(tr("Contest information"),
                                       QStringLiteral("contestInfoDock"),
                                       contestInfoPane);

    m_cwInputWindow = new QWidget(this, Qt::Tool);
    m_cwInputWindow->setObjectName(QStringLiteral("cwInputWindow"));
    m_cwInputWindow->setWindowTitle(tr("Live CW input"));
    auto *cwLayout = new QVBoxLayout(m_cwInputWindow);
    m_cwMessageEdit = new QLineEdit(m_cwInputWindow);
    m_cwMessageEdit->setObjectName(QStringLiteral("cwMessageInput"));
    m_cwMessageEdit->setPlaceholderText(tr("Type to transmit on the active radio/VFO"));
    m_cwMessageEdit->setFont(monoFont(10, true));
    QSettings cwSettings;
    m_cwConfiguredRadio = -1;
    m_cwWpm = qBound(5, cwSettings.value(QStringLiteral("cw/wpm"), 25).toInt(), 80);
    m_cwStatusLabel = new QLabel(tr("CW keyer disconnected"), m_cwInputWindow);
    m_cwStatusLabel->setObjectName(QStringLiteral("cwKeyerStatus"));
    m_cwStatusLabel->setWordWrap(true);
    cwLayout->addWidget(m_cwMessageEdit);
    cwLayout->addWidget(new QLabel(
        tr("Esc: stop CW; Esc when idle: hide. PageUp/PageDown: speed.\n"
           "Text is sent immediately; Enter clears the input for a new line."),
        m_cwInputWindow));
    cwLayout->addWidget(m_cwStatusLabel);
    m_cwInputWindow->resize(600, 140);
    m_cwInputWindow->addActions(
        {escapeCwAction, abortCwAction, cwInputAction, cwFasterAction, cwSlowerAction});
    connect(m_cwMessageEdit, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (!text.startsWith(m_cwTypedText)) {
            m_cwMessageEdit->setText(m_cwTypedText);
            showOperatorMessage(tr("CW text is already queued. Press Esc to abort."), true);
            return;
        }
        const QString added = text.mid(m_cwTypedText.size());
        if (added.isEmpty()) {
            return;
        }
        QString problem;
        const int context = currentEntryContext();
        if (!prepareCwTransmission(context, &problem)) {
            m_cwMessageEdit->setText(m_cwTypedText);
            showOperatorMessage(problem, true);
            m_cwStatusLabel->setText(problem);
            return;
        }
        stopAutoCq();
        const int radio = cwRadioForContext(context);
        ++m_cwEnqueuedRequests;
        m_cwKeyerController.enqueueText(
            added.toUpper(), qBound(0, m_radioSnapshots[radio].focusVfo, 1));
        m_cwTypedText = text;
    });
    connect(m_cwMessageEdit, &QLineEdit::returnPressed,
            this, [this] {
                m_cwTypedText.clear();
                m_cwMessageEdit->clear();
            });

    // Check callsign (worked band/mode matrix for the current call).
    auto *checkCallPane = createPane(QStringLiteral("checkCallPane"));
    m_checkCallGrid = new QTableWidget(checkCallPane);
    m_checkCallGrid->setObjectName(QStringLiteral("checkCallGrid"));
    m_checkCallGrid->setFont(monoFont(10, true));
    m_checkCallGrid->verticalHeader()->hide();
    m_checkCallGrid->verticalHeader()->setDefaultSectionSize(
        QFontMetrics(monoFont(10, true)).height() + 4);
    m_checkCallGrid->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_checkCallGrid->horizontalHeader()->setHighlightSections(false);
    m_checkCallGrid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_checkCallGrid->setSelectionMode(QAbstractItemView::NoSelection);
    m_checkCallGrid->setFocusPolicy(Qt::NoFocus);
    m_checkCallGrid->setShowGrid(false);
    m_checkCallGrid->setToolTip(
        tr("A time means the callsign was worked; +M means this station would add a new multiplier."));
    m_checkCallTitle = new QLabel(checkCallPane);
    m_checkCallTitle->setObjectName(QStringLiteral("checkCallTitle"));
    m_checkCallTitle->setFont(monoFont(10, true));
    m_checkCallTitle->setWordWrap(true);
    checkCallPane->layout()->addWidget(m_checkCallGrid);
    checkCallPane->layout()->addWidget(m_checkCallTitle);
    m_checkCallDock = createPaneDock(tr("Check callsign"),
                                     QStringLiteral("checkCallDock"), checkCallPane);

    // Check partial (super check partial from master.cpy, call history and log).
    auto *checkPartialPane = createPane(QStringLiteral("checkPartialPane"));
    m_checkPartial = new QListWidget(checkPartialPane);
    m_checkPartial->setObjectName(QStringLiteral("checkPartial"));
    m_checkPartial->setFont(monoFont(11, true));
    m_checkPartial->setFlow(QListView::LeftToRight);
    m_checkPartial->setWrapping(true);
    m_checkPartial->setResizeMode(QListView::Adjust);
    m_checkPartial->setSpacing(2);
    m_checkPartial->setFocusPolicy(Qt::NoFocus);
    m_checkPartial->setToolTip(
        tr("White: not in log · green: worked on another band/mode · "
           "red: dupe · bold: exact match. Click to copy into the entry line."));
    connect(m_checkPartial, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                const int context = currentEntryContext();
                m_callsignEdits[context]->setText(item->text());
                m_callsignEdits[context]->setFocus();
            });
    checkPartialPane->layout()->addWidget(m_checkPartial);
    m_checkPartialDock = createPaneDock(tr("Check partial"),
                                        QStringLiteral("checkPartialDock"),
                                        checkPartialPane);

    // Bandmap (spots for the active band).
    auto *bandmapPane = createPane(QStringLiteral("bandmapPane"));
    auto *bandmapControls = new QHBoxLayout;
    bandmapControls->setContentsMargins(2, 2, 2, 0);
    m_bandmapBandFilter = new QComboBox(bandmapPane);
    m_bandmapBandFilter->setObjectName(QStringLiteral("bandmapBandFilter"));
    m_bandmapBandFilter->addItem(tr("Active band"), QStringLiteral("@active"));
    m_bandmapBandFilter->addItem(tr("All bands"), QString());
    for (const QString &band : defaultBands()) {
        m_bandmapBandFilter->addItem(band, band);
    }
    m_bandmapModeFilter = new QComboBox(bandmapPane);
    m_bandmapModeFilter->setObjectName(QStringLiteral("bandmapModeFilter"));
    m_bandmapModeFilter->addItem(tr("All modes"), QString());
    for (const QString &mode : {QStringLiteral("CW"), QStringLiteral("SSB"),
                                QStringLiteral("RTTY"), QStringLiteral("FT8"),
                                QStringLiteral("FT4"), QStringLiteral("PSK"),
                                QStringLiteral("FM"), QStringLiteral("AM")}) {
        m_bandmapModeFilter->addItem(mode, mode);
    }
    m_bandmapContinentFilter = new QComboBox(bandmapPane);
    m_bandmapContinentFilter->setObjectName(QStringLiteral("bandmapContinentFilter"));
    m_bandmapContinentFilter->addItem(tr("All continents"), QString());
    for (const QString &continent : {QStringLiteral("AF"), QStringLiteral("AN"),
                                     QStringLiteral("AS"), QStringLiteral("EU"),
                                     QStringLiteral("NA"), QStringLiteral("OC"),
                                     QStringLiteral("SA")}) {
        m_bandmapContinentFilter->addItem(continent, continent);
    }
    m_bandmapContinentFilter->addItem(tr("Unknown"), QStringLiteral("@unknown"));
    m_onlyNewMultiplierSpots =
        new QCheckBox(tr("New mult only"), bandmapPane);
    m_onlyNewMultiplierSpots->setObjectName(
        QStringLiteral("bandmapNewMultiplierFilter"));
    m_onlyNewMultiplierSpots->setToolTip(
        tr("Show only spots that can add a multiplier under the active contest rules."));
    auto *bandmapSortSelector = new QComboBox(bandmapPane);
    bandmapSortSelector->setObjectName(QStringLiteral("bandmapSortSelector"));
    bandmapSortSelector->addItem(tr("Frequency"), QStringLiteral("frequency"));
    bandmapSortSelector->addItem(tr("Newest first"), QStringLiteral("time"));
    bandmapSortSelector->setToolTip(
        tr("Sort spots by frequency or by time, newest first."));
    m_hideWorkedSpots = new QCheckBox(tr("Hide worked"), bandmapPane);
    m_hideWorkedSpots->setChecked(true);
    auto *tuneSelectedSpot = new QPushButton(tr("Tune"), bandmapPane);
    tuneSelectedSpot->setObjectName(QStringLiteral("tuneSelectedSpotButton"));
    bandmapControls->addWidget(m_bandmapBandFilter, 1);
    bandmapControls->addWidget(m_bandmapModeFilter);
    bandmapControls->addWidget(m_bandmapContinentFilter);
    bandmapControls->addWidget(bandmapSortSelector);
    bandmapControls->addWidget(m_hideWorkedSpots);
    bandmapControls->addWidget(tuneSelectedSpot);
    static_cast<QVBoxLayout *>(bandmapPane->layout())->addLayout(bandmapControls);
    auto *multiplierFilterLayout = new QHBoxLayout;
    multiplierFilterLayout->setContentsMargins(2, 0, 2, 0);
    multiplierFilterLayout->addWidget(m_onlyNewMultiplierSpots);
    multiplierFilterLayout->addStretch();
    static_cast<QVBoxLayout *>(bandmapPane->layout())->addLayout(
        multiplierFilterLayout);
    m_bandmap = new QTableWidget(0, 6, bandmapPane);
    m_bandmap->setObjectName(QStringLiteral("dxBandmap"));
    m_bandmap->setHorizontalHeaderLabels(
        {tr("kHz"), tr("Call"), tr("Age"), tr("Spotter"), tr("Source"),
         tr("Comment")});
    m_bandmap->setFont(monoFont(10, true));
    m_bandmap->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bandmap->setSelectionMode(QAbstractItemView::SingleSelection);
    m_bandmap->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bandmap->setShowGrid(false);
    m_bandmap->setWordWrap(false);
    m_bandmap->verticalHeader()->hide();
    m_bandmap->verticalHeader()->setDefaultSectionSize(
        QFontMetrics(monoFont(10, true)).height() + 2);
    m_bandmap->horizontalHeader()->setHighlightSections(false);
    m_bandmap->horizontalHeader()->setStretchLastSection(true);
    m_bandmap->setFocusPolicy(Qt::StrongFocus);
    m_bandmap->installEventFilter(this);
    m_bandmap->setToolTip(
        tr("Ctrl+Up/Down selects a spot, copies its callsign and tunes the active radio; Enter repeats it. "
           "Orange = spot nearest the active radio frequency; grey = already worked."));
    connect(m_bandmap, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::activateSpot);
    connect(tuneSelectedSpot, &QPushButton::clicked, this, [this] {
        activateSpot(m_bandmap->currentRow(), 0);
    });
    connect(m_bandmapBandFilter, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshBandmap);
    connect(m_bandmapModeFilter, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshBandmap);
    connect(m_bandmapContinentFilter, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshBandmap);
    connect(bandmapSortSelector, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshBandmap);
    connect(m_hideWorkedSpots, &QCheckBox::toggled,
            this, &MainWindow::refreshBandmap);
    connect(m_onlyNewMultiplierSpots, &QCheckBox::toggled,
            this, &MainWindow::refreshBandmap);
    bandmapPane->layout()->addWidget(m_bandmap);
    m_bandmapDock = createPaneDock(tr("Bandmap"), QStringLiteral("dxBandmapDock"),
                                   bandmapPane);

    auto *radioABandmapPane = createPane(QStringLiteral("radioABandmapPane"));
    m_radioABandmap = new QTableWidget(0, 6, radioABandmapPane);
    m_radioABandmap->setObjectName(QStringLiteral("dxBandmapRadioA"));
    m_radioABandmap->setHorizontalHeaderLabels(
        {tr("kHz"), tr("Call"), tr("Age"), tr("Spotter"), tr("Source"),
         tr("Comment")});
    m_radioABandmap->setFont(monoFont(10, true));
    m_radioABandmap->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_radioABandmap->setSelectionMode(QAbstractItemView::SingleSelection);
    m_radioABandmap->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_radioABandmap->setShowGrid(false);
    m_radioABandmap->setWordWrap(false);
    m_radioABandmap->verticalHeader()->hide();
    m_radioABandmap->verticalHeader()->setDefaultSectionSize(
        QFontMetrics(monoFont(10, true)).height() + 2);
    m_radioABandmap->horizontalHeader()->setHighlightSections(false);
    m_radioABandmap->horizontalHeader()->setStretchLastSection(true);
    m_radioABandmap->setFocusPolicy(Qt::StrongFocus);
    m_radioABandmap->installEventFilter(this);
    m_radioABandmap->setToolTip(
        tr("Radio A spots. Enter or double-click to copy the callsign and tune Radio A."));
    connect(m_radioABandmap, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                if (const QTableWidgetItem *item = m_radioABandmap->item(row, 0)) {
                    const int radio = m_so2rToggle->isChecked()
                        ? 0 : m_activeRadioSelector->currentData().toInt();
                    activateSpotId(item->data(Qt::UserRole).toLongLong(), radio, 0);
                }
            });
    radioABandmapPane->layout()->addWidget(m_radioABandmap);
    m_radioABandmapDock = createPaneDock(
        tr("Bandmap — Radio A"), QStringLiteral("radioABandmapDock"),
        radioABandmapPane);

    // DX cluster announcements (raw, chronological).
    auto *clusterPane = createPane(QStringLiteral("clusterPane"));
    auto *clusterCommandLayout = new QHBoxLayout;
    clusterCommandLayout->setContentsMargins(2, 2, 2, 0);
    m_clusterConnectionSelector = new QComboBox(clusterPane);
    m_clusterConnectionSelector->setObjectName(
        QStringLiteral("dxClusterConnectionSelector"));
    m_clusterConnectionSelector->addItem(tr("No connection"), -1);
    m_clusterCommandInput = new QLineEdit(clusterPane);
    m_clusterCommandInput->setObjectName(QStringLiteral("dxClusterCommandInput"));
    m_clusterCommandInput->setPlaceholderText(
        tr("Cluster command (e.g. DX W1AW 14025.0 comment)"));
    m_clusterCommandInput->setMaxLength(512);
    auto *sendClusterCommand = new QPushButton(tr("Send"), clusterPane);
    sendClusterCommand->setObjectName(QStringLiteral("sendDxClusterCommand"));
    clusterCommandLayout->addWidget(m_clusterConnectionSelector);
    clusterCommandLayout->addWidget(m_clusterCommandInput, 1);
    clusterCommandLayout->addWidget(sendClusterCommand);
    static_cast<QVBoxLayout *>(clusterPane->layout())->addLayout(clusterCommandLayout);
    connect(sendClusterCommand, &QPushButton::clicked,
            this, &MainWindow::sendDxClusterCommand);
    connect(m_clusterCommandInput, &QLineEdit::returnPressed,
            this, &MainWindow::sendDxClusterCommand);
    m_clusterLog = new QTableWidget(0, 6, clusterPane);
    m_clusterLog->setObjectName(QStringLiteral("clusterLog"));
    m_clusterLog->setHorizontalHeaderLabels(
        {tr("UTC"), tr("kHz"), tr("Call"), tr("Spotter"), tr("Source"),
         tr("Comment")});
    m_clusterLog->setFont(monoFont(9, false));
    m_clusterLog->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_clusterLog->setSelectionMode(QAbstractItemView::SingleSelection);
    m_clusterLog->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_clusterLog->setShowGrid(false);
    m_clusterLog->setWordWrap(false);
    m_clusterLog->verticalHeader()->hide();
    m_clusterLog->verticalHeader()->setDefaultSectionSize(
        QFontMetrics(monoFont(9, false)).height() + 2);
    m_clusterLog->horizontalHeader()->setHighlightSections(false);
    m_clusterLog->horizontalHeader()->setStretchLastSection(true);
    m_clusterLog->setToolTip(tr("Double-click a spot to copy and tune it."));
    connect(m_clusterLog, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (const QTableWidgetItem *item = m_clusterLog->item(row, 0)) {
            activateSpotId(item->data(Qt::UserRole).toLongLong());
        }
    });
    clusterPane->layout()->addWidget(m_clusterLog);
    m_clusterDock = createPaneDock(tr("DX Cluster announcements"),
                                   QStringLiteral("dxClusterDock"), clusterPane);

    // Greyline (day/night terminator with the QTH and the entered callsign).
    auto *greylinePane = createPane(QStringLiteral("greylinePane"));
    m_greylineWidget = new GreylineWidget(
        [this]() -> std::optional<QPointF> {
            double lat = 0.0;
            double lon = 0.0;
            if (!SupportDataParser::maidenheadToLatLon(m_station.grid, lat, lon)) {
                return std::nullopt;
            }
            return QPointF(lon, lat);
        },
        [this]() -> std::optional<QPointF> {
            const QString call =
                m_callsignEdits[currentEntryContext()]->text().trimmed().toUpper();
            if (call.isEmpty()) {
                return std::nullopt;
            }
            const CtyPrefixEntry *entry =
                SupportDataParser::findCtyEntry(call, m_ctyPrefixes);
            if (entry == nullptr || !entry->hasLocation) {
                return std::nullopt;
            }
            return QPointF(entry->longitude, entry->latitude);
        },
        greylinePane);
    m_greylineWidget->setObjectName(QStringLiteral("greylineMap"));
    greylinePane->layout()->addWidget(m_greylineWidget);
    m_greylineDock = createPaneDock(tr("Greyline"), QStringLiteral("greylineDock"),
                                    greylinePane);
    for (int vfo = 0; vfo < 2; ++vfo) {
        connect(m_callsignEdits[vfo], &QLineEdit::textChanged,
                m_greylineWidget, qOverload<>(&QWidget::update));
    }

    auto *networkStationsPane = createPane(QStringLiteral("networkStationsPane"));
    m_networkStationsStatus = new QLabel(networkStationsPane);
    m_networkStationsStatus->setObjectName(QStringLiteral("networkStationsStatus"));
    m_networkStationsStatus->setWordWrap(true);
    networkStationsPane->layout()->addWidget(m_networkStationsStatus);
    m_networkStationsTable = new QTableWidget(0, 4, networkStationsPane);
    m_networkStationsTable->setObjectName(QStringLiteral("networkStationsTable"));
    m_networkStationsTable->setHorizontalHeaderLabels(
        {tr("Station"), tr("Band"), tr("Mode"), tr("State")});
    m_networkStationsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkStationsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_networkStationsTable->setShowGrid(false);
    m_networkStationsTable->verticalHeader()->hide();
    m_networkStationsTable->horizontalHeader()->setStretchLastSection(true);
    networkStationsPane->layout()->addWidget(m_networkStationsTable);
    m_networkStationsDock = createPaneDock(
        tr("Network stations"), QStringLiteral("networkStationsDock"),
        networkStationsPane);
    m_networkStationsTimer = new QTimer(this);
    m_networkStationsTimer->setInterval(5000);
    connect(m_networkStationsTimer, &QTimer::timeout,
            this, &MainWindow::refreshNetworkStations);
    connect(m_networkStationsDock, &QDockWidget::visibilityChanged,
            this, [this](bool visible) {
                if (visible) {
                    refreshNetworkStations();
                }
            });
    m_networkStationsTimer->start();

    // Rate.
    auto *ratePane = createPane(QStringLiteral("ratePane"));
    m_rateLabel = new QLabel(ratePane);
    m_rateLabel->setObjectName(QStringLiteral("rateText"));
    m_rateLabel->setFont(monoFont(10, true));
    m_rateLabel->setTextFormat(Qt::PlainText);
    m_rateLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_rateLabel->setMargin(4);
    ratePane->layout()->addWidget(m_rateLabel);
    auto *rateChart = new RatePaceChart(ratePane);
    m_rateChart = rateChart;
    ratePane->layout()->addWidget(rateChart);
    m_rateDock = createPaneDock(tr("Rate"), QStringLiteral("rateDock"), ratePane);

    // Summary.
    auto *summaryPane = createPane(QStringLiteral("summaryPane"));
    m_summaryTable = new QTableWidget(summaryPane);
    m_summaryTable->setObjectName(QStringLiteral("summaryTable"));
    m_summaryTable->setFont(monoFont(10, true));
    m_summaryTable->verticalHeader()->hide();
    m_summaryTable->verticalHeader()->setDefaultSectionSize(
        QFontMetrics(monoFont(10, true)).height() + 4);
    m_summaryTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_summaryTable->horizontalHeader()->setMinimumSectionSize(8);
    m_summaryTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_summaryTable->horizontalHeader()->setHighlightSections(false);
    m_summaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_summaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_summaryTable->setFocusPolicy(Qt::NoFocus);
    m_summaryTable->setShowGrid(false);
    m_finalScoreLabel = new QLabel(summaryPane);
    m_finalScoreLabel->setObjectName(QStringLiteral("finalScore"));
    m_finalScoreLabel->setFont(monoFont(11, true));
    m_finalScoreLabel->setAlignment(Qt::AlignCenter);
    summaryPane->layout()->addWidget(m_summaryTable);
    summaryPane->layout()->addWidget(m_finalScoreLabel);
    m_summaryDock = createPaneDock(tr("Summary"), QStringLiteral("summaryDock"),
                                   summaryPane);

    m_spotExpiryTimer = new QTimer(this);
    m_spotExpiryTimer->setInterval(60'000);
    connect(m_spotExpiryTimer, &QTimer::timeout,
            this, &MainWindow::refreshBandmap);
    m_spotExpiryTimer->start();
    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout, this, [this] {
        updateTopStatus();
        refreshRate();
    });
    m_clockTimer->start();
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (now == nullptr) {
            return;
        }
        for (int context = 0; context < 2; ++context) {
            if (m_entryGroups[context]->isAncestorOf(now)
                && context != m_activeEntryContext) {
                setActiveEntryContext(context);
                return;
            }
        }
    });

    applyDefaultDockLayout();
    createWindowMenu();
    restoreWindowLayout();
    m_radioABandmapDock->hide();
    updateFunctionKeyButtons();

    statusBar()->showMessage(tr("Database ready"));
    loadBuiltInDefinitions();
    loadOperator();
    updateStationLabel();
    loadContests();
    loadSupportData();
    if (m_repository.isStandaloneLog()) {
        setWindowFilePath(m_repository.databasePath());
        m_openLogLabel->setToolTip(m_repository.databasePath());
    }
    m_catStaleTimer = new QTimer(this);
    m_catStaleTimer->setInterval(1000);
    connect(m_catStaleTimer, &QTimer::timeout, this, [this] {
        if (radioCatStale(0) || radioCatStale(1)) {
            updateEntryHeaders();
        }
    });
    m_catStaleTimer->start();
    QTimer::singleShot(0, this, &MainWindow::autoConnectRadios);
    const int spotLifetimeMinutes = qBound(
        1, QSettings().value(QStringLiteral("dxCluster/spotLifetimeMinutes"), 60).toInt(),
        1440);
    QString spotError;
    const QVector<Spot> storedSpots = m_repository.recentSpots(500, &spotError);
    if (!spotError.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to load DX spots"), spotError);
    } else {
        for (const Spot &spot : storedSpots) {
            const qint64 ageSeconds =
                spot.timestampUtc.secsTo(QDateTime::currentDateTimeUtc());
            if (ageSeconds >= 0 && ageSeconds <= spotLifetimeMinutes * 60) {
                m_spots.insert(spot.id, spot);
            }
        }
    }
    updateEntryHeaders();
    setActiveEntryContext(0);
    refreshBandmap();
    updateTopStatus();
    refreshRate();
    m_callsignEdits[0]->setFocus();
    QTimer::singleShot(0, this, [this] {
        autoStartHostServer();
        autoStartNetworkClient();
    });
    m_retranslateUi = [this, fileMenu, newLogAction, contestMenu, operatingMenu,
                       radioMenu, settingsMenu, languageMenu, englishAction,
                       polishAction, colorPaletteMenu, currentPaletteAction,
                       monokaiPaletteAction, operatorAction, operatorCallsignAction, radioAction,
                       radioBAction, clusterAction, undoQsoAction,
                       allowDupeLoggingAction, configureEsmAction,
                       exportCabrilloAction, exportAdifAction, tuneSelectedSpot,
                       bandmapSortSelector, sendClusterCommand] {
        fileMenu->setTitle(tr("&File"));
        newLogAction->setText(tr("&New log..."));
        contestMenu->setTitle(tr("&Contest"));
        operatingMenu->setTitle(tr("O&perating"));
        radioMenu->setTitle(tr("&Radio"));
        settingsMenu->setTitle(tr("Op&tions"));
        languageMenu->setTitle(tr("Language"));
        englishAction->setText(tr("English"));
        polishAction->setText(tr("Polski"));
        colorPaletteMenu->setTitle(tr("Color palette"));
        currentPaletteAction->setText(tr("Current"));
        monokaiPaletteAction->setText(tr("Monokai"));
        operatorAction->setText(tr("Operator and &station..."));
        operatorCallsignAction->setText(tr("Operator callsign..."));
        radioAction->setText(tr("Radio A CAT settings..."));
        radioBAction->setText(tr("Radio B CAT settings..."));
        clusterAction->setText(tr("&DX cluster..."));
        undoQsoAction->setText(tr("Undo last QSO change"));
        allowDupeLoggingAction->setText(tr("Allow logging duplicate QSOs"));
        configureEsmAction->setText(tr("Configure ESM messages..."));
        m_esmAction->setText(tr("Enter Sends Message (ESM)"));
        m_autoCqAction->setText(tr("Auto CQ (F1 repeat)"));
        exportCabrilloAction->setText(tr("Export Cabrillo..."));
        exportAdifAction->setText(tr("Export ADIF..."));
        findChild<QMenu *>(QStringLiteral("windowMenu"))->setTitle(tr("&Window"));
        findChild<QAction *>(QStringLiteral("resetLayoutAction"))->setText(
            tr("Restore default layout"));
        m_checkCallDock->setWindowTitle(tr("Check callsign"));
        m_checkPartialDock->setWindowTitle(tr("Check partial"));
        m_summaryDock->setWindowTitle(tr("Summary"));
        m_contestInfoDock->setWindowTitle(tr("Contest information"));
        m_onlyNewMultiplierSpots->setText(tr("New mult only"));
        m_onlyNewMultiplierSpots->setToolTip(
            tr("Show only spots that can add a multiplier under the active contest rules."));
        m_hideWorkedSpots->setText(tr("Hide worked"));
        tuneSelectedSpot->setText(tr("Tune"));
        sendClusterCommand->setText(tr("Send"));
        m_clusterCommandInput->setPlaceholderText(
            tr("Cluster command (e.g. DX W1AW 14025.0 comment)"));
        m_clusterConnectionSelector->setItemText(0, tr("No connection"));
        m_bandmapBandFilter->setItemText(0, tr("Active band"));
        m_bandmapBandFilter->setItemText(1, tr("All bands"));
        m_bandmapModeFilter->setItemText(0, tr("All modes"));
        m_bandmapContinentFilter->setItemText(0, tr("All continents"));
        m_bandmapContinentFilter->setItemText(
            m_bandmapContinentFilter->findData(QStringLiteral("@unknown")), tr("Unknown"));
        bandmapSortSelector->setItemText(0, tr("Frequency"));
        bandmapSortSelector->setItemText(1, tr("Newest first"));
        for (QTableWidget *table : {m_bandmap, m_radioABandmap}) {
            table->setHorizontalHeaderLabels(
                {tr("kHz"), tr("Call"), tr("Age"), tr("Spotter"), tr("Source"),
                 tr("Comment")});
        }
        m_clusterLog->setHorizontalHeaderLabels(
            {tr("UTC"), tr("kHz"), tr("Call"), tr("Spotter"), tr("Source"), tr("Comment")});
        m_checkCallGrid->setToolTip(
            tr("A time means the callsign was worked; +M means this station would add a new multiplier."));
        updateEntryHeaders();
        refreshCheckWindows();
        refreshDxClusterStatus();
        refreshBandmap();
        updateTopStatus();
    };
    const QVariantList savedClusters = QSettings().value(
        QStringLiteral("dxCluster/connections")).toList();
    if (!savedClusters.isEmpty()) {
        QTimer::singleShot(0, this, [this, savedClusters] {
            for (const QVariant &saved : savedClusters) {
                const QVariantMap values = saved.toMap();
                const DxClusterConfig config{
                    values.value(QStringLiteral("host")).toString(),
                    values.value(QStringLiteral("port"), 7300).toInt(),
                    values.value(QStringLiteral("callsign")).toString()
                };
                if (config.host.trimmed().isEmpty()
                    || config.callsign.trimmed().isEmpty()
                    || config.port < 1 || config.port > 65535) {
                    qWarning().noquote()
                        << "Ignoring invalid saved DX cluster connection.";
                    continue;
                }
                connectDxCluster(config, false);
            }
        });
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_retranslateUi) {
        m_retranslateUi();
    }
}

void MainWindow::recordNetworkContact()
{
    m_networkLastContactUtc = QDateTime::currentDateTimeUtc();
    QSettings settings;
    settings.setValue(QStringLiteral("network/client/lastContactUtc"),
                      m_networkLastContactUtc.toString(Qt::ISODateWithMs));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qWarning().noquote() << "Could not persist last synchronization contact.";
    }
}

void MainWindow::updateNetworkOperatingState()
{
    if (m_hostServer) {
        m_hostServer->setHostOperatingState(
            m_bandSelectors[currentEntryContext()]->currentText().trimmed(),
            m_modeSelectors[currentEntryContext()]->currentText().trimmed());
    }
    if (!m_syncClient || m_activeEntryContext < 0 || m_activeEntryContext > 1
        || m_bandSelectors[m_activeEntryContext] == nullptr
        || m_modeSelectors[m_activeEntryContext] == nullptr) {
        return;
    }
    m_syncClient->setOperatingState(
        m_bandSelectors[m_activeEntryContext]->currentText().trimmed(),
        m_modeSelectors[m_activeEntryContext]->currentText().trimmed().toUpper());
}

void MainWindow::refreshNetworkStations()
{
    if (m_hostServer) {
        updateNetworkOperatingState();
    }
    if (m_networkStationsDock == nullptr || !m_networkStationsDock->isVisible()
        || m_networkStationsTable == nullptr || m_networkStationsStatus == nullptr) {
        return;
    }
    const Contest *contest = activeContest();
    const QString logUuid = contest != nullptr && !contest->logUuid.isEmpty()
        ? contest->logUuid
        : QSettings().value(QStringLiteral("network/client/logUuid")).toString();
    if (logUuid.isEmpty()) {
        m_networkStationsTable->setRowCount(0);
        m_networkStationsStatus->setText(tr("No synchronized log selected."));
        return;
    }
    if (m_hostServer) {
        QJsonArray stations = m_hostServer->connectedStations(logUuid);
        showNetworkStations(stations);
        return;
    }
    if (m_syncClient && m_syncClient->state() == SyncClient::State::Ready
        && QSettings().value(QStringLiteral("network/client/logUuid")).toString()
            == logUuid) {
        if (m_networkStationsRequestId.isEmpty()) {
            m_networkStationsRequestId = m_syncClient->getStatus();
        }
        if (m_networkStationsRequestId.isEmpty()) {
            m_networkStationsStatus->setText(
                tr("Unable to request the connected station list."));
        }
        return;
    }
    m_networkStationsRequestId.clear();
    m_networkStationsTable->setRowCount(0);
    m_networkStationsStatus->setText(
        tr("Connected stations are unavailable while disconnected."));
}

void MainWindow::showNetworkStations(const QJsonArray &stations)
{
    m_networkStationsTable->setSortingEnabled(false);
    m_networkStationsTable->setRowCount(stations.size());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int row = 0; row < stations.size(); ++row) {
        const QJsonObject station = stations.at(row).toObject();
        const QString computerName =
            station.value(QStringLiteral("computer_name")).toString().trimmed();
        const QString stationCallsign =
            station.value(QStringLiteral("station_callsign")).toString().trimmed();
        const QString label = !computerName.isEmpty()
            ? computerName
            : (stationCallsign.isEmpty() ? tr("Unknown station") : stationCallsign);
        const QString band = station.value(QStringLiteral("operating_band"))
                                 .toString().trimmed();
        const QString mode = station.value(QStringLiteral("operating_mode"))
                                 .toString().trimmed();
        const QDateTime updated = QDateTime::fromString(
            station.value(QStringLiteral("operating_state_updated_utc")).toString(),
            Qt::ISODateWithMs).toUTC();
        QString state;
        if (!updated.isValid()) {
            state = tr("No radio data");
        } else if (isStationOperatingStateStale(updated, now)) {
            state = tr("Stale");
        } else {
            state = tr("Connected");
        }
        const QStringList values{
            label, band.isEmpty() ? tr("—") : band,
            mode.isEmpty() ? tr("—") : mode, state
        };
        for (int column = 0; column < values.size(); ++column) {
            m_networkStationsTable->setItem(
                row, column, new QTableWidgetItem(values.at(column)));
        }
    }
    m_networkStationsTable->setSortingEnabled(true);
    m_networkStationsTable->sortItems(0);
    m_networkStationsTable->resizeColumnsToContents();
    m_networkStationsStatus->setText(
        tr("Connected stations: %1").arg(stations.size()));
}

void MainWindow::autoStartNetworkClient()
{
    QSettings settings;
    if (m_hostServer
        || !settings.value(QStringLiteral("network/client/autoStart"), false).toBool()) {
        return;
    }
    const QString host =
        settings.value(QStringLiteral("network/client/host")).toString().trimmed();
    const int port = settings.value(QStringLiteral("network/client/port"), 7443).toInt();
    const QString logUuid =
        settings.value(QStringLiteral("network/client/logUuid")).toString();
    QString error;
    const QString stationUuid = m_repository.localStationUuid(&error);
    QHostAddress serverAddress;
    if (host.isEmpty() || !serverAddress.setAddress(host)
        || port < 1 || port > 65535 || logUuid.isEmpty()
        || stationUuid.isEmpty() || !error.isEmpty()
        || !activeContest() || activeContest()->logUuid != logUuid) {
        statusBar()->showMessage(
            tr("Automatic network connection was not started: saved IP, port, "
               "log or station identity is invalid. %1").arg(error),
            15000);
        return;
    }
    configureClientConnection(true);
}

void MainWindow::autoStartHostServer()
{
    const QSettings settings;
    if (!settings.value(QStringLiteral("network/host/autoStart"), false).toBool()
        || m_hostServer) {
        return;
    }
    const QString address =
        settings.value(QStringLiteral("network/host/address")).toString().trimmed();
    const int port = settings.value(QStringLiteral("network/host/port"), 7443).toInt();
    const int maximumConnections =
        settings.value(QStringLiteral("network/host/maxConnections"), 16).toInt();
    QString error;
    QHostAddress bindAddress;
    if (address.isEmpty() || !bindAddress.setAddress(address)
        || port < 1 || port > 65535) {
        statusBar()->showMessage(
            tr("Automatic server startup was not possible: saved bind address or "
               "port is invalid."),
            15000);
        return;
    }
    auto repository = std::make_unique<CentralRepository>();
    const Contest *log = activeContest();
    if (!log || !repository->attach(m_repository, &error)
        || !m_repository.setHostedLog(log->logUuid, &error)) {
        statusBar()->showMessage(
            tr("Automatic server startup failed: %1").arg(error), 15000);
        return;
    }
    auto server = std::make_unique<SyncServer>(*repository);
    if (!server->hostOpenLog(log->logUuid, settings.value(
            QStringLiteral("network/stationName"), QHostInfo::localHostName()).toString(),
            &error)
        || !server->listenTrustedNetwork(address, static_cast<quint16>(port),
                                     maximumConnections, &error)) {
        m_repository.setHostedLog(QString());
        statusBar()->showMessage(
            tr("Automatic server startup failed: %1").arg(error), 15000);
        return;
    }
    m_hostRepository = std::move(repository);
    m_hostRepository->setCtyPrefixes(m_ctyPrefixes);
    m_hostServer = std::move(server);
    m_networkLogId = log->id;
    connect(m_hostServer.get(), &SyncServer::logChanged,
            this, &MainWindow::refreshRecentQsos);
    if (m_hostModeAction != nullptr) {
        m_hostModeAction->setText(tr("Stop central server on this station"));
    }
    if (m_hostImportAction != nullptr) {
        m_hostImportAction->setEnabled(false);
    }
    statusBar()->showMessage(
        tr("Central server started automatically on port %1.").arg(port), 10000);
}

QDockWidget *MainWindow::createPaneDock(const QString &title, const QString &objectName,
                                        QWidget *content)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    dock->setFeatures(QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetFloatable
                      | QDockWidget::DockWidgetClosable);
    dock->setWidget(content);
    return dock;
}

void MainWindow::applyDefaultDockLayout()
{
    // Left: check windows and contest info; right: bandmap column plus rate,
    // summary and cluster stacked beside it (the DXLog default arrangement).
    addDockWidget(Qt::LeftDockWidgetArea, m_checkCallDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_checkPartialDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_contestInfoDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_radioABandmapDock);
    splitDockWidget(m_checkCallDock, m_radioABandmapDock, Qt::Horizontal);
    addDockWidget(Qt::LeftDockWidgetArea, m_greylineDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_networkStationsDock);
    tabifyDockWidget(m_contestInfoDock, m_greylineDock);
    addDockWidget(Qt::RightDockWidgetArea, m_bandmapDock);
    addDockWidget(Qt::RightDockWidgetArea, m_rateDock);
    splitDockWidget(m_bandmapDock, m_rateDock, Qt::Horizontal);
    splitDockWidget(m_rateDock, m_summaryDock, Qt::Vertical);
    splitDockWidget(m_summaryDock, m_clusterDock, Qt::Vertical);
    for (QDockWidget *dock : {m_checkCallDock, m_checkPartialDock,
                              m_radioABandmapDock, m_greylineDock,
                              m_networkStationsDock,
                              m_contestInfoDock, m_bandmapDock, m_rateDock,
                              m_summaryDock, m_clusterDock}) {
        dock->setFloating(false);
        dock->show();
    }
    m_radioABandmapDock->hide();
    m_networkStationsDock->hide();
    m_contestInfoDock->raise();
    resizeDocks({m_checkCallDock, m_radioABandmapDock, m_bandmapDock, m_rateDock},
                {230, 280, 250, 400}, Qt::Horizontal);
    resizeDocks({m_checkCallDock, m_checkPartialDock, m_contestInfoDock},
                {250, 260, 230}, Qt::Vertical);
    resizeDocks({m_rateDock, m_summaryDock, m_clusterDock},
                {200, 230, 320}, Qt::Vertical);
}

void MainWindow::createWindowMenu()
{
    QMenu *windowMenu = menuBar()->addMenu(tr("&Window"));
    windowMenu->setObjectName(QStringLiteral("windowMenu"));
    for (QDockWidget *dock : {m_checkCallDock, m_checkPartialDock, m_bandmapDock,
                              m_clusterDock, m_rateDock, m_summaryDock,
                              m_contestInfoDock, m_greylineDock,
                              m_networkStationsDock}) {
        windowMenu->addAction(dock->toggleViewAction());
    }
    windowMenu->addSeparator();
    QAction *resetAction = windowMenu->addAction(tr("Restore default layout"));
    resetAction->setObjectName(QStringLiteral("resetLayoutAction"));
    connect(resetAction, &QAction::triggered, this, &MainWindow::resetWindowLayout);
}

void MainWindow::restoreWindowLayout()
{
    QSettings settings;
    const QByteArray geometry = settings.value(
        QStringLiteral("ui/mainWindowGeometry")).toByteArray();
    const bool hasGeometry = !geometry.isEmpty();
    const bool geometryRestored = !hasGeometry || restoreGeometry(geometry);
    if (!geometryRestored) {
        settings.remove(QStringLiteral("ui/mainWindowGeometry"));
    }
    const QByteArray state = settings.value(
        QStringLiteral("ui/mainWindowStateV2")).toByteArray();
    const bool hasState = !state.isEmpty();
    const bool stateRestored = !hasState || restoreState(state, 2);
    if (!stateRestored) {
        settings.remove(QStringLiteral("ui/mainWindowStateV2"));
    }
    if (!geometryRestored || !stateRestored) {
        statusBar()->showMessage(
            tr("Saved window layout was invalid; the default layout is in use."),
            6000);
    }
}

void MainWindow::saveWindowLayout()
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/mainWindowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/mainWindowStateV2"), saveState(2));
}

void MainWindow::resetWindowLayout()
{
    showNormal();
    resize(1600, 960);
    for (QDockWidget *dock : {m_checkCallDock, m_checkPartialDock,
                              m_radioABandmapDock, m_greylineDock,
                              m_networkStationsDock,
                              m_contestInfoDock, m_bandmapDock, m_rateDock,
                              m_summaryDock, m_clusterDock}) {
        removeDockWidget(dock);
    }
    applyDefaultDockLayout();
    saveWindowLayout();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowLayout();
    if (m_syncClient) {
        m_syncClient->stopSynchronization();
        m_syncClient->disconnectFromServer();
    }
    if (m_hostServer) {
        m_hostServer->close();
        m_hostServer.reset();
    }
    m_hostRepository.reset();
    m_repository.setHostedLog(QString());
    QMainWindow::closeEvent(event);
}

void MainWindow::configureHostMode()
{
    QString error;
    if (m_hostServer) {
        m_hostServer->close();
        m_hostServer.reset();
        m_hostRepository.reset();
        m_repository.setHostedLog(QString());
        m_hostModeAction->setText(tr("Start/stop network server..."));
        m_hostImportAction->setEnabled(false);
        statusBar()->showMessage(tr("Central server stopped on this station."), 5000);
        return;
    }
    configureHostModeStart();
}

void MainWindow::configureN1mmBroadcast()
{
        QSettings settings;
        const N1mmUdpConfiguration current = m_n1mmBroadcaster.configuration();
        QDialog dialog(this);
        dialog.setWindowTitle(tr("N1MM UDP broadcast"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        auto *enabled = new QCheckBox(tr("Broadcast each newly logged QSO"), &dialog);
        enabled->setObjectName(QStringLiteral("n1mmUdpEnabled"));
        enabled->setChecked(current.enabled);
        auto *address = new QLineEdit(
            settings.value(QStringLiteral("n1mmUdp/address"),
                           current.address.toString()).toString(), &dialog);
        address->setObjectName(QStringLiteral("n1mmUdpAddress"));
        address->setPlaceholderText(tr("IPv4 or IPv6 address"));
        auto *port = new QSpinBox(&dialog);
        port->setObjectName(QStringLiteral("n1mmUdpPort"));
        port->setRange(1, 65535);
        port->setValue(settings.value(QStringLiteral("n1mmUdp/port"), 12060).toInt());
        form->addRow(QString(), enabled);
        form->addRow(tr("Destination IP address:"), address);
        form->addRow(tr("UDP port:"), port);
        layout->addLayout(form);
        auto *notice = new QLabel(
            tr("UDP is fire-and-forget: the logger can detect local send errors, "
               "but cannot determine whether a receiver is listening."), &dialog);
        notice->setWordWrap(true);
        layout->addWidget(notice);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        N1mmUdpConfiguration configuration;
        configuration.enabled = enabled->isChecked();
        configuration.address.setAddress(address->text().trimmed());
        configuration.port = static_cast<quint16>(port->value());
        QString error;
        if (!m_n1mmBroadcaster.setConfiguration(configuration, &error)) {
            QMessageBox::warning(this, tr("Invalid N1MM UDP destination"), error);
            return;
        }
        settings.setValue(QStringLiteral("n1mmUdp/enabled"), configuration.enabled);
        settings.setValue(QStringLiteral("n1mmUdp/address"),
                          configuration.address.toString());
        settings.setValue(QStringLiteral("n1mmUdp/port"), configuration.port);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::critical(this, tr("Unable to save N1MM UDP settings"),
                                  settings.fileName());
            return;
        }
        statusBar()->showMessage(
            configuration.enabled ? tr("N1MM UDP QSO broadcast enabled.")
                                  : tr("N1MM UDP QSO broadcast disabled."),
            5000);
}

void MainWindow::broadcastFullLogToN1mm()
{
        const Contest *contest = activeContest();
        if (contest == nullptr) {
            QMessageBox::warning(this, tr("No log selected"),
                                 tr("Select a log before broadcasting it."));
            return;
        }
        if (!m_n1mmBroadcaster.configuration().enabled) {
            QMessageBox::warning(this, tr("N1MM UDP broadcast disabled"),
                                 tr("Enable N1MM UDP broadcast in Options first."));
            return;
        }
        QString error;
        const QVector<Qso> qsos = m_repository.qsosForContest(contest->id, 0, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to load log"), error);
            return;
        }
        const QString destination = QStringLiteral("%1:%2")
            .arg(m_n1mmBroadcaster.configuration().address.toString())
            .arg(m_n1mmBroadcaster.configuration().port);
        if (QMessageBox::question(
                this, tr("Broadcast full log"),
                tr("Send all %1 logged QSOs in this log to %2? "
                   "This is a separate, manual UDP operation.")
                    .arg(qsos.size()).arg(destination))
            != QMessageBox::Yes) {
            return;
        }
        int sentCount = 0;
        if (!m_n1mmBroadcaster.broadcastLog(
                qsos, contest->name, operatorIdentity(), &sentCount, &error)) {
            QMessageBox::warning(this, tr("N1MM UDP broadcast incomplete"),
                                 tr("Sent %1 QSO datagrams before the local UDP error: %2")
                                     .arg(sentCount).arg(error));
            return;
        }
        statusBar()->showMessage(
            tr("Sent %1 QSO datagrams to %2 (receiver delivery is not acknowledged).")
                .arg(sentCount).arg(destination), 10000);
}

void MainWindow::manageQtc()
{
        const Contest *contest = activeContest();
        const ContestDefinition *definition = activeDefinition();
        const bool wae = definition != nullptr
            && (definition->id.startsWith(QStringLiteral("wae-"))
                || definition->cabrilloContest.startsWith(
                    QStringLiteral("WAE-DC"), Qt::CaseInsensitive));
        if (contest == nullptr || !wae) {
            QMessageBox::information(
                this, tr("WAE QTC"),
                tr("Select a WAE CW, SSB, or RTTY log to manage QTC exchanges."));
            return;
        }
        if (m_networkReadOnly) {
            QMessageBox::warning(this, tr("Read-only log"),
                                 tr("This station cannot modify the synchronized log."));
            return;
        }
        QString error;
        const QVector<Qso> qsos =
            m_repository.qsosForContest(contest->id, 0, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to load QTC log"), error);
            return;
        }
        if (qsos.isEmpty()) {
            QMessageBox::information(this, tr("No partner QSO"),
                                     tr("Log a QSO with the QTC partner first."));
            return;
        }
        QtcDialog dialog(qsos, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const Qso *partner = dialog.partnerQso();
        if (partner == nullptr) {
            QMessageBox::critical(this, tr("QTC partner unavailable"),
                                  tr("The selected partner QSO could not be loaded."));
            return;
        }
        const qint64 partnerId = partner->id;
        QtcTransfer transfer;
        if (!dialog.transfer(&transfer, &error)) {
            QMessageBox::warning(this, tr("Invalid QTC"), error);
            return;
        }
        std::optional<Qso> updated = m_repository.qsoById(partnerId, &error);
        if (!error.isEmpty() || !updated.has_value()) {
            QMessageBox::critical(this, tr("Unable to load partner QSO"),
                                  error.isEmpty() ? tr("The partner QSO no longer exists.")
                                                  : error);
            return;
        }
        updated->qtcTransfers.append(std::move(transfer));
        if (!m_repository.updateQso(*updated, &error)) {
            QMessageBox::critical(this, tr("Unable to save QTC"), error);
            return;
        }
        refreshRecentQsos();
        statusBar()->showMessage(
            tr("WAE QTC saved with its partner QSO and queued for synchronization."),
            7000);
}

void MainWindow::configureHostModeStart()
{
    QString error;
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Network server - open log"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *address = new QLineEdit(
        settings.value(QStringLiteral("network/host/address"),
                       QStringLiteral("0.0.0.0")).toString(), &dialog);
    address->setObjectName(QStringLiteral("hostBindAddress"));
    form->addRow(tr("Bind address:"), address);
    auto *port = new QSpinBox(&dialog);
    port->setObjectName(QStringLiteral("hostBindPort"));
    port->setRange(1, 65535);
    port->setValue(settings.value(QStringLiteral("network/host/port"), 7443).toInt());
    form->addRow(tr("TCP port:"), port);
    auto *maximumConnections = new QSpinBox(&dialog);
    maximumConnections->setObjectName(QStringLiteral("hostMaximumConnections"));
    maximumConnections->setRange(1, 256);
    maximumConnections->setValue(
        settings.value(QStringLiteral("network/host/maxConnections"), 16).toInt());
    form->addRow(tr("Maximum connected stations:"), maximumConnections);

    layout->addLayout(form);
    auto *autoStartHost = new QCheckBox(
        tr("Start the central server automatically at application startup"), &dialog);
    autoStartHost->setObjectName(QStringLiteral("autoStartHostServerCheck"));
    autoStartHost->setChecked(
        settings.value(QStringLiteral("network/host/autoStart"), false).toBool());
    layout->addWidget(autoStartHost);
    auto *notice = new QLabel(
        tr("This server accepts any station that can reach the selected network "
           "interface. Traffic is not encrypted; use only on a trusted LAN/VPN. "
           "All client QSOs are saved directly in the currently open log. "
           "Clients must open the same contest type."),
        &dialog);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Start server"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString bindAddress = address->text().trimmed();
    QHostAddress hostAddress;
    if (bindAddress.isEmpty() || !hostAddress.setAddress(bindAddress)) {
        QMessageBox::warning(this, tr("Invalid listen address"),
                             tr("Enter a numeric IPv4 or IPv6 bind address."));
        return;
    }

    if (m_syncClient) {
        disconnectNetworkConnection();
    }
    auto repository = std::make_unique<CentralRepository>();
    const Contest *log = activeContest();
    if (!log || !repository->attach(m_repository, &error)
        || !m_repository.setHostedLog(log->logUuid, &error)) {
        QMessageBox::critical(this, tr("Unable to initialize central database"), error);
        return;
    }
    auto server = std::make_unique<SyncServer>(*repository);
    if (!server->hostOpenLog(log->logUuid, settings.value(
            QStringLiteral("network/stationName"), QHostInfo::localHostName()).toString(),
            &error)
        || !server->listenTrustedNetwork(
            bindAddress, static_cast<quint16>(port->value()),
            maximumConnections->value(), &error)) {
        m_repository.setHostedLog(QString());
        QMessageBox::critical(this, tr("Unable to start central server"), error);
        return;
    }

    settings.setValue(QStringLiteral("network/host/address"), bindAddress);
    settings.setValue(QStringLiteral("network/host/port"), port->value());
    settings.setValue(QStringLiteral("network/host/maxConnections"),
                      maximumConnections->value());
    settings.setValue(QStringLiteral("network/host/autoStart"),
                      autoStartHost->isChecked());
    settings.setValue(QStringLiteral("network/client/autoStart"), false);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        server->close();
        m_repository.setHostedLog(QString());
        QMessageBox::critical(this, tr("Unable to save host settings"),
                              tr("The server has been stopped because its settings "
                                 "could not be saved."));
        return;
    }
    m_hostRepository = std::move(repository);
    m_hostRepository->setCtyPrefixes(m_ctyPrefixes);
    m_hostServer = std::move(server);
    m_networkLogId = log->id;
    connect(m_hostServer.get(), &SyncServer::logChanged,
            this, &MainWindow::refreshRecentQsos);
    m_hostModeAction->setText(tr("Stop hosted central server"));
    m_hostImportAction->setEnabled(false);
    statusBar()->showMessage(
        tr("This station is hosting the central server on %1:%2.")
            .arg(bindAddress).arg(m_hostServer->serverPort()),
        15000);
}

void MainWindow::importLocalLogToHost()
{
    if (!m_hostServer || !m_hostRepository) {
        QMessageBox::warning(
            this, tr("Host mode is not active"),
            tr("Start the central server on this station before creating or importing "
               "a central log."));
        return;
    }
    const Contest *sourceLog = activeContest();
    if (sourceLog == nullptr) {
        QMessageBox::warning(this, tr("No local log selected"),
                             tr("Select a local log to import."));
        return;
    }
    const Contest source = *sourceLog;

    QString error;
    const QString yaml = m_repository.contestDefinitionYaml(source.id, &error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to load contest definition"), error);
        return;
    }
    ContestDefinition definition;
    if (yaml.trimmed().isEmpty()
        || !ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("local log import"), definition, &error)) {
        QMessageBox::critical(
            this, tr("Invalid contest definition"),
            error.isEmpty()
                ? tr("A contest definition is required before importing a log.")
                : error);
        return;
    }

    QVector<Qso> sourceQsos = m_repository.qsosForContest(source.id, 0, &error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to read local QSOs"), error);
        return;
    }
    const QString sourceStationUuid = m_repository.localStationUuid(&error);
    if (sourceStationUuid.isEmpty() || !error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to identify this station"), error);
        return;
    }

    ContestEngine engine;
    QVector<Qso> validatedSourceQsos;
    QSet<QString> sourceQsoUuids;
    for (const Qso &qso : sourceQsos) {
        const bool validIdentity = validImportUuid(qso.qsoUuid)
            && qso.logUuid == source.logUuid
            && !qso.callsign.trimmed().isEmpty()
            && ContestEngine::isValidCallsign(qso.callsign)
            && !qso.stationCallsign.trimmed().isEmpty()
            && (qso.stationUuid.isEmpty() || validImportUuid(qso.stationUuid))
            && qso.timestampUtc.isValid() && qso.frequencyHz > 0 && qso.points >= 0;
        const bool validBand = definition.bands.contains(
            qso.band, Qt::CaseInsensitive);
        const bool validMode = definition.modes.contains(
            qso.mode, Qt::CaseInsensitive);
        if (!validIdentity || !validBand || !validMode
            || sourceQsoUuids.contains(qso.qsoUuid)) {
            QMessageBox::critical(
                this, tr("Local log failed validation"),
                tr("QSO %1 has an invalid UUID, callsign, time, frequency, band, "
                   "mode or duplicate UUID. No data was imported.")
                    .arg(qso.qsoUuid));
            return;
        }
        sourceQsoUuids.insert(qso.qsoUuid);
        QMap<QString, QString> sentFields = qso.sentExchangeFields;
        if (sentFields.isEmpty()) {
            sentFields = LogExporter::mapLegacyExchange(
                qso.sentExchange, definition.sentExchange);
        }
        QMap<QString, QString> receivedFields = qso.receivedExchangeFields;
        if (receivedFields.isEmpty()) {
            receivedFields = LogExporter::mapLegacyExchange(
                qso.receivedExchange, definition.receivedExchange);
        }
        std::optional<int> serialNumber;
        for (const ExchangeFieldDefinition &field : definition.sentExchange) {
            if (field.type != ExchangeFieldType::Serial || !field.generated) {
                continue;
            }
            bool serialValid = false;
            const int serial = sentFields.value(field.name).toInt(&serialValid);
            if (!serialValid || serial < definition.serialStart) {
                QMessageBox::critical(
                    this, tr("Local log failed validation"),
                    tr("QSO %1 has no valid stored generated serial.")
                        .arg(qso.qsoUuid));
                return;
            }
            serialNumber = serial;
            break;
        }
        if (definition.serialServerRequired && qso.reservationUuid.isEmpty()) {
            QMessageBox::critical(
                this, tr("Serial reservation required"),
                tr("QSO %1 belongs to a contest that requires central serial "
                   "reservations. Its reservation cannot be safely imported.")
                    .arg(qso.qsoUuid));
            return;
        }
        const ContestEvaluationContext context = engine.buildContext(
            definition, validatedSourceQsos, multiplierResolver());
        const ContestEvaluationResult evaluation = engine.evaluate(
            definition, qso.callsign, qso.band, qso.mode, receivedFields,
            context, serialNumber, sentFields, true);
        if (!evaluation.valid) {
            const QString issue = evaluation.issues.isEmpty()
                ? tr("Contest exchange validation failed.")
                : QStringLiteral("%1: %2")
                      .arg(evaluation.issues.first().field,
                           evaluation.issues.first().message);
            QMessageBox::critical(
                this, tr("Local log failed validation"),
                tr("QSO %1 does not satisfy the contest definition: %2")
                    .arg(qso.qsoUuid, issue));
            return;
        }
        validatedSourceQsos.append(qso);
    }

    QVector<Contest> centralLogs = m_hostRepository->repository()->contests(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to list central logs"), error);
        return;
    }
    auto centralLog = std::find_if(
        centralLogs.begin(), centralLogs.end(), [&source](const Contest &log) {
            return log.logUuid == source.logUuid;
        });
    const bool createCentralLog = centralLog == centralLogs.end();
    qint64 centralContestId = 0;
    if (!createCentralLog) {
        centralContestId = centralLog->id;
        const QString centralYaml =
            m_hostRepository->repository()->contestDefinitionYaml(centralLog->id, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to read central definition"), error);
            return;
        }
        const auto hash = [](const QString &text) {
            return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256);
        };
        if (hash(centralYaml) != hash(yaml)) {
            QMessageBox::critical(
                this, tr("Contest definition mismatch"),
                tr("The existing central log has a different contest definition. "
                   "The local source was left unchanged."));
            return;
        }
    }

    QVector<Qso> centralQsos;
    if (!createCentralLog) {
        centralQsos = m_hostRepository->repository()->qsosForContest(
            centralContestId, 0, &error);
    }
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to read central QSOs"), error);
        return;
    }
    QHash<QString, Qso> centralByUuid;
    QHash<QString, Qso> centralByDupeKey;
    QSet<QString> centralServerDuplicateKeys;
    for (const Qso &qso : centralQsos) {
        centralByUuid.insert(qso.qsoUuid, qso);
        centralByDupeKey.insert(importDupeIdentity(definition.dupeKey, qso), qso);
        centralServerDuplicateKeys.insert(
            qso.callsign.trimmed().toUpper() + QChar(0x1f)
            + qso.band.trimmed().toLower() + QChar(0x1f)
            + qso.mode.trimmed().toUpper());
    }

    QVector<Qso> toImport;
    int alreadyPresent = 0;
    int skippedConflicts = 0;
    QStringList conflictCalls;
    QSet<QString> seenSourceDupeKeys;
    QSet<QString> seenSourceServerDuplicateKeys;
    for (const Qso &qso : sourceQsos) {
        const auto sameUuid = centralByUuid.constFind(qso.qsoUuid);
        if (sameUuid != centralByUuid.cend()) {
            if (sameImportedQso(qso, sameUuid.value(), sourceStationUuid)) {
                ++alreadyPresent;
            } else {
                ++skippedConflicts;
                if (conflictCalls.size() < 5) {
                    conflictCalls.append(qso.callsign);
                }
            }
            continue;
        }
        const QString dupeKey = importDupeIdentity(definition.dupeKey, qso);
        if (centralByDupeKey.contains(dupeKey)) {
            ++skippedConflicts;
            if (conflictCalls.size() < 5) {
                conflictCalls.append(qso.callsign);
            }
            continue;
        }
        const QString serverDuplicateKey = qso.callsign.trimmed().toUpper()
            + QChar(0x1f) + qso.band.trimmed().toLower()
            + QChar(0x1f) + qso.mode.trimmed().toUpper();
        if (centralServerDuplicateKeys.contains(serverDuplicateKey)
            || seenSourceDupeKeys.contains(dupeKey)
            || seenSourceServerDuplicateKeys.contains(serverDuplicateKey)) {
            ++skippedConflicts;
            if (conflictCalls.size() < 5) {
                conflictCalls.append(qso.callsign);
            }
            continue;
        }
        toImport.append(qso);
        seenSourceDupeKeys.insert(dupeKey);
        seenSourceServerDuplicateKeys.insert(serverDuplicateKey);
    }

    for (const Qso &qso : toImport) {
        if (qso.reservationUuid.isEmpty() && !definition.serialServerRequired) {
            continue;
        }
        const QJsonArray reservations =
            m_hostRepository->serialReservations(source.logUuid, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to validate serial reservation"), error);
            return;
        }
        const auto reservation = std::find_if(
            reservations.cbegin(), reservations.cend(),
            [&qso](const QJsonValue &value) {
                return value.isObject()
                    && value.toObject()
                           .value(QStringLiteral("reservation_uuid")).toString()
                        == qso.reservationUuid;
            });
        QMap<QString, QString> sentFields = qso.sentExchangeFields;
        if (sentFields.isEmpty()) {
            sentFields = LogExporter::mapLegacyExchange(
                qso.sentExchange, definition.sentExchange);
        }
        QString serialField;
        for (const ExchangeFieldDefinition &field : definition.sentExchange) {
            if (field.type == ExchangeFieldType::Serial && field.generated) {
                serialField = field.name;
                break;
            }
        }
        bool serialValid = false;
        const int serialNumber = serialField.isEmpty()
            ? 0 : sentFields.value(serialField).toInt(&serialValid);
        const QString expectedStationUuid = qso.stationUuid.isEmpty()
            ? sourceStationUuid : qso.stationUuid;
        const QJsonObject reservationObject =
            reservation == reservations.cend() ? QJsonObject{} : reservation->toObject();
        if (reservation == reservations.cend()
            || reservationObject.value(QStringLiteral("status")).toString()
                != QStringLiteral("reserved")
            || reservationObject.value(QStringLiteral("station_uuid")).toString()
                != expectedStationUuid
            || reservationObject.value(QStringLiteral("band")).toString()
                .compare(qso.band, Qt::CaseInsensitive) != 0
            || reservationObject.value(QStringLiteral("mode")).toString()
                .compare(qso.mode, Qt::CaseInsensitive) != 0
            || !serialValid
            || reservationObject.value(QStringLiteral("serial")).toInt() != serialNumber) {
            QMessageBox::critical(
                this, tr("Serial reservation unavailable"),
                tr("QSO %1 uses a serial reservation that is not available and "
                   "matching on the central server. Historical used serials cannot "
                   "be recreated safely; no QSO was imported.")
                    .arg(qso.qsoUuid));
            return;
        }
    }

    const ContestScore preview = engine.score(
        definition, sourceQsos, multiplierResolver());
    const QString conflictSummary = skippedConflicts == 0
        ? tr("No duplicate or conflicting QSOs were found.")
        : tr("%1 QSO(s) conflict with an existing or duplicate contact and will "
             "be skipped if you continue. Examples: %2")
              .arg(skippedConflicts)
              .arg(conflictCalls.join(QStringLiteral(", ")));
    QMessageBox previewDialog(QMessageBox::Question, tr("Review log import"),
        tr("Source: %1\nDestination: %2\n"
           "Source QSOs: %3\nAlready imported: %4\nTo import: %5\n"
           "Contest multipliers in preview: %6\n\n%7\n\n"
           "The local source log will not be changed.")
            .arg(source.name, source.logUuid)
            .arg(sourceQsos.size())
            .arg(alreadyPresent)
            .arg(toImport.size())
            .arg(preview.totalMultipliers)
            .arg(conflictSummary),
        QMessageBox::NoButton, this);
    auto *continueButton = previewDialog.addButton(
        skippedConflicts == 0 ? tr("Import") : tr("Skip conflicts and import"),
        QMessageBox::AcceptRole);
    previewDialog.addButton(QMessageBox::Cancel);
    previewDialog.exec();
    if (previewDialog.clickedButton() != continueButton) {
        return;
    }

    if (createCentralLog) {
        Contest copy = source;
        copy.id = 0;
        copy.logUuid = source.logUuid;
        centralContestId = m_hostRepository->repository()->createContestLog(
            definition, yaml, copy, &error);
        if (centralContestId <= 0) {
            QMessageBox::critical(this, tr("Unable to create central log"), error);
            return;
        }
    }

    QString importUuid = m_hostRepository->beginLogImport(
        source.logUuid, source.logUuid, source.name,
        sourceStationUuid, sourceQsos.size(), &error);
    if (importUuid.isEmpty() || !error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to start import audit"), error);
        return;
    }
    int importedCount = alreadyPresent;
    int skippedCount = skippedConflicts;
    if (!m_hostRepository->updateLogImport(
            importUuid, importedCount, skippedCount, QStringLiteral("in_progress"),
            {}, &error)) {
        QMessageBox::critical(this, tr("Unable to update import audit"), error);
        return;
    }

    QProgressDialog progress(
        tr("Importing QSO batches..."), tr("Pause and resume later"),
        0, toImport.size(), this);
    progress.setWindowTitle(tr("Central log import"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    for (int offset = 0; offset < toImport.size(); offset += 100) {
        if (progress.wasCanceled()) {
            break;
        }
        QJsonArray operations;
        const int end = qMin(offset + 100, toImport.size());
        for (int index = offset; index < end; ++index) {
            const Qso &qso = toImport.at(index);
            const QString operationStationUuid = qso.stationUuid.isEmpty()
                ? sourceStationUuid : qso.stationUuid;
            operations.append(importOperation(
                qso, source.logUuid, operationStationUuid));
        }
        QJsonArray results;
        if (!m_hostRepository->submitBatch(
                source.logUuid, operations, &results, &error)) {
            const QString status = error.contains(QStringLiteral("DUPLICATE_QSO"))
                ? QStringLiteral("conflict") : QStringLiteral("failed");
            QString auditError;
            if (!m_hostRepository->updateLogImport(
                    importUuid, importedCount, skippedCount, status, error,
                    &auditError)) {
                error += tr("\nImport audit could not be updated: %1").arg(auditError);
            }
            QMessageBox::critical(
                this, tr("Log import stopped"),
                tr("The central server rejected a batch. The source is unchanged; "
                   "the audit records the failure and the import can be retried "
                   "after resolving it.\n\n%1").arg(error));
            return;
        }
        if (results.size() != operations.size()) {
            error = tr("The central repository returned an incomplete batch result.");
            QString auditError;
            if (!m_hostRepository->updateLogImport(
                    importUuid, importedCount, skippedCount,
                    QStringLiteral("failed"), error, &auditError)) {
                error += tr("\nImport audit could not be updated: %1").arg(auditError);
            }
            QMessageBox::critical(this, tr("Log import stopped"), error);
            return;
        }
        importedCount += end - offset;
        if (!m_hostRepository->updateLogImport(
                importUuid, importedCount, skippedCount,
                QStringLiteral("in_progress"), {}, &error)) {
            QMessageBox::critical(this, tr("Unable to update import audit"), error);
            return;
        }
        progress.setValue(importedCount - alreadyPresent);
        QApplication::processEvents();
    }
    if (progress.wasCanceled()) {
        statusBar()->showMessage(
            tr("Import paused. Choose the same local log later to resume it."), 10000);
        return;
    }
    if (!m_hostRepository->updateLogImport(
            importUuid, importedCount, skippedCount,
            QStringLiteral("completed"), {}, &error)) {
        QMessageBox::critical(this, tr("Unable to complete import audit"), error);
        return;
    }
    progress.setValue(toImport.size());
    QMessageBox::information(
        this, tr("Central log import complete"),
        tr("Import audit: %1\nImported or already present: %2\nSkipped by operator: %3")
            .arg(importUuid).arg(importedCount).arg(skippedCount));
}

void MainWindow::configureNetworkConnection()
{
    if (!activeContest()) {
        QMessageBox::warning(this, tr("Network operation"), tr("Open a log first."));
        return;
    }
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Network operation"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *role = new QComboBox(&dialog);
    role->setObjectName(QStringLiteral("networkRoleSelector"));
    role->addItem(tr("Server"), QStringLiteral("server"));
    role->addItem(tr("Client"), QStringLiteral("client"));
    role->setCurrentIndex(m_hostServer ? 0 : 1);
    form->addRow(tr("Role:"), role);
    auto *name = new QLineEdit(settings.value(
        QStringLiteral("network/stationName"), QHostInfo::localHostName()).toString(), &dialog);
    name->setObjectName(QStringLiteral("networkStationName"));
    form->addRow(tr("Unique station name:"), name);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString stationName = name->text().trimmed();
    if (stationName.isEmpty() || stationName.size() > 255
        || std::any_of(stationName.cbegin(), stationName.cend(), [](QChar c) {
               return c.category() == QChar::Other_Control;
           })) {
        QMessageBox::warning(this, tr("Invalid station name"),
                             tr("Enter a non-empty station name (up to 255 characters)."));
        return;
    }
    settings.setValue(QStringLiteral("network/stationName"), stationName);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(this, tr("Network operation"),
                              tr("Unable to save the station name."));
        return;
    }
    disconnectNetworkConnection();
    if (m_hostServer) {
        configureHostMode();
    }
    if (role->currentData().toString() == QStringLiteral("server")) {
        configureHostModeStart();
    } else {
        configureClientConnection();
    }
}

void MainWindow::configureClientConnection(bool automatic)
{
    QString error;
    const QVector<Contest> contests = m_repository.contests(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to load local logs"), error);
        return;
    }
    QVector<Contest> connectableLogs;
    for (const Contest &contest : contests) {
        if (!contest.logUuid.trimmed().isEmpty()) {
            connectableLogs.append(contest);
        }
    }
    QSettings settings;
    const Contest *active = activeContest();
    if (!active) {
        showOperatorMessage(tr("Open a log first."), true);
        return;
    }
    const QString defaultLogUuid = active != nullptr && !active->logUuid.isEmpty()
        ? active->logUuid
        : (connectableLogs.isEmpty() ? QString() : connectableLogs.first().logUuid);
    const QString savedHost =
        settings.value(QStringLiteral("network/client/host")).toString();
    const quint16 savedPort = static_cast<quint16>(
        settings.value(QStringLiteral("network/client/port"), 7443).toInt());
    SyncConnectionDialog dialog(
        QVector<Contest>{*active},
        savedHost, savedPort,
        defaultLogUuid,
        settings.value(QStringLiteral("network/client/autoStart"), false).toBool(),
        this);
    if (!automatic && dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (dialog.autoStart() && (dialog.joinRemote() || dialog.createRemote())) {
        QMessageBox::warning(
            this, tr("Automatic connection unavailable"),
            tr("Automatic startup requires an already selected local log. "
               "Join or create the central log first, then enable automatic startup."));
        return;
    }
    QHostAddress serverAddress;
    if (dialog.host().isEmpty() || !serverAddress.setAddress(dialog.host())
        || (!dialog.joinRemote() && !dialog.createRemote()
            && dialog.logUuid().isEmpty())) {
        QMessageBox::warning(
            this, tr("Invalid network settings"),
            tr("Enter a valid server IP address and choose a local or remote log."));
        return;
    }
    QString definitionYaml;
    if (!dialog.joinRemote() && !dialog.createRemote()) {
        const auto selectedLog = std::find_if(
            connectableLogs.cbegin(), connectableLogs.cend(),
            [&dialog](const Contest &contest) {
                return contest.logUuid == dialog.logUuid();
            });
        if (selectedLog == connectableLogs.cend()) {
            QMessageBox::critical(
                this, tr("Unable to select local log"),
                tr("The selected local log is no longer available."));
            return;
        }
        definitionYaml =
            m_repository.contestDefinitionYaml(selectedLog->id, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to load contest definition"), error);
            return;
        }
    }

    settings.setValue(QStringLiteral("network/client/host"), dialog.host());
    settings.setValue(QStringLiteral("network/client/port"), dialog.port());
    settings.setValue(QStringLiteral("network/client/autoStart"), dialog.autoStart());
    settings.setValue(QStringLiteral("network/host/autoStart"), false);
    if (!dialog.joinRemote() && !dialog.createRemote()) {
        settings.setValue(QStringLiteral("network/client/logUuid"), dialog.logUuid());
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(
            this, tr("Unable to save network settings"),
            tr("The server address and selected log could not be saved."));
        return;
    }

    if (!dialog.joinRemote() && !dialog.createRemote()
        && !m_repository.setSynchronizationEnabled(dialog.logUuid(), true, &error)) {
        QMessageBox::critical(this, tr("Unable to enable synchronization"), error);
        return;
    }
    m_joiningRemoteLog = dialog.joinRemote();
    m_creatingRemoteLog = dialog.createRemote();
    m_mergingRemoteLog = false;
    m_pendingRemoteContest = {};
    m_pendingRemoteDefinitionYaml.clear();
    if (m_syncClient) {
        m_syncClient->stopSynchronization();
        m_syncClient->disconnectFromServer();
        m_syncClient.reset();
    }
    if (!m_syncClient) {
        m_syncClient = std::make_unique<SyncClient>();
        m_networkLogId = active ? active->id : 0;
        connect(m_syncClient.get(), &SyncClient::logBound, this,
                [this](const QString &uuid) {
                    QSettings settings;
                    settings.setValue(QStringLiteral("network/client/logUuid"), uuid);
                    settings.sync();
                    if (settings.status() != QSettings::NoError) {
                        showOperatorMessage(tr("Unable to save network settings"), true);
                    }
                    loadContests();
                });
        connect(m_syncClient.get(), &SyncClient::synchronizationFinished, this,
                [this](int, int, qint64, bool success, const QString &detail) {
                    if (success) {
                        refreshRecentQsos();
                    } else {
                        showOperatorMessage(detail, true);
                    }
                });
        connect(m_syncClient.get(), &SyncClient::readOnlyChanged,
                this, [this](bool readOnly) {
                    m_networkReadOnly = readOnly;
                    if (readOnly) {
                        statusBar()->showMessage(
                            tr("Connected with read-only access; QSO changes and "
                               "serial reservations are disabled."),
                            15000);
                    }
                });
        connect(m_syncClient.get(), &SyncClient::stateChanged,
                this, [this](SyncClient::State state, const QString &message) {
                    if (state == SyncClient::State::Ready) {
                        recordNetworkContact();
                        updateNetworkOperatingState();
                        refreshNetworkStations();
                    } else if (state == SyncClient::State::Disconnected
                               || state == SyncClient::State::Error) {
                        refreshNetworkStations();
                    }
                    if (m_joiningRemoteLog
                        && (state == SyncClient::State::Error
                            || state == SyncClient::State::Disconnected)) {
                        m_joiningRemoteLog = false;
                        m_pendingRemoteContest = {};
                        m_pendingRemoteDefinitionYaml.clear();
                    }
                    if (!message.isEmpty()) {
                        statusBar()->showMessage(message, 10000);
                    } else if (state == SyncClient::State::Disconnected) {
                        statusBar()->showMessage(tr("Central synchronization disconnected."),
                                                 5000);
                    }
                });
        connect(m_syncClient.get(), &SyncClient::ready,
                this, [this](const QString &, qint64 revision, const QString &) {
                    recordNetworkContact();
                    if (m_joiningRemoteLog) {
                        QString joinError;
                        ContestDefinition definition;
                        if (m_pendingRemoteContest.logUuid.isEmpty()
                            || !ContestDefinitionLoader().loadText(
                                m_pendingRemoteDefinitionYaml,
                                QStringLiteral("remote central log"),
                                definition, &joinError)) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to join central log"),
                                joinError.isEmpty()
                                    ? tr("The selected central log is no longer available.")
                                    : joinError);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        Contest remoteLog = m_pendingRemoteContest;
                        qint64 localId = 0;
                        if (!m_repository.withTransaction(
                                [this, &definition, &remoteLog,
                                 &localId, &joinError](QString *operationError) {
                                    localId = m_repository.createContestLog(
                                        definition, m_pendingRemoteDefinitionYaml,
                                        remoteLog, operationError);
                                    return localId > 0
                                        && m_repository.setSynchronizationEnabled(
                                            remoteLog.logUuid, true, operationError);
                                }, &joinError)) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to create local log"), joinError);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        QSettings settings;
                        settings.setValue(
                            QStringLiteral("network/client/logUuid"),
                            remoteLog.logUuid);
                        settings.sync();
                        if (settings.status() != QSettings::NoError) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to save joined log settings"),
                                tr("The local copy was created, but its selected-log "
                                   "setting could not be saved."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        m_joiningRemoteLog = false;
                        loadContests();
                        const int logIndex = m_contestSelector->findData(localId);
                        if (logIndex >= 0) {
                            m_contestSelector->setCurrentIndex(logIndex);
                        }
                        if (!m_syncClient->startSynchronization(
                                m_repository, &joinError)) {
                            QMessageBox::critical(
                                this, tr("Unable to start synchronization"), joinError);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        statusBar()->showMessage(
                            tr("Joined central log %1; downloading its current state.")
                                .arg(remoteLog.name), 12000);
                        return;
                    }
                    if (m_creatingRemoteLog || m_mergingRemoteLog) {
                        QString mergeError;
                        const QVector<SerialReservation> reservations =
                            m_repository.serialReservations(
                                m_pendingRemoteContest.logUuid, &mergeError);
                        if (!mergeError.isEmpty()) {
                            QMessageBox::critical(
                                this, tr("Unable to read serial history"), mergeError);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        if (reservations.size() > 1000) {
                            QMessageBox::critical(
                                this, tr("Serial history is too large"),
                                tr("This log has more than 1000 serial reservations. "
                                   "Use Host import, which can transfer the history "
                                   "in bounded batches."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        QJsonArray history;
                        for (const SerialReservation &reservation : reservations) {
                            history.append(QJsonObject{
                                {QStringLiteral("reservation_uuid"),
                                 reservation.reservationUuid},
                                {QStringLiteral("station_uuid"),
                                 reservation.stationUuid},
                                {QStringLiteral("scope_key"), reservation.scopeKey},
                                {QStringLiteral("band"), reservation.band},
                                {QStringLiteral("mode"), reservation.mode},
                                {QStringLiteral("serial"), reservation.serialNumber},
                                {QStringLiteral("status"), reservation.status},
                                {QStringLiteral("qso_uuid"), reservation.qsoUuid}
                            });
                        }
                        if (history.isEmpty()) {
                            completeRemoteLogMerge();
                        } else {
                            m_pendingSerialHistoryRequestId =
                                m_syncClient->importSerialHistory(
                                    m_pendingRemoteContest.logUuid, history);
                            if (m_pendingSerialHistoryRequestId.isEmpty()) {
                                QMessageBox::critical(
                                    this, tr("Unable to transfer serial history"),
                                    tr("The client could not submit serial history "
                                       "to the central server."));
                                m_syncClient->disconnectFromServer();
                            }
                        }
                        return;
                    }
                    statusBar()->showMessage(
                        tr("Connected to central log; server revision %1.")
                            .arg(revision), 10000);
                });
        connect(m_syncClient.get(), &SyncClient::logsDiscovered,
                this, [this](const QJsonArray &logs) {
                    if (!m_joiningRemoteLog && !m_creatingRemoteLog) {
                        return;
                    }
                    QString error;
                    const QVector<Contest> localLogs = m_repository.contests(&error);
                    if (!error.isEmpty()) {
                        m_joiningRemoteLog = false;
                        m_creatingRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Unable to inspect local logs"), error);
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    if (m_creatingRemoteLog) {
                        QStringList localChoices;
                        for (const Contest &local : localLogs) {
                            localChoices.append(QStringLiteral("%1 — %2")
                                .arg(local.name, local.stationCallsign));
                        }
                        bool accepted = false;
                        const QString selected = QInputDialog::getItem(
                            this, tr("Create central log"),
                            tr("Choose the local log to create centrally:"),
                            localChoices, 0, false, &accepted);
                        if (!accepted) {
                            m_creatingRemoteLog = false;
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        const int localIndex = localChoices.indexOf(selected);
                        if (localIndex < 0 || localIndex >= localLogs.size()) {
                            m_creatingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to select local log"),
                                tr("The selected local log is no longer available."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        const Contest local = localLogs.at(localIndex);
                        m_pendingRemoteDefinitionYaml =
                            m_repository.contestDefinitionYaml(local.id, &error);
                        if (!error.isEmpty()) {
                            m_creatingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to load contest definition"), error);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        QVector<Qso> sourceQsos = m_repository.qsosForContest(
                            local.id, 0, &error);
                        if (!error.isEmpty()) {
                            m_creatingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to read local QSOs"), error);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        const QString stationUuid =
                            m_repository.localStationUuid(&error);
                        if (!error.isEmpty()) {
                            m_creatingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to identify this station"), error);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        if (std::any_of(
                                sourceQsos.cbegin(), sourceQsos.cend(),
                                [&stationUuid](const Qso &qso) {
                                    return qso.stationUuid != stationUuid;
                                })) {
                            m_creatingRemoteLog = false;
                            QMessageBox::warning(
                                this, tr("Administrator import required"),
                                tr("This local log contains QSOs recorded by other "
                                   "stations. Create it through Host import so their "
                                   "origin identities can be preserved."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        QMessageBox confirm(
                            QMessageBox::Question, tr("Create central log"),
                            tr("Create a central log from “%1” and import its %2 "
                               "local QSO(s)? Existing local data will be retained. "
                               "This operation requires administrator permission.")
                                .arg(local.name).arg(sourceQsos.size()),
                            QMessageBox::NoButton, this);
                        auto *createButton = confirm.addButton(
                            tr("Create and import"), QMessageBox::AcceptRole);
                        confirm.addButton(QMessageBox::Cancel);
                        confirm.exec();
                        if (confirm.clickedButton() != createButton) {
                            m_creatingRemoteLog = false;
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        m_pendingRemoteContest = local;
                        const QString requestId = m_syncClient->createRemoteLog(
                            local, m_pendingRemoteDefinitionYaml);
                        if (requestId.isEmpty()) {
                            m_creatingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to create central log"),
                                tr("The client is no longer waiting for a log selection."));
                            m_syncClient->disconnectFromServer();
                        }
                        return;
                    }
                    QStringList choices;
                    QVector<QJsonObject> candidates;
                    QSet<QString> seenUuids;
                    for (const QJsonValue &value : logs) {
                        const QJsonObject remote = value.toObject();
                        const QString logUuid =
                            remote.value(QStringLiteral("log_uuid")).toString();
                        if (seenUuids.contains(logUuid)) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Invalid central log list"),
                                tr("The server listed a central log more than once."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        seenUuids.insert(logUuid);
                        const bool alreadyLocal = std::any_of(
                            localLogs.cbegin(), localLogs.cend(),
                            [&logUuid](const Contest &local) {
                                return local.logUuid == logUuid;
                            });
                        const QString label = QStringLiteral("%1 — %2%3")
                            .arg(remote.value(QStringLiteral("name")).toString(),
                                 logUuid,
                                 alreadyLocal
                                     ? tr(" (merge into existing local log)")
                                     : QString());
                        choices.append(label);
                        candidates.append(remote);
                    }
                    if (choices.isEmpty()) {
                        m_joiningRemoteLog = false;
                        QMessageBox::information(
                            this, tr("No remote logs to join"),
                            tr("Every accessible central log already has a local "
                               "log. Select that local log in the connection dialog "
                               "to synchronize it."));
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    bool accepted = false;
                    const QString selected = QInputDialog::getItem(
                        this, tr("Join or merge central log"),
                        tr("Choose a central log to copy or merge locally:"),
                        choices, 0, false, &accepted);
                    if (!accepted) {
                        m_joiningRemoteLog = false;
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    const int index = choices.indexOf(selected);
                    if (index < 0 || index >= candidates.size()) {
                        m_joiningRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Unable to select central log"),
                            tr("The selected central log is not in the server list."));
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    const QJsonObject remote = candidates.at(index);
                    const QString yaml =
                        remote.value(QStringLiteral("definition_yaml")).toString();
                    const QString selectedUuid =
                        remote.value(QStringLiteral("log_uuid")).toString();
                    const auto existingLocal = std::find_if(
                        localLogs.cbegin(), localLogs.cend(),
                        [&selectedUuid](const Contest &local) {
                            return local.logUuid == selectedUuid;
                        });
                    if (existingLocal != localLogs.cend()) {
                        const QString localYaml =
                            m_repository.contestDefinitionYaml(existingLocal->id, &error);
                        if (!error.isEmpty()) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to compare contest definitions"), error);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        if (QCryptographicHash::hash(
                                localYaml.toUtf8(), QCryptographicHash::Sha256)
                            != QCryptographicHash::hash(
                                yaml.toUtf8(), QCryptographicHash::Sha256)) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Contest definition mismatch"),
                                tr("The local and central logs use different contest "
                                   "definitions. They cannot be merged automatically."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        const QVector<Qso> localQsos =
                            m_repository.qsosForContest(existingLocal->id, 0, &error);
                        if (!error.isEmpty()) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Unable to inspect local QSOs"), error);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        const QString stationUuid =
                            m_repository.localStationUuid(&error);
                        if (!error.isEmpty()
                            || std::any_of(
                                localQsos.cbegin(), localQsos.cend(),
                                [&stationUuid](const Qso &qso) {
                                    return qso.stationUuid != stationUuid;
                                })) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Administrator import required"),
                                error.isEmpty()
                                    ? tr("This local log contains QSOs from other "
                                         "stations; safe automatic merge is unavailable.")
                                    : error);
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        QMessageBox confirm(
                            QMessageBox::Question, tr("Merge local log"),
                            tr("Merge %1 local QSO(s) into central log “%2”? "
                               "Identical records will be kept once. Divergent "
                               "records will remain local as synchronization "
                               "conflicts; neither side is silently overwritten.")
                                .arg(localQsos.size())
                                .arg(existingLocal->name),
                            QMessageBox::NoButton, this);
                        auto *mergeButton = confirm.addButton(
                            tr("Merge and synchronize"), QMessageBox::AcceptRole);
                        confirm.addButton(QMessageBox::Cancel);
                        confirm.exec();
                        if (confirm.clickedButton() != mergeButton) {
                            m_joiningRemoteLog = false;
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        m_mergingRemoteLog = true;
                        m_joiningRemoteLog = false;
                        m_pendingRemoteContest = *existingLocal;
                        m_pendingRemoteDefinitionYaml = localYaml;
                    } else {
                        m_joiningRemoteLog = true;
                        m_mergingRemoteLog = false;
                    }
                    ContestDefinition definition;
                    if (yaml.trimmed().isEmpty()
                        || !ContestDefinitionLoader().loadText(
                            yaml, QStringLiteral("remote central log"),
                            definition, &error)) {
                        m_joiningRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Invalid central log definition"),
                            error.isEmpty()
                                ? tr("The central log has no contest definition.")
                                : error);
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    Contest remoteLog;
                    remoteLog.logUuid =
                        remote.value(QStringLiteral("log_uuid")).toString();
                    if (existingLocal != localLogs.cend()) {
                        remoteLog.id = existingLocal->id;
                    }
                    remoteLog.name = remote.value(QStringLiteral("name")).toString();
                    remoteLog.stationCallsign =
                        remote.value(QStringLiteral("station_callsign")).toString();
                    const QJsonValue startsAt =
                        remote.value(QStringLiteral("starts_at_utc"));
                    if (startsAt.isString()) {
                        const QDateTime parsed = QDateTime::fromString(
                            startsAt.toString(), Qt::ISODateWithMs);
                        if (!parsed.isValid()) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Invalid central log metadata"),
                                tr("The central log start time is invalid."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        remoteLog.startsAtUtc = parsed.toUTC();
                    } else if (!startsAt.isNull()) {
                        m_joiningRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Invalid central log metadata"),
                            tr("The central log start time is invalid."));
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    const QJsonValue properties =
                        remote.value(QStringLiteral("properties"));
                    if (!properties.isObject()) {
                        m_joiningRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Invalid central log metadata"),
                            tr("The central log properties are invalid."));
                        m_syncClient->disconnectFromServer();
                        return;
                    }
                    const QJsonObject propertyObject = properties.toObject();
                    for (auto property = propertyObject.constBegin();
                         property != propertyObject.constEnd(); ++property) {
                        if (!property.value().isString()) {
                            m_joiningRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Invalid central log metadata"),
                                tr("The central log properties are invalid."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        remoteLog.properties.insert(
                            property.key(), property.value().toString());
                    }
                    m_pendingRemoteContest = remoteLog;
                    m_pendingRemoteDefinitionYaml = yaml;
                    const QString definitionHash = QString::fromLatin1(
                        QCryptographicHash::hash(
                            yaml.toUtf8(), QCryptographicHash::Sha256).toHex());
                    if (!m_syncClient->selectRemoteLog(
                            remoteLog.logUuid, definitionHash)) {
                        m_joiningRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Unable to select central log"),
                            tr("The connection is no longer waiting for a log selection."));
                        m_syncClient->disconnectFromServer();
                    }
                });
        connect(m_syncClient.get(), &SyncClient::requestFailed,
        this, [this](const QString &requestId, const QString &code,
                     const QString &message, bool retryable) {
                    if (requestId == m_networkStationsRequestId) {
                        m_networkStationsRequestId.clear();
                        m_networkStationsStatus->setText(
                            tr("Station list request failed: %1").arg(message));
                    }
                    if (requestId == m_pendingSerialHistoryRequestId) {
                        m_pendingSerialHistoryRequestId.clear();
                        m_creatingRemoteLog = false;
                        m_mergingRemoteLog = false;
                        QMessageBox::critical(
                            this, tr("Serial history transfer failed"),
                            tr("%1\nNo local data was removed. Check administrator "
                               "permissions and serial conflicts before retrying.")
                                .arg(message));
                        return;
                    }
                    if (requestId == m_pendingAdminSerialRequestId) {
                        m_pendingAdminSerialRequestId.clear();
                        showOperatorMessage(
                            tr("Unable to load central serial reservations: %1")
                                .arg(message),
                            true);
                        return;
                    }
                    if (requestId == m_pendingSerialAdjustmentRequestId) {
                        m_pendingSerialAdjustmentRequestId.clear();
                        showOperatorMessage(
                            tr("Serial correction failed and was not applied: %1")
                                .arg(message),
                            true);
                        return;
                    }
                    statusBar()->showMessage(
                        tr("Network request failed (%1): %2").arg(code, message),
                        15000);
            const QString reservationUuid =
                m_serialRequestReservationUuids.value(requestId);
            const QString cancelReservationUuid =
                m_serialCancelRequestReservationUuids.value(requestId);
            if (!cancelReservationUuid.isEmpty()
                && m_serialCancelContexts.contains(cancelReservationUuid)) {
                const int context =
                    m_serialCancelContexts.value(cancelReservationUuid);
                if (!retryable) {
                    m_serialCancelContexts.remove(cancelReservationUuid);
                    for (auto it =
                             m_serialCancelRequestReservationUuids.begin();
                         it != m_serialCancelRequestReservationUuids.end();) {
                        if (it.key() == requestId
                            || it.value() == cancelReservationUuid) {
                            it = m_serialCancelRequestReservationUuids.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    if (m_cancelSerialButtons[context] != nullptr) {
                        m_cancelSerialButtons[context]->setEnabled(true);
                        m_cancelSerialButtons[context]->setText(
                            tr("Cancel serial"));
                    }
                }
                showOperatorMessage(
                    retryable
                        ? tr("Serial cancellation is queued for retry: %1")
                              .arg(message)
                        : tr("Serial cancellation failed: %1").arg(message),
                    !retryable);
                return;
            }
            if (reservationUuid.isEmpty()
                || !m_serialReservationContexts.contains(reservationUuid)) {
                return;
            }
            if (!retryable) {
                m_serialReservationContexts.remove(reservationUuid);
                for (auto it = m_serialRequestReservationUuids.begin();
                     it != m_serialRequestReservationUuids.end();) {
                    if (it.key() == requestId || it.value() == reservationUuid) {
                        it = m_serialRequestReservationUuids.erase(it);
                    } else {
                        ++it;
                    }
                }
                m_serialReservationPendingBands.remove(reservationUuid);
                m_serialReservationPendingModes.remove(reservationUuid);
            }
            showOperatorMessage(
                retryable
                    ? tr("Serial request is queued and will be retried: %1")
                          .arg(message)
                    : tr("Serial reservation failed: %1").arg(message),
                !retryable);
        });
        connect(m_syncClient.get(), &SyncClient::responseReceived,
                this, [this](const QString &requestId, const QString &type,
                             const QJsonObject &payload) {
                    recordNetworkContact();
                    if (type == QStringLiteral("changes") || type == QStringLiteral("snapshot_page")
                        || type == QStringLiteral("batch_result")) {
                        refreshRecentQsos();
                    }
                    if (requestId == m_networkStationsRequestId) {
                        m_networkStationsRequestId.clear();
                        const QJsonValue connected =
                            payload.value(QStringLiteral("connected_stations"));
                        if (type == QStringLiteral("status")
                            && connected.isArray()) {
                            showNetworkStations(connected.toArray());
                        } else {
                            m_networkStationsTable->setRowCount(0);
                            m_networkStationsStatus->setText(
                                tr("The server returned an invalid station list."));
                        }
                    }
                    if (type == QStringLiteral("log_created")
                        && m_creatingRemoteLog) {
                        const QString logUuid =
                            payload.value(QStringLiteral("log_uuid")).toString();
                        const QString yaml =
                            payload.value(QStringLiteral("definition_yaml")).toString();
                        const QString expectedHash = QString::fromLatin1(
                            QCryptographicHash::hash(
                                m_pendingRemoteDefinitionYaml.toUtf8(),
                                QCryptographicHash::Sha256).toHex());
                        const QString returnedHash = QString::fromLatin1(
                            QCryptographicHash::hash(
                                yaml.toUtf8(), QCryptographicHash::Sha256).toHex());
                        if (logUuid != m_pendingRemoteContest.logUuid
                            || yaml != m_pendingRemoteDefinitionYaml
                            || expectedHash != returnedHash
                            || payload.value(QStringLiteral("current_revision"))
                                   .toInteger(-1) != 0
                            || !m_syncClient->selectRemoteLog(logUuid, expectedHash)) {
                            m_creatingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Invalid central log response"),
                                tr("The server did not confirm the expected log "
                                   "identity and contest definition."));
                            m_syncClient->disconnectFromServer();
                        }
                        return;
                    }
                    if (requestId == m_pendingSerialHistoryRequestId) {
                        m_pendingSerialHistoryRequestId.clear();
                        if (type != QStringLiteral("serial_history_imported")
                            || payload.value(QStringLiteral("log_uuid")).toString()
                                   != m_pendingRemoteContest.logUuid
                            || payload.value(QStringLiteral("count")).toInt(-1) < 0) {
                            m_creatingRemoteLog = false;
                            m_mergingRemoteLog = false;
                            QMessageBox::critical(
                                this, tr("Invalid serial history response"),
                                tr("The central server returned an invalid serial "
                                   "history confirmation."));
                            m_syncClient->disconnectFromServer();
                            return;
                        }
                        completeRemoteLogMerge();
                        return;
                    }
                    if (type == QStringLiteral("metrics")) {
                        QMessageBox::information(
                            this, tr("Central synchronization metrics"),
                            QString::fromUtf8(QJsonDocument(payload).toJson(
                                QJsonDocument::Indented)));
                        return;
                    }
                    if (type == QStringLiteral("serial_adjusted")
                        && requestId == m_pendingSerialAdjustmentRequestId) {
                        m_pendingSerialAdjustmentRequestId.clear();
                        const QString correctedReservationUuid =
                            payload.value(
                                QStringLiteral("reservation_uuid")).toString();
                        const int correctedSerial =
                            payload.value(
                                QStringLiteral("corrected_serial")).toInt();
                        const Contest *activeLog = activeContest();
                        const ContestDefinition *definition =
                            activeLog != nullptr
                                && activeLog->logUuid
                                    == payload.value(
                                           QStringLiteral("log_uuid")).toString()
                                ? activeDefinition() : nullptr;
                        if (definition != nullptr) {
                            QString serialFieldName;
                            int serialFieldWidth = 0;
                            for (const ExchangeFieldDefinition &field :
                                 definition->sentExchange) {
                                if (field.type == ExchangeFieldType::Serial
                                    && field.generated) {
                                    serialFieldName = field.name;
                                    serialFieldWidth = field.width;
                                    break;
                                }
                            }
                            if (!serialFieldName.isEmpty()) {
                                QString formatted = QString::number(correctedSerial);
                                if (serialFieldWidth > 0) {
                                    formatted = formatted.rightJustified(
                                        serialFieldWidth, QLatin1Char('0'));
                                }
                                for (int context = 0; context < 2; ++context) {
                                    if (m_serialReservationUuids[context]
                                        != correctedReservationUuid) {
                                        continue;
                                    }
                                    m_serialReservationSerials[context] = formatted;
                                    if (QLineEdit *edit =
                                            m_sentExchangeEdits[context].value(
                                                serialFieldName, nullptr)) {
                                        edit->setText(formatted);
                                    }
                                }
                                updateSerialFields();
                            }
                        }
                        QMessageBox::information(
                            this, tr("Serial reservation corrected"),
                            tr("Serial %1 was corrected to %2. The administrator "
                               "reason has been recorded.")
                                .arg(payload.value(
                                    QStringLiteral("previous_serial")).toInt())
                                .arg(payload.value(
                                    QStringLiteral("corrected_serial")).toInt()));
                        m_pendingAdminSerialRequestId =
                            m_syncClient->getSerialReservations();
                        return;
                    }
                    if (type == QStringLiteral("serial_reservations")
                        && requestId == m_pendingAdminSerialRequestId) {
                        m_pendingAdminSerialRequestId.clear();
                        const QJsonArray reservations =
                            payload.value(QStringLiteral("reservations")).toArray();
                        QDialog reservationDialog(this);
                        reservationDialog.setObjectName(
                            QStringLiteral("adminSerialReservationsDialog"));
                        reservationDialog.setWindowTitle(
                            tr("Central serial reservations"));
                        reservationDialog.resize(900, 500);
                        auto *reservationLayout =
                            new QVBoxLayout(&reservationDialog);
                        auto *reservationTable =
                            new QTableWidget(&reservationDialog);
                        reservationTable->setObjectName(
                            QStringLiteral("adminSerialReservationsTable"));
                        reservationTable->setColumnCount(10);
                        reservationTable->setHorizontalHeaderLabels(
                            {tr("Reservation"), tr("Station"), tr("Scope"),
                             tr("Serial"), tr("State"), tr("Band"), tr("Mode"),
                             tr("QSO"), tr("Created"), tr("Updated")});
                        reservationTable->setSelectionBehavior(
                            QAbstractItemView::SelectRows);
                        reservationTable->setSelectionMode(
                            QAbstractItemView::SingleSelection);
                        reservationTable->setEditTriggers(
                            QAbstractItemView::NoEditTriggers);
                        reservationTable->setRowCount(reservations.size());
                        for (int row = 0; row < reservations.size(); ++row) {
                            const QJsonObject item = reservations.at(row).toObject();
                            const QStringList values{
                                item.value(QStringLiteral("reservation_uuid")).toString(),
                                item.value(QStringLiteral("station_uuid")).toString(),
                                item.value(QStringLiteral("scope_key")).toString(),
                                QString::number(item.value(
                                    QStringLiteral("serial")).toInt()),
                                item.value(QStringLiteral("status")).toString(),
                                item.value(QStringLiteral("band")).toString(),
                                item.value(QStringLiteral("mode")).toString(),
                                item.value(QStringLiteral("qso_uuid")).toString(),
                                item.value(QStringLiteral("created_at_utc")).toString(),
                                item.value(QStringLiteral("updated_at_utc")).toString()
                            };
                            for (int column = 0; column < values.size(); ++column) {
                                reservationTable->setItem(
                                    row, column,
                                    new QTableWidgetItem(values.at(column)));
                            }
                        }
                        reservationTable->resizeColumnsToContents();
                        reservationLayout->addWidget(reservationTable, 1);
                        auto *actions = new QHBoxLayout;
                        auto *correct = new QPushButton(
                            tr("Correct selected reservation..."),
                            &reservationDialog);
                        correct->setObjectName(
                            QStringLiteral("correctSerialReservationButton"));
                        auto *closeReservations = new QPushButton(
                            tr("Close"), &reservationDialog);
                        actions->addWidget(correct);
                        actions->addStretch();
                        actions->addWidget(closeReservations);
                        reservationLayout->addLayout(actions);
                        connect(closeReservations, &QPushButton::clicked,
                                &reservationDialog, &QDialog::accept);
                        connect(correct, &QPushButton::clicked,
                                &reservationDialog,
                                [this, &reservationDialog, reservationTable] {
                            const int row = reservationTable->currentRow();
                            if (row < 0) {
                                showOperatorMessage(
                                    tr("Select a serial reservation to correct."),
                                    true);
                                return;
                            }
                            if (reservationTable->item(row, 4)->text()
                                != QStringLiteral("reserved")) {
                                showOperatorMessage(
                                    tr("Only unused reservations can be corrected; "
                                       "used or cancelled serials are immutable."),
                                    true);
                                return;
                            }
                            const QString uuid =
                                reservationTable->item(row, 0)->text();
                            const int previous =
                                reservationTable->item(row, 3)->text().toInt();
                            bool accepted = false;
                            const int corrected = QInputDialog::getInt(
                                &reservationDialog, tr("Correct serial number"),
                                tr("New number for reservation %1:")
                                    .arg(uuid),
                                previous, 1, std::numeric_limits<int>::max(),
                                1, &accepted);
                            if (!accepted) {
                                return;
                            }
                            const QString reason = QInputDialog::getText(
                                &reservationDialog, tr("Audit reason"),
                                tr("Required reason for this correction:"),
                                QLineEdit::Normal, {}, &accepted).trimmed();
                            if (!accepted) {
                                return;
                            }
                            if (reason.isEmpty()) {
                                showOperatorMessage(
                                    tr("A non-empty audit reason is required."),
                                    true);
                                return;
                            }
                            const auto confirmation = QMessageBox::question(
                                &reservationDialog, tr("Confirm serial correction"),
                                tr("Change reserved serial %1 to %2?\n\nReason: %3")
                                    .arg(previous).arg(corrected).arg(reason),
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No);
                            if (confirmation != QMessageBox::Yes) {
                                return;
                            }
                            const QString adjustmentRequest =
                                m_syncClient->adjustSerialReservation(
                                    uuid, previous, corrected, reason);
                            if (adjustmentRequest.isEmpty()) {
                                showOperatorMessage(
                                    tr("The correction could not be sent. Verify the "
                                       "connection and administrator permission."),
                                    true);
                                return;
                            }
                            m_pendingSerialAdjustmentRequestId = adjustmentRequest;
                            reservationDialog.accept();
                        });
                        reservationDialog.exec();
                        return;
                    }
                    if (type == QStringLiteral("serial_cancelled")) {
                        const QString reservationUuid =
                            payload.value(QStringLiteral("reservation_uuid")).toString();
                        if (!m_serialCancelContexts.contains(reservationUuid)) {
                            return;
                        }
                        const int context =
                            m_serialCancelContexts.take(reservationUuid);
                        for (auto it =
                                 m_serialCancelRequestReservationUuids.begin();
                             it != m_serialCancelRequestReservationUuids.end();) {
                            if (it.key() == requestId
                                || it.value() == reservationUuid) {
                                it = m_serialCancelRequestReservationUuids.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        const Contest *contest = activeContest();
                        const bool reservationIsActive =
                            m_serialReservationUuids[context] == reservationUuid
                            && contest != nullptr
                            && contest->logUuid
                                == payload.value(QStringLiteral("log_uuid")).toString();
                        if (reservationIsActive) {
                            m_serialReservationUuids[context].clear();
                            m_serialReservationBands[context].clear();
                            m_serialReservationModes[context].clear();
                            m_serialReservationSerials[context].clear();
                        }
                        if (reservationIsActive) {
                            const ContestDefinition *definition = activeDefinition();
                            if (definition == nullptr
                                || !definition->serialServerRequired) {
                                updateSerialFields();
                                showOperatorMessage(
                                    tr("Unused serial reservation cancelled; the number "
                                       "will not be reused."));
                                return;
                            }
                            for (const ExchangeFieldDefinition &field :
                                 definition->sentExchange) {
                                if (field.type == ExchangeFieldType::Serial
                                    && field.generated) {
                                    if (QLineEdit *edit =
                                            m_sentExchangeEdits[context].value(
                                                field.name, nullptr)) {
                                        edit->clear();
                                    }
                                    break;
                                }
                            }
                        }
                        updateSerialFields();
                        showOperatorMessage(
                            tr("Unused serial reservation cancelled; the number will "
                               "not be reused."));
                        return;
                    }
                    if (type != QStringLiteral("serial_reserved")) {
                        return;
                    }
                    const QString reservationUuid =
                        payload.value(QStringLiteral("reservation_uuid")).toString();
                    if (!m_serialReservationContexts.contains(reservationUuid)) {
                        return;
                    }
                    const int context =
                        m_serialReservationContexts.take(reservationUuid);
                    const QString expectedBand =
                        m_serialReservationPendingBands.take(reservationUuid);
                    const QString expectedMode =
                        m_serialReservationPendingModes.take(reservationUuid);
                    m_serialRequestReservationUuids.remove(requestId);
                    for (auto it = m_serialRequestReservationUuids.begin();
                         it != m_serialRequestReservationUuids.end();) {
                        if (it.value() == reservationUuid) {
                            it = m_serialRequestReservationUuids.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    const QString responseBand =
                        payload.value(QStringLiteral("band")).toString().trimmed()
                            .toLower();
                    const QString responseMode =
                        payload.value(QStringLiteral("mode")).toString().trimmed()
                            .toUpper();
                    if (payload.value(QStringLiteral("status")).toString()
                            != QStringLiteral("reserved")
                        || payload.value(QStringLiteral("formatted_serial"))
                               .toString().isEmpty()
                        || responseBand != expectedBand
                        || responseMode != expectedMode) {
                        showOperatorMessage(
                            tr("Central server returned an invalid serial reservation."),
                            true);
                        return;
                    }
                    const Contest *contest = activeContest();
                    if (contest == nullptr
                        || contest->logUuid
                            != payload.value(QStringLiteral("log_uuid")).toString()) {
                        showOperatorMessage(
                            tr("Serial reserved for a different log; select that log "
                               "before using it."),
                            true);
                        return;
                    }
                    QString repositoryError;
                    const QString stationUuid =
                        m_repository.localStationUuid(&repositoryError);
                    if (!repositoryError.isEmpty()
                        || payload.value(QStringLiteral("station_uuid")).toString()
                            != stationUuid) {
                        showOperatorMessage(
                            repositoryError.isEmpty()
                                ? tr("The serial was reserved for a different station.")
                                : tr("Unable to verify station identity: %1")
                                      .arg(repositoryError),
                            true);
                        return;
                    }
                    const ContestDefinition *definition = activeDefinition();
                    if (definition == nullptr) {
                        showOperatorMessage(
                            tr("The active contest definition is unavailable."), true);
                        return;
                    }
                    QString serialFieldName;
                    int serialFieldWidth = 0;
                    for (const ExchangeFieldDefinition &field :
                         definition->sentExchange) {
                        if (field.type == ExchangeFieldType::Serial && field.generated) {
                            serialFieldName = field.name;
                            serialFieldWidth = field.width;
                            break;
                        }
                    }
                    const QString formattedSerial =
                        payload.value(QStringLiteral("formatted_serial")).toString();
                    const std::optional<SerialReservation> stored =
                        m_repository.serialReservation(contest->logUuid,
                                                       reservationUuid,
                                                       &repositoryError);
                    if (!repositoryError.isEmpty()
                        || !stored.has_value()
                        || serialFieldName.isEmpty()
                        || stored->status != QStringLiteral("reserved")
                        || stored->stationUuid != stationUuid
                        || stored->band != responseBand
                        || stored->mode != responseMode
                        || QString::number(stored->serialNumber)
                               .rightJustified(serialFieldWidth, QLatin1Char('0'))
                               != formattedSerial
                        || stored->serialNumber
                            != payload.value(QStringLiteral("serial")).toInt(-1)) {
                        showOperatorMessage(
                            repositoryError.isEmpty()
                                ? tr("The reserved serial could not be verified in the "
                                     "local cache.")
                                : tr("Unable to verify the reserved serial: %1")
                                      .arg(repositoryError),
                            true);
                        return;
                    }
                    m_serialReservationUuids[context] = reservationUuid;
                    m_serialReservationBands[context] = responseBand;
                    m_serialReservationModes[context] = responseMode;
                    m_serialReservationSerials[context] = formattedSerial;
                    if (QLineEdit *edit =
                            m_sentExchangeEdits[context].value(serialFieldName, nullptr)) {
                        edit->setText(formattedSerial);
                    }
                    updateSerialFields();
                    showOperatorMessage(
                        tr("Server reserved serial %1.")
                            .arg(m_serialReservationSerials[context]));
                });
    }

    const QString stationUuid = m_repository.localStationUuid(&error);
    if (stationUuid.isEmpty() || !error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to identify this station"), error);
        return;
    }
    SyncClientConfiguration configuration;
    configuration.host = dialog.host();
    configuration.port = dialog.port();
    configuration.stationUuid = stationUuid;
    configuration.computerName = settings.value(
        QStringLiteral("network/stationName"), QHostInfo::localHostName()).toString();
    configuration.stationCallsign = m_station.callsign;
    configuration.operatorName = operatorIdentity();
    configuration.trustedNetworkMode = true;
    configuration.useServerActiveLog = true;
    configuration.logUuid = dialog.joinRemote() || dialog.createRemote()
        ? QString() : dialog.logUuid();
    if (!definitionYaml.trimmed().isEmpty()) {
        configuration.contestDefinitionSha256 = QString::fromLatin1(
            QCryptographicHash::hash(definitionYaml.toUtf8(),
                                     QCryptographicHash::Sha256).toHex());
    }
    m_syncClient->connectToServer(configuration);
    updateNetworkOperatingState();
    if (!dialog.joinRemote() && !dialog.createRemote()
        && !m_syncClient->startSynchronization(m_repository, &error)) {
        m_syncClient->disconnectFromServer();
        QMessageBox::critical(this, tr("Unable to start synchronization"), error);
        return;
    }
    statusBar()->showMessage(
        dialog.joinRemote()
            || dialog.createRemote()
            ? tr("Connecting over the trusted network to discover central logs.")
            : tr("Connecting over the trusted network to central log %1.")
                  .arg(dialog.logUuid()),
        10000);
}

void MainWindow::completeRemoteLogMerge()
{
    if (!m_syncClient || m_pendingRemoteContest.logUuid.isEmpty()) {
        QMessageBox::critical(
            this, tr("Unable to complete log merge"),
            tr("The selected local log identity is unavailable."));
        return;
    }
    QString error;
    const QString logUuid = m_pendingRemoteContest.logUuid;
    if (!m_repository.setSynchronizationEnabled(logUuid, true, &error)
        || !m_repository.enqueueExistingQsosForSync(logUuid, &error)) {
        QMessageBox::critical(this, tr("Unable to prepare local log merge"), error);
        m_syncClient->disconnectFromServer();
        m_creatingRemoteLog = false;
        m_mergingRemoteLog = false;
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("network/client/logUuid"), logUuid);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(
            this, tr("Unable to save joined log settings"),
            tr("Local log data has been retained, but the selected-log setting "
               "could not be saved."));
        m_syncClient->disconnectFromServer();
        m_creatingRemoteLog = false;
        m_mergingRemoteLog = false;
        return;
    }
    const qint64 localId = m_pendingRemoteContest.id;
    const QString logName = m_pendingRemoteContest.name;
    m_creatingRemoteLog = false;
    m_mergingRemoteLog = false;
    m_pendingRemoteContest = {};
    m_pendingRemoteDefinitionYaml.clear();
    loadContests();
    const int logIndex = m_contestSelector->findData(localId);
    if (logIndex >= 0) {
        m_contestSelector->setCurrentIndex(logIndex);
    }
    if (!m_syncClient->startSynchronization(m_repository, &error)) {
        QMessageBox::critical(
            this, tr("Unable to start synchronization"), error);
        m_syncClient->disconnectFromServer();
        return;
    }
    statusBar()->showMessage(
        tr("Local log “%1” is merging with its central copy; synchronization "
           "will reconcile identical records and report conflicts.")
            .arg(logName), 15000);
}

void MainWindow::disconnectNetworkConnection()
{
    m_networkLogId = 0;
    if (m_hostServer) {
        configureHostMode();
    }
    if (!m_syncClient) {
        statusBar()->showMessage(tr("No central network connection is active."), 5000);
        return;
    }
    m_syncClient->stopSynchronization();
    m_syncClient->disconnectFromServer();
    statusBar()->showMessage(
        tr("Disconnected. Local logs and pending synchronization data were retained."),
        7000);
}

void MainWindow::showNetworkStatus()
{
    const Contest *active = activeContest();
    const QString configuredLogUuid =
        QSettings().value(QStringLiteral("network/client/logUuid")).toString();
    const QString logUuid = active != nullptr && !active->logUuid.isEmpty()
        ? active->logUuid : configuredLogUuid;
    if (logUuid.isEmpty()) {
        QMessageBox::information(
            this, tr("Network status"),
            tr("No synchronized log is selected or configured."));
        return;
    }

    QString error;
    const QVector<Contest> logs = m_repository.contests(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to load local logs"), error);
        return;
    }
    const auto log = std::find_if(logs.cbegin(), logs.cend(),
        [&logUuid](const Contest &candidate) { return candidate.logUuid == logUuid; });
    const QString logName = log == logs.cend() ? logUuid : log->name;
    const QString stationUuid = m_repository.localStationUuid(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to identify this station"), error);
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("networkStatusDialog"));
    dialog.setWindowTitle(tr("Network status and synchronization queue"));
    dialog.resize(1000, 700);
    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(&dialog);
    summary->setObjectName(QStringLiteral("networkStatusSummary"));
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *stationsStatus = new QLabel(&dialog);
    stationsStatus->setObjectName(QStringLiteral("connectedStationsStatus"));
    stationsStatus->setWordWrap(true);
    layout->addWidget(stationsStatus);
    auto *stations = new QTableWidget(&dialog);
    stations->setObjectName(QStringLiteral("connectedStationsTable"));
    stations->setColumnCount(6);
    stations->setHorizontalHeaderLabels(
        {tr("Computer"), tr("Station callsign"), tr("Operator"),
         tr("Band"), tr("Mode"), tr("State")});
    stations->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stations->setSelectionBehavior(QAbstractItemView::SelectRows);
    stations->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(stations);

    auto *queue = new QTableWidget(&dialog);
    queue->setObjectName(QStringLiteral("networkOutboxTable"));
    queue->setColumnCount(5);
    queue->setHorizontalHeaderLabels(
        {tr("Operation"), tr("Entity"), tr("State"), tr("Attempts"), tr("Last error")});
    queue->setSelectionBehavior(QAbstractItemView::SelectRows);
    queue->setSelectionMode(QAbstractItemView::SingleSelection);
    queue->setEditTriggers(QAbstractItemView::NoEditTriggers);
    queue->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(queue, 1);

    auto *serialStatus = new QTableWidget(&dialog);
    serialStatus->setObjectName(QStringLiteral("serialReservationStatusTable"));
    serialStatus->setColumnCount(9);
    serialStatus->setHorizontalHeaderLabels(
        {tr("Type"), tr("Reservation"), tr("Serial / scope"), tr("Band"),
         tr("Mode"), tr("State"), tr("Station / QSO"), tr("Created"),
         tr("Updated")});
    serialStatus->setEditTriggers(QAbstractItemView::NoEditTriggers);
    serialStatus->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(serialStatus);

    auto *buttons = new QHBoxLayout;
    auto *syncRunStatus = new QLabel(
        tr("No manual synchronization run in this window."), &dialog);
    syncRunStatus->setObjectName(QStringLiteral("syncRunStatus"));
    layout->addWidget(syncRunStatus);
    auto *refresh = new QPushButton(tr("Refresh"), &dialog);
    refresh->setObjectName(QStringLiteral("refreshNetworkStatusButton"));
    auto *syncNow = new QPushButton(tr("Sync now"), &dialog);
    syncNow->setObjectName(QStringLiteral("syncNowButton"));
    auto *checkConsistency = new QPushButton(tr("Check consistency"), &dialog);
    checkConsistency->setObjectName(QStringLiteral("checkConsistencyButton"));
    auto *serverMetrics = new QPushButton(tr("Server metrics..."), &dialog);
    serverMetrics->setObjectName(QStringLiteral("serverMetricsButton"));
    auto *adminSerials = new QPushButton(tr("Admin serials..."), &dialog);
    adminSerials->setObjectName(QStringLiteral("adminSerialReservationsButton"));
    auto *retryFailed = new QPushButton(tr("Retry selected failed operation"), &dialog);
    retryFailed->setObjectName(QStringLiteral("retrySyncOperationButton"));
    auto *compareConflict = new QPushButton(tr("Compare conflict..."), &dialog);
    compareConflict->setObjectName(QStringLiteral("compareSyncConflictButton"));
    compareConflict->setEnabled(false);
    auto *exportReport = new QPushButton(tr("Export diagnostics..."), &dialog);
    exportReport->setObjectName(QStringLiteral("exportSyncDiagnosticsButton"));
    auto *close = new QPushButton(tr("Close"), &dialog);
    buttons->addWidget(refresh);
    buttons->addWidget(syncNow);
    buttons->addWidget(checkConsistency);
    buttons->addWidget(serverMetrics);
    buttons->addWidget(adminSerials);
    buttons->addWidget(retryFailed);
    buttons->addWidget(compareConflict);
    buttons->addWidget(exportReport);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (m_syncClient) {
        connect(m_syncClient.get(), &SyncClient::synchronizationStarted,
                &dialog, [syncRunStatus] {
                    syncRunStatus->setText(QObject::tr("Synchronization in progress..."));
                });
        connect(m_syncClient.get(), &SyncClient::synchronizationFinished,
                &dialog, [syncRunStatus](int uploaded, int downloaded,
                                         qint64 elapsedMs, bool success,
                                         const QString &detail) {
                    syncRunStatus->setText(
                        QObject::tr("%1 Uploaded: %2; downloaded: %3; elapsed: %4 ms.")
                            .arg(detail)
                            .arg(uploaded)
                            .arg(downloaded)
                            .arg(elapsedMs));
                    syncRunStatus->setProperty("success", success);
                });
    }

    QVector<SyncOutboxOperation> operations;
    QVector<SyncConflict> conflicts;
    QJsonObject serverMetricsSnapshot;
    QString stationStatusRequestId;
    QString consistencyDigestRequestId;
    const auto showStations = [stations, stationsStatus, stationUuid](
                                  const QJsonArray &connected) {
        stations->setSortingEnabled(false);
        stations->setRowCount(connected.size());
        for (int row = 0; row < connected.size(); ++row) {
            const QJsonObject station = connected.at(row).toObject();
            const QString uuid = station.value(QStringLiteral("station_uuid")).toString();
            const auto description = [&station](const QString &key) {
                const QString value = station.value(key).toString().trimmed();
                return value.isEmpty() ? tr("Not provided") : value;
            };
            const QStringList values{
                description(QStringLiteral("computer_name")),
                description(QStringLiteral("station_callsign")),
                description(QStringLiteral("operator_name")),
                description(QStringLiteral("operating_band")),
                description(QStringLiteral("operating_mode")),
                uuid == stationUuid ? tr("Connected (this station)") : tr("Connected")
            };
            for (int column = 0; column < values.size(); ++column) {
                stations->setItem(row, column, new QTableWidgetItem(values.at(column)));
            }
        }
        stations->setSortingEnabled(true);
        stations->sortItems(0);
        stations->resizeColumnsToContents();
        stationsStatus->setText(tr("Connected stations for this log: %1")
                                    .arg(connected.size()));
    };
    const auto refreshStations = [this, stations, stationsStatus, showStations,
                                  logUuid, &stationStatusRequestId] {
        if (m_syncClient && m_syncClient->state() == SyncClient::State::Ready
            && QSettings().value(QStringLiteral("network/client/logUuid")).toString()
                == logUuid) {
            if (stationStatusRequestId.isEmpty()) {
                stationStatusRequestId = m_syncClient->getStatus();
                if (stationStatusRequestId.isEmpty()) {
                    stations->setRowCount(0);
                    stationsStatus->setText(tr("Unable to request connected stations."));
                }
            }
        } else {
            stationStatusRequestId.clear();
            if (m_hostServer) {
                showStations(m_hostServer->connectedStations(logUuid));
            } else {
                stations->setRowCount(0);
                stationsStatus->setText(
                    tr("Connected stations unavailable: not connected to this log."));
            }
        }
    };
    QTimer stationRefreshTimer(&dialog);
    stationRefreshTimer.setObjectName(QStringLiteral("connectedStationsRefreshTimer"));
    stationRefreshTimer.setInterval(5000);
    connect(&stationRefreshTimer, &QTimer::timeout, &dialog, refreshStations);
    connect(refresh, &QPushButton::clicked, &dialog, refreshStations);
    const auto connectionStateText = [this] {
        if (!m_syncClient) {
            return tr("Offline (not connected)");
        }
        switch (m_syncClient->state()) {
        case SyncClient::State::Disconnected:
        case SyncClient::State::Backoff:
        case SyncClient::State::Error:
            return tr("Offline");
        case SyncClient::State::Connecting:
        case SyncClient::State::Handshaking:
        case SyncClient::State::Authenticating:
        case SyncClient::State::SelectingLog:
            return tr("Connecting");
        case SyncClient::State::Ready:
            return tr("Connected");
        }
        return tr("Unknown");
    };
    const auto refreshView = [this, summary, queue, serialStatus, retryFailed, syncNow,
                              checkConsistency,
                              serverMetrics, adminSerials, compareConflict, &operations,
                              &conflicts, &error,
                              logName, logUuid, stationUuid,
                              connectionStateText] {
        operations = m_repository.syncOutbox(logUuid, false, &error);
        if (!error.isEmpty()) {
            summary->setText(tr("Unable to load synchronization queue: %1").arg(error));
            queue->setRowCount(0);
            retryFailed->setEnabled(false);
            compareConflict->setEnabled(false);
            return;
        }
        conflicts = m_repository.syncConflicts(logUuid, &error);
        if (!error.isEmpty()) {
            summary->setText(tr("Unable to load synchronization conflicts: %1")
                                 .arg(error));
            queue->setRowCount(0);
            retryFailed->setEnabled(false);
            compareConflict->setEnabled(false);
            return;
        }
        int pendingCount = 0;
        int conflictCount = 0;
        int failedCount = 0;
        int inFlightCount = 0;
        for (const SyncOutboxOperation &operation : operations) {
            if (operation.status != QStringLiteral("accepted")) {
                ++pendingCount;
            }
            if (operation.status == QStringLiteral("conflict")) {
                ++conflictCount;
            } else if (operation.status == QStringLiteral("failed")) {
                ++failedCount;
            } else if (operation.status == QStringLiteral("in-flight")) {
                ++inFlightCount;
            }
        }
        const qint64 cursor = m_repository.syncCursor(logUuid, &error);
        if (!error.isEmpty()) {
            summary->setText(tr("Unable to read synchronization cursor: %1").arg(error));
            queue->setRowCount(0);
            retryFailed->setEnabled(false);
            compareConflict->setEnabled(false);
            return;
        }
        const qint64 serverRevision =
            m_syncClient ? m_syncClient->currentRevision() : 0;
        QString operatorState = connectionStateText();
        if (operatorState == QLatin1String("Connected")) {
            if (m_networkReadOnly) {
                operatorState = tr("Read-only");
            } else if (conflictCount > 0) {
                operatorState = tr("Conflict");
            } else if (inFlightCount > 0) {
                operatorState = tr("Syncing");
            } else if (pendingCount > 0) {
                operatorState = tr("Pending");
            } else {
                operatorState = tr("Connected");
            }
        } else if (operatorState.startsWith(QLatin1String("Offline"))
                   && pendingCount > 0) {
            operatorState = tr("Offline (%1 pending)").arg(pendingCount);
        }
        const QString lastContact = m_networkLastContactUtc.isValid()
            ? m_networkLastContactUtc.toLocalTime().toString(Qt::ISODate)
            : tr("never");
        summary->setText(
            tr("State: %1    Log: %2 (%3)    Station UUID: %4\n"
               "Applied revision: %5    Server revision: %6    Last contact: %7\n"
               "Pending operations: %8    Conflicts: %9    Failed: %10")
                .arg(operatorState, logName, logUuid, stationUuid)
                .arg(cursor)
                .arg(serverRevision)
                .arg(lastContact)
                .arg(pendingCount)
                .arg(conflictCount)
                .arg(failedCount));
        queue->setRowCount(operations.size());
        for (int row = 0; row < operations.size(); ++row) {
            const SyncOutboxOperation &operation = operations.at(row);
            const QStringList values{
                operation.operation, operation.entityUuid, operation.status,
                QString::number(operation.attempts), operation.lastError
            };
            for (int column = 0; column < values.size(); ++column) {
                queue->setItem(row, column, new QTableWidgetItem(values.at(column)));
            }
        }
        queue->resizeColumnsToContents();
        const QVector<SerialReservation> reservations =
            m_repository.serialReservations(logUuid, &error);
        if (!error.isEmpty()) {
            summary->setText(tr("Unable to load local serial reservations: %1")
                                 .arg(error));
            serialStatus->setRowCount(0);
            retryFailed->setEnabled(false);
            compareConflict->setEnabled(false);
            return;
        }
        const QVector<SerialReservationRequest> serialRequests =
            m_repository.pendingSerialReservationRequests(
                logUuid, stationUuid, &error);
        if (!error.isEmpty()) {
            summary->setText(tr("Unable to load pending serial requests: %1")
                                 .arg(error));
            serialStatus->setRowCount(0);
            retryFailed->setEnabled(false);
            compareConflict->setEnabled(false);
            return;
        }
        const int reservationRows = reservations.size() + serialRequests.size();
        serialStatus->setRowCount(reservationRows);
        int row = 0;
        for (const SerialReservation &reservation : reservations) {
            const QStringList values{
                tr("Reservation"), reservation.reservationUuid,
                QStringLiteral("%1 / %2")
                    .arg(reservation.serialNumber)
                    .arg(reservation.scopeKey),
                reservation.band, reservation.mode, reservation.status,
                reservation.stationUuid
                    + (reservation.qsoUuid.isEmpty()
                           ? QString() : QStringLiteral(" / ") + reservation.qsoUuid),
                reservation.createdAtUtc.toLocalTime().toString(Qt::ISODate),
                reservation.updatedAtUtc.toLocalTime().toString(Qt::ISODate)
            };
            for (int column = 0; column < values.size(); ++column) {
                serialStatus->setItem(row, column,
                                      new QTableWidgetItem(values.at(column)));
            }
            ++row;
        }
        const bool offline = connectionStateText().startsWith(
            QLatin1String("Offline"));
        for (const SerialReservationRequest &request : serialRequests) {
            const QStringList values{
                request.operation, request.reservationUuid,
                request.operation == QStringLiteral("reserve_serial")
                    ? tr("No number allocated") : QString(),
                request.band, request.mode,
                offline ? tr("Queued offline") : tr("Waiting for server"),
                request.stationUuid,
                request.createdAtUtc.toLocalTime().toString(Qt::ISODate),
                QString()
            };
            for (int column = 0; column < values.size(); ++column) {
                serialStatus->setItem(row, column,
                                      new QTableWidgetItem(values.at(column)));
            }
            ++row;
        }
        serialStatus->resizeColumnsToContents();
        retryFailed->setEnabled(false);
        syncNow->setEnabled(m_syncClient
                            && m_syncClient->state() == SyncClient::State::Ready
                            && QSettings().value(
                                   QStringLiteral("network/client/logUuid")).toString()
                                == logUuid);
        checkConsistency->setEnabled(
            operations.isEmpty() && m_syncClient
            && m_syncClient->state() == SyncClient::State::Ready
            && !m_syncClient->isReadOnly()
            && QSettings().value(
                   QStringLiteral("network/client/logUuid")).toString() == logUuid);
        serverMetrics->setEnabled(m_syncClient
                                  && m_syncClient->state()
                                      == SyncClient::State::Ready
                                  && QSettings().value(
                                         QStringLiteral("network/client/logUuid"))
                                         .toString() == logUuid);
        adminSerials->setEnabled(m_syncClient
                                 && m_syncClient->state() == SyncClient::State::Ready
                                 && QSettings().value(
                                        QStringLiteral("network/client/logUuid"))
                                        .toString() == logUuid);
    };
    connect(refresh, &QPushButton::clicked, &dialog, refreshView);
    connect(serverMetrics, &QPushButton::clicked, &dialog, [this] {
        if (!m_syncClient || m_syncClient->getMetrics().isEmpty()) {
            showOperatorMessage(
                tr("Server metrics are unavailable while disconnected."), true);
        }
    });
    connect(adminSerials, &QPushButton::clicked, &dialog, [this, logUuid] {
        if (!m_syncClient
            || QSettings().value(QStringLiteral("network/client/logUuid"))
                   .toString() != logUuid
            || (m_pendingAdminSerialRequestId =
                    m_syncClient->getSerialReservations()).isEmpty()) {
            showOperatorMessage(
                tr("Central serial administration is unavailable while disconnected."),
                true);
        }
    });
    connect(queue, &QTableWidget::itemSelectionChanged, &dialog,
            [this, queue, retryFailed, compareConflict, &operations, &conflicts] {
                const int row = queue->currentRow();
                retryFailed->setEnabled(
                    !m_networkReadOnly && row >= 0 && row < operations.size()
                    && operations.at(row).status == QStringLiteral("failed"));
                compareConflict->setEnabled(
                    row >= 0 && row < operations.size()
                    && operations.at(row).status == QStringLiteral("conflict")
                    && std::any_of(conflicts.cbegin(), conflicts.cend(),
                        [&operations, row](const SyncConflict &conflict) {
                            return conflict.operationUuid
                                == operations.at(row).operationUuid;
                        }));
            });
    connect(compareConflict, &QPushButton::clicked, &dialog,
            [this, &dialog, queue, &operations, &conflicts, refreshView, logUuid] {
                const int row = queue->currentRow();
                if (row < 0 || row >= operations.size()
                    || operations.at(row).status != QStringLiteral("conflict")) {
                    return;
                }
                const auto conflict = std::find_if(
                    conflicts.cbegin(), conflicts.cend(),
                    [&operations, row](const SyncConflict &candidate) {
                        return candidate.operationUuid
                            == operations.at(row).operationUuid;
                    });
                if (conflict == conflicts.cend()) {
                    QMessageBox::warning(
                        &dialog, QObject::tr("Conflict details unavailable"),
                        QObject::tr("The conflict record is not present in the local "
                                    "database."));
                    return;
                }
                const QJsonDocument localDocument =
                    QJsonDocument::fromJson(conflict->localPayloadJson.toUtf8());
                const QJsonDocument remoteDocument =
                    QJsonDocument::fromJson(conflict->remotePayloadJson.toUtf8());
                if (!localDocument.isObject() || !remoteDocument.isObject()) {
                    QMessageBox::critical(
                        &dialog, QObject::tr("Invalid conflict data"),
                        QObject::tr("Stored conflict payload is not valid JSON."));
                    return;
                }
                QJsonObject local = localDocument.object();
                QJsonObject remote = remoteDocument.object();
                if (local.value(QStringLiteral("qso")).isObject()) {
                    local = local.value(QStringLiteral("qso")).toObject();
                }
                if (remote.value(QStringLiteral("qso")).isObject()) {
                    remote = remote.value(QStringLiteral("qso")).toObject();
                }
                const QStringList fields{
                    QStringLiteral("callsign"),
                    QStringLiteral("station_callsign"),
                    QStringLiteral("band"),
                    QStringLiteral("timestamp_utc"),
                    QStringLiteral("frequency_hz"),
                    QStringLiteral("mode"),
                    QStringLiteral("sent_exchange"),
                    QStringLiteral("received_exchange"),
                    QStringLiteral("sent_exchange_fields"),
                    QStringLiteral("received_exchange_fields"),
                    QStringLiteral("points")
                };

                QDialog comparison(&dialog);
                comparison.setObjectName(QStringLiteral("syncConflictComparisonDialog"));
                comparison.setWindowTitle(QObject::tr("Merge local and server versions"));
                comparison.resize(850, 520);
                auto *comparisonLayout = new QVBoxLayout(&comparison);
                auto *caption = new QLabel(
                    QObject::tr("Conflict for entity %1. Select which version to use "
                                "for each field, then merge against the latest server version.")
                        .arg(conflict->entityUuid),
                    &comparison);
                caption->setWordWrap(true);
                comparisonLayout->addWidget(caption);
                auto *comparisonTable = new QTableWidget(&comparison);
                comparisonTable->setObjectName(QStringLiteral("syncConflictComparison"));
                comparisonTable->setColumnCount(4);
                comparisonTable->setHorizontalHeaderLabels(
                    {QObject::tr("Field"), QObject::tr("Local"), QObject::tr("Server"),
                     QObject::tr("Use")});
                comparisonTable->setRowCount(fields.size());
                comparisonTable->horizontalHeader()->setStretchLastSection(true);
                comparisonTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
                const auto displayValue = [](const QJsonValue &value) {
                    if (value.isUndefined()) {
                        return QObject::tr("<missing>");
                    }
                    if (value.isObject()) {
                        return QString::fromUtf8(
                            QJsonDocument(value.toObject())
                                .toJson(QJsonDocument::Compact));
                    }
                    if (value.isArray()) {
                        return QString::fromUtf8(
                            QJsonDocument(value.toArray())
                                .toJson(QJsonDocument::Compact));
                    }
                    return value.toVariant().toString();
                };
                for (int fieldRow = 0; fieldRow < fields.size(); ++fieldRow) {
                    const QString &field = fields.at(fieldRow);
                    comparisonTable->setItem(
                        fieldRow, 0, new QTableWidgetItem(field));
                    comparisonTable->setItem(
                        fieldRow, 1,
                        new QTableWidgetItem(displayValue(local.value(field))));
                    comparisonTable->setItem(
                        fieldRow, 2,
                        new QTableWidgetItem(displayValue(remote.value(field))));
                    auto *choice = new QComboBox(comparisonTable);
                    choice->setObjectName(
                        QStringLiteral("syncConflictChoice_%1").arg(field));
                    choice->addItem(QObject::tr("Local"), 0);
                    choice->addItem(QObject::tr("Server"), 1);
                    comparisonTable->setCellWidget(fieldRow, 3, choice);
                }
                comparisonTable->resizeColumnsToContents();
                comparisonLayout->addWidget(comparisonTable, 1);
                auto *acceptServer = new QPushButton(
                    QObject::tr("Accept server version"), &comparison);
                acceptServer->setObjectName(QStringLiteral("acceptServerConflictButton"));
                acceptServer->setEnabled(!m_networkReadOnly);
                auto *keepLocal = new QPushButton(
                    QObject::tr("Keep local as new edit"), &comparison);
                keepLocal->setObjectName(QStringLiteral("keepLocalConflictButton"));
                keepLocal->setEnabled(!m_networkReadOnly);
                auto *mergeSelected = new QPushButton(
                    QObject::tr("Merge selected fields"), &comparison);
                mergeSelected->setObjectName(
                    QStringLiteral("mergeSelectedConflictButton"));
                mergeSelected->setEnabled(!m_networkReadOnly);
                auto *closeComparison =
                    new QPushButton(QObject::tr("Close"), &comparison);
                QObject::connect(closeComparison, &QPushButton::clicked,
                                 &comparison, &QDialog::accept);
                auto *comparisonButtons = new QHBoxLayout;
                comparisonButtons->addWidget(acceptServer);
                comparisonButtons->addWidget(keepLocal);
                comparisonButtons->addWidget(mergeSelected);
                comparisonButtons->addStretch();
                comparisonButtons->addWidget(closeComparison);
                comparisonLayout->addLayout(comparisonButtons);
                const QString conflictOperationUuid = conflict->operationUuid;
                QObject::connect(mergeSelected, &QPushButton::clicked,
                                 &comparison,
                                 [this, &comparison, &dialog, refreshView, logUuid,
                                  comparisonTable, fields, local, remote,
                                  conflictOperationUuid] {
                    QJsonObject merged;
                    for (int row = 0; row < fields.size(); ++row) {
                        auto *choice = qobject_cast<QComboBox *>(
                            comparisonTable->cellWidget(row, 3));
                        if (choice == nullptr) {
                            QMessageBox::critical(
                                &comparison, tr("Unable to merge conflict"),
                                tr("A field selection is unavailable."));
                            return;
                        }
                        const QJsonObject &source = choice->currentData().toInt() == 0
                            ? local : remote;
                        const QJsonValue value = source.value(fields.at(row));
                        if (value.isUndefined()) {
                            QMessageBox::critical(
                                &comparison, tr("Unable to merge conflict"),
                                tr("The selected version is missing field %1.")
                                    .arg(fields.at(row)));
                            return;
                        }
                        merged.insert(fields.at(row), value);
                    }
                    const auto answer = QMessageBox::question(
                        &comparison, tr("Merge QSO conflict"),
                        tr("Save the selected field values as a new update against "
                           "the latest server version? The merged result and both "
                           "original versions will be recorded in the audit."),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (answer != QMessageBox::Yes) {
                        return;
                    }
                    QString resolutionError;
                    if (!m_repository.resolveSyncConflictMerge(
                            conflictOperationUuid, merged, &resolutionError)) {
                        QMessageBox::critical(
                            &comparison, tr("Unable to resolve conflict"),
                            resolutionError);
                        return;
                    }
                    refreshRecentQsos();
                    refreshBandmap();
                    refreshScoreSummary();
                    refreshView();
                    if (m_syncClient
                        && QSettings().value(
                               QStringLiteral("network/client/logUuid")).toString()
                            == logUuid
                        && m_syncClient->state() == SyncClient::State::Ready) {
                        m_syncClient->synchronizeNow();
                    }
                    comparison.accept();
                    dialog.setProperty("lastConflictResolution",
                                       QStringLiteral("merge"));
                });
                QObject::connect(acceptServer, &QPushButton::clicked,
                                 &comparison,
                                 [this, &comparison, &dialog, refreshView,
                                  conflictOperationUuid] {
                    const auto answer = QMessageBox::question(
                        &comparison, tr("Accept server version"),
                        tr("Replace the local QSO with the server version? The "
                           "conflicting local edit will remain in the resolution audit."),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (answer != QMessageBox::Yes) {
                        return;
                    }
                    QString resolutionError;
                    if (!m_repository.resolveSyncConflictAcceptServer(
                            conflictOperationUuid, &resolutionError)) {
                        QMessageBox::critical(
                            &comparison, tr("Unable to resolve conflict"),
                            resolutionError);
                        return;
                    }
                    refreshRecentQsos();
                    refreshBandmap();
                    refreshScoreSummary();
                    refreshView();
                    comparison.accept();
                    dialog.setProperty("lastConflictResolution",
                                       QStringLiteral("accept_server"));
                });
                QObject::connect(keepLocal, &QPushButton::clicked,
                                 &comparison,
                                 [this, &comparison, &dialog, refreshView, logUuid,
                                  conflictOperationUuid] {
                    const auto answer = QMessageBox::question(
                        &comparison, tr("Keep local version"),
                        tr("Create a new update using the local QSO against the "
                           "latest server version? The server may still reject it "
                           "if another station changes the QSO first."),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (answer != QMessageBox::Yes) {
                        return;
                    }
                    QString resolutionError;
                    if (!m_repository.resolveSyncConflictKeepLocal(
                            conflictOperationUuid, &resolutionError)) {
                        QMessageBox::critical(
                            &comparison, tr("Unable to resolve conflict"),
                            resolutionError);
                        return;
                    }
                    refreshRecentQsos();
                    refreshBandmap();
                    refreshScoreSummary();
                    refreshView();
                    if (m_syncClient
                        && QSettings().value(
                               QStringLiteral("network/client/logUuid")).toString()
                            == logUuid
                        && m_syncClient->state() == SyncClient::State::Ready) {
                        m_syncClient->synchronizeNow();
                    }
                    comparison.accept();
                    dialog.setProperty("lastConflictResolution",
                                       QStringLiteral("keep_local"));
                });
                comparison.exec();
            });
    connect(retryFailed, &QPushButton::clicked, &dialog,
            [this, &dialog, queue, &operations, logUuid, refreshView] {
                if (m_networkReadOnly) {
                    showOperatorMessage(
                        tr("This station has read-only access; queued changes cannot "
                           "be retried."),
                        true);
                    return;
                }
                const int row = queue->currentRow();
                if (row < 0 || row >= operations.size()
                    || operations.at(row).status != QStringLiteral("failed")) {
                    return;
                }
                QString retryError;
                if (!m_repository.retrySyncOperation(
                        operations.at(row).operationUuid, &retryError)) {
                    QMessageBox::critical(&dialog, tr("Unable to retry operation"),
                                          retryError);
                    return;
                }
                refreshView();
                if (m_syncClient
                    && QSettings().value(QStringLiteral("network/client/logUuid"))
                           .toString() == logUuid
                    && m_syncClient->state() == SyncClient::State::Ready) {
                    m_syncClient->synchronizeNow();
                }
            });
    connect(syncNow, &QPushButton::clicked, &dialog,
            [this, &dialog, logUuid, refreshView, syncRunStatus] {
                if (!m_syncClient
                    || QSettings().value(QStringLiteral("network/client/logUuid"))
                           .toString() != logUuid
                    || !m_syncClient->synchronizeNow()) {
                    QMessageBox::warning(
                        &dialog, tr("Synchronization unavailable"),
                        tr("Connect this log to its central server before synchronizing."));
                    syncRunStatus->setText(tr("Synchronization could not be started."));
                }
                refreshView();
            });
    connect(checkConsistency, &QPushButton::clicked, &dialog,
            [this, &dialog, logUuid, syncRunStatus,
             &consistencyDigestRequestId] {
                if (!m_syncClient || !consistencyDigestRequestId.isEmpty()) {
                    return;
                }
                consistencyDigestRequestId = m_syncClient->checkConsistency();
                if (consistencyDigestRequestId.isEmpty()) {
                    syncRunStatus->setText(
                        tr("Synchronize the log and resolve queued operations before auditing."));
                    return;
                }
                syncRunStatus->setText(tr("Checking replica consistency..."));
            });
    connect(exportReport, &QPushButton::clicked, &dialog,
            [this, &dialog, logName, logUuid, stationUuid, &operations,
             &serverMetricsSnapshot, connectionStateText] {
                const QString path = QFileDialog::getSaveFileName(
                    &dialog, tr("Export synchronization diagnostics"),
                    QStringLiteral("sync-diagnostics.json"),
                    tr("JSON files (*.json)"));
                if (path.isEmpty()) {
                    return;
                }
                QJsonArray operationSummary;
                for (const SyncOutboxOperation &operation : operations) {
                    operationSummary.append(QJsonObject{
                        {QStringLiteral("operation_uuid"), operation.operationUuid},
                        {QStringLiteral("entity_uuid"), operation.entityUuid},
                        {QStringLiteral("operation"), operation.operation},
                        {QStringLiteral("status"), operation.status},
                        {QStringLiteral("attempts"), operation.attempts},
                        {QStringLiteral("last_error"), operation.lastError}
                    });
                }
                QString cursorError;
                const qint64 cursor = m_repository.syncCursor(logUuid, &cursorError);
                if (!cursorError.isEmpty()) {
                    QMessageBox::critical(&dialog,
                                          tr("Unable to export diagnostics"),
                                          cursorError);
                    return;
                }
                QJsonObject report{
                    {QStringLiteral("generated_at_utc"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("connection_state"), connectionStateText()},
                    {QStringLiteral("host"),
                     QSettings().value(QStringLiteral("network/client/host")).toString()},
                    {QStringLiteral("port"),
                     QSettings().value(QStringLiteral("network/client/port")).toInt()},
                    {QStringLiteral("log_name"), logName},
                    {QStringLiteral("log_uuid"), logUuid},
                    {QStringLiteral("station_uuid"), stationUuid},
                    {QStringLiteral("server_uuid"),
                     m_syncClient ? m_syncClient->serverUuid() : QString()},
                    {QStringLiteral("server_revision"),
                     m_syncClient ? m_syncClient->currentRevision() : 0},
                    {QStringLiteral("applied_revision"), cursor},
                    {QStringLiteral("last_contact_utc"),
                     m_networkLastContactUtc.isValid()
                         ? m_networkLastContactUtc.toString(Qt::ISODateWithMs)
                         : QString()},
                    {QStringLiteral("operations"), operationSummary}
                };
                if (serverMetricsSnapshot.value(QStringLiteral("log_uuid"))
                        .toString() == logUuid) {
                    report.insert(QStringLiteral("server_metrics"),
                                  serverMetricsSnapshot);
                }
                const QByteArray reportBytes =
                    QJsonDocument(report).toJson(QJsonDocument::Indented);
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly)
                    || file.write(reportBytes) != reportBytes.size()
                    || !file.commit()) {
                    QMessageBox::critical(&dialog,
                                          tr("Unable to export diagnostics"),
                                          file.errorString());
                }
            });
    if (m_syncClient) {
        connect(m_syncClient.get(), &SyncClient::stateChanged, &dialog,
                [refreshView, refreshStations](SyncClient::State, const QString &) {
                    refreshView();
                    refreshStations();
                });
        connect(m_syncClient.get(), &SyncClient::responseReceived, &dialog,
                [this, &dialog, refreshView, showStations, stations, stationsStatus, logUuid,
                 &stationStatusRequestId, &serverMetricsSnapshot,
                 &consistencyDigestRequestId, syncRunStatus](
                    const QString &requestId, const QString &type,
                    const QJsonObject &payload) {
                    recordNetworkContact();
                    if (requestId == stationStatusRequestId
                        && !stationStatusRequestId.isEmpty()) {
                        stationStatusRequestId.clear();
                        const QJsonValue connected =
                            payload.value(QStringLiteral("connected_stations"));
                        bool valid = type == QStringLiteral("status")
                            && payload.value(QStringLiteral("log_uuid")).toString() == logUuid
                            && connected.isArray();
                        for (const QJsonValue &value : connected.toArray()) {
                            const QJsonObject station = value.toObject();
                            valid = valid && value.isObject()
                                && !QUuid(station.value(
                                    QStringLiteral("station_uuid")).toString()).isNull()
                                && !QUuid(station.value(
                                    QStringLiteral("session_uuid")).toString()).isNull();
                            for (const QString &field : {
                                     QStringLiteral("computer_name"),
                                     QStringLiteral("station_callsign"),
                                     QStringLiteral("operator_name"),
                                     QStringLiteral("operating_band"),
                                     QStringLiteral("operating_mode"),
                                     QStringLiteral("operating_state_updated_utc")}) {
                                valid = valid && (!station.contains(field)
                                    || station.value(field).isString());
                            }
                        }
                        if (valid) {
                            showStations(connected.toArray());
                        } else {
                            stations->setRowCount(0);
                            stationsStatus->setText(
                                tr("Server returned an invalid connected stations list."));
                        }
                    }
                    if (type == QStringLiteral("metrics")) {
                        serverMetricsSnapshot = payload;
                    }
                    if (!consistencyDigestRequestId.isEmpty()
                        && requestId == consistencyDigestRequestId) {
                        consistencyDigestRequestId.clear();
                        QString digestError;
                        const qint64 cursor =
                            m_repository.syncCursor(logUuid, &digestError);
                        QJsonObject localDigest =
                            m_repository.syncConsistencyDigest(logUuid, &digestError);
                        QJsonObject serverDigest = payload;
                        const qint64 serverRevision =
                            serverDigest.take(QStringLiteral("revision")).toInteger(-1);
                        const bool valid = digestError.isEmpty()
                            && type == QStringLiteral("consistency_digest")
                            && serverRevision == cursor
                            && serverDigest.value(
                                QStringLiteral("digest_version")).toInt(-1) == 1
                            && serverDigest.value(QStringLiteral("count")).isDouble()
                            && serverDigest.value(QStringLiteral("groups")).isObject();
                        const bool consistent =
                            valid && serverDigest == localDigest;
                        if (consistent) {
                            syncRunStatus->setText(
                                tr("Consistency verified at revision %1 (%2 QSOs).")
                                    .arg(cursor)
                                    .arg(localDigest.value(
                                        QStringLiteral("count")).toInt()));
                            syncRunStatus->setProperty("consistency", "verified");
                        } else {
                            const QString detail = !digestError.isEmpty()
                                ? digestError
                                : valid
                                    ? tr("Replica mismatch at revision %1. No local data was changed.")
                                          .arg(cursor)
                                    : tr("Consistency result was invalid or the log advanced. Run synchronization and check again.");
                            syncRunStatus->setText(detail);
                            syncRunStatus->setProperty("consistency", "mismatch");
                            if (valid) {
                                QMessageBox::warning(
                                    &dialog, tr("Replica consistency mismatch"),
                                    detail + QLatin1Char('\n')
                                        + tr("Review diagnostics before considering snapshot recovery."));
                            }
                        }
                    }
                    refreshView();
                });
        connect(m_syncClient.get(), &SyncClient::requestFailed, &dialog,
                [refreshView, stations, stationsStatus, &stationStatusRequestId,
                 &consistencyDigestRequestId, syncRunStatus](
                    const QString &requestId, const QString &code,
                    const QString &message, bool) {
                    if (requestId == stationStatusRequestId
                        && !stationStatusRequestId.isEmpty()) {
                        stationStatusRequestId.clear();
                        stations->setRowCount(0);
                        stationsStatus->setText(
                            tr("Unable to load connected stations: %1").arg(message));
                    }
                    if (requestId == consistencyDigestRequestId
                        && !consistencyDigestRequestId.isEmpty()) {
                        consistencyDigestRequestId.clear();
                        syncRunStatus->setText(
                            tr("Consistency audit failed (%1): %2")
                                .arg(code, message));
                    }
                    refreshView();
                });
    }
    refreshView();
    stationsStatus->setText(tr("Loading connected stations..."));
    refreshStations();
    stationRefreshTimer.start();
    dialog.exec();
}

QString MainWindow::operatorIdentity() const
{
    return m_operatorCallsign.isEmpty() ? m_station.operatorName : m_operatorCallsign;
}

void MainWindow::configureOperatorCallsign()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("operatorCallsignDialog"));
    dialog.setWindowTitle(tr("Operator callsign"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *prompt = new QLabel(
        tr("Operator callsign (A-Z, 0-9, /; leave empty to clear):"), &dialog);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);
    auto *edit = new QLineEdit(m_operatorCallsign, &dialog);
    edit->setObjectName(QStringLiteral("activeOperatorCallsign"));
    edit->setMaxLength(20);
    edit->setPlaceholderText(m_station.callsign);
    layout->addWidget(edit);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    const auto validate = [edit, buttons] {
        const QString callsign = edit->text().trimmed().toUpper();
        const bool valid = callsign.isEmpty()
            || QRegularExpression(QStringLiteral("^[A-Z0-9/]{1,20}$"))
                   .match(callsign).hasMatch();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    connect(edit, &QLineEdit::textChanged, &dialog, validate);
    validate();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    edit->selectAll();
    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_operatorCallsign = edit->text().trimmed().toUpper();
    LogSettings(m_repository).setValue(QStringLiteral("station/operatorCallsign"), m_operatorCallsign);
    applyActiveLogStation();
}

void MainWindow::configureOperator()
{
    OperatorDialog dialog(m_station, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const Station updated = dialog.station();
    if (updated.callsign.isEmpty()) {
        QMessageBox::warning(this, tr("Station callsign required"),
                             tr("Enter the station callsign before saving."));
        return;
    }

    m_defaultStation = updated;
    LogSettings settings(m_repository);
    settings.beginGroup(QStringLiteral("station"));
    settings.setValue(QStringLiteral("callsign"), m_defaultStation.callsign);
    settings.setValue(QStringLiteral("operatorName"), m_defaultStation.operatorName);
    settings.setValue(QStringLiteral("grid"), m_defaultStation.grid);
    settings.setValue(QStringLiteral("powerClass"), m_defaultStation.powerClass);
    settings.setValue(QStringLiteral("email"), m_defaultStation.email);
    settings.setValue(QStringLiteral("address"), m_defaultStation.address);
    settings.setValue(QStringLiteral("city"), m_defaultStation.city);
    settings.setValue(QStringLiteral("stateProvince"), m_defaultStation.stateProvince);
    settings.setValue(QStringLiteral("postalCode"), m_defaultStation.postalCode);
    settings.setValue(QStringLiteral("country"), m_defaultStation.country);
    settings.endGroup();
    // The dialog shows the active log's callsign, so an edit there applies to the log.
    const Contest *contest = activeContest();
    if (contest != nullptr && !contest->stationCallsign.isEmpty()
        && contest->stationCallsign != updated.callsign) {
        Contest log = *contest;
        log.stationCallsign = updated.callsign;
        QString error;
        if (!m_repository.updateContestLog(log, &error)) {
            QMessageBox::critical(this, tr("Unable to save log callsign"), error);
        } else {
            const qint64 contestId = log.id;
            for (Contest &stored : m_contests) {
                if (stored.id == contestId) {
                    stored.stationCallsign = updated.callsign;
                }
            }
        }
    }
    applyActiveLogStation();
    statusBar()->showMessage(tr("Station settings saved"), 3000);
}

void MainWindow::configureRadio()
{
    configureRadioContext(0);
}

void MainWindow::configureRadioB()
{
    configureRadioContext(1);
}

void MainWindow::configureRadioContext(int radio)
{
    radio = qBound(0, radio, 1);
    const QString key = QStringLiteral("radio/%1/autoConnect").arg(radio == 0 ? 'A' : 'B');
    RadioConfigDialog dialog(radioConfigFromSettings(radio),
                             radio == 0 ? tr("Radio A") : tr("Radio B"),
                             QSettings().value(key, false).toBool(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const RadioConfig config = dialog.config();
    saveRadioConfig(radio, config);
    QSettings().setValue(key, dialog.autoConnect());
    switch (dialog.action()) {
    case RadioConfigDialog::Action::Connect:
        m_radioControllers[radio].connectRadio(config);
        break;
    case RadioConfigDialog::Action::Disconnect:
        m_radioControllers[radio].disconnectRadio();
        break;
    case RadioConfigDialog::Action::Save:
        statusBar()->showMessage(tr("Radio settings saved"), 3000);
        break;
    }
}

void MainWindow::importLog()
{
    if (m_networkReadOnly) {
        showOperatorMessage(tr("This station has read-only access to the log."), true);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import ADIF / Cabrillo"), {},
        tr("ADIF / Cabrillo logs (*.adi *.adif *.log *.cbr *.cabrillo);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    ImportedLog imported;
    if (!LogImporter::readFile(path, &imported, &error)) {
        QMessageBox::warning(this, tr("Unable to import log"), error);
        return;
    }
    QDialog destination(this);
    destination.setObjectName(QStringLiteral("logImportDestinationDialog"));
    destination.setWindowTitle(tr("Import destination"));
    auto *destinationLayout = new QVBoxLayout(&destination);
    destinationLayout->addWidget(new QLabel(
        tr("%1 QSO records found. Choose the destination log.").arg(imported.records.size()),
        &destination));
    auto *choice = new QComboBox(&destination);
    choice->setObjectName(QStringLiteral("logImportDestination"));
    if (const Contest *current = activeContest()) {
        choice->addItem(tr("Current log: %1").arg(current->name), false);
    }
    choice->addItem(tr("Create a new log..."), true);
    destinationLayout->addWidget(choice);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &destination);
    connect(buttons, &QDialogButtonBox::accepted, &destination, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &destination, &QDialog::reject);
    destinationLayout->addWidget(buttons);
    if (destination.exec() != QDialog::Accepted) {
        return;
    }
    const bool createNew = choice->currentData().toBool();
    Contest target;
    NewLogContestOption selected;
    QMap<QString, QString> sentValues;
    if (createNew) {
        QVector<NewLogContestOption> options;
        if (!newLogOptions(&options, &error)) {
            QMessageBox::warning(this, tr("Unable to load contest definitions"), error);
            return;
        }
        Station station = m_defaultStation;
        if (!imported.stationCallsign.isEmpty()) {
            station.callsign = imported.stationCallsign;
        }
        NewLogDialog dialog(options, station, {}, this);
        for (const auto &option : options) {
            if (option.definition.id.compare(imported.contestCode, Qt::CaseInsensitive) == 0
                || (!option.definition.cabrilloContest.isEmpty()
                    && option.definition.cabrilloContest.compare(
                        imported.contestCode, Qt::CaseInsensitive) == 0)) {
                dialog.selectContest(option.definition.id);
                break;
            }
        }
        if (dialog.exec() != QDialog::Accepted || dialog.selectedOption() == nullptr) {
            return;
        }
        selected = *dialog.selectedOption();
        target = dialog.log();
        sentValues = dialog.sentExchange();
    } else {
        const Contest *current = activeContest();
        const ContestDefinition *definition = activeDefinition();
        if (current == nullptr || definition == nullptr) {
            QMessageBox::warning(this, tr("Unable to import log"),
                                 tr("Select a log with a contest definition."));
            return;
        }
        target = *current;
        selected.definition = *definition;
        if (target.stationCallsign.isEmpty()) {
            target.stationCallsign = m_station.callsign;
        }
    }
    QVector<Qso> existing;
    if (!createNew) {
        existing = m_repository.qsosForContest(target.id, 0, &error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Unable to read log"), error);
            return;
        }
    }
    LogImportPreview preview;
    if (!LogImporter::prepare(imported, selected.definition, target.stationCallsign,
                              existing, multiplierResolver(), &preview, &error)) {
        QMessageBox::warning(this, tr("Unable to import log"), error);
        return;
    }
    if (preview.qsos.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to import"),
            tr("All %1 records already exist in the destination log.").arg(preview.skipped));
        return;
    }
    QString summary = tr("Log: %1\nQSO records to import: %2\nIdentical records skipped: %3\n\n"
                         "Other contest duplicates will be retained with zero points.\n"
                         "Import these records?")
        .arg(target.name).arg(preview.qsos.size()).arg(preview.skipped);
    if (!imported.contestCode.isEmpty()) {
        summary.prepend(tr("Source contest: %1\n").arg(imported.contestCode));
    }
    if (QMessageBox::question(this, tr("Confirm log import"), summary,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
    if (m_networkReadOnly) {
        showOperatorMessage(tr("This station has read-only access to the log."), true);
        return;
    }
    if (createNew) {
        if (createLogFile(target, selected, sentValues, preview.qsos)) {
            showOperatorMessage(tr("Imported %1 QSOs; skipped %2 identical records.")
                                    .arg(preview.qsos.size()).arg(preview.skipped));
        }
        return;
    }
    if (!m_repository.withTransaction([&](QString *operationError) {
            for (Qso &qso : preview.qsos) {
                qso.contestId = target.id;
                if (!m_repository.saveQso(qso, operationError)) {
                    return false;
                }
            }
            return true;
        }, &error)) {
        QMessageBox::critical(this, tr("Import failed — no records saved"), error);
        return;
    }
    refreshRecentQsos();
    updateSerialFields();
    refreshBandmap();
    updateDupeIndicator();
    showOperatorMessage(tr("Imported %1 QSOs; skipped %2 identical records.")
                            .arg(preview.qsos.size()).arg(preview.skipped));
}

void MainWindow::exportCabrillo()
{
    exportCurrentLog(LogExportFormat::Cabrillo);
}

void MainWindow::exportAdif()
{
    exportCurrentLog(LogExportFormat::Adif);
}

void MainWindow::exportCurrentLog(LogExportFormat format)
{
    if (m_contestSelector->currentIndex() < 0) {
        QMessageBox::warning(this, tr("No contest selected"),
                             tr("Select a contest log before exporting."));
        return;
    }
    const qint64 contestId = m_contestSelector->currentData().toLongLong();
    const auto contestIt = std::find_if(
        m_contests.cbegin(), m_contests.cend(),
        [contestId](const Contest &contest) { return contest.id == contestId; });
    if (contestIt == m_contests.cend()) {
        QMessageBox::critical(this, tr("Contest unavailable"),
                              tr("The selected contest could not be loaded."));
        return;
    }
    QString error;
    const QVector<Qso> qsos = m_repository.qsosForContest(contestId, 0, &error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to load log for export"), error);
        return;
    }
    const auto definition = m_definitions.constFind(contestId);
    std::optional<ContestDefinition> exportDefinition;
    if (definition != m_definitions.cend()) {
        exportDefinition = definition.value();
        // Logs created before a built-in definition gained its Cabrillo name keep old YAML.
        if (exportDefinition->cabrilloContest.isEmpty()) {
            exportDefinition->cabrilloContest =
                m_builtInCabrilloContests.value(exportDefinition->id);
        }
    }
    const ContestDefinition *definitionPtr =
        exportDefinition.has_value() ? &*exportDefinition : nullptr;
    LogExportOptions options;
    options.createdBy = QStringLiteral("Contest Logger %1")
                            .arg(QStringLiteral(CONTEST_LOGGER_VERSION));
    if (definitionPtr != nullptr) {
        options.claimedScore =
            ContestEngine().score(*definitionPtr, qsos, multiplierResolver()).finalScore;
    }
    const QString suggestedName = QStringLiteral("%1.%2")
        .arg(contestIt->code.toLower(), format == LogExportFormat::Cabrillo
                                             ? QStringLiteral("log")
                                             : QStringLiteral("adi"));
    const QString path = QFileDialog::getSaveFileName(
        this,
        format == LogExportFormat::Cabrillo ? tr("Export Cabrillo")
                                            : tr("Export ADIF"),
        suggestedName,
        format == LogExportFormat::Cabrillo
            ? tr("Cabrillo logs (*.log);;All files (*)")
            : tr("ADIF logs (*.adi *.adif);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!LogExporter::writeFile(path, format, *contestIt, definitionPtr,
                                qsos, m_station, options, &error)) {
        QMessageBox::critical(this, tr("Export validation or write failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Log exported to %1").arg(path), 5000);
}

void MainWindow::backupDatabase()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Back up SQLite database"),
        QStringLiteral("contest-logger-backup-%1.sqlite")
            .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))),
        tr("SQLite database (*.sqlite *.db)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_repository.backupTo(path, &error)) {
        QMessageBox::critical(this, tr("Database backup failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Verified database backup saved to %1").arg(path), 5000);
}

void MainWindow::configureAutomaticBackups()
{
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Automatic database backups"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *enabled = new QCheckBox(tr("Create backups automatically"), &dialog);
    enabled->setObjectName(QStringLiteral("automaticBackupsEnabled"));
    enabled->setChecked(settings.value(
        QStringLiteral("backups/automaticEnabled"), true).toBool());
    auto *interval = new QSpinBox(&dialog);
    interval->setObjectName(QStringLiteral("automaticBackupIntervalDays"));
    interval->setRange(1, 365);
    interval->setValue(qBound(1, settings.value(
        QStringLiteral("backups/automaticIntervalDays"), 1).toInt(), 365));
    interval->setSuffix(tr(" days"));
    auto *retention = new QSpinBox(&dialog);
    retention->setObjectName(QStringLiteral("automaticBackupRetentionCount"));
    retention->setRange(1, 365);
    retention->setValue(qBound(1, settings.value(
        QStringLiteral("backups/automaticRetentionCount"), 30).toInt(), 365));
    retention->setSuffix(tr(" backups"));
    form->addRow(enabled);
    form->addRow(tr("Minimum interval:"), interval);
    form->addRow(tr("Keep newest:"), retention);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    settings.setValue(QStringLiteral("backups/automaticEnabled"), enabled->isChecked());
    settings.setValue(QStringLiteral("backups/automaticIntervalDays"), interval->value());
    settings.setValue(QStringLiteral("backups/automaticRetentionCount"), retention->value());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(
            this, tr("Unable to save backup settings"),
            tr("The automatic backup settings could not be saved."));
        return;
    }
    statusBar()->showMessage(
        tr("Automatic backup settings saved; they take effect on next startup."), 5000);
}

void MainWindow::configureDxCluster()
{
    QSettings settings;
    DxClusterConfig config{
        settings.value(QStringLiteral("dxCluster/host")).toString(),
        settings.value(QStringLiteral("dxCluster/port"), 7300).toInt(),
        settings.value(QStringLiteral("dxCluster/callsign"), m_station.callsign)
            .toString()
    };
    const int savedSpotLifetimeMinutes = qBound(
        1, settings.value(QStringLiteral("dxCluster/spotLifetimeMinutes"), 60).toInt(),
        1440);
    DxClusterConfigDialog dialog(config, savedSpotLifetimeMinutes, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    config = dialog.config();
    if (config.host.isEmpty() || config.callsign.isEmpty()) {
        QMessageBox::warning(this, tr("DX cluster settings required"),
                             tr("Enter a cluster host and station callsign."));
        return;
    }
    settings.setValue(QStringLiteral("dxCluster/host"), config.host);
    settings.setValue(QStringLiteral("dxCluster/port"), config.port);
    settings.setValue(QStringLiteral("dxCluster/callsign"), config.callsign);
    const int spotLifetimeMinutes = dialog.spotLifetimeMinutes();
    settings.setValue(QStringLiteral("dxCluster/spotLifetimeMinutes"),
                      spotLifetimeMinutes);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(
            this, tr("Unable to save DX cluster settings"),
            tr("The cluster host, callsign, or spot retention could not be saved."));
        return;
    }
    if (!connectDxCluster(config)) {
        QMessageBox::warning(
            this, tr("Unable to save DX cluster connections"),
            tr("The connection was not started because its configuration could not be saved."));
        return;
    }
    refreshBandmap();
}

bool MainWindow::saveDxClusterConfigs()
{
    QVariantList saved;
    for (const DxClusterConfig &config : std::as_const(m_dxClusterConfigs)) {
        saved.append(QVariantMap{
            {QStringLiteral("host"), config.host},
            {QStringLiteral("port"), config.port},
            {QStringLiteral("callsign"), config.callsign}
        });
    }
    QSettings settings;
    settings.setValue(QStringLiteral("dxCluster/connections"), saved);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qWarning().noquote() << "Could not save DX cluster connection settings.";
        return false;
    }
    return true;
}

bool MainWindow::connectDxCluster(const DxClusterConfig &config, bool persist)
{
    int connectionIndex = -1;
    for (int index = 0; index < m_dxClusterConfigs.size(); ++index) {
        const DxClusterConfig &existing = m_dxClusterConfigs.at(index);
        if (existing.host.compare(config.host, Qt::CaseInsensitive) == 0
            && existing.port == config.port) {
            connectionIndex = index;
            break;
        }
    }
    DxClusterController *controller = nullptr;
    if (connectionIndex >= 0) {
        controller = m_dxClusterControllers.at(connectionIndex);
        m_dxClusterConfigs[connectionIndex] = config;
    } else {
        controller = new DxClusterController(this);
        connectionIndex = m_dxClusterControllers.size();
        m_dxClusterControllers.append(controller);
        m_dxClusterConfigs.append(config);
        m_clusterConnectionSelector->addItem(
            QStringLiteral("%1:%2").arg(config.host).arg(config.port),
            connectionIndex);
        connect(controller, &DxClusterController::statusChanged, this,
                [this, controller](const DxClusterStatus &status) {
                    updateDxClusterStatus(controller, status);
                });
        const QString source = QStringLiteral("%1:%2").arg(config.host).arg(config.port);
        connect(controller, &DxClusterController::spotReceived, this,
                [this, source](const Spot &received) {
                    Spot spot = received;
                    spot.source = source;
                    receiveSpot(spot);
                });
    }
    if (persist && !saveDxClusterConfigs()) {
        return false;
    }
    m_clusterConnectionSelector->setCurrentIndex(
        m_clusterConnectionSelector->findData(connectionIndex));
    controller->start(config);
    return true;
}

void MainWindow::disconnectDxClusters()
{
    if (m_dxClusterControllers.isEmpty()) {
        updateDxClusterStatus(
            {ConnectionState::Disconnected, tr("Cluster disconnected."), 0});
        return;
    }
    for (DxClusterController *controller : std::as_const(m_dxClusterControllers)) {
        controller->stop();
    }
}

void MainWindow::sendDxClusterCommand()
{
    const int index = m_clusterConnectionSelector->currentData().toInt();
    if (index < 0 || index >= m_dxClusterControllers.size()) {
        statusBar()->showMessage(tr("Select a DX cluster connection first."), 5000);
        return;
    }
    const QString command = m_clusterCommandInput->text().trimmed();
    if (command.isEmpty()) {
        statusBar()->showMessage(tr("Enter a DX cluster command or spot first."), 5000);
        return;
    }
    m_dxClusterControllers.at(index)->sendCommand(command);
    m_clusterCommandInput->clear();
}

void MainWindow::configureCwKeyer()
{
    const int radioIndex = cwRadioForContext(currentEntryContext());
    QSettings settings;
    CwKeyerConfig config = cwKeyerConfigFromSettings(
        settings, radioIndex, radioConfigFromSettings(radioIndex));
    CwKeyerConfigDialog dialog(config, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    config = dialog.config();
    if (config.serialDevicePath.isEmpty()) {
        QMessageBox::warning(this, tr("Serial device required"),
                             tr("Enter the serial device for the CW keyer."));
        return;
    }
    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(config.type));
    settings.setValue(QStringLiteral("cw/wpm"), config.wpm);
    settings.setValue(QStringLiteral("cw/serial/devicePath"),
                      config.serialDevicePath);
    settings.setValue(QStringLiteral("cw/serial/line"),
                      static_cast<int>(config.serialLine));
    setCwWpm(config.wpm);
    m_cwConfiguredRadio = radioIndex;
    m_cwKeyerController.connectKeyer(config);
}

void MainWindow::showCwInput()
{
    m_cwInputWindow->show();
    m_cwInputWindow->raise();
    m_cwInputWindow->activateWindow();
    m_cwMessageEdit->setFocus(Qt::ShortcutFocusReason);
    m_cwMessageEdit->setCursorPosition(m_cwMessageEdit->text().size());
}

int MainWindow::cwRadioForContext(int context) const
{
    return m_so2rToggle->isChecked()
        ? qBound(0, context, 1) : m_activeRadioSelector->currentData().toInt();
}

bool MainWindow::prepareCwTransmission(int context, QString *problem)
{
    if (m_modeSelectors[context]->currentText().compare(
            QStringLiteral("CW"), Qt::CaseInsensitive) != 0) {
        if (problem != nullptr) {
            *problem = tr("CW transmission requires CW mode on the active entry.");
        }
        return false;
    }
    const int radio = cwRadioForContext(context);
    if (m_cwConfiguredRadio != radio) {
        if (m_cwConfiguredRadio < 0 || m_cwStatus.state != ConnectionState::Connected) {
            if (problem != nullptr) {
                *problem = tr("Connect a CW keyer before sending.");
            }
            return false;
        }
        if (m_cwStatus.processedEnqueues != m_cwEnqueuedRequests
            || m_cwStatus.transmitting || m_cwStatus.queuedMessages > 0) {
            if (problem != nullptr) {
                *problem = tr("Stop CW before switching the transmitting radio.");
            }
            return false;
        }
        QSettings settings;
        CwKeyerConfig config = cwKeyerConfigFromSettings(
            settings, radio, radioConfigFromSettings(radio));
        config.wpm = m_cwWpm;
        m_cwConfiguredRadio = radio;
        m_cwKeyerController.connectKeyer(config);
    } else if (m_cwStatus.state != ConnectionState::Connected) {
        if (problem != nullptr) {
            *problem = tr("Connect a CW keyer before sending.");
        }
        return false;
    }
    const int transmitVfo = qBound(0, m_radioSnapshots[radio].focusVfo, 1);
    const CwKeyerType keyerType = static_cast<CwKeyerType>(
        QSettings().value(QStringLiteral("cw/type"),
                          static_cast<int>(CwKeyerType::Mock)).toInt());
    if (transmitVfo == 1 && keyerType == CwKeyerType::Cwdaemon) {
        if (problem != nullptr) {
            *problem = tr("cwdaemon cannot select a transmit VFO. Select VFO A or use "
                          "a Hamlib keyer for VFO B transmission.");
        }
        return false;
    }
    return true;
}

bool MainWindow::sendCwTemplate(QString message, int context, QString *problem,
                                QString callsignOverride)
{
    context = qBound(0, context, 1);
    if (!prepareCwTransmission(context, problem)) {
        return false;
    }
    const int radio = cwRadioForContext(context);
    const int transmitVfo = qBound(0, m_radioSnapshots[radio].focusVfo, 1);
    QStringList sentValues;
    QString serialValue;
    if (const ContestDefinition *definition = activeDefinition()) {
        const int nextSerial = ContestEngine().nextSerialNumber(*definition, m_logQsos);
        serialValue = QString::number(nextSerial);
        for (const ExchangeFieldDefinition &field : definition->sentExchange) {
            QString value;
            if (const QLineEdit *edit = m_sentExchangeEdits[context].value(field.name, nullptr)) {
                value = edit->text().trimmed();
            }
            if (value.isEmpty()) {
                value = field.defaultValue;
            }
            if (field.type == ExchangeFieldType::Serial && field.generated) {
                value = QString::number(nextSerial);
                if (field.width > 0) {
                    value = value.rightJustified(field.width, QLatin1Char('0'));
                }
            }
            if (value.isEmpty()) {
                continue;
            }
            value = value.toUpper();
            if (field.type == ExchangeFieldType::Serial) {
                serialValue = value;
            }
            sentValues.append(value);
        }
    }
    const QString callsign = callsignOverride.isEmpty()
        ? m_callsignEdits[context]->text().trimmed() : callsignOverride.trimmed();
    QMap<QString, QString> macros;
    macros.insert(QStringLiteral("CALL"), callsign.toUpper());
    macros.insert(QStringLiteral("MYCALL"), m_station.callsign.toUpper());
    macros.insert(QStringLiteral("OPERATOR"), operatorIdentity());
    macros.insert(QStringLiteral("WPM"), QString::number(m_cwWpm));
    macros.insert(QStringLiteral("SERIAL"), serialValue);
    macros.insert(QStringLiteral("NR"), serialValue);
    macros.insert(QStringLiteral("SENT"), sentValues.join(QLatin1Char(' ')));
    if (const ContestDefinition *definition = activeDefinition()) {
        for (const ExchangeFieldDefinition &field : definition->sentExchange) {
            const QLineEdit *edit = m_sentExchangeEdits[context].value(field.name, nullptr);
            QString value = edit == nullptr ? field.defaultValue : edit->text().trimmed();
            if (value.isEmpty()) {
                value = field.defaultValue;
            }
            if (field.type == ExchangeFieldType::Serial && field.generated) {
                value = serialValue;
            }
            macros.insert(field.name, value.toUpper());
        }
    }
    message = expandCwMacros(message, macros);
    message = message.simplified();
    if (message.isEmpty()) {
        if (problem != nullptr) {
            *problem = tr("The message is empty after substituting macros.");
        }
        return false;
    }
    ++m_cwEnqueuedRequests;
    m_cwKeyerController.enqueueMessage(message, transmitVfo);
    return true;
}

bool MainWindow::sendEsmSequence(const QString &sequence, int context,
                                 QString callsignOverride)
{
    const QStringList messages = sequence.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &message : messages) {
        if (message.trimmed().isEmpty()) {
            continue;
        }
        QString problem;
        if (!sendCwTemplate(message, context, &problem, callsignOverride)) {
            showOperatorMessage(tr("ESM message not sent: %1").arg(problem), true);
            return false;
        }
    }
    return true;
}

QString MainWindow::esmSequenceForContest(const QString &stage,
                                          int fallbackFunctionKey) const
{
    const Contest *contest = activeContest();
    if (contest == nullptr) {
        return functionKeyText(m_repository, fallbackFunctionKey);
    }
    const QString key = QStringLiteral("cw/esm/%1/%2").arg(contest->id).arg(stage);
    return LogSettings(m_repository).value(key, functionKeyText(m_repository, fallbackFunctionKey)).toString();
}

void MainWindow::sendFunctionKey(int index)
{
    if (index != 0) {
        stopAutoCq();
    }
    sendFunctionKeyForContext(index, currentEntryContext());
}

bool MainWindow::sendFunctionKeyForContext(int index, int context)
{
    if (index < 0 || index >= 8) {
        return false;
    }
    context = qBound(0, context, 1);
    const QString mode = m_modeSelectors[context]->currentText().trimmed().toUpper();
    const QString label = functionKeyLabel(m_repository, index);
    if (mode != QStringLiteral("CW")) {
        showOperatorMessage(
            tr("F%1 %2: function-key messages are sent in CW only (entry mode is %3)")
                .arg(index + 1).arg(label, mode.isEmpty() ? tr("unset") : mode),
            true);
        return false;
    }
    QString problem;
    if (!sendCwTemplate(functionKeyText(m_repository, index), context, &problem)) {
        showOperatorMessage(tr("F%1 %2 not sent: %3").arg(index + 1).arg(label, problem),
                            true);
        return false;
    }
    statusBar()->showMessage(tr("F%1 %2 sent").arg(index + 1).arg(label), 2000);
    return true;
}

void MainWindow::handleEntryEnter(int vfo, bool callsignField)
{
    vfo = qBound(0, vfo, 1);
    if (callsignField && tuneFrequencyFromCallsign(vfo)) {
        return;
    }
    const bool esmEnabled = m_esmAction->isChecked()
        && m_modeSelectors[vfo]->currentText().compare(
               QStringLiteral("CW"), Qt::CaseInsensitive) == 0;
    if (!esmEnabled) {
        logQsoForVfo(vfo);
        return;
    }

    stopAutoCq();
    if (callsignField) {
        if (m_callsignEdits[vfo]->text().trimmed().isEmpty()) {
            return;
        }
        if (!sendEsmSequence(esmSequenceForContest(QStringLiteral("call"), 4), vfo)) {
            return;
        }
        const ContestDefinition *definition = activeDefinition();
        if (definition == nullptr) {
            return;
        }
        for (const ExchangeFieldDefinition &field : definition->receivedExchange) {
            if (QLineEdit *edit = m_exchangeEdits[vfo].value(field.name, nullptr)) {
                edit->setFocus(Qt::TabFocusReason);
                edit->selectAll();
                return;
            }
        }
        return;
    }

    if (m_callsignEdits[vfo]->text().trimmed().isEmpty()) {
        logQsoForVfo(vfo);
        return;
    }
    const QString loggedCall = m_callsignEdits[vfo]->text().trimmed();
    logQsoForVfo(vfo);
    if (m_callsignEdits[vfo]->text().isEmpty()) {
        sendEsmSequence(esmSequenceForContest(QStringLiteral("logged"), 2),
                        vfo, loggedCall);
    }
}

void MainWindow::setRunMode(int mode)
{
    mode = qBound(0, mode, 1);
    if (mode == 0) {
        for (int radio = 0; radio < 2; ++radio) {
            m_runFrequencies[radio] = connectedVfoFrequencies(m_radioSnapshots[radio]);
        }
    }
    LogSettings(m_repository).setValue(QStringLiteral("operating/runMode"), mode);
    {
        const QSignalBlocker blocker(m_runModeSelector);
        m_runModeSelector->setCurrentIndex(m_runModeSelector->findData(mode));
    }
    const QString actionName = mode == 0 ? QStringLiteral("runModeAction")
                                         : QStringLiteral("searchAndPounceAction");
    if (QAction *action = findChild<QAction *>(actionName)) {
        action->setChecked(true);
    }
    if (mode != 0) {
        stopAutoCq();
    }
}

void MainWindow::setAutoCqEnabled(bool enabled)
{
    if (!enabled) {
        m_autoCqEnabled = false;
        m_autoCqTimer->stop();
        return;
    }
    const int context = currentEntryContext();
    if (m_runModeSelector->currentData().toInt() != 0
        || m_modeSelectors[context]->currentText().compare(
               QStringLiteral("CW"), Qt::CaseInsensitive) != 0
        || !m_callsignEdits[context]->text().trimmed().isEmpty()
        || m_cwStatus.state != ConnectionState::Connected) {
        showOperatorMessage(
            tr("Auto CQ requires Run mode, CW, an empty callsign field, and a ready keyer "
               "configured for the selected radio."),
            true);
        const QSignalBlocker blocker(m_autoCqAction);
        m_autoCqAction->setChecked(false);
        m_autoCqEnabled = false;
        return;
    }
    m_autoCqEnabled = true;
    if (!sendFunctionKeyForContext(0, context)) {
        stopAutoCq();
    }
}

void MainWindow::stopAutoCq()
{
    if (!m_autoCqEnabled && !m_autoCqAction->isChecked()) {
        return;
    }
    m_autoCqEnabled = false;
    m_autoCqTimer->stop();
    const QSignalBlocker blocker(m_autoCqAction);
    m_autoCqAction->setChecked(false);
}

void MainWindow::editFunctionKey(int index)
{
    if (index < 0 || index >= 8) {
        return;
    }
    bool ok = false;
    const QString label = QInputDialog::getText(
        this, tr("F%1 message").arg(index + 1), tr("Button label:"),
        QLineEdit::Normal, functionKeyLabel(m_repository, index), &ok);
    if (!ok) {
        return;
    }
    const QString text = QInputDialog::getText(
        this, tr("F%1 message").arg(index + 1),
        tr("CW text ({CALL}, {MYCALL}, {OPERATOR}, {WPM}, {SERIAL}, {NR}, "
           "{SENT} and sent-field macros are supported):"),
        QLineEdit::Normal, functionKeyText(m_repository, index), &ok);
    if (!ok) {
        return;
    }
    LogSettings settings(m_repository);
    settings.setValue(QStringLiteral("cw/functionKeys/F%1/label").arg(index + 1),
                      label.trimmed());
    settings.setValue(QStringLiteral("cw/functionKeys/F%1/text").arg(index + 1),
                      text.trimmed());
    updateFunctionKeyButtons();
}

void MainWindow::configureFunctionKeys()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("CW function key messages"));
    dialog.setMinimumWidth(760);

    auto *layout = new QVBoxLayout(&dialog);
    auto *help = new QLabel(
        tr("Define the label and CW message for each key. Supported macros: "
           "{CALL}, {MYCALL}, {OPERATOR}, {WPM}, {SERIAL}, {NR}, {SENT}, "
           "and sent exchange fields."),
        &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("Key"), &dialog), 0, 0);
    grid->addWidget(new QLabel(tr("Button label"), &dialog), 0, 1);
    grid->addWidget(new QLabel(tr("CW message"), &dialog), 0, 2);

    QLineEdit *labels[8]{};
    QPlainTextEdit *messages[8]{};
    for (int key = 0; key < 8; ++key) {
        auto *keyLabel = new QLabel(QStringLiteral("F%1").arg(key + 1), &dialog);
        labels[key] = new QLineEdit(functionKeyLabel(m_repository, key), &dialog);
        labels[key]->setObjectName(QStringLiteral("functionKeyLabel%1").arg(key + 1));
        messages[key] = new QPlainTextEdit(functionKeyText(m_repository, key), &dialog);
        messages[key]->setObjectName(QStringLiteral("functionKeyMessage%1").arg(key + 1));
        messages[key]->setTabChangesFocus(true);
        messages[key]->setFixedHeight(48);
        grid->addWidget(keyLabel, key + 1, 0);
        grid->addWidget(labels[key], key + 1, 1);
        grid->addWidget(messages[key], key + 1, 2);
    }
    grid->setColumnStretch(2, 1);
    layout->addLayout(grid);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *resetButton = buttons->addButton(tr("Restore defaults"),
                                           QDialogButtonBox::ResetRole);
    connect(resetButton, &QPushButton::clicked, &dialog, [&] {
        for (int key = 0; key < 8; ++key) {
            labels[key]->setText(QString::fromLatin1(kFunctionKeyDefaults[key].label));
            messages[key]->setPlainText(QString::fromLatin1(kFunctionKeyDefaults[key].text));
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (int key = 0; key < 8; ++key) {
            if (labels[key]->text().trimmed().isEmpty()
                || messages[key]->toPlainText().trimmed().isEmpty()) {
                QMessageBox::warning(
                    &dialog, tr("Invalid function key message"),
                    tr("F%1 needs both a button label and a non-empty CW message.")
                        .arg(key + 1));
                return;
            }
        }

        LogSettings settings(m_repository);
        for (int key = 0; key < 8; ++key) {
            const QString group =
                QStringLiteral("cw/functionKeys/F%1/").arg(key + 1);
            settings.setValue(group + QStringLiteral("label"),
                              labels[key]->text().trimmed());
            settings.setValue(group + QStringLiteral("text"),
                              messages[key]->toPlainText().trimmed());
        }
        updateFunctionKeyButtons();
        dialog.accept();
    });
    layout->addWidget(buttons);

    dialog.resize(900, 620);
    dialog.exec();
}

void MainWindow::configureEsmMessages()
{
    const Contest *contest = activeContest();
    if (contest == nullptr) {
        QMessageBox::information(this, tr("ESM messages"),
                                 tr("Select an active contest before configuring ESM."));
        return;
    }

    const QString settingsPrefix = QStringLiteral("cw/esm/%1/").arg(contest->id);
    LogSettings settings(m_repository);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("ESM messages — %1").arg(contest->name));
    dialog.setMinimumWidth(560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *help = new QLabel(
        tr("Enter one CW message per line. Blank lines disable a step. Messages support "
           "{CALL}, {MYCALL}, {OPERATOR}, {WPM}, {SERIAL}, {NR}, {SENT} and sent "
           "exchange-field macros. Defaults use the current F5 and F3 messages."),
        &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *form = new QFormLayout;
    auto *callMessages = new QPlainTextEdit(&dialog);
    callMessages->setObjectName(QStringLiteral("esmCallMessages"));
    callMessages->setPlainText(settings.value(
        settingsPrefix + QStringLiteral("call"), functionKeyText(m_repository, 4)).toString());
    auto *loggedMessages = new QPlainTextEdit(&dialog);
    loggedMessages->setObjectName(QStringLiteral("esmLoggedMessages"));
    loggedMessages->setPlainText(settings.value(
        settingsPrefix + QStringLiteral("logged"), functionKeyText(m_repository, 2)).toString());
    form->addRow(tr("After Call Enter:"), callMessages);
    form->addRow(tr("After successful QSO:"), loggedMessages);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        settings.setValue(settingsPrefix + QStringLiteral("call"),
                          callMessages->toPlainText().trimmed());
        settings.setValue(settingsPrefix + QStringLiteral("logged"),
                          loggedMessages->toPlainText().trimmed());
        dialog.accept();
    });
    layout->addWidget(buttons);
    dialog.resize(680, 460);
    dialog.exec();
}

void MainWindow::updateFunctionKeyButtons()
{
    for (int key = 0; key < 8; ++key) {
        const QString label = functionKeyLabel(m_repository, key);
        const QString text = functionKeyText(m_repository, key);
        m_functionKeyButtons[key]->setText(QStringLiteral("F%1  %2").arg(key + 1).arg(label));
        m_functionKeyButtons[key]->setToolTip(
            tr("%1\nRight-click to edit.").arg(text));
        m_functionKeyActions[key]->setText(
            tr("Send F%1 — %2").arg(key + 1).arg(label));
    }
}

void MainWindow::abortCwTransmission()
{
    stopAutoCq();
    m_cwTypedText.clear();
    m_cwMessageEdit->clear();
    m_cwKeyerController.abortTransmission();
}

void MainWindow::setCwWpm(int wpm)
{
    m_cwWpm = qBound(5, wpm, 80);
    QSettings settings;
    settings.setValue(QStringLiteral("cw/wpm"), m_cwWpm);
    m_cwKeyerController.setWpm(m_cwWpm);
    updateTopStatus();
    refreshRate();
}

void MainWindow::updateCwStatus(const CwKeyerStatus &status)
{
    m_cwStatus = status;
    QString state;
    switch (status.state) {
    case ConnectionState::Disconnected: state = tr("Disconnected"); break;
    case ConnectionState::Connecting: state = tr("Connecting"); break;
    case ConnectionState::Connected: state = tr("Ready"); break;
    case ConnectionState::Error: state = tr("Error"); break;
    case ConnectionState::Unsupported: state = tr("Unsupported"); break;
    }
    m_cwStatusLabel->setText(
        tr("CW: %1 — %2 — %3 WPM — queue %4%5%6")
            .arg(state,
                 status.message.isEmpty() ? tr("Idle") : status.message)
            .arg(status.wpm)
            .arg(status.queuedMessages)
            .arg(status.state == ConnectionState::Connected && !status.pacing.isEmpty()
                     ? QStringLiteral(" — %1").arg(status.completionTimedOut
                                                       ? tr("NO COMPLETION — queue stopped")
                                                       : status.pacing)
                     : QString())
            .arg(status.transmitting
                     ? tr(" — TX VFO %1").arg(status.transmitVfo == 0 ? 'A' : 'B')
                     : QString()));
    m_cwStatusLabel->setToolTip(
        tr("Completion is confirmed when the keyer reports end of transmission "
           "(cwdaemon reply); otherwise it is estimated from the message length and speed."));
    m_cwStatusLabel->setStyleSheet(
        !status.error.isEmpty() || status.state == ConnectionState::Error
                || status.completionTimedOut
            ? QStringLiteral("color: #c62828;")
            : QString());
    if (!status.error.isEmpty() || status.state == ConnectionState::Error
        || status.completionTimedOut) {
        showOperatorMessage(status.error.isEmpty() ? status.message : status.error, true);
    }
    if (!m_autoCqEnabled) {
        return;
    }
    if (status.state != ConnectionState::Connected) {
        stopAutoCq();
    } else if (status.transmitting || status.queuedMessages > 0) {
        m_autoCqTimer->stop();
    } else if (!m_autoCqTimer->isActive()) {
        m_autoCqTimer->start();
    }
}

void MainWindow::setSo2vEnabled(bool enabled)
{
    const int radio = m_activeRadioSelector->currentData().toInt();
    const RadioSnapshot &snapshot = m_radioSnapshots[radio];
    if (enabled && (m_so2rToggle->isChecked()
                    || snapshot.state != ConnectionState::Connected
                    || !snapshot.dualVfoSupported)) {
        QMessageBox::warning(
            this, tr("SO2V unavailable"),
            tr("SO2V requires one connected radio with two accessible VFOs and cannot "
               "be combined with SO2R."));
        const QSignalBlocker blocker(m_so2vToggle);
        m_so2vToggle->setChecked(false);
        return;
    }
    m_entryGroups[1]->setVisible(enabled);
    m_radioABandmapDock->setVisible(enabled || m_so2rToggle->isChecked());
    if (enabled && !snapshot.dualReceiveSupported) {
        showOperatorMessage(tr("SO2V without dual receive: the radio exposes two VFOs but "
                               "only one can be heard at a time."), true);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("radio/so2vEnabled"), enabled);
    if (!enabled && snapshot.focusVfo != 0) {
        focusVfo(0);
    }
    if (!enabled) {
        setActiveEntryContext(0);
    }
    updateEntryHeaders();
    refreshBandmap();
    statusBar()->showMessage(
        enabled ? tr("SO2V enabled. VFO A and B have independent QSO entry.")
                : tr("SO2V disabled."),
        4000);
}

void MainWindow::setSo2rEnabled(bool enabled)
{
    if (enabled
        && (m_radioSnapshots[0].state != ConnectionState::Connected
            || m_radioSnapshots[1].state != ConnectionState::Connected)) {
        QMessageBox::warning(
            this, tr("SO2R unavailable"),
            tr("Connect both Radio A and Radio B before enabling SO2R."));
        const QSignalBlocker blocker(m_so2rToggle);
        m_so2rToggle->setChecked(false);
        return;
    }
    if (enabled && m_so2vToggle->isChecked()) {
        const QSignalBlocker blocker(m_so2vToggle);
        m_so2vToggle->setChecked(false);
    }
    for (int i = 0; i < 2; ++i) {
        m_focusButtons[i]->setText(
            enabled ? (i == 0 ? tr("Focus Radio A") : tr("Focus Radio B"))
                    : (i == 0 ? tr("RX focus A") : tr("RX focus B")));
    }
    m_entryGroups[1]->setVisible(enabled || m_so2vToggle->isChecked());
    m_radioABandmapDock->setVisible(enabled || m_so2vToggle->isChecked());
    if (!m_entryGroups[1]->isVisible()) {
        setActiveEntryContext(0);
    }
    if (enabled) {
        m_activeRadioSelector->setCurrentIndex(
            m_activeRadioSelector->findData(0));
    }
    statusBar()->showMessage(
        enabled ? tr("SO2R enabled. QSO contexts are routed to separate radios.")
                : tr("SO2R disabled."),
        4000);
    updateRadioSnapshotForRadio(
        m_activeRadioSelector->currentData().toInt(),
        m_radioSnapshots[m_activeRadioSelector->currentData().toInt()]);
    refreshBandmap();
}

void MainWindow::focusVfo(int vfo)
{
    if (vfo < 0 || vfo > 1) {
        return;
    }
    if (m_so2rToggle->isChecked()) {
        m_activeRadioSelector->setCurrentIndex(
            m_activeRadioSelector->findData(vfo));
        setActiveEntryContext(vfo);
        m_callsignEdits[vfo]->setFocus();
        return;
    }
    const int radio = m_activeRadioSelector->currentData().toInt();
    const RadioSnapshot &snapshot = m_radioSnapshots[radio];
    if (snapshot.state != ConnectionState::Connected) {
        statusBar()->showMessage(
            tr("Radio %1 is not connected.")
                .arg(radio == 0 ? tr("A") : tr("B")),
            5000);
        return;
    }
    if (vfo == 1 && !snapshot.dualVfoSupported) {
        statusBar()->showMessage(tr("VFO B is not available on this radio."), 5000);
        return;
    }
    if (vfo == 1 && !m_so2vToggle->isChecked()) {
        m_so2vToggle->setChecked(true);
    }
    setActiveEntryContext(vfo);
    m_callsignEdits[vfo]->setFocus();
}

void MainWindow::selectContest(int index)
{
    if (index < 0 || index >= m_contests.size()) {
        for (int vfo = 0; vfo < 2; ++vfo) {
            m_serialReservationUuids[vfo].clear();
            m_serialReservationBands[vfo].clear();
            m_serialReservationModes[vfo].clear();
            m_serialReservationSerials[vfo].clear();
        }
        applyActiveLogStation();
        m_activeContestLabel->setText(tr("No log is available."));
        if (m_openLogLabel != nullptr) {
            m_openLogLabel->setText(tr("no log"));
        }
        rebuildQsoForm();
        refreshRecentQsos();
        refreshBandmap();
        return;
    }

    const Contest &contest = m_contests.at(index);
    if (m_networkLogId != 0 && contest.id != m_networkLogId) {
        disconnectNetworkConnection();
        if (m_hostServer) {
            configureHostMode();
        }
        m_networkLogId = 0;
        statusBar()->showMessage(tr("Network stopped because the open log changed."), 10000);
    }
    QSettings settings;
    if (settings.value(QStringLiteral("session/activeContestId")).toLongLong()
        != contest.id) {
        for (int vfo = 0; vfo < 2; ++vfo) {
            m_serialReservationUuids[vfo].clear();
            m_serialReservationBands[vfo].clear();
            m_serialReservationModes[vfo].clear();
            m_serialReservationSerials[vfo].clear();
        }
    }
    settings.setValue(QStringLiteral("session/activeContestId"), contest.id);
    if (m_openLogLabel != nullptr) {
        m_openLogLabel->setText(contest.name);
        m_openLogLabel->setToolTip(contest.description);
    }
    applyActiveLogStation();
    const auto definition = m_definitions.constFind(contest.id);
    if (definition == m_definitions.cend()) {
        m_activeContestLabel->setText(
            QStringLiteral("<h2>%1</h2><p>%2</p><p>%3</p>")
                .arg(contest.name.toHtmlEscaped(),
                     contest.description.toHtmlEscaped(),
                     tr("This log has no YAML contest definition.").toHtmlEscaped()));
    } else {
        QStringList sentFields;
        for (const ExchangeFieldDefinition &field : definition->sentExchange) {
            QString detail = fieldTypeName(field.type);
            if (field.generated) {
                detail += tr(", generated");
            } else if (!field.defaultValue.isEmpty()) {
                detail += tr(", default: %1").arg(field.defaultValue);
            }
            if (field.hasWhen) {
                detail += tr(", station-dependent");
            }
            if (field.width > 0) {
                detail += tr(", width: %1").arg(field.width);
            }
            sentFields.append(QStringLiteral("%1 (%2%3)")
                                  .arg(field.label.toHtmlEscaped(),
                                       detail.toHtmlEscaped(),
                                       field.required
                                           ? QStringLiteral(", required")
                                           : QString()));
        }
        QStringList receivedFields;
        for (const ExchangeFieldDefinition &field : definition->receivedExchange) {
            receivedFields.append(QStringLiteral("%1 (%2)")
                                      .arg(field.label.toHtmlEscaped(),
                                           (fieldTypeName(field.type)
                                            + (field.required
                                                   ? tr(", required")
                                                   : tr(", optional")))
                                               .toHtmlEscaped()));
        }
        QStringList multiplierDescriptions;
        for (const MultiplierDefinition &multiplier : definition->multipliers) {
            QString type;
            if (multiplier.type == QStringLiteral("exchange")) {
                type = tr("Exchange: %1").arg(multiplier.field).toHtmlEscaped();
            } else if (multiplier.type == QStringLiteral("prefix")) {
                type = tr("Callsign prefix").toHtmlEscaped();
            } else if (multiplier.type == QStringLiteral("dxcc")) {
                type = tr("DXCC entity").toHtmlEscaped();
            } else {
                type = tr("Country").toHtmlEscaped();
            }
            QStringList scopes;
            if (multiplier.perBand) {
                scopes.append(tr("per band"));
            }
            if (multiplier.perMode) {
                scopes.append(tr("per mode"));
            }
            const QString scope = scopes.isEmpty()
                ? QString() : QStringLiteral(" (%1)").arg(scopes.join(QStringLiteral(", ")));
            multiplierDescriptions.append(QStringLiteral("%1%2").arg(type, scope));
        }
        const QString preview = tr(
            "<h2>%1</h2>"
            "<p>%2</p>"
            "<p><b>Allowed bands:</b> %3<br>"
            "<b>Modes:</b> %4<br>"
            "<b>QSO points:</b> %5<br>"
            "<b>Serial starts at:</b> %6</p>"
            "<p><b>Sent exchange:</b> %7<br>"
            "<b>Received exchange:</b> %8</p>"
            "<p><b>Multipliers:</b> %9</p>"
            "<p><b>Duplicate key:</b> %10</p>")
            .arg(definition->name.toHtmlEscaped(),
                 definition->description.toHtmlEscaped(),
                 definition->bands.join(QStringLiteral(", ")).toHtmlEscaped(),
                 definition->modes.join(QStringLiteral(", ")).toHtmlEscaped())
            .arg(definition->qsoPoints)
            .arg(definition->serialStart)
            .arg(sentFields.join(QStringLiteral(", ")),
                 receivedFields.join(QStringLiteral(", ")),
                 multiplierDescriptions.isEmpty()
                     ? tr("None").toHtmlEscaped()
                     : multiplierDescriptions.join(QStringLiteral(", ")),
                 definition->dupeKey.join(QStringLiteral(", ")).toHtmlEscaped());
        m_activeContestLabel->setText(preview);
    }
    const auto configureSelector = [](QComboBox *selector,
                                      const QStringList &allowedValues,
                                      const QStringList &fallbackValues,
                                      bool constrained) {
        const QString current = selector->currentText();
        const QStringList &values = constrained ? allowedValues : fallbackValues;
        selector->setEditable(!constrained);
        selector->clear();
        selector->addItems(values);
        const auto match = std::find_if(
            values.cbegin(), values.cend(), [&current](const QString &value) {
                return value.compare(current, Qt::CaseInsensitive) == 0;
            });
        if (match != values.cend()) {
            selector->setCurrentIndex(static_cast<int>(match - values.cbegin()));
        } else if (!constrained && !current.isEmpty()) {
            selector->setCurrentText(current);
        } else if (!values.isEmpty()) {
            selector->setCurrentIndex(0);
        }
    };
    const bool hasDefinition = definition != m_definitions.cend();
    for (int vfo = 0; vfo < 2; ++vfo) {
        configureSelector(m_bandSelectors[vfo],
                          hasDefinition ? definition->bands : QStringList{},
                          defaultBands(), hasDefinition);
        configureSelector(m_modeSelectors[vfo],
                          hasDefinition ? definition->modes : QStringList{},
                          defaultModes(), hasDefinition);
    }
    rebuildQsoForm();
    updateSentReportForMode(0);
    updateSentReportForMode(1);
    refreshRecentQsos();
    updateDupeIndicator();
    refreshBandmap();
    statusBar()->showMessage(tr("Active log: %1").arg(contest.name), 3000);
}

bool MainWindow::newLogOptions(QVector<NewLogContestOption> *result, QString *error) const
{
    QVector<NewLogContestOption> options;
    QVector<ContestDefinitionFile> files;
    if (!loadContestDefinitionFiles(&files, error)) {
        return false;
    }
    QHash<QString, int> optionIndexById;
    QSet<QString> fileIds;
    for (const ContestDefinitionFile &file : files) {
        const QString id = file.definition.id.toUpper();
        fileIds.insert(id);
        optionIndexById.insert(id, static_cast<int>(options.size()));
        options.append({file.definition, file.yaml, file.path});
    }
    if (m_repository.isStandaloneLog()) {
        const auto assets = m_repository.logAssets(error);
        if (error != nullptr && !error->isEmpty()) {
            return false;
        }
        for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
            if (!it.key().startsWith(QLatin1String("definitions/"))) {
                continue;
            }
            ContestDefinition definition;
            const QString yaml = QString::fromUtf8(it.value());
            if (!ContestDefinitionLoader().loadText(yaml, it.key(), definition, error)) {
                return false;
            }
            const QString id = definition.id.toUpper();
            if (!optionIndexById.contains(id)) {
                optionIndexById.insert(id, static_cast<int>(options.size()));
                options.append({definition, yaml, tr("installed")});
            }
        }
    }
    for (const Contest &contest : m_contests) {
        const auto definition = m_definitions.constFind(contest.id);
        if (definition == m_definitions.cend()) {
            continue;
        }
        const QString id = definition->id.toUpper();
        if (fileIds.contains(id)) {
            continue;
        }
        const bool templateRow = contest.code.compare(id, Qt::CaseInsensitive) == 0;
        const auto existing = optionIndexById.constFind(id);
        if (existing != optionIndexById.cend() && !templateRow) {
            continue;
        }
        const QString yaml = m_repository.contestDefinitionYaml(contest.id, error);
        if (error != nullptr && !error->isEmpty()) {
            return false;
        }
        NewLogContestOption option{definition.value(), yaml, tr("installed")};
        if (existing != optionIndexById.cend()) {
            options[existing.value()] = std::move(option);
        } else {
            optionIndexById.insert(id, static_cast<int>(options.size()));
            options.append(std::move(option));
        }
    }
    *result = std::move(options);
    return true;
}

void MainWindow::createNewLog()
{
    QVector<NewLogContestOption> options;
    QString error;
    if (!newLogOptions(&options, &error)) {
        QMessageBox::critical(this, tr("Unable to load contest definition"), error);
        return;
    }
    NewLogDialog dialog(options, m_defaultStation, {}, this);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedOption() == nullptr) {
        return;
    }
    createLogFile(dialog.log(), *dialog.selectedOption(), dialog.sentExchange());
}

void MainWindow::openLog()
{
    QFileDialog dialog(this, tr("Open log"));
    dialog.setObjectName(QStringLiteral("openLogFileDialog"));
    dialog.setNameFilter(tr("Log databases (*.sqlite);;All files (*)"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(QSettings().value(QStringLiteral("session/logDirectory"),
                                          QDir::homePath()).toString());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString path = dialog.selectedFiles().first();
    if (m_repository.isStandaloneLog()
        && QFileInfo(path).canonicalFilePath()
            == QFileInfo(m_repository.databasePath()).canonicalFilePath()) {
        return;
    }
    SqliteRepository candidate{QSqlDatabase()};
    QString error;
    if (!candidate.openLogFile(path, &error)) {
        QMessageBox::critical(this, tr("Unable to open log"), error);
        return;
    }
    if (!backupLogIfDue(candidate, &error)) {
        QMessageBox::critical(this, tr("Automatic database backup failed"), error);
        return;
    }
    activateLogFile(candidate);
}

QString MainWindow::chooseNewLogPath(const QString &name)
{
    QString safeName = name;
    safeName.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")),
                     QStringLiteral("_"));
    QFileDialog dialog(this, tr("Save new log"));
    dialog.setObjectName(QStringLiteral("newLogFileDialog"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(tr("Log databases (*.sqlite)"));
    dialog.setDefaultSuffix(QStringLiteral("sqlite"));
    dialog.setOption(QFileDialog::DontConfirmOverwrite);
    dialog.setDirectory(QSettings().value(QStringLiteral("session/logDirectory"),
                                          QDir::homePath()).toString());
    dialog.selectFile(safeName + QStringLiteral(".sqlite"));
    return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().first() : QString();
}

bool MainWindow::createLogFile(Contest log, const NewLogContestOption &option,
                               const QMap<QString, QString> &sentValues,
                               const QVector<Qso> &qsos)
{
    const QString path = chooseNewLogPath(log.name);
    if (path.isEmpty()) {
        return false;
    }
    QString error;
    QMap<QString, QByteArray> assets;
    if (!supportDataAssets(&assets, &error)) {
        QMessageBox::critical(this, tr("Unable to create log"), error);
        return false;
    }
    QVector<NewLogContestOption> definitions;
    if (!newLogOptions(&definitions, &error)) {
        QMessageBox::critical(this, tr("Unable to create log"), error);
        return false;
    }
    for (const auto &definition : definitions) {
        assets.insert(QStringLiteral("definitions/%1").arg(definition.definition.id),
                      definition.yaml.toUtf8());
    }
    assets.insert(QStringLiteral("definitions/%1").arg(option.definition.id), option.yaml.toUtf8());
    QVariantMap settings;
    if (m_repository.isStandaloneLog()) {
        settings = m_repository.logSettings(&error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to create log"), error);
            return false;
        }
    } else {
        QSettings defaults;
        for (const QString &key : defaults.allKeys()) {
            if (isLogSetting(key) && !key.startsWith(QLatin1String("contest/"))
                && !key.startsWith(QLatin1String("cw/esm/"))) {
                settings.insert(key, defaults.value(key));
            }
        }
    }
    // New files start with contest id 1; never inherit another log's sent exchange.
    for (auto it = settings.begin(); it != settings.end();) {
        if (it.key().startsWith(QLatin1String("contest/"))
            || it.key().startsWith(QLatin1String("cw/esm/"))) {
            it = settings.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = sentValues.cbegin(); it != sentValues.cend(); ++it) {
        for (int vfo = 0; vfo < 2; ++vfo) {
            settings.insert(QStringLiteral("contest/sentExchange/1/%1/vfo%2")
                                .arg(it.key()).arg(vfo), it.value());
        }
    }
    SqliteRepository candidate{QSqlDatabase()};
    if (!candidate.createLogFile(path, option.definition, option.yaml, log,
                                  settings, assets, qsos, &error)) {
        QMessageBox::critical(this, tr("Unable to create log"), error);
        return false;
    }
    activateLogFile(candidate);
    return true;
}

void MainWindow::activateLogFile(SqliteRepository &candidate)
{
    abortCwTransmission();
    disconnectNetworkConnection();
    m_syncClient.reset();
    m_networkLogId = 0;
    m_networkReadOnly = false;
    m_joiningRemoteLog = false;
    m_creatingRemoteLog = false;
    m_mergingRemoteLog = false;
    m_pendingRemoteContest = {};
    m_pendingRemoteDefinitionYaml.clear();
    m_pendingSerialHistoryRequestId.clear();
    m_pendingAdminSerialRequestId.clear();
    m_pendingSerialAdjustmentRequestId.clear();
    for (int vfo = 0; vfo < 2; ++vfo) {
        wipeEntry(vfo);
        m_serialReservationUuids[vfo].clear();
        m_serialReservationBands[vfo].clear();
        m_serialReservationModes[vfo].clear();
        m_serialReservationSerials[vfo].clear();
    }
    m_repository.swap(candidate);
    m_logQsos.clear();
    loadOperator();
    loadSupportData();
    loadContests();
    updateFunctionKeyButtons();
    refreshNetworkStations();
    LogSettings settings(m_repository);
    {
        const QSignalBlocker blocker(m_esmAction);
        m_esmAction->setChecked(settings.value(QStringLiteral("operating/esmEnabled"), false).toBool());
    }
    if (auto *action = findChild<QAction *>(QStringLiteral("allowDupeLoggingAction"))) {
        const QSignalBlocker blocker(action);
        action->setChecked(settings.value(QStringLiteral("operating/allowDupeLogging"), false).toBool());
    }
    setRunMode(settings.value(QStringLiteral("operating/runMode"), 0).toInt());
    setWindowFilePath(m_repository.databasePath());
    m_openLogLabel->setToolTip(m_repository.databasePath());
    QSettings global;
    global.setValue(QStringLiteral("session/logFile"), m_repository.databasePath());
    global.setValue(QStringLiteral("session/logDirectory"), QFileInfo(m_repository.databasePath()).absolutePath());
    statusBar()->showMessage(tr("Opened log: %1").arg(m_repository.databasePath()), 5000);
}

void MainWindow::editLogProperties()
{
    const Contest *contest = activeContest();
    if (contest == nullptr) {
        QMessageBox::warning(this, tr("No log selected"),
                             tr("Select a log before editing its properties."));
        return;
    }
    NewLogContestOption option;
    QMap<QString, QString> sentValues;
    const auto definition = m_definitions.constFind(contest->id);
    LogSettings settings(m_repository);
    if (definition != m_definitions.cend()) {
        option.definition = definition.value();
        option.source = contest->code;
        for (const ExchangeFieldDefinition &field : definition->sentExchange) {
            sentValues.insert(
                field.name,
                settings.value(QStringLiteral("contest/sentExchange/%1/%2/vfo0")
                                   .arg(contest->id)
                                   .arg(field.name),
                               field.defaultValue).toString());
        }
    } else {
        option.definition.id = contest->code;
        option.definition.name = contest->name;
        option.definition.description = contest->description;
        option.source = contest->code;
    }
    Contest current = *contest;
    if (current.stationCallsign.isEmpty()) {
        current.stationCallsign = m_station.callsign;
    }
    NewLogDialog dialog(option, current, sentValues, logNames(contest->id), this);
    if (m_repository.isStandaloneLog()) {
        dialog.findChild<QLineEdit *>(QStringLiteral("newLogName"))->setReadOnly(true);
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    Contest updated = dialog.log();
    updated.id = contest->id;
    QString error;
    if (!m_repository.updateContestLog(updated, &error)) {
        QMessageBox::critical(this, tr("Unable to save log properties"), error);
        return;
    }
    saveSentExchangeValues(updated.id, dialog.sentExchange());
    loadContests();
    statusBar()->showMessage(tr("Log properties saved: %1").arg(updated.name), 5000);
}

void MainWindow::saveSentExchangeValues(qint64 contestId,
                                        const QMap<QString, QString> &values)
{
    LogSettings settings(m_repository);
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        for (int vfo = 0; vfo < 2; ++vfo) {
            settings.setValue(QStringLiteral("contest/sentExchange/%1/%2/vfo%3")
                                  .arg(contestId)
                                  .arg(it.key())
                                  .arg(vfo),
                              it.value());
        }
    }
}

QStringList MainWindow::logNames(qint64 excludedId) const
{
    QStringList names;
    for (const Contest &contest : m_contests) {
        if (contest.id != excludedId) {
            names.append(contest.name);
        }
    }
    return names;
}

const Contest *MainWindow::activeContest() const
{
    if (m_contestSelector == nullptr || m_contestSelector->currentIndex() < 0) {
        return nullptr;
    }
    const qint64 contestId = m_contestSelector->currentData().toLongLong();
    const auto contest = std::find_if(
        m_contests.cbegin(), m_contests.cend(),
        [contestId](const Contest &candidate) { return candidate.id == contestId; });
    return contest == m_contests.cend() ? nullptr : &*contest;
}

void MainWindow::applyActiveLogStation()
{
    m_station = m_defaultStation;
    const Contest *contest = activeContest();
    if (contest != nullptr && !contest->stationCallsign.isEmpty()) {
        m_station.callsign = contest->stationCallsign;
    }
    updateStationLabel();
    if (m_syncClient) {
        m_syncClient->setStationIdentity(m_station.callsign, operatorIdentity());
    }
    updateSentExchangeVisibility(0);
    updateSentExchangeVisibility(1);
}

void MainWindow::updateSentExchangeVisibility(int vfo)
{
    vfo = qBound(0, vfo, 1);
    const ContestDefinition *definition = activeDefinition();
    if (definition == nullptr || m_sentExchangeEdits[vfo].isEmpty()) {
        return;
    }
    Qso stationQso;
    stationQso.callsign = m_station.callsign;
    stationQso.stationCallsign = m_station.callsign;
    const auto resolver = multiplierResolver();
    const QMap<QString, QString> facts = resolver ? resolver(stationQso)
                                                   : QMap<QString, QString>{};
    if (!facts.contains(QStringLiteral("station.dxcc"))) {
        return;
    }
    for (const ExchangeFieldDefinition &field : definition->sentExchange) {
        if (!field.hasWhen) {
            continue;
        }
        QLineEdit *edit = m_sentExchangeEdits[vfo].value(field.name, nullptr);
        if (edit != nullptr && edit->parentWidget() != nullptr) {
            edit->parentWidget()->setVisible(
                ContestEngine::sentFieldApplies(field, facts));
        }
    }
}

void MainWindow::importContestDefinition()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Import contest definition"), contestDefinitionsDirectory(),
        tr("Contest definitions (*.yaml *.yml);;All files (*)"));
    if (filePath.isEmpty()) {
        return;
    }

    QFile source(filePath);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Unable to read definition"),
                              QStringLiteral("%1: %2")
                                  .arg(filePath, source.errorString()));
        return;
    }
    const QString yaml = QString::fromUtf8(source.readAll());
    ContestDefinition definition;
    QString error;
    ContestDefinitionLoader loader;
    if (!loader.loadText(yaml, filePath, definition, &error)) {
        QMessageBox::critical(this, tr("Invalid contest definition"), error);
        return;
    }

    if (!storeContestDefinitionFile(yaml, &error)) {
        QMessageBox::critical(this, tr("Unable to import definition"), error);
        return;
    }
    if (!storeSupportAsset(QStringLiteral("definitions/%1").arg(definition.id), yaml.toUtf8())) {
        return;
    }
    statusBar()->showMessage(tr("Imported contest: %1").arg(definition.name), 5000);
}

void MainWindow::restoreContestDefinitionVersion()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Restore previous contest definition"),
        contestDefinitionVersionsDirectory(),
        tr("Versioned definitions (*.yaml);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Unable to read definition version"),
                              QStringLiteral("%1: %2").arg(path, source.errorString()));
        return;
    }
    const QString yaml = QString::fromUtf8(source.readAll());
    ContestDefinition definition;
    QString error;
    if (!ContestDefinitionLoader().loadText(yaml, path, definition, &error)) {
        QMessageBox::critical(this, tr("Invalid contest definition version"), error);
        return;
    }
    if (QMessageBox::question(
            this, tr("Restore contest definition?"),
            tr("Restore the saved version of '%1'? The current definition will "
               "be saved as another version first.")
                .arg(definition.name))
        != QMessageBox::Yes) {
        return;
    }
    if (!storeContestDefinitionFile(yaml, &error)) {
        QMessageBox::critical(this, tr("Unable to restore definition version"), error);
        return;
    }
    if (!storeSupportAsset(QStringLiteral("definitions/%1").arg(definition.id), yaml.toUtf8())) {
        return;
    }
    statusBar()->showMessage(tr("Restored contest definition: %1")
                                 .arg(definition.name), 5000);
}

void MainWindow::importCallHistory()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import call history"), {},
        tr("Call history (*.csv *.txt *.dat);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Unable to read call history"),
                              QStringLiteral("%1: %2").arg(path, file.errorString()));
        return;
    }
    const QByteArray contents = file.readAll();
    QHash<QString, CallHistoryEntry> imported;
    QString error;
    if (!SupportDataParser::parseCallHistory(
            QString::fromUtf8(contents), imported, &error)) {
        QMessageBox::critical(this, tr("Invalid call history"), error);
        return;
    }
    if (!storeSupportAsset(supportAssetKey(QStringLiteral("support/history"), path), contents)) {
        return;
    }
    for (auto it = imported.cbegin(); it != imported.cend(); ++it) {
        m_callHistory.insert(it.key(), it.value());
    }
    QSettings().setValue(QStringLiteral("supportData/callHistory"), path);
    QStringList paths = QSettings().value(
        QStringLiteral("supportData/callHistoryFiles")).toStringList();
    if (!paths.contains(path)) {
        paths.append(path);
    }
    QSettings().setValue(QStringLiteral("supportData/callHistoryFiles"), paths);
    rebuildCheckPartialIndex();
    QMessageBox::information(
        this, tr("Call history imported"),
        tr("Imported %1 callsigns. Matching records can suggest exchange fields.")
            .arg(imported.size()));
}

void MainWindow::importMasterCpy()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import callsign list"), {},
        tr("Callsign lists (*.scp *.cpy *.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Unable to read master.cpy"),
                              QStringLiteral("%1: %2").arg(path, file.errorString()));
        return;
    }
    const QByteArray contents = file.readAll();
    QStringList imported;
    QString error;
    if (!SupportDataParser::parseMasterCpy(
            QString::fromUtf8(contents), imported, &error)) {
        QMessageBox::critical(this, tr("Invalid master.cpy"), error);
        return;
    }
    if (!storeSupportAsset(supportAssetKey(QStringLiteral("support/master"), path), contents)) {
        return;
    }
    QSet<QString> known(m_masterCallsigns.cbegin(), m_masterCallsigns.cend());
    for (const QString &callsign : imported) {
        if (!known.contains(callsign)) {
            known.insert(callsign);
            m_masterCallsigns.append(callsign);
        }
    }
    QSettings settings;
    QStringList paths = settings.value(
        QStringLiteral("supportData/masterCpyFiles")).toStringList();
    if (paths.isEmpty()) {
        const QString legacyPath = settings.value(
            QStringLiteral("supportData/masterCpy")).toString();
        if (!legacyPath.isEmpty()) {
            paths.append(legacyPath);
        }
    }
    settings.setValue(QStringLiteral("supportData/masterCpy"), path);
    if (!paths.contains(path)) {
        paths.append(path);
    }
    settings.setValue(QStringLiteral("supportData/masterCpyFiles"), paths);
    rebuildCheckPartialIndex();
    QMessageBox::information(this, tr("master.cpy imported"),
                             tr("Loaded %1 callsign suggestions.")
                                 .arg(imported.size()));
}

void MainWindow::importCtyDat()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import wl_cty.dat"), {},
        tr("Country files (wl_cty.dat cty.dat *.dat);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Unable to read wl_cty.dat"),
                              QStringLiteral("%1: %2").arg(path, file.errorString()));
        return;
    }
    const QByteArray contents = file.readAll();
    QVector<CtyPrefixEntry> imported;
    QString error;
    if (!SupportDataParser::parseCtyDat(
            QString::fromUtf8(contents), imported, &error)) {
        QMessageBox::critical(this, tr("Invalid wl_cty.dat"), error);
        return;
    }
    if (!storeSupportAsset(QStringLiteral("support/cty"), contents)) {
        return;
    }
    m_ctyPrefixes = std::move(imported);
    if (m_hostRepository) {
        m_hostRepository->setCtyPrefixes(m_ctyPrefixes);
    }
    m_ctyCache.clear();
    refreshRecentQsos();
    QSettings().setValue(QStringLiteral("supportData/ctyDat"), path);
    QMessageBox::information(
        this, tr("wl_cty.dat imported"),
        tr("Loaded %1 callsign prefixes for country and zone lookup.")
            .arg(m_ctyPrefixes.size()));
    for (int vfo = 0; vfo < 2; ++vfo) {
        updateCallLookup(vfo);
    }
}

void MainWindow::restoreSupportDataVersion()
{
    const QString historyDirectory = QDir(applicationDataDirectory())
        .filePath(QStringLiteral("support-data-versions"));
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Restore downloaded support data"), historyDirectory,
        tr("Saved versions (*.bak);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const QString fileName = QFileInfo(path).absoluteDir().dirName();
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, tr("Unable to read support-data version"),
                              QStringLiteral("%1: %2").arg(path, source.errorString()));
        return;
    }
    const QByteArray contents = source.readAll();
    if (fileName != QStringLiteral("master.scp")
        && fileName != QStringLiteral("wl_cty.dat")) {
        QMessageBox::warning(
            this, tr("Unknown support-data version"),
            tr("Only downloaded Master.SCP and wl_cty.dat versions can be restored."));
        return;
    }
    QStringList callsigns;
    QVector<CtyPrefixEntry> prefixes;
    QString error;
    const bool valid = fileName == QStringLiteral("master.scp")
        ? SupportDataParser::parseMasterCpy(QString::fromUtf8(contents), callsigns, &error)
        : SupportDataParser::parseCtyDat(QString::fromUtf8(contents), prefixes, &error);
    if (!valid) {
        QMessageBox::critical(this, tr("Invalid support-data version"), error);
        return;
    }
    if (QMessageBox::question(
            this, tr("Restore support data?"),
            tr("Restore the selected version of %1? The current downloaded version "
               "will be saved before it is replaced.")
                .arg(fileName))
        != QMessageBox::Yes) {
        return;
    }
    QString restoredPath;
    if (!saveSupportDataFile(fileName, contents, restoredPath, error)) {
        QMessageBox::critical(this, tr("Unable to restore support data"), error);
        return;
    }
    QSettings settings;
    if (fileName == QStringLiteral("master.scp")) {
        settings.setValue(QStringLiteral("supportData/masterScpPath"), restoredPath);
        settings.remove(QStringLiteral("supportData/masterScpEtag"));
    } else {
        settings.setValue(QStringLiteral("supportData/ctyDat"), restoredPath);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(
            this, tr("Unable to save support-data settings"),
            tr("The restored file was saved, but its active path could not be stored."));
        return;
    }
    if (!storeSupportAsset(supportAssetKey(
            fileName == QLatin1String("master.scp") ? QStringLiteral("support/master")
                                                   : QStringLiteral("support/cty"),
            restoredPath), contents)) {
        return;
    }
    loadSupportData();
    refreshRecentQsos();
    statusBar()->showMessage(tr("Restored support data: %1").arg(fileName), 5000);
}

bool MainWindow::supportDataAssets(QMap<QString, QByteArray> *assets, QString *error) const
{
    error->clear();
    if (m_repository.isStandaloneLog()) {
        *assets = m_repository.logAssets(error);
        return error->isEmpty();
    }
    QSettings settings;
    const auto addFiles = [&](const QString &prefix, QStringList paths) {
        paths.removeDuplicates();
        for (const QString &path : paths) {
            if (path.isEmpty()) {
                continue;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("%1: %2").arg(path, file.errorString());
                return false;
            }
            const QByteArray contents = file.readAll();
            if (file.error() != QFileDevice::NoError) {
                *error = QStringLiteral("%1: %2").arg(path, file.errorString());
                return false;
            }
            assets->insert(supportAssetKey(prefix, path), contents);
        }
        return true;
    };
    auto history = settings.value(QStringLiteral("supportData/callHistoryFiles")).toStringList();
    if (history.isEmpty()) {
        history.append(settings.value(QStringLiteral("supportData/callHistory")).toString());
    }
    auto master = settings.value(QStringLiteral("supportData/masterCpyFiles")).toStringList();
    if (master.isEmpty()) {
        master.append(settings.value(QStringLiteral("supportData/masterCpy")).toString());
    }
    master.append(settings.value(QStringLiteral("supportData/masterScpPath")).toString());
    return addFiles(QStringLiteral("support/history"), history)
        && addFiles(QStringLiteral("support/master"), master)
        && addFiles(QStringLiteral("support/cty"),
                    {settings.value(QStringLiteral("supportData/ctyDat")).toString()});
}

bool MainWindow::storeSupportAsset(const QString &key, const QByteArray &contents)
{
    if (!m_repository.isStandaloneLog()) {
        return true;
    }
    QString error;
    if (!m_repository.setLogAsset(key, contents, &error)) {
        QMessageBox::critical(this, tr("Unable to save log data"), error);
        return false;
    }
    return true;
}

void MainWindow::loadSupportData()
{
    m_callHistory.clear();
    m_masterCallsigns.clear();
    m_ctyPrefixes.clear();
    if (m_hostRepository) {
        m_hostRepository->setCtyPrefixes(m_ctyPrefixes);
    }
    m_ctyCache.clear();
    if (m_repository.isStandaloneLog()) {
        QString error;
        const auto assets = m_repository.logAssets(&error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to read log data"), error);
            return;
        }
        for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
            const QString contents = QString::fromUtf8(it.value());
            bool valid = true;
            if (it.key().startsWith(QLatin1String("support/history/"))) {
                QHash<QString, CallHistoryEntry> entries;
                valid = SupportDataParser::parseCallHistory(contents, entries, &error);
                for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) {
                    m_callHistory.insert(entry.key(), entry.value());
                }
            } else if (it.key().startsWith(QLatin1String("support/master/"))) {
                QStringList callsigns;
                valid = SupportDataParser::parseMasterCpy(contents, callsigns, &error);
                m_masterCallsigns.append(callsigns);
            } else if (it.key() == QLatin1String("support/cty")) {
                valid = SupportDataParser::parseCtyDat(contents, m_ctyPrefixes, &error);
            }
            if (!valid) {
                QMessageBox::critical(this, tr("Invalid embedded support data"),
                                      QStringLiteral("%1: %2").arg(it.key(), error));
                return;
            }
        }
        m_masterCallsigns.removeDuplicates();
        rebuildCheckPartialIndex();
        for (int vfo = 0; vfo < 2; ++vfo) {
            updateCallLookup(vfo);
        }
        return;
    }
    QSettings settings;
    const auto readFile = [this](const QString &path, QString &contents) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(
                this, tr("Support data unavailable"),
                tr("Could not open %1: %2").arg(path, file.errorString()));
            return false;
        }
        contents = QString::fromUtf8(file.readAll());
        return true;
    };

    QString error;
    QStringList historyPaths = settings.value(
        QStringLiteral("supportData/callHistoryFiles")).toStringList();
    if (historyPaths.isEmpty()) {
        const QString legacyPath =
            settings.value(QStringLiteral("supportData/callHistory")).toString();
        if (!legacyPath.isEmpty()) {
            historyPaths.append(legacyPath);
        }
    }
    for (const QString &path : historyPaths) {
        QString contents;
        if (!readFile(path, contents)) {
            continue;
        }
        QHash<QString, CallHistoryEntry> entries;
        if (SupportDataParser::parseCallHistory(
                contents, entries, &error)) {
            for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
                m_callHistory.insert(it.key(), it.value());
            }
        } else {
            QMessageBox::warning(this, tr("Call history unavailable"),
                                 tr("%1: %2").arg(path, error));
        }
    }
    QStringList masterPaths = settings.value(
        QStringLiteral("supportData/masterCpyFiles")).toStringList();
    if (masterPaths.isEmpty()) {
        const QString legacyPath =
            settings.value(QStringLiteral("supportData/masterCpy")).toString();
        if (!legacyPath.isEmpty()) {
            masterPaths.append(legacyPath);
        }
    }
    bool masterPathsPruned = false;
    // Paths saved by older versions pointed into the nested ContestLogger/ContestLogger directory.
    for (QString &path : masterPaths) {
        if (!QFileInfo::exists(path)) {
            const QString relocated = QDir(applicationDataDirectory())
                                          .filePath(QFileInfo(path).fileName());
            if (QFileInfo::exists(relocated)) {
                path = relocated;
                masterPathsPruned = true;
            }
        }
    }
    masterPaths.removeDuplicates();
    for (const QString &path : masterPaths) {
        if (!QFileInfo::exists(path)) {
            masterPathsPruned = true;
            continue;
        }
        QString contents;
        if (!readFile(path, contents)) {
            continue;
        }
        QStringList callsigns;
        if (SupportDataParser::parseMasterCpy(
                contents, callsigns, &error)) {
            for (const QString &callsign : callsigns) {
                if (!m_masterCallsigns.contains(callsign)) {
                    m_masterCallsigns.append(callsign);
                }
            }
        } else {
            QMessageBox::warning(this, tr("master.cpy unavailable"),
                                 tr("%1: %2").arg(path, error));
        }
    }
    if (masterPathsPruned) {
        QStringList existing;
        for (const QString &path : masterPaths) {
            if (QFileInfo::exists(path)) {
                existing.append(path);
            }
        }
        settings.setValue(QStringLiteral("supportData/masterCpyFiles"), existing);
        const QString scpPath =
            settings.value(QStringLiteral("supportData/masterScpPath")).toString();
        if (!scpPath.isEmpty() && !QFileInfo::exists(scpPath)) {
            const QString relocated = QDir(applicationDataDirectory())
                                          .filePath(QFileInfo(scpPath).fileName());
            if (QFileInfo::exists(relocated)) {
                settings.setValue(QStringLiteral("supportData/masterScpPath"), relocated);
            } else {
                settings.remove(QStringLiteral("supportData/masterScpPath"));
                settings.remove(QStringLiteral("supportData/masterScpEtag"));
            }
        }
    }
    const QString ctyPath =
        settings.value(QStringLiteral("supportData/ctyDat")).toString();
    if (!ctyPath.isEmpty()) {
        QString contents;
        QVector<CtyPrefixEntry> prefixes;
        if (readFile(ctyPath, contents)) {
            if (SupportDataParser::parseCtyDat(contents, prefixes, &error)) {
                m_ctyPrefixes = std::move(prefixes);
                if (m_hostRepository) {
                    m_hostRepository->setCtyPrefixes(m_ctyPrefixes);
                }
                m_ctyCache.clear();
                if (!m_logQsos.isEmpty()) {
                    refreshRecentQsos();
                }
            } else {
                QMessageBox::warning(this, tr("wl_cty.dat unavailable"),
                                     tr("%1: %2").arg(ctyPath, error));
            }
        }
    }
    rebuildCheckPartialIndex();
    for (int vfo = 0; vfo < 2; ++vfo) {
        updateCallLookup(vfo);
    }
}

void MainWindow::rebuildCheckPartialIndex()
{
    QSet<QString> unique;
    for (const QString &callsign : std::as_const(m_masterCallsigns)) {
        unique.insert(callsign.trimmed().toUpper());
    }
    for (auto it = m_callHistory.cbegin(); it != m_callHistory.cend(); ++it) {
        unique.insert(it.key().trimmed().toUpper());
    }
    unique.remove(QString());
    m_checkCallsigns = QStringList(unique.cbegin(), unique.cend());
    std::sort(m_checkCallsigns.begin(), m_checkCallsigns.end());
    refreshCheckWindows();
}

void MainWindow::updateCallLookup(int vfo)
{
    vfo = qBound(0, vfo, 1);
    const QString callsign = m_callsignEdits[vfo]->text().trimmed().toUpper();
    if (callsign.isEmpty()) {
        m_callInfoLabels[vfo]->clear();
        return;
    }
    QStringList details;
    const auto history = m_callHistory.constFind(callsign);
    if (history != m_callHistory.cend()) {
        const QString name = history->fields.value(QStringLiteral("name")).trimmed();
        const QString qth = history->fields.value(QStringLiteral("qth")).trimmed();
        if (!name.isEmpty()) {
            details.append(name);
        }
        if (!qth.isEmpty()) {
            details.append(qth);
        }

        const auto definition = m_definitions.constFind(
            m_contestSelector->currentData().toLongLong());
        if (definition != m_definitions.cend()) {
            const QStringList legacyExchange = history->fields.value(
                QStringLiteral("exchange")).split(
                    QRegularExpression(QStringLiteral("\\s+")),
                    Qt::SkipEmptyParts);
            qsizetype nonReportFields = 0;
            for (const ExchangeFieldDefinition &field : definition->receivedExchange) {
                if (field.type != ExchangeFieldType::Rst) {
                    ++nonReportFields;
                }
            }
            qsizetype nonReportIndex = 0;
            for (qsizetype fieldIndex = 0;
                 fieldIndex < definition->receivedExchange.size(); ++fieldIndex) {
                const ExchangeFieldDefinition &field =
                    definition->receivedExchange.at(fieldIndex);
                QString key = field.name.trimmed().toLower();
                key.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                            QStringLiteral("_"));
                QString value = history->fields.value(key);
                if (value.isEmpty()) {
                    QString labelKey = field.label.trimmed().toLower();
                    labelKey.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                                     QStringLiteral("_"));
                    value = history->fields.value(labelKey);
                }
                if (value.isEmpty()
                    && field.type == ExchangeFieldType::Rst) {
                    value = history->fields.value(QStringLiteral("report"));
                }
                if (value.isEmpty() && field.type == ExchangeFieldType::Serial) {
                    value = history->fields.value(QStringLiteral("serial"));
                }
                if (value.isEmpty() && field.name.compare(
                        QStringLiteral("zone"), Qt::CaseInsensitive) == 0) {
                    const QString label = field.label.toLower();
                    value = history->fields.value(
                        label.contains(QStringLiteral("itu"))
                            ? QStringLiteral("itu_zone")
                            : QStringLiteral("cq_zone"));
                }
                if (value.isEmpty() && !legacyExchange.isEmpty()) {
                    if (legacyExchange.size() == definition->receivedExchange.size()) {
                        value = legacyExchange.at(fieldIndex);
                    } else if (field.type != ExchangeFieldType::Rst
                               && legacyExchange.size() == nonReportFields) {
                        value = legacyExchange.at(nonReportIndex);
                    } else if (field.type != ExchangeFieldType::Rst
                               && legacyExchange.size() == 1
                               && nonReportFields == 1) {
                        const bool isOnlyReport =
                            !definition->receivedExchange.isEmpty()
                            && definition->receivedExchange.first().type
                                == ExchangeFieldType::Rst
                            && QRegularExpression(
                                   QStringLiteral("^[1-5][1-9][1-9]?$"))
                                   .match(legacyExchange.first()).hasMatch();
                        if (!isOnlyReport) {
                            value = legacyExchange.first();
                        }
                    }
                }
                if (field.type != ExchangeFieldType::Rst) {
                    ++nonReportIndex;
                }
                QLineEdit *edit = m_exchangeEdits[vfo].value(field.name, nullptr);
                if (edit != nullptr && edit->text().trimmed().isEmpty()
                    && !value.trimmed().isEmpty()) {
                    edit->setText(value.trimmed());
                }
            }
        }
    }

    if (const CtyPrefixEntry *bestMatch =
            SupportDataParser::findCtyEntry(callsign, m_ctyPrefixes)) {
        details.append(tr("%1 · CQ %2 / ITU %3")
                           .arg(bestMatch->entity)
                           .arg(bestMatch->cqZone)
                           .arg(bestMatch->ituZone));
    }
    m_callInfoLabels[vfo]->setText(details.join(QStringLiteral(" — ")));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress) {
        return QMainWindow::eventFilter(watched, event);
    }
    const auto *keyEvent = static_cast<QKeyEvent *>(event);
    if ((watched == m_bandmap || watched == m_radioABandmap)
        && (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)) {
        auto *bandmap = static_cast<QTableWidget *>(watched);
        const QTableWidgetItem *item = bandmap->item(bandmap->currentRow(), 0);
        if (item != nullptr) {
            if (bandmap == m_radioABandmap) {
                const int radio = m_so2rToggle->isChecked()
                    ? 0 : m_activeRadioSelector->currentData().toInt();
                activateSpotId(item->data(Qt::UserRole).toLongLong(), radio, 0);
            } else {
                activateSpot(bandmap->currentRow(), 0);
            }
        }
        return true;
    }
    if (keyEvent->key() != Qt::Key_Space
        || keyEvent->modifiers() != Qt::NoModifier) {
        return QMainWindow::eventFilter(watched, event);
    }

    for (int vfo = 0; vfo < 2; ++vfo) {
        if (watched == m_callsignEdits[vfo]) {
            const auto definition = m_definitions.constFind(
                m_contestSelector->currentData().toLongLong());
            if (definition != m_definitions.cend()
                && !definition->receivedExchange.isEmpty()) {
                QLineEdit *first = nullptr;
                for (const ExchangeFieldDefinition &field : definition->receivedExchange) {
                    first = m_exchangeEdits[vfo].value(field.name, nullptr);
                    if (first != nullptr) {
                        break;
                    }
                }
                if (first != nullptr) {
                    first->setFocus(Qt::TabFocusReason);
                    return true;
                }
            }
            return QMainWindow::eventFilter(watched, event);
        }
        qsizetype currentIndex = -1;
        for (auto it = m_exchangeEdits[vfo].cbegin();
             it != m_exchangeEdits[vfo].cend(); ++it) {
            if (it.value() == watched) {
                const auto definition = m_definitions.constFind(
                    m_contestSelector->currentData().toLongLong());
                if (definition != m_definitions.cend()) {
                    for (qsizetype i = 0; i < definition->receivedExchange.size(); ++i) {
                        if (definition->receivedExchange.at(i).name == it.key()) {
                            currentIndex = i;
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (currentIndex >= 0) {
            const auto definition = m_definitions.constFind(
                m_contestSelector->currentData().toLongLong());
            if (definition != m_definitions.cend()) {
                QLineEdit *next = m_callsignEdits[vfo];
                for (qsizetype i = currentIndex + 1;
                     i < definition->receivedExchange.size(); ++i) {
                    QLineEdit *candidate = m_exchangeEdits[vfo].value(
                        definition->receivedExchange.at(i).name, nullptr);
                    if (candidate != nullptr) {
                        next = candidate;
                        break;
                    }
                }
                if (next != nullptr) {
                    next->setFocus(Qt::TabFocusReason);
                    if (next == m_callsignEdits[vfo]) {
                        next->selectAll();
                    }
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::loadBuiltInDefinitions()
{
    if (m_repository.isStandaloneLog()) {
        return;
    }
    QString error;
    if (!installBundledContestDefinitions(&error)) {
        QMessageBox::critical(this, tr("Unable to install example definition"), error);
        return;
    }
    QVector<ContestDefinitionFile> files;
    if (!loadContestDefinitionFiles(&files, &error)) {
        QMessageBox::critical(this, tr("Invalid contest definition"), error);
        return;
    }
    for (const ContestDefinitionFile &file : files) {
        const ContestDefinition &definition = file.definition;
        if (!definition.cabrilloContest.isEmpty()) {
            m_builtInCabrilloContests.insert(definition.id, definition.cabrilloContest);
        }
    }
}

void MainWindow::loadOperator()
{
    LogSettings settings(m_repository);
    settings.beginGroup(QStringLiteral("station"));
    m_defaultStation.callsign = settings.value(QStringLiteral("callsign")).toString();
    m_defaultStation.operatorName =
        settings.value(QStringLiteral("operatorName")).toString();
    m_operatorCallsign =
        settings.value(QStringLiteral("operatorCallsign")).toString().trimmed().toUpper();
    m_defaultStation.grid = settings.value(QStringLiteral("grid")).toString();
    m_defaultStation.powerClass = settings.value(QStringLiteral("powerClass")).toString();
    m_defaultStation.email = settings.value(QStringLiteral("email")).toString();
    m_defaultStation.address = settings.value(QStringLiteral("address")).toString();
    m_defaultStation.city = settings.value(QStringLiteral("city")).toString();
    m_defaultStation.stateProvince =
        settings.value(QStringLiteral("stateProvince")).toString();
    m_defaultStation.postalCode = settings.value(QStringLiteral("postalCode")).toString();
    m_defaultStation.country = settings.value(QStringLiteral("country")).toString();
    settings.endGroup();
    m_station = m_defaultStation;
}

void MainWindow::loadContests()
{
    QString error;
    m_contests = m_repository.contests(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to load logs"), error);
        return;
    }

    m_definitions.clear();
    for (const Contest &contest : m_contests) {
        const QString yaml = m_repository.contestDefinitionYaml(contest.id, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to load contest definition"), error);
            return;
        }
        if (yaml.isEmpty()) {
            continue;
        }
        ContestDefinition definition;
        if (!ContestDefinitionLoader().loadText(
                yaml, contest.code, definition, &error)) {
            QMessageBox::critical(this, tr("Stored contest definition is invalid"), error);
            continue;
        }
        m_definitions.insert(contest.id, std::move(definition));
    }

    int selectedIndex = -1;
    {
        const QSignalBlocker blocker(m_contestSelector);
        m_contestSelector->clear();
        for (const Contest &contest : m_contests) {
            m_contestSelector->addItem(contest.name, contest.id);
        }
        if (!m_contests.isEmpty()) {
            QSettings settings;
            const qint64 previousContest =
                settings.value(QStringLiteral("session/activeContestId")).toLongLong();
            const int previousIndex = m_contestSelector->findData(previousContest);
            if (previousIndex >= 0) {
                m_contestSelector->setCurrentIndex(previousIndex);
            }
            selectedIndex = m_contestSelector->currentIndex();
        }
    }
    if (selectedIndex < 0) {
        m_activeContestLabel->setText(tr("No logs are defined."));
        selectContest(-1);
    } else {
        selectContest(selectedIndex);
    }
}

void MainWindow::updateStationLabel()
{
    const QString operatorText = operatorIdentity().isEmpty()
        ? tr("Operator not configured")
        : operatorIdentity().toHtmlEscaped();
    const QString callsignText = m_station.callsign.isEmpty()
        ? tr("Callsign not set")
        : m_station.callsign.toHtmlEscaped();
    const QString gridText = m_station.grid.isEmpty()
        ? tr("Grid not set")
        : m_station.grid.toHtmlEscaped();
    m_stationLabel->setText(
        tr("<b>%1</b> &mdash; %2 &mdash; %3")
            .arg(callsignText, operatorText, gridText));
    updateTopStatus();
}

void MainWindow::rebuildQsoForm()
{
    rebuildQsoForm(0);
    rebuildQsoForm(1);
}

void MainWindow::rebuildQsoForm(int vfo)
{
    for (QHBoxLayout *layout : {m_exchangeLayouts[vfo], m_sentExchangeLayouts[vfo]}) {
        while (QLayoutItem *item = layout->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
    }
    m_exchangeEdits[vfo].clear();
    m_sentExchangeEdits[vfo].clear();
    m_reserveSerialButtons[vfo] = nullptr;
    m_cancelSerialButtons[vfo] = nullptr;
    if (m_contestSelector->currentIndex() < 0) {
        return;
    }

    const qint64 contestId =
        m_contestSelector->currentData().toLongLong();
    const auto definition = m_definitions.constFind(contestId);
    if (definition == m_definitions.cend()) {
        return;
    }
    QString historyError;
    const QVector<Qso> priorQsos =
        m_repository.qsosForContest(contestId, 0, &historyError);
    if (!historyError.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to prepare sent exchange"),
                              historyError);
        return;
    }
    const int nextSerial = ContestEngine().nextSerialNumber(*definition, priorQsos);
    const QFont sentFont = monoFont(11, true);
    const QFont receivedFont = monoFont(14, true);
    const auto fieldWidth = [](const QFont &font, int characters) {
        const QFontMetrics metrics(font);
        return metrics.horizontalAdvance(QLatin1Char('W')) * qMax(characters, 3) + 14;
    };
    QWidget *panel = m_entryGroups[vfo];
    LogSettings settings(m_repository);
    for (const ExchangeFieldDefinition &field : definition->sentExchange) {
        auto *edit = new QLineEdit;
        edit->setObjectName(QStringLiteral("sentExchange_%1").arg(field.name));
        edit->setProperty("sentField", true);
        edit->setFont(sentFont);
        edit->setFocusPolicy(Qt::ClickFocus);
        edit->setToolTip(tr("Sent %1 (station setting, click to change)").arg(field.label));
        if (field.width > 0) {
            edit->setMaxLength(field.width);
        }
        edit->setFixedWidth(fieldWidth(sentFont, field.width > 0 ? field.width : 4));
        const QString settingKey = QStringLiteral("contest/sentExchange/%1/%2/vfo%3")
            .arg(contestId)
            .arg(field.name)
            .arg(vfo);
        QString configured = settings.value(settingKey, field.defaultValue).toString();
        edit->setText(configured);
        if (field.type == ExchangeFieldType::Serial && field.generated) {
            if (definition->serialServerRequired) {
                configured.clear();
            } else {
                configured = QString::number(nextSerial);
                if (field.width > 0) {
                    configured = configured.rightJustified(field.width, QLatin1Char('0'));
                }
            }
            edit->setText(configured);
            edit->setReadOnly(true);
            edit->setToolTip(definition->serialServerRequired
                ? tr("Reserve this serial from the central server before transmission")
                : tr("Sent serial number, generated on logging"));
        }
        const QString sentLabel = field.label.size() > 10 ? field.name.toUpper() : field.label;
        QWidget *sentColumn = fieldColumn(tr("Snt %1").arg(sentLabel), edit, panel);
        sentColumn->setToolTip(tr("Sent %1").arg(field.label));
        m_sentExchangeLayouts[vfo]->addWidget(sentColumn);
        m_sentExchangeEdits[vfo].insert(field.name, edit);
        if (field.type == ExchangeFieldType::Serial && field.generated
            && definition->serialServerRequired) {
            auto *reserveButton = new QPushButton(tr("Reserve"), panel);
            reserveButton->setObjectName(
                QStringLiteral("reserveSerialButtonV%1").arg(vfo));
            reserveButton->setToolTip(
                tr("Request the next serial number from the central server"));
            connect(reserveButton, &QPushButton::clicked, this,
                    [this, vfo] { reserveSerialForVfo(vfo); });
            m_sentExchangeLayouts[vfo]->addWidget(reserveButton);
            m_reserveSerialButtons[vfo] = reserveButton;
            auto *cancelButton = new QPushButton(tr("Cancel serial"), panel);
            cancelButton->setObjectName(
                QStringLiteral("cancelSerialButtonV%1").arg(vfo));
            cancelButton->setToolTip(
                tr("Permanently cancel this unused serial; it will not be reused"));
            cancelButton->setEnabled(false);
            connect(cancelButton, &QPushButton::clicked, this,
                    [this, vfo] { cancelSerialForVfo(vfo); });
            m_sentExchangeLayouts[vfo]->addWidget(cancelButton);
            m_cancelSerialButtons[vfo] = cancelButton;
        }
        if (!(field.type == ExchangeFieldType::Serial && field.generated)) {
            installUppercase(edit);
            connect(edit, &QLineEdit::textChanged, this,
                    [this, settingKey](const QString &value) {
                        LogSettings(m_repository).setValue(settingKey, value);
                    });
        }
        edit->installEventFilter(this);
    }
    for (const ExchangeFieldDefinition &field : definition->receivedExchange) {
        if (field.type == ExchangeFieldType::Rst) {
            continue;
        }
        auto *edit = new QLineEdit;
        edit->setObjectName(QStringLiteral("exchange_%1").arg(field.name));
        edit->installEventFilter(this);
        edit->setFont(receivedFont);
        edit->setToolTip(field.required ? tr("%1 (required)").arg(field.label)
                                        : tr("%1 (optional)").arg(field.label));
        if (field.width > 0) {
            edit->setMaxLength(field.width);
        }
        edit->setMinimumWidth(fieldWidth(receivedFont, field.width > 0 ? field.width : 5));
        installUppercase(edit);
        QString receivedLabel = field.label.size() > 14 ? field.name.toUpper() : field.label;
        m_exchangeLayouts[vfo]->addWidget(
            fieldColumn(field.required ? receivedLabel : receivedLabel + QLatin1Char('?'),
                        edit, panel),
            field.width > 0 && field.width <= 3 ? 0 : 1);
        m_exchangeEdits[vfo].insert(field.name, edit);
        connect(edit, &QLineEdit::returnPressed,
                this, [this, vfo] { handleEntryEnter(vfo, false); });
        connect(edit, &QLineEdit::textChanged,
                this, [this, vfo] { updateDupeIndicatorForVfo(vfo); });
    }
    updateSentExchangeVisibility(vfo);
    updateSerialFields();
}

void MainWindow::updateSentReportForMode(int vfo)
{
    vfo = qBound(0, vfo, 1);
    const auto definition = m_definitions.constFind(
        m_contestSelector->currentData().toLongLong());
    if (definition == m_definitions.cend()) {
        return;
    }
    const QString mode = m_modeSelectors[vfo]->currentText().trimmed();
    const bool voiceMode =
        mode.compare(QStringLiteral("SSB"), Qt::CaseInsensitive) == 0
        || mode.compare(QStringLiteral("USB"), Qt::CaseInsensitive) == 0
        || mode.compare(QStringLiteral("LSB"), Qt::CaseInsensitive) == 0
        || mode.compare(QStringLiteral("FM"), Qt::CaseInsensitive) == 0
        || mode.compare(QStringLiteral("AM"), Qt::CaseInsensitive) == 0;
    const QString report = voiceMode ? QStringLiteral("59")
                                     : QStringLiteral("599");
    for (const ExchangeFieldDefinition &field : definition->sentExchange) {
        if (field.type != ExchangeFieldType::Rst) {
            continue;
        }
        QLineEdit *edit = m_sentExchangeEdits[vfo].value(field.name, nullptr);
        if (edit == nullptr) {
            continue;
        }
        const QString current = edit->text().trimmed();
        if (current.isEmpty() || current == QStringLiteral("59")
            || current == QStringLiteral("599")) {
            edit->setText(report);
        }
    }
}

const ContestDefinition *MainWindow::activeDefinition() const
{
    if (m_contestSelector == nullptr || m_contestSelector->currentIndex() < 0) {
        return nullptr;
    }
    const auto definition =
        m_definitions.constFind(m_contestSelector->currentData().toLongLong());
    return definition == m_definitions.cend() ? nullptr : &definition.value();
}

QString MainWindow::exchangeDisplay(const QMap<QString, QString> &fields,
                                    const QString &fallback, bool sent) const
{
    QStringList values;
    if (const ContestDefinition *definition = activeDefinition()) {
        for (const ExchangeFieldDefinition &field :
             sent ? definition->sentExchange : definition->receivedExchange) {
            const QString value = fields.value(field.name).trimmed();
            if (!value.isEmpty()) {
                values.append(value.toUpper());
            }
        }
    } else {
        for (const QString &value : fields) {
            if (!value.trimmed().isEmpty()) {
                values.append(value.trimmed().toUpper());
            }
        }
    }
    return values.isEmpty() ? fallback : values.join(QLatin1Char(' '));
}

void MainWindow::refreshRecentQsos()
{
    if (m_syncClient) {
        m_syncClient->flushPendingChanges();
    }
    m_recentQsos->setRowCount(0);
    m_logQsos.clear();
    m_workedMultiplierKeys.clear();
    if (m_contestSelector->currentIndex() >= 0) {
        QString error;
        QVector<Qso> qsos = m_repository.qsosForContest(
            m_contestSelector->currentData().toLongLong(), 0, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to load the log"), error);
        } else {
            std::sort(qsos.begin(), qsos.end(), [](const Qso &left, const Qso &right) {
                if (left.timestampUtc != right.timestampUtc) {
                    return left.timestampUtc < right.timestampUtc;
                }
                return left.id < right.id;
            });
            m_logQsos = std::move(qsos);
        }
    }
    const ContestDefinition *definition = activeDefinition();
    m_score = ContestScore{};
    if (definition != nullptr) {
        const ContestEngine engine;
        const auto resolver = multiplierResolver();
        m_score = engine.score(*definition, m_logQsos, resolver);
        m_workedMultiplierKeys =
            engine.buildContext(*definition, m_logQsos, resolver).workedMultiplierKeys;
    }

    const auto updateFilterOptions = [this](
                                         QComboBox *filter,
                                         const QStringList &values,
                                         const QString &allLabel,
                                         bool lowercase) {
        const QString selected = filter->currentData().toString();
        const QSignalBlocker blocker(filter);
        filter->clear();
        filter->addItem(allLabel, QStringLiteral("all"));
        QStringList options = values;
        for (const Qso &qso : std::as_const(m_logQsos)) {
            const QString value = lowercase ? qso.band.toLower() : qso.mode.toUpper();
            if (!value.isEmpty() && !options.contains(value, Qt::CaseInsensitive)) {
                options.append(value);
            }
        }
        std::sort(options.begin(), options.end(), [](const QString &left, const QString &right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });
        for (const QString &value : std::as_const(options)) {
            filter->addItem(value, lowercase ? value.toLower() : value.toUpper());
        }
        const int selectedIndex = filter->findData(selected);
        filter->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    };
    updateFilterOptions(m_logBandFilter,
                        definition != nullptr ? definition->bands : QStringList{},
                        tr("All bands"), true);
    updateFilterOptions(m_logModeFilter,
                        definition != nullptr ? definition->modes : QStringList{},
                        tr("All modes"), false);
    m_logTableNeedsColumnResize = true;
    refreshRecentQsoTable();

    refreshScoreSummary();
    refreshRate();
    updateTopStatus();
    refreshCheckWindows();
}

void MainWindow::refreshRecentQsoTable()
{
    if (m_recentQsos == nullptr) {
        return;
    }
    const QString query = m_logSearchEdit->text().trimmed();
    const QString bandFilter = m_logBandFilter->currentData().toString();
    const QString modeFilter = m_logModeFilter->currentData().toString();
    const QString validityFilter = m_logValidityFilter->currentData().toString();
    QVector<qsizetype> matchingIndices;
    matchingIndices.reserve(qMin<qsizetype>(m_logQsos.size(), 1000));
    qsizetype matchingCount = 0;
    constexpr qsizetype kVisibleRows = 1000;
    for (qsizetype index = m_logQsos.size(); index > 0; --index) {
        const qsizetype qsoIndex = index - 1;
        const Qso &qso = m_logQsos.at(qsoIndex);
        if (bandFilter != QStringLiteral("all")
            && qso.band.compare(bandFilter, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (modeFilter != QStringLiteral("all")
            && qso.mode.compare(modeFilter, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if ((validityFilter == QStringLiteral("valid") && qso.invalid)
            || (validityFilter == QStringLiteral("invalid") && !qso.invalid)) {
            continue;
        }
        if (!query.isEmpty()) {
            const QString haystack = qso.callsign + QLatin1Char(' ')
                + qso.band + QLatin1Char(' ') + qso.mode + QLatin1Char(' ')
                + exchangeDisplay(qso.sentExchangeFields, qso.sentExchange, true)
                + QLatin1Char(' ')
                + exchangeDisplay(qso.receivedExchangeFields, qso.receivedExchange, false);
            if (!haystack.contains(query, Qt::CaseInsensitive)) {
                continue;
            }
        }
        ++matchingCount;
        if (matchingIndices.size() < kVisibleRows) {
            matchingIndices.append(qsoIndex);
        }
    }
    std::reverse(matchingIndices.begin(), matchingIndices.end());

    m_recentQsos->setUpdatesEnabled(false);
    m_recentQsos->setRowCount(static_cast<int>(matchingIndices.size()));
    const QColor multColor(QStringLiteral("#CE0000"));
    for (qsizetype rowIndex = 0; rowIndex < matchingIndices.size(); ++rowIndex) {
        const qsizetype index = matchingIndices.at(rowIndex);
        const Qso &qso = m_logQsos.at(index);
        const int increment = m_score.newMultipliersByQsoId.value(qso.id);
        const int row = static_cast<int>(rowIndex);
        const QStringList values{
            QString::number(index + 1),
            qso.timestampUtc.toUTC().toString(QStringLiteral("HHmm")),
            qso.band, qso.mode,
            qso.frequencyHz > 0
                ? QString::number(static_cast<double>(qso.frequencyHz) / 1000.0, 'f', 1)
                : QString(),
            qso.callsign,
            exchangeDisplay(qso.sentExchangeFields, qso.sentExchange, true),
            exchangeDisplay(qso.receivedExchangeFields, qso.receivedExchange, false),
            increment > 0 ? QString::number(increment) : QString(),
            QString::number(qso.points)};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                item->setData(Qt::UserRole, qso.id);
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            } else if (column == 4 || column == 9) {
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
            if (column == 8 && increment > 0) {
                item->setForeground(multColor);
            }
            if (qso.invalid) {
                QFont font = item->font();
                font.setStrikeOut(true);
                item->setFont(font);
                item->setForeground(QColor(QStringLiteral("#A0A0A0")));
                item->setToolTip(tr("Invalid QSO; excluded from scoring and exports."));
            }
            m_recentQsos->setItem(row, column, item);
        }
    }
    m_logFilterResultLabel->setText(
        tr("%1 of %2 QSOs").arg(matchingCount).arg(m_logQsos.size()));
    if (m_logTableNeedsColumnResize) {
        m_recentQsos->resizeColumnsToContents();
        const QFontMetrics metrics(m_recentQsos->font());
        static const int minimumChars[] = {5, 5, 5, 4, 8, 11, 8, 10, 6, 4};
        for (int column = 0; column < 10; ++column) {
            const int minimum = metrics.horizontalAdvance(QLatin1Char('W'))
                                    * minimumChars[column] + 8;
            if (m_recentQsos->columnWidth(column) < minimum) {
                m_recentQsos->setColumnWidth(column, minimum);
            }
        }
        m_logTableNeedsColumnResize = false;
    }
    m_recentQsos->setUpdatesEnabled(true);
    m_recentQsos->scrollToBottom();
}

void MainWindow::refreshScoreSummary()
{
    const ContestDefinition *definition = activeDefinition();
    QStringList bands = definition != nullptr ? definition->bands : QStringList{};
    for (const Qso &qso : std::as_const(m_logQsos)) {
        if (!bands.contains(qso.band, Qt::CaseInsensitive)) {
            bands.append(qso.band);
        }
    }
    struct BandTotals {
        int qsos = 0;
        int points = 0;
        int multipliers = 0;
    };
    QHash<QString, BandTotals> totals;
    BandTotals grand;
    if (definition != nullptr) {
        for (auto it = m_score.perBand.cbegin(); it != m_score.perBand.cend(); ++it) {
            totals.insert(it.key(), {it->qsos, it->points, it->multipliers});
        }
        grand = {m_score.totalQsos, m_score.totalPoints, m_score.totalMultipliers};
    } else {
        for (const Qso &qso : std::as_const(m_logQsos)) {
            if (qso.invalid) {
                continue;
            }
            BandTotals &band = totals[qso.band.toLower()];
            ++band.qsos;
            band.points += qso.points;
            ++grand.qsos;
            grand.points += qso.points;
        }
    }
    const bool hasMultipliers = definition != nullptr && !definition->multipliers.isEmpty();
    const bool unresolved = !m_score.unresolvedMultiplierKeys.isEmpty();
    m_scorePoints = grand.points;
    m_scoreMultipliers = grand.multipliers;
    m_scoreTotal = definition != nullptr ? m_score.finalScore : grand.points;

    const QString activeBand =
        m_bandSelectors[currentEntryContext()]->currentText().trimmed().toLower();
    m_summaryTable->clear();
    m_summaryTable->setColumnCount(5);
    m_summaryTable->setHorizontalHeaderLabels(
        {tr("Band"), tr("QSO"), tr("Pts"), tr("Mult"), tr("Avg")});
    m_summaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_summaryTable->setRowCount(static_cast<int>(bands.size()) + 1);
    const auto fillRow = [this](int row, const QString &band, const BandTotals &value,
                                const QColor &background, const QColor &foreground) {
        const QStringList cells{
            band, QString::number(value.qsos), QString::number(value.points),
            QString::number(value.multipliers),
            value.qsos > 0
                ? QString::number(static_cast<double>(value.points) / value.qsos, 'f', 2)
                : QStringLiteral("-")};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setTextAlignment(column == 0 ? Qt::AlignLeft | Qt::AlignVCenter
                                               : Qt::AlignRight | Qt::AlignVCenter);
            if (background.isValid()) {
                item->setBackground(background);
            }
            if (foreground.isValid()) {
                item->setForeground(foreground);
            }
            m_summaryTable->setItem(row, column, item);
        }
    };
    for (int row = 0; row < bands.size(); ++row) {
        const QString band = bands.at(row).toLower();
        const bool current = band == activeBand;
        fillRow(row, band, totals.value(band),
                current ? QColor(QStringLiteral("#4682B4")) : QColor(),
                current ? QColor(Qt::white) : QColor());
    }
    fillRow(static_cast<int>(bands.size()), tr("TOTAL"), grand,
            QColor(QStringLiteral("#C0C0C0")), QColor(Qt::black));
    if (!hasMultipliers) {
        m_summaryTable->hideColumn(3);
    } else {
        m_summaryTable->showColumn(3);
    }
    m_finalScoreLabel->setText(
        tr("FINAL SCORE: %1%2").arg(groupedNumber(m_scoreTotal),
                                    unresolved ? QStringLiteral(" *") : QString()));
    QString scoreTip;
    if (unresolved) {
        QStringList sample = m_score.unresolvedMultiplierKeys.mid(0, 10);
        scoreTip = tr("* %n QSO multiplier value(s) could not be resolved (load CTY.DAT for "
                      "DXCC/zone/continent multipliers):\n%1",
                      nullptr, static_cast<int>(m_score.unresolvedMultiplierKeys.size()))
                       .arg(sample.join(QLatin1Char('\n')));
    }
    m_finalScoreLabel->setToolTip(scoreTip);
}

void MainWindow::refreshRate()
{
    if (m_rateLabel == nullptr) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const ContestDefinition *definition = activeDefinition();
    int lastHour = 0;
    int lastTenMinutes = 0;
    for (auto it = m_logQsos.crbegin(); it != m_logQsos.crend(); ++it) {
        if (it->invalid) {
            continue;
        }
        const qint64 age = it->timestampUtc.secsTo(now);
        if (age > 3600) {
            break;
        }
        ++lastHour;
        if (age <= 600) {
            ++lastTenMinutes;
        }
    }
    const auto rateOfLast = [this, &now](qsizetype count) {
        QVector<const Qso *> validQsos;
        validQsos.reserve(m_logQsos.size());
        for (const Qso &qso : m_logQsos) {
            if (!qso.invalid) {
                validQsos.append(&qso);
            }
        }
        count = qMin(count, validQsos.size());
        if (count == 0) {
            return 0LL;
        }
        const qint64 seconds = qMax<qint64>(
            60, validQsos.at(validQsos.size() - count)->timestampUtc.secsTo(now));
        return static_cast<long long>(count * 3600 / seconds);
    };
    QVector<int> pace(12, 0);
    for (const Qso &qso : std::as_const(m_logQsos)) {
        if (qso.invalid) {
            continue;
        }
        const qint64 age = qso.timestampUtc.secsTo(now);
        if (age >= 0 && age < 3600) {
            const int bucket = 11 - static_cast<int>(age / 300);
            ++pace[bucket];
        }
    }
    if (m_rateChart != nullptr) {
        static_cast<RatePaceChart *>(m_rateChart)->setSamples(pace);
    }
    QString bandSummary = tr("By band:");
    QString modeSummary = tr("By mode:");
    if (definition != nullptr) {
        const auto appendGroups = [](QString &line, const auto &groups) {
            for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
                line += QStringLiteral(" %1 %2/%3/%4;")
                    .arg(it.key())
                    .arg(it->qsos)
                    .arg(it->points)
                    .arg(it->multipliers);
            }
        };
        appendGroups(bandSummary, m_score.perBand);
        appendGroups(modeSummary, m_score.perMode);
    }
    m_rateLabel->setText(
        tr("Last 10 QSOs  : %1 /h\n"
           "Last 100 QSOs : %2 /h\n"
           "Last 60 min   : %3 QSO\n"
           "Last 10 min   : %4 QSO\n"
           "Total QSOs    : %5\n"
           "CW speed      : %6 WPM\n%7\n%8")
            .arg(rateOfLast(10), 5)
            .arg(rateOfLast(100), 5)
            .arg(lastHour, 5)
            .arg(lastTenMinutes, 5)
            .arg(definition != nullptr
                    ? m_score.totalQsos
                    : static_cast<int>(std::count_if(
                        m_logQsos.cbegin(), m_logQsos.cend(),
                        [](const Qso &qso) { return !qso.invalid; })),
                 5)
            .arg(m_cwWpm, 5)
            .arg(bandSummary, modeSummary));
}

void MainWindow::updateTopStatus()
{
    if (m_topStatus == nullptr) {
        return;
    }
    const QString contest = m_contestSelector->currentIndex() >= 0
        ? m_contestSelector->currentText() : tr("no log");
    const QString station = m_station.callsign.isEmpty()
        ? tr("NO CALL") : m_station.callsign.toUpper();
    const ContestDefinition *definition = activeDefinition();
    const int context = currentEntryContext();
    const QString cwSpeed = m_modeSelectors[context] != nullptr
            && m_modeSelectors[context]->currentText().compare(
                   QStringLiteral("CW"), Qt::CaseInsensitive) == 0
        ? tr("  %1 WPM").arg(m_cwWpm) : QString();
    QString text = QStringLiteral("%1z  %2%3  │  %4  │  QSO %5  PTS %6")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")) + cwSpeed,
             station,
             operatorIdentity().isEmpty()
                 ? QString() : QStringLiteral(" (%1)").arg(operatorIdentity()),
             contest)
        .arg(definition != nullptr
                 ? m_score.totalQsos
                 : static_cast<int>(std::count_if(
                     m_logQsos.cbegin(), m_logQsos.cend(),
                     [](const Qso &qso) { return !qso.invalid; })))
        .arg(m_scorePoints);
    if (definition != nullptr && !definition->multipliers.isEmpty()) {
        text += QStringLiteral("  MULT %1").arg(m_scoreMultipliers);
    }
    text += QStringLiteral("  SCORE %1").arg(groupedNumber(m_scoreTotal));
    if (!m_score.unresolvedMultiplierKeys.isEmpty()) {
        text += QStringLiteral("*");
    }
    m_topStatus->setText(text);
}

int MainWindow::currentEntryContext() const
{
    const int context = qBound(0, m_activeEntryContext, 1);
    return m_entryGroups[context] != nullptr && m_entryGroups[context]->isVisible()
        ? context : 0;
}

void MainWindow::setActiveEntryContext(int context)
{
    context = qBound(0, context, 1);
    const bool contextChanged = context != m_activeEntryContext;
    m_activeEntryContext = context;
    if (contextChanged && m_so2vToggle->isChecked()
        && !m_so2rToggle->isChecked()) {
        const int radio = m_activeRadioSelector->currentData().toInt();
        const RadioSnapshot &snapshot = m_radioSnapshots[radio];
        if (snapshot.state == ConnectionState::Connected && snapshot.dualVfoSupported) {
            const auto vfo = std::find_if(
                snapshot.vfos.cbegin(), snapshot.vfos.cend(),
                [context](const Vfo &candidate) { return candidate.number == context; });
            if (snapshot.focusVfo != context) {
                m_radioControllers[radio].setVfoFocus(context);
            }
            if (vfo != snapshot.vfos.cend() && vfo->frequencyHz > 0) {
                m_radioControllers[radio].setFrequency(vfo->frequencyHz, context);
            }
        }
    }
    for (int index = 0; index < 2; ++index) {
        m_entryGroups[index]->setProperty("active", index == m_activeEntryContext);
        repolish(m_entryGroups[index]);
        repolish(m_entryHeaders[index]);
    }
    refreshCheckWindows();
    refreshBandmap();
    refreshScoreSummary();
    updateTopStatus();
    updateNetworkOperatingState();
}

void MainWindow::updateEntryHeaders()
{
    const bool so2r = m_so2rToggle->isChecked();
    const int activeRadio = m_activeRadioSelector->currentData().toInt();
    for (int context = 0; context < 2; ++context) {
        const int radio = so2r ? context : activeRadio;
        const RadioSnapshot &snapshot = m_radioSnapshots[radio];
        const int vfoNumber = so2r ? snapshot.focusVfo : context;
        QString title = so2r
            ? tr("RADIO %1").arg(radio == 0 ? QLatin1Char('A') : QLatin1Char('B'))
            : tr("VFO %1 · RADIO %2")
                  .arg(context == 0 ? QLatin1Char('A') : QLatin1Char('B'))
                  .arg(radio == 0 ? QLatin1Char('A') : QLatin1Char('B'));
        const bool catStale = radioCatStale(radio);
        if (snapshot.state == ConnectionState::Connected) {
            qint64 frequency = snapshot.frequencyHz;
            for (const Vfo &vfo : snapshot.vfos) {
                if (vfo.number == vfoNumber && vfo.frequencyHz > 0) {
                    frequency = vfo.frequencyHz;
                }
            }
            title += QStringLiteral("   %1 kHz")
                .arg(static_cast<double>(frequency) / 1000.0, 0, 'f', 2);
            if (catStale) {
                title += tr("   CAT STALE");
            }
        } else {
            title += tr("   (no CAT)");
        }
        title += QStringLiteral("   %1  %2")
            .arg(m_bandSelectors[context]->currentText(),
                 m_modeSelectors[context]->currentText());
        const bool transmitting = so2r
            ? activeRadio == context && snapshot.state == ConnectionState::Connected
            : snapshot.state == ConnectionState::Connected && snapshot.focusVfo == context;
        if (transmitting) {
            title += tr("   ◀ TX");
        }
        if (!so2r && m_so2vToggle->isChecked() && context == 1
            && !snapshot.dualReceiveSupported) {
            title += tr("   [SO2V, no dual RX]");
        }
        m_entryHeaders[context]->setText(title);
        m_entryHeaders[context]->setToolTip(
            catStale ? tr("No CAT update received for more than 5 seconds; band, mode and "
                          "frequency may be out of date.")
                     : QString());
    }
}

bool MainWindow::radioCatStale(int radio) const
{
    radio = qBound(0, radio, 1);
    return m_radioSnapshots[radio].state == ConnectionState::Connected
        && m_radioSnapshotTimes[radio].isValid()
        && m_radioSnapshotTimes[radio].msecsTo(QDateTime::currentDateTimeUtc()) > 5000;
}

ContestEngine::MultiplierResolver MainWindow::multiplierResolver() const
{
    if (m_ctyPrefixes.isEmpty()) {
        return {};
    }
    return [this](const Qso &qso) {
        const auto resolve = [this](const QString &rawCall) {
            const QString call = rawCall.trimmed().toUpper();
            const auto cached = m_ctyCache.constFind(call);
            if (cached != m_ctyCache.cend()) {
                return cached.value();
            }
            const QMap<QString, QString> resolved =
                SupportDataParser::resolveCtyMultipliers(call, m_ctyPrefixes);
            m_ctyCache.insert(call, resolved);
            return resolved;
        };
        QMap<QString, QString> resolved = resolve(qso.callsign);
        const QString stationCall = qso.stationCallsign.isEmpty()
            ? m_station.callsign : qso.stationCallsign;
        const QMap<QString, QString> station = resolve(stationCall);
        for (const QString &key : {QStringLiteral("dxcc"),
                                   QStringLiteral("country"),
                                   QStringLiteral("continent"),
                                   QStringLiteral("cq_zone"),
                                   QStringLiteral("itu_zone")}) {
            if (station.contains(key)) {
                resolved.insert(QStringLiteral("station.%1").arg(key),
                                station.value(key));
            }
        }
        if (resolved.contains(QStringLiteral("dxcc"))
            && station.contains(QStringLiteral("dxcc"))) {
            const bool sameEntity = resolved.value(QStringLiteral("dxcc"))
                == station.value(QStringLiteral("dxcc"));
            resolved.insert(QStringLiteral("same_dxcc"), sameEntity ? "true" : "false");
            resolved.insert(QStringLiteral("same_country"), sameEntity ? "true" : "false");
        }
        if (resolved.contains(QStringLiteral("continent"))
            && station.contains(QStringLiteral("continent"))) {
            resolved.insert(QStringLiteral("same_continent"),
                resolved.value(QStringLiteral("continent"))
                        == station.value(QStringLiteral("continent"))
                    ? "true" : "false");
        }
        return resolved;
    };
}

void MainWindow::updateSerialFields()
{
    const ContestDefinition *definition = activeDefinition();
    if (definition == nullptr) {
        return;
    }
    if (definition->serialServerRequired) {
        for (int entry = 0; entry < 2; ++entry) {
            if (m_cancelSerialButtons[entry] != nullptr) {
                const bool cancelling = m_serialCancelContexts.contains(
                    m_serialReservationUuids[entry]);
                m_cancelSerialButtons[entry]->setEnabled(
                    !m_serialReservationUuids[entry].isEmpty() && !cancelling);
                m_cancelSerialButtons[entry]->setText(
                    cancelling ? tr("Cancelling...") : tr("Cancel serial"));
            }
            for (const ExchangeFieldDefinition &field : definition->sentExchange) {
                if (field.type == ExchangeFieldType::Serial && field.generated) {
                    if (QLineEdit *edit =
                            m_sentExchangeEdits[entry].value(field.name, nullptr)) {
                        const bool reservationMatches =
                            m_serialReservationBands[entry]
                                == m_bandSelectors[entry]->currentText().trimmed()
                                       .toLower()
                            && m_serialReservationModes[entry]
                                == m_modeSelectors[entry]->currentText().trimmed()
                                       .toUpper();
                        edit->setText(reservationMatches
                            ? m_serialReservationSerials[entry] : QString());
                    }

                }
            }
        }
        return;
    }
    const int nextSerial = ContestEngine().nextSerialNumber(*definition, m_logQsos);
    for (const ExchangeFieldDefinition &field : definition->sentExchange) {
        if (field.type != ExchangeFieldType::Serial || !field.generated) {
            continue;
        }
        QString formatted = QString::number(nextSerial);
        if (field.width > 0) {
            formatted = formatted.rightJustified(field.width, QLatin1Char('0'));
        }
        for (int entry = 0; entry < 2; ++entry) {
            if (QLineEdit *edit = m_sentExchangeEdits[entry].value(field.name, nullptr)) {
                edit->setText(formatted);
            }
        }
    }
}

void MainWindow::cancelSerialForVfo(int vfo)
{
    vfo = qBound(0, vfo, 1);
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; serial reservations cannot be changed."),
            true);
        return;
    }
    const Contest *contest = activeContest();
    const QString reservationUuid = m_serialReservationUuids[vfo];
    if (contest == nullptr || reservationUuid.isEmpty()) {
        showOperatorMessage(tr("There is no active serial reservation to cancel."),
                            true);
        return;
    }
    if (!m_hostServer && (!m_syncClient || m_syncClient->state() != SyncClient::State::Ready
        || QSettings().value(QStringLiteral("network/client/logUuid")).toString()
            != contest->logUuid)) {
        showOperatorMessage(
            tr("Connect to the central server for this log before cancelling the "
               "reservation."),
            true);
        return;
    }
    if (m_serialCancelContexts.contains(reservationUuid)) {
        showOperatorMessage(tr("Cancellation of this serial is already pending."));
        return;
    }

    const auto result = QMessageBox::question(
        this, tr("Cancel serial reservation"),
        tr("Cancel serial %1? It will be permanently consumed and will not be "
           "reused, even if no QSO is logged.")
            .arg(m_serialReservationSerials[vfo]),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    if (m_hostServer) {
        QString error;
        const QString stationUuid = m_repository.localStationUuid(&error);
        if (!error.isEmpty() || !m_hostRepository->cancelSerialReservation(
                contest->logUuid, stationUuid, reservationUuid, false, &error)) {
            showOperatorMessage(error, true);
            return;
        }
        m_serialReservationUuids[vfo].clear();
        m_serialReservationBands[vfo].clear();
        m_serialReservationModes[vfo].clear();
        m_serialReservationSerials[vfo].clear();
        updateSerialFields();
        return;
    }
    const QString requestId = m_syncClient->cancelSerial(reservationUuid);
    if (requestId.isEmpty()) {
        showOperatorMessage(
            tr("The cancellation request could not be queued. The reservation "
               "remains active."),
            true);
        return;
    }
    m_serialCancelContexts.insert(reservationUuid, vfo);
    m_serialCancelRequestReservationUuids.insert(requestId, reservationUuid);
    updateSerialFields();
    showOperatorMessage(tr("Cancelling serial %1 on the central server...")
                            .arg(m_serialReservationSerials[vfo]));
}

void MainWindow::reserveSerialForVfo(int vfo)
{
    vfo = qBound(0, vfo, 1);
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; serial reservations cannot be changed."),
            true);
        return;
    }
    const Contest *contest = activeContest();
    const ContestDefinition *definition = activeDefinition();
    if (contest == nullptr || definition == nullptr
        || !definition->serialServerRequired) {
        showOperatorMessage(tr("The selected contest does not require a server serial."),
                            true);
        return;
    }
    if (!m_hostServer && (!m_syncClient
        || QSettings().value(QStringLiteral("network/client/logUuid")).toString()
            != contest->logUuid)) {
        showOperatorMessage(
            tr("Connect this log to its central server before requesting a serial."),
            true);
        return;
    }
    const QString band = m_bandSelectors[vfo]->currentText().trimmed().toLower();
    const QString mode = m_modeSelectors[vfo]->currentText().trimmed().toUpper();
    if (!m_serialReservationUuids[vfo].isEmpty()) {
        if (m_serialReservationBands[vfo] == band
            && m_serialReservationModes[vfo] == mode) {
            showOperatorMessage(
                tr("Serial %1 is already reserved for this band and mode.")
                    .arg(m_serialReservationSerials[vfo]));
        } else {
            showOperatorMessage(
                tr("Use or return to the existing reservation before requesting "
                   "another serial for this entry."),
                true);
        }
        return;
    }

    QString error;
    const QVector<SerialReservation> reservations =
        m_repository.serialReservations(contest->logUuid, &error);
    if (!error.isEmpty()) {
        showOperatorMessage(tr("Unable to load serial reservations: %1").arg(error),
                            true);
        return;
    }
    const QString stationUuid = m_repository.localStationUuid(&error);
    if (!error.isEmpty() || stationUuid.isEmpty()) {
        showOperatorMessage(
            error.isEmpty() ? tr("The local station identity is unavailable.")
                            : tr("Unable to identify station: %1").arg(error),
            true);
        return;
    }
    const QVector<SerialReservationRequest> pendingRequests =
        m_repository.pendingSerialReservationRequests(
            contest->logUuid, stationUuid, &error);
    if (!error.isEmpty()) {
        showOperatorMessage(
            tr("Unable to inspect pending serial requests: %1").arg(error), true);
        return;
    }
    for (const SerialReservationRequest &pending : pendingRequests) {
        if (pending.operation == QStringLiteral("reserve_serial")
            && pending.band == band && pending.mode == mode) {
            showOperatorMessage(
                tr("A serial request for %1/%2 is already queued. No number is "
                   "allocated until the server confirms it.")
                    .arg(band, mode),
                !m_hostServer && m_syncClient->state() != SyncClient::State::Ready);
            return;
        }
    }
    for (const SerialReservation &reservation : reservations) {
        if (reservation.reservationUuid == m_serialReservationUuids[1 - vfo]) {
            continue;
        }
        if (reservation.stationUuid == stationUuid
            && reservation.status == QStringLiteral("reserved")
            && reservation.band == band && reservation.mode == mode) {
            m_serialReservationUuids[vfo] = reservation.reservationUuid;
            m_serialReservationBands[vfo] = band;
            m_serialReservationModes[vfo] = mode;
            m_serialReservationSerials[vfo] =
                QString::number(reservation.serialNumber)
                    .rightJustified([definition] {
                        for (const ExchangeFieldDefinition &field :
                             definition->sentExchange) {
                            if (field.type == ExchangeFieldType::Serial
                                && field.generated) {
                                return field.width;
                            }
                        }
                        return 0;
                    }(), QLatin1Char('0'));
            updateSerialFields();
            showOperatorMessage(
                tr("Restored reserved serial %1 from the local cache.")
                    .arg(m_serialReservationSerials[vfo]));
            return;
        }
    }
    for (auto it = m_serialReservationContexts.cbegin();
         it != m_serialReservationContexts.cend(); ++it) {
        if (it.value() == vfo) {
            showOperatorMessage(tr("A serial reservation request is already pending."));
            return;
        }
    }

    const QString reservationUuid = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    if (m_hostServer) {
        const QJsonObject reservation = m_hostRepository->reserveSerial(
            contest->logUuid, stationUuid, reservationUuid, band, mode, &error);
        if (!error.isEmpty() || reservation.isEmpty()) {
            showOperatorMessage(tr("Unable to reserve serial: %1").arg(error), true);
            return;
        }
        m_serialReservationUuids[vfo] = reservationUuid;
        m_serialReservationBands[vfo] = band;
        m_serialReservationModes[vfo] = mode;
        m_serialReservationSerials[vfo] =
            reservation.value(QStringLiteral("formatted_serial")).toString();
        updateSerialFields();
        return;
    }
    m_serialReservationContexts.insert(reservationUuid, vfo);
    m_serialReservationPendingBands.insert(reservationUuid, band);
    m_serialReservationPendingModes.insert(reservationUuid, mode);
    const QString requestId = m_syncClient->reserveSerial(reservationUuid, band, mode);
    bool queuedOffline = false;
    if (requestId.isEmpty()
        && m_syncClient->state() != SyncClient::State::Ready) {
        const QVector<SerialReservationRequest> queued =
            m_repository.pendingSerialReservationRequests(
                contest->logUuid, stationUuid, &error);
        if (!error.isEmpty()) {
            showOperatorMessage(
                tr("Unable to verify queued serial request: %1").arg(error), true);
        } else {
            queuedOffline = std::any_of(
                queued.cbegin(), queued.cend(),
                [&reservationUuid](const SerialReservationRequest &request) {
                    return request.reservationUuid == reservationUuid
                        && request.operation == QStringLiteral("reserve_serial");
                });
        }
    }
    if (requestId.isEmpty() && !queuedOffline) {
        m_serialReservationContexts.remove(reservationUuid);
        m_serialReservationPendingBands.remove(reservationUuid);
        m_serialReservationPendingModes.remove(reservationUuid);
        if (error.isEmpty()) {
            showOperatorMessage(
                tr("Could not queue the serial reservation request. Check the network "
                   "connection and synchronization state."),
                true);
        }
        return;
    }
    if (!requestId.isEmpty()) {
        m_serialRequestReservationUuids.insert(requestId, reservationUuid);
    }
    showOperatorMessage(
        m_syncClient->state() == SyncClient::State::Ready
            ? tr("Requesting the next serial from the central server...")
            : tr("Serial request queued offline. No number is allocated and QSO "
                 "logging remains blocked until the server confirms a reservation."));
}

void MainWindow::autoConnectRadios()
{
    QSettings settings;
    for (int radio = 0; radio < 2; ++radio) {
        if (settings.value(QStringLiteral("radio/%1/autoConnect")
                               .arg(radio == 0 ? QLatin1Char('A') : QLatin1Char('B')),
                           false)
                .toBool()) {
            m_radioControllers[radio].connectRadio(radioConfigFromSettings(radio));
        }
    }
}

void MainWindow::showOperatorMessage(const QString &message, bool alert)
{
    m_messageLine->setText(QStringLiteral("* %1 *").arg(message));
    m_messageLine->setProperty("alert", alert);
    repolish(m_messageLine);
    m_messageTimer->start(10'000);
}

void MainWindow::wipeEntry(int context)
{
    context = qBound(0, context, 1);
    m_callsignEdits[context]->clear();
    for (QLineEdit *edit : std::as_const(m_exchangeEdits[context])) {
        edit->clear();
    }
    m_messageLine->clear();
    m_callsignEdits[context]->setFocus();
}

bool MainWindow::tuneFrequencyFromCallsign(int context)
{
    context = qBound(0, context, 1);
    const std::optional<qint64> frequencyHz =
        frequencyHzFromCallsignInput(m_callsignEdits[context]->text());
    if (!frequencyHz.has_value()) {
        return false;
    }

    const int radio = m_so2rToggle->isChecked()
        ? context : m_activeRadioSelector->currentData().toInt();
    const int radioVfo = m_so2rToggle->isChecked()
        ? m_radioSnapshots[radio].focusVfo : context;
    m_radioControllers[radio].setFrequency(*frequencyHz, radioVfo);
    m_callsignEdits[context]->clear();
    return true;
}

void MainWindow::rebuildCallCompleters()
{
    QSet<QString> unique(m_checkCallsigns.cbegin(), m_checkCallsigns.cend());
    for (const Qso &qso : std::as_const(m_logQsos)) {
        const QString callsign = qso.callsign.trimmed().toUpper();
        if (!callsign.isEmpty()) {
            unique.insert(callsign);
        }
    }
    unique.remove(QString());
    QStringList candidates(unique.cbegin(), unique.cend());
    std::sort(candidates.begin(), candidates.end());
    for (QStringListModel *model : m_callCompleterModels) {
        if (model != nullptr) {
            model->setStringList(candidates);
        }
    }
}

void MainWindow::refreshCheckWindows()
{
    if (m_checkPartial == nullptr) {
        return;
    }
    rebuildCallCompleters();
    const int context = currentEntryContext();
    const QString callsign = m_callsignEdits[context]->text().trimmed().toUpper();
    const QString band = m_bandSelectors[context]->currentText().trimmed().toLower();
    const QString mode = m_modeSelectors[context]->currentText().trimmed().toUpper();
    const ContestDefinition *definition = activeDefinition();
    const QStringList dupeKey = definition != nullptr ? definition->dupeKey : QStringList{};

    // Check partial: master.cpy / call history / log callsigns containing the input.
    QSet<QString> loggedCalls;
    QSet<QString> dupeCalls;
    for (const Qso &qso : std::as_const(m_logQsos)) {
        if (qso.invalid) {
            continue;
        }
        const QString call = qso.callsign.toUpper();
        loggedCalls.insert(call);
        if (sameDupeKey(dupeKey, qso, call, band, mode)) {
            dupeCalls.insert(call);
        }
    }
    m_checkPartial->setUpdatesEnabled(false);
    m_checkPartial->clear();
    int matches = 0;
    if (callsign.size() >= 2) {
        QStringList candidates;
        for (const QString &call : std::as_const(m_checkCallsigns)) {
            if (call.contains(callsign)) {
                candidates.append(call);
            }
        }
        for (const QString &call : std::as_const(loggedCalls)) {
            if (call.contains(callsign) && !candidates.contains(call)) {
                candidates.append(call);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [&callsign](const QString &left, const QString &right) {
                      const bool leftPrefix = left.startsWith(callsign);
                      const bool rightPrefix = right.startsWith(callsign);
                      if (leftPrefix != rightPrefix) {
                          return leftPrefix;
                      }
                      return left < right;
                  });
        matches = static_cast<int>(candidates.size());
        const QFont normal = monoFont(11, false);
        const QFont bold = monoFont(11, true);
        for (const QString &call : std::as_const(candidates)) {
            if (m_checkPartial->count() >= 300) {
                break;
            }
            auto *item = new QListWidgetItem(call, m_checkPartial);
            item->setFont(call == callsign ? bold : normal);
            if (dupeCalls.contains(call)) {
                item->setForeground(QColor(QStringLiteral("#FF4040")));
            } else if (loggedCalls.contains(call)) {
                item->setForeground(QColor(QStringLiteral("#7CFC00")));
            } else {
                item->setForeground(QColor(Qt::white));
            }
        }
    }
    m_checkPartial->setUpdatesEnabled(true);
    m_checkPartialDock->setWindowTitle(
        matches > 0 ? tr("Check partial — %1").arg(matches) : tr("Check partial"));

    // Check callsign: band x mode matrix for the exact callsign.
    QStringList bands = definition != nullptr && !definition->bands.isEmpty()
        ? definition->bands
        : QStringList{QStringLiteral("160m"), QStringLiteral("80m"), QStringLiteral("40m"),
                      QStringLiteral("20m"), QStringLiteral("15m"), QStringLiteral("10m")};
    QStringList modes = definition != nullptr && !definition->modes.isEmpty()
        ? definition->modes : QStringList{QStringLiteral("CW"), QStringLiteral("SSB")};
    for (QString &value : bands) {
        value = value.toLower();
    }
    for (QString &value : modes) {
        value = value.toUpper();
    }
    QHash<QString, QString> worked;
    const ContestEngine multiplierEngine;
    const auto multiplierResolver = this->multiplierResolver();
    int workedCount = 0;
    for (const Qso &qso : std::as_const(m_logQsos)) {
        if (!qso.invalid && !callsign.isEmpty()
            && qso.callsign.compare(callsign, Qt::CaseInsensitive) == 0) {
            worked.insert(qso.band.toLower() + QLatin1Char('|') + qso.mode.toUpper(),
                          qso.timestampUtc.toUTC().toString(QStringLiteral("HHmm")));
            ++workedCount;
        }
    }
    m_checkCallGrid->clear();
    m_checkCallGrid->setColumnCount(static_cast<int>(modes.size()) + 1);
    QStringList headers{tr("Band")};
    headers.append(modes);
    m_checkCallGrid->setHorizontalHeaderLabels(headers);
    m_checkCallGrid->setRowCount(static_cast<int>(bands.size()));
    for (int row = 0; row < bands.size(); ++row) {
        const bool currentBand = bands.at(row) == band;
        auto *bandItem = new QTableWidgetItem(bands.at(row));
        if (currentBand) {
            bandItem->setBackground(QColor(QStringLiteral("#4682B4")));
            bandItem->setForeground(Qt::white);
        }
        m_checkCallGrid->setItem(row, 0, bandItem);
        for (int column = 0; column < modes.size(); ++column) {
            const QString cellBand = bands.at(row);
            const QString cellMode = modes.at(column);
            const QString time =
                worked.value(cellBand + QLatin1Char('|') + cellMode);
            Qso candidate;
            candidate.callsign = callsign;
            candidate.band = cellBand;
            candidate.mode = cellMode;
            const QStringList multiplierKeys = definition == nullptr
                ? QStringList{}
                : multiplierEngine.multiplierKeys(
                      *definition, candidate, multiplierResolver);
            const bool hasMultiplier = !multiplierKeys.isEmpty();
            const bool newMultiplier = std::any_of(
                multiplierKeys.cbegin(), multiplierKeys.cend(),
                [this](const QString &key) {
                    return !key.isEmpty()
                        && !m_workedMultiplierKeys.contains(key);
                });
            QString cellText = time;
            if (cellText.isEmpty()) {
                cellText = newMultiplier ? QStringLiteral("+M")
                    : hasMultiplier ? QStringLiteral("M")
                                    : QStringLiteral("·");
            } else if (hasMultiplier) {
                cellText += newMultiplier ? QStringLiteral(" +M")
                                          : QStringLiteral(" M");
            }
            auto *item = new QTableWidgetItem(cellText);
            item->setTextAlignment(Qt::AlignCenter);
            const bool currentCell = currentBand && modes.at(column) == mode;
            if (newMultiplier) {
                item->setToolTip(tr("New multiplier available for this band and mode."));
                item->setForeground(QColor(QStringLiteral("#006400")));
                item->setBackground(QColor(QStringLiteral("#DDF2DD")));
            } else if (hasMultiplier) {
                item->setToolTip(tr("Multiplier already worked."));
            } else {
                item->setToolTip(tr("No multiplier can be resolved for this callsign."));
            }
            if (!time.isEmpty() && currentCell) {
                item->setBackground(QColor(QStringLiteral("#CE0000")));
                item->setForeground(Qt::white);
            } else if (!time.isEmpty()) {
                item->setForeground(QColor(QStringLiteral("#000080")));
            } else if (currentBand && !newMultiplier) {
                item->setBackground(QColor(QStringLiteral("#4682B4")));
                item->setForeground(Qt::white);
            }
            m_checkCallGrid->setItem(row, column + 1, item);
        }
    }
    if (callsign.isEmpty()) {
        m_checkCallTitle->setText(tr("Type a callsign to see where it is worked."));
    } else {
        const QString info = m_callInfoLabels[context]->text();
        QString dxcc;
        if (const CtyPrefixEntry *entry =
                SupportDataParser::findCtyEntry(callsign, m_ctyPrefixes)) {
            dxcc = tr("DXCC: %1 (%2), %3, CQ %4 / ITU %5")
                       .arg(entry->entity, entry->prefix, entry->continent)
                       .arg(entry->cqZone)
                       .arg(entry->ituZone);
            double myLat = 0.0;
            double myLon = 0.0;
            if (entry->hasLocation
                && SupportDataParser::maidenheadToLatLon(m_station.grid, myLat, myLon)) {
                const GeoBearing bearing = SupportDataParser::bearingBetween(
                    myLat, myLon, entry->latitude, entry->longitude);
                dxcc += tr("\nAzimuth: %1° · %2 km")
                            .arg(qRound(bearing.azimuthDegrees))
                            .arg(qRound(bearing.distanceKm));
            } else if (entry->hasLocation) {
                dxcc += tr("\nAzimuth: set a valid station grid locator");
            }
        }
        m_checkCallTitle->setText(
            QStringLiteral("%1 — %2%3%4")
                .arg(callsign,
                     workedCount == 0 ? tr("not in log") : tr("%n QSO(s)", "", workedCount),
                     info.isEmpty() ? QString() : QStringLiteral(" — ") + info,
                     dxcc.isEmpty() ? QString() : QLatin1Char('\n') + dxcc));
    }
}

void MainWindow::logQso()
{
    const int radio = m_activeRadioSelector->currentData().toInt();
    logQsoForVfo(m_so2rToggle->isChecked()
                     ? radio : m_radioSnapshots[radio].focusVfo);
}

void MainWindow::logQsoForVfo(int vfo)
{
    vfo = qBound(0, vfo, 1);
    if (tuneFrequencyFromCallsign(vfo)) {
        return;
    }
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; QSO changes are disabled."),
            true);
        return;
    }
    const int radio = m_so2rToggle->isChecked()
        ? vfo : m_activeRadioSelector->currentData().toInt();
    const int radioVfo = m_so2rToggle->isChecked()
        ? m_radioSnapshots[radio].focusVfo : vfo;
    QLineEdit *callsignEdit = m_callsignEdits[vfo];
    QComboBox *bandSelector = m_bandSelectors[vfo];
    QComboBox *modeSelector = m_modeSelectors[vfo];
    QMap<QString, QLineEdit *> &exchangeEdits = m_exchangeEdits[vfo];
    if (m_contestSelector->currentIndex() < 0) {
        statusBar()->showMessage(tr("Select a log before entering a QSO."), 5000);
        return;
    }
    if (m_station.callsign.trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Station callsign required"),
                             tr("Configure the station callsign before logging."));
        configureOperator();
        return;
    }
    const QString callsign = callsignEdit->text().trimmed().toUpper();
    if (callsign.isEmpty()) {
        callsignEdit->setFocus();
        return;
    }

    const qint64 contestId =
        m_contestSelector->currentData().toLongLong();
    QString error;
    const QVector<Qso> previousQsos =
        m_repository.qsosForContest(contestId, 0, &error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to check log"), error);
        return;
    }
    QMap<QString, QString> receivedFields;
    for (auto it = exchangeEdits.cbegin(); it != exchangeEdits.cend(); ++it) {
        receivedFields.insert(it.key(), it.value()->text().trimmed());
    }
    QMap<QString, QString> sentOverrides;
    for (auto it = m_sentExchangeEdits[vfo].cbegin();
         it != m_sentExchangeEdits[vfo].cend(); ++it) {
        sentOverrides.insert(it.key(), it.value()->text().trimmed());
    }
    if (const auto rstDefinition = m_definitions.constFind(contestId);
        rstDefinition != m_definitions.cend()) {
        const QString mode = modeSelector->currentText().trimmed().toUpper();
        const bool voice = mode == QLatin1String("SSB") || mode == QLatin1String("USB")
            || mode == QLatin1String("LSB") || mode == QLatin1String("FM")
            || mode == QLatin1String("AM");
        QString sentRst;
        for (const ExchangeFieldDefinition &field : rstDefinition->sentExchange) {
            if (field.type == ExchangeFieldType::Rst) {
                sentRst = sentOverrides.value(field.name).trimmed();
                break;
            }
        }
        if (sentRst.isEmpty()) {
            sentRst = voice ? QStringLiteral("59") : QStringLiteral("599");
        }
        for (const ExchangeFieldDefinition &field : rstDefinition->receivedExchange) {
            if (field.type == ExchangeFieldType::Rst) {
                receivedFields.insert(field.name, sentRst);
            }
        }
    }
    QMap<QString, QString> sentFields = sentOverrides;
    int points = 0;
    QStringList newMultipliers;
    bool isDupe = false;
    const bool allowDupeLogging = LogSettings(m_repository).value(
        QStringLiteral("operating/allowDupeLogging"), false).toBool();
    const auto definition = m_definitions.constFind(contestId);
    QString reservationUuid;
    std::optional<int> requestedSerial;
    if (definition != m_definitions.cend()) {
        if (definition->serialServerRequired) {
            const Contest *contest = activeContest();
            const QString band = bandSelector->currentText().trimmed().toLower();
            const QString mode = modeSelector->currentText().trimmed().toUpper();
            if (contest == nullptr || (!m_hostServer && (!m_syncClient
                || m_syncClient->state() != SyncClient::State::Ready
                || QSettings().value(QStringLiteral("network/client/logUuid"))
                       .toString() != contest->logUuid))) {
                showOperatorMessage(
                    tr("A ready central connection for this log is required to use "
                       "server serials."),
                    true);
                return;
            }
            reservationUuid = m_serialReservationUuids[vfo];
            if (reservationUuid.isEmpty()
                || m_serialReservationBands[vfo] != band
                || m_serialReservationModes[vfo] != mode) {
                showOperatorMessage(
                    tr("Reserve a serial for the selected band and mode before "
                       "logging this QSO."),
                    true);
                return;
            }
            if (m_serialCancelContexts.contains(reservationUuid)) {
                showOperatorMessage(
                    tr("The serial reservation is being cancelled and cannot be "
                       "used for a QSO."),
                    true);
                return;
            }
            const std::optional<SerialReservation> reservation =
                m_repository.serialReservation(contest->logUuid, reservationUuid,
                                               &error);
            if (!error.isEmpty()) {
                showOperatorMessage(
                    tr("Unable to verify serial reservation: %1").arg(error), true);
                return;
            }
            QString serialFieldName;
            int serialFieldWidth = 0;
            for (const ExchangeFieldDefinition &field : definition->sentExchange) {
                if (field.type == ExchangeFieldType::Serial && field.generated) {
                    serialFieldName = field.name;
                    serialFieldWidth = field.width;
                    break;
                }
            }
            if (!reservation.has_value()
                || reservation->status != QStringLiteral("reserved")
                || reservation->stationUuid
                    != m_repository.localStationUuid(&error)
                || reservation->band != band || reservation->mode != mode
                || serialFieldName.isEmpty()) {
                showOperatorMessage(
                    error.isEmpty()
                        ? tr("The serial reservation is missing, expired, or does not "
                             "match this station, band, and mode.")
                        : tr("Unable to verify station identity: %1").arg(error),
                    true);
                return;
            }
            const QString formattedSerial =
                QString::number(reservation->serialNumber)
                    .rightJustified(serialFieldWidth, QLatin1Char('0'));
            if (m_serialReservationSerials[vfo] != formattedSerial
                || sentOverrides.value(serialFieldName) != formattedSerial) {
                showOperatorMessage(
                    tr("The entered serial does not match the reserved serial."),
                    true);
                return;
            }
            requestedSerial = reservation->serialNumber;
        }
        const ContestEngine engine;
        const auto resolver = multiplierResolver();
        ContestEvaluationContext context =
            engine.buildContext(*definition, previousQsos, resolver);
        Qso candidate;
        candidate.callsign = callsign;
        candidate.stationCallsign = m_station.callsign;
        if (resolver) {
            context.resolvedMultiplierValues = resolver(candidate);
        }
        const ContestEvaluationResult evaluation =
            engine.evaluate(*definition, callsign,
                            bandSelector->currentText().trimmed().toLower(),
                            modeSelector->currentText().trimmed().toUpper(),
                            receivedFields, context, requestedSerial,
                            sentOverrides, allowDupeLogging);
        isDupe = evaluation.duplicate;
        if (evaluation.duplicate && !allowDupeLogging) {
            showOperatorMessage(tr("%1 is a DUPE on %2 %3 — not logged")
                                    .arg(callsign, bandSelector->currentText(),
                                         modeSelector->currentText()),
                                true);
            callsignEdit->setFocus();
            callsignEdit->selectAll();
            return;
        }
        if (!evaluation.valid) {
            const QStringList issues = [&evaluation] {
                QStringList result;
                for (const ContestValidationIssue &issue : evaluation.issues) {
                    result.append(QStringLiteral("%1: %2").arg(issue.field, issue.message));
                }
                return result;
            }();
            showOperatorMessage(tr("Not logged — %1").arg(issues.join(QStringLiteral("; "))),
                                true);
            for (const ContestValidationIssue &issue : evaluation.issues) {
                QString field = issue.field;
                QLineEdit *problem = nullptr;
                if (field == QLatin1String("callsign")) {
                    problem = callsignEdit;
                } else if (field.startsWith(QLatin1String("received."))) {
                    problem = exchangeEdits.value(field.mid(9), nullptr);
                } else if (field.startsWith(QLatin1String("sent."))) {
                    problem = m_sentExchangeEdits[vfo].value(field.mid(5), nullptr);
                } else {
                    problem = exchangeEdits.value(field, nullptr);
                }
                if (problem != nullptr) {
                    problem->setFocus();
                    problem->selectAll();
                    break;
                }
            }
            return;
        }
        sentFields = evaluation.sentExchange;
        receivedFields = evaluation.receivedExchange;
        points = evaluation.duplicate ? 0 : evaluation.points;
        if (!evaluation.duplicate) {
            for (const QString &key : evaluation.newMultiplierKeys) {
                QString label = key.section(QLatin1Char(':'), -1);
                newMultipliers.append(label.section(QLatin1Char('@'), 0, 0));
            }
        }
    } else {
        if (!ContestEngine::isValidCallsign(callsign)) {
            showOperatorMessage(tr("Not logged — %1 is not a valid callsign").arg(callsign),
                                true);
            callsignEdit->setFocus();
            callsignEdit->selectAll();
            return;
        }
        for (const Qso &previous : previousQsos) {
            if (previous.invalid) {
                continue;
            }
            if (previous.callsign.compare(callsign, Qt::CaseInsensitive) == 0
                && previous.band == bandSelector->currentText().trimmed().toLower()
                && previous.mode == modeSelector->currentText().trimmed().toUpper()) {
                isDupe = true;
                if (!allowDupeLogging) {
                    showOperatorMessage(tr("%1 is a DUPE on %2 %3 — not logged")
                                            .arg(callsign, bandSelector->currentText(),
                                                 modeSelector->currentText()),
                                        true);
                    callsignEdit->setFocus();
                    callsignEdit->selectAll();
                    return;
                }
            }
        }
    }

    Qso qso;
    qso.contestId = contestId;
    qso.callsign = callsign;
    qso.stationCallsign = m_station.callsign.trimmed().toUpper();
    qso.band = bandSelector->currentText().trimmed().toLower();
    qso.timestampUtc = QDateTime::currentDateTimeUtc();
    const RadioSnapshot &snapshot = m_radioSnapshots[radio];
    qso.frequencyHz = snapshot.frequencyHz;
    for (const Vfo &radioVfoState : snapshot.vfos) {
        if (radioVfoState.number == radioVfo && radioVfoState.frequencyHz > 0) {
            qso.frequencyHz = radioVfoState.frequencyHz;
            break;
        }
    }
    qso.mode = modeSelector->currentText().trimmed().toUpper();
    qso.sentExchangeFields = sentFields;
    qso.receivedExchangeFields = receivedFields;
    qso.sentExchange = exchangeText(sentFields);
    qso.receivedExchange = exchangeText(receivedFields);
    qso.points = points;
    qso.reservationUuid = reservationUuid;
    if (!m_repository.saveQso(qso, &error)) {
        QMessageBox::critical(this, tr("Unable to save QSO"), error);
        return;
    }
    if (m_n1mmBroadcaster.configuration().enabled) {
        const Contest *contest = activeContest();
        if (contest == nullptr
            || !m_n1mmBroadcaster.broadcastQso(
                qso, contest != nullptr ? contest->name : QString(),
                operatorIdentity(), &error)) {
            statusBar()->showMessage(
                tr("QSO was saved, but N1MM UDP broadcast failed: %1")
                    .arg(contest == nullptr
                             ? tr("the active log is unavailable.") : error),
                10000);
        }
    }
    if (!reservationUuid.isEmpty()) {
        m_serialReservationUuids[vfo].clear();
        m_serialReservationBands[vfo].clear();
        m_serialReservationModes[vfo].clear();
        m_serialReservationSerials[vfo].clear();
    }
    callsignEdit->clear();
    for (QLineEdit *edit : exchangeEdits) {
        edit->clear();
    }
    refreshRecentQsos();
    updateSerialFields();
    refreshBandmap();
    updateDupeIndicator();
    callsignEdit->setFocus();
    if (isDupe) {
        showOperatorMessage(tr("%1 — DUPE logged with 0 points").arg(callsign), true);
    } else if (!newMultipliers.isEmpty()) {
        showOperatorMessage(tr("%1 — NEW MULTIPLIER %2")
                                .arg(callsign, newMultipliers.join(QStringLiteral(", "))),
                            true);
    } else {
        showOperatorMessage(tr("QSO #%1 %2 logged — %3 pts")
                                .arg(m_logQsos.size())
                                .arg(callsign)
                                .arg(points));
    }
    statusBar()->showMessage(tr("QSO with %1 logged.").arg(callsign), 3000);
}

void MainWindow::editSelectedQso(int row, int)
{
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; QSO changes are disabled."),
            true);
        return;
    }
    const QTableWidgetItem *idItem = m_recentQsos->item(row, 0);
    if (idItem == nullptr) {
        return;
    }
    QString error;
    const std::optional<Qso> stored =
        m_repository.qsoById(idItem->data(Qt::UserRole).toLongLong(), &error);
    if (!error.isEmpty() || !stored.has_value()) {
        QMessageBox::critical(this, tr("Unable to load QSO"),
                              error.isEmpty() ? tr("The QSO no longer exists.") : error);
        return;
    }
    const Qso original = *stored;
    const auto definition = m_definitions.constFind(original.contestId);
    const bool hasDefinition = definition != m_definitions.cend();

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("editQsoDialog"));
    dialog.setWindowTitle(tr("Edit QSO #%1 with %2").arg(original.id).arg(original.callsign));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *callEdit = new QLineEdit(original.callsign, &dialog);
    callEdit->setObjectName(QStringLiteral("editCallsign"));
    installUppercase(callEdit);
    auto *timeEdit = new QDateTimeEdit(original.timestampUtc.toUTC(), &dialog);
    timeEdit->setObjectName(QStringLiteral("editTime"));
    timeEdit->setTimeZone(QTimeZone::UTC);
    timeEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
    auto *bandEdit = new QComboBox(&dialog);
    bandEdit->setObjectName(QStringLiteral("editBand"));
    bandEdit->setEditable(!hasDefinition);
    bandEdit->addItems(hasDefinition && !definition->bands.isEmpty() ? definition->bands
                                                                     : defaultBands());
    if (bandEdit->findText(original.band) < 0) {
        bandEdit->addItem(original.band);
    }
    bandEdit->setCurrentText(original.band);
    auto *modeEdit = new QComboBox(&dialog);
    modeEdit->setObjectName(QStringLiteral("editMode"));
    modeEdit->setEditable(!hasDefinition);
    modeEdit->addItems(hasDefinition && !definition->modes.isEmpty()
                           ? definition->modes
                           : QStringList{QStringLiteral("CW"), QStringLiteral("SSB"),
                                         QStringLiteral("FM"), QStringLiteral("AM"),
                                         QStringLiteral("RTTY"), QStringLiteral("DATA")});
    if (modeEdit->findText(original.mode) < 0) {
        modeEdit->addItem(original.mode);
    }
    modeEdit->setCurrentText(original.mode);
    auto *frequencyEdit = new QLineEdit(QString::number(original.frequencyHz), &dialog);
    frequencyEdit->setObjectName(QStringLiteral("editFrequency"));
    form->addRow(tr("Callsign:"), callEdit);
    form->addRow(tr("Time:"), timeEdit);
    form->addRow(tr("Band:"), bandEdit);
    form->addRow(tr("Mode:"), modeEdit);
    form->addRow(tr("Frequency (Hz):"), frequencyEdit);

    QMap<QString, QLineEdit *> sentEdits;
    QMap<QString, QLineEdit *> receivedEdits;
    const auto addExchangeEdit = [&dialog, form](const QString &prefix, const QString &label,
                                                 const QString &name, const QString &value) {
        auto *edit = new QLineEdit(value, &dialog);
        edit->setObjectName(prefix + name);
        installUppercase(edit);
        form->addRow(label, edit);
        return edit;
    };
    if (hasDefinition) {
        for (const ExchangeFieldDefinition &field : definition->sentExchange) {
            sentEdits.insert(field.name,
                             addExchangeEdit(QStringLiteral("editSent_"),
                                             tr("Sent %1:").arg(field.label), field.name,
                                             original.sentExchangeFields.value(field.name)));
        }
        for (const ExchangeFieldDefinition &field : definition->receivedExchange) {
            receivedEdits.insert(
                field.name,
                addExchangeEdit(QStringLiteral("editRcvd_"), field.label + QLatin1Char(':'),
                                field.name, original.receivedExchangeFields.value(field.name)));
        }
    } else {
        sentEdits.insert(QString(), addExchangeEdit(QStringLiteral("editSent_"), tr("Sent:"),
                                                    QStringLiteral("text"),
                                                    original.sentExchange));
        receivedEdits.insert(QString(),
                             addExchangeEdit(QStringLiteral("editRcvd_"), tr("Received:"),
                                             QStringLiteral("text"),
                                             original.receivedExchange));
    }
    layout->addLayout(form);
    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setObjectName(QStringLiteral("editErrors"));
    errorLabel->setWordWrap(true);
    errorLabel->setStyleSheet(QStringLiteral("color: #c62828; font-weight: bold;"));
    errorLabel->hide();
    layout->addWidget(errorLabel);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    Qso edited = original;
    const auto validate = [&]() -> QStringList {
        QStringList problems;
        const bool allowDupeLogging = LogSettings(m_repository).value(
            QStringLiteral("operating/allowDupeLogging"), false).toBool();
        Qso candidate = original;
        candidate.callsign = callEdit->text().trimmed().toUpper();
        candidate.band = bandEdit->currentText().trimmed().toLower();
        candidate.mode = modeEdit->currentText().trimmed().toUpper();
        candidate.timestampUtc = timeEdit->dateTime().toUTC();
        bool frequencyOk = false;
        candidate.frequencyHz = frequencyEdit->text().trimmed().toLongLong(&frequencyOk);
        if (!frequencyOk || candidate.frequencyHz < 0) {
            problems.append(tr("Frequency: enter a non-negative number of Hz."));
        }
        if (!candidate.timestampUtc.isValid()) {
            problems.append(tr("Time: invalid date/time."));
        }
        if (!ContestEngine::isValidCallsign(candidate.callsign)) {
            problems.append(tr("Callsign: \"%1\" is not a valid callsign.")
                                .arg(candidate.callsign));
        }
        if (candidate.band.isEmpty() || candidate.mode.isEmpty()) {
            problems.append(tr("Band and mode are required."));
        }
        if (!problems.isEmpty()) {
            return problems;
        }

        QString loadError;
        QVector<Qso> others = m_repository.qsosForContest(candidate.contestId, 0, &loadError);
        if (!loadError.isEmpty()) {
            return {tr("Unable to load the log: %1").arg(loadError)};
        }
        others.erase(std::remove_if(others.begin(), others.end(),
                                    [&candidate](const Qso &other) {
                                        return other.id == candidate.id;
                                    }),
                     others.end());

        if (hasDefinition) {
            QMap<QString, QString> sentFields;
            for (auto it = sentEdits.cbegin(); it != sentEdits.cend(); ++it) {
                sentFields.insert(it.key(), it.value()->text().trimmed());
            }
            QMap<QString, QString> receivedFields;
            for (auto it = receivedEdits.cbegin(); it != receivedEdits.cend(); ++it) {
                receivedFields.insert(it.key(), it.value()->text().trimmed());
            }
            std::optional<int> serial;
            for (const ExchangeFieldDefinition &field : definition->sentExchange) {
                if (field.type == ExchangeFieldType::Serial) {
                    bool serialOk = false;
                    const int value = sentFields.value(field.name).toInt(&serialOk);
                    if (serialOk) {
                        serial = value;
                    } else {
                        problems.append(tr("Sent %1: enter a serial number.").arg(field.label));
                    }
                }
            }
            if (!problems.isEmpty()) {
                return problems;
            }
            const auto resolver = multiplierResolver();
            ContestEvaluationContext context =
                ContestEngine().buildContext(*definition, others, resolver);
            Qso locationContext = candidate;
            if (locationContext.stationCallsign.isEmpty()) {
                locationContext.stationCallsign = m_station.callsign;
            }
            if (resolver) {
                context.resolvedMultiplierValues = resolver(locationContext);
            }
            const ContestEvaluationResult evaluation =
                ContestEngine().evaluate(*definition, candidate.callsign, candidate.band,
                                         candidate.mode, receivedFields, context, serial,
                                         sentFields, allowDupeLogging);
            for (const ContestValidationIssue &issue : evaluation.issues) {
                problems.append(QStringLiteral("%1: %2").arg(issue.field, issue.message));
            }
            if (evaluation.duplicate && !allowDupeLogging) {
                problems.append(tr("%1 is already logged on %2 %3 (dupe).")
                                    .arg(candidate.callsign, candidate.band, candidate.mode));
            }
            if (!problems.isEmpty() || !evaluation.valid) {
                if (problems.isEmpty()) {
                    problems.append(tr("The QSO does not match the contest definition."));
                }
                return problems;
            }
            candidate.sentExchangeFields = evaluation.sentExchange;
            candidate.receivedExchangeFields = evaluation.receivedExchange;
            candidate.sentExchange = exchangeText(candidate.sentExchangeFields);
            candidate.receivedExchange = exchangeText(candidate.receivedExchangeFields);
            candidate.points = evaluation.duplicate ? 0 : evaluation.points;
        } else {
            bool duplicate = false;
            for (const Qso &other : std::as_const(others)) {
                if (other.callsign.compare(candidate.callsign, Qt::CaseInsensitive) == 0
                    && other.band.compare(candidate.band, Qt::CaseInsensitive) == 0
                    && other.mode.compare(candidate.mode, Qt::CaseInsensitive) == 0) {
                    duplicate = true;
                    if (!allowDupeLogging) {
                        problems.append(tr("%1 is already logged on %2 %3 (dupe).")
                                            .arg(candidate.callsign, candidate.band,
                                                 candidate.mode));
                        return problems;
                    }
                }
            }
            if (duplicate) {
                candidate.points = 0;
            }
            candidate.sentExchange = sentEdits.value(QString())->text().trimmed();
            candidate.receivedExchange = receivedEdits.value(QString())->text().trimmed();
        }
        edited = candidate;
        return {};
    };
    bool saved = false;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QStringList problems = validate();
        if (!problems.isEmpty()) {
            errorLabel->setText(problems.join(QLatin1Char('\n')));
            errorLabel->show();
            return;
        }
        if (!m_repository.updateQso(edited, &error)) {
            errorLabel->setText(tr("Unable to update QSO: %1").arg(error));
            errorLabel->show();
            return;
        }
        saved = true;
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!saved) {
        return;
    }
    refreshRecentQsos();
    updateSerialFields();
    updateDupeIndicator();
    showOperatorMessage(tr("QSO #%1 %2 updated").arg(edited.id).arg(edited.callsign));
}

void MainWindow::deleteSelectedQso(int row)
{
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; QSO changes are disabled."),
            true);
        return;
    }
    const QTableWidgetItem *idItem = m_recentQsos->item(row, 0);
    if (idItem == nullptr) {
        return;
    }
    const qint64 id = idItem->data(Qt::UserRole).toLongLong();
    const auto qso = m_repository.qsoById(id);
    if (!qso.has_value()) {
        QMessageBox::critical(this, tr("Unable to delete QSO"),
                              tr("The selected QSO no longer exists."));
        return;
    }
    if (QMessageBox::question(
            this, tr("Delete QSO"),
            tr("Delete QSO #%1 with %2?").arg(id).arg(qso->callsign),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_repository.deleteQso(id, &error)) {
        QMessageBox::critical(this, tr("Unable to delete QSO"), error);
        return;
    }
    refreshRecentQsos();
    updateSerialFields();
    updateDupeIndicator();
    showOperatorMessage(tr("QSO #%1 with %2 deleted").arg(id).arg(qso->callsign));
    statusBar()->showMessage(tr("QSO #%1 deleted.").arg(id), 3000);
}

void MainWindow::undoLastQsoChange()
{
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; QSO changes are disabled."),
            true);
        return;
    }
    if (m_contestSelector->currentIndex() < 0) {
        showOperatorMessage(tr("Select a log before undoing a QSO change."), true);
        return;
    }
    const qint64 contestId = m_contestSelector->currentData().toLongLong();
    QString error;
    if (!m_repository.undoLastQsoChange(contestId, &error)) {
        showOperatorMessage(tr("Unable to undo QSO change: %1").arg(error), true);
        return;
    }
    refreshRecentQsos();
    updateSerialFields();
    updateDupeIndicator();
    showOperatorMessage(tr("Last QSO change undone."));
}

void MainWindow::setSelectedQsoInvalid(int row, bool invalid)
{
    if (m_networkReadOnly) {
        showOperatorMessage(
            tr("This station has read-only access; QSO changes are disabled."),
            true);
        return;
    }
    const QTableWidgetItem *idItem = m_recentQsos->item(row, 0);
    if (idItem == nullptr) {
        return;
    }
    const qint64 id = idItem->data(Qt::UserRole).toLongLong();
    QString error;
    const auto stored = m_repository.qsoById(id, &error);
    if (!error.isEmpty() || !stored.has_value()) {
        QMessageBox::critical(
            this, tr("Unable to update QSO"),
            error.isEmpty() ? tr("The selected QSO no longer exists.") : error);
        return;
    }
    if (stored->invalid == invalid) {
        return;
    }
    if (invalid
        && QMessageBox::question(
               this, tr("Mark QSO invalid"),
               tr("Mark QSO #%1 with %2 invalid? It will remain in the log history "
                  "but no longer count toward score, multipliers, or exports.")
                   .arg(id).arg(stored->callsign),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes) {
        return;
    }

    Qso changed = *stored;
    changed.invalid = invalid;
    if (invalid) {
        changed.points = 0;
    } else {
        const auto definition = m_definitions.constFind(changed.contestId);
        if (definition != m_definitions.cend()) {
            QVector<Qso> others = m_repository.qsosForContest(
                changed.contestId, 0, &error);
            if (!error.isEmpty()) {
                QMessageBox::critical(this, tr("Unable to restore QSO"), error);
                return;
            }
            others.erase(std::remove_if(others.begin(), others.end(),
                                        [&changed](const Qso &other) {
                                            return other.id == changed.id;
                                        }),
                         others.end());
            const auto resolver = multiplierResolver();
            ContestEngine engine;
            ContestEvaluationContext context =
                engine.buildContext(*definition, others, resolver);
            if (resolver) {
                context.resolvedMultiplierValues = resolver(changed);
            }
            std::optional<int> serial;
            for (const ExchangeFieldDefinition &field : definition->sentExchange) {
                if (field.type != ExchangeFieldType::Serial) {
                    continue;
                }
                bool ok = false;
                const int value = changed.sentExchangeFields.value(field.name)
                                      .toInt(&ok);
                if (ok) {
                    serial = value;
                }
                break;
            }
            const bool allowDuplicate = LogSettings(m_repository).value(
                QStringLiteral("operating/allowDupeLogging"), false).toBool();
            const ContestEvaluationResult evaluation = engine.evaluate(
                *definition, changed.callsign, changed.band, changed.mode,
                changed.receivedExchangeFields, context, serial,
                changed.sentExchangeFields, allowDuplicate);
            if (!evaluation.valid) {
                const QString problem = evaluation.issues.isEmpty()
                    ? tr("it conflicts with the active contest rules")
                    : evaluation.issues.first().field + QStringLiteral(": ")
                        + evaluation.issues.first().message;
                QMessageBox::warning(
                    this, tr("Unable to restore QSO"),
                    tr("Correct the QSO before restoring it: %1").arg(problem));
                return;
            }
            changed.points = evaluation.duplicate ? 0 : evaluation.points;
        } else {
            const bool duplicate = std::any_of(
                m_logQsos.cbegin(), m_logQsos.cend(),
                [&changed](const Qso &other) {
                    return other.id != changed.id && !other.invalid
                        && other.callsign.compare(changed.callsign,
                                                  Qt::CaseInsensitive) == 0
                        && other.band.compare(changed.band, Qt::CaseInsensitive) == 0
                        && other.mode.compare(changed.mode, Qt::CaseInsensitive) == 0;
                });
            if (duplicate) {
                QMessageBox::warning(
                    this, tr("Unable to restore QSO"),
                    tr("Another valid QSO already has the same callsign, band, and mode."));
                return;
            }
        }
    }
    if (!m_repository.updateQso(changed, &error)) {
        QMessageBox::critical(this, tr("Unable to update QSO"), error);
        return;
    }
    refreshRecentQsos();
    updateSerialFields();
    updateDupeIndicator();
    showOperatorMessage(invalid
        ? tr("QSO #%1 with %2 marked invalid").arg(id).arg(changed.callsign)
        : tr("QSO #%1 with %2 restored").arg(id).arg(changed.callsign));
}

void MainWindow::updateDupeIndicator()
{
    updateDupeIndicatorForVfo(0);
    updateDupeIndicatorForVfo(1);
}

void MainWindow::updateDupeIndicatorForVfo(int vfo)
{
    vfo = qBound(0, vfo, 1);
    QLabel *dupeLabel = m_dupeLabels[vfo];
    const QString callsign = m_callsignEdits[vfo]->text().trimmed().toUpper();
    const QString band = m_bandSelectors[vfo]->currentText().trimmed().toLower();
    const QString mode = m_modeSelectors[vfo]->currentText().trimmed().toUpper();
    const ContestDefinition *definition = activeDefinition();
    QString state = QStringLiteral("ok");
    QString text;
    QStringList workedBands;
    if (!callsign.isEmpty() && m_contestSelector->currentIndex() >= 0) {
        const QStringList dupeKey =
            definition != nullptr ? definition->dupeKey : QStringList{};
        bool duplicate = false;
        for (const Qso &qso : std::as_const(m_logQsos)) {
            if (qso.invalid
                || qso.callsign.compare(callsign, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (!workedBands.contains(qso.band)) {
                workedBands.append(qso.band);
            }
            duplicate = duplicate || sameDupeKey(dupeKey, qso, callsign, band, mode);
        }
        if (duplicate) {
            state = QStringLiteral("dupe");
            text = tr("DUPE");
        } else if (definition != nullptr) {
            Qso candidate;
            candidate.callsign = callsign;
            candidate.band = band;
            candidate.mode = mode;
            for (auto it = m_exchangeEdits[vfo].cbegin(); it != m_exchangeEdits[vfo].cend();
                 ++it) {
                const QString value = it.value()->text().trimmed().toUpper();
                if (!value.isEmpty()) {
                    candidate.receivedExchangeFields.insert(it.key(), value);
                }
            }
            QStringList newMultipliers;
            const QStringList keys =
                ContestEngine().multiplierKeys(*definition, candidate, multiplierResolver());
            for (const QString &key : keys) {
                if (!m_workedMultiplierKeys.contains(key)) {
                    newMultipliers.append(
                        key.section(QLatin1Char(':'), -1).section(QLatin1Char('@'), 0, 0));
                }
            }
            if (!newMultipliers.isEmpty()) {
                state = QStringLiteral("mult");
                text = tr("NEW MULT");
                dupeLabel->setToolTip(tr("New multiplier: %1")
                                          .arg(newMultipliers.join(QStringLiteral(", "))));
            } else if (workedBands.isEmpty()) {
                text = tr("NEW");
            }
        }
    }
    if (state != QStringLiteral("mult")) {
        dupeLabel->setToolTip(QString());
    }
    dupeLabel->setText(text);
    dupeLabel->setProperty("state", state);
    repolish(dupeLabel);

    QStringList bandMarks;
    for (const QString &worked : std::as_const(workedBands)) {
        bandMarks.append(worked.compare(band, Qt::CaseInsensitive) == 0
                             ? QStringLiteral("<span style='color:#CE0000'>%1</span>")
                                   .arg(worked.toHtmlEscaped())
                             : worked.toHtmlEscaped());
    }
    m_workedBandLabels[vfo]->setText(
        bandMarks.isEmpty() ? QString()
                            : tr("Worked: %1").arg(bandMarks.join(QLatin1Char(' '))));
    if (vfo == currentEntryContext()) {
        refreshCheckWindows();
    }
}

void MainWindow::updateRadioSnapshotForRadio(int radio, const RadioSnapshot &snapshot)
{
    radio = qBound(0, radio, 1);
    const RadioSnapshot previousSnapshot = m_radioSnapshots[radio];
    const auto frequencies = connectedVfoFrequencies(snapshot);
    const bool frequencyChanged = frequencies != connectedVfoFrequencies(previousSnapshot);
    m_radioSnapshots[radio] = snapshot;
    m_radioSnapshotTimes[radio] = QDateTime::currentDateTimeUtc();
    if (snapshot.state != ConnectionState::Connected) {
        m_runFrequencies[radio].clear();
    } else if (m_runModeSelector->currentData().toInt() == 0) {
        for (auto it = frequencies.cbegin(); it != frequencies.cend(); ++it) {
            if (!m_runFrequencies[radio].contains(it.key())) {
                m_runFrequencies[radio].insert(it.key(), it.value());
            } else if (qAbs(it.value() - m_runFrequencies[radio].value(it.key())) > 3000) {
                setRunMode(1);
                break;
            }
        }
    }
    if (snapshot.state != ConnectionState::Connected) {
        m_lastRadioModes[0].clear();
        m_lastRadioModes[1].clear();
    }
    const int activeRadio = m_activeRadioSelector->currentData().toInt();
    const RadioSnapshot &activeSnapshot = m_radioSnapshots[activeRadio];
    if (!m_so2rToggle->isChecked() && !activeSnapshot.dualVfoSupported
        && m_so2vToggle->isChecked()) {
        const QSignalBlocker blocker(m_so2vToggle);
        m_so2vToggle->setChecked(false);
        m_entryGroups[1]->hide();
        m_radioABandmapDock->hide();
    }

    for (int context = 0; context < 2; ++context) {
        const int contextRadio = m_so2rToggle->isChecked() ? context : activeRadio;
        const int contextVfo = m_so2rToggle->isChecked()
            ? m_radioSnapshots[contextRadio].focusVfo : context;
        const RadioSnapshot &contextSnapshot = m_radioSnapshots[contextRadio];
        const auto vfo = std::find_if(
            contextSnapshot.vfos.cbegin(), contextSnapshot.vfos.cend(),
            [contextVfo](const Vfo &candidate) { return candidate.number == contextVfo; });
        if (vfo != contextSnapshot.vfos.cend()) {
            if (vfo->frequencyHz > 0) {
                const QString band = bandForFrequency(vfo->frequencyHz);
                if (!band.isEmpty()) {
                    m_bandSelectors[context]->setCurrentText(band);
                }
            }
            // The radio only overrides the entry mode when its own mode changed since the
            // previous poll, so an operator's manual mode selection is not reverted every second.
            if (!vfo->mode.isEmpty() && vfo->mode != m_lastRadioModes[context]) {
                QString entryMode = vfo->mode.toUpper();
                if (entryMode == QLatin1String("USB") || entryMode == QLatin1String("LSB")) {
                    entryMode = QStringLiteral("SSB");
                } else if (entryMode == QLatin1String("CWR")) {
                    entryMode = QStringLiteral("CW");
                } else if (entryMode == QLatin1String("RTTYR")) {
                    entryMode = QStringLiteral("RTTY");
                }
                const int modeIndex = m_modeSelectors[context]->findText(
                    entryMode, Qt::MatchFixedString);
                if (modeIndex >= 0) {
                    m_modeSelectors[context]->setCurrentIndex(modeIndex);
                }
                m_lastRadioModes[context] = vfo->mode;
            }
        }
        m_focusButtons[context]->setStyleSheet(
            (m_so2rToggle->isChecked()
                 ? activeRadio == context
                     && contextSnapshot.state == ConnectionState::Connected
                 : activeRadio == radio && contextSnapshot.focusVfo == context)
                ? QStringLiteral("background: #FFFF00; color: black; font-weight: bold; "
                                 "border: 1px solid #806000; padding: 1px 8px;")
                : QStringLiteral("padding: 1px 8px;"));
    }

    const auto stateName = [this](ConnectionState state) {
        switch (state) {
        case ConnectionState::Disconnected: return tr("Disconnected");
        case ConnectionState::Connecting: return tr("Connecting");
        case ConnectionState::Connected: return tr("Connected");
        case ConnectionState::Error: return tr("Error");
        case ConnectionState::Unsupported: return tr("Unsupported");
        }
        return tr("Unknown");
    };
    const auto radioDetail = [&stateName](const RadioSnapshot &value) {
        if (value.state != ConnectionState::Connected) {
            return value.message.isEmpty() ? stateName(value.state) : value.message;
        }
        QString detail = QStringLiteral("%1 MHz %2")
            .arg(static_cast<double>(value.frequencyHz) / 1'000'000.0, 0, 'f', 6)
            .arg(value.mode);
        if (value.dualVfoSupported && value.vfos.size() >= 2) {
            detail += QStringLiteral(" (A %1, B %2)")
                .arg(value.vfos.at(0).frequencyHz / 1'000'000.0, 0, 'f', 3)
                .arg(value.vfos.at(1).frequencyHz / 1'000'000.0, 0, 'f', 3);
        }
        return detail;
    };
    m_radioLabel->setText(
        tr("Radio A: %1 — Radio B: %2 — active: %3")
            .arg(radioDetail(m_radioSnapshots[0]),
                 radioDetail(m_radioSnapshots[1]),
                 activeRadio == 0 ? tr("A") : tr("B")));
    m_radioLabel->setStyleSheet(
        m_radioSnapshots[0].state == ConnectionState::Error
                || m_radioSnapshots[1].state == ConnectionState::Error
            ? QStringLiteral("color: #c62828;")
            : QString());
    updateEntryHeaders();
    if (frequencyChanged || previousSnapshot.focusVfo != snapshot.focusVfo
        || previousSnapshot.state != snapshot.state) {
        refreshBandmap();
    }
}

RadioConfig MainWindow::radioConfigFromSettings(int radio) const
{
    radio = qBound(0, radio, 1);
    QSettings settings;
    settings.beginGroup(QStringLiteral("radio/%1").arg(radio == 0 ? 'A' : 'B'));
    const bool hasDedicatedSettings = settings.contains(QStringLiteral("backend"));
    RadioConfig config;
    config.backend = static_cast<RadioBackendType>(
        settings.value(QStringLiteral("backend"),
                      static_cast<int>(RadioBackendType::Hamlib)).toInt());
    config.transport = static_cast<RadioTransport>(
        settings.value(QStringLiteral("transport"),
                       static_cast<int>(RadioTransport::Serial)).toInt());
    config.modelId = settings.value(QStringLiteral("modelId"), 1).toInt();
    config.devicePath = settings.value(QStringLiteral("devicePath")).toString();
    config.host = settings.value(QStringLiteral("host"), config.host).toString();
    config.port = settings.value(QStringLiteral("port"), config.port).toInt();
    config.baudRate = settings.value(QStringLiteral("baudRate"), 9600).toInt();
    config.dataBits = settings.value(QStringLiteral("dataBits"), config.dataBits).toInt();
    config.stopBits = settings.value(QStringLiteral("stopBits"), config.stopBits).toInt();
    config.parity = static_cast<SerialParity>(
        settings.value(QStringLiteral("parity"), static_cast<int>(config.parity)).toInt());
    config.handshake = static_cast<SerialHandshake>(
        settings.value(QStringLiteral("handshake"),
                       static_cast<int>(config.handshake)).toInt());
    config.rtsState = static_cast<SerialControlState>(
        settings.value(QStringLiteral("rtsState"),
                       static_cast<int>(config.rtsState)).toInt());
    config.dtrState = static_cast<SerialControlState>(
        settings.value(QStringLiteral("dtrState"),
                       static_cast<int>(config.dtrState)).toInt());
    config.timeoutMs = settings.value(QStringLiteral("timeoutMs"), config.timeoutMs).toInt();
    config.pollIntervalMs =
        settings.value(QStringLiteral("pollIntervalMs"), config.pollIntervalMs).toInt();
    settings.endGroup();
    if (radio == 0 && !hasDedicatedSettings) {
        settings.beginGroup(QStringLiteral("radio"));
        config.backend = static_cast<RadioBackendType>(
            settings.value(QStringLiteral("backend"),
                           static_cast<int>(RadioBackendType::Hamlib)).toInt());
        config.transport = static_cast<RadioTransport>(
            settings.value(QStringLiteral("transport"),
                           static_cast<int>(RadioTransport::Serial)).toInt());
        config.modelId = settings.value(QStringLiteral("modelId"), 1).toInt();
        config.devicePath = settings.value(QStringLiteral("devicePath")).toString();
        config.host = settings.value(QStringLiteral("host"), config.host).toString();
        config.port = settings.value(QStringLiteral("port"), config.port).toInt();
        config.baudRate = settings.value(QStringLiteral("baudRate"), 9600).toInt();
        config.dataBits = settings.value(QStringLiteral("dataBits"), config.dataBits).toInt();
        config.stopBits = settings.value(QStringLiteral("stopBits"), config.stopBits).toInt();
        config.parity = static_cast<SerialParity>(
            settings.value(QStringLiteral("parity"), static_cast<int>(config.parity)).toInt());
        config.handshake = static_cast<SerialHandshake>(
            settings.value(QStringLiteral("handshake"),
                           static_cast<int>(config.handshake)).toInt());
        config.rtsState = static_cast<SerialControlState>(
            settings.value(QStringLiteral("rtsState"),
                           static_cast<int>(config.rtsState)).toInt());
        config.dtrState = static_cast<SerialControlState>(
            settings.value(QStringLiteral("dtrState"),
                           static_cast<int>(config.dtrState)).toInt());
        config.timeoutMs =
            settings.value(QStringLiteral("timeoutMs"), config.timeoutMs).toInt();
        config.pollIntervalMs =
            settings.value(QStringLiteral("pollIntervalMs"), config.pollIntervalMs).toInt();
        settings.endGroup();
    }
    return config;
}

void MainWindow::saveRadioConfig(int radio, const RadioConfig &config)
{
    radio = qBound(0, radio, 1);
    QSettings settings;
    settings.beginGroup(QStringLiteral("radio/%1").arg(radio == 0 ? 'A' : 'B'));
    settings.setValue(QStringLiteral("backend"), static_cast<int>(config.backend));
    settings.setValue(QStringLiteral("transport"), static_cast<int>(config.transport));
    settings.setValue(QStringLiteral("modelId"), config.modelId);
    settings.setValue(QStringLiteral("devicePath"), config.devicePath);
    settings.setValue(QStringLiteral("host"), config.host);
    settings.setValue(QStringLiteral("port"), config.port);
    settings.setValue(QStringLiteral("baudRate"), config.baudRate);
    settings.setValue(QStringLiteral("dataBits"), config.dataBits);
    settings.setValue(QStringLiteral("stopBits"), config.stopBits);
    settings.setValue(QStringLiteral("parity"), static_cast<int>(config.parity));
    settings.setValue(QStringLiteral("handshake"), static_cast<int>(config.handshake));
    settings.setValue(QStringLiteral("rtsState"), static_cast<int>(config.rtsState));
    settings.setValue(QStringLiteral("dtrState"), static_cast<int>(config.dtrState));
    settings.setValue(QStringLiteral("timeoutMs"), config.timeoutMs);
    settings.setValue(QStringLiteral("pollIntervalMs"), config.pollIntervalMs);
    settings.endGroup();
}

void MainWindow::updateDxClusterStatus(DxClusterController *controller,
                                       const DxClusterStatus &status)
{
    m_dxClusterStatuses.insert(controller, status);
    refreshDxClusterStatus();
}

void MainWindow::updateDxClusterStatus(const DxClusterStatus &status)
{
    m_dxClusterStatuses.insert(nullptr, status);
    refreshDxClusterStatus();
}

void MainWindow::refreshDxClusterStatus()
{
    int connected = 0;
    int connecting = 0;
    bool hasError = false;
    QString latestMessage;
    for (const DxClusterStatus &status : std::as_const(m_dxClusterStatuses)) {
        connected += status.state == ConnectionState::Connected;
        connecting += status.state == ConnectionState::Connecting;
        hasError = hasError || status.state == ConnectionState::Error;
        if (!status.message.isEmpty()) {
            latestMessage = status.message;
        }
    }
    m_dxClusterConnected = connected > 0;
    if (!m_dxClusterConnected && m_clusterLog != nullptr) {
        m_clusterLog->setRowCount(0);
    }
    const QString state = connected > 0
        ? tr("%n connected", nullptr, connected)
        : connecting > 0 ? tr("%n connecting", nullptr, connecting)
                         : hasError ? tr("Error") : tr("Disconnected");
    m_clusterLabel->setText(
        tr("DX cluster: %1%2").arg(state, latestMessage.isEmpty()
            ? QString() : QStringLiteral(" — ") + latestMessage));
    m_clusterDock->setWindowTitle(tr("DX Cluster announcements — %1").arg(state));
    m_clusterLabel->setStyleSheet(
        connected == 0 && hasError
            ? QStringLiteral("color: #c62828;")
            : QString());
}

void MainWindow::receiveSpot(const Spot &receivedSpot)
{
    Spot spot = receivedSpot;
    QString error;
    if (!m_repository.saveSpot(spot, &error)) {
        QMessageBox::critical(this, tr("Unable to save DX spot"), error);
        return;
    }
    m_spots.insert(spot.id, spot);
    if (m_spots.size() > 500) {
        auto oldest = m_spots.cbegin();
        for (auto it = m_spots.cbegin(); it != m_spots.cend(); ++it) {
            if (it.value().timestampUtc < oldest.value().timestampUtc) {
                oldest = it;
            }
        }
        m_spots.remove(oldest.key());
    }
    if (m_dxClusterConnected) {
        const bool atBottom = m_clusterLog->verticalScrollBar()->value()
            >= m_clusterLog->verticalScrollBar()->maximum() - 2;
        appendClusterRow(m_clusterLog, spot);
        m_clusterLog->resizeColumnsToContents();
        if (atBottom) {
            m_clusterLog->scrollToBottom();
        }
    }
    refreshBandmap();
}

void MainWindow::refreshBandmap()
{
    if (m_bandmap == nullptr || m_radioABandmap == nullptr) {
        return;
    }
    const ContestDefinition *definition = activeDefinition();
    const QStringList dupeKey = definition != nullptr ? definition->dupeKey : QStringList{};
    QSet<QString> loggedCalls;
    for (const Qso &qso : std::as_const(m_logQsos)) {
        loggedCalls.insert(qso.callsign.toUpper());
    }
    const auto isWorked = [this, &dupeKey](const Spot &spot) {
        const QString mode = spot.mode.trimmed().toUpper();
        return std::any_of(m_logQsos.cbegin(), m_logQsos.cend(),
                           [&](const Qso &qso) {
                               if (qso.invalid) {
                                   return false;
                               }
                               if (mode.isEmpty()) {
                                   return qso.callsign.compare(spot.callsign,
                                                               Qt::CaseInsensitive) == 0
                                       && qso.band.compare(spot.band,
                                                           Qt::CaseInsensitive) == 0;
                               }
                               return sameDupeKey(dupeKey, qso, spot.callsign,
                                                  spot.band, mode);
                           });
    };
    const auto hasNewMultiplier = [this, definition](const Spot &spot, int context) {
        if (definition == nullptr || definition->multipliers.isEmpty()) {
            return false;
        }
        Qso candidate;
        candidate.callsign = spot.callsign.trimmed().toUpper();
        candidate.band = spot.band.trimmed().toLower();
        candidate.mode = spot.mode.trimmed().toUpper();
        if (candidate.mode.isEmpty()) {
            candidate.mode = m_modeSelectors[context]->currentText().trimmed().toUpper();
        }
        if (candidate.mode == QLatin1String("USB")
            || candidate.mode == QLatin1String("LSB")) {
            candidate.mode = QStringLiteral("SSB");
        }
        if (candidate.callsign.isEmpty() || candidate.band.isEmpty()) {
            return false;
        }
        const QStringList keys = ContestEngine().multiplierKeys(
            *definition, candidate, multiplierResolver());
        return std::any_of(keys.cbegin(), keys.cend(),
                           [this](const QString &key) {
                               return !key.isEmpty()
                                   && !m_workedMultiplierKeys.contains(key);
                           });
    };
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const int spotLifetimeMinutes = qBound(
        1, QSettings().value(QStringLiteral("dxCluster/spotLifetimeMinutes"), 60).toInt(),
        1440);
    QVector<Spot> ordered = m_spots.values().toVector();
    std::sort(ordered.begin(), ordered.end(), [](const Spot &left, const Spot &right) {
        return left.timestampUtc > right.timestampUtc;
    });
    const QComboBox *sortSelector =
        findChild<QComboBox *>(QStringLiteral("bandmapSortSelector"));
    const bool sortByTime = sortSelector != nullptr
        && sortSelector->currentData().toString() == QLatin1String("time");

    const auto populate = [this, &ordered, &isWorked, &hasNewMultiplier,
                           &loggedCalls, &now, sortByTime, spotLifetimeMinutes](
                              QTableWidget *bandmap, QDockWidget *dock,
                              int context, int radio, int labelRadio) {
        qint64 selectedSpot = -1;
        if (const QTableWidgetItem *selected =
                bandmap->item(bandmap->currentRow(), 0)) {
            selectedSpot = selected->data(Qt::UserRole).toLongLong();
        }
        QString filterBand = m_bandmapBandFilter->currentData().toString();
        if (filterBand == QStringLiteral("@active")) {
            filterBand = m_bandSelectors[context]->currentText().trimmed().toLower();
        }
        const auto normalizeSpotMode = [](const QString &value) {
            const QString upper = value.trimmed().toUpper();
            return upper == QLatin1String("USB") || upper == QLatin1String("LSB")
                ? QStringLiteral("SSB") : upper;
        };
        const QString filterMode = normalizeSpotMode(m_bandmapModeFilter->currentData().toString());
        const QString filterContinent =
            m_bandmapContinentFilter->currentData().toString();
        QSet<QString> displayed;
        QVector<QPair<Spot, bool>> visible;
        for (const Spot &spot : std::as_const(ordered)) {
            const qint64 ageSeconds = spot.timestampUtc.secsTo(now);
            if (ageSeconds < 0 || ageSeconds > spotLifetimeMinutes * 60) {
                continue;
            }
            if (!filterBand.isEmpty()
                && spot.band.compare(filterBand, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (!filterMode.isEmpty()
                && normalizeSpotMode(spot.mode) != filterMode) {
                continue;
            }
            if (!filterContinent.isEmpty()) {
                const CtyPrefixEntry *ctyEntry =
                    SupportDataParser::findCtyEntry(spot.callsign, m_ctyPrefixes);
                const QString continent = ctyEntry == nullptr
                    ? QStringLiteral("@unknown") : ctyEntry->continent.toUpper();
                if (continent != filterContinent) {
                    continue;
                }
            }
            const QString dedupeKey =
                spot.callsign.toUpper() + QLatin1Char('|') + spot.band;
            if (displayed.contains(dedupeKey)) {
                continue;
            }
            displayed.insert(dedupeKey);
            const bool worked = isWorked(spot);
            if (m_onlyNewMultiplierSpots->isChecked()
                && !hasNewMultiplier(spot, context)) {
                continue;
            }
            if (worked && m_hideWorkedSpots->isChecked()) {
                continue;
            }
            visible.append({spot, worked});
        }
        std::sort(visible.begin(), visible.end(), [sortByTime](const auto &left,
                                                               const auto &right) {
            if (sortByTime) {
                if (left.first.timestampUtc != right.first.timestampUtc) {
                    return left.first.timestampUtc > right.first.timestampUtc;
                }
                return left.first.frequencyHz < right.first.frequencyHz;
            }
            if (left.first.frequencyHz != right.first.frequencyHz) {
                return left.first.frequencyHz < right.first.frequencyHz;
            }
            return left.first.timestampUtc > right.first.timestampUtc;
        });

        const RadioSnapshot &snapshot = m_radioSnapshots[radio];
        qint64 radioFrequency = 0;
        if (snapshot.state == ConnectionState::Connected) {
            radioFrequency = snapshot.frequencyHz;
            const int vfoNumber = m_so2rToggle->isChecked()
                ? snapshot.focusVfo : context;
            for (const Vfo &vfo : snapshot.vfos) {
                if (vfo.number == vfoNumber && vfo.frequencyHz > 0) {
                    radioFrequency = vfo.frequencyHz;
                }
            }
        }
        int nearestRow = -1;
        qint64 nearestDistance = 3'000;
        for (int row = 0; row < visible.size(); ++row) {
            const qint64 distance =
                qAbs(visible.at(row).first.frequencyHz - radioFrequency);
            if (radioFrequency > 0 && distance <= nearestDistance) {
                nearestDistance = distance;
                nearestRow = row;
            }
        }

        bandmap->setUpdatesEnabled(false);
        bandmap->setRowCount(static_cast<int>(visible.size()));
        const QFont normal = monoFont(10, true);
        QFont workedFont = monoFont(10, false);
        workedFont.setItalic(true);
        int selectRow = -1;
        for (int row = 0; row < visible.size(); ++row) {
            const Spot &spot = visible.at(row).first;
            const bool worked = visible.at(row).second;
            const bool newStation = !loggedCalls.contains(spot.callsign.toUpper());
            const qint64 ageMinutes = spot.timestampUtc.secsTo(now) / 60;
            const QStringList values{
                QString::number(static_cast<double>(spot.frequencyHz) / 1000.0, 'f', 1),
                spot.callsign,
                QStringLiteral("%1m").arg(ageMinutes),
                spot.spotter,
                spot.source,
                spot.comment};
            for (int column = 0; column < values.size(); ++column) {
                auto *item = new QTableWidgetItem(values.at(column));
                item->setFont(worked ? workedFont : normal);
                if (worked) {
                    item->setForeground(QColor(QStringLiteral("#707070")));
                } else if (column == 1) {
                    item->setForeground(newStation ? QColor(QStringLiteral("#006400"))
                                                   : QColor(Qt::black));
                } else {
                    item->setForeground(QColor(QStringLiteral("#000080")));
                }
                if (row == nearestRow) {
                    item->setBackground(QColor(QStringLiteral("#FFA500")));
                }
                if (column == 0) {
                    item->setData(Qt::UserRole, spot.id);
                    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                }
                bandmap->setItem(row, column, item);
            }
            if (spot.id == selectedSpot) {
                selectRow = row;
            }
        }
        bandmap->resizeColumnsToContents();
        bandmap->setUpdatesEnabled(true);
        if (selectRow >= 0) {
            bandmap->setCurrentCell(selectRow, 0);
        } else if (nearestRow >= 0) {
            bandmap->scrollToItem(bandmap->item(nearestRow, 0),
                                  QAbstractItemView::PositionAtCenter);
        }
        const QString radioLabel = labelRadio == 0 ? tr("Radio A") : tr("Radio B");
        dock->setWindowTitle(
            filterBand.isEmpty()
                ? tr("Bandmap — %1 — all bands (%2)").arg(radioLabel).arg(visible.size())
                : tr("Bandmap — %1 — %2 (%3)")
                      .arg(radioLabel, filterBand).arg(visible.size()));
    };

    const bool so2v = m_so2vToggle->isChecked();
    const bool so2r = m_so2rToggle->isChecked();
    if (so2v || so2r) {
        const int radioA = so2r ? 0 : m_activeRadioSelector->currentData().toInt();
        const int radioB = so2r ? 1 : radioA;
        populate(m_radioABandmap, m_radioABandmapDock, 0, radioA, 0);
        populate(m_bandmap, m_bandmapDock, 1, radioB, 1);
    } else {
        const int radio = m_activeRadioSelector->currentData().toInt();
        populate(m_bandmap, m_bandmapDock, currentEntryContext(), radio, radio);
    }
}

void MainWindow::activateSpot(int row, int)
{
    if (const QTableWidgetItem *idItem = m_bandmap->item(row, 0)) {
        if (m_so2vToggle->isChecked() || m_so2rToggle->isChecked()) {
            const int radio = m_so2rToggle->isChecked()
                ? 1 : m_activeRadioSelector->currentData().toInt();
            activateSpotId(idItem->data(Qt::UserRole).toLongLong(), radio, 1);
        } else {
            activateSpotId(idItem->data(Qt::UserRole).toLongLong());
        }
    }
}

void MainWindow::activateSpotId(qint64 spotId, int radio, int context)
{
    const auto spot = m_spots.constFind(spotId);
    if (spot == m_spots.cend()) {
        return;
    }
    if (radio < 0) {
        radio = m_activeRadioSelector->currentData().toInt();
    }
    if (context < 0) {
        context = m_so2rToggle->isChecked()
            ? radio : qBound(0, m_radioSnapshots[radio].focusVfo, 1);
    }
    m_callsignEdits[context]->setText(spot->callsign);
    if (!spot->band.isEmpty()) {
        m_bandSelectors[context]->setCurrentText(spot->band);
    }
    if (!spot->mode.isEmpty()) {
        m_modeSelectors[context]->setCurrentText(spot->mode);
    }
    if (m_radioSnapshots[radio].state != ConnectionState::Connected) {
        m_callsignEdits[context]->setFocus();
        statusBar()->showMessage(
            tr("Spot copied to Radio %1 entry; that radio is not connected, so it was not tuned.")
                .arg(radio == 0 ? tr("A") : tr("B")),
            5000);
        return;
    }
    const int vfo = m_so2rToggle->isChecked()
        ? m_radioSnapshots[radio].focusVfo : context;
    m_radioControllers[radio].setFrequency(spot->frequencyHz, vfo);
    m_callsignEdits[context]->setFocus();
    statusBar()->showMessage(
        tr("Tuning Radio %3 to %1 kHz for %2.")
            .arg(static_cast<double>(spot->frequencyHz) / 1000.0, 0, 'f', 1)
            .arg(spot->callsign)
            .arg(radio == 0 ? tr("A") : tr("B")),
        5000);
}

} // namespace logger
