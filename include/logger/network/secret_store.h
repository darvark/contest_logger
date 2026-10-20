#pragma once

#include <QString>

namespace logger {

class SecretStore final {
public:
    static bool read(const QString &key, QString *secret,
                     QString *error = nullptr);
    static bool write(const QString &key, const QString &secret,
                      QString *error = nullptr);
    static bool remove(const QString &key, QString *error = nullptr);
};

} // namespace logger
