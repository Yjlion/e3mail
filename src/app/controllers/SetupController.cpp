// SPDX-License-Identifier: MPL-2.0
#include "SetupController.h"

#include "MailApp.h"
#include "engine/AccountManager.h"
#include "engine/Autoconfig.h"

#include <QFutureWatcher>
#include <QLocale>

using namespace e3;

SetupController::SetupController(QObject *parent) : QObject(parent), m_autoconfig(new Autoconfig(this))
{
    connect(m_autoconfig, &Autoconfig::finished, this, &SetupController::suggested);
}

void SetupController::suggested(const Autoconfig::Result &r)
{
    m_looking = false;
    m_incomingSuggestions.clear();
    for (const auto &s : r.incoming)
        m_incomingSuggestions.append({{QStringLiteral("protocol"), s.protocol},
                                      {QStringLiteral("host"), s.host},
                                      {QStringLiteral("port"), s.port},
                                      {QStringLiteral("security"), net::securityToString(s.security)},
                                      {QStringLiteral("username"), s.username}});
    useProtocol(m_protocol);
    m_smtpHost = r.smtp.host;
    m_smtpPort = r.smtp.port;
    m_smtpSecurity = net::securityToString(r.smtp.security);
    m_smtpUser = r.smtp.username;
    if (r.source == QLatin1String("provider"))
        m_note = tr("Server settings from your provider.");
    else if (r.source == QLatin1String("ispdb"))
        m_note = tr("Server settings from Thunderbird's provider database.");
    else if (r.source == QLatin1String("mx"))
        m_note = tr("Server settings of the service that receives your domain's mail. Check them before connecting.");
    else if (r.source == QLatin1String("autodiscover"))
        m_note = tr("Server settings from your provider's Autodiscover service.");
    else if (r.source == QLatin1String("srv"))
        m_note = tr("Server settings from DNS. Check them before connecting.");
    else
        m_note = tr("Server settings guessed from the address. Check them before connecting.");
    // DNS is not authenticated: hosts it names outside the address's domain
    // are shown before anything connects to them.
    const bool review = r.needsReview && !m_showServers;
    if (r.needsReview)
        m_showServers = true;
    setBusy(false);
    Q_EMIT changed();
    if (m_submitAfter) {
        m_submitAfter = false;
        if (!review)
            submit();
    }
}

void SetupController::setBusy(bool busy, const QString &error)
{
    m_busy = busy;
    m_error = error;
    if (busy || error.isEmpty())
        m_cert.clear();
    Q_EMIT busyChanged();
}

void SetupController::trustCertificate()
{
    if (m_cert.isEmpty())
        return;
    const QByteArray sha = m_cert.value(QStringLiteral("sha256")).toByteArray();
    const QString at = m_cert.value(QStringLiteral("host")).toString() + u':' + m_cert.value(QStringLiteral("port")).toString();
    if (m_cert.value(QStringLiteral("server")) == QLatin1String("smtp")) {
        m_smtpPin = sha;
        m_smtpPinFor = at;
    } else {
        m_inPin = sha;
        m_inPinFor = at;
    }
    submit();
}

void SetupController::suggest()
{
    const QString addr = m_addr.trimmed();
    if (!addr.contains(u'@') || addr == m_suggestedFor)
        return;
    m_suggestedFor = addr;
    m_looking = true;
    setBusy(true);
    m_autoconfig->lookup(addr);
}

void SetupController::useProtocol(const QString &protocol)
{
    m_protocol = protocol;
    for (const QVariantMap &s : std::as_const(m_incomingSuggestions)) {
        if (s.value(QStringLiteral("protocol")).toString() == protocol) {
            m_inHost = s.value(QStringLiteral("host")).toString();
            m_inPort = s.value(QStringLiteral("port")).toInt();
            m_inSecurity = s.value(QStringLiteral("security")).toString();
            m_inUser = s.value(QStringLiteral("username")).toString();
        }
    }
    if (m_incomingSuggestions.isEmpty() || m_inHost.isEmpty()) {
        const QString domain = m_addr.section(u'@', 1).trimmed().toLower();
        m_inHost = (protocol == QLatin1String("pop3") ? QStringLiteral("pop.") : QStringLiteral("imap.")) + domain;
        m_inPort = protocol == QLatin1String("pop3") ? 995 : 993;
        m_inSecurity = QStringLiteral("ssl");
        m_inUser.clear();
    }
    Q_EMIT changed();
}

void SetupController::submit()
{
    if (!m_addr.contains(u'@') || m_password.isEmpty()) {
        setBusy(false, tr("Enter your email address and password."));
        return;
    }
    // The fields are about to be filled in: check what they will hold.
    if (m_looking) {
        m_submitAfter = true;
        return;
    }
    Account::Settings s;
    s.addr = m_addr.trimmed();
    s.displayName = m_name.trimmed();
    s.protocol = m_protocol;
    s.inHost = m_inHost.trimmed();
    s.inPort = quint16(m_inPort);
    s.inSecurity = m_inSecurity;
    s.inUser = m_inUser.isEmpty() ? s.addr : m_inUser;
    s.smtpUser = m_smtpUser;
    s.inPassword = m_password;
    s.smtpHost = m_smtpHost.trimmed();
    s.smtpPort = quint16(m_smtpPort);
    s.smtpSecurity = m_smtpSecurity;
    // A pin holds only for the server it was given for.
    const QString inAt = s.inHost + u':' + QString::number(s.inPort);
    const QString smtpAt = s.smtpHost + u':' + QString::number(s.smtpPort);
    if (m_inPinFor == inAt)
        s.inCertSha256 = m_inPin;
    if (m_smtpPinFor == smtpAt)
        s.smtpCertSha256 = m_smtpPin;
    setBusy(true);
    auto *watcher = new QFutureWatcher<Account::CheckResult>(this);
    connect(watcher, &QFutureWatcher<Account::CheckResult>::finished, this, [this, watcher, s, inAt, smtpAt] {
        watcher->deleteLater();
        const Account::CheckResult r = watcher->result();
        if (!r.ok()) {
            if (r.cert && r.server == QLatin1String("smtp") && !r.cert->pinChanged && r.cert->sha256 == m_inPin
                && m_inPinFor.section(u':', 0, 0) == s.smtpHost) {
                // The certificate just trusted for this host serves SMTP too.
                m_smtpPin = m_inPin;
                m_smtpPinFor = smtpAt;
                submit();
                return;
            }
            m_showServers = true;
            Q_EMIT changed();
            setBusy(false, r.error);
            if (r.cert) {
                const net::CertInfo &c = *r.cert;
                QStringList problems = c.problems;
                problems.removeDuplicates();
                m_cert = {{QStringLiteral("server"), r.server},
                          {QStringLiteral("host"), c.host},
                          {QStringLiteral("port"), c.port},
                          {QStringLiteral("sha256"), c.sha256},
                          {QStringLiteral("fingerprint"), net::formatFingerprint(c.sha256)},
                          {QStringLiteral("subject"), c.subject},
                          {QStringLiteral("issuer"), c.issuer},
                          {QStringLiteral("expires"), QLocale().toString(c.expires.date(), QLocale::ShortFormat)},
                          {QStringLiteral("problems"), problems.join(u'\n')},
                          {QStringLiteral("changed"), c.pinChanged}};
                Q_EMIT busyChanged();
            }
            return;
        }
        MailApp *app = MailApp::create(nullptr, nullptr);
        Account *a = app->manager()->create();
        a->configure(s);
        a->start();
        app->accountConfigured();
        setBusy(false);
        Q_EMIT done(a->id());
    });
    watcher->setFuture(Account::check(s));
}
