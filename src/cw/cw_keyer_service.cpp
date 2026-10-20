#include "logger/cw/cw_keyer_service.h"

#include <QTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QUdpSocket>

#include <memory>
#include <mutex>

#ifdef LOGGER_WITH_HAMLIB
#include <hamlib/rig.h>
#endif

namespace logger {
namespace {

class MockCwKeyer final : public ICwKeyer {
public:
    bool connect(const CwKeyerConfig &config, QString *) override
    {
        m_wpm = config.wpm;
        m_completionDelayMs = config.mockCompletionDelayMs;
        m_never = config.mockCompletionNever;
        m_connected = true;
        return true;
    }

    void disconnect() override
    {
        m_connected = false;
        m_pending = false;
    }

    bool sendText(const QString &text, int, QString *error) override
    {
        if (!m_connected || text.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("Mock keyer is disconnected or message is empty.");
            }
            return false;
        }
        m_lastMessage = text;
        m_pending = true;
        m_sentAt.start();
        return true;
    }

    bool abort(QString *) override
    {
        m_lastMessage.clear();
        m_pending = false;
        return m_connected;
    }

    bool supportsCompletion() const override
    {
        return m_never || m_completionDelayMs >= 0;
    }

    bool pollCompleted(QString *error) override
    {
        Q_UNUSED(error)
        if (!m_pending || m_never || m_completionDelayMs < 0
            || m_sentAt.elapsed() < m_completionDelayMs) {
            return false;
        }
        m_pending = false;
        return true;
    }

    bool setWpm(int wpm, QString *error) override
    {
        if (!m_connected || wpm < 5 || wpm > 80) {
            if (error != nullptr) {
                *error = QStringLiteral("WPM must be between 5 and 80.");
            }
            return false;
        }
        m_wpm = wpm;
        return true;
    }

private:
    bool m_connected = false;
    bool m_pending = false;
    bool m_never = false;
    int m_completionDelayMs = -1;
    QElapsedTimer m_sentAt;
    int m_wpm = 25;
    QString m_lastMessage;
};

class CwdaemonKeyer final : public ICwKeyer {
public:
    bool connect(const CwKeyerConfig &config, QString *error) override
    {
        if (config.host.trimmed().isEmpty() || config.port < 1
            || config.port > 65535) {
            if (error != nullptr) {
                *error = QStringLiteral("cwdaemon host and UDP port are invalid.");
            }
            return false;
        }
        m_host = config.host.trimmed();
        if (!m_address.setAddress(m_host)) {
            const QHostInfo info = QHostInfo::fromName(m_host);
            if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
                if (error != nullptr) {
                    *error = info.errorString().isEmpty()
                        ? QStringLiteral("cwdaemon host could not be resolved.")
                        : info.errorString();
                }
                return false;
            }
            m_address = info.addresses().first();
        }
        m_port = static_cast<quint16>(config.port);
        m_wpm = config.wpm;
        m_replyEnabled = config.cwdaemonCompletionReply;
        m_pendingToken.clear();
        m_socket = std::make_unique<QUdpSocket>();
        m_connected = true;
        return true;
    }

    void disconnect() override
    {
        m_connected = false;
        m_pendingToken.clear();
        m_socket.reset();
    }

    bool sendText(const QString &text, int transmitVfo, QString *error) override
    {
        if (transmitVfo != 0) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "cwdaemon cannot select a transmit VFO; VFO B transmission is disabled.");
            }
            return false;
        }
        if (!m_connected || text.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("cwdaemon is not configured or message is empty.");
            }
            return false;
        }
        if (!m_replyEnabled) {
            return sendDatagram(text.toUtf8(), error);
        }
        // Discard stale replies, then ask cwdaemon to answer "h<token>\r\n"
        // once the following text has been played.
        drainReplies();
        const QByteArray token = QByteArray::number(++m_tokenCounter);
        QByteArray request(1, char(27));
        request.append('h');
        request.append(token);
        if (!sendDatagram(request, error) || !sendDatagram(text.toUtf8(), error)) {
            return false;
        }
        m_pendingToken = token;
        return true;
    }

    bool abort(QString *error) override
    {
        m_pendingToken.clear();
        drainReplies();
        return m_connected && sendDatagram(QByteArray("\x1b" "7", 2), error);
    }

    bool supportsCompletion() const override { return m_replyEnabled; }

    bool pollCompleted(QString *error) override
    {
        Q_UNUSED(error)
        if (m_socket == nullptr || m_pendingToken.isEmpty()) {
            return false;
        }
        while (m_socket->hasPendingDatagrams()) {
            QByteArray reply;
            reply.resize(static_cast<qsizetype>(m_socket->pendingDatagramSize()));
            m_socket->readDatagram(reply.data(), reply.size());
            reply = reply.trimmed();
            if (reply == "h" + m_pendingToken) {
                m_pendingToken.clear();
                return true;
            }
        }
        return false;
    }

    bool setWpm(int wpm, QString *error) override
    {
        if (!m_connected || wpm < 5 || wpm > 80) {
            if (error != nullptr) {
                *error = QStringLiteral("WPM must be between 5 and 80.");
            }
            return false;
        }
        m_wpm = wpm;
        QByteArray command(1, char(27));
        command.append('2');
        command.append(QByteArray::number(wpm));
        return sendDatagram(command, error);
    }

