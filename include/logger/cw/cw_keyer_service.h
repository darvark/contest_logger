#pragma once

#include "logger/domain/types.h"
#include "logger/radio/radio_service.h"

#include <QMap>
#include <QObject>
#include <QThread>

namespace logger {

enum class CwKeyerType {
    Hamlib,
    Cwdaemon,
    Mock
};

struct CwKeyerConfig {
    CwKeyerType type = CwKeyerType::Mock;
    RadioConfig radio;
    QString host = QStringLiteral("127.0.0.1");
    int port = 6789;
    int wpm = 25;
    // cwdaemon: request "<ESC>h<token>" replies to confirm end of transmission.
    bool cwdaemonCompletionReply = true;
    // Mock keyer: -1 = no completion support (estimate pacing); >= 0 = report
    // completion that many ms after sendText().
    int mockCompletionDelayMs = -1;
    // Mock keyer: claim completion support but never report it (timeout tests).
    bool mockCompletionNever = false;
    // Completion timeout = estimated duration * factor + margin.
    double completionTimeoutFactor = 3.0;
    int completionTimeoutMarginMs = 3000;
};

struct CwKeyerStatus {
    ConnectionState state = ConnectionState::Disconnected;
    QString message;
    int queuedMessages = 0;
    int wpm = 25;
    bool transmitting = false;
    int transmitVfo = 0;
    // True when the keyer itself confirms end of transmission; false when the
    // queue is paced by estimated duration.
    bool completionConfirmed = false;
    // "keyer confirmation" or "estimated timing" (empty when not connected).
    QString pacing;
    // True when the last message got no completion confirmation in time and
    // the queue was stopped.
    bool completionTimedOut = false;
};

struct HamlibCwCapabilities {
    bool modelAvailable = false;
    bool sendText = false;
    bool setSpeed = false;
};

HamlibCwCapabilities inspectHamlibCwCapabilities(int modelId);
QString expandCwMacros(QString message, const QMap<QString, QString> &values);

class ICwKeyer {
public:
    virtual ~ICwKeyer() = default;
    virtual bool connect(const CwKeyerConfig &config, QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool sendText(const QString &text, int transmitVfo, QString *error) = 0;
    virtual bool abort(QString *error) = 0;
    virtual bool setWpm(int wpm, QString *error) = 0;
    // True if the keyer can confirm end of transmission via pollCompleted().
    virtual bool supportsCompletion() const { return false; }
    // Non-blocking. Returns true exactly once after the text most recently
    // passed to sendText() has been fully played. Never true after abort().
    virtual bool pollCompleted(QString *error = nullptr)
    {
        Q_UNUSED(error)
        return false;
    }
};

class CwKeyerWorker final : public QObject {
    Q_OBJECT

public:
    explicit CwKeyerWorker(QObject *parent = nullptr);
    ~CwKeyerWorker() override;

public slots:
    void connectKeyer(const logger::CwKeyerConfig &config);
    void disconnectKeyer();
    void enqueueMessage(const QString &text, int transmitVfo = 0);
    void abortTransmission();
    void setWpm(int wpm);

signals:
    void statusChanged(const logger::CwKeyerStatus &status);

private slots:
    void sendNextMessage();
    void pollCompletion();
    void completionTimedOut();

private:
    void publishStatus(const QString &message = {});
    int estimatedDurationMs(const QString &text) const;
    void clearPendingCompletion();

    ICwKeyer *m_keyer = nullptr;
    class QTimer *m_queueTimer = nullptr;
    class QTimer *m_pollTimer = nullptr;
    class QTimer *m_timeoutTimer = nullptr;
    struct QueuedMessage {
        QString text;
        int transmitVfo = 0;
    };
    QList<QueuedMessage> m_queue;
    CwKeyerConfig m_config;
    bool m_transmitting = false;
    int m_transmittingVfo = 0;
    bool m_timedOut = false;
    QString m_lastError;
};

class CwKeyerController final : public QObject {
    Q_OBJECT

public:
    explicit CwKeyerController(QObject *parent = nullptr);
    ~CwKeyerController() override;

    void connectKeyer(const CwKeyerConfig &config);
    void disconnectKeyer();
    void enqueueMessage(const QString &text, int transmitVfo = 0);
    void abortTransmission();
    void setWpm(int wpm);

signals:
    void connectRequested(const logger::CwKeyerConfig &config);
    void disconnectRequested();
    void enqueueRequested(const QString &text, int transmitVfo);
    void abortRequested();
    void setWpmRequested(int wpm);
    void statusChanged(const logger::CwKeyerStatus &status);

private:
    QThread m_thread;
    CwKeyerWorker *m_worker = nullptr;
};

} // namespace logger

Q_DECLARE_METATYPE(logger::CwKeyerConfig)
Q_DECLARE_METATYPE(logger::CwKeyerStatus)
