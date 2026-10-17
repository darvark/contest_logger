#include "logger/ui/main_window.h"

#include "logger/contest/contest_engine.h"
#include "logger/radio/band_plan.h"
#include "logger/ui/new_log_dialog.h"
#include "logger/ui/operator_dialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDir>
#include <QFontMetrics>
#include <QFrame>
#include <QInputDialog>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QStandardPaths>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTabWidget>
#include <QTimeZone>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <utility>

namespace logger {
namespace {

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
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        error = QStringLiteral("Unable to create the application data directory.");
        return false;
    }
    path = QDir(directory).filePath(fileName);
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

class DxClusterConfigDialog final : public QDialog {
public:
    explicit DxClusterConfigDialog(const DxClusterConfig &config,
                                   int spotLifetimeMinutes, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("DX cluster connection"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_host = new QLineEdit(config.host, this);
        m_host->setPlaceholderText(QStringLiteral("cluster.example.net"));
        m_port = new QSpinBox(this);
        m_port->setRange(1, 65535);
        m_port->setValue(config.port);
        m_callsign = new QLineEdit(config.callsign, this);
        m_spotLifetime = new QSpinBox(this);
        m_spotLifetime->setObjectName(QStringLiteral("dxSpotLifetimeMinutes"));
        m_spotLifetime->setRange(1, 1440);
        m_spotLifetime->setSuffix(tr(" min"));
        m_spotLifetime->setValue(spotLifetimeMinutes);
        form->addRow(tr("Host:"), m_host);
        form->addRow(tr("Telnet port:"), m_port);
        form->addRow(tr("Station callsign:"), m_callsign);
        form->addRow(tr("Spot lifetime:"), m_spotLifetime);
        layout->addLayout(form);
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
        m_type = new QComboBox(this);
        m_type->setObjectName(QStringLiteral("cwKeyerProvider"));
        m_type->addItem(tr("Hamlib radio"), static_cast<int>(CwKeyerType::Hamlib));
        m_type->addItem(tr("cwdaemon (UDP)"), static_cast<int>(CwKeyerType::Cwdaemon));
        m_type->addItem(tr("Mock (testing)"), static_cast<int>(CwKeyerType::Mock));
        m_type->setCurrentIndex(m_type->findData(static_cast<int>(config.type)));
        form->addRow(tr("Provider:"), m_type);
        m_host = new QLineEdit(config.type == CwKeyerType::Cwdaemon
                                   ? config.host : config.radio.host, this);
        m_host->setPlaceholderText(QStringLiteral("127.0.0.1"));
        form->addRow(tr("Host:"), m_host);
        m_port = new QSpinBox(this);
        m_port->setRange(1, 65535);
        m_port->setValue(config.type == CwKeyerType::Cwdaemon
                             ? config.port : config.radio.port);
        form->addRow(tr("UDP / rigctld port:"), m_port);
        m_modelId = new QSpinBox(this);
        m_modelId->setRange(1, 100000);
        m_modelId->setValue(config.radio.modelId);
        form->addRow(tr("Hamlib model ID:"), m_modelId);
        m_transport = new QComboBox(this);
        m_transport->setObjectName(QStringLiteral("cwHamlibTransport"));
        m_transport->addItem(tr("Serial"), static_cast<int>(RadioTransport::Serial));
        m_transport->addItem(tr("TCP / rigctld"), static_cast<int>(RadioTransport::Tcp));
        m_transport->setCurrentIndex(
            m_transport->findData(static_cast<int>(config.radio.transport)));
        form->addRow(tr("Hamlib transport:"), m_transport);
        m_device = new QLineEdit(config.radio.devicePath, this);
        form->addRow(tr("Serial device:"), m_device);
        m_baud = new QSpinBox(this);
        m_baud->setRange(1200, 115200);
        m_baud->setSingleStep(1200);
        m_baud->setValue(config.radio.baudRate);
        form->addRow(tr("Baud rate:"), m_baud);
        m_rtsState = new QComboBox(this);
        m_rtsState->setObjectName(QStringLiteral("cwHamlibRtsState"));
        populateSerialControlSelector(m_rtsState, config.radio.rtsState);
        form->addRow(tr("Hamlib serial RTS:"), m_rtsState);
        m_dtrState = new QComboBox(this);
        m_dtrState->setObjectName(QStringLiteral("cwHamlibDtrState"));
        populateSerialControlSelector(m_dtrState, config.radio.dtrState);
        form->addRow(tr("Hamlib serial DTR:"), m_dtrState);
        m_wpm = new QSpinBox(this);
        m_wpm->setRange(5, 80);
        m_wpm->setValue(config.wpm);
        form->addRow(tr("Speed (WPM):"), m_wpm);
        layout->addLayout(form);
        updateSerialControlVisibility(form);
        connect(m_type, &QComboBox::currentIndexChanged,
                this, [this, form] { updateSerialControlVisibility(form); });
        connect(m_transport, &QComboBox::currentIndexChanged,
                this, [this, form] { updateSerialControlVisibility(form); });
        auto *note = new QLabel(
            tr("Hamlib opens its configured rig connection. Use rigctld if CAT "
               "already owns the radio's serial port. cwdaemon uses UDP port 6789 "
               "by default and does not provide a delivery acknowledgement."),
            this);
        note->setWordWrap(true);
        layout->addWidget(note);
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
        result.type = static_cast<CwKeyerType>(m_type->currentData().toInt());
        result.host = m_host->text().trimmed();
        result.port = m_port->value();
        result.wpm = m_wpm->value();
        result.radio.backend = RadioBackendType::Hamlib;
        result.radio.transport =
            static_cast<RadioTransport>(m_transport->currentData().toInt());
        result.radio.modelId = m_modelId->value();
        result.radio.host = m_host->text().trimmed();
        result.radio.port = m_port->value();
        result.radio.devicePath = m_device->text().trimmed();
        result.radio.baudRate = m_baud->value();
        result.radio.rtsState =
            static_cast<SerialControlState>(m_rtsState->currentData().toInt());
        result.radio.dtrState =
            static_cast<SerialControlState>(m_dtrState->currentData().toInt());
        return result;
    }

private:
    void updateSerialControlVisibility(QFormLayout *form)
    {
        const bool visible =
            m_type->currentData().toInt() == static_cast<int>(CwKeyerType::Hamlib)
            && m_transport->currentData().toInt()
                == static_cast<int>(RadioTransport::Serial);
        m_rtsState->setVisible(visible);
        m_dtrState->setVisible(visible);
        if (QWidget *label = form->labelForField(m_rtsState)) {
            label->setVisible(visible);
        }
        if (QWidget *label = form->labelForField(m_dtrState)) {
            label->setVisible(visible);
        }
    }

    QComboBox *m_type = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QSpinBox *m_modelId = nullptr;
    QComboBox *m_transport = nullptr;
    QLineEdit *m_device = nullptr;
    QSpinBox *m_baud = nullptr;
    QComboBox *m_rtsState = nullptr;
    QComboBox *m_dtrState = nullptr;
    QSpinBox *m_wpm = nullptr;
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

CwKeyerConfig cwKeyerConfigFromSettings(const QSettings &settings, int radioIndex,
                                       RadioConfig radio)
{
    const int previousRadioIndex =
        settings.value(QStringLiteral("cw/radioIndex"), radioIndex).toInt();
    CwKeyerConfig config;
    config.type = static_cast<CwKeyerType>(
        settings.value(QStringLiteral("cw/type"),
                       static_cast<int>(CwKeyerType::Mock)).toInt());
    config.host = settings.value(QStringLiteral("cw/host"),
                                 QStringLiteral("127.0.0.1")).toString();
    config.port = settings.value(QStringLiteral("cw/port"), 6789).toInt();
    config.wpm = settings.value(QStringLiteral("cw/wpm"), 25).toInt();
    config.radio = std::move(radio);
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

QString functionKeyLabel(int key)
{
    QSettings settings;
    return settings.value(QStringLiteral("cw/functionKeys/F%1/label").arg(key + 1),
                          QString::fromLatin1(kFunctionKeyDefaults[key].label))
        .toString();
}

QString functionKeyText(int key)
{
    QSettings settings;
    return settings.value(QStringLiteral("cw/functionKeys/F%1/text").arg(key + 1),
                          QString::fromLatin1(kFunctionKeyDefaults[key].text))
        .toString();
}

QString dxlogStyleSheet()
{
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
QFrame#cwPane, QFrame#contestInfoPane, QFrame#contestInfoPane QScrollArea,
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
        spot.callsign, spot.spotter, spot.comment};
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

} // namespace

MainWindow::MainWindow(SqliteRepository &repository, QWidget *parent)
    : QMainWindow(parent)
    , m_repository(repository)
{
    setWindowTitle(tr("Contest Logger"));
    resize(1600, 960);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setStyleSheet(dxlogStyleSheet());

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *newLogAction = fileMenu->addAction(tr("&New log..."));
    newLogAction->setObjectName(QStringLiteral("newLogAction"));
    newLogAction->setShortcut(QKeySequence::New);
    connect(newLogAction, &QAction::triggered, this, &MainWindow::createNewLog);
    auto *logPropertiesAction = fileMenu->addAction(tr("Log &properties..."));
    logPropertiesAction->setObjectName(QStringLiteral("logPropertiesAction"));
    connect(logPropertiesAction, &QAction::triggered,
            this, &MainWindow::editLogProperties);
    fileMenu->addSeparator();
    auto *contestMenu = menuBar()->addMenu(tr("&Contest"));
    auto *importAction = contestMenu->addAction(tr("&Import YAML definition..."));
    connect(importAction, &QAction::triggered,
            this, &MainWindow::importContestDefinition);
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
    m_esmAction->setChecked(QSettings().value(QStringLiteral("operating/esmEnabled"),
                                               false).toBool());
    connect(m_esmAction, &QAction::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("operating/esmEnabled"), enabled);
    });
    m_autoCqAction = operatingMenu->addAction(tr("Auto CQ (F1 repeat)"));
    m_autoCqAction->setObjectName(QStringLiteral("autoCqAction"));
    m_autoCqAction->setCheckable(true);
    connect(m_autoCqAction, &QAction::toggled,
            this, &MainWindow::setAutoCqEnabled);
    const int savedRunMode =
        qBound(0, QSettings().value(QStringLiteral("operating/runMode"), 0).toInt(), 1);
    runModeGroup->actions().at(savedRunMode)->setChecked(true);
    auto *radioMenu = menuBar()->addMenu(tr("&Radio"));
    auto *settingsMenu = menuBar()->addMenu(tr("&Options"));
    auto *operatorAction = settingsMenu->addAction(tr("&Operator and station..."));
    connect(operatorAction, &QAction::triggered,
            this, &MainWindow::configureOperator);
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
    connect(&m_dxClusterController, &DxClusterController::statusChanged,
            this, &MainWindow::updateDxClusterStatus);
    connect(&m_dxClusterController, &DxClusterController::spotReceived,
            this, &MainWindow::receiveSpot);
    connect(&m_cwKeyerController, &CwKeyerController::statusChanged,
            this, &MainWindow::updateCwStatus);

