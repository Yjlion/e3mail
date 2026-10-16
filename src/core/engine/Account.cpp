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
#include "sync/MailboxFallback.h"
#include "sync/SyncSession.h"
#ifdef E3MAIL_HAVE_P2P
#include "sync/IrohTransport.h"
#endif
#include "util/Log.h"

#include <QDateTime>
#include <QDir>
#include <QSet>
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
    m_push.setSingleShot(true);
    m_push.setInterval(200);
    connect(&m_push, &QTimer::timeout, this, [this] {
        if (m_sync)
            m_sync->push();
    });
    // Paired devices that are away are dialed again: soon at first (one may
    // just be starting), then less often, up to once a minute.
    m_dial.setSingleShot(true);
    connect(&m_dial, &QTimer::timeout, this, [this] {
#ifdef E3MAIL_HAVE_P2P
        if (auto *t = qobject_cast<sync::IrohTransport *>(m_transport.get()))
            t->dialPaired();
#endif
        m_dial.start(qMin(m_dial.interval() * 2, 60 * 1000));
    });
    // Whatever changed here, the other devices hear of it.
    connect(this, &Account::mailChanged, this, &Account::pushSoon);
    m_fallback.setSingleShot(true);
    // Five minutes; E3MAIL_FALLBACK_SECONDS shortens it for live tests.
    const int fallbackSeconds = qEnvironmentVariableIntValue("E3MAIL_FALLBACK_SECONDS");
    m_fallback.setInterval((fallbackSeconds > 0 ? fallbackSeconds : 5 * 60) * 1000);
    connect(&m_fallback, &QTimer::timeout, this, &Account::mailFallbackNow);
}

Account::~Account()
{
    stop();
    stopDevices();
}

bool Account::devicesAvailable()
{
#ifdef E3MAIL_HAVE_P2P
    return true;
#else
    return false;
#endif
}

bool Account::startDevices()
{
#ifdef E3MAIL_HAVE_P2P
    if (m_transport)
        return true;
    QByteArray secret = QByteArray::fromHex(m_config->get(cfg::P2pSecret).toLatin1());
    if (secret.size() != 32) {
        secret = sync::IrohTransport::generateSecret();
        m_config->set(cfg::P2pSecret, QString::fromLatin1(secret.toHex()));
    }
    sync::IrohTransport::Options o;
    o.secret = secret;
    o.port = quint16(m_config->getInt(cfg::P2pPort));
    o.loopback = m_config->getBool(cfg::P2pLoopback);
    o.relayOnly = m_config->getBool(cfg::P2pRelayOnly);
    const QString relay = m_config->get(cfg::P2pRelay);
    if (relay == QLatin1String("off")) {
        o.relay = sync::IrohTransport::Relay::Off;
    } else if (relay.startsWith(QLatin1String("https://")) || relay.startsWith(QLatin1String("http://"))) {
        o.relay = sync::IrohTransport::Relay::Custom;
        o.relayUrl = relay;
    }
    auto t = std::make_unique<sync::IrohTransport>(*m_db, m_ops->deviceId(), o);
    if (!t->isRunning())
        return false;
    m_sync = std::make_unique<sync::SyncSession>(*m_ctx, *t);
    sync::IrohTransport *raw = t.get();
    connect(m_sync.get(), &sync::SyncSession::changed, this, [this, raw] {
        raw->refreshPeers(); // another device may have joined (device.pair)
        Q_EMIT mailChanged();
    });
    connect(raw, &sync::IrohTransport::refusedBy, this, &Account::refusedBy);
    connect(raw, &sync::DeviceTransport::connected, this, &Account::statusChanged);
    connect(raw, &sync::DeviceTransport::disconnected, this, &Account::statusChanged);
    m_transport = std::move(t);
    raw->dialPaired();
    m_dial.start(5000);
    // A device that went away is looked for soon again.
    connect(raw, &sync::DeviceTransport::disconnected, this, [this] { m_dial.start(5000); });
    qCInfo(lcEngine) << "devices: endpoint" << raw->nodeId();
    return true;
#else
    return false;
#endif
}

