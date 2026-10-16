#include "logger/radio/radio_service.h"

#include <QTimer>

#include <memory>

#include <mutex>

#ifdef LOGGER_WITH_HAMLIB
#include <hamlib/rig.h>
#endif

namespace logger {

class RadioWorker::Backend {
public:
    virtual ~Backend() = default;
    virtual bool open(const RadioConfig &config, QString *error) = 0;
    virtual void close() = 0;
    virtual bool read(RadioSnapshot &snapshot, QString *error) = 0;
    virtual bool setFrequency(qint64 frequencyHz, int vfo, QString *error) = 0;
    virtual bool setVfoFocus(int vfo, QString *error) = 0;
};

namespace {

QString serialControlStateName(SerialControlState state)
{
    switch (state) {
    case SerialControlState::Unchanged:
        return QStringLiteral("UNSET");
    case SerialControlState::On:
        return QStringLiteral("ON");
    case SerialControlState::Off:
        return QStringLiteral("OFF");
    }
    return QStringLiteral("UNSET");
}

QString serialParityName(SerialParity parity)
{
    switch (parity) {
    case SerialParity::None: return QStringLiteral("None");
    case SerialParity::Odd: return QStringLiteral("Odd");
    case SerialParity::Even: return QStringLiteral("Even");
    case SerialParity::Mark: return QStringLiteral("Mark");
    case SerialParity::Space: return QStringLiteral("Space");
    }
    return QStringLiteral("None");
}

QString serialHandshakeName(SerialHandshake handshake)
{
    switch (handshake) {
    case SerialHandshake::None: return QStringLiteral("None");
    case SerialHandshake::XonXoff: return QStringLiteral("XONXOFF");
    case SerialHandshake::Hardware: return QStringLiteral("Hardware");
    }
    return QStringLiteral("None");
}

class MockBackend final : public RadioWorker::Backend {
public:
    bool open(const RadioConfig &config, QString *) override
    {
        m_dualReceiveSupported = config.mockDualReceiveSupported;
        m_open = true;
        return true;
    }

    void close() override { m_open = false; }

    bool read(RadioSnapshot &snapshot, QString *error) override
    {
        if (!m_open) {
            if (error != nullptr) {
                *error = QStringLiteral("Mock radio is not connected.");
            }
            return false;
        }
        snapshot.dualVfoSupported = true;
        snapshot.dualReceiveSupported = m_dualReceiveSupported;
        snapshot.focusVfo = m_focusVfo;
        snapshot.vfos = {
            {QStringLiteral("A"), 0, m_frequencyHz[0], QStringLiteral("CW"),
             ConnectionState::Connected},
            {QStringLiteral("B"), 1, m_frequencyHz[1], QStringLiteral("CW"),
             ConnectionState::Connected}
        };
        snapshot.frequencyHz = m_frequencyHz[m_focusVfo];
        snapshot.mode = QStringLiteral("CW");
        return true;
    }

    bool setFrequency(qint64 frequencyHz, int vfo, QString *error) override
    {
        if (!m_open || frequencyHz <= 0 || vfo < 0 || vfo > 1) {
            if (error != nullptr) {
                *error = QStringLiteral("Mock radio cannot tune to this frequency.");
            }
            return false;
        }
        m_frequencyHz[vfo] = frequencyHz;
        return true;
    }

    bool setVfoFocus(int vfo, QString *error) override
    {
        if (!m_open || vfo < 0 || vfo > 1) {
            if (error != nullptr) {
                *error = QStringLiteral("Mock radio has no requested VFO.");
            }
            return false;
        }
        m_focusVfo = vfo;
        return true;
    }

private:
    bool m_open = false;
    qint64 m_frequencyHz[2]{14'025'000, 7'025'000};
    int m_focusVfo = 0;
    bool m_dualReceiveSupported = true;
};

#ifdef LOGGER_WITH_HAMLIB
void loadHamlibBackends()
{
    static std::once_flag backendsLoaded;
    std::call_once(backendsLoaded, [] { rig_load_all_backends(); });
}

int collectRadioModel(const struct rig_caps *caps, rig_ptr_t data)
{
    if (caps == nullptr || data == nullptr) {
        return 1;
    }
    auto *models = static_cast<QVector<RadioModel> *>(data);
    models->append({static_cast<int>(caps->rig_model),
                    QString::fromLocal8Bit(caps->model_name)});
    return 1;
}

QString modeName(rmode_t mode)
{
    if (mode & RIG_MODE_CW) return QStringLiteral("CW");
    if (mode & RIG_MODE_CWR) return QStringLiteral("CWR");
    if (mode & RIG_MODE_USB) return QStringLiteral("USB");
    if (mode & RIG_MODE_LSB) return QStringLiteral("LSB");
    if (mode & RIG_MODE_FM) return QStringLiteral("FM");
    if (mode & RIG_MODE_AM) return QStringLiteral("AM");
    if (mode & RIG_MODE_RTTY) return QStringLiteral("RTTY");
    if (mode & RIG_MODE_PKTUSB) return QStringLiteral("DATA");
    return QStringLiteral("Unknown");
}

class HamlibBackend final : public RadioWorker::Backend {
public:
    ~HamlibBackend() override { close(); }

