// SPDX-License-Identifier: MPL-2.0
#include "BackupController.h"

#include "MailApp.h"
#include "SignIn.h"
#include "backup/Crypto.h"
#include "engine/AccountManager.h"
#include "engine/Backups.h"
#include "net/SyncSocket.h"
#include "store/Config.h"
#ifdef E3MAIL_HAVE_SFTP
#include "backup/Sftp.h"
#endif

#include <QDesktopServices>
#include <QFutureWatcher>
#include <QLocale>
#include <QSysInfo>
#include <QtConcurrentRun>

using namespace e3;

namespace {

// What a thread brings back: a master key or a staged restore, or what
// stopped it.
struct Done
{
    QByteArray master;
    backups::Staged staged;
    QString error;
    QVariantMap confirm;
};

template<typename F>
Done guarded(F &&f)
{
    Done d;
    try {
        f(d);
    } catch (const backup::BadPassphrase &) {
        d.error = QCoreApplication::translate("BackupController", "The passphrase does not open the backups there.");
#ifdef E3MAIL_HAVE_SFTP
    } catch (const backup::HostKeyError &e) {
        d.confirm = {{QStringLiteral("kind"), QStringLiteral("hostKey")},
                     {QStringLiteral("fingerprint"), QString::fromLatin1(e.sha256)},
                     {QStringLiteral("changed"), e.changed}};
#endif
    } catch (const net::CertError &e) {
        d.confirm = {{QStringLiteral("kind"), QStringLiteral("certificate")},
                     {QStringLiteral("sha256"), QString::fromLatin1(e.info.sha256)},
                     {QStringLiteral("fingerprint"), net::formatFingerprint(e.info.sha256)},
                     {QStringLiteral("changed"), e.info.pinChanged}};
    } catch (const std::exception &e) {
        d.error = QString::fromUtf8(e.what());
    }
    return d;
}

} // namespace

BackupController::BackupController(QObject *parent) : QObject(parent)
{
    // The account switching, or a scheduled backup finishing.
    connect(MailApp::create(nullptr, nullptr), &MailApp::backupsChanged, this, &BackupController::changed);
}

Account *BackupController::account() const
{
    return MailApp::create(nullptr, nullptr)->account();
}

bool BackupController::busy() const
{
    return m_busy || MailApp::create(nullptr, nullptr)->backingUp(account());
}

bool BackupController::isSetUp() const
{
    return account() && backups::isSetUp(*account());
}

QStringList BackupController::types() const
{
    QStringList t = {QStringLiteral("folder"), QStringLiteral("webdav"), QStringLiteral("s3")};
#ifdef E3MAIL_HAVE_SFTP
    t.append(QStringLiteral("sftp"));
#endif
    if (net::oauth::forBackups(QStringLiteral("google")))
        t.append(QStringLiteral("gdrive"));
    if (net::oauth::forBackups(QStringLiteral("microsoft")))
        t.append(QStringLiteral("onedrive"));
    return t;
}

QString BackupController::where() const
{
    if (!isSetUp())
        return {};
    const QJsonObject c = backups::config(*account());
    const QString type = c.value(QLatin1String("type")).toString();
    if (type == QLatin1String("folder"))
        return c.value(QLatin1String("path")).toString();
    if (type == QLatin1String("webdav"))
        return QUrl(c.value(QLatin1String("url")).toString()).toDisplayString(QUrl::RemoveUserInfo);
    if (type == QLatin1String("s3"))
        return QStringLiteral("s3://%1/%2").arg(c.value(QLatin1String("bucket")).toString(), c.value(QLatin1String("prefix")).toString());
    if (type == QLatin1String("sftp"))
        return QStringLiteral("sftp://%1@%2%3").arg(c.value(QLatin1String("user")).toString(), c.value(QLatin1String("host")).toString(),
                                                     c.value(QLatin1String("path")).toString());
    if (type == QLatin1String("gdrive"))
        return tr("Google Drive, folder “%1”").arg(c.value(QLatin1String("folder")).toString(QStringLiteral("e3mail backup")));
    if (type == QLatin1String("onedrive"))
        return tr("OneDrive, Apps/e3mail");
    return type;
}

QString BackupController::last() const
{
    if (!account())
        return {};
    const QDateTime t = QDateTime::fromString(account()->ctx().config.get(cfg::BackupLast), Qt::ISODate);
    return t.isValid() ? QLocale().toString(t.toLocalTime(), QLocale::ShortFormat) : QString();
}

QString BackupController::lastError() const
{
    return account() ? account()->ctx().config.get(cfg::BackupLastError) : QString();
}

void BackupController::setBusy(bool busy, const QString &error)
{
    m_busy = busy;
    m_error = error;
    if (busy)
        m_confirm.clear();
    Q_EMIT changed();
}

QString BackupController::recoveryCode() const
{
    // Crockford's base 32: no I, L, O or U to misread.
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    const QByteArray r = backup::crypto::random(25);
    QString code;
    for (int i = 0; i < 25; ++i) {
        if (i && i % 5 == 0)
            code += u'-';
        code += QLatin1Char(alphabet[uchar(r[i]) % 32]);
    }
    return code;
}

void BackupController::signInToCloud(const QString &type)
{
    const auto provider = net::oauth::forBackups(type == QLatin1String("gdrive") ? QStringLiteral("google")
                                                                                 : QStringLiteral("microsoft"));
    if (!provider)
        return;
    auto *s = new SignIn(*provider, account() ? account()->addr() : QString(), this);
    connect(s, &SignIn::openBrowser, this, [](const QUrl &url) { QDesktopServices::openUrl(url); });
    connect(s, &SignIn::finished, this, [this, s](const QString &error) {
        s->deleteLater();
        if (!error.isEmpty()) {
            setBusy(false, error);
            return;
        }
        m_cloudSecret = s->refreshToken();
        m_cloudClient = {{QStringLiteral("clientId"), s->provider().clientId},
                         {QStringLiteral("clientSecret"), s->provider().clientSecret},
                         {QStringLiteral("tokenUrl"), s->provider().tokenUrl.toString()}};
        setBusy(false);
    });
    setBusy(true);
    s->start();
}