private:
    void drainReplies()
    {
        while (m_socket != nullptr && m_socket->hasPendingDatagrams()) {
            m_socket->readDatagram(nullptr, 0);
        }
    }

    bool sendDatagram(const QByteArray &payload, QString *error)
    {
        if (m_socket == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("cwdaemon UDP socket is unavailable.");
            }
            return false;
        }
        const qint64 written =
            m_socket->writeDatagram(payload, m_address, m_port);
        if (written != payload.size()) {
            if (error != nullptr) {
                *error = m_socket->errorString();
            }
            return false;
        }
        return true;
    }

    QString m_host;
    QHostAddress m_address;
    quint16 m_port = 6789;
    int m_wpm = 25;
    bool m_connected = false;
    bool m_replyEnabled = false;
    int m_tokenCounter = 0;
    QByteArray m_pendingToken;
    std::unique_ptr<QUdpSocket> m_socket;
};

#ifdef LOGGER_WITH_HAMLIB
class HamlibCwKeyer final : public ICwKeyer {
public:
    ~HamlibCwKeyer() override { disconnect(); }

    bool connect(const CwKeyerConfig &config, QString *error) override
    {
        static std::once_flag backendsLoaded;
        std::call_once(backendsLoaded, [] { rig_load_all_backends(); });
        m_rig = rig_init(static_cast<rig_model_t>(config.radio.modelId));
        if (m_rig == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib does not recognize model ID %1.")
                             .arg(config.radio.modelId);
            }
            return false;
        }
        if (m_rig->caps == nullptr || m_rig->caps->send_morse == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Selected Hamlib model does not support CW text.");
            }
            disconnect();
            return false;
        }
        if (config.radio.transport == RadioTransport::Tcp) {
            if (!setConf(QStringLiteral("rig_pathname"),
                         QStringLiteral("%1:%2").arg(config.radio.host)
                             .arg(config.radio.port), error)) {
                disconnect();
                return false;
            }
        } else {
            if (config.radio.devicePath.trimmed().isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("A serial device path is required.");
                }
                disconnect();
                return false;
            }
            if (!setConf(QStringLiteral("rig_pathname"), config.radio.devicePath,
                         error)
                || !setConf(QStringLiteral("serial_speed"),
                            QString::number(config.radio.baudRate), error)
                || (config.radio.rtsState != SerialControlState::Unchanged
                    && !setConf(QStringLiteral("rts_state"),
                                config.radio.rtsState == SerialControlState::On
                                    ? QStringLiteral("ON") : QStringLiteral("OFF"),
                                error))
                || (config.radio.dtrState != SerialControlState::Unchanged
                    && !setConf(QStringLiteral("dtr_state"),
                                config.radio.dtrState == SerialControlState::On
                                    ? QStringLiteral("ON") : QStringLiteral("OFF"),
                                error))) {
                disconnect();
                return false;
            }
        }
        const int result = rig_open(m_rig);
        if (result != RIG_OK) {
            if (error != nullptr) {
                *error = QString::fromLocal8Bit(rigerror(result));
            }
            disconnect();
            return false;
        }
        m_open = true;
        m_completionPollingReady = m_rig->caps->wait_morse == rig_wait_morse
            && rig_set_cache_timeout_ms(m_rig, HAMLIB_CACHE_PTT, 0) == RIG_OK;
        return true;
    }

    void disconnect() override
    {
        if (m_rig == nullptr) {
            return;
        }
        if (m_open) {
            rig_stop_morse(m_rig, m_transmitVfo);
            m_completionPending = false;
            rig_close(m_rig);
            m_open = false;
        }
        m_completionPollingReady = false;
        rig_cleanup(m_rig);
        m_rig = nullptr;
    }

    bool sendText(const QString &text, int transmitVfo, QString *error) override
    {
        if (!m_open || text.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib keyer is not connected or message is empty.");
            }
            return false;
        }
        const QByteArray bytes = text.toUtf8();
        if (transmitVfo < 0 || transmitVfo > 1) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib transmit VFO must be A or B.");
            }
            return false;
        }
        m_transmitVfo = transmitVfo == 0 ? RIG_VFO_A : RIG_VFO_B;
        m_completionPending = false;
        const int result = rig_send_morse(m_rig, m_transmitVfo, bytes.constData());
        if (!check(result, error)) {
            return false;
        }
        if (supportsCompletion()) {
            m_completionPending = true;
            m_completionStarted.start();
        }
        return true;
    }

    bool abort(QString *error) override
    {
        if (!m_open) {
            if (error != nullptr) {
                *error = QStringLiteral("Hamlib keyer is disconnected.");
            }
            return false;
        }
        if (!check(rig_stop_morse(m_rig, m_transmitVfo), error)) {
            return false;
        }
        m_completionPending = false;
        return true;
    }

    bool supportsCompletion() const override
    {
        return m_open && m_rig != nullptr && m_rig->caps != nullptr
            && m_rig->caps->wait_morse != nullptr && m_completionPollingReady;
    }

    bool pollCompleted(QString *error) override
    {
        if (!m_completionPending || !supportsCompletion()
            || m_completionStarted.elapsed() < 200) {
            return false;
        }
        ptt_t ptt = RIG_PTT_OFF;
        const int result = rig_get_ptt(m_rig, m_transmitVfo, &ptt);
        if (!check(result, error)) {
            m_completionPending = false;
            return false;
        }
        if (ptt == RIG_PTT_OFF) {
            m_completionPending = false;
            return true;
        }
        return false;
    }

    bool setWpm(int wpm, QString *error) override
    {
        if (!m_open || wpm < 5 || wpm > 80) {
            if (error != nullptr) {
                *error = QStringLiteral("WPM must be between 5 and 80.");
            }
            return false;
        }
        if ((m_rig->caps->has_set_level & RIG_LEVEL_KEYSPD) == 0) {
            if (error != nullptr) {
                *error = QStringLiteral("Selected Hamlib model does not support CW speed control.");
            }
            return false;
        }
        value_t value{};
        value.i = wpm;
        return check(rig_set_level(m_rig, m_transmitVfo, RIG_LEVEL_KEYSPD, value),
                     error);
    }

