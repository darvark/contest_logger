#include "logger/network/secret_store.h"

#include <QProcess>
#include <QStringList>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace logger {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

#ifdef Q_OS_LINUX
bool runSecretTool(const QStringList &arguments, const QByteArray &input,
                   QByteArray *output, QString *error)
{
    QProcess process;
    process.start(QStringLiteral("secret-tool"), arguments);
    if (!process.waitForStarted()) {
        setError(error, QStringLiteral(
            "System secret storage is unavailable (secret-tool could not start)."));
        return false;
    }
    if (!input.isEmpty()) {
        process.write(input);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished();
        setError(error, QStringLiteral("System secret storage request timed out."));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        const QString detail =
            QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (detail.contains(QStringLiteral("No such secret"),
                            Qt::CaseInsensitive)
            || detail.contains(QStringLiteral("not found"),
                               Qt::CaseInsensitive)) {
            if (output != nullptr) {
                output->clear();
            }
            return true;
        }
        setError(error, detail);
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("System secret storage request failed.");
        }
        return false;
    }
    if (output != nullptr) {
        *output = process.readAllStandardOutput();
    }
    return true;
}

QStringList secretAttributes(const QString &key)
{
    return {QStringLiteral("service"), QStringLiteral("contest-logger"),
            QStringLiteral("key"), key};
}
#endif

} // namespace

bool SecretStore::read(const QString &key, QString *secret, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (secret != nullptr) {
        secret->clear();
    }
    if (key.trimmed().isEmpty() || secret == nullptr) {
        setError(error, QStringLiteral("Secret key and output are required."));
        return false;
    }
#ifdef Q_OS_WIN
    const std::wstring target =
        (L"ContestLogger/" + key.toStdWString());
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() == ERROR_NOT_FOUND) {
            return true;
        }
        setError(error, QStringLiteral("Could not read the Windows credential store."));
        return false;
    }
    const QByteArray value(
        reinterpret_cast<const char *>(credential->CredentialBlob),
        static_cast<qsizetype>(credential->CredentialBlobSize));
    CredFree(credential);
    *secret = QString::fromUtf8(value);
    return true;
#elif defined(Q_OS_LINUX)
    QByteArray output;
    if (!runSecretTool(secretAttributes(key), {}, &output, error)) {
        return false;
    }
    *secret = QString::fromUtf8(output).trimmed();
    return true;
#else
    Q_UNUSED(key);
    setError(error, QStringLiteral(
        "No system secret-store backend is available on this platform."));
    return false;
#endif
}

bool SecretStore::write(const QString &key, const QString &secret, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (key.trimmed().isEmpty() || secret.isEmpty()) {
        setError(error, QStringLiteral("Secret key and value are required."));
        return false;
    }
#ifdef Q_OS_WIN
    const std::wstring target =
        (L"ContestLogger/" + key.toStdWString());
    const QByteArray value = secret.toUtf8();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char *>(value.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"Contest Logger");
    if (!CredWriteW(&credential, 0)) {
        setError(error, QStringLiteral("Could not write the Windows credential store."));
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    QStringList arguments{QStringLiteral("store"),
                          QStringLiteral("--label=Contest Logger synchronization token")};
    arguments.append(secretAttributes(key));
    return runSecretTool(arguments, secret.toUtf8(), nullptr, error);
#else
    Q_UNUSED(key);
    Q_UNUSED(secret);
    setError(error, QStringLiteral(
        "No system secret-store backend is available on this platform."));
    return false;
#endif
}

bool SecretStore::remove(const QString &key, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (key.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Secret key is required."));
        return false;
    }
#ifdef Q_OS_WIN
    const std::wstring target =
        (L"ContestLogger/" + key.toStdWString());
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)
        && GetLastError() != ERROR_NOT_FOUND) {
        setError(error, QStringLiteral("Could not delete the Windows credential."));
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    QStringList arguments{QStringLiteral("clear")};
    arguments.append(secretAttributes(key));
    QString localError;
    if (runSecretTool(arguments, {}, nullptr, &localError)) {
        return true;
    }
    if (localError.contains(QStringLiteral("No such secret"))
        || localError.contains(QStringLiteral("not found"), Qt::CaseInsensitive)) {
        return true;
    }
    setError(error, localError);
    return false;
#else
    Q_UNUSED(key);
    setError(error, QStringLiteral(
        "No system secret-store backend is available on this platform."));
    return false;
#endif
}

} // namespace logger