void Account::stopDevices()
{
    m_dial.stop();
    m_sync.reset();
    m_transport.reset();
}

bool Account::isDeviceConnected(const QString &device) const
{
#ifdef E3MAIL_HAVE_P2P
    if (auto *t = qobject_cast<sync::IrohTransport *>(m_transport.get()))
        return t->isConnected(device);
#endif
    Q_UNUSED(device)
    return false;
}

void Account::removeDevice(const QString &device)
{
    {
        Transaction tx(*m_db);
        m_db->run("DELETE FROM devices WHERE id=?", device);
        m_ops->record(op::DeviceUnpair, {{QStringLiteral("device"), device}});
        tx.commit();
    }
#ifdef E3MAIL_HAVE_P2P
    if (auto *t = qobject_cast<sync::IrohTransport *>(m_transport.get()))
        t->refreshPeers();
#endif
    notifyChanged();
    Q_EMIT statusChanged();
}

void Account::refusedBy(const QString &device)
{
    // Just after pairing, the other side may not have taken this device into
    // its list yet; only a pairing older than this counts as a removal.
    constexpr qint64 kSettled = 10 * 60 * 1000;
    const auto pairedAt = m_db->queryInt("SELECT paired_at FROM devices WHERE id=?", device);
    if (!pairedAt || QDateTime::currentMSecsSinceEpoch() - *pairedAt < kSettled)
        return;
    const QString name = m_db->queryText("SELECT name FROM devices WHERE id=?", device).value_or(QString());
    // Removed there while this device was away (ADR 0010 amendment,
    // 2026-10-16): stop dialing it, waiting for its acknowledgements and
    // mailing it changes. Not an op: the other devices still have it.
    m_db->run("DELETE FROM devices WHERE id=?", device);
    m_config->set(cfg::P2pRemovedBy, name);
    qCInfo(lcEngine) << "devices: removed from the account by" << device;
#ifdef E3MAIL_HAVE_P2P
    if (auto *t = qobject_cast<sync::IrohTransport *>(m_transport.get()))
        t->refreshPeers();
#endif
    notifyChanged();
    Q_EMIT statusChanged();
}

bool Account::mailFallbackNow()
{
    QSet<QString> connected;
#ifdef E3MAIL_HAVE_P2P
    if (auto *t = qobject_cast<sync::IrohTransport *>(m_transport.get())) {
        const QStringList c = t->connectedDevices();
        connected = QSet<QString>(c.cbegin(), c.cend());
    }
#endif
    try {
        if (!sync::MailboxFallback::queueIfDue(*m_ctx, connected))
            return false;
    } catch (const std::exception &e) {
        qCWarning(lcEngine) << "device sync by mail failed:" << e.what();
        return false;
    }
    if (m_worker)
        m_worker->wake(); // it sends it; otherwise the next pass will
    return true;
}

void Account::pushSoon()
{
    if (!m_fallback.isActive() && m_db->queryInt("SELECT 1 FROM devices LIMIT 1"))
        m_fallback.start();
    if (m_sync)
        m_push.start();
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
    m_config->setInt(cfg::AcceptInvalidCerts, s.acceptInvalidCertificates);
    m_config->set(cfg::InCertSha256, QString::fromLatin1(s.inCertSha256));
    m_config->set(cfg::SmtpCertSha256, QString::fromLatin1(s.smtpCertSha256));
    m_config->setInt(cfg::Configured, 1);
    tx.commit();
    storePasswords(s.inPassword, s.smtpPassword);
    mail::Keyring::ensureSelfKey(*m_ctx, s.addr.trimmed());
}