private:
    bool setConf(const QString &name, const QString &value, QString *error)
    {
        const QByteArray key = name.toLocal8Bit();
        const QByteArray setting = value.toLocal8Bit();
        const hamlib_token_t token = rig_token_lookup(m_rig, key.constData());
        if (token == RIG_CONF_END) {
            return true;
        }
        return check(rig_set_conf(m_rig, token, setting.data()), error);
    }

    bool check(int result, QString *error) const
    {
        if (result == RIG_OK) {
            return true;
        }
        if (error != nullptr) {
            *error = QString::fromLocal8Bit(rigerror(result));
        }
        return false;
    }

    RIG *m_rig = nullptr;
    bool m_open = false;
    bool m_completionPollingReady = false;
    bool m_completionPending = false;
    QElapsedTimer m_completionStarted;
    vfo_t m_transmitVfo = RIG_VFO_A;
};
#endif

ICwKeyer *createKeyer(CwKeyerType type)
{
    switch (type) {
    case CwKeyerType::Mock:
        return new MockCwKeyer;
    case CwKeyerType::Cwdaemon:
        return new CwdaemonKeyer;
    case CwKeyerType::Hamlib:
#ifdef LOGGER_WITH_HAMLIB
        return new HamlibCwKeyer;
#else
        return nullptr;
#endif
    }
    return nullptr;
}

} // namespace

QString expandCwMacros(QString message, const QMap<QString, QString> &values)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        message.replace(QStringLiteral("{%1}").arg(it.key()), it.value(),
                        Qt::CaseInsensitive);
    }
    return message;
}

