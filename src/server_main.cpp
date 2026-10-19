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
    parser.addOption(addressOption);
    parser.addOption(portOption);
    parser.addOption(certificateOption);
    parser.addOption(privateKeyOption);
    parser.addOption(tokenFileOption);
    parser.addOption(maximumConnectionsOption);
    parser.process(application);

    QTextStream errorOutput(stderr);
    if (parser.isSet(checkOption) == parser.isSet(listenOption)) {
        errorOutput << "Choose exactly one of --check or --listen.\n";
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

    logger::CentralRepository repository;
    QString error;
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
