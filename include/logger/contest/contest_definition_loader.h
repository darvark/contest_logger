#pragma once

#include "logger/domain/contest_definition.h"

#include <QString>

namespace logger {

class ContestDefinitionLoader final {
public:
    bool loadFile(const QString &filePath, ContestDefinition &definition,
                  QString *error = nullptr) const;
    bool loadText(const QString &yaml, const QString &sourceName,
                  ContestDefinition &definition, QString *error = nullptr) const;
};

} // namespace logger