HamlibCwCapabilities inspectHamlibCwCapabilities(int modelId)
{
    HamlibCwCapabilities result;
#ifdef LOGGER_WITH_HAMLIB
    static std::once_flag backendsLoaded;
    std::call_once(backendsLoaded, [] { rig_load_all_backends(); });
    RIG *rig = rig_init(static_cast<rig_model_t>(modelId));
    if (rig == nullptr) {
        return result;
    }
    result.modelAvailable = true;
    result.sendText = rig->caps != nullptr && rig->caps->send_morse != nullptr;
    result.setSpeed = rig->caps != nullptr
        && (rig->caps->has_set_level & RIG_LEVEL_KEYSPD) != 0;
    rig_cleanup(rig);
#else
    Q_UNUSED(modelId)
#endif
    return result;
}

CwKeyerWorker::CwKeyerWorker(QObject *parent)
    : QObject(parent)
    , m_queueTimer(new QTimer(this))
{
    m_queueTimer->setSingleShot(true);
    connect(m_queueTimer, &QTimer::timeout,
            this, &CwKeyerWorker::sendNextMessage);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(50);
    connect(m_pollTimer, &QTimer::timeout, this, &CwKeyerWorker::pollCompletion);
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout,
            this, &CwKeyerWorker::completionTimedOut);
}

CwKeyerWorker::~CwKeyerWorker()
{
    disconnectKeyer();
}

void CwKeyerWorker::connectKeyer(const CwKeyerConfig &config)
{
    disconnectKeyer();
    m_config = config;
    m_keyer = createKeyer(config.type);
    if (m_keyer == nullptr) {
        m_lastError = tr("This build does not include Hamlib CW support.");
        publishStatus(m_lastError);
        return;
    }
    QString error;
    if (!m_keyer->connect(config, &error)) {
        delete m_keyer;
        m_keyer = nullptr;
        m_lastError = error;
        publishStatus(error);
        return;
    }
    m_config.wpm = qBound(5, config.wpm, 80);
    m_lastError.clear();
    if (!m_keyer->setWpm(m_config.wpm, &error)) {
        m_lastError = tr("CW keyer ready; initial WPM could not be applied: %1")
                          .arg(error);
        publishStatus(m_lastError);
    } else {
        publishStatus(tr("CW keyer ready."));
    }
}

void CwKeyerWorker::disconnectKeyer()
{
    if (m_queueTimer != nullptr) {
        m_queueTimer->stop();
    }
    clearPendingCompletion();
    m_timedOut = false;
    m_queue.clear();
    m_transmitting = false;
    if (m_keyer != nullptr) {
        QString error;
        m_keyer->abort(&error);
        m_keyer->disconnect();
        delete m_keyer;
        m_keyer = nullptr;
    }
    publishStatus(tr("CW keyer disconnected."));
}

void CwKeyerWorker::enqueueMessage(const QString &text, int transmitVfo)
{
    queueText(text.trimmed(), transmitVfo, false);
}

void CwKeyerWorker::enqueueText(const QString &text, int transmitVfo)
{
    queueText(text, transmitVfo, true);
}

void CwKeyerWorker::queueText(const QString &text, int transmitVfo, bool live)
{
    ++m_processedEnqueues;
    if (text.isEmpty()) {
        publishStatus(tr("CW message is empty."));
        return;
    }
    if (m_keyer == nullptr) {
        publishStatus(tr("Connect a CW keyer before sending."));
        return;
    }
    if (transmitVfo < 0 || transmitVfo > 1) {
        publishStatus(tr("CW transmit VFO must be A or B."));
        return;
    }
    if (live && !m_queue.isEmpty() && m_queue.last().live
        && m_queue.last().transmitVfo == transmitVfo) {
        m_queue.last().text += text;
        publishStatus(tr("CW text queued."));
        return;
    }
    if (m_queue.size() >= 100) {
        publishStatus(tr("CW queue is full (100 messages maximum)."));
        return;
    }
    m_queue.append({text, transmitVfo, live});
    if (!m_transmitting && !m_queueTimer->isActive()) {
        sendNextMessage();
    } else {
        publishStatus(tr("CW message queued."));
    }
}

void CwKeyerWorker::abortTransmission()
{
    m_queueTimer->stop();
    clearPendingCompletion();
    m_timedOut = false;
    m_queue.clear();
    if (m_keyer == nullptr) {
        publishStatus(tr("CW keyer is disconnected."));
        return;
    }
    QString error;
    if (!m_keyer->abort(&error)) {
        m_lastError = error;
        publishStatus(error);
        return;
    }
    publishStatus(tr("CW transmission aborted; queue cleared."));
}

