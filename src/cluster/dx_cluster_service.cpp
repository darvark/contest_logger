#include "logger/cluster/dx_cluster_service.h"

#include <QTcpSocket>
#include <QRegularExpression>
#include <QTimer>
#include <QTime>
#include <QTimeZone>

#include <algorithm>
#include <functional>

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

QString bandForFrequency(qint64 frequencyHz)
{
    const double mhz = static_cast<double>(frequencyHz) / 1'000'000.0;
    const QVector<QPair<double, double>> ranges{
        {1.8, 2.0}, {3.5, 4.0}, {5.0, 5.5}, {7.0, 7.3},
        {10.1, 10.15}, {14.0, 14.35}, {18.068, 18.168},
        {21.0, 21.45}, {24.89, 24.99}, {28.0, 29.7},
        {50.0, 54.0}, {144.0, 148.0}
    };
    const QStringList names{
        QStringLiteral("160m"), QStringLiteral("80m"), QStringLiteral("60m"),
        QStringLiteral("40m"), QStringLiteral("30m"), QStringLiteral("20m"),
        QStringLiteral("17m"), QStringLiteral("15m"), QStringLiteral("12m"),
        QStringLiteral("10m"), QStringLiteral("6m"), QStringLiteral("2m")
    };
    for (qsizetype i = 0; i < ranges.size(); ++i) {
        if (mhz >= ranges.at(i).first && mhz <= ranges.at(i).second) {
            return names.at(i);
        }
    }
    return {};
}

QByteArray stripTelnet(QByteArrayView bytes, int &state,
                       const std::function<void(quint8, quint8)> &reply)
{
    constexpr quint8 IAC = 255;
    constexpr quint8 DONT = 254;
    constexpr quint8 DO = 253;
    constexpr quint8 WONT = 252;
    constexpr quint8 WILL = 251;
    constexpr quint8 SB = 250;
    constexpr quint8 SE = 240;
    QByteArray output;
    quint8 negotiation = 0;
    for (const char raw : bytes) {
        const quint8 byte = static_cast<quint8>(raw);
        switch (state) {
        case 0:
            if (byte == IAC) {
                state = 1;
            } else {
                output.append(raw);
            }
            break;
        case 1:
            if (byte == IAC) {
                output.append(static_cast<char>(IAC));
                state = 0;
            } else if (byte == DO || byte == DONT || byte == WILL || byte == WONT) {
                negotiation = byte;
                state = 2;
            } else if (byte == SB) {
                state = 3;
            } else {
                state = 0;
            }
            break;
        case 2:
            reply(negotiation, byte);
            state = 0;
            break;
        case 3:
            if (byte == IAC) {
                state = 4;
            }
            break;
        case 4:
            state = byte == SE ? 0 : 3;
            break;
        default:
            state = 0;
            break;
        }
    }
    return output;
}

} // namespace

