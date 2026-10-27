#include "logger/data/application_data.h"
#include "logger/contest/contest_definition_loader.h"
#include "logger/data/sqlite_repository.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QRegularExpression>

#include <utility>

namespace logger {
namespace {

bool writeDefinition(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents) != contents.size() || !file.commit()) {
        *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool copyLegacyApplicationData(const QString &directory, QString *error)
{
    const QString legacyDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (legacyDirectory.isEmpty()
        || QDir::cleanPath(legacyDirectory) == QDir::cleanPath(directory)
        || !QFileInfo(legacyDirectory).isDir()) {
        return true;
    }

    const QDir legacy(legacyDirectory);
    QDirIterator iterator(legacyDirectory, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString source = iterator.next();
        const QString destination = QDir(directory).filePath(
            legacy.relativeFilePath(source));
        if (QFileInfo::exists(destination)) {
            continue;
        }
        if (!QDir().mkpath(QFileInfo(destination).path())
            || !QFile::copy(source, destination)) {
            *error = QStringLiteral("%1: could not migrate application data.")
                         .arg(source);
            return false;
        }
    }
    return true;
}

} // namespace

bool backupLogIfDue(SqliteRepository &repository, QString *error)
{
    error->clear();
    QSettings settings;
    if (!repository.isStandaloneLog()
        || !settings.value(QStringLiteral("backups/automaticEnabled"), true).toBool()) {
        return true;
    }
    const int interval = qBound(1, settings.value(
        QStringLiteral("backups/automaticIntervalDays"), 1).toInt(), 365);
    const int retention = qBound(1, settings.value(
        QStringLiteral("backups/automaticRetentionCount"), 30).toInt(), 365);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        QFileInfo(repository.databasePath()).canonicalFilePath().toUtf8(),
        QCryptographicHash::Sha256).toHex());
    const QDir directory(QDir(applicationDataDirectory()).filePath(
        QStringLiteral("backups/") + key));
    if (!QDir().mkpath(directory.path())) {
        *error = QStringLiteral("Could not create automatic backup directory: %1")
                     .arg(directory.path());
        return false;
    }
    const auto files = directory.entryInfoList(
        {QStringLiteral("automatic-*.sqlite")}, QDir::Files, QDir::Name | QDir::Reversed);
    static const QRegularExpression pattern(
        QStringLiteral("^automatic-(\\d{4}-\\d{2}-\\d{2})\\.sqlite$"));
    QDate latest;
    for (const QFileInfo &file : files) {
        const auto match = pattern.match(file.fileName());
        const QDate date = QDate::fromString(match.captured(1), QStringLiteral("yyyy-MM-dd"));
        if (date.isValid() && date > latest) {
            latest = date;
        }
    }
    const QDate today = QDate::currentDate();
    if (!latest.isValid() || latest.daysTo(today) >= interval) {
        const QString path = directory.filePath(QStringLiteral("automatic-%1.sqlite")
            .arg(today.toString(QStringLiteral("yyyy-MM-dd"))));
        if (!QFileInfo::exists(path) && !repository.backupTo(path, error)) {
            return false;
        }
    }
    const auto backups = directory.entryInfoList(
        {QStringLiteral("automatic-*.sqlite")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (qsizetype index = retention; index < backups.size(); ++index) {
        if (!QFile::remove(backups.at(index).absoluteFilePath())) {
            *error = QStringLiteral("Could not remove old backup: %1")
                         .arg(backups.at(index).absoluteFilePath());
            return false;
        }
    }
    return true;
}

QString applicationDataDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QCoreApplication::applicationName());
}

QString contestDefinitionsDirectory()
{
    return QDir(applicationDataDirectory()).filePath(QStringLiteral("contests"));
}

QString contestDefinitionVersionsDirectory()
{
    return QDir(contestDefinitionsDirectory()).filePath(QStringLiteral("versions"));
}

bool initializeApplicationData(QString *error)
{
    error->clear();
    const QString directory = applicationDataDirectory();
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        *error = QCoreApplication::translate(
            "ApplicationData", "Could not create the application data directory: %1")
                     .arg(directory);
        return false;
    }
    if (!copyLegacyApplicationData(directory, error)) {
        return false;
    }
    if (!QDir().mkpath(contestDefinitionsDirectory())) {
        *error = QCoreApplication::translate(
            "ApplicationData", "Could not create the application data directory: %1")
                     .arg(directory);
        return false;
    }

    // Read the previous native settings before redirecting default QSettings.
    QSettings previous;
    const QString settingsRoot = QDir(directory).filePath(QStringLiteral("config"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot);
    QSettings settings;
    settings.setFallbacksEnabled(false);
    if (!QFileInfo::exists(settings.fileName())) {
        const QStringList keys = previous.allKeys();
        if (previous.status() != QSettings::NoError) {
            *error = QCoreApplication::translate(
                "ApplicationData", "Could not read the previous application settings.");
            return false;
        }
        for (const QString &key : keys) {
            settings.setValue(key, previous.value(key));
        }
        settings.setValue(QStringLiteral("application/settingsVersion"), 1);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        *error = QCoreApplication::translate(
            "ApplicationData", "Could not write the application settings: %1")
                     .arg(settings.fileName());
        return false;
    }
    return true;
}