void CwKeyerWorker::setWpm(int wpm)
{
    if (wpm < 5 || wpm > 80) {
        m_lastError = tr("WPM must be between 5 and 80.");
        publishStatus(m_lastError);
        return;
    }
    if (m_keyer == nullptr) {
        m_config.wpm = wpm;
        publishStatus(tr("WPM saved; connect a keyer to apply it."));
        return;
    }
    QString error;
    if (!m_keyer->setWpm(wpm, &error)) {
        m_lastError = error;
        publishStatus(error);
        return;
    }
    m_config.wpm = wpm;
    m_lastError.clear();
    publishStatus(tr("CW speed set to %1 WPM.").arg(wpm));
}

void CwKeyerWorker::sendNextMessage()
{
    if (m_keyer == nullptr || m_queue.isEmpty()) {
        m_transmitting = false;
        publishStatus();
        return;
    }
    const QueuedMessage queued = m_queue.takeFirst();
    const QString &message = queued.text;
    QString error;
    if (!m_keyer->sendText(message, queued.transmitVfo, &error)) {
        m_transmitting = false;
        m_lastError = error;
        m_queue.clear();
        publishStatus(error);
        return;
    }
    m_transmitting = true;
    m_transmittingVfo = queued.transmitVfo;
    m_timedOut = false;
    publishStatus(tr("Sending CW."));
    const int estimate = estimatedDurationMs(message, queued.live);
    if (m_keyer->supportsCompletion()) {
        // Safety policy: if the keyer never confirms, the worker stops the
        // queue instead of sending blindly (which could key over the operator
        // or a still-playing message). See completionTimedOut().
        m_timeoutTimer->start(static_cast<int>(
            estimate * m_config.completionTimeoutFactor
            + m_config.completionTimeoutMarginMs));
        m_pollTimer->start();
    } else {
        m_queueTimer->start(estimate);
    }
}

void CwKeyerWorker::pollCompletion()
{
    if (m_keyer == nullptr || !m_transmitting) {
        clearPendingCompletion();
        return;
    }
    QString error;
    if (m_keyer->pollCompleted(&error)) {
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        sendNextMessage();
    } else if (!error.isEmpty()) {
        m_pollTimer->stop();
        m_timeoutTimer->stop();
        m_queue.clear();
        m_transmitting = false;
        QString abortError;
        if (m_keyer != nullptr && !m_keyer->abort(&abortError)
            && !abortError.isEmpty()) {
            error += tr("; abort failed: %1").arg(abortError);
        }
        m_lastError = tr("CW completion check failed: %1").arg(error);
        publishStatus(m_lastError);
    }
}

void CwKeyerWorker::completionTimedOut()
{
    m_pollTimer->stop();
    // Last chance: the confirmation may have arrived just before the timeout.
    QString completionError;
    if (m_keyer != nullptr) {
        if (m_keyer->pollCompleted(&completionError)) {
            sendNextMessage();
            return;
        }
        if (!completionError.isEmpty()) {
            m_pollTimer->stop();
            m_timeoutTimer->stop();
            m_queue.clear();
            m_transmitting = false;
            QString abortError;
            if (m_keyer != nullptr && !m_keyer->abort(&abortError)
                && !abortError.isEmpty()) {
                completionError += tr("; abort failed: %1").arg(abortError);
            }
            m_lastError = tr("CW completion check failed: %1").arg(completionError);
            publishStatus(m_lastError);
            return;
        }
    }
    // Safer behaviour: stop and clear the queue, silence the keyer, and tell
    // the operator, rather than blindly sending queued messages.
    m_queue.clear();
    m_transmitting = false;
    m_timedOut = true;
    if (m_keyer != nullptr) {
        QString error;
        m_keyer->abort(&error);
    }
    publishStatus(tr("No completion confirmation from keyer; CW queue stopped."));
}

void CwKeyerWorker::clearPendingCompletion()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    if (m_timeoutTimer != nullptr) {
        m_timeoutTimer->stop();
    }
    m_transmitting = false;
}

