#pragma once

#include "logger/domain/types.h"

#include <QObject>
#include <QThread>

class QTimer;

namespace logger {

enum class RadioBackendType {
    Hamlib,
    Mock
};

enum class RadioTransport {
    Serial,
    Tcp
};

struct RadioConfig {
    RadioBackendType backend = RadioBackendType::Hamlib;
    RadioTransport transport = RadioTransport::Serial;
    int modelId = 1;
    QString devicePath;
    QString host = QStringLiteral("127.0.0.1");
    int port = 4532;
    int baudRate = 9600;
    bool mockDualReceiveSupported = true;
    int timeoutMs = 3000;
};

struct RadioSnapshot {
    ConnectionState state = ConnectionState::Disconnected;
    qint64 frequencyHz = 0;
    QString mode;
    QString message;
    QVector<Vfo> vfos;
    int focusVfo = 0;
    bool dualVfoSupported = false;
    bool dualReceiveSupported = false;
};

struct RadioProbeResult {
    bool ok = false;
    QString message;
    qint64 frequencyHz = 0;
    QString mode;
    int vfoCount = 0;
    bool dualVfo = false;
    bool dualReceive = false;
};

RadioProbeResult probeRadio(const RadioConfig &config, int timeoutMs = 3000);

class RadioWorker final : public QObject {
    Q_OBJECT

public:
    class Backend;
    explicit RadioWorker(QObject *parent = nullptr);
    ~RadioWorker() override;

public slots:
    void connectRadio(const logger::RadioConfig &config);
    void disconnectRadio();
    void setFrequency(qint64 frequencyHz, int vfo = 0);
    void setVfoFocus(int vfo);

signals:
    void snapshotChanged(const logger::RadioSnapshot &snapshot);

private slots:
    void pollRadio();

private:
    Backend *m_backend = nullptr;
    QTimer *m_pollTimer = nullptr;
};

class RadioController final : public QObject {
    Q_OBJECT

public:
    explicit RadioController(QObject *parent = nullptr);
    ~RadioController() override;

    void connectRadio(const RadioConfig &config);
    void disconnectRadio();
    void setFrequency(qint64 frequencyHz, int vfo = 0);
    void setVfoFocus(int vfo);

signals:
    void connectRequested(const logger::RadioConfig &config);
    void disconnectRequested();
    void tuneRequested(qint64 frequencyHz, int vfo);
    void focusRequested(int vfo);
    void snapshotChanged(const logger::RadioSnapshot &snapshot);

private:
    QThread m_thread;
    RadioWorker *m_worker = nullptr;
};

} // namespace logger

Q_DECLARE_METATYPE(logger::RadioConfig)
Q_DECLARE_METATYPE(logger::RadioSnapshot)
