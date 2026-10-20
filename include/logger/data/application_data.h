#pragma once

#include "logger/domain/contest_definition.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace logger {

QString applicationDataDirectory();
QString contestDefinitionsDirectory();
bool initializeApplicationData(QString *error);
bool installBundledContestDefinitions(QString *error);
QStringList contestDefinitionFiles();
struct ContestDefinitionFile {
    ContestDefinition definition;
    QString yaml;
    QString path;
};
bool loadContestDefinitionFiles(QVector<ContestDefinitionFile> *files, QString *error);
bool storeContestDefinitionFile(const QString &yaml, QString *error);

} // namespace logger