void CwKeyerWorker::publishStatus(const QString &message)
{
    CwKeyerStatus status;
    status.state = m_keyer != nullptr ? ConnectionState::Connected
                                      : (m_lastError.isEmpty()
                                             ? ConnectionState::Disconnected
                                             : ConnectionState::Error);
    status.message = message.isEmpty() ? m_lastError : message;
    status.error = m_lastError;
    status.queuedMessages = m_queue.size();
    status.wpm = m_config.wpm;
    status.transmitting = m_transmitting;
    status.processedEnqueues = m_processedEnqueues;
    status.transmitVfo = m_transmittingVfo;
    status.completionTimedOut = m_timedOut;
    if (m_keyer != nullptr) {
        status.completionConfirmed = m_keyer->supportsCompletion();
        status.pacing = status.completionConfirmed ? tr("keyer confirmation")
                                                   : tr("estimated timing");
    }
    emit statusChanged(status);
}

int CwKeyerWorker::estimatedDurationMs(const QString &text, bool live) const
{
    const int wpm = qMax(5, m_config.wpm);
    if (live) {
        static const QMap<QChar, QString> morse{
            {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
            {'E', "."}, {'F', "..-."}, {'G', "--."}, {'H', "...."},
            {'I', ".."}, {'J', ".---"}, {'K', "-.-"}, {'L', ".-.."},
            {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."},
            {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"},
            {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"},
            {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"}, {'1', ".----"},
            {'2', "..---"}, {'3', "...--"}, {'4', "....-"}, {'5', "....."},
            {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
            {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
            {'=', "-...-"}, {'+', ".-.-."}, {'-', "-....-"}, {'@', ".--.-."},
            {'(', "-.--."}, {')', "-.--.-"}, {':', "---..."}, {';', "-.-.-."},
            {'\'', ".----."}, {'"', ".-..-."}, {'!', "-.-.--"}, {'_', "..--.-"}
        };
        int units = 0;
        for (const QChar ch : text.toUpper()) {
            if (ch.isSpace()) {
                // The preceding character already includes a three-unit gap.
                units += 4;
                continue;
            }
            const auto code = morse.constFind(ch);
            if (code == morse.cend()) {
                units += 12;
                continue;
            }
            units += code->size() - 1 + 3;
            for (const QChar element : *code) {
                units += element == QLatin1Char('.') ? 1 : 3;
            }
        }
        return qMax(1, units * 1200 / wpm);
    }
    const int units = qMax(1, text.size()) * 12;
    return qMax(250, units * 1200 / wpm);
}

CwKeyerController::CwKeyerController(QObject *parent)
    : QObject(parent)
    , m_worker(new CwKeyerWorker)
{
    qRegisterMetaType<CwKeyerConfig>();
    qRegisterMetaType<CwKeyerStatus>();
    m_worker->moveToThread(&m_thread);
    connect(this, &CwKeyerController::connectRequested,
            m_worker, &CwKeyerWorker::connectKeyer, Qt::QueuedConnection);
    connect(this, &CwKeyerController::disconnectRequested,
            m_worker, &CwKeyerWorker::disconnectKeyer, Qt::QueuedConnection);
    connect(this, &CwKeyerController::enqueueRequested,
            m_worker, &CwKeyerWorker::enqueueMessage, Qt::QueuedConnection);
    connect(this, &CwKeyerController::textRequested,
            m_worker, &CwKeyerWorker::enqueueText, Qt::QueuedConnection);
    connect(this, &CwKeyerController::abortRequested,
            m_worker, &CwKeyerWorker::abortTransmission, Qt::QueuedConnection);
    connect(this, &CwKeyerController::setWpmRequested,
            m_worker, &CwKeyerWorker::setWpm, Qt::QueuedConnection);
    connect(m_worker, &CwKeyerWorker::statusChanged,
            this, &CwKeyerController::statusChanged, Qt::QueuedConnection);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

CwKeyerController::~CwKeyerController()
{
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "disconnectKeyer",
                                  Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    m_worker = nullptr;
}

void CwKeyerController::connectKeyer(const CwKeyerConfig &config)
{
    emit connectRequested(config);
}

void CwKeyerController::disconnectKeyer()
{
    emit disconnectRequested();
}

void CwKeyerController::enqueueMessage(const QString &text, int transmitVfo)
{
    emit enqueueRequested(text, transmitVfo);
}

void CwKeyerController::abortTransmission()
{
    emit abortRequested();
}

void CwKeyerController::enqueueText(const QString &text, int transmitVfo)
{
    emit textRequested(text, transmitVfo);
}

void CwKeyerController::setWpm(int wpm)
{
    emit setWpmRequested(wpm);
}

} // namespace logger
