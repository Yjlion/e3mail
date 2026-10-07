// SPDX-License-Identifier: MPL-2.0
#include "Account.h"

#include "SecretStore.h"
#include "crypto/Pgp.h"
#include "mail/Keyring.h"
#include "net/Imap.h"
#include "net/Pop3.h"
#include "net/Smtp.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "store/Schema.h"
#include "util/Log.h"

#include <QDir>
#include <QtConcurrent>

namespace e3 {

namespace {
QString secretKey(int id, const char *which)
{
    return QStringLiteral("account-%1-%2").arg(id).arg(QLatin1String(which));
}
} // namespace

Account::Account(int id, const QString &dir, QObject *parent) : QObject(parent), m_id(id), m_dir(dir)
{
    QDir().mkpath(dir);
    m_db = std::make_unique<Database>(dir + QStringLiteral("/mail.db"));
    Schema::migrate(*m_db);
    m_blobs = std::make_unique<BlobStore>(dir + QStringLiteral("/blobs"));
    m_pgp = std::make_unique<crypto::Pgp>();
    m_config = std::make_unique<Config>(*m_db);
    m_ops = std::make_unique<OpLog>(*m_db);
    m_ctx = std::make_unique<MailContext>(MailContext{*m_db, *m_blobs, *m_pgp, *m_config, *m_ops});
    mail::Keyring::loadAll(*m_ctx);
}

Account::~Account()
{
    stop();
}

QString Account::addr() const
{
    return m_config->get(cfg::Addr);
}

QString Account::displayName() const
{
    return m_config->get(cfg::DisplayName);
}

bool Account::isConfigured() const
{
    return m_config->getBool(cfg::Configured);
}

QString Account::secretLocation() const
{
    return m_config->isSet(cfg::InPassword) ? QStringLiteral("database") : QStringLiteral("keyring");
}

void Account::configure(const Settings &s)
{
    Transaction tx(*m_db);
    m_config->set(cfg::Addr, s.addr.trimmed());
    m_config->set(cfg::DisplayName, s.displayName.trimmed());
    m_config->set(cfg::Protocol, s.protocol);
    m_config->set(cfg::InHost, s.inHost.trimmed());
    m_config->setInt(cfg::InPort, s.inPort);
    m_config->set(cfg::InSecurity, s.inSecurity);
    m_config->set(cfg::InUser, s.inUser.isEmpty() ? s.addr.trimmed() : s.inUser.trimmed());
    m_config->set(cfg::SmtpHost, s.smtpHost.trimmed());
    m_config->setInt(cfg::SmtpPort, s.smtpPort);
    m_config->set(cfg::SmtpSecurity, s.smtpSecurity);
    m_config->set(cfg::SmtpUser, s.smtpUser.isEmpty() ? m_config->get(cfg::InUser) : s.smtpUser.trimmed());
    m_config->setInt("accept_invalid_certs", s.acceptInvalidCertificates);
    m_config->setInt(cfg::Configured, 1);
    tx.commit();

    const QString smtpPassword = s.smtpPassword.isEmpty() ? s.inPassword : s.smtpPassword;
    const bool inKeyring = SecretStore::write(secretKey(m_id, "in"), s.inPassword)
        && SecretStore::write(secretKey(m_id, "smtp"), smtpPassword);
    if (inKeyring) {
        m_db->run("DELETE FROM config WHERE key IN (?, ?)", cfg::InPassword, cfg::SmtpPassword);
    } else {
        qCWarning(lcEngine) << "no OS keyring; storing the password in the account database";
        m_config->set(cfg::InPassword, s.inPassword);
        m_config->set(cfg::SmtpPassword, smtpPassword);
    }
    mail::Keyring::ensureSelfKey(*m_ctx, s.addr.trimmed());
}

Account::Settings Account::settings() const
{
    Settings s;
    s.addr = m_config->get(cfg::Addr);
    s.displayName = m_config->get(cfg::DisplayName);
    s.protocol = m_config->get(cfg::Protocol);
    s.inHost = m_config->get(cfg::InHost);
    s.inPort = quint16(m_config->getInt(cfg::InPort));
    s.inSecurity = m_config->get(cfg::InSecurity);
    s.inUser = m_config->get(cfg::InUser);
    s.smtpHost = m_config->get(cfg::SmtpHost);
    s.smtpPort = quint16(m_config->getInt(cfg::SmtpPort));
    s.smtpSecurity = m_config->get(cfg::SmtpSecurity);
    s.smtpUser = m_config->get(cfg::SmtpUser);
    s.acceptInvalidCertificates = m_config->getBool("accept_invalid_certs");
    if (m_config->isSet(cfg::InPassword)) {
        s.inPassword = m_config->get(cfg::InPassword);
        s.smtpPassword = m_config->get(cfg::SmtpPassword);
    } else {
        s.inPassword = SecretStore::read(secretKey(m_id, "in")).value_or(QString());
        s.smtpPassword = SecretStore::read(secretKey(m_id, "smtp")).value_or(s.inPassword);
    }
    return s;
}

static MailWorker::Credentials credentialsFrom(const Account::Settings &s)
{
    MailWorker::Credentials c;
    c.protocol = s.protocol;
    c.incoming = {s.inHost, s.inPort, net::securityFromString(s.inSecurity), s.inUser.isEmpty() ? s.addr : s.inUser,
                  s.inPassword, s.acceptInvalidCertificates};
    c.smtp = {s.smtpHost, s.smtpPort, net::securityFromString(s.smtpSecurity),
              s.smtpUser.isEmpty() ? c.incoming.user : s.smtpUser,
              s.smtpPassword.isEmpty() ? s.inPassword : s.smtpPassword, s.acceptInvalidCertificates};
    return c;
}

MailWorker::Credentials Account::credentials() const
{
    return credentialsFrom(settings());
}

QFuture<QString> Account::check(const Settings &s)
{
    const MailWorker::Credentials c = credentialsFrom(s);
    return QtConcurrent::run([c]() -> QString {
        try {
            if (c.protocol == QLatin1String("pop3")) {
                net::Pop3Client pop;
                pop.connect(c.incoming);
                pop.quit();
            } else {
                net::ImapClient imap;
                imap.connect(c.incoming);
                imap.select();
                imap.logout();
            }
            net::SmtpClient smtp;
            smtp.connect(c.smtp);
            smtp.quit();
            return QString();
        } catch (const std::exception &e) {
            return QString::fromUtf8(e.what());
        }
    });
}

void Account::start()
{
    if (m_worker || !isConfigured())
        return;
    m_worker = new MailWorker(m_dir, credentials(), this);
    connect(m_worker, &MailWorker::mailChanged, this, &Account::mailChanged);
    connect(m_worker, &MailWorker::newMail, this, &Account::newMail);
    connect(m_worker, &MailWorker::sent, this, &Account::mailChanged);
    connect(m_worker, &MailWorker::backfillProgress, this, [this](int remaining) {
        if (remaining == m_olderRemaining)
            return;
        m_olderRemaining = remaining;
        Q_EMIT statusChanged();
    });
    connect(m_worker, &MailWorker::status, this, [this](const QString &state, const QString &detail) {
        m_status = state;
        m_statusDetail = detail;
        Q_EMIT statusChanged();
    });
    m_worker->start();
}

void Account::stop()
{
    if (!m_worker)
        return;
    m_worker->requestStop();
    m_worker->wait();
    delete m_worker;
    m_status = QStringLiteral("offline");
    Q_EMIT statusChanged();
}

void Account::syncNow()
{
    if (m_worker)
        m_worker->wake();
    else
        start();
}

bool Account::isRunning() const
{
    return m_worker && m_worker->isRunning();
}

} // namespace e3