std::optional<Spot> DxSpotParser::parseLine(const QByteArray &line,
                                             const QDateTime &receivedAtUtc,
                                             QString *error)
{
    setError(error, {});
    static const QRegularExpression spotPattern(
        QStringLiteral("^\\s*DX\\s+de\\s+([^:\\s]+):\\s*"
                       "([0-9]+(?:\\.[0-9]+)?)\\s+"
                       "([A-Z0-9/]{3,})\\b(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString text = QString::fromUtf8(line).trimmed();
    const auto match = spotPattern.match(text);
    if (!match.hasMatch()) {
        setError(error, QStringLiteral("Not a DX cluster spot line."));
        return std::nullopt;
    }

    bool frequencyOk = false;
    const double reportedFrequency = match.captured(2).toDouble(&frequencyOk);
    if (!frequencyOk || reportedFrequency <= 0.0) {
        setError(error, QStringLiteral("Spot frequency is invalid."));
        return std::nullopt;
    }
    const double frequencyHz = reportedFrequency < 1000.0
        ? reportedFrequency * 1'000'000.0
        : reportedFrequency * 1000.0;
    if (frequencyHz < 100'000.0 || frequencyHz > 1'000'000'000.0) {
        setError(error, QStringLiteral("Spot frequency is outside supported range."));
        return std::nullopt;
    }

    Spot spot;
    spot.spotter = match.captured(1).toUpper();
    spot.frequencyHz = qRound64(frequencyHz);
    spot.callsign = match.captured(3).toUpper();
    spot.comment = match.captured(4).trimmed();
    spot.band = bandForFrequency(spot.frequencyHz);
    spot.timestampUtc = receivedAtUtc.toUTC();
    spot.source = QStringLiteral("DX cluster");

    static const QRegularExpression modePattern(
        QStringLiteral("(?:^|\\s)(CW|SSB|USB|LSB|RTTY|FT8|FT4|PSK|FM|AM)(?:\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto modeMatch = modePattern.match(spot.comment);
    if (modeMatch.hasMatch()) {
        const QString mode = modeMatch.captured(1).toUpper();
        spot.mode = mode == QStringLiteral("SSB") ? QStringLiteral("SSB") : mode;
    }

    // The spot time is the last "HHMMZ" token. A token with an impossible
    // time (e.g. 2575Z) makes the whole spot invalid rather than carrying a
    // bogus timestamp.
    static const QRegularExpression timestampPattern(QStringLiteral("\\b([0-9]{4})Z\\b"));
    auto timestampMatches = timestampPattern.globalMatch(spot.comment);
    QString hhmm;
    while (timestampMatches.hasNext()) {
        hhmm = timestampMatches.next().captured(1);
    }
    if (!hhmm.isEmpty()) {
        const int hour = hhmm.left(2).toInt();
        const int minute = hhmm.mid(2, 2).toInt();
        if (hour > 23 || minute > 59) {
            setError(error, QStringLiteral("Spot time is invalid."));
            return std::nullopt;
        }
        // Date policy: the spot carries only HHMM, so choose the date (the
        // day before, same day or day after the receive date) whose
        // timestamp is nearest to the receive time. This handles 2359Z
        // received just after midnight (previous day) and small clock skew
        // such as 0001Z received at 23:59 (next day).
        const QDateTime received = receivedAtUtc.toUTC();
        const QTime time(hour, minute);
        QDateTime best;
        qint64 bestDistance = 0;
        for (int offset = -1; offset <= 1; ++offset) {
            const QDateTime candidate(received.date().addDays(offset), time, QTimeZone::UTC);
            const qint64 distance = qAbs(candidate.secsTo(received));
            if (!best.isValid() || distance < bestDistance) {
                best = candidate;
                bestDistance = distance;
            }
        }
        spot.timestampUtc = best.toUTC();
    }
    return spot;
}

DxClusterWorker::DxClusterWorker(QObject *parent)
    : QObject(parent)
{
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &DxClusterWorker::reconnect);
    m_loginTimer = new QTimer(this);
    m_loginTimer->setSingleShot(true);
    connect(m_loginTimer, &QTimer::timeout, this, &DxClusterWorker::sendLogin);
}

DxClusterWorker::~DxClusterWorker()
{
    stop();
}

void DxClusterWorker::start(const DxClusterConfig &config)
{
    stop();
    static const QRegularExpression callsignPattern(
        QStringLiteral("^[A-Z0-9/]{3,20}$"));
    if (config.host.trimmed().isEmpty() || config.port < 1 || config.port > 65535
        || !callsignPattern.match(config.callsign.trimmed().toUpper()).hasMatch()) {
        emit statusChanged({ConnectionState::Error,
                            tr("Enter a host, valid TCP port and valid station callsign."),
                            0});
        return;
    }
    m_config = config;
    m_config.host = config.host.trimmed();
    m_config.callsign = config.callsign.trimmed().toUpper();
    m_config.reconnectInitialMs = qBound(10, config.reconnectInitialMs, 30'000);
    m_config.loginFallbackMs = qBound(10, config.loginFallbackMs, 60'000);
    m_requested = true;
    m_reconnectDelayMs = m_config.reconnectInitialMs;
    connectToCluster();
}

void DxClusterWorker::stop()
{
    m_requested = false;
    if (m_reconnectTimer != nullptr) {
        m_reconnectTimer->stop();
    }
    if (m_loginTimer != nullptr) {
        m_loginTimer->stop();
    }
    if (m_socket != nullptr) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_lineBuffer.clear();
    m_telnetBuffer.clear();
    m_telnetState = 0;
    emit statusChanged({ConnectionState::Disconnected, tr("Cluster disconnected."), 0});
}

void DxClusterWorker::connectToCluster()
{
    if (!m_requested) {
        return;
    }
    if (m_socket != nullptr) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
    }
    m_socket = new QTcpSocket(this);
    m_loggedIn = false;
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        m_reconnectDelayMs = m_config.reconnectInitialMs;
        m_lineBuffer.clear();
        m_telnetBuffer.clear();
        m_telnetState = 0;
        m_loggedIn = false;
        emit statusChanged({ConnectionState::Connected,
                            tr("Connected to %1:%2.")
                                .arg(m_config.host).arg(m_config.port),
                            0});
        m_loginTimer->start(m_config.loginFallbackMs);
    });
    connect(m_socket, &QTcpSocket::readyRead,
            this, &DxClusterWorker::readAvailableData);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &DxClusterWorker::socketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, &DxClusterWorker::socketError);
    emit statusChanged({ConnectionState::Connecting,
                        tr("Connecting to %1:%2...")
                            .arg(m_config.host).arg(m_config.port),
                        0});
    m_socket->connectToHost(m_config.host, static_cast<quint16>(m_config.port));
}

void DxClusterWorker::readAvailableData()
{
    if (m_socket == nullptr) {
        return;
    }
    m_telnetBuffer = stripTelnet(
        m_socket->readAll(), m_telnetState,
        [this](quint8 command, quint8 option) { sendTelnetReply(command, option); });
    m_lineBuffer.append(m_telnetBuffer);
    m_telnetBuffer.clear();

    if (!m_loggedIn) {
        const qsizetype lastNewline = m_lineBuffer.lastIndexOf('\n');
        const QByteArray tail = m_lineBuffer.mid(lastNewline + 1);
        if (looksLikeLoginPrompt(tail)) {
            // Prompts have no newline; drop it so it does not prefix the next line.
            m_lineBuffer.truncate(lastNewline + 1);
            sendLogin();
        } else if (lastNewline > 0
                   && looksLikeLoginPrompt(m_lineBuffer.left(lastNewline)
                                               .split('\n').constLast())) {
            sendLogin();
        }
    }
    if (m_lineBuffer.size() > 64 * 1024) {
        m_lineBuffer.clear();
        scheduleReconnect(tr("Cluster sent an oversized unterminated line."));
        return;
    }
    while (true) {
        const qsizetype newline = m_lineBuffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = m_lineBuffer.left(newline);
        m_lineBuffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        processLine(line);
    }
}

void DxClusterWorker::socketDisconnected()
{
    if (m_requested) {
        scheduleReconnect(tr("Cluster connection closed."));
    }
}

void DxClusterWorker::socketError()
{
    if (m_socket == nullptr || !m_requested) {
        return;
    }
    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect(m_socket->errorString());
    }
}

