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
    bool hasLocation = false;
    double latitude = 0.0;   // degrees, north positive
    double longitude = 0.0;  // degrees, east positive
};

struct GeoBearing {
    double azimuthDegrees = 0.0;
    double distanceKm = 0.0;
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
    static const CtyPrefixEntry *findCtyEntry(
        const QString &callsign, const QVector<CtyPrefixEntry> &prefixes);
    static bool maidenheadToLatLon(const QString &locator, double &latitude,
                                   double &longitude);
    static GeoBearing bearingBetween(double fromLat, double fromLon,
                                     double toLat, double toLon);
    static QMap<QString, QString> resolveCtyMultipliers(
        const QString &callsign, const QVector<CtyPrefixEntry> &prefixes);
};

} // namespace logger