    bool open(const RadioConfig &config, QString *error) override
    {
        loadHamlibBackends();

        if (config.modelId <= 0) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib model ID must be a positive number.");
            }
            return false;
        }
        if (config.transport == RadioTransport::Tcp && config.modelId != 2) {
            if (error != nullptr) {
                *error = QStringLiteral("TCP / rigctld transport requires Hamlib model 2 (NET rigctl).");
            }
            return false;
        }
        if (config.transport == RadioTransport::Serial && config.modelId == 2) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib model 2 (NET rigctl) requires TCP / rigctld transport.");
            }
            return false;
        }
        if (config.transport == RadioTransport::Serial && config.devicePath.trimmed().isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("A serial device path is required (for example /dev/ttyUSB0 or COM3).");
            }
            return false;
        }
        if (config.transport == RadioTransport::Tcp
            && (config.host.trimmed().isEmpty() || config.port < 1 || config.port > 65535)) {
            if (error != nullptr) {
                *error = QStringLiteral("A valid rigctld host and TCP port (1–65535) are required.");
            }
            return false;
        }

        m_rig = rig_init(static_cast<rig_model_t>(config.modelId));
        if (m_rig == nullptr || m_rig->caps == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib does not recognize model ID %1 or has no capabilities for it.")
                             .arg(config.modelId);
            }
            close();
            return false;
        }

        if (config.transport == RadioTransport::Tcp) {
            if (!setConfiguration(QStringLiteral("rig_pathname"),
                                  QStringLiteral("%1:%2").arg(config.host).arg(config.port),
                                  error, true)
                || !setConfiguration(QStringLiteral("timeout"), QString::number(qMax(1, config.timeoutMs)), error)) {
                close();
                return false;
            }
        } else {
            const int serialMin = m_rig->caps->serial_rate_min;
            const int serialMax = m_rig->caps->serial_rate_max;
            if (serialMin > 0 && serialMax > 0
                && (config.baudRate < serialMin || config.baudRate > serialMax)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Baud rate %1 is outside this radio's supported range (%2–%3).")
                                 .arg(config.baudRate).arg(serialMin).arg(serialMax);
                }
                close();
                return false;
            }
            if (config.baudRate <= 0) {
                if (error != nullptr) {
                    *error = QStringLiteral("Serial baud rate must be a positive number.");
                }
                close();
                return false;
            }
            const bool hasSerialRateRange = serialMin > 0 && serialMax >= serialMin;
            if (!setConfiguration(QStringLiteral("rig_pathname"), config.devicePath,
                                  error, true)
                || !setConfiguration(QStringLiteral("serial_speed"),
                                     QString::number(config.baudRate), error,
                                     hasSerialRateRange)
                || !setConfiguration(QStringLiteral("data_bits"),
                                     QString::number(config.dataBits), error,
                                     config.dataBits != 8)
                || !setConfiguration(QStringLiteral("stop_bits"),
                                     QString::number(config.stopBits), error,
                                     config.stopBits != 1)
                || !setConfiguration(QStringLiteral("serial_parity"),
                                     serialParityName(config.parity), error,
                                     config.parity != SerialParity::None)
                || !setConfiguration(QStringLiteral("serial_handshake"),
                                     serialHandshakeName(config.handshake), error,
                                     config.handshake != SerialHandshake::None)
                || (config.rtsState != SerialControlState::Unchanged
                    && !setConfiguration(QStringLiteral("rts_state"),
                                         serialControlStateName(config.rtsState), error))
                || (config.dtrState != SerialControlState::Unchanged
                    && !setConfiguration(QStringLiteral("dtr_state"),
                                         serialControlStateName(config.dtrState), error))
                || !setConfiguration(QStringLiteral("timeout"), QString::number(qMax(1, config.timeoutMs)), error)) {
                close();
                return false;
            }
        }

        const int result = rig_open(m_rig);
        if (result != RIG_OK) {
            if (error != nullptr) {
                *error = QString::fromLocal8Bit(rigerror(result));
            }
            close();
            return false;
        }
        m_open = true;
        const int vfoList = m_rig->state.vfo_list;
        m_dualVfoSupported =
            (vfoList & (RIG_VFO_A | RIG_VFO_B)) == (RIG_VFO_A | RIG_VFO_B);
        m_dualReceiveSupported = rig_has_get_func(m_rig, RIG_FUNC_DUAL_WATCH)
                                     || rig_has_set_func(m_rig, RIG_FUNC_DUAL_WATCH)
            || ((vfoList & (RIG_VFO_MAIN | RIG_VFO_SUB))
                == (RIG_VFO_MAIN | RIG_VFO_SUB))
            || (vfoList & (RIG_VFO_SUB_A | RIG_VFO_SUB_B)) != 0;
        if (m_dualVfoSupported) {
            const int aResult = rig_get_freq(m_rig, RIG_VFO_A, &m_frequencies[0]);
            const int bResult = rig_get_freq(m_rig, RIG_VFO_B, &m_frequencies[1]);
            m_dualVfoSupported = aResult == RIG_OK && bResult == RIG_OK;
        }
        return true;
    }

    void close() override
    {
        if (m_rig == nullptr) {
            return;
        }
        if (m_open) {
            rig_close(m_rig);
            m_open = false;
        }
        rig_cleanup(m_rig);
        m_rig = nullptr;
    }

    bool read(RadioSnapshot &snapshot, QString *error) override
    {
        if (m_rig == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib radio is not initialized.");
            }
            return false;
        }
        snapshot.dualVfoSupported = m_dualVfoSupported;
        snapshot.dualReceiveSupported = m_dualReceiveSupported;
        snapshot.focusVfo = m_focusVfo;
        const int count = m_dualVfoSupported ? 2 : 1;
        snapshot.vfos.clear();
        for (int index = 0; index < count; ++index) {
            const vfo_t vfo = vfoId(index);
            freq_t frequency = 0;
            rmode_t mode = RIG_MODE_NONE;
            pbwidth_t passband = 0;
            const int freqResult = rig_get_freq(m_rig, vfo, &frequency);
            const int modeResult = rig_get_mode(m_rig, vfo, &mode, &passband);
            if (freqResult != RIG_OK || modeResult != RIG_OK) {
                if (error != nullptr) {
                    const int result = freqResult != RIG_OK ? freqResult : modeResult;
                    *error = QString::fromLocal8Bit(rigerror(result));
                }
                return false;
            }
            m_frequencies[index] = frequency;
            snapshot.vfos.append({
                index == 0 ? QStringLiteral("A") : QStringLiteral("B"),
                index, static_cast<qint64>(frequency), modeName(mode),
                ConnectionState::Connected
            });
        }
        snapshot.frequencyHz =
            snapshot.vfos.at(qMin(m_focusVfo, snapshot.vfos.size() - 1)).frequencyHz;
        snapshot.mode =
            snapshot.vfos.at(qMin(m_focusVfo, snapshot.vfos.size() - 1)).mode;
        return true;
    }

    bool setFrequency(qint64 frequencyHz, int vfo, QString *error) override
    {
        if (m_rig == nullptr || !m_open || frequencyHz <= 0 || vfo < 0
            || vfo > (m_dualVfoSupported ? 1 : 0)) {
            if (error != nullptr) {
                *error = QStringLiteral("Radio is not connected or frequency is invalid.");
            }
            return false;
        }
        const int result = rig_set_freq(m_rig, vfoId(vfo),
                                        static_cast<freq_t>(frequencyHz));
        if (result != RIG_OK) {
            if (error != nullptr) {
                *error = QString::fromLocal8Bit(rigerror(result));
            }
            return false;
        }
        return true;
    }

    bool setVfoFocus(int vfo, QString *error) override
    {
        if (!m_open || vfo < 0 || vfo > 1 || (vfo == 1 && !m_dualVfoSupported)) {
            if (error != nullptr) {
                *error = QStringLiteral("Selected radio does not support the requested VFO.");
            }
            return false;
        }
        const int result = rig_set_vfo(m_rig, vfoId(vfo));
        if (result != RIG_OK) {
            if (error != nullptr) {
                *error = QString::fromLocal8Bit(rigerror(result));
            }
            return false;
        }
        m_focusVfo = vfo;
        return true;
    }