void DxClusterWorker::scheduleReconnect(const QString &message)
{
    if (!m_requested || (m_reconnectTimer != nullptr && m_reconnectTimer->isActive())) {
        return;
    }
    const int delayMs = m_reconnectDelayMs;
    const int delay = (delayMs + 999) / 1000;
    // Arm the timer before aborting: abort() can emit disconnected() synchronously.
    m_loginTimer->stop();
    m_reconnectTimer->start(delayMs);
    m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, 30'000);
    emit statusChanged({ConnectionState::Error,
                        tr("%1 Reconnecting in %2 s.").arg(message).arg(delay),
                        delay});
    if (m_socket != nullptr) {
        m_socket->abort();
    }
}

bool DxClusterWorker::looksLikeLoginPrompt(const QByteArray &text)
{
    const QByteArray t = text.trimmed().toLower();
    return t.endsWith(':') && (t.contains("login") || t.contains("call"));
}

void DxClusterWorker::sendLogin()
{
    m_loginTimer->stop();
    if (m_loggedIn || m_socket == nullptr
        || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_loggedIn = true;
    m_socket->write(m_config.callsign.toUtf8() + '\n');
}

void DxClusterWorker::processLine(const QByteArray &line)
{
    const QString text = QString::fromUtf8(line).trimmed();
    if (text.isEmpty()) {
        return;
    }
    QString error;
    const auto spot = DxSpotParser::parseLine(line, QDateTime::currentDateTimeUtc(),
                                               &error);
    if (spot.has_value()) {
        emit spotReceived(*spot);
    }
}

void DxClusterWorker::sendTelnetReply(quint8 command, quint8 option)
{
    if (m_socket == nullptr) {
        return;
    }
    constexpr quint8 IAC = 255;
    constexpr quint8 DO = 253;
    constexpr quint8 WILL = 251;
    constexpr quint8 DONT = 254;
    constexpr quint8 WONT = 252;
    if (command != DO && command != WILL) {
        return;
    }
    const quint8 response = command == DO ? WONT : DONT;
    const char bytes[]{static_cast<char>(IAC), static_cast<char>(response),
                       static_cast<char>(option)};
    m_socket->write(bytes, 3);
}

void DxClusterWorker::reconnect()
{
    connectToCluster();
}

DxClusterController::DxClusterController(QObject *parent)
    : QObject(parent)
    , m_worker(new DxClusterWorker)
{
    qRegisterMetaType<DxClusterConfig>();
    qRegisterMetaType<DxClusterStatus>();
    qRegisterMetaType<Spot>();
    m_worker->moveToThread(&m_thread);
    connect(this, &DxClusterController::startRequested,
            m_worker, &DxClusterWorker::start, Qt::QueuedConnection);
    connect(this, &DxClusterController::stopRequested,
            m_worker, &DxClusterWorker::stop, Qt::QueuedConnection);
    connect(m_worker, &DxClusterWorker::statusChanged,
            this, &DxClusterController::statusChanged, Qt::QueuedConnection);
    connect(m_worker, &DxClusterWorker::spotReceived,
            this, &DxClusterController::spotReceived, Qt::QueuedConnection);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

DxClusterController::~DxClusterController()
{
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    m_worker = nullptr;
}

void DxClusterController::start(const DxClusterConfig &config)
{
    emit startRequested(config);
}

void DxClusterController::stop()
{
    emit stopRequested();
}

} // namespace logger
