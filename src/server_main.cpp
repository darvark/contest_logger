#include "logger/data/central_repository.h"
#include "logger/network/sync_server.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ContestLogger"));
    QCoreApplication::setApplicationName(QStringLiteral("ContestLoggerServer"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Contest Logger central database initialization check"));
    parser.addHelpOption();
    const QCommandLineOption dataDirectoryOption(
        QStringList{QStringLiteral("data-dir")},
        QStringLiteral("Central data directory."),
        QStringLiteral("directory"));
    const QCommandLineOption checkOption(
        QStringList{QStringLiteral("check")},
        QStringLiteral("Initialize/check the central database and exit."));
    const QCommandLineOption listenOption(
        QStringList{QStringLiteral("listen")},
        QStringLiteral("Run the TLS synchronization server."));
    const QCommandLineOption restoreOption(
        QStringList{QStringLiteral("restore-backup")},
        QStringLiteral("Restore a verified central database backup and exit."),
        QStringLiteral("backup-file"));
    const QCommandLineOption addressOption(
        QStringList{QStringLiteral("address")},
        QStringLiteral("Numeric bind address (default: 0.0.0.0)."),
        QStringLiteral("ip"), QStringLiteral("0.0.0.0"));
    const QCommandLineOption portOption(
        QStringList{QStringLiteral("port")},
        QStringLiteral("TCP port (default: 7443; use 0 for an ephemeral port)."),
        QStringLiteral("port"), QStringLiteral("7443"));
    const QCommandLineOption certificateOption(
        QStringList{QStringLiteral("tls-cert")},
        QStringLiteral("PEM TLS certificate chain."),
        QStringLiteral("file"));
    const QCommandLineOption privateKeyOption(
        QStringList{QStringLiteral("tls-key")},
        QStringLiteral("Unencrypted RSA PEM private key."),
        QStringLiteral("file"));
    const QCommandLineOption tokenFileOption(
        QStringList{QStringLiteral("tokens")},
        QStringLiteral("JSON station credential file."),
        QStringLiteral("file"));
    const QCommandLineOption maximumConnectionsOption(
        QStringList{QStringLiteral("max-connections")},
        QStringLiteral("Maximum active TLS sessions (1-256)."),
        QStringLiteral("count"), QStringLiteral("16"));
    parser.addOption(dataDirectoryOption);
    parser.addOption(checkOption);
    parser.addOption(listenOption);
    parser.addOption(restoreOption);
    parser.addOption(addressOption);
    parser.addOption(portOption);
    parser.addOption(certificateOption);
    parser.addOption(privateKeyOption);
    parser.addOption(tokenFileOption);
    parser.addOption(maximumConnectionsOption);
    parser.process(application);

    QTextStream errorOutput(stderr);
    const int selectedActions = static_cast<int>(parser.isSet(checkOption))
        + static_cast<int>(parser.isSet(listenOption))
        + static_cast<int>(parser.isSet(restoreOption));
    if (selectedActions != 1) {
        errorOutput << "Choose exactly one of --check, --listen or --restore-backup.\n";
        return 2;
    }

    QString dataDirectory = parser.value(dataDirectoryOption);
    if (dataDirectory.isEmpty()) {
        dataDirectory = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
    }
    if (dataDirectory.isEmpty()) {
        errorOutput << "No writable application data directory is available; "
                       "specify --data-dir.\n";
        return 2;
    }

    QString error;
    if (parser.isSet(restoreOption)) {
        if (!QDir().mkpath(dataDirectory)) {
            errorOutput << "Could not create the central data directory.\n";
            return 1;
        }
        const QString databasePath =
            QDir(dataDirectory).filePath(QStringLiteral("central-log.sqlite"));
        if (!logger::SqliteRepository::restoreFromBackup(
                databasePath, parser.value(restoreOption), &error)) {
            errorOutput << "Central database restore failed: " << error << '\n';
            return 1;
        }
        logger::CentralRepository restoredRepository;
        if (!restoredRepository.initialize(dataDirectory, &error)) {
            errorOutput << "Restored central database validation failed: "
                        << error << '\n';
            return 1;
        }
        QTextStream output(stdout);
        output << "Verified central database backup restored: "
               << QDir::toNativeSeparators(databasePath) << '\n'
               << "A pre-restore database, if present, was preserved beside it.\n";
        return 0;
    }

    logger::CentralRepository repository;
    if (!repository.initialize(dataDirectory, &error)) {
        errorOutput << "Central database check failed: " << error << '\n';
        return 1;
    }

    if (parser.isSet(listenOption)) {
        bool portValid = false;
        const uint portValue = parser.value(portOption).toUInt(&portValid);
        bool connectionsValid = false;
        const int maximumConnections =
            parser.value(maximumConnectionsOption).toInt(&connectionsValid);
        if (!portValid || portValue > 65535 || !connectionsValid
            || maximumConnections < 1 || maximumConnections > 256
            || parser.value(certificateOption).isEmpty()
            || parser.value(privateKeyOption).isEmpty()
            || parser.value(tokenFileOption).isEmpty()) {
            errorOutput << "Listen options are invalid or missing TLS/token files.\n";
            return 2;
        }
        logger::SyncServer server(repository);
        if (!server.listen(parser.value(addressOption), static_cast<quint16>(portValue),
                           parser.value(certificateOption),
                           parser.value(privateKeyOption),
                           parser.value(tokenFileOption),
                           maximumConnections, &error)) {
            errorOutput << "Could not start TLS synchronization server: "
                        << error << '\n';
            return 1;
        }
        QTextStream output(stdout);
        output << "TLS synchronization server listening on "
               << parser.value(addressOption) << ':' << server.serverPort() << '\n'
               << "Central database: "
               << QDir::toNativeSeparators(repository.databasePath()) << '\n';
        output.flush();
        return application.exec();
    }

    QTextStream output(stdout);
    output << "Central database is ready: "
           << QDir::toNativeSeparators(repository.databasePath()) << '\n'
           << "Backups: "
           << QDir::toNativeSeparators(repository.backupDirectory()) << '\n';
    return 0;
}
