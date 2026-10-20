#include "logger/network/secret_store.h"

#include <QtTest>

class SecretStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsInvalidRequests();
};

void SecretStoreTest::rejectsInvalidRequests()
{
    QString secret = QStringLiteral("stale value");
    QString error;
    QVERIFY(!logger::SecretStore::read(QString(), &secret, &error));
    QVERIFY(secret.isEmpty());
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!logger::SecretStore::read(QStringLiteral("key"), nullptr, &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!logger::SecretStore::write(QStringLiteral("key"), QString(), &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!logger::SecretStore::remove(QString(), &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(SecretStoreTest)
#include "test_secret_store.moc"
