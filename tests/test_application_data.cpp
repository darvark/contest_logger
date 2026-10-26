#include "logger/data/application_data.h"
#include "logger/data/sqlite_repository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace logger;

class ApplicationDataTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void firstStartCreatesSettingsDatabaseAndDefinitions();
    void migratesPreviousSettingsOnlyOnce();
    void preservesEditedDefinitionsAndDiscoversYamlAndYml();
    void reportsInvalidAndDuplicateDefinitions();
    void importsPersistAndReplaceTheExistingFile();
    void reportsDirectoryAndSettingsCreationFailures();

private:
    QTemporaryDir m_legacySettings;
};

void ApplicationDataTest::initTestCase()
{
    Q_INIT_RESOURCE(contests);
    QVERIFY(m_legacySettings.isValid());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("ContestLoggerStartupTest"));
}

void ApplicationDataTest::init()
{
    QCoreApplication::setApplicationName(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       m_legacySettings.path());
}

void ApplicationDataTest::cleanup()
{
    const QStringList directories{
        applicationDataDirectory(),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)};
    for (const QString &directory : directories) {
        if (QFileInfo(directory).isDir()) {
            QVERIFY(QDir(directory).removeRecursively());
        } else if (QFileInfo::exists(directory)) {
            QVERIFY(QFile::remove(directory));
        }
    }
}

void ApplicationDataTest::firstStartCreatesSettingsDatabaseAndDefinitions()
{
    QCOMPARE(applicationDataDirectory(),
             QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                 .filePath(QCoreApplication::applicationName()));
    QVERIFY(!QFileInfo::exists(applicationDataDirectory()));
    const QString legacyDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(legacyDirectory));
    QFile legacyMarker(QDir(legacyDirectory).filePath(
        QStringLiteral("legacy-marker.txt")));
    QVERIFY(legacyMarker.open(QIODevice::WriteOnly));
    QCOMPARE(legacyMarker.write(QByteArrayLiteral("legacy data")), 11);
    legacyMarker.close();
    QString error;
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QFile migratedMarker(QDir(applicationDataDirectory()).filePath(
        QStringLiteral("legacy-marker.txt")));
    QVERIFY(migratedMarker.open(QIODevice::ReadOnly));
    QCOMPARE(migratedMarker.readAll(), QByteArrayLiteral("legacy data"));
    QVERIFY(QFileInfo::exists(QSettings().fileName()));
    QVERIFY(QSettings().fileName().startsWith(
        applicationDataDirectory() + QLatin1Char('/')));
    QCOMPARE(QSettings().format(), QSettings::IniFormat);
    QVERIFY2(installBundledContestDefinitions(&error), qPrintable(error));
    QVector<ContestDefinitionFile> files;
    QVERIFY2(loadContestDefinitionFiles(&files, &error), qPrintable(error));
    QCOMPARE(files.size(), 22);
    for (const ContestDefinitionFile &file : files) {
        QVERIFY(QFileInfo(file.path).isWritable());
    }

    const QString connection = QCoreApplication::applicationName();
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QDir(applicationDataDirectory()).filePath(
            QStringLiteral("contest-logger.sqlite")));
        QVERIFY2(database.open(), qPrintable(database.lastError().text()));
        SqliteRepository repository(database);
        QVERIFY2(repository.initialize(&error), qPrintable(error));
        QVERIFY(QFileInfo(database.databaseName()).size() > 0);
    }
    QSqlDatabase::removeDatabase(connection);

    QSettings().setValue(QStringLiteral("station/callsign"), QStringLiteral("SP9TEST"));
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QCOMPARE(QSettings().value(QStringLiteral("station/callsign")).toString(),
             QStringLiteral("SP9TEST"));
}

void ApplicationDataTest::migratesPreviousSettingsOnlyOnce()
{
    {
        QSettings previous;
        previous.setValue(QStringLiteral("station/callsign"), QStringLiteral("SP1OLD"));
        previous.setValue(QStringLiteral("radio/model"), 123);
        previous.sync();
        QCOMPARE(previous.status(), QSettings::NoError);
    }
    QString error;
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QCOMPARE(QSettings().value(QStringLiteral("station/callsign")).toString(),
             QStringLiteral("SP1OLD"));
    QCOMPARE(QSettings().value(QStringLiteral("radio/model")).toInt(), 123);
    {
        QSettings current;
        current.remove(QStringLiteral("radio/model"));
        current.setValue(QStringLiteral("station/callsign"), QStringLiteral("SP2NEW"));
        current.sync();
    }
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QCOMPARE(QSettings().value(QStringLiteral("station/callsign")).toString(),
             QStringLiteral("SP2NEW"));
    QVERIFY(!QSettings().contains(QStringLiteral("radio/model")));
}

