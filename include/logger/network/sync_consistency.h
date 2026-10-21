#pragma once

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

namespace logger {

inline QJsonObject syncConsistencyDigest(const QJsonArray &snapshots)
{
    QMap<QString, QJsonArray> groups;
    for (const QJsonValue &value : snapshots) {
        const QJsonObject snapshot = value.toObject();
        const QString qsoUuid = snapshot.value(QStringLiteral("qso_uuid")).toString();
        const QString group = qsoUuid.left(2);
        groups[group].append(snapshot);
    }

    QJsonObject digests;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const QByteArray json =
            QJsonDocument(it.value()).toJson(QJsonDocument::Compact);
        digests.insert(it.key(), QJsonObject{
            {QStringLiteral("count"), it.value().size()},
            {QStringLiteral("sha256"),
             QString::fromLatin1(QCryptographicHash::hash(
                 json, QCryptographicHash::Sha256).toHex())}
        });
    }
    return QJsonObject{
        {QStringLiteral("digest_version"), 1},
        {QStringLiteral("count"), snapshots.size()},
        {QStringLiteral("groups"), digests}
    };
}

} // namespace logger
