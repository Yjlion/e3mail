// SPDX-License-Identifier: MPL-2.0
#include "SetupController.h"

#include "MailApp.h"
#include "engine/AccountManager.h"
#include "engine/Autoconfig.h"

#include <QFutureWatcher>

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
    Q_EMIT busyChanged();
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
    setBusy(true);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, s] {
        watcher->deleteLater();
        const QString error = watcher->result();
        if (!error.isEmpty()) {
            m_showServers = true;
            Q_EMIT changed();
            setBusy(false, error);
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
