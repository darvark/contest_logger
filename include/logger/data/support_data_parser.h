#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace logger {

struct CallHistoryEntry {
    QString callsign;
    QMap<QString, QString> fields;
};

struct CtyPrefixEntry {
    QString prefix;
    QString entity;
    QString continent;
    int cqZone = 0;
    int ituZone = 0;
    bool exactCallsign = false;
};

class SupportDataParser final {
public:
    static bool parseCallHistory(const QString &contents,
                                 QHash<QString, CallHistoryEntry> &entries,
                                 QString *error = nullptr);
    static bool parseMasterCpy(const QString &contents, QStringList &callsigns,
                               QString *error = nullptr);
    static bool parseCtyDat(const QString &contents,
                            QVector<CtyPrefixEntry> &prefixes,
                            QString *error = nullptr);
    static QMap<QString, QString> resolveCtyMultipliers(
        const QString &callsign, const QVector<CtyPrefixEntry> &prefixes);
};

} // namespace logger