void Account::storePasswords(const QString &inPassword, const QString &smtp)
{
    const QString smtpPassword = smtp.isEmpty() ? inPassword : smtp;
    const bool inKeyring = SecretStore::write(secretKey(m_id, "in"), inPassword)
        && SecretStore::write(secretKey(m_id, "smtp"), smtpPassword);
    if (inKeyring) {
        m_db->run("DELETE FROM config WHERE key IN (?, ?)", cfg::InPassword, cfg::SmtpPassword);
    } else {
        qCWarning(lcEngine) << "no OS keyring; storing the password in the account database";
        m_config->set(cfg::InPassword, inPassword);
        m_config->set(cfg::SmtpPassword, smtpPassword);
    }
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
    s.acceptInvalidCertificates = m_config->getBool(cfg::AcceptInvalidCerts);
    s.inCertSha256 = m_config->get(cfg::InCertSha256).toLatin1();
    s.smtpCertSha256 = m_config->get(cfg::SmtpCertSha256).toLatin1();
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
    c.incoming.host = s.inHost;
    c.incoming.port = s.inPort;
    c.incoming.security = net::securityFromString(s.inSecurity);
    c.incoming.user = s.inUser.isEmpty() ? s.addr : s.inUser;
    c.incoming.password = s.inPassword;
    c.incoming.acceptInvalidCertificates = s.acceptInvalidCertificates;
    c.incoming.pinnedSha256 = s.inCertSha256;
    c.smtp.host = s.smtpHost;
    c.smtp.port = s.smtpPort;
    c.smtp.security = net::securityFromString(s.smtpSecurity);
    c.smtp.user = s.smtpUser.isEmpty() ? c.incoming.user : s.smtpUser;
    c.smtp.password = s.smtpPassword.isEmpty() ? s.inPassword : s.smtpPassword;
    c.smtp.acceptInvalidCertificates = s.acceptInvalidCertificates;
    c.smtp.pinnedSha256 = s.smtpCertSha256;
    return c;
}

MailWorker::Credentials Account::credentials() const
{
    return credentialsFrom(settings());
}

QFuture<Account::CheckResult> Account::check(const Settings &s)
{
    const MailWorker::Credentials c = credentialsFrom(s);
    return QtConcurrent::run([c]() -> CheckResult {
        CheckResult r;
        r.server = QStringLiteral("in");
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
            r.server = QStringLiteral("smtp");
            net::SmtpClient smtp;
            smtp.connect(c.smtp);
            smtp.quit();
            r.server.clear();
        } catch (const net::CertError &e) {
            r.error = QString::fromUtf8(e.what());
            r.cert = e.info;
        } catch (const std::exception &e) {
            r.error = QString::fromUtf8(e.what());
        }
        return r;
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
    connect(m_worker, &MailWorker::certificateRefused, this, [this](const QString &server, const QByteArray &sha) {
        m_refusedServer = server;
        m_refusedSha256 = sha;
    });
    connect(m_worker, &MailWorker::status, this, [this](const QString &state, const QString &detail) {
        if (state != QLatin1String("cert")) {
            m_refusedServer.clear();
            m_refusedSha256.clear();
        }
        m_status = state;
        m_statusDetail = detail;
        Q_EMIT statusChanged();
    });
    m_worker->start();
    if (m_db->queryInt("SELECT 1 FROM devices WHERE node_id IS NOT NULL LIMIT 1"))
        startDevices();
}

void Account::restart()
{
    stop();
    start();
}

void Account::trustRefusedCertificate()
{
    if (m_refusedSha256.isEmpty())
        return;
    m_config->set(m_refusedServer == QLatin1String("smtp") ? cfg::SmtpCertSha256 : cfg::InCertSha256,
                  QString::fromLatin1(m_refusedSha256));
    m_refusedServer.clear();
    m_refusedSha256.clear();
    restart();
}

void Account::forgetCertificate(const QString &server)
{
    m_config->set(server == QLatin1String("smtp") ? cfg::SmtpCertSha256 : cfg::InCertSha256, QString());
    restart();
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