private:
    vfo_t vfoId(int index) const
    {
        return index == 0 ? RIG_VFO_A : RIG_VFO_B;
    }

    bool setConfiguration(const QString &name, const QString &value, QString *error,
                          bool required = false)
    {
        if (m_rig == nullptr) {
            return false;
        }
        const QByteArray key = name.toLocal8Bit();
        const QByteArray setting = value.toLocal8Bit();
        const hamlib_token_t token = rig_token_lookup(m_rig, key.constData());
        if (token == RIG_CONF_END) {
            if (required && error != nullptr) {
                *error = QStringLiteral("Hamlib model %1 does not expose required setting '%2'.")
                             .arg(m_rig->caps != nullptr
                                      ? QString::fromLocal8Bit(m_rig->caps->model_name)
                                      : QStringLiteral("unknown"), name);
            }
            return !required;
        }
        const int result = rig_set_conf(m_rig, token, setting.data());
        if (result != RIG_OK) {
            if (error != nullptr) {
                *error = QString::fromLocal8Bit(rigerror(result));
            }
            return false;
        }
        return true;
    }

    RIG *m_rig = nullptr;
    bool m_open = false;
    bool m_dualVfoSupported = false;
    bool m_dualReceiveSupported = false;
    int m_focusVfo = 0;
    freq_t m_frequencies[2]{};
};
#endif

} // namespace

