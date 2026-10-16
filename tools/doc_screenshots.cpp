// Generates the screenshots used by docs/USER_GUIDE.md.
// Usage: QT_QPA_PLATFORM=offscreen doc_screenshots <output-directory>
#include "logger/contest/contest_definition_loader.h"
#include "logger/data/sqlite_repository.h"
#include "logger/ui/main_window.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <functional>

using namespace logger;

namespace {

void wait(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

bool save(QWidget *widget, const QString &directory, const QString &name)
{
    const QString path = QDir(directory).filePath(name + QStringLiteral(".png"));
    if (!widget->grab().save(path)) {
        QTextStream(stderr) << "Unable to write " << path << '\n';
        return false;
    }
    QTextStream(stdout) << "Wrote " << path << '\n';
    return true;
}

QAction *findAction(QWidget *window, const QString &text)
{
    for (QAction *action : window->findChildren<QAction *>()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

// Opens a modal dialog through `open`, lets `prepare` adjust it, captures it and closes it.
bool captureModal(const std::function<void()> &open, const QString &directory,
                  const QString &name,
                  const std::function<void(QDialog *)> &prepare = {})
{
    bool saved = false;
    QTimer timer;
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        timer.stop();
        if (prepare) {
            prepare(dialog);
        }
        wait(200);
        saved = save(dialog, directory, name);
        dialog->reject();
    });
    timer.start();
    open();
    timer.stop();
    if (!saved) {
        QTextStream(stderr) << "Dialog not captured: " << name << '\n';
    }
    return saved;
}

Qso makeQso(qint64 contestId, int serial, const QString &call, const QString &band,
            qint64 frequencyHz, const QString &received, const QDateTime &time)
{
    Qso qso;
    qso.contestId = contestId;
    qso.callsign = call;
    qso.stationCallsign = QStringLiteral("SN6U");
    qso.band = band;
    qso.frequencyHz = frequencyHz;
    qso.mode = QStringLiteral("CW");
    qso.timestampUtc = time;
    const QString sentSerial = QStringLiteral("%1").arg(serial, 3, 10, QLatin1Char('0'));
    qso.sentExchangeFields = {{QStringLiteral("report"), QStringLiteral("599")},
                              {QStringLiteral("serial"), sentSerial}};
    qso.receivedExchangeFields = {{QStringLiteral("report"), QStringLiteral("599")},
                                  {QStringLiteral("serial"), received}};
    qso.sentExchange = QStringLiteral("599 %1").arg(sentSerial);
    qso.receivedExchange = QStringLiteral("599 %1").arg(received);
    qso.points = 1;
    return qso;
}

} // namespace

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(contests);
    QApplication application(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    if (argc < 2) {
        QTextStream(stderr) << "Usage: doc_screenshots <output-directory>\n";
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(output);

    QTemporaryDir settingsDir;
    QTemporaryDir dataDir;
    QCoreApplication::setOrganizationName(QStringLiteral("ContestLoggerDocs"));
    QCoreApplication::setApplicationName(QStringLiteral("ContestLoggerDocs"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    {
        QSettings settings;
        settings.setValue(QStringLiteral("station/callsign"), QStringLiteral("SN6U"));
        settings.setValue(QStringLiteral("station/operatorName"), QStringLiteral("Jan Kowalski"));
        settings.setValue(QStringLiteral("station/grid"), QStringLiteral("JO81"));
        settings.setValue(QStringLiteral("station/powerClass"), QStringLiteral("LOW"));
        settings.setValue(QStringLiteral("station/email"), QStringLiteral("sn6u@example.com"));
        settings.setValue(QStringLiteral("station/city"), QStringLiteral("Wroclaw"));
        settings.setValue(QStringLiteral("station/country"), QStringLiteral("Poland"));
        settings.setValue(QStringLiteral("radio/A/backend"),
                          static_cast<int>(RadioBackendType::Mock));
        settings.setValue(QStringLiteral("radio/A/autoConnect"), true);
    }

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    database.setDatabaseName(dataDir.filePath(QStringLiteral("docs.sqlite")));
    if (!database.open()) {
        QTextStream(stderr) << "Unable to open database\n";
        return 1;
    }
    SqliteRepository repository(database);
    QString error;
    if (!repository.initialize(&error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }

    QFile yamlFile(QStringLiteral(":/contests/cqwpx-cw.yaml"));
    if (!yamlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream(stderr) << "Missing CQ WPX CW resource\n";
        return 1;
    }
    const QString yaml = QString::fromUtf8(yamlFile.readAll());
    ContestDefinition definition;
    if (!ContestDefinitionLoader().loadText(yaml, QStringLiteral("cqwpx-cw.yaml"),
                                            definition, &error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    Contest log;
    log.name = QStringLiteral("SN6U CQ WPX CW 2026");
    log.stationCallsign = QStringLiteral("SN6U");
    log.startsAtUtc = QDateTime::currentDateTimeUtc();
    log.properties = {{QStringLiteral("CATEGORY-OPERATOR"), QStringLiteral("SINGLE-OP")},
                      {QStringLiteral("CATEGORY-ASSISTED"), QStringLiteral("NON-ASSISTED")},
                      {QStringLiteral("CATEGORY-POWER"), QStringLiteral("LOW")},
                      {QStringLiteral("CATEGORY-BAND"), QStringLiteral("ALL")},
                      {QStringLiteral("CATEGORY-MODE"), QStringLiteral("CW")},
                      {QStringLiteral("CLUB"), QStringLiteral("SP DX CLUB")}};
    const qint64 contestId = repository.createContestLog(definition, yaml, log, &error);
    if (contestId <= 0) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-3 * 3600);
    const struct {
        const char *call;
        const char *band;
        qint64 frequency;
        const char *received;
    } contacts[] = {
        {"K3LR", "20m", 14'012'000, "23"},   {"OK1NA", "20m", 14'018'300, "11"},
        {"LA3MN", "20m", 14'021'500, "1"},   {"DL1ABC", "20m", 14'025'100, "142"},
        {"SP5ZZ", "20m", 14'030'200, "57"},  {"W1AW", "40m", 7'010'500, "310"},
        {"EA8CN", "40m", 7'012'000, "88"},   {"9A1A", "40m", 7'015'700, "1204"},
        {"OH2BH", "40m", 7'020'000, "64"},   {"K4JNN", "80m", 3'512'000, "90"},
        {"G4BUO", "80m", 3'520'000, "33"},   {"S50A", "80m", 3'525'500, "771"}};
    int serial = 1;
    for (const auto &contact : contacts) {
        Qso qso = makeQso(contestId, serial, QString::fromLatin1(contact.call),
                          QString::fromLatin1(contact.band), contact.frequency,
                          QString::fromLatin1(contact.received),
                          start.addSecs(serial * 420));
        if (!repository.saveQso(qso, &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        ++serial;
    }
    const struct {
        const char *call;
        qint64 frequency;
        const char *spotter;
    } spots[] = {{"PJ2T", 14'005'000, "W3LPL"}, {"CN3A", 14'017'500, "DL8LAS"},
                 {"OK1NA", 14'018'300, "OM3RM"}, {"P40W", 14'026'000, "K1TTT"},
                 {"ZF2MJ", 14'033'000, "N2IC"},  {"V26K", 14'041'200, "VE3EJ"}};
    int age = 900;
    for (const auto &item : spots) {
        Spot spot;
        spot.callsign = QString::fromLatin1(item.call);
        spot.frequencyHz = item.frequency;
        spot.band = QStringLiteral("20m");
        spot.mode = QStringLiteral("CW");
        spot.spotter = QString::fromLatin1(item.spotter);
        spot.comment = QStringLiteral("CW 28 dB 30 WPM CQ");
        spot.timestampUtc = QDateTime::currentDateTimeUtc().addSecs(-age);
        spot.source = QStringLiteral("cluster");
        age -= 120;
        if (!repository.saveSpot(spot, &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
    }

    MainWindow window(repository);
    window.resize(1680, 1000);
    window.show();
    wait(300);
    auto *selector = window.findChild<QComboBox *>(QStringLiteral("contestSelector"));
    const int index = selector != nullptr
        ? selector->findText(QStringLiteral("SN6U CQ WPX"), Qt::MatchContains) : -1;
    if (index < 0) {
        QTextStream(stderr) << "Demo log not found in the selector\n";
        return 1;
    }
    selector->setCurrentIndex(index);
    wait(2500);
    if (auto *call = window.findChild<QLineEdit *>(QStringLiteral("callsignInputA"))) {
        call->setFocus();
        call->setText(QStringLiteral("OK1N"));
    }
    wait(500);

    bool ok = save(&window, output, QStringLiteral("main-window"));

    const auto trigger = [&window](const QString &text) {
        return [&window, text] {
            if (QAction *action = findAction(&window, text)) {
                action->trigger();
            } else {
                QTextStream(stderr) << "Action not found: " << text << '\n';
            }
        };
    };
    ok &= captureModal(trigger(QStringLiteral("&New log...")), output,
                       QStringLiteral("new-log"), [](QDialog *dialog) {
                           dialog->resize(1100, 720);
                           if (auto *list = dialog->findChild<QListWidget *>(
                                   QStringLiteral("newLogContestList"))) {
                               const auto items = list->findItems(
                                   QStringLiteral("CQ WPX Contest"), Qt::MatchStartsWith);
                               if (!items.isEmpty()) {
                                   list->setCurrentItem(items.first());
                               }
                           }
                       });
    ok &= captureModal(trigger(QStringLiteral("Log &properties...")), output,
                       QStringLiteral("log-properties"),
                       [](QDialog *dialog) { dialog->resize(1100, 680); });
    ok &= captureModal(trigger(QStringLiteral("&Operator and station...")), output,
                       QStringLiteral("operator-station"));
    ok &= captureModal(trigger(QStringLiteral("Radio A CAT settings...")), output,
                       QStringLiteral("radio-cat"),
                       [](QDialog *dialog) { dialog->resize(560, 500); });
    ok &= captureModal(trigger(QStringLiteral("&DX cluster...")), output,
                       QStringLiteral("dx-cluster"));
    ok &= captureModal(trigger(QStringLiteral("CW keyer...")), output,
                       QStringLiteral("cw-keyer"));
    ok &= captureModal([&window] {
        QMetaObject::invokeMethod(&window, "editSelectedQso", Q_ARG(int, 0), Q_ARG(int, 0));
    }, output, QStringLiteral("edit-qso"));

    window.close();
    return ok ? 0 : 1;
}