void ApplicationDataTest::preservesEditedDefinitionsAndDiscoversYamlAndYml()
{
    QString error;
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QVERIFY2(installBundledContestDefinitions(&error), qPrintable(error));
    QVector<ContestDefinitionFile> files;
    QVERIFY2(loadContestDefinitionFiles(&files, &error), qPrintable(error));
    const ContestDefinitionFile original = files.first();
    const QString edited = original.yaml + QStringLiteral("\n# User modification\n");
    {
        QFile file(original.path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(edited.toUtf8()), edited.toUtf8().size());
    }
    const QString customPath = QDir(contestDefinitionsDirectory()).filePath(
        QStringLiteral("custom.yml"));
    QString custom = original.yaml;
    custom.replace(QStringLiteral("id: %1").arg(original.definition.id),
                   QStringLiteral("id: custom-test"));
    {
        QFile file(customPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(custom.toUtf8()), custom.toUtf8().size());
    }
    QVERIFY2(installBundledContestDefinitions(&error), qPrintable(error));
    QVERIFY2(loadContestDefinitionFiles(&files, &error), qPrintable(error));
    QCOMPARE(files.size(), 23);
    bool foundCustom = false;
    bool foundEdited = false;
    for (const ContestDefinitionFile &file : files) {
        foundCustom |= file.definition.id == QStringLiteral("custom-test");
        foundEdited |= file.path == original.path && file.yaml == edited;
    }
    QVERIFY(foundCustom);
    QVERIFY(foundEdited);
}

void ApplicationDataTest::reportsInvalidAndDuplicateDefinitions()
{
    QString error;
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QVERIFY2(installBundledContestDefinitions(&error), qPrintable(error));
    QVector<ContestDefinitionFile> files;
    QVERIFY2(loadContestDefinitionFiles(&files, &error), qPrintable(error));
    const ContestDefinitionFile original = files.first();
    const QString invalidPath = QDir(contestDefinitionsDirectory()).filePath(
        QStringLiteral("invalid.yaml"));
    {
        QFile file(invalidPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("invalid: [");
    }
    QVERIFY(!loadContestDefinitionFiles(&files, &error));
    QVERIFY2(error.contains(invalidPath), qPrintable(error));
    QVERIFY(QFile::remove(invalidPath));
    const QString duplicatePath = QDir(contestDefinitionsDirectory()).filePath(
        QStringLiteral("duplicate.yml"));
    QVERIFY(QFile::copy(original.path, duplicatePath));
    QVERIFY(!loadContestDefinitionFiles(&files, &error));
    QVERIFY2(error.contains(QStringLiteral("Duplicate")), qPrintable(error));
}

void ApplicationDataTest::importsPersistAndReplaceTheExistingFile()
{
    QString error;
    QVERIFY2(initializeApplicationData(&error), qPrintable(error));
    QFile source(QStringLiteral(":/contests/serial-sprint.yaml"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QString yaml = QString::fromUtf8(source.readAll());
    yaml.replace(QStringLiteral("id: serial-sprint"), QStringLiteral("id: imported-test"));
    QVERIFY2(storeContestDefinitionFile(yaml, &error), qPrintable(error));
    const QDir directory(contestDefinitionsDirectory());
    QVERIFY(QFile::rename(directory.filePath(QStringLiteral("imported-test.yaml")),
                          directory.filePath(QStringLiteral("renamed.yml"))));
    yaml += QStringLiteral("\n# Updated import\n");
    QVERIFY2(storeContestDefinitionFile(yaml, &error), qPrintable(error));
    QVector<ContestDefinitionFile> files;
    QVERIFY2(loadContestDefinitionFiles(&files, &error), qPrintable(error));
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().yaml, yaml);
    QCOMPARE(QFileInfo(files.first().path).fileName(), QStringLiteral("renamed.yml"));
    const QDir versions(QDir(contestDefinitionVersionsDirectory())
                            .filePath(QStringLiteral("imported-test")));
    const QStringList versionFiles = versions.entryList(
        {QStringLiteral("*.yaml")}, QDir::Files);
    QCOMPARE(versionFiles.size(), 1);
    QFile savedVersion(versions.filePath(versionFiles.first()));
    QVERIFY(savedVersion.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(QString::fromUtf8(savedVersion.readAll()).contains(
        QStringLiteral("schema_version: 1")));
    QVERIFY(!versionFiles.first().isEmpty());
}

void ApplicationDataTest::reportsDirectoryAndSettingsCreationFailures()
{
    const QString directory = applicationDataDirectory();
    QVERIFY(QDir().mkpath(QFileInfo(directory).absolutePath()));
    {
        QFile blocker(directory);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
    }
    QString error;
    QVERIFY(!initializeApplicationData(&error));
    QVERIFY2(error.contains(directory), qPrintable(error));
    QVERIFY(QFile::remove(directory));
    QVERIFY(QDir().mkpath(directory));
    {
        QFile blocker(QDir(directory).filePath(QStringLiteral("config")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
    }
    QVERIFY(!initializeApplicationData(&error));
    QVERIFY2(error.contains(QStringLiteral("settings")), qPrintable(error));
}

QTEST_MAIN(ApplicationDataTest)
#include "test_application_data.moc"
