// SPDX-License-Identifier: MPL-2.0
#include "SecretStore.h"

#include "util/Log.h"

#include <QEventLoop>

#ifdef E3MAIL_HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace e3 {

namespace {
#ifdef E3MAIL_HAVE_KEYCHAIN
const QString kService = QStringLiteral("e3mail");

bool disabled()
{
    return !qEnvironmentVariableIsEmpty("E3MAIL_NO_KEYCHAIN");
}

template<typename Job>
bool runJob(Job &job)
{
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    return job.error() == QKeychain::NoError;
}
#endif
} // namespace

bool SecretStore::available()
{
#ifdef E3MAIL_HAVE_KEYCHAIN
    if (disabled())
        return false;
    // Probe once: a write and delete of a throwaway entry.
    static const bool ok = [] {
        const QString probe = QStringLiteral("e3mail-probe");
        if (!write(probe, QStringLiteral("x")))
            return false;
        remove(probe);
        return true;
    }();
    return ok;
#else
    return false;
#endif
}

bool SecretStore::write(const QString &key, const QString &value)
{
#ifdef E3MAIL_HAVE_KEYCHAIN
    if (disabled())
        return false;
    QKeychain::WritePasswordJob job(kService);
    job.setKey(key);
    job.setTextData(value);
    if (!runJob(job)) {
        qCInfo(lcEngine) << "keyring unavailable:" << job.errorString();
        return false;
    }
    return true;
#else
    Q_UNUSED(key)
    Q_UNUSED(value)
    return false;
#endif
}

std::optional<QString> SecretStore::read(const QString &key)
{
#ifdef E3MAIL_HAVE_KEYCHAIN
    if (disabled())
        return std::nullopt;
    QKeychain::ReadPasswordJob job(kService);
    job.setKey(key);
    if (!runJob(job))
        return std::nullopt;
    return job.textData();
#else
    Q_UNUSED(key)
    return std::nullopt;
#endif
}

void SecretStore::remove(const QString &key)
{
#ifdef E3MAIL_HAVE_KEYCHAIN
    if (disabled())
        return;
    QKeychain::DeletePasswordJob job(kService);
    job.setKey(key);
    runJob(job);
#else
    Q_UNUSED(key)
#endif
}

} // namespace e3
