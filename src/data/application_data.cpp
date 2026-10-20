#include "logger/data/application_data.h"
#include "logger/contest/contest_definition_loader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>

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

} // namespace

QString applicationDataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString contestDefinitionsDirectory()
{
    return QDir(applicationDataDirectory()).filePath(QStringLiteral("contests"));
}

bool initializeApplicationData(QString *error)
{
    error->clear();
    const QString directory = applicationDataDirectory();
    if (directory.isEmpty() || !QDir().mkpath(contestDefinitionsDirectory())) {
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
    return writeDefinition(target, yaml.toUtf8(), error);
}

} // namespace logger