QJsonObject BackupController::withSignIn(const QVariantMap &destination) const
{
    QJsonObject c = QJsonObject::fromVariantMap(destination);
    const QString type = c.value(QLatin1String("type")).toString();
    if (type == QLatin1String("gdrive") || type == QLatin1String("onedrive")) {
        for (auto it = m_cloudClient.constBegin(); it != m_cloudClient.constEnd(); ++it)
            c.insert(it.key(), it.value());
    }
    if (type == QLatin1String("sftp"))
        c.insert(QStringLiteral("port"), c.value(QLatin1String("port")).toVariant().toInt());
    return c;
}

void BackupController::setUp(const QVariantMap &destination, const QString &secret, const QString &passphrase)
{
    if (!account())
        return;
    if (passphrase.size() < 12) {
        setBusy(false, tr("Use a passphrase of at least 12 characters, or a recovery code."));
        return;
    }
    m_lastDestination = destination;
    m_lastSecret = secret;
    m_lastPassphrase = passphrase;
    m_lastWasRestore = false;
    const QJsonObject c = withSignIn(destination);
    const QString s = c.value(QLatin1String("type")).toString().startsWith(QLatin1String("gdrive"))
                          || c.value(QLatin1String("type")).toString() == QLatin1String("onedrive")
                      ? m_cloudSecret
                      : secret;
    setBusy(true);
    auto *w = new QFutureWatcher<Done>(this);
    connect(w, &QFutureWatcher<Done>::finished, this, [this, w, c, s] {
        w->deleteLater();
        const Done d = w->result();
        if (!d.confirm.isEmpty()) {
            m_confirm = d.confirm;
            setBusy(false);
            return;
        }
        if (!d.error.isEmpty() || !account()) {
            setBusy(false, d.error);
            return;
        }
        backups::store(*account(), c, s, d.master);
        setBusy(false);
        backUpNow();
    });
    const QByteArray pass = passphrase.toUtf8();
    w->setFuture(QtConcurrent::run([c, s, pass] {
        return guarded([&](Done &d) { d.master = backups::openOrCreate(c, s, pass); });
    }));
}

void BackupController::trust()
{
    if (m_confirm.isEmpty())
        return;
    if (m_confirm.value(QStringLiteral("kind")) == QLatin1String("hostKey"))
        m_lastDestination.insert(QStringLiteral("hostKey"), m_confirm.value(QStringLiteral("fingerprint")));
    else
        m_lastDestination.insert(QStringLiteral("pin"), m_confirm.value(QStringLiteral("sha256")));
    m_confirm.clear();
    if (m_lastWasRestore)
        restore(m_lastDestination, m_lastSecret, m_lastPassphrase);
    else
        setUp(m_lastDestination, m_lastSecret, m_lastPassphrase);
}

void BackupController::backUpNow()
{
    if (!isSetUp() || m_busy)
        return;
    MailApp::create(nullptr, nullptr)->backUp(account());
}

void BackupController::stop()
{
    if (!account())
        return;
    backups::forget(*account());
    m_cloudSecret.clear();
    Q_EMIT changed();
}

void BackupController::restore(const QVariantMap &destination, const QString &secret, const QString &passphrase)
{
    m_lastDestination = destination;
    m_lastSecret = secret;
    m_lastPassphrase = passphrase;
    m_lastWasRestore = true;
    const QJsonObject c = withSignIn(destination);
    const QString type = c.value(QLatin1String("type")).toString();
    const QString s = type == QLatin1String("gdrive") || type == QLatin1String("onedrive") ? m_cloudSecret : secret;
    AccountManager *mgr = MailApp::create(nullptr, nullptr)->manager();
    const QString root = mgr->root();
    const QString device = MailApp::create(nullptr, nullptr)->defaultDeviceName();
    m_percent = 0;
    setBusy(true);
    auto *w = new QFutureWatcher<Done>(this);
    connect(w, &QFutureWatcher<Done>::finished, this, [this, w, mgr] {
        w->deleteLater();
        const Done d = w->result();
        if (!d.confirm.isEmpty()) {
            m_confirm = d.confirm;
            setBusy(false);
            return;
        }
        if (!d.error.isEmpty()) {
            setBusy(false, d.error);
            return;
        }
        try {
            Account *a = backups::adopt(*mgr, d.staged);
            a->start();
            MailApp::create(nullptr, nullptr)->accountConfigured();
            setBusy(false);
            Q_EMIT restored(a->id());
        } catch (const std::exception &e) {
            setBusy(false, QString::fromUtf8(e.what()));
        }
    });
    const QByteArray pass = passphrase.toUtf8();
    QPointer<BackupController> self(this);
    w->setFuture(QtConcurrent::run([c, s, pass, root, device, self] {
        return guarded([&](Done &d) {
            auto dest = backups::destination(c, s);
            if (!dest)
                throw net::NetError(QStringLiteral("unknown backup destination"));
            d.staged = backups::stage(root, *dest, pass, QString(), device, [self](int p) {
                QMetaObject::invokeMethod(self.data(), [self, p] {
                    if (self) {
                        self->m_percent = p;
                        Q_EMIT self->changed();
                    }
                });
            });
        });
    }));
}