QVector<RadioModel> availableRadioModels()
{
#ifdef LOGGER_WITH_HAMLIB
    loadHamlibBackends();
    QVector<RadioModel> models;
    rig_list_foreach(collectRadioModel, &models);
    std::sort(models.begin(), models.end(), [](const RadioModel &left,
                                               const RadioModel &right) {
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });
    return models;
#else
    return {};
#endif
}

RadioProbeResult probeRadio(const RadioConfig &config, int timeoutMs)
{
    RadioProbeResult result;
    std::unique_ptr<RadioWorker::Backend> backend;
    if (config.backend == RadioBackendType::Mock) {
        backend = std::make_unique<MockBackend>();
    } else {
#ifdef LOGGER_WITH_HAMLIB
        backend = std::make_unique<HamlibBackend>();
#else
        result.message = QStringLiteral("This build does not include Hamlib support.");
        return result;
#endif
    }

    RadioConfig probeConfig = config;
    probeConfig.timeoutMs = qMax(1, timeoutMs);
    QString error;
    if (!backend->open(probeConfig, &error)) {
        result.message = error.isEmpty()
            ? QStringLiteral("Unable to open the radio connection.") : error;
        backend->close();
        return result;
    }

    RadioSnapshot snapshot;
    if (!backend->read(snapshot, &error)) {
        result.message = error.isEmpty()
            ? QStringLiteral("The radio opened, but frequency and mode could not be read.")
            : QStringLiteral("Radio opened, but reading status failed: %1").arg(error);
        backend->close();
        return result;
    }
    backend->close();
    result.ok = true;
    result.frequencyHz = snapshot.frequencyHz;
    result.mode = snapshot.mode;
    result.vfoCount = snapshot.vfos.size();
    result.dualVfo = snapshot.dualVfoSupported;
    result.dualReceive = snapshot.dualReceiveSupported;
    result.message = QStringLiteral(
                         "Connected: %1 Hz, %2; %3 VFO%4; dual VFO access %5; dual receive %6.")
                         .arg(result.frequencyHz)
                         .arg(result.mode)
                         .arg(result.vfoCount)
                         .arg(result.vfoCount == 1 ? QString() : QStringLiteral("s"))
                         .arg(result.dualVfo ? QStringLiteral("supported")
                                             : QStringLiteral("not detected"))
                         .arg(result.dualReceive ? QStringLiteral("supported")
                                                 : QStringLiteral("not detected"));
    return result;
}

RadioWorker::RadioWorker(QObject *parent)
    : QObject(parent)
{
}

RadioWorker::~RadioWorker()
{
    disconnectRadio();
}

void RadioWorker::connectRadio(const RadioConfig &config)
{
    disconnectRadio();
    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Connecting;
    snapshot.message = tr("Connecting to radio...");
    emit snapshotChanged(snapshot);

    if (config.backend == RadioBackendType::Mock) {
        m_backend = new MockBackend;
    } else {
#ifdef LOGGER_WITH_HAMLIB
        m_backend = new HamlibBackend;
#else
        snapshot.state = ConnectionState::Unsupported;
        snapshot.message = tr("This build does not include Hamlib support.");
        emit snapshotChanged(snapshot);
        return;
#endif
    }

    QString error;
    if (!m_backend->open(config, &error)) {
        delete m_backend;
        m_backend = nullptr;
        snapshot.state = ConnectionState::Error;
        snapshot.message = error;
        emit snapshotChanged(snapshot);
        return;
    }

    snapshot.state = ConnectionState::Connected;
    snapshot.message = tr("Radio connected.");
    if (!m_backend->read(snapshot, &error)) {
        snapshot.state = ConnectionState::Error;
        snapshot.message = error;
        m_backend->close();
        delete m_backend;
        m_backend = nullptr;
        emit snapshotChanged(snapshot);
        return;
    }
    emit snapshotChanged(snapshot);

    if (m_pollTimer == nullptr) {
        m_pollTimer = new QTimer(this);
        connect(m_pollTimer, &QTimer::timeout, this, &RadioWorker::pollRadio);
    }
    m_pollTimer->setInterval(qBound(100, config.pollIntervalMs, 10000));
    m_pollTimer->start();
}

