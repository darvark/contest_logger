#include "logger/data/sqlite_repository.h"
#include "logger/ui/main_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QStandardPaths>

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

    const QString dataDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(dataDirectory)) {
        QMessageBox::critical(nullptr, QObject::tr("Startup error"),
                              QObject::tr("Could not create the application data directory."));
        return 1;
    }

    QLockFile applicationLock(dataDirectory + QStringLiteral("/contest-logger.lock"));
    if (!applicationLock.tryLock(0)) {
        QMessageBox::critical(nullptr, QObject::tr("Application already running"),
                              QObject::tr("Close the other Contest Logger instance first."));
        return 1;
    }
    const QString databasePath =
        dataDirectory + QStringLiteral("/contest-logger.sqlite");
    if (parser.isSet(restoreOption)) {
        QString error;
        if (!logger::SqliteRepository::restoreFromBackup(
                databasePath, parser.value(restoreOption), &error)) {
            QMessageBox::critical(nullptr, QObject::tr("Database restore failed"), error);
            return 1;
        }
        QMessageBox::information(nullptr, QObject::tr("Database restored"),
                                 QObject::tr("The verified backup was restored. "
                                             "Start Contest Logger normally."));
        return 0;
    }

    const QString connectionName = QStringLiteral("contest-logger-main");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (!database.open()) {
            QMessageBox::critical(nullptr, QObject::tr("Database error"),
                                  database.lastError().text());
            return 1;
        }

        logger::SqliteRepository repository(database);
        QString error;
        const QFileInfo currentDatabase(databasePath);
        if (currentDatabase.exists() && currentDatabase.size() > 0) {
            const QString backupDirectory = dataDirectory + QStringLiteral("/backups");
            if (!QDir().mkpath(backupDirectory)) {
                QMessageBox::critical(nullptr, QObject::tr("Backup directory error"),
                                      QObject::tr("Could not create the automatic backup directory."));
                return 1;
            }
            const QString backupPath = backupDirectory
                + QStringLiteral("/automatic-%1.sqlite")
                      .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
            if (!QFileInfo::exists(backupPath)
                && !repository.backupTo(backupPath, &error)) {
                QMessageBox::critical(
                    nullptr, QObject::tr("Automatic database backup failed"),
                    QObject::tr("A safety backup could not be created; the database "
                                "was not opened for writing.\n\n%1").arg(error));
                return 1;
            }
            const QFileInfoList backups = QDir(backupDirectory).entryInfoList(
                {QStringLiteral("automatic-*.sqlite")},
                QDir::Files, QDir::Name);
            const QDate oldestDate = QDate::currentDate().addDays(-30);
            static const QRegularExpression backupName(
                QStringLiteral("^automatic-(\\d{4}-\\d{2}-\\d{2})\\.sqlite$"));
            for (const QFileInfo &backup : backups) {
                const auto match = backupName.match(backup.fileName());
                const QDate backupDate = match.hasMatch()
                    ? QDate::fromString(match.captured(1), QStringLiteral("yyyy-MM-dd"))
                    : QDate();
                if (backupDate.isValid() && backupDate < oldestDate) {
                    QFile::remove(backup.absoluteFilePath());
                }
            }
        }
        if (!repository.initialize(&error)) {
            QMessageBox::critical(nullptr, QObject::tr("Database initialization failed"), error);
            return 1;
        }

        logger::MainWindow window(repository);
        window.show();
        const int result = application.exec();
        repository = logger::SqliteRepository(QSqlDatabase());
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return result;
    }
}