    radioMenu->addSeparator();
    auto *clusterAction = radioMenu->addAction(tr("&DX cluster..."));
    connect(clusterAction, &QAction::triggered,
            this, &MainWindow::configureDxCluster);
    auto *disconnectClusterAction =
        radioMenu->addAction(tr("Disconnect DX cluster"));
        disconnectClusterAction->setObjectName(QStringLiteral("disconnectDxClusterAction"));
    connect(disconnectClusterAction, &QAction::triggered,
            &m_dxClusterController, &DxClusterController::stop);
    radioMenu->addSeparator();
    auto *cwKeyerAction = radioMenu->addAction(tr("CW keyer..."));
    connect(cwKeyerAction, &QAction::triggered,
            this, &MainWindow::configureCwKeyer);
        auto *reconnectCwAction = radioMenu->addAction(tr("Reconnect CW keyer"));
        reconnectCwAction->setObjectName(QStringLiteral("reconnectCwKeyerAction"));
        connect(reconnectCwAction, &QAction::triggered, this, [this] {
            const int radioIndex = m_cwRadioSelector->currentData().toInt();
            QSettings settings;
            const CwKeyerConfig config = cwKeyerConfigFromSettings(
                settings, radioIndex, radioConfigFromSettings(radioIndex));
            m_cwWpmSpin->setValue(config.wpm);
            m_cwConfiguredRadio = radioIndex;
            m_cwKeyerController.connectKeyer(config);
        });
    auto *disconnectCwAction = radioMenu->addAction(tr("Disconnect CW keyer"));
        disconnectCwAction->setObjectName(QStringLiteral("disconnectCwKeyerAction"));
    connect(disconnectCwAction, &QAction::triggered,
            &m_cwKeyerController, &CwKeyerController::disconnectKeyer);
    auto *abortCwAction = radioMenu->addAction(tr("Abort CW transmission"));
    abortCwAction->setShortcuts({QKeySequence(Qt::Key_Escape),
                                 QKeySequence(Qt::CTRL | Qt::Key_Escape)});
    abortCwAction->setShortcutContext(Qt::WindowShortcut);
    connect(abortCwAction, &QAction::triggered,
            this, &MainWindow::abortCwTransmission);

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
    auto *previousSpotAction = operatingMenu->addAction(tr("Previous bandmap spot"));
    previousSpotAction->setObjectName(QStringLiteral("previousBandmapSpotAction"));
    previousSpotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    previousSpotAction->setShortcutContext(Qt::WindowShortcut);
    connect(previousSpotAction, &QAction::triggered, this,
            [this] {
                QTableWidget *bandmap =
                    (m_so2vToggle->isChecked() || m_so2rToggle->isChecked())
                        && currentEntryContext() == 0
                    ? m_radioABandmap : m_bandmap;
                selectAdjacentBandmapRow(bandmap, -1);
            });
    auto *nextSpotAction = operatingMenu->addAction(tr("Next bandmap spot"));
    nextSpotAction->setObjectName(QStringLiteral("nextBandmapSpotAction"));
    nextSpotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    nextSpotAction->setShortcutContext(Qt::WindowShortcut);
    connect(nextSpotAction, &QAction::triggered, this,
            [this] {
                QTableWidget *bandmap =
                    (m_so2vToggle->isChecked() || m_so2rToggle->isChecked())
                        && currentEntryContext() == 0
                    ? m_radioABandmap : m_bandmap;
                selectAdjacentBandmapRow(bandmap, 1);
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

    auto *exportCabrilloAction = fileMenu->addAction(tr("Export Cabrillo..."));
    connect(exportCabrilloAction, &QAction::triggered,
            this, &MainWindow::exportCabrillo);
    auto *exportAdifAction = fileMenu->addAction(tr("Export ADIF..."));
    connect(exportAdifAction, &QAction::triggered,
            this, &MainWindow::exportAdif);
    fileMenu->addSeparator();
    auto *backupAction = fileMenu->addAction(tr("Back up database..."));
    connect(backupAction, &QAction::triggered,
            this, &MainWindow::backupDatabase);
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
        auto *manager = new QNetworkAccessManager(this);
        const QUrl catalogUrl(QStringLiteral(
            "https://www.supercheckpartial.com/api/v1/files"));
        const QNetworkRequest catalogRequest = supportDataRequest(catalogUrl);
        issueNetworkRequest(this, manager, catalogRequest,
                            [this, manager](QNetworkReply *catalogReply) {
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
            if (cachedEtag == etag && QFileInfo::exists(cachePath)) {
                fileRequest.setRawHeader(
                    "If-None-Match", QByteArray("\"") + etag.toUtf8() + QByteArray("\""));
            }
            issueNetworkRequest(this, manager, fileRequest,
                                [this, manager, fileName, etag, expectedSize]
                                (QNetworkReply *fileReply) {
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
                loadSupportData();
                manager->deleteLater();
                statusBar()->showMessage(
                    tr("Loaded %1 callsigns from Master.SCP.").arg(callsigns.size()), 6000);
            });
        });
    });
    auto *ctyDatAction =
        supportDataMenu->addAction(tr("wl_cty.dat from file..."));
    connect(ctyDatAction, &QAction::triggered,
            this, &MainWindow::importCtyDat);
    auto *downloadCtyAction =
        supportDataMenu->addAction(tr("Download/update wl_cty.dat..."));
    downloadCtyAction->setObjectName(QStringLiteral("downloadWlCtyAction"));
    connect(downloadCtyAction, &QAction::triggered, this, [this] {
        auto *manager = new QNetworkAccessManager(this);
        const QNetworkRequest request = supportDataRequest(QUrl(
            QStringLiteral("https://www.country-files.com/cty/wl_cty.dat")));
        issueNetworkRequest(this, manager, request,
                            [this, manager](QNetworkReply *reply) {
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
    m_contestSelector = new QComboBox(toolbar);
    m_contestSelector->setObjectName(QStringLiteral("contestSelector"));
    m_contestSelector->setMinimumContentsLength(30);
    m_contestSelector->setMinimumWidth(260);
    m_contestSelector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    toolbar->addWidget(m_contestSelector);
    connect(m_contestSelector, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::selectContest);
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
    m_recentQsos->setToolTip(tr("Double-click a QSO to edit it."));
    connect(m_recentQsos, &QTableWidget::cellDoubleClicked,
            this, &MainWindow::editSelectedQso);
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
                    if (vfo == currentEntryContext() || m_so2vToggle->isChecked()
                        || m_so2rToggle->isChecked()) {
                        refreshBandmap();
                        refreshScoreSummary();
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

    // CW messages / keyer.
    auto *cwPane = createPane(QStringLiteral("cwPane"));
    auto *cwLayout = new QGridLayout;
    cwLayout->setContentsMargins(4, 4, 4, 4);
    cwLayout->setSpacing(4);
    m_cwMessageEdit = new QLineEdit(cwPane);
    m_cwMessageEdit->setObjectName(QStringLiteral("cwMessageInput"));
    m_cwMessageEdit->setPlaceholderText(tr("Free text; {CALL} inserts current callsign"));
    m_cwMessageEdit->setFont(monoFont(10, true));
    m_cwRadioSelector = new QComboBox(cwPane);
    m_cwRadioSelector->setObjectName(QStringLiteral("cwRadioSelector"));
    m_cwRadioSelector->addItem(tr("Radio A"), 0);
    m_cwRadioSelector->addItem(tr("Radio B"), 1);
    QSettings cwSettings;
    m_cwRadioSelector->setCurrentIndex(
        m_cwRadioSelector->findData(
            cwSettings.value(QStringLiteral("cw/radioIndex"), 0).toInt()));
    m_cwConfiguredRadio = -1;
    m_cwWpmSpin = new QSpinBox(cwPane);
    m_cwWpmSpin->setRange(5, 80);
    m_cwWpmSpin->setSuffix(tr(" WPM"));
    m_cwWpmSpin->setValue(cwSettings.value(QStringLiteral("cw/wpm"), 25).toInt());
    auto *sendCwButton = new QPushButton(tr("Send"), cwPane);
    sendCwButton->setObjectName(QStringLiteral("queueCwButton"));
    auto *abortCwButton = new QPushButton(tr("Abort (Esc)"), cwPane);
    abortCwButton->setObjectName(QStringLiteral("abortCwButton"));
    m_cwStatusLabel = new QLabel(tr("CW keyer disconnected"), cwPane);
    m_cwStatusLabel->setObjectName(QStringLiteral("cwKeyerStatus"));
    m_cwStatusLabel->setWordWrap(true);
    cwLayout->addWidget(new QLabel(tr("Text:"), cwPane), 0, 0);
    cwLayout->addWidget(m_cwMessageEdit, 0, 1, 1, 3);
    cwLayout->addWidget(new QLabel(tr("Radio:"), cwPane), 1, 0);
    cwLayout->addWidget(m_cwRadioSelector, 1, 1);
    cwLayout->addWidget(m_cwWpmSpin, 1, 2, 1, 2);
    cwLayout->addWidget(sendCwButton, 2, 1);
    cwLayout->addWidget(abortCwButton, 2, 2, 1, 2);
    cwLayout->addWidget(m_cwStatusLabel, 3, 0, 1, 4);
    cwLayout->setRowStretch(4, 1);
    cwLayout->setColumnStretch(1, 1);
    static_cast<QVBoxLayout *>(cwPane->layout())->addLayout(cwLayout);
    m_cwDock = createPaneDock(tr("CW messages / keyer"),
                              QStringLiteral("cwKeyerDock"), cwPane);
    connect(sendCwButton, &QPushButton::clicked,
            this, &MainWindow::queueCwMessage);
    connect(abortCwButton, &QPushButton::clicked,
            this, &MainWindow::abortCwTransmission);
    connect(m_cwMessageEdit, &QLineEdit::returnPressed,
            this, &MainWindow::queueCwMessage);
    connect(m_cwWpmSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::setCwWpm);
    connect(m_cwRadioSelector, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
                m_cwConfiguredRadio = -1;
                statusBar()->showMessage(
                    tr("CW radio changed. Reconfigure the keyer before transmitting."),
                    5000);
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
                                QStringLiteral("USB"), QStringLiteral("LSB"),
                                QStringLiteral("RTTY"), QStringLiteral("FT8"),
                                QStringLiteral("FT4"), QStringLiteral("PSK"),
                                QStringLiteral("FM"), QStringLiteral("AM")}) {
        m_bandmapModeFilter->addItem(mode, mode);
    }
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
    bandmapControls->addWidget(bandmapSortSelector);
    bandmapControls->addWidget(m_hideWorkedSpots);
    bandmapControls->addWidget(tuneSelectedSpot);
    static_cast<QVBoxLayout *>(bandmapPane->layout())->addLayout(bandmapControls);
    m_bandmap = new QTableWidget(0, 5, bandmapPane);
    m_bandmap->setObjectName(QStringLiteral("dxBandmap"));
    m_bandmap->setHorizontalHeaderLabels(
        {tr("kHz"), tr("Call"), tr("Age"), tr("Spotter"), tr("Comment")});
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
        tr("Ctrl+Up/Down selects a spot; Enter copies its callsign and tunes the active radio. "
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
    connect(bandmapSortSelector, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshBandmap);
    connect(m_hideWorkedSpots, &QCheckBox::toggled,
            this, &MainWindow::refreshBandmap);
    bandmapPane->layout()->addWidget(m_bandmap);
    m_bandmapDock = createPaneDock(tr("Bandmap"), QStringLiteral("dxBandmapDock"),
                                   bandmapPane);

    auto *radioABandmapPane = createPane(QStringLiteral("radioABandmapPane"));
    m_radioABandmap = new QTableWidget(0, 5, radioABandmapPane);
    m_radioABandmap->setObjectName(QStringLiteral("dxBandmapRadioA"));
    m_radioABandmap->setHorizontalHeaderLabels(
        {tr("kHz"), tr("Call"), tr("Age"), tr("Spotter"), tr("Comment")});
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
    m_clusterLog = new QTableWidget(0, 5, clusterPane);
    m_clusterLog->setObjectName(QStringLiteral("clusterLog"));
    m_clusterLog->setHorizontalHeaderLabels(
        {tr("UTC"), tr("kHz"), tr("Call"), tr("Spotter"), tr("Comment")});
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

    // Rate.
    auto *ratePane = createPane(QStringLiteral("ratePane"));
    m_rateLabel = new QLabel(ratePane);
    m_rateLabel->setObjectName(QStringLiteral("rateText"));
    m_rateLabel->setFont(monoFont(10, true));
    m_rateLabel->setTextFormat(Qt::PlainText);
    m_rateLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_rateLabel->setMargin(4);
    ratePane->layout()->addWidget(m_rateLabel);
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
    m_summaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
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
    // Left: check windows and CW; right: bandmap column plus rate,
    // summary and cluster stacked beside it (the DXLog default arrangement).
    addDockWidget(Qt::LeftDockWidgetArea, m_checkCallDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_checkPartialDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_cwDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_radioABandmapDock);
    splitDockWidget(m_checkCallDock, m_radioABandmapDock, Qt::Horizontal);
    tabifyDockWidget(m_cwDock, m_contestInfoDock);
    addDockWidget(Qt::RightDockWidgetArea, m_bandmapDock);
    addDockWidget(Qt::RightDockWidgetArea, m_rateDock);
    splitDockWidget(m_bandmapDock, m_rateDock, Qt::Horizontal);
    splitDockWidget(m_rateDock, m_summaryDock, Qt::Vertical);
    splitDockWidget(m_summaryDock, m_clusterDock, Qt::Vertical);
    for (QDockWidget *dock : {m_checkCallDock, m_checkPartialDock, m_cwDock,
                              m_radioABandmapDock,
                              m_contestInfoDock, m_bandmapDock, m_rateDock,
                              m_summaryDock, m_clusterDock}) {
        dock->setFloating(false);
        dock->show();
    }
    m_radioABandmapDock->hide();
    m_cwDock->raise();
    resizeDocks({m_checkCallDock, m_radioABandmapDock, m_bandmapDock, m_rateDock},
                {230, 280, 250, 400}, Qt::Horizontal);
    resizeDocks({m_checkCallDock, m_checkPartialDock, m_cwDock},
                {250, 260, 230}, Qt::Vertical);
    resizeDocks({m_rateDock, m_summaryDock, m_clusterDock},
                {200, 230, 320}, Qt::Vertical);
}

void MainWindow::createWindowMenu()
{
    QMenu *windowMenu = menuBar()->addMenu(tr("&Window"));
    for (QDockWidget *dock : {m_checkCallDock, m_checkPartialDock, m_bandmapDock,
                              m_clusterDock, m_rateDock, m_summaryDock, m_cwDock,
                              m_contestInfoDock}) {
        windowMenu->addAction(dock->toggleViewAction());
    }
    windowMenu->addSeparator();
    QAction *resetAction = windowMenu->addAction(tr("Restore default layout"));
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
    for (QDockWidget *dock : {m_checkCallDock, m_checkPartialDock, m_cwDock,
                              m_radioABandmapDock,
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
    QMainWindow::closeEvent(event);
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
    QSettings settings;
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
    refreshBandmap();
    m_dxClusterController.start(config);
}

void MainWindow::configureCwKeyer()
{
    const int radioIndex = m_cwRadioSelector->currentData().toInt();
    QSettings settings;
    CwKeyerConfig config = cwKeyerConfigFromSettings(
        settings, radioIndex, radioConfigFromSettings(radioIndex));
    CwKeyerConfigDialog dialog(config, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    config = dialog.config();
    if (config.type == CwKeyerType::Hamlib
        && config.radio.transport == RadioTransport::Serial
        && config.radio.devicePath.isEmpty()) {
        QMessageBox::warning(this, tr("Serial device required"),
                             tr("Select a serial device for the Hamlib keyer."));
        return;
    }
    settings.setValue(QStringLiteral("cw/type"), static_cast<int>(config.type));
    settings.setValue(QStringLiteral("cw/host"), config.host);
    settings.setValue(QStringLiteral("cw/port"), config.port);
    settings.setValue(QStringLiteral("cw/wpm"), config.wpm);
    settings.setValue(QStringLiteral("cw/radioIndex"), radioIndex);
    settings.setValue(QStringLiteral("cw/hamlib/modelId"), config.radio.modelId);
    settings.setValue(QStringLiteral("cw/hamlib/transport"),
                      static_cast<int>(config.radio.transport));
    settings.setValue(QStringLiteral("cw/hamlib/devicePath"), config.radio.devicePath);
    settings.setValue(QStringLiteral("cw/hamlib/baudRate"), config.radio.baudRate);
    settings.setValue(QStringLiteral("cw/hamlib/rtsState"),
                      static_cast<int>(config.radio.rtsState));
    settings.setValue(QStringLiteral("cw/hamlib/dtrState"),
                      static_cast<int>(config.radio.dtrState));
    settings.setValue(QStringLiteral("cw/hamlib/host"), config.radio.host);
    settings.setValue(QStringLiteral("cw/hamlib/port"), config.radio.port);
    m_cwWpmSpin->setValue(config.wpm);
    m_cwConfiguredRadio = radioIndex;
    m_cwKeyerController.connectKeyer(config);
}

void MainWindow::queueCwMessage()
{
    const QString message = m_cwMessageEdit->text().trimmed();
    if (message.isEmpty()) {
        return;
    }
    const int radio = m_cwRadioSelector->currentData().toInt();
    const int context = m_so2rToggle->isChecked()
        ? radio : qBound(0, m_radioSnapshots[radio].focusVfo, 1);
    QString problem;
    if (!sendCwTemplate(message, context, &problem)) {
        QMessageBox::warning(this, tr("CW not sent"), problem);
        return;
    }
    m_cwMessageEdit->clear();
}

bool MainWindow::sendCwTemplate(QString message, int context, QString *problem)
{
    const int radio = m_cwRadioSelector->currentData().toInt();
    if (m_cwConfiguredRadio != radio) {
        if (problem != nullptr) {
            *problem = tr("Reconfigure the CW keyer for Radio %1 before transmitting.")
                           .arg(radio == 0 ? tr("A") : tr("B"));
        }
        return false;
    }
    context = qBound(0, context, 1);
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
    QStringList sentValues;
    if (const ContestDefinition *definition = activeDefinition()) {
        const int nextSerial = ContestEngine().nextSerialNumber(*definition, m_logQsos);
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
            sentValues.append(value);
            message.replace(QStringLiteral("{%1}").arg(field.name.toUpper()), value,
                            Qt::CaseInsensitive);
        }
    }
    message.replace(QStringLiteral("{CALL}"),
                    m_callsignEdits[context]->text().trimmed().toUpper(),
                    Qt::CaseInsensitive);
    message.replace(QStringLiteral("{MYCALL}"), m_station.callsign.toUpper(),
                    Qt::CaseInsensitive);
    message.replace(QStringLiteral("{SENT}"), sentValues.join(QLatin1Char(' ')),
                    Qt::CaseInsensitive);
    message = message.simplified();
    if (message.isEmpty()) {
        if (problem != nullptr) {
            *problem = tr("The message is empty after substituting macros.");
        }
        return false;
    }
    m_cwKeyerController.enqueueMessage(message, transmitVfo);
    return true;
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
    const QString label = functionKeyLabel(index);
    if (mode != QStringLiteral("CW")) {
        showOperatorMessage(
            tr("F%1 %2: function-key messages are sent in CW only (entry mode is %3)")
                .arg(index + 1).arg(label, mode.isEmpty() ? tr("unset") : mode),
            true);
        return false;
    }
    QString problem;
    if (!sendCwTemplate(functionKeyText(index), context, &problem)) {
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
        if (!sendFunctionKeyForContext(4, vfo)) {
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
    logQsoForVfo(vfo);
    if (m_callsignEdits[vfo]->text().isEmpty()) {
        sendFunctionKeyForContext(2, vfo);
    }
}

void MainWindow::setRunMode(int mode)
{
    mode = qBound(0, mode, 1);
    QSettings().setValue(QStringLiteral("operating/runMode"), mode);
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
        || m_cwStatus.state != ConnectionState::Connected
        || m_cwConfiguredRadio != m_cwRadioSelector->currentData().toInt()) {
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
        QLineEdit::Normal, functionKeyLabel(index), &ok);
    if (!ok) {
        return;
    }
    const QString text = QInputDialog::getText(
        this, tr("F%1 message").arg(index + 1),
        tr("CW text ({CALL}, {MYCALL}, {SENT} and {FIELD} macros are supported):"),
        QLineEdit::Normal, functionKeyText(index), &ok);
    if (!ok) {
        return;
    }
    QSettings settings;
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
           "{CALL}, {MYCALL}, {SENT}, and sent exchange fields such as {SERIAL}."),
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
        labels[key] = new QLineEdit(functionKeyLabel(key), &dialog);
        labels[key]->setObjectName(QStringLiteral("functionKeyLabel%1").arg(key + 1));
        messages[key] = new QPlainTextEdit(functionKeyText(key), &dialog);
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

        QSettings settings;
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

void MainWindow::updateFunctionKeyButtons()
{
    for (int key = 0; key < 8; ++key) {
        const QString label = functionKeyLabel(key);
        const QString text = functionKeyText(key);
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
    m_cwKeyerController.abortTransmission();
}

void MainWindow::setCwWpm(int wpm)
{
    QSettings settings;
    settings.setValue(QStringLiteral("cw/wpm"), wpm);
    m_cwKeyerController.setWpm(wpm);
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
        status.state == ConnectionState::Error || status.completionTimedOut
            ? QStringLiteral("color: #c62828;")
            : QString());
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
        applyActiveLogStation();
        m_activeContestLabel->setText(tr("No log is available."));
        rebuildQsoForm();
        refreshRecentQsos();
        refreshBandmap();
        return;
    }

    const Contest &contest = m_contests.at(index);
    QSettings settings;
    settings.setValue(QStringLiteral("session/activeContestId"), contest.id);
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

void MainWindow::createNewLog()
{
    QVector<NewLogContestOption> options;
    QHash<QString, int> optionIndexById;
    for (const Contest &contest : m_contests) {
        const auto definition = m_definitions.constFind(contest.id);
        if (definition == m_definitions.cend()) {
            continue;
        }
        const QString id = definition->id.toUpper();
        const bool templateRow = contest.code.compare(id, Qt::CaseInsensitive) == 0;
        const auto existing = optionIndexById.constFind(id);
        if (existing != optionIndexById.cend() && !templateRow) {
            continue;
        }
        QString error;
        const QString yaml = m_repository.contestDefinitionYaml(contest.id, &error);
        if (!error.isEmpty()) {
            QMessageBox::critical(this, tr("Unable to load contest definition"), error);
            return;
        }
        NewLogContestOption option{definition.value(), yaml, tr("installed")};
        if (existing != optionIndexById.cend()) {
            options[existing.value()] = std::move(option);
        } else {
            optionIndexById.insert(id, static_cast<int>(options.size()));
            options.append(std::move(option));
        }
    }

    NewLogDialog dialog(options, m_defaultStation, logNames(), this);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedOption() == nullptr) {
        return;
    }
    const NewLogContestOption selected = *dialog.selectedOption();
    const Contest log = dialog.log();
    QString error;
    const qint64 newId =
        m_repository.createContestLog(selected.definition, selected.yaml, log, &error);
    if (newId <= 0) {
        QMessageBox::critical(this, tr("Unable to create log"), error);
        return;
    }
    saveSentExchangeValues(newId, dialog.sentExchange());
    QSettings().setValue(QStringLiteral("session/activeContestId"), newId);
    loadContests();
    const int created = m_contestSelector->findData(newId);
    if (created >= 0 && m_contestSelector->currentIndex() != created) {
        m_contestSelector->setCurrentIndex(created);
    }
    statusBar()->showMessage(
        tr("New log created: %1 (%2)").arg(log.name, log.stationCallsign), 5000);
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
    QSettings settings;
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
    QSettings settings;
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
        this, tr("Import contest definition"), {},
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

    const QStringList installedCodes = m_repository.contestDefinitionCodes(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to inspect contest definitions"), error);
        return;
    }
    const QString code = definition.id.toUpper();
    if (installedCodes.contains(code)
        && QMessageBox::question(
               this, tr("Replace contest definition?"),
               tr("A definition with ID '%1' is already installed. Replace it?")
                   .arg(definition.id))
            != QMessageBox::Yes) {
        return;
    }

    if (!m_repository.saveContestDefinition(definition, yaml, &error)) {
        QMessageBox::critical(this, tr("Unable to import definition"), error);
        return;
    }
    loadContests();
    int index = -1;
    for (const Contest &contest : m_contests) {
        if (contest.code == code) {
            index = m_contestSelector->findData(contest.id);
            break;
        }
    }
    if (index >= 0) {
        m_contestSelector->setCurrentIndex(index);
    }
    statusBar()->showMessage(tr("Imported contest: %1").arg(definition.name), 5000);
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
    QHash<QString, CallHistoryEntry> imported;
    QString error;
    if (!SupportDataParser::parseCallHistory(
            QString::fromUtf8(file.readAll()), imported, &error)) {
        QMessageBox::critical(this, tr("Invalid call history"), error);
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
    QStringList imported;
    QString error;
    if (!SupportDataParser::parseMasterCpy(
            QString::fromUtf8(file.readAll()), imported, &error)) {
        QMessageBox::critical(this, tr("Invalid master.cpy"), error);
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
    QVector<CtyPrefixEntry> imported;
    QString error;
    if (!SupportDataParser::parseCtyDat(
            QString::fromUtf8(file.readAll()), imported, &error)) {
        QMessageBox::critical(this, tr("Invalid wl_cty.dat"), error);
        return;
    }
    m_ctyPrefixes = std::move(imported);
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

void MainWindow::loadSupportData()
{
    m_callHistory.clear();
    m_masterCallsigns.clear();
    m_ctyPrefixes.clear();
    m_ctyCache.clear();
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
    for (const QString &path : masterPaths) {
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
    const QString ctyPath =
        settings.value(QStringLiteral("supportData/ctyDat")).toString();
    if (!ctyPath.isEmpty()) {
        QString contents;
        QVector<CtyPrefixEntry> prefixes;
        if (readFile(ctyPath, contents)) {
            if (SupportDataParser::parseCtyDat(contents, prefixes, &error)) {
                m_ctyPrefixes = std::move(prefixes);
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
    QString error;
    const QStringList installedCodes =
        m_repository.contestDefinitionCodes(&error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("Unable to inspect contest definitions"), error);
        return;
    }

    const QStringList resources{
        QStringLiteral(":/contests/general-logging.yaml"),
        QStringLiteral(":/contests/serial-sprint.yaml"),
        QStringLiteral(":/contests/grid-field-day.yaml"),
        QStringLiteral(":/contests/cqww-cw.yaml"),
        QStringLiteral(":/contests/cqww-ssb.yaml"),
        QStringLiteral(":/contests/cqwpx-cw.yaml"),
        QStringLiteral(":/contests/cqwpx-ssb.yaml"),
        QStringLiteral(":/contests/iaruhfc.yaml"),
        QStringLiteral(":/contests/spdx.yaml"),
        QStringLiteral(":/contests/wag.yaml"),
        QStringLiteral(":/contests/arrl-dx-cw.yaml"),
        QStringLiteral(":/contests/arrl-dx-ssb.yaml"),
        QStringLiteral(":/contests/arrl-dx-10m.yaml"),
        QStringLiteral(":/contests/eudx.yaml"),
        QStringLiteral(":/contests/euhfc.yaml")
    };
    const ContestDefinitionLoader loader;
    for (const QString &resource : resources) {
        QFile file(resource);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::critical(
                this, tr("Unable to load example definition"),
                QStringLiteral("%1: %2").arg(resource, file.errorString()));
            continue;
        }
        const QString yaml = QString::fromUtf8(file.readAll());
        ContestDefinition definition;
        if (!loader.loadText(yaml, resource, definition, &error)) {
            QMessageBox::critical(this, tr("Invalid built-in definition"), error);
            continue;
        }
        if (!definition.cabrilloContest.isEmpty()) {
            m_builtInCabrilloContests.insert(definition.id, definition.cabrilloContest);
        }
        if (installedCodes.contains(definition.id.toUpper())) {
            continue;
        }
        if (!m_repository.saveContestDefinition(definition, yaml, &error)) {
            QMessageBox::critical(this, tr("Unable to install example definition"), error);
        }
    }
}

void MainWindow::loadOperator()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("station"));
    m_defaultStation.callsign = settings.value(QStringLiteral("callsign")).toString();
    m_defaultStation.operatorName =
        settings.value(QStringLiteral("operatorName")).toString();
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
    } else {
        selectContest(selectedIndex);
    }
}

void MainWindow::updateStationLabel()
{
    const QString operatorText = m_station.operatorName.isEmpty()
        ? tr("Operator not configured")
        : m_station.operatorName.toHtmlEscaped();
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
    QSettings settings;
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
            configured = QString::number(nextSerial);
            if (field.width > 0) {
                configured = configured.rightJustified(field.width, QLatin1Char('0'));
            }
            edit->setText(configured);
            edit->setReadOnly(true);
            edit->setToolTip(tr("Sent serial number, generated on logging"));
        }
        const QString sentLabel = field.label.size() > 10 ? field.name.toUpper() : field.label;
        QWidget *sentColumn = fieldColumn(tr("Snt %1").arg(sentLabel), edit, panel);
        sentColumn->setToolTip(tr("Sent %1").arg(field.label));
        m_sentExchangeLayouts[vfo]->addWidget(sentColumn);
        m_sentExchangeEdits[vfo].insert(field.name, edit);
        if (!(field.type == ExchangeFieldType::Serial && field.generated)) {
            installUppercase(edit);
            connect(edit, &QLineEdit::textChanged, this,
                    [settingKey](const QString &value) {
                        QSettings().setValue(settingKey, value);
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
    QVector<int> increments(m_logQsos.size(), 0);
    if (definition != nullptr) {
        const ContestEngine engine;
        const auto resolver = multiplierResolver();
        m_score = engine.score(*definition, m_logQsos, resolver);
        m_workedMultiplierKeys =
            engine.buildContext(*definition, m_logQsos, resolver).workedMultiplierKeys;
        for (qsizetype index = 0; index < m_logQsos.size(); ++index) {
            increments[index] = m_score.newMultipliersByQsoId.value(m_logQsos.at(index).id);
        }
    }

    // Only the tail of the log is rendered; the full log stays cached for checks.
    constexpr qsizetype kVisibleRows = 1000;
    const qsizetype first = qMax<qsizetype>(0, m_logQsos.size() - kVisibleRows);
    m_recentQsos->setUpdatesEnabled(false);
    m_recentQsos->setRowCount(static_cast<int>(m_logQsos.size() - first));
    const QColor multColor(QStringLiteral("#CE0000"));
    for (qsizetype index = first; index < m_logQsos.size(); ++index) {
        const Qso &qso = m_logQsos.at(index);
        const int row = static_cast<int>(index - first);
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
            increments.at(index) > 0 ? QString::number(increments.at(index)) : QString(),
            QString::number(qso.points)};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                item->setData(Qt::UserRole, qso.id);
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            } else if (column == 4 || column == 9) {
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
            if (column == 8 && increments.at(index) > 0) {
                item->setForeground(multColor);
            }
            m_recentQsos->setItem(row, column, item);
        }
    }
    m_recentQsos->resizeColumnsToContents();
    {
        const QFontMetrics metrics(m_recentQsos->font());
        static const int minimumChars[] = {5, 5, 5, 4, 8, 11, 8, 10, 6, 4};
        for (int column = 0; column < 10; ++column) {
            const int minimum = metrics.horizontalAdvance(QLatin1Char('W'))
                                    * minimumChars[column] + 8;
            if (m_recentQsos->columnWidth(column) < minimum) {
                m_recentQsos->setColumnWidth(column, minimum);
            }
        }
    }
    m_recentQsos->setUpdatesEnabled(true);
    m_recentQsos->scrollToBottom();

    refreshScoreSummary();
    refreshRate();
    updateTopStatus();
    refreshCheckWindows();
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
    int lastHour = 0;
    int lastTenMinutes = 0;
    for (auto it = m_logQsos.crbegin(); it != m_logQsos.crend(); ++it) {
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
        count = qMin(count, m_logQsos.size());
        if (count == 0) {
            return 0LL;
        }
        const qint64 seconds =
            qMax<qint64>(60, m_logQsos.at(m_logQsos.size() - count).timestampUtc.secsTo(now));
        return static_cast<long long>(count * 3600 / seconds);
    };
    m_rateLabel->setText(
        tr("Last 10 QSOs  : %1 /h\n"
           "Last 100 QSOs : %2 /h\n"
           "Last 60 min   : %3 QSO\n"
           "Last 10 min   : %4 QSO\n"
           "Total QSOs    : %5\n"
           "CW speed      : %6 WPM")
            .arg(rateOfLast(10), 5)
            .arg(rateOfLast(100), 5)
            .arg(lastHour, 5)
            .arg(lastTenMinutes, 5)
            .arg(m_logQsos.size(), 5)
            .arg(m_cwWpmSpin->value(), 5));
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
    QString text = QStringLiteral("%1z  %2%3  │  %4  │  QSO %5  PTS %6")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")),
             station,
             m_station.operatorName.isEmpty()
                 ? QString() : QStringLiteral(" (%1)").arg(m_station.operatorName),
             contest)
        .arg(m_logQsos.size())
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

void MainWindow::refreshCheckWindows()
{
    if (m_checkPartial == nullptr) {
        return;
    }
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
    int workedCount = 0;
    for (const Qso &qso : std::as_const(m_logQsos)) {
        if (!callsign.isEmpty() && qso.callsign.compare(callsign, Qt::CaseInsensitive) == 0) {
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
            const QString time =
                worked.value(bands.at(row) + QLatin1Char('|') + modes.at(column));
            auto *item = new QTableWidgetItem(time.isEmpty() ? QStringLiteral("·") : time);
            item->setTextAlignment(Qt::AlignCenter);
            const bool currentCell = currentBand && modes.at(column) == mode;
            if (!time.isEmpty() && currentCell) {
                item->setBackground(QColor(QStringLiteral("#CE0000")));
                item->setForeground(Qt::white);
            } else if (!time.isEmpty()) {
                item->setForeground(QColor(QStringLiteral("#000080")));
            } else if (currentBand) {
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
    const auto definition = m_definitions.constFind(contestId);
    if (definition != m_definitions.cend()) {
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
                            receivedFields, context, std::nullopt,
                            sentOverrides);
        if (evaluation.duplicate) {
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
        points = evaluation.points;
        for (const QString &key : evaluation.newMultiplierKeys) {
            QString label = key.section(QLatin1Char(':'), -1);
            newMultipliers.append(label.section(QLatin1Char('@'), 0, 0));
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
            if (previous.callsign.compare(callsign, Qt::CaseInsensitive) == 0
                && previous.band == bandSelector->currentText().trimmed().toLower()
                && previous.mode == modeSelector->currentText().trimmed().toUpper()) {
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
    if (!m_repository.saveQso(qso, &error)) {
        QMessageBox::critical(this, tr("Unable to save QSO"), error);
        return;
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
    if (!newMultipliers.isEmpty()) {
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
                                         sentFields);
            for (const ContestValidationIssue &issue : evaluation.issues) {
                problems.append(QStringLiteral("%1: %2").arg(issue.field, issue.message));
            }
            if (evaluation.duplicate) {
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
            candidate.points = evaluation.points;
        } else {
            for (const Qso &other : std::as_const(others)) {
                if (other.callsign.compare(candidate.callsign, Qt::CaseInsensitive) == 0
                    && other.band.compare(candidate.band, Qt::CaseInsensitive) == 0
                    && other.mode.compare(candidate.mode, Qt::CaseInsensitive) == 0) {
                    problems.append(tr("%1 is already logged on %2 %3 (dupe).")
                                        .arg(candidate.callsign, candidate.band,
                                             candidate.mode));
                    return problems;
                }
            }
            candidate.sentExchange = sentEdits.value(QString())->text().trimmed();
            candidate.receivedExchange = receivedEdits.value(QString())->text().trimmed();
        }
        edited = candidate;
        return {};
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QStringList problems = validate();
        if (problems.isEmpty()) {
            dialog.accept();
            return;
        }
        errorLabel->setText(problems.join(QLatin1Char('\n')));
        errorLabel->show();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!m_repository.updateQso(edited, &error)) {
        QMessageBox::critical(this, tr("Unable to update QSO"), error);
        return;
    }
    refreshRecentQsos();
    updateSerialFields();
    updateDupeIndicator();
    showOperatorMessage(tr("QSO #%1 %2 updated").arg(edited.id).arg(edited.callsign));
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
            if (qso.callsign.compare(callsign, Qt::CaseInsensitive) != 0) {
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
    m_radioSnapshots[radio] = snapshot;
    m_radioSnapshotTimes[radio] = QDateTime::currentDateTimeUtc();
    bool frequencyChanged = false;
    if (previousSnapshot.state == ConnectionState::Connected
        && snapshot.state == ConnectionState::Connected) {
        if (!previousSnapshot.vfos.isEmpty() && !snapshot.vfos.isEmpty()) {
            for (const Vfo &vfo : snapshot.vfos) {
                const auto previousVfo = std::find_if(
                    previousSnapshot.vfos.cbegin(), previousSnapshot.vfos.cend(),
                    [&vfo](const Vfo &candidate) { return candidate.number == vfo.number; });
                if (previousVfo != previousSnapshot.vfos.cend()
                    && previousVfo->frequencyHz > 0 && vfo.frequencyHz > 0
                    && previousVfo->frequencyHz != vfo.frequencyHz) {
                    frequencyChanged = true;
                    break;
                }
            }
        } else if (previousSnapshot.focusVfo == snapshot.focusVfo
                   && previousSnapshot.frequencyHz > 0 && snapshot.frequencyHz > 0) {
            frequencyChanged = previousSnapshot.frequencyHz != snapshot.frequencyHz;
        }
    }
    if (frequencyChanged && m_runModeSelector->currentData().toInt() == 0) {
        setRunMode(1);
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

void MainWindow::updateDxClusterStatus(const DxClusterStatus &status)
{
    m_dxClusterConnected = status.state == ConnectionState::Connected;
    if (!m_dxClusterConnected && m_clusterLog != nullptr) {
        m_clusterLog->setRowCount(0);
    }

    QString state;
    switch (status.state) {
    case ConnectionState::Disconnected: state = tr("Disconnected"); break;
    case ConnectionState::Connecting: state = tr("Connecting"); break;
    case ConnectionState::Connected: state = tr("Connected"); break;
    case ConnectionState::Error: state = tr("Error"); break;
    case ConnectionState::Unsupported: state = tr("Unsupported"); break;
    }
    m_clusterLabel->setText(tr("DX cluster: %1 — %2").arg(state, status.message));
    m_clusterDock->setWindowTitle(tr("DX Cluster announcements — %1").arg(state));
    m_clusterLabel->setStyleSheet(
        status.state == ConnectionState::Error
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

    const auto populate = [this, &ordered, &isWorked, &loggedCalls, &now, sortByTime,
                           spotLifetimeMinutes](
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
        const QString filterMode = m_bandmapModeFilter->currentData().toString();
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
                && spot.mode.compare(filterMode, Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString dedupeKey =
                spot.callsign.toUpper() + QLatin1Char('|') + spot.band;
            if (displayed.contains(dedupeKey)) {
                continue;
            }
            displayed.insert(dedupeKey);
            const bool worked = isWorked(spot);
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