void RadioWorker::disconnectRadio()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    if (m_backend != nullptr) {
        m_backend->close();
        delete m_backend;
        m_backend = nullptr;
    }
    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Disconnected;
    snapshot.message = tr("Radio disconnected.");
    emit snapshotChanged(snapshot);
}

void RadioWorker::setFrequency(qint64 frequencyHz, int vfo)
{
    RadioSnapshot snapshot;
    QString error;
    if (m_backend == nullptr) {
        snapshot.state = ConnectionState::Disconnected;
        snapshot.message = tr("Cannot tune: radio is disconnected.");
    } else if (!m_backend->setFrequency(frequencyHz, vfo, &error)) {
        snapshot.state = ConnectionState::Error;
        snapshot.message = tr("Unable to tune radio: %1").arg(error);
    } else if (!m_backend->read(snapshot, &error)) {
        snapshot.state = ConnectionState::Error;
        snapshot.message = error;
    } else {
        snapshot.state = ConnectionState::Connected;
        snapshot.message = tr("Radio tuned.");
    }
    emit snapshotChanged(snapshot);
}

void RadioWorker::setVfoFocus(int vfo)
{
    RadioSnapshot snapshot;
    QString error;
    if (m_backend == nullptr) {
        snapshot.state = ConnectionState::Disconnected;
        snapshot.message = tr("Cannot change VFO focus: radio is disconnected.");
    } else if (!m_backend->setVfoFocus(vfo, &error)) {
        snapshot.state = ConnectionState::Error;
        snapshot.message = error;
    } else if (!m_backend->read(snapshot, &error)) {
        snapshot.state = ConnectionState::Error;
        snapshot.message = error;
    } else {
        snapshot.state = ConnectionState::Connected;
        snapshot.message = tr("RX focus changed to VFO %1.").arg(vfo == 0 ? 'A' : 'B');
    }
    emit snapshotChanged(snapshot);
}

void RadioWorker::pollRadio()
{
    if (m_backend == nullptr) {
        return;
    }
    RadioSnapshot snapshot;
    snapshot.state = ConnectionState::Connected;
    QString error;
    if (!m_backend->read(snapshot, &error)) {
        snapshot.state = ConnectionState::Error;
        snapshot.message = error;
        m_pollTimer->stop();
        m_backend->close();
        delete m_backend;
        m_backend = nullptr;
    }
    emit snapshotChanged(snapshot);
}

RadioController::RadioController(QObject *parent)
    : QObject(parent)
    , m_worker(new RadioWorker)
{
    qRegisterMetaType<RadioConfig>();
    qRegisterMetaType<RadioSnapshot>();
    m_worker->moveToThread(&m_thread);
    connect(this, &RadioController::connectRequested,
            m_worker, &RadioWorker::connectRadio, Qt::QueuedConnection);
    connect(this, &RadioController::disconnectRequested,
            m_worker, &RadioWorker::disconnectRadio, Qt::QueuedConnection);
    connect(this, &RadioController::tuneRequested,
            m_worker, &RadioWorker::setFrequency, Qt::QueuedConnection);
    connect(this, &RadioController::focusRequested,
            m_worker, &RadioWorker::setVfoFocus, Qt::QueuedConnection);
    connect(m_worker, &RadioWorker::snapshotChanged,
            this, &RadioController::snapshotChanged, Qt::QueuedConnection);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

RadioController::~RadioController()
{
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "disconnectRadio",
                                  Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    m_worker = nullptr;
}

void RadioController::connectRadio(const RadioConfig &config)
{
    emit connectRequested(config);
}

void RadioController::disconnectRadio()
{
    emit disconnectRequested();
}

void RadioController::setFrequency(qint64 frequencyHz, int vfo)
{
    emit tuneRequested(frequencyHz, vfo);
}

void RadioController::setVfoFocus(int vfo)
{
    emit focusRequested(vfo);
}

} // namespace logger