bool installBundledContestDefinitions(QString *error)
{
    error->clear();
    const QDir target(contestDefinitionsDirectory());
    if (!QDir().mkpath(target.path())) {
        *error = QStringLiteral("Could not create contest definitions directory: %1")
                     .arg(target.path());
        return false;
    }
    const QDir resources(QStringLiteral(":/contests"));
    const QStringList names = resources.entryList(
        {QStringLiteral("*.yaml"), QStringLiteral("*.yml")}, QDir::Files, QDir::Name);
    if (names.isEmpty()) {
        *error = QStringLiteral("Bundled contest definitions are missing.");
        return false;
    }
    for (const QString &name : names) {
        const QString path = target.filePath(name);
        if (QFileInfo::exists(path)) {
            continue;
        }
        QFile source(resources.filePath(name));
        if (!source.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("%1: %2").arg(source.fileName(), source.errorString());
            return false;
        }
        const QByteArray contents = source.readAll();
        if (source.error() != QFileDevice::NoError) {
            *error = QStringLiteral("%1: %2").arg(source.fileName(), source.errorString());
            return false;
        }
        if (!writeDefinition(path, contents, error)) {
            return false;
        }
    }
    return true;
}

QStringList contestDefinitionFiles()
{
    const QDir directory(contestDefinitionsDirectory());
    QStringList paths;
    const QStringList names = directory.entryList(
        {QStringLiteral("*.yaml"), QStringLiteral("*.yml")}, QDir::Files, QDir::Name);
    for (const QString &name : names) {
        paths.append(directory.filePath(name));
    }
    return paths;
}

bool loadContestDefinitionFiles(QVector<ContestDefinitionFile> *files, QString *error)
{
    error->clear();
    const QFileInfo directory(contestDefinitionsDirectory());
    if (!directory.isDir() || !directory.isReadable()) {
        *error = QStringLiteral("Could not read contest definitions directory: %1")
                     .arg(directory.filePath());
        return false;
    }
    QVector<ContestDefinitionFile> result;
    QSet<QString> ids;
    for (const QString &path : contestDefinitionFiles()) {
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
            *error = QStringLiteral("%1: %2").arg(path, source.errorString());
            return false;
        }
        ContestDefinitionFile entry;
        entry.path = path;
        entry.yaml = QString::fromUtf8(source.readAll());
        if (source.error() != QFileDevice::NoError) {
            *error = QStringLiteral("%1: %2").arg(path, source.errorString());
            return false;
        }
        if (!ContestDefinitionLoader().loadText(
                entry.yaml, path, entry.definition, error)) {
            return false;
        }
        if (ids.contains(entry.definition.id)) {
            *error = QStringLiteral("%1: Duplicate contest definition ID '%2'.")
                         .arg(path, entry.definition.id);
            return false;
        }
        ids.insert(entry.definition.id);
        result.append(std::move(entry));
    }
    *files = std::move(result);
    return true;
}

bool storeContestDefinitionFile(const QString &yaml, QString *error)
{
    error->clear();
    ContestDefinition definition;
    if (!ContestDefinitionLoader().loadText(
            yaml, QStringLiteral("import"), definition, error)) {
        return false;
    }
    const QDir directory(contestDefinitionsDirectory());
    if (!QDir().mkpath(directory.path())) {
        *error = QStringLiteral("Could not create contest definitions directory: %1")
                     .arg(directory.path());
        return false;
    }
    QVector<ContestDefinitionFile> files;
    if (!loadContestDefinitionFiles(&files, error)) {
        return false;
    }
    QString target = directory.filePath(definition.id + QStringLiteral(".yaml"));
    for (const ContestDefinitionFile &file : files) {
        if (file.definition.id == definition.id) {
            target = file.path;
            break;
        }
        if (file.path == target) {
            *error = QStringLiteral("%1: File belongs to a different contest ID.")
                         .arg(target);
            return false;
        }
    }
    QFileInfo currentInfo(target);
    if (currentInfo.isFile()) {
        QFile current(target);
        if (!current.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("%1: %2").arg(target, current.errorString());
            return false;
        }
        const QByteArray previous = current.readAll();
        if (previous != yaml.toUtf8()) {
            const QByteArray digest = QCryptographicHash::hash(
                previous, QCryptographicHash::Sha256).toHex().left(12);
            const QString versionDirectory = QDir(
                contestDefinitionVersionsDirectory()).filePath(definition.id);
            if (!QDir().mkpath(versionDirectory)) {
                *error = QStringLiteral("Could not create contest version directory: %1")
                             .arg(versionDirectory);
                return false;
            }
            const QString versionPath = QDir(versionDirectory).filePath(
                QStringLiteral("%1-%2.yaml")
                    .arg(QDateTime::currentDateTimeUtc()
                             .toString(QStringLiteral("yyyyMMdd'T'hhmmsszzz'Z'")),
                         QString::fromLatin1(digest)));
            if (!writeDefinition(versionPath, previous, error)) {
                return false;
            }
        }
    }
    return writeDefinition(target, yaml.toUtf8(), error);
}

} // namespace logger
