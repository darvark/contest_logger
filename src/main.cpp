#include "logger/data/sqlite_repository.h"
#include "logger/data/application_data.h"
#include "logger/ui/main_window.h"

#include <QApplication>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDate>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QLockFile>
#include <QMessageBox>
#include <QPainter>
#include <QLocale>
#include <QPixmap>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSettings>
#include <QSplashScreen>
#include <QTranslator>

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(contests);
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP")
            .contains(QStringLiteral("KDE"), Qt::CaseInsensitive)
        && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME")) {
        qputenv("QT_QPA_PLATFORMTHEME", QByteArrayLiteral("generic"));
    }
#endif
    QApplication application(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QCoreApplication::setOrganizationName(QStringLiteral("ContestLogger"));
    QCoreApplication::setApplicationName(QStringLiteral("ContestLogger"));
    QSettings settings;
    QString language = settings.value(
        QStringLiteral("ui/language"), QStringLiteral("en")).toString();
    if (language != QLatin1String("en") && language != QLatin1String("pl")) {
        qWarning().noquote() << "Unsupported saved UI language; using English.";
        language = QStringLiteral("en");
    }
    QLocale::setDefault(language == QLatin1String("pl")
                            ? QLocale(QLocale::Polish, QLocale::Poland)
                            : QLocale(QLocale::English, QLocale::UnitedStates));
    auto *translator = new QTranslator(&application);
    translator->setObjectName(QStringLiteral("uiTranslator"));
    if (language == QLatin1String("pl")) {
        if (!translator->load(QStringLiteral(":/i18n/contest_logger_pl.qm"))) {
            QMessageBox::critical(
                nullptr, QStringLiteral("Startup error"),
                QStringLiteral("The Polish translation catalog could not be loaded."));
            return 1;
        }
        application.installTranslator(translator);
    }
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Cross-platform amateur radio contest logger"));
    parser.addHelpOption();
    const QCommandLineOption restoreOption(
        QStringList{QStringLiteral("restore-backup")},
        QStringLiteral("Restore the database from a verified backup and exit."),
        QStringLiteral("backup-file"));
    parser.addOption(restoreOption);
    parser.process(application);

    QPixmap splashImage(520, 180);
    splashImage.fill(QColor(QStringLiteral("#ECECEC")));
    {
        QPainter painter(&splashImage);
        painter.fillRect(0, 0, 8, splashImage.height(), QColor(QStringLiteral("#20B2AA")));
        painter.setPen(QColor(QStringLiteral("#202020")));
        QFont titleFont = application.font();
        titleFont.setPointSize(22);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(QRect(36, 34, 448, 42), Qt::AlignLeft | Qt::AlignVCenter,
                         QCoreApplication::applicationName());
        painter.setFont(application.font());
        painter.setPen(QColor(QStringLiteral("#555555")));
        painter.drawText(QRect(36, 90, 448, 24), Qt::AlignLeft | Qt::AlignVCenter,
                         QObject::tr("Starting up..."));
    }
    QSplashScreen splash(splashImage);
    splash.show();
    application.processEvents();
    const auto updateSplash = [&splash, &application](const QString &message) {
        splash.showMessage(message, Qt::AlignBottom | Qt::AlignHCenter,
                           QColor(QStringLiteral("#202020")));
        application.processEvents();
    };
    const auto showStartupError = [&splash](QWidget *parent, const QString &title,
                                            const QString &message) {
        splash.hide();
        QMessageBox::critical(parent, title, message);
    };

    const QString dataDirectory = logger::applicationDataDirectory();
    if (dataDirectory.isEmpty() || !QDir().mkpath(dataDirectory)) {
        showStartupError(nullptr, QObject::tr("Startup error"),
                         QObject::tr("Could not create the application data directory."));
        return 1;
    }

    QLockFile applicationLock(dataDirectory + QStringLiteral("/contest-logger.lock"));
    if (!applicationLock.tryLock(0)) {
        showStartupError(nullptr, QObject::tr("Application already running"),
                         QObject::tr("Close the other Contest Logger instance first."));
        return 1;
    }
    QString initializationError;
    if (!logger::initializeApplicationData(&initializationError)
        || !logger::installBundledContestDefinitions(&initializationError)) {
        showStartupError(nullptr, QObject::tr("Startup error"), initializationError);
        return 1;
    }
    const QString databasePath =
        QSettings().value(QStringLiteral("session/logFile")).toString();
    if (parser.isSet(restoreOption)) {
        QString error;
        if (databasePath.isEmpty()) {
            showStartupError(nullptr, QObject::tr("Database restore failed"),
                             QObject::tr("Open a log file before restoring its backup."));
            return 1;
        }
        updateSplash(QObject::tr("Restoring database backup..."));
        if (!logger::SqliteRepository::restoreFromBackup(
                databasePath, parser.value(restoreOption), &error)) {
            showStartupError(nullptr, QObject::tr("Database restore failed"), error);
            return 1;
        }
        splash.hide();
        QMessageBox::information(nullptr, QObject::tr("Database restored"),
                                 QObject::tr("The verified backup was restored. "
                                             "Start Contest Logger normally."));
        return 0;
    }

    const QString connectionName = QStringLiteral("contest-logger-main");
    {
        updateSplash(QObject::tr("Opening database..."));
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(QStringLiteral(":memory:"));
        if (!database.open()) {
            showStartupError(nullptr, QObject::tr("Database error"),
                             database.lastError().text());
            return 1;
        }

        logger::SqliteRepository repository(database);
        QString error;
        if (!repository.initialize(&error)) {
            showStartupError(nullptr, QObject::tr("Database initialization failed"), error);
            return 1;
        }
        bool openedLog = false;
        if (!databasePath.isEmpty()) {
            openedLog = repository.openLogFile(databasePath, &error);
            if (!openedLog) {
                splash.hide();
                QMessageBox::warning(
                    nullptr, QObject::tr("Unable to reopen log"),
                    error + QObject::tr("\nChoose File > Open log or create a new log."));
            }
        }
        if (openedLog && !logger::backupLogIfDue(repository, &error)) {
            showStartupError(nullptr, QObject::tr("Automatic database backup failed"), error);
            return 1;
        }

        updateSplash(QObject::tr("Preparing the main window..."));
        int result = 0;
        {
            logger::MainWindow window(repository);
            window.show();
            splash.finish(&window);
            result = application.exec();
        }
        {
            // Release every handle to the connection before removeDatabase().
            logger::SqliteRepository empty{QSqlDatabase()};
            repository.swap(empty);
        }
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return result;
    }
}
