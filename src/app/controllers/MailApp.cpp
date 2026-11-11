// SPDX-License-Identifier: MPL-2.0
#include "MailApp.h"
#include "AndroidSync.h"
#include "SignIn.h"
#include "engine/Backups.h"
#include "net/OAuth.h"
#include "FileIo.h"
#include "Languages.h"
#include "InlineImages.h"
#include "NotifierBackends.h"
#include "RichText.h"

#include "crypto/Pgp.h"
#include "engine/AccountManager.h"
#include "engine/AtRest.h"
#include "mail/Calendar.h"
#include "mail/CalendarLayout.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#ifdef E3MAIL_HAVE_P2P
#include "engine/Pairing.h"
#endif
#ifdef E3MAIL_HAVE_CAMERA
#include "QrScanner.h"
#endif
#include "mail/ContactsCsv.h"
#include "mail/Export.h"
#include "mail/Organize.h"
#include "mail/Policy.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "mail/SecureJoin.h"
#include "mail/Signatures.h"
#include "mail/Structured.h"
#include "mail/VCard.h"
#include "mime/Html.h"
#include "mime/Part.h"
#include "mime/Source.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "util/LocalePatterns.h"
#include "util/Paths.h"

#include <QDesktopServices>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QSettings>
#include <QJSEngine>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QTimeZone>
#include <QTextDocument>

using namespace e3;

MailApp *MailApp::s_instance = nullptr;

namespace {
// This device's own settings, beside the language (Languages.cpp).
QSettings appSettings()
{
    return QSettings(Paths::dataDir() + QStringLiteral("/app.ini"), QSettings::IniFormat);
}
} // namespace

namespace {

QString reasonText(TrashReason r)
{
    switch (r) {
    case TrashReason::Expired: return QObject::tr("Its disappearing-message timer fired.");
    case TrashReason::Unaccepted: return QObject::tr("The sender was never accepted.");
    case TrashReason::Blocked: return QObject::tr("The sender is blocked.");
    case TrashReason::Cleartext: return QObject::tr("It arrived unencrypted while encryption is required.");
    case TrashReason::User: break;
    }
    return QObject::tr("You deleted it.");
}

QString addrList(const QList<mime::Address> &list)
{
    QStringList out;
    for (const auto &a : list)
        out.append(a.display());
    return out.join(QStringLiteral(", "));
}

int daysUntil(const QDateTime &dt)
{
    return int(qMax<qint64>(0, (QDateTime::currentDateTimeUtc().secsTo(dt) + 86399) / 86400));
}

} // namespace

MailApp::MailApp(AccountManager *manager, QObject *parent) : QObject(parent), m_mgr(manager), m_tag(tag::Inbox)
{
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(120);
    connect(&m_refresh, &QTimer::timeout, this, &MailApp::refresh);
    connect(&m_refresh, &QTimer::timeout, this, &MailApp::armExpiry);
    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (Account *a : m_mgr->accounts()) {
            if (mail::Organize::expireDue(a->ctx(), now))
                a->notifyChanged();
        }
        armExpiry();
    });
    QTimer::singleShot(0, this, &MailApp::armExpiry);
    m_reminder.setSingleShot(true);
    connect(&m_reminder, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (Account *a : m_mgr->accounts()) {
            for (const auto &o : mail::Calendar::dueReminders(a->ctx(), now)) {
                const QString when = o.event.allDay ? tr("Today") : QLocale().toString(o.start.toLocalTime().time(), QLocale::ShortFormat);
                m_notifier->remind(a->id(), o.event.uid, o.event.summary.isEmpty() ? tr("Event") : o.event.summary,
                                   o.event.location.isEmpty() ? when : tr("%1 · %2").arg(when, o.event.location));
            }
        }
        armReminder();
    });
    QTimer::singleShot(0, this, &MailApp::armReminder);
    connect(m_mgr, &AccountManager::accountsChanged, this, &MailApp::accountsChanged);
    connect(m_mgr, &AccountManager::selectedChanged, this, &MailApp::switchAccount);
    for (Account *a : m_mgr->accounts()) {
        connect(a, &Account::statusChanged, this, &MailApp::accountsChanged);
    }

    m_notifier = new Notifier(makeSystemNotifierBackend(), this);
    m_notifier->setMode(Notifier::modeFromString(appSettings().value(QStringLiteral("notifications")).toString()));
    m_notifier->setWindowActive([] { return QGuiApplication::applicationState() == Qt::ApplicationActive; });
    // The process keeps running in the background only while the service
    // does; it is started here, at launch, while e3mail is in front.
    androidsync::onStopPressed([this] { setBackgroundSync(false); });
    // Daily backups: looked at a few minutes after starting, then hourly.
    auto *backupTimer = new QTimer(this);
    connect(backupTimer, &QTimer::timeout, this, [this, backupTimer] {
        backupTimer->setInterval(60 * 60 * 1000);
        for (Account *a : m_mgr->accounts()) {
            if (!backups::isSetUp(*a))
                continue;
            const QDateTime last = QDateTime::fromString(a->ctx().config.get(cfg::BackupLast), Qt::ISODate);
            if (!last.isValid() || last.secsTo(QDateTime::currentDateTimeUtc()) > 23 * 3600)
                backUp(a);
        }
    });
    backupTimer->start(3 * 60 * 1000);
    if (backgroundSync())
        androidsync::start();
    connect(m_notifier, &Notifier::reminderActivated, this, [this](int accountId, const QString &uid) {
        if (m_mgr->account(accountId))
            m_mgr->select(accountId);
        Q_EMIT showEventRequested(uid);
        Q_EMIT raiseRequested();
    });
    connect(m_notifier, &Notifier::activated, this, [this](int accountId, qint64 msgId) {
        if (accountId >= 0 && m_mgr->account(accountId)) {
            m_mgr->select(accountId);
            selectTag(tag::Inbox);
            if (msgId)
                selectMessage(msgId);
        }
        Q_EMIT raiseRequested();
    });
    connect(m_mgr, &AccountManager::accountsChanged, this, &MailApp::wireNotifications);
    wireNotifications();
    switchAccount();
}

// Every account's new mail, not only the one on screen.
void MailApp::wireNotifications()
{
    const QList<Account *> all = m_mgr->accounts();
    m_notifier->setAccountCount(int(all.size()));
    for (Account *a : all) {
        if (a->property("e3NotifyWired").toBool())
            continue;
        a->setProperty("e3NotifyWired", true);
        connect(a, &Account::newMail, this, [this, a](qint64 id) {
            // Our own mail from another device is filed as Sent, not announced.
            if (const auto d = mail::Search::detail(a->ctx(), id); d && d->direction != Direction::Outgoing)
                m_notifier->arrived(a->id(), a->addr(), id, d->from.name.isEmpty() ? d->from.addr : d->from.name,
                                    d->subject);
        });
    }
}

QString MailApp::calendarMode() const
{
    const QString mode = appSettings().value(QStringLiteral("calendarMode")).toString();
    return mode == QLatin1String("week") || mode == QLatin1String("day") ? mode : QStringLiteral("month");
}

void MailApp::setCalendarMode(const QString &mode)
{
    if (mode == calendarMode() || (mode != QLatin1String("month") && mode != QLatin1String("week") && mode != QLatin1String("day")))
        return;
    QSettings s = appSettings();
    s.setValue(QStringLiteral("calendarMode"), mode);
    Q_EMIT calendarModeChanged();
}

QString MailApp::notificationMode() const
{
    return Notifier::modeToString(m_notifier->mode());
}

void MailApp::setNotificationMode(const QString &mode)
{
    const Notifier::Mode m = Notifier::modeFromString(mode);
    if (m == m_notifier->mode())
        return;
    m_notifier->setMode(m);
    QSettings s = appSettings();
    s.setValue(QStringLiteral("notifications"), Notifier::modeToString(m));
    Q_EMIT notificationsChanged();
}

bool MailApp::notificationsAvailable() const
{
    return m_notifier->available();
}

bool MailApp::backgroundSync() const
{
    return androidsync::available() && appSettings().value(QStringLiteral("backgroundSync")).toBool();
}

void MailApp::setBackgroundSync(bool on)
{
    if (!androidsync::available() || on == backgroundSync())
        return;
    QSettings s = appSettings();
    s.setValue(QStringLiteral("backgroundSync"), on);
    on ? androidsync::start() : androidsync::stop();
    Q_EMIT backgroundSyncChanged();
}

bool MailApp::backgroundSyncAvailable() const
{
    return androidsync::available();
}

MailApp *MailApp::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

QString MailApp::version() const
{
    return QStringLiteral(E3MAIL_VERSION);
}

bool MailApp::portable() const
{
    return Paths::isPortable();
}

bool MailApp::firstRunAcknowledged() const
{
    return QFile::exists(Paths::firstRunMarker());
}

void MailApp::acknowledgeFirstRun()
{
    QFile f(Paths::firstRunMarker());
    if (f.open(QIODevice::WriteOnly))
        f.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8() + '\n');
    Q_EMIT firstRunChanged();
}

bool MailApp::hasAccounts() const
{
    for (Account *a : m_mgr->accounts()) {
        if (a->isConfigured())
            return true;
    }
    return false;
}

QVariantList MailApp::accounts() const
{
    QVariantList out;
    for (Account *a : m_mgr->accounts()) {
        if (!a->isConfigured())
            continue;
        out.append(QVariantMap{{QStringLiteral("id"), a->id()},
                               {QStringLiteral("addr"), a->addr()},
                               {QStringLiteral("name"), a->displayName()},
                               {QStringLiteral("status"), a->status()}});
    }
    return out;
}

Account *MailApp::account() const
{
    return m_mgr->selected();
}

int MailApp::currentAccountId() const
{
    return m_mgr->selectedId();
}

QString MailApp::accountAddr() const
{
    return account() ? account()->addr() : QString();
}

QString MailApp::accountName() const
{
    return account() ? account()->displayName() : QString();
}

QString MailApp::accountStatus() const
{
    return account() ? account()->status() : QString();
}

QVariantMap MailApp::refusedCertificate() const
{
    if (!account() || account()->refusedSha256().isEmpty())
        return {};
    return {{QStringLiteral("server"), account()->refusedServer()},
            {QStringLiteral("fingerprint"), net::formatFingerprint(account()->refusedSha256())}};
}

void MailApp::trustRefusedCertificate()
{
    if (account())
        account()->trustRefusedCertificate();
}

void MailApp::forgetCertificate(const QString &server)
{
    if (account())
        account()->forgetCertificate(server);
}

bool MailApp::devicesAvailable() const
{
    return Account::devicesAvailable();
}

bool MailApp::scanAvailable() const
{
#ifdef E3MAIL_HAVE_CAMERA
    // Screenshots show the scan button on machines without a camera.
    return QrScanner::cameraPresent() || qEnvironmentVariableIsSet("E3MAIL_SCREENSHOT_CAMERA");
#else
    return false;
#endif
}

QVariantList MailApp::devices() const
{
    QVariantList out;
    if (!account())
        return out;
    Statement st(account()->ctx().db, "SELECT id, name, last_seen FROM devices ORDER BY paired_at");
    while (st.step()) {
        const QString id = st.text(0);
        out.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), st.text(1).isEmpty() ? tr("Unnamed device") : st.text(1)},
            {QStringLiteral("online"), account()->isDeviceConnected(id)},
            {QStringLiteral("lastSeen"), st.isNull(2) ? QString()
                 : QLocale().toString(QDateTime::fromMSecsSinceEpoch(st.int64(2)), QLocale::ShortFormat)}});
    }
    return out;
}

QString MailApp::deviceName() const
{
    return account() ? account()->ctx().config.get(cfg::DeviceName) : QString();
}

QString MailApp::removedBy() const
{
    return account() ? account()->ctx().config.get(cfg::P2pRemovedBy) : QString();
}

void MailApp::dismissRemoved()
{
    if (!account())
        return;
    account()->ctx().config.set(cfg::P2pRemovedBy, QString());
    Q_EMIT devicesChanged();
}

void MailApp::setDeviceName(const QString &name)
{
    if (!account() || name.trimmed() == deviceName())
        return;
    account()->ctx().config.set(cfg::DeviceName, name.trimmed().left(64));
    Q_EMIT devicesChanged();
}

QString MailApp::relay() const
{
    return account() ? account()->ctx().config.get(cfg::P2pRelay) : QString();
}

void MailApp::setRelay(const QString &relay)
{
    if (!account() || relay.trimmed() == this->relay())
        return;
    account()->ctx().config.set(cfg::P2pRelay, relay.trimmed());
    // The endpoint starts again with it, if it was running.
    if (account()->deviceTransport()) {
        account()->stopDevices();
        account()->startDevices();
    }
    Q_EMIT devicesChanged();
}

QString MailApp::defaultDeviceName() const
{
    // Set by scripts/screenshots.sh, so no real host name is pictured.
    if (const QString set = qEnvironmentVariable("E3MAIL_DEVICE_NAME"); !set.isEmpty())
        return set;
    const QString host = QSysInfo::machineHostName().section(u'.', 0, 0);
    return host.isEmpty() || host == QLatin1String("localhost") ? QSysInfo::prettyProductName() : host;
}

QString MailApp::codeAccount(const QString &code) const
{
#ifdef E3MAIL_HAVE_P2P
    if (const auto c = pairing::parseCode(code))
        return c->account;
#endif
    Q_UNUSED(code)
    return {};
}

QString MailApp::startPairing()
{
#ifdef E3MAIL_HAVE_P2P
    cancelPairing();
    if (!account())
        return {};
    if (account()->ctx().config.get(cfg::DeviceName).isEmpty())
        account()->ctx().config.set(cfg::DeviceName, defaultDeviceName());
    auto *offer = new PairingOffer(*account(), this);
    if (!offer->isReady()) {
        offer->deleteLater();
        return {};
    }
    connect(offer, &PairingOffer::requested, this, &MailApp::pairingRequested);
    connect(offer, &PairingOffer::progress, this, &MailApp::pairingProgress);
    connect(offer, &PairingOffer::finished, this, [this](bool ok, const QString &message) {
        Q_EMIT pairingFinished(ok, message);
        Q_EMIT devicesChanged();
    });
    m_offer = offer;
    return offer->code();
#else
    return {};
#endif
}

void MailApp::allowPairing()
{
#ifdef E3MAIL_HAVE_P2P
    if (auto *o = qobject_cast<PairingOffer *>(m_offer.data()))
        o->allow();
#endif
}

void MailApp::denyPairing()
{
#ifdef E3MAIL_HAVE_P2P
    if (auto *o = qobject_cast<PairingOffer *>(m_offer.data()))
        o->deny();
#endif
}

void MailApp::cancelPairing()
{
    if (m_offer)
        m_offer->deleteLater();
    m_offer = nullptr;
}

void MailApp::removeDevice(const QString &id)
{
    if (account())
        account()->removeDevice(id);
    Q_EMIT devicesChanged();
}

bool MailApp::joinDevice(const QString &code, const QString &name)
{
#ifdef E3MAIL_HAVE_P2P
    cancelJoin();
    auto *join = new PairingJoin(*m_mgr, code, name.trimmed().isEmpty() ? defaultDeviceName() : name, this);
    if (!join->isValid()) {
        join->deleteLater();
        return false;
    }
    connect(join, &PairingJoin::waiting, this, &MailApp::joinWaiting);
    connect(join, &PairingJoin::progress, this, &MailApp::joinProgress);
    connect(join, &PairingJoin::finished, this, [this, join](bool ok, const QString &message, int id) {
        if (ok) {
            if (Account *a = m_mgr->account(id))
                a->start();
            accountConfigured();
        }
        Q_EMIT joinFinished(ok, message);
        join->deleteLater();
    });
    m_join = join;
    return true;
#else
    Q_UNUSED(code)
    Q_UNUSED(name)
    return false;
#endif
}

void MailApp::cancelJoin()
{
    if (m_join)
        m_join->deleteLater();
    m_join = nullptr;
}

QString MailApp::accountSignsInWith() const
{
    if (!account())
        return {};
    const QString auth = account()->ctx().config.get(cfg::Auth);
    if (!auth.startsWith(QLatin1String("oauth:")))
        return {};
    const auto p = net::oauth::known(auth.mid(6));
    return p ? p->name : QString();
}

void MailApp::signInAgain()
{
    Account *a = account();
    if (!a)
        return;
    const Account::Settings s = a->settings();
    auto provider = net::oauth::known(s.auth.mid(6));
    if (!s.auth.startsWith(QLatin1String("oauth:")) || !provider)
        return;
    // The client the account was set up with, so that renewal keeps working.
    if (!s.oauthClientId.isEmpty()) {
        provider->clientId = s.oauthClientId;
        provider->clientSecret = s.oauthClientSecret;
    }
    if (!s.oauthTokenUrl.isEmpty())
        provider->tokenUrl = QUrl(s.oauthTokenUrl);
    auto *signIn = new SignIn(*provider, s.addr, this);
    connect(signIn, &SignIn::openBrowser, this, [](const QUrl &url) { QDesktopServices::openUrl(url); });
    const QPointer<Account> account(a);
    connect(signIn, &SignIn::finished, this, [this, signIn, account](const QString &error) {
        signIn->deleteLater();
        if (!account)
            return;
        if (!error.isEmpty() || (!signIn->email().isEmpty()
                                 && signIn->email().compare(account->addr(), Qt::CaseInsensitive) != 0)) {
            Q_EMIT notify(error.isEmpty() ? tr("You signed in as %1, not as %2.").arg(signIn->email(), account->addr())
                                         : error);
            return;
        }
        account->storeRefreshToken(signIn->refreshToken());
        account->stop();
        account->start();
    });
    signIn->start();
}

void MailApp::backUp(Account *a)
{
    if (!a || m_backingUp.contains(a->id()))
        return;
    m_backingUp.insert(a->id());
    Q_EMIT backupsChanged();
    auto *w = new QFutureWatcher<backups::Outcome>(this);
    const QPointer<Account> account(a);
    const int id = a->id();
    connect(w, &QFutureWatcher<backups::Outcome>::finished, this, [this, w, account, id] {
        w->deleteLater();
        m_backingUp.remove(id);
        if (account)
            backups::record(*account, w->result());
        Q_EMIT backupsChanged();
    });
    w->setFuture(backups::run(*a));
}

bool MailApp::backingUp(const Account *a) const
{
    return a && m_backingUp.contains(a->id());
}

QString MailApp::accountStatusDetail() const
{
    return account() ? account()->statusDetail() : QString();
}

int MailApp::olderRemaining() const
{
    return account() ? account()->olderRemaining() : 0;
}

void MailApp::switchAccount()
{
    if (m_connected)
        disconnect(m_connected, nullptr, this, nullptr);
    m_connected = account();
    // Everything that belongs to an account is cleared here, unconditionally.
    m_tag = tag::Inbox;
    m_labelId = 0;
    m_search.clear();
    m_selected = 0;
    m_thread.clear();
    if (m_connected) {
        connect(m_connected, &Account::mailChanged, this, &MailApp::scheduleRefresh);
        connect(m_connected, &Account::statusChanged, this, &MailApp::statusChanged);
        connect(m_connected, &Account::statusChanged, this, &MailApp::accountsChanged);
        connect(m_connected, &Account::statusChanged, this, &MailApp::devicesChanged);
        connect(m_connected, &Account::newMail, this, [this](qint64 id) {
            if (!account())
                return;
            if (const auto d = mail::Search::detail(account()->ctx(), id))
                Q_EMIT newMailArrived(d->from.name.isEmpty() ? d->from.addr : d->from.name, d->subject);
        });
    }
    Q_EMIT accountChanged();
    Q_EMIT statusChanged();
    Q_EMIT devicesChanged();
    Q_EMIT viewChanged();
    Q_EMIT selectionChanged();
    refresh();
}

void MailApp::accountConfigured()
{
    for (Account *a : m_mgr->accounts()) {
        disconnect(a, &Account::statusChanged, this, &MailApp::accountsChanged);
        connect(a, &Account::statusChanged, this, &MailApp::accountsChanged);
    }
    Q_EMIT accountsChanged();
    switchAccount();
}

void MailApp::selectAccount(int id)
{
    m_mgr->select(id);
}

void MailApp::removeAccount(int id)
{
    m_mgr->remove(id);
    Q_EMIT accountsChanged();
    switchAccount();
}

void MailApp::scheduleRefresh()
{
    m_refresh.start();
}

void MailApp::refresh()
{
    reloadMessages();
    reloadLabels();
    reloadCounts();
    reloadThread();
    reloadSignatures();
    Q_EMIT calendarChanged();
    armReminder();
}

void MailApp::reloadMessages()
{
    if (!account()) {
        m_messages.setMessages({});
        return;
    }
    SearchQuery q = SearchQuery::forTag(m_labelId ? QString() : m_tag);
    q.labelId = m_labelId;
    q.text = m_search;
    if (!m_search.isEmpty() && m_tag == tag::Inbox && !m_labelId)
        q.tag = tag::All; // searching from the inbox searches everything
    m_messages.setMessages(mail::Search::list(account()->ctx(), q));
}

void MailApp::reloadLabels()
{
    m_labels.clear();
    if (account()) {
        for (const LabelInfo &l : mail::Organize::labels(account()->ctx()))
            m_labels.append(QVariantMap{{QStringLiteral("id"), l.id},
                                        {QStringLiteral("name"), l.name},
                                        {QStringLiteral("color"), l.color},
                                        {QStringLiteral("count"), l.count}});
    }
    Q_EMIT labelsChanged();
}

void MailApp::reloadCounts()
{
    m_counts.clear();
    if (account()) {
        MailContext &ctx = account()->ctx();
        m_counts.insert(QStringLiteral("inbox"), mail::Search::count(ctx, tag::Inbox, true));
        m_counts.insert(QStringLiteral("unverified"), mail::Search::count(ctx, tag::Unverified, false));
        m_counts.insert(QStringLiteral("drafts"), mail::Search::count(ctx, tag::Drafts, false));
        m_counts.insert(QStringLiteral("trash"), mail::Search::count(ctx, tag::Trash, false));
        m_counts.insert(QStringLiteral("outbox"),
                        int(ctx.db.queryInt("SELECT count(*) FROM outbox").value_or(0)));
    }
    Q_EMIT countsChanged();
}

void MailApp::selectTag(const QString &t)
{
    m_tag = t;
    m_labelId = 0;
    m_selected = 0;
    Q_EMIT viewChanged();
    reloadMessages();
    reloadThread();
}

void MailApp::selectLabel(qint64 labelId)
{
    m_labelId = labelId;
    m_tag.clear();
    m_selected = 0;
    Q_EMIT viewChanged();
    reloadMessages();
    reloadThread();
}

void MailApp::setSearch(const QString &text)
{
    if (text == m_search)
        return;
    m_search = text;
    Q_EMIT viewChanged();
    reloadMessages();
}

void MailApp::selectMessage(qint64 id)
{
    m_selected = id;
    if (account() && id) {
        const auto d = mail::Search::detail(account()->ctx(), id);
        if (d && d->unread) {
            mail::Organize::markRead(account()->ctx(), id, true);
            scheduleRefresh();
        }
    }
    reloadThread();
}

void MailApp::reloadThread()
{
    m_thread.clear();
    if (account() && m_selected) {
        if (!mail::Search::detail(account()->ctx(), m_selected))
            m_selected = 0;
        else
            for (qint64 id : mail::Search::thread(account()->ctx(), m_selected))
                m_thread.append(messageMap(id));
    }
    Q_EMIT selectionChanged();
}

QVariantMap MailApp::messageMap(qint64 id) const
{
    const auto d = mail::Search::detail(account()->ctx(), id);
    if (!d)
        return {};
    // Images the message carries for its HTML (cid:) are shown in it, from
    // the stored parts, and are not listed again as attachments.
    QString html = d->bodyHtml;
    QSet<qsizetype> shownInline;
    for (qsizetype i = 0; i < d->attachments.size() && !html.isEmpty(); ++i) {
        const AttachmentInfo &a = d->attachments[i];
        if (a.contentId.isEmpty())
            continue;
        const QString ref = QStringLiteral("src=\"cid:") + mime::escapeHtml(a.contentId) + u'"';
        if (!html.contains(ref))
            continue;
        const QImage img = InlineImages::decode(account()->ctx().blobs.get(a.blob), a.mimeType);
        if (img.isNull())
            continue;
        const QString url = InlineImages::put(QStringLiteral("%1-%2").arg(account()->id()).arg(d->id), int(i), img);
        html.replace(ref, QStringLiteral("src=\"") + url + u'"');
        shownInline.insert(i);
    }
    // A cid: that names no part we can show becomes its alt text, as a
    // remote image does.
    static const QRegularExpression unresolved(QStringLiteral("<img src=\"cid:[^\"]*\"(?: width=\"\\d+\")?(?: alt=\"([^\"]*)\")?>"));
    for (auto m = unresolved.match(html); m.hasMatch(); m = unresolved.match(html, m.capturedStart())) {
        const QString alt = m.captured(1);
        html.replace(m.capturedStart(), m.capturedLength(), alt.isEmpty() ? QString() : u'[' + alt + u']');
    }
    QVariantList attachments;
    for (qsizetype i = 0; i < d->attachments.size(); ++i) {
        const AttachmentInfo &a = d->attachments[i];
        if (shownInline.contains(i))
            continue;
        attachments.append(QVariantMap{{QStringLiteral("index"), int(i)},
                                       {QStringLiteral("name"), a.filename},
                                       {QStringLiteral("type"), a.mimeType},
                                       {QStringLiteral("size"), QLocale().formattedDataSize(a.size)}});
    }
    QVariantList labels;
    for (const LabelInfo &l : d->labels)
        labels.append(QVariantMap{{QStringLiteral("id"), l.id}, {QStringLiteral("name"), l.name},
                                  {QStringLiteral("color"), l.color}});
    int heldDaysLeft = -1;
    if (d->held) {
        const int window = account()->ctx().config.getInt(cfg::UnverifiedTrashDays);
        if (window > 0)
            heldDaysLeft = daysUntil(d->heldAt.toUTC().addDays(window)); // UTC: no DST hour
    }
    return {
        {QStringLiteral("id"), d->id},
        {QStringLiteral("from"), d->from.name.isEmpty() ? d->from.addr : d->from.name},
        {QStringLiteral("fromAddr"), d->from.addr},
        {QStringLiteral("to"), addrList(d->to)},
        {QStringLiteral("cc"), addrList(d->cc)},
        {QStringLiteral("bcc"), addrList(d->bcc)},
        {QStringLiteral("subject"), d->subject.isEmpty() ? tr("(no subject)") : d->subject},
        {QStringLiteral("date"), QLocale().toString(d->date.toLocalTime().date(), QLocale::LongFormat) + u' '
                                     + QLocale().toString(d->date.toLocalTime().time(), QLocale::ShortFormat)},
        {QStringLiteral("shortDate"), MessageListModel::formatDate(d->date)},
        {QStringLiteral("html"), html},
        {QStringLiteral("text"), d->bodyText},
        {QStringLiteral("remoteBlocked"), d->remoteBlocked},
        {QStringLiteral("encrypted"), d->encrypted},
        {QStringLiteral("signed"), d->signedBySender},
        {QStringLiteral("verified"), d->verified},
        {QStringLiteral("importance"), d->importance},
        {QStringLiteral("attachments"), attachments},
        {QStringLiteral("undelivered"), d->undelivered.join(QStringLiteral(", "))},
        {QStringLiteral("held"), d->held},
        {QStringLiteral("heldDaysLeft"), heldDaysLeft},
        {QStringLiteral("trashed"), d->trashed},
        {QStringLiteral("trashReason"), d->trashed ? reasonText(d->trashReason) : QString()},
        {QStringLiteral("purgeDays"), d->trashed ? daysUntil(d->purgeAt) : -1},
        {QStringLiteral("archived"), d->archived},
        {QStringLiteral("outgoing"), d->direction == Direction::Outgoing},
        {QStringLiteral("state"), int(d->state)},
        {QStringLiteral("rawAvailable"), d->rawAvailable},
        {QStringLiteral("labels"), labels},
        {QStringLiteral("unread"), d->unread},
        {QStringLiteral("receiptAsked"), d->direction == Direction::Outgoing && !d->receiptTo.isEmpty()},
        {QStringLiteral("readBy"), [&] {
             QStringList names;
             for (const auto &r : d->readBy) {
                 const auto c = mail::Contacts::get(account()->ctx(), r.addr);
                 names.append(c && !c->name.isEmpty() ? c->name : r.addr);
             }
             return names.join(QStringLiteral(", "));
         }()},
        {QStringLiteral("invitation"), [&]() -> QVariantMap {
             const auto inv = mail::Calendar::invitation(account()->ctx(), *d);
             if (!inv)
                 return {};
             auto statusName = [](mail::ICal::PartStat p) {
                 return QString::fromLatin1(mail::ICal::partStatName(p)).toLower();
             };
             QVariantList attendees;
             for (const auto &a : inv->attendees)
                 attendees.append(QVariantMap{{QStringLiteral("addr"), a.addr},
                                              {QStringLiteral("name"), a.name.isEmpty() ? a.addr : a.name},
                                              {QStringLiteral("status"), statusName(a.partstat)}});
             return {{QStringLiteral("method"), QString::fromLatin1(mail::ICal::methodName(inv->method)).toLower()},
                     {QStringLiteral("uid"), inv->event.uid},
                     {QStringLiteral("title"), inv->event.summary},
                     {QStringLiteral("when"), mail::Calendar::describeWhen(inv->event)},
                     {QStringLiteral("where"), inv->event.location},
                     {QStringLiteral("description"), inv->event.description},
                     {QStringLiteral("url"), inv->event.url},
                     {QStringLiteral("recurrence"), mail::Calendar::describeRule(inv->event.rrule)},
                     {QStringLiteral("organizer"), inv->event.organizer.name.isEmpty() ? inv->event.organizer.addr : inv->event.organizer.name},
                     {QStringLiteral("attendees"), attendees},
                     {QStringLiteral("myStatus"), inv->ourStatus ? statusName(*inv->ourStatus) : QString()},
                     {QStringLiteral("inCalendar"), inv->inCalendar},
                     {QStringLiteral("outdated"), inv->outdated},
                     {QStringLiteral("cancelled"), inv->cancelled || inv->event.status == mail::ICal::Status::Cancelled},
                     {QStringLiteral("mine"), inv->organizerIsSelf},
                     {QStringLiteral("organizerIsSender"), inv->organizerIsSender},
                     {QStringLiteral("active"), mail::Calendar::actionable(account()->ctx(), *d)},
                     {QStringLiteral("canAdd"), d->direction == Direction::Incoming && !d->held && !d->trashed
                                                    && inv->method != mail::ICal::Method::Reply && !inv->outdated},
                     {QStringLiteral("replyFrom"), inv->replyFrom},
                     {QStringLiteral("replyStatus"), statusName(inv->replyStatus)}};
         }()},
        {QStringLiteral("structuredActive"), mail::Structured::actionable(account()->ctx(), *d)},
        {QStringLiteral("structured"), [&] {
             QVariantList out;
             const QLocale locale;
             const auto when = [&locale](const QDateTime &t, bool allDay) {
                 return allDay ? locale.toString(t.date(), QLocale::LongFormat)
                               : locale.toString(t.toLocalTime(), QLocale::ShortFormat);
             };
             for (const auto &it : mail::Structured::parse(d->structured)) {
                 QVariantList fields;
                 if (it.start.isValid()) {
                     QString w = when(it.start, it.allDay);
                     if (it.end.isValid())
                         w += QStringLiteral(" – ") + when(it.end, it.allDay);
                     fields.append(QVariantMap{{QStringLiteral("label"), tr("When")}, {QStringLiteral("value"), w}});
                 }
                 if (!it.place.isEmpty())
                     fields.append(QVariantMap{{QStringLiteral("label"), tr("Where")}, {QStringLiteral("value"), it.place}});
                 for (const auto &f : it.fields)
                     fields.append(QVariantMap{{QStringLiteral("label"), f.label}, {QStringLiteral("value"), f.value}});
                 out.append(QVariantMap{{QStringLiteral("title"), it.title},
                                        {QStringLiteral("fields"), fields},
                                        {QStringLiteral("calendar"), it.start.isValid()},
                                        {QStringLiteral("link"), it.link}});
             }
             return out;
         }()},
        {QStringLiteral("disappear"), [&] {
             // Rounded up, in the largest whole unit: "in 3 hours", not "in 2 h 59".
             if (d->trashed || d->ephemeralSeconds <= 0)
                 return QString();
             if (!d->expiresAt.isValid())
                 return tr("disappears %1 after reading").arg(lifetime(d->ephemeralSeconds));
             const qint64 left = qMax<qint64>(60, QDateTime::currentDateTime().secsTo(d->expiresAt));
             const qint64 unit = left > 2 * 86400 ? 86400 : left > 2 * 3600 ? 3600 : 60;
             return tr("disappears in %1").arg(lifetime(int((left + unit - 1) / unit * unit)));
         }()},
    };
}

void MailApp::armExpiry()
{
    std::optional<qint64> next;
    for (Account *a : m_mgr->accounts()) {
        if (const auto at = mail::Organize::nextExpiry(a->ctx()))
            next = next ? qMin(*next, *at) : *at;
    }
    if (!next) {
        m_expiry.stop();
        return;
    }
    // At most a day ahead, so a clock change is noticed; never in the past.
    const qint64 wait = qBound<qint64>(0, *next - QDateTime::currentMSecsSinceEpoch() + 500, 24LL * 3600 * 1000);
    m_expiry.start(int(wait));
}

void MailApp::armReminder()
{
    std::optional<qint64> next;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (Account *a : m_mgr->accounts()) {
        if (const auto at = mail::Calendar::nextReminderMs(a->ctx(), now))
            next = next ? qMin(*next, *at) : *at;
    }
    if (!next) {
        m_reminder.stop();
        return;
    }
    m_reminder.start(int(qBound<qint64>(0, *next - now + 500, 24LL * 3600 * 1000)));
}

void MailApp::syncNow()
{
    for (Account *a : m_mgr->accounts())
        a->syncNow();
}

#define WITH_ACCOUNT(stmt)                                                                                             \
    do {                                                                                                               \
        if (Account *a = account()) {                                                                                  \
            MailContext &ctx = a->ctx();                                                                               \
            stmt;                                                                                                      \
            a->notifyChanged();                                                                                        \
        }                                                                                                              \
    } while (0)

void MailApp::markRead(qint64 id, bool read)
{
    WITH_ACCOUNT(mail::Organize::markRead(ctx, id, read));
}

void MailApp::trash(qint64 id)
{
    WITH_ACCOUNT(mail::Organize::trash(ctx, id, TrashReason::User));
    if (id == m_selected)
        m_selected = 0;
    Q_EMIT notify(tr("Moved to Trash."));
}

void MailApp::restore(qint64 id)
{
    WITH_ACCOUNT(mail::Organize::restore(ctx, id));
    Q_EMIT notify(tr("Restored."));
}

void MailApp::purge(qint64 id)
{
    WITH_ACCOUNT(mail::Organize::purge(ctx, id));
    if (id == m_selected)
        m_selected = 0;
}

void MailApp::archive(qint64 id, bool archived)
{
    WITH_ACCOUNT(mail::Organize::archive(ctx, id, archived));
    if (archived && m_tag == tag::Inbox && id == m_selected)
        m_selected = 0;
    Q_EMIT notify(archived ? tr("Archived.") : tr("Moved to Inbox."));
}

void MailApp::emptyTrash()
{
    WITH_ACCOUNT(mail::Organize::emptyTrash(ctx));
    m_selected = 0;
}

void MailApp::accept(const QString &addr)
{
    int released = 0;
    WITH_ACCOUNT(released = mail::Organize::accept(ctx, addr));
    Q_EMIT notify(tr("Accepted %1. %n message(s) moved to the Inbox.", "", released).arg(addr));
}

void MailApp::block(const QString &pattern)
{
    WITH_ACCOUNT(mail::Contacts::block(ctx, pattern));
    Q_EMIT notify(tr("Blocked %1. Their mail will go to Trash.").arg(pattern));
}

void MailApp::unblock(const QString &pattern)
{
    WITH_ACCOUNT(mail::Contacts::unblock(ctx, pattern));
}

QStringList MailApp::blocklist() const
{
    return account() ? mail::Contacts::blocklist(account()->ctx()) : QStringList();
}

void MailApp::setLabel(qint64 msgId, qint64 labelId, bool on)
{
    WITH_ACCOUNT(mail::Organize::setLabel(ctx, msgId, labelId, on));
}

qint64 MailApp::createLabel(const QString &name, const QString &color)
{
    qint64 id = 0;
    WITH_ACCOUNT(id = mail::Organize::createLabel(ctx, name, color));
    return id;
}

void MailApp::renameLabel(qint64 labelId, const QString &name, const QString &color)
{
    WITH_ACCOUNT(mail::Organize::renameLabel(ctx, labelId, name, color));
}

void MailApp::deleteLabel(qint64 labelId)
{
    WITH_ACCOUNT(mail::Organize::deleteLabel(ctx, labelId));
    if (m_labelId == labelId)
        selectTag(tag::Inbox);
}

bool MailApp::saveAttachment(qint64 msgId, int index, const QUrl &dest)
{
    if (!account())
        return false;
    const auto d = mail::Search::detail(account()->ctx(), msgId);
    if (!d || index < 0 || index >= d->attachments.size())
        return false;
    const bool ok = FileIo::write(dest, account()->ctx().blobs.get(d->attachments[index].blob));
    Q_EMIT notify(ok ? tr("Saved %1.").arg(d->attachments[index].filename) : tr("Could not save the attachment."));
    return ok;
}

QString MailApp::addStructuredToCalendar(qint64 msgId, int item)
{
    if (!account())
        return {};
    const auto d = mail::Search::detail(account()->ctx(), msgId);
    // Only from a card that offers it: the same rule as the button.
    if (!d || !mail::Structured::actionable(account()->ctx(), *d))
        return {};
    const auto items = mail::Structured::parse(d->structured);
    if (item < 0 || item >= items.size() || !items[item].start.isValid())
        return {};
    const auto &it = items[item];
    mail::ICal::Event e;
    e.summary = it.title;
    e.allDay = it.allDay;
    e.start = it.start;
    if (it.end.isValid())
        e.end = it.allDay ? it.end.addDays(1) : it.end;
    else
        e.end = it.start.addSecs(it.allDay ? 86400 : 3600);
    e.location = it.place;
    QStringList lines;
    for (const auto &f : it.fields)
        lines.append(f.label + QStringLiteral(": ") + f.value);
    e.description = lines.join(u'\n');
    e.url = it.link;
    QString uid;
    WITH_ACCOUNT(uid = mail::Calendar::addEvent(ctx, e, d->messageId));
    Q_EMIT notify(uid.isEmpty() ? tr("Could not add the event.") : tr("Added to the calendar."));
    return uid;
}

namespace {

QString partStatName(mail::ICal::PartStat p)
{
    return QString::fromLatin1(mail::ICal::partStatName(p)).toLower();
}

} // namespace

QVariantMap MailApp::eventMap(const mail::Calendar::Event &e, const QDateTime &start, const QDateTime &end) const
{
    auto &ctx = account()->ctx();
    QVariantList attendees;
    for (const auto &a : e.attendees)
        attendees.append(QVariantMap{{QStringLiteral("addr"), a.addr},
                                     {QStringLiteral("name"), a.name.isEmpty() ? a.addr : a.name},
                                     {QStringLiteral("status"), partStatName(a.partstat)}});
    mail::ICal::Event instance = e;
    instance.start = start;
    instance.end = end;
    const auto mine = mail::Calendar::ourStatus(ctx, e);
    return {{QStringLiteral("uid"), e.uid},
            {QStringLiteral("title"), e.summary.isEmpty() ? tr("(No title)") : e.summary},
            {QStringLiteral("start"), start.toLocalTime()},
            {QStringLiteral("end"), end.toLocalTime()},
            {QStringLiteral("allDay"), e.allDay},
            {QStringLiteral("location"), e.location},
            {QStringLiteral("description"), e.description},
            {QStringLiteral("url"), e.url},
            {QStringLiteral("organizer"), e.organizer.addr},
            {QStringLiteral("organizerName"), e.organizer.name.isEmpty() ? e.organizer.addr : e.organizer.name},
            {QStringLiteral("mine"), e.organizer.addr.isEmpty() || mail::Calendar::organizedByUs(ctx, e)},
            {QStringLiteral("status"), int(e.status)},
            {QStringLiteral("myStatus"), mine ? partStatName(*mine) : QString()},
            {QStringLiteral("recurring"), !e.rrule.isEmpty()},
            {QStringLiteral("recurrence"), mail::Calendar::describeRule(e.rrule)},
            {QStringLiteral("attendees"), attendees},
            {QStringLiteral("reminder"), e.reminderMinutes ? *e.reminderMinutes : -1},
            {QStringLiteral("when"), mail::Calendar::describeWhen(instance)},
            {QStringLiteral("timeText"), e.allDay ? tr("All day")
                                                  : QLocale().toString(start.toLocalTime().time(), QLocale::ShortFormat) + QStringLiteral(" – ")
                                                        + QLocale().toString(end.toLocalTime().time(), QLocale::ShortFormat)},
            {QStringLiteral("day"), (e.allDay ? start.toUTC().date() : start.toLocalTime().date()).toString(Qt::ISODate)},
            // The last day it touches. An all-day event's days are its
            // dates, not where its UTC midnights fall on this clock.
            {QStringLiteral("lastDay"),
             (e.allDay ? qMax(start.toUTC().date(), end.toUTC().date().addDays(-1))
                       : (end > start ? end.addMSecs(-1) : start).toLocalTime().date())
                 .toString(Qt::ISODate)},
            {QStringLiteral("movable"), mail::Calendar::movable(ctx, e)},
            {QStringLiteral("sent"), mail::Calendar::organizedByUs(ctx, e) && !e.attendees.isEmpty()}};
}

QVariantList MailApp::calendarEvents(const QDateTime &from, const QDateTime &to) const
{
    QVariantList out;
    if (!account() || !from.isValid() || !to.isValid())
        return out;
    for (const auto &o : mail::Calendar::list(account()->ctx(), from.toUTC(), to.toUTC()))
        out.append(eventMap(o.event, o.start, o.end));
    return out;
}

QVariantMap MailApp::calendarEvent(const QString &uid) const
{
    if (!account())
        return {};
    const auto e = mail::Calendar::get(account()->ctx(), uid);
    if (!e)
        return {};
    QVariantMap m = eventMap(*e, e->start, e->end);
    const QDateTime s = e->allDay ? e->start.toUTC() : e->start.toLocalTime();
    // The end as the editor shows it: all-day ends are inclusive on screen.
    const QDateTime t = e->allDay ? e->end.toUTC().addDays(-1) : e->end.toLocalTime();
    m.insert(QStringLiteral("startDate"), s.date().toString(Qt::ISODate));
    m.insert(QStringLiteral("startTime"), s.time().toString(QStringLiteral("HH:mm")));
    m.insert(QStringLiteral("endDate"), t.date().toString(Qt::ISODate));
    m.insert(QStringLiteral("endTime"), t.time().toString(QStringLiteral("HH:mm")));
    QStringList attendees;
    for (const auto &a : e->attendees)
        attendees.append(mime::Address{a.name, a.addr}.display());
    m.insert(QStringLiteral("attendees"), attendees.join(QStringLiteral(", ")));
    m.insert(QStringLiteral("attendeeList"), eventMap(*e, e->start, e->end).value(QStringLiteral("attendees")));
    const auto rule = e->rule();
    QString freq = QStringLiteral("none");
    QVariantList byDay;
    QString ends = QStringLiteral("never");
    if (rule) {
        switch (rule->freq) {
        case mail::ICal::Rule::Freq::Daily: freq = QStringLiteral("daily"); break;
        case mail::ICal::Rule::Freq::Weekly: freq = QStringLiteral("weekly"); break;
        case mail::ICal::Rule::Freq::Monthly: freq = QStringLiteral("monthly"); break;
        case mail::ICal::Rule::Freq::Yearly: freq = QStringLiteral("yearly"); break;
        }
        for (const Qt::DayOfWeek d : rule->byDay)
            byDay.append(int(d));
        if (rule->count > 0)
            ends = QStringLiteral("count");
        else if (rule->until.isValid())
            ends = QStringLiteral("until");
    } else if (!e->rrule.isEmpty()) {
        freq = QStringLiteral("other"); // kept as it came, not editable here
    }
    m.insert(QStringLiteral("freq"), freq);
    m.insert(QStringLiteral("rrule"), e->rrule);
    m.insert(QStringLiteral("interval"), rule ? rule->interval : 1);
    m.insert(QStringLiteral("byDay"), byDay);
    m.insert(QStringLiteral("ends"), ends);
    m.insert(QStringLiteral("count"), rule ? rule->count : 0);
    m.insert(QStringLiteral("untilDate"), rule && rule->until.isValid() ? rule->until.toLocalTime().date().toString(Qt::ISODate) : QString());
    m.insert(QStringLiteral("sent"), mail::Calendar::organizedByUs(account()->ctx(), *e) && !e->attendees.isEmpty());
    m.insert(QStringLiteral("sequence"), e->sequence);
    return m;
}

QString MailApp::saveEvent(const QVariantMap &f, bool sendUpdate)
{
    if (!account())
        return {};
    auto &ctx = account()->ctx();
    const QString uid = f.value(QStringLiteral("uid")).toString();
    mail::Calendar::Event e;
    if (!uid.isEmpty()) {
        if (const auto have = mail::Calendar::get(ctx, uid))
            e = *have;
    }
    e.uid = uid;
    e.summary = f.value(QStringLiteral("title")).toString().trimmed();
    e.allDay = f.value(QStringLiteral("allDay")).toBool();
    e.location = f.value(QStringLiteral("location")).toString().trimmed();
    e.description = f.value(QStringLiteral("description")).toString();
    e.url = f.value(QStringLiteral("url")).toString().trimmed();
    const QDate sd = QDate::fromString(f.value(QStringLiteral("startDate")).toString(), Qt::ISODate);
    const QDate ed = QDate::fromString(f.value(QStringLiteral("endDate")).toString(), Qt::ISODate);
    const QTime st = QTime::fromString(f.value(QStringLiteral("startTime")).toString(), QStringLiteral("HH:mm"));
    const QTime et = QTime::fromString(f.value(QStringLiteral("endTime")).toString(), QStringLiteral("HH:mm"));
    if (e.summary.isEmpty()) {
        Q_EMIT notify(tr("Give the event a title."));
        return {};
    }
    if (!sd.isValid() || (!e.allDay && !st.isValid())) {
        Q_EMIT notify(tr("The start is not a date and time."));
        return {};
    }
    if (e.allDay) {
        e.start = QDateTime(sd, QTime(0, 0), QTimeZone::UTC);
        e.end = QDateTime((ed.isValid() && ed >= sd ? ed : sd).addDays(1), QTime(0, 0), QTimeZone::UTC);
        e.tzid.clear();
    } else {
        const QTimeZone zone = QTimeZone::systemTimeZone();
        e.start = QDateTime(sd, st, zone).toUTC();
        const QDateTime end = QDateTime(ed.isValid() ? ed : sd, et.isValid() ? et : st.addSecs(3600), zone).toUTC();
        e.end = end > e.start ? end : e.start.addSecs(3600);
        e.tzid = QString::fromLatin1(zone.id());
    }
    // Recurrence
    const QString freq = f.value(QStringLiteral("freq")).toString();
    if (freq == QLatin1String("none")) {
        e.rrule.clear();
    } else if (freq != QLatin1String("other")) {
        mail::ICal::Rule r;
        r.freq = freq == QLatin1String("daily") ? mail::ICal::Rule::Freq::Daily
            : freq == QLatin1String("monthly")  ? mail::ICal::Rule::Freq::Monthly
            : freq == QLatin1String("yearly")   ? mail::ICal::Rule::Freq::Yearly
                                                : mail::ICal::Rule::Freq::Weekly;
        r.interval = qBound(1, f.value(QStringLiteral("interval")).toInt(), 999);
        if (r.freq == mail::ICal::Rule::Freq::Weekly) {
            for (const QVariant &d : f.value(QStringLiteral("byDay")).toList()) {
                const int day = d.toInt();
                if (day >= 1 && day <= 7 && !r.byDay.contains(Qt::DayOfWeek(day)))
                    r.byDay.append(Qt::DayOfWeek(day));
            }
        }
        const QString ends = f.value(QStringLiteral("ends")).toString();
        if (ends == QLatin1String("count"))
            r.count = qBound(1, f.value(QStringLiteral("count")).toInt(), 10000);
        else if (ends == QLatin1String("until")) {
            const QDate u = QDate::fromString(f.value(QStringLiteral("untilDate")).toString(), Qt::ISODate);
            if (u.isValid())
                r.until = QDateTime(u, QTime(23, 59, 59), e.allDay ? QTimeZone(QTimeZone::UTC) : QTimeZone::systemTimeZone()).toUTC();
        }
        e.rrule = mail::ICal::writeRule(r);
    }
    const int reminder = f.value(QStringLiteral("reminder"), -1).toInt();
    if (reminder >= 0)
        e.reminderMinutes = reminder;
    else
        e.reminderMinutes.reset();
    // Attendees, as a recipient line.
    QList<mail::ICal::Attendee> attendees;
    const QString self = mime::normalizeAddr(ctx.config.get(cfg::Addr));
    for (const mime::Address &a : mime::parseAddressList(f.value(QStringLiteral("attendees")).toString().toUtf8())) {
        const QString addr = mime::normalizeAddr(a.addr);
        if (!addr.contains(u'@') || addr == self)
            continue;
        mail::ICal::Attendee at;
        at.addr = addr;
        at.name = a.name;
        at.rsvp = true;
        for (const auto &old : e.attendees) {
            if (old.addr == addr) {
                at.partstat = old.partstat;
                if (at.name.isEmpty())
                    at.name = old.name;
            }
        }
        attendees.append(at);
    }
    // Someone else's meeting keeps its attendees; only the organizer edits them.
    if (e.organizer.addr.isEmpty() || mail::Calendar::organizedByUs(ctx, e))
        e.attendees = attendees;
    const mail::Calendar::SendResult r = mail::Calendar::save(ctx, e, sendUpdate);
    account()->notifyChanged();
    if (!r.error.isEmpty())
        Q_EMIT notify(tr("Saved, but the invitation was not sent: %1").arg(r.error));
    else if (!r.queued.isEmpty())
        Q_EMIT notify(tr("Invitation sent."));
    return r.uid;
}

void MailApp::cancelEvent(const QString &uid, bool sendCancel)
{
    if (!account())
        return;
    const auto r = mail::Calendar::cancel(account()->ctx(), uid, sendCancel);
    account()->notifyChanged();
    if (!r.error.isEmpty())
        Q_EMIT notify(tr("Cancelled, but the attendees were not told: %1").arg(r.error));
}

void MailApp::removeEvent(const QString &uid)
{
    WITH_ACCOUNT(mail::Calendar::removeLocal(ctx, uid));
}

void MailApp::replyToInvitation(qint64 msgId, const QString &answer)
{
    if (!account())
        return;
    const mail::ICal::PartStat p = answer == QLatin1String("accept") ? mail::ICal::PartStat::Accepted
        : answer == QLatin1String("decline")                        ? mail::ICal::PartStat::Declined
                                                                    : mail::ICal::PartStat::Tentative;
    const auto r = mail::Calendar::rsvp(account()->ctx(), msgId, p);
    account()->notifyChanged();
    account()->syncNow();
    if (r.uid.isEmpty())
        Q_EMIT notify(r.error);
    else if (!r.error.isEmpty())
        Q_EMIT notify(tr("Saved in your calendar, but the answer was not sent: %1").arg(r.error));
    else
        Q_EMIT notify(tr("Answer sent."));
}

void MailApp::addInvitationToCalendar(qint64 msgId)
{
    if (!account())
        return;
    const auto r = mail::Calendar::addFromMessage(account()->ctx(), msgId);
    account()->notifyChanged();
    Q_EMIT notify(r.uid.isEmpty() ? r.error : tr("Added to the calendar."));
}

bool MailApp::exportEvent(const QString &uid, const QUrl &dest)
{
    if (!account())
        return false;
    const QByteArray ics = mail::Calendar::exportIcs(account()->ctx(), uid);
    const bool ok = !ics.isEmpty() && FileIo::write(dest, ics);
    Q_EMIT notify(ok ? tr("Saved %1.").arg(FileIo::displayName(dest)) : tr("Could not save the event."));
    return ok;
}

QString MailApp::suggestedEventFileName(const QString &uid) const
{
    QString name = tr("event");
    if (account()) {
        if (const auto e = mail::Calendar::get(account()->ctx(), uid); e && !e->summary.isEmpty())
            name = e->summary;
    }
    static const QRegularExpression unsafe(QStringLiteral("[\\\\/:*?\"<>|]"));
    return name.replace(unsafe, QStringLiteral("_")).left(80) + QStringLiteral(".ics");
}

QString MailApp::describeRecurrence(const QString &rrule) const
{
    return mail::Calendar::describeRule(rrule);
}

QVariantMap MailApp::attendeePolicy(const QString &attendees) const
{
    QVariantMap out{{QStringLiteral("encrypt"), false}, {QStringLiteral("missing"), QStringList()}};
    if (!account())
        return out;
    QStringList addrs;
    for (const mime::Address &a : mime::parseAddressList(attendees.toUtf8())) {
        if (a.addr.contains(u'@'))
            addrs.append(mime::normalizeAddr(a.addr));
    }
    if (addrs.isEmpty())
        return out;
    const auto ready = mail::Policy::evaluate(account()->ctx(), addrs, SendEncryption::Auto);
    out.insert(QStringLiteral("encrypt"), ready.willEncrypt);
    out.insert(QStringLiteral("missing"), ready.missingKeys);
    out.insert(QStringLiteral("canSend"), ready.canSend);
    return out;
}

QString MailApp::formatDay(const QString &isoDate) const
{
    const QDate d = QDate::fromString(isoDate, Qt::ISODate);
    return d.isValid() ? QLocale().toString(d, QLocale::LongFormat) : QString();
}

QString MailApp::weekStart(const QString &isoDate) const
{
    const QDate d = QDate::fromString(isoDate, Qt::ISODate);
    if (!d.isValid())
        return {};
    const int back = (d.dayOfWeek() - int(QLocale().firstDayOfWeek()) + 7) % 7;
    return d.addDays(-back).toString(Qt::ISODate);
}

QVariantMap MailApp::calendarGrid(const QString &firstDay, int days) const
{
    const QDate first = QDate::fromString(firstDay, Qt::ISODate);
    if (!first.isValid())
        return {};
    days = qBound(1, days, 14);
    const QLocale locale;
    const QTimeZone zone = QTimeZone::systemTimeZone();
    // A day more on each side: an all-day event's dates are UTC's, and the
    // layout leaves out what does not belong.
    QList<mail::Calendar::Occurrence> occurrences;
    if (account())
        occurrences = mail::Calendar::list(account()->ctx(), QDateTime(first.addDays(-1), QTime(0, 0), zone).toUTC(),
                                           QDateTime(first.addDays(days + 1), QTime(0, 0), zone).toUTC());
    QList<mail::CalendarLayout::Item> items;
    items.reserve(occurrences.size());
    for (const auto &o : occurrences)
        items.append({o.start, o.end, o.event.allDay});
    const auto layout = mail::CalendarLayout::layout(items, first, days, zone);

    const QDate today = QDate::currentDate();
    QVariantList dayList;
    for (const auto &day : layout.days) {
        QVariantList blocks;
        for (const auto &b : day.blocks) {
            const auto &o = occurrences[b.item];
            QVariantMap m = eventMap(o.event, o.start, o.end);
            m.insert(QStringLiteral("startMin"), b.startMin);
            m.insert(QStringLiteral("endMin"), b.endMin);
            m.insert(QStringLiteral("column"), b.column);
            m.insert(QStringLiteral("columns"), b.columns);
            m.insert(QStringLiteral("before"), b.before);
            m.insert(QStringLiteral("after"), b.after);
            blocks.append(m);
        }
        dayList.append(QVariantMap{{QStringLiteral("iso"), day.date.toString(Qt::ISODate)},
                                   {QStringLiteral("weekday"), locale.standaloneDayName(day.date.dayOfWeek(), QLocale::ShortFormat)},
                                   {QStringLiteral("number"), locale.toString(day.date.day())},
                                   {QStringLiteral("label"), locale.toString(day.date, QLocale::LongFormat)},
                                   {QStringLiteral("today"), day.date == today},
                                   {QStringLiteral("blocks"), blocks}});
    }
    QVariantList bars;
    for (const auto &b : layout.bars) {
        const auto &o = occurrences[b.item];
        QVariantMap m = eventMap(o.event, o.start, o.end);
        m.insert(QStringLiteral("firstDay"), b.firstDay);
        m.insert(QStringLiteral("days"), b.days);
        m.insert(QStringLiteral("lane"), b.lane);
        m.insert(QStringLiteral("before"), b.before);
        m.insert(QStringLiteral("after"), b.after);
        bars.append(m);
    }
    QVariantList hours;
    const QString hour = LocalePatterns::hour(locale);
    for (int h = 0; h < 24; ++h)
        hours.append(locale.toString(QTime(h, 0), hour));
    return {{QStringLiteral("title"), days == 1 ? locale.toString(first, QLocale::LongFormat)
                                                : LocalePatterns::dateRange(locale, first, first.addDays(days - 1))},
            // For a phone's width.
            {QStringLiteral("briefTitle"), LocalePatterns::dateRange(locale, first, first.addDays(days - 1), true)},
            {QStringLiteral("first"), first.toString(Qt::ISODate)},
            {QStringLiteral("hours"), hours},
            {QStringLiteral("days"), dayList},
            {QStringLiteral("bars"), bars},
            {QStringLiteral("lanes"), layout.lanes}};
}

QString MailApp::moved(const mail::Calendar::SendResult &r)
{
    account()->notifyChanged();
    if (r.uid.isEmpty())
        Q_EMIT notify(tr("This event cannot be moved here. Open it to change it."));
    else if (!r.error.isEmpty())
        Q_EMIT notify(tr("Saved, but the invitation was not sent: %1").arg(r.error));
    else if (!r.queued.isEmpty())
        Q_EMIT notify(tr("Invitation sent."));
    return r.uid;
}

QString MailApp::rescheduleEvent(const QString &uid, const QString &day, int startMin, bool sendUpdate)
{
    const QDate d = QDate::fromString(day, Qt::ISODate);
    if (!account() || !d.isValid())
        return {};
    auto &ctx = account()->ctx();
    const auto e = mail::Calendar::get(ctx, uid);
    if (!e)
        return {};
    startMin = qBound(0, startMin, mail::CalendarLayout::kDayMinutes - 1);
    const QDateTime start = QDateTime(d, QTime(startMin / 60, startMin % 60), QTimeZone::systemTimeZone()).toUTC();
    return moved(mail::Calendar::reschedule(ctx, uid, start, start.addMSecs(e->durationMs()), sendUpdate));
}

QString MailApp::stretchEvent(const QString &uid, const QString &day, int endMin, bool sendUpdate)
{
    const QDate d = QDate::fromString(day, Qt::ISODate);
    if (!account() || !d.isValid())
        return {};
    auto &ctx = account()->ctx();
    const auto e = mail::Calendar::get(ctx, uid);
    if (!e)
        return {};
    endMin = qBound(0, endMin, mail::CalendarLayout::kDayMinutes);
    // Midnight is the start of the next day.
    const QDateTime end = QDateTime(d.addDays(endMin / mail::CalendarLayout::kDayMinutes),
                                    QTime((endMin % mail::CalendarLayout::kDayMinutes) / 60, endMin % 60),
                                    QTimeZone::systemTimeZone())
                              .toUTC();
    return moved(mail::Calendar::reschedule(ctx, uid, e->start, end, sendUpdate));
}

int MailApp::calendarReminderDefault() const
{
    return account() ? account()->ctx().config.getInt(cfg::CalendarReminder) : 15;
}

QString MailApp::suggestedFileName(qint64 msgId, int index) const
{
    if (!account())
        return {};
    const auto d = mail::Search::detail(account()->ctx(), msgId);
    if (!d || index < 0 || index >= d->attachments.size())
        return {};
    // Never let a sender choose a path.
    QString name = d->attachments[index].filename;
    name.replace(u'/', u'_').replace(u'\\', u'_');
    return name;
}

namespace {

QVariantList outlineOf(const QByteArray &raw)
{
    QVariantList out;
    for (const auto &n : mime::Source::outline(raw))
        out.append(QVariantMap{{QStringLiteral("depth"), n.depth},
                               {QStringLiteral("type"), QString::fromLatin1(n.mimeType)},
                               {QStringLiteral("encoding"), QString::fromLatin1(n.encoding)},
                               {QStringLiteral("filename"), n.filename},
                               {QStringLiteral("size"), QLocale().formattedDataSize(n.size)}});
    return out;
}

} // namespace

QVariantMap MailApp::messageSource(qint64 msgId) const
{
    if (!account())
        return {};
    MailContext &ctx = account()->ctx();
    const mail::Export::Source s = mail::Export::source(ctx, msgId);
    QVariantMap out{{QStringLiteral("retained"), s.retained},
                    {QStringLiteral("encrypted"), s.encrypted},
                    {QStringLiteral("subject"),
                     ctx.db.queryText("SELECT subject FROM messages WHERE id=?", msgId).value_or(QString())},
                    {QStringLiteral("fileName"), mail::Export::fileName(ctx, msgId, QStringLiteral("eml"))}};
    if (!s.retained)
        return out;
    out.insert(QStringLiteral("original"), QString::fromUtf8(s.raw));
    out.insert(QStringLiteral("outline"), outlineOf(s.raw));
    if (!s.inner.isEmpty()) {
        out.insert(QStringLiteral("decrypted"), QString::fromUtf8(s.inner));
        out.insert(QStringLiteral("innerOutline"), outlineOf(s.inner));
    }
    if (!s.decryptError.isEmpty())
        out.insert(QStringLiteral("decryptError"), s.decryptError);
    return out;
}

QString MailApp::shortenSource(const QString &source) const
{
    return mime::Source::shorten(source);
}

QVariantMap MailApp::exportInfo(qint64 msgId) const
{
    if (!account())
        return {};
    Statement st(account()->ctx().db, "SELECT raw_blob IS NOT NULL, encrypted FROM messages WHERE id=?");
    st.bindAll(msgId);
    if (!st.step())
        return {};
    return {{QStringLiteral("retained"), st.integer(0) != 0}, {QStringLiteral("encrypted"), st.integer(1) != 0}};
}

bool MailApp::saveMessage(qint64 msgId, const QUrl &dest, bool decrypted)
{
    if (!account())
        return false;
    MailContext &ctx = account()->ctx();
    const QByteArray data = decrypted ? mail::Export::decryptedEml(ctx, msgId) : mail::Export::eml(ctx, msgId);
    if (data.isEmpty()) {
        Q_EMIT notify(decrypted ? tr("This message cannot be decrypted, or its original is no longer kept.")
                                : tr("The original of this message is no longer kept."));
        return false;
    }
    const bool ok = FileIo::write(dest, data);
    Q_EMIT notify(ok ? tr("Saved %1.").arg(FileIo::displayName(dest))
                     : tr("Could not write %1.").arg(FileIo::displayName(dest)));
    return ok;
}

QString MailApp::suggestedEmlName(qint64 msgId) const
{
    return account() ? mail::Export::fileName(account()->ctx(), msgId, QStringLiteral("eml")) : QString();
}

void MailApp::openLink(const QString &url)
{
    const QString safe = mime::safeHref(url);
    if (!safe.isEmpty())
        QDesktopServices::openUrl(QUrl(safe));
}

QVariantList MailApp::contacts(const QString &filter) const
{
    QVariantList out;
    if (!account())
        return out;
    for (const ContactInfo &c : mail::Contacts::list(account()->ctx(), filter))
        out.append(QVariantMap{{QStringLiteral("id"), c.id},
                               {QStringLiteral("addr"), c.addr},
                               {QStringLiteral("name"), c.name},
                               {QStringLiteral("known"), c.isKnown()},
                               {QStringLiteral("hasKey"), !c.fingerprint.isEmpty()},
                               {QStringLiteral("verified"), c.verified},
                               {QStringLiteral("blocked"), c.blocked}});
    return out;
}

QVariantMap MailApp::contact(qint64 id) const
{
    if (!account())
        return {};
    const auto c = mail::Contacts::byId(account()->ctx(), id);
    if (!c)
        return {};
    QString fpr = c->fingerprint;
    for (int i = fpr.size() - 4; i > 0; i -= 4)
        fpr.insert(i, u' ');
    return {{QStringLiteral("id"), c->id},
            {QStringLiteral("addr"), c->addr},
            {QStringLiteral("name"), c->name},
            {QStringLiteral("known"), c->isKnown()},
            {QStringLiteral("trusted"), mail::Contacts::isTrusted(account()->ctx(), c->addr)},
            {QStringLiteral("trustSet"), c->trust ? (*c->trust ? QStringLiteral("on") : QStringLiteral("off"))
                                                  : QStringLiteral("auto")},
            {QStringLiteral("fingerprint"), fpr},
            {QStringLiteral("verified"), c->verified},
            {QStringLiteral("joining"), secureJoinState(c->addr)},
            {QStringLiteral("preferEncrypt"), c->preferEncrypt},
            {QStringLiteral("blocked"), c->blocked},
            {QStringLiteral("encryption"), c->encryptionOverride ? int(*c->encryptionOverride) : -1},
            {QStringLiteral("organization"), c->organization},
            {QStringLiteral("title"), c->title},
            {QStringLiteral("notes"), c->notes},
            {QStringLiteral("birthday"), c->birthday},
            {QStringLiteral("phones"), [&c] {
                 QVariantList out;
                 for (const ContactPhone &p : c->phones)
                     out.append(QVariantMap{{QStringLiteral("label"), p.label}, {QStringLiteral("number"), p.number}});
                 return out;
             }()}};
}

QString MailApp::secureJoinInvite()
{
    return account() ? mail::SecureJoin::invite(account()->ctx()) : QString();
}

QString MailApp::renewSecureJoinInvite()
{
    return account() ? mail::SecureJoin::renewInvite(account()->ctx()) : QString();
}

bool MailApp::isSecureJoinInvite(const QString &text) const
{
    return mail::SecureJoin::parse(text).has_value();
}

QString MailApp::secureJoinAddress(const QString &text) const
{
    const auto inv = mail::SecureJoin::parse(text);
    return inv ? inv->addr : QString();
}

QString MailApp::secureJoin(const QString &text)
{
    if (!account())
        return {};
    const auto inv = mail::SecureJoin::parse(text);
    if (!inv)
        return tr("This is not a contact's code. A code to verify someone starts with OPENPGP4FPR: or https://i.delta.chat/.");
    try {
        mail::SecureJoin::join(account()->ctx(), *inv);
    } catch (const mail::ComposeError &e) {
        return e.message();
    }
    account()->notifyChanged();
    account()->syncNow();
    return {};
}

QString MailApp::lifetime(int seconds) const
{
    constexpr int kHour = 3600, kDay = 24 * kHour, kWeek = 7 * kDay;
    if (seconds >= kWeek && seconds % kWeek == 0)
        return tr("%n week(s)", nullptr, seconds / kWeek);
    if (seconds >= kDay && seconds % kDay == 0)
        return tr("%n day(s)", nullptr, seconds / kDay);
    if (seconds >= kHour && seconds % kHour == 0)
        return tr("%n hour(s)", nullptr, seconds / kHour);
    return tr("%n minute(s)", nullptr, qMax(1, seconds / 60));
}

QString MailApp::setPassphrase(const QString &current, const QString &next)
{
    if (AtRest::hasPassphrase() && !AtRest::checkPassphrase(m_mgr->root(), current))
        return tr("The current passphrase is not right.");
    AtRest::setPassphrase(next);
    for (Account *a : m_mgr->accounts())
        AtRest::reguard(a->dir(), a->id(), a->dbKey());
    Q_EMIT accountChanged();
    return {};
}

bool MailApp::hasPassphrase() const
{
    return AtRest::hasPassphrase();
}

void MailApp::setContactTrust(qint64 id, const QString &trust)
{
    if (!account())
        return;
    const auto c = mail::Contacts::byId(account()->ctx(), id);
    if (!c)
        return;
    mail::Contacts::setTrust(account()->ctx(), c->addr,
                             trust == QLatin1String("auto") ? std::nullopt
                                                            : std::optional<bool>(trust == QLatin1String("on")));
    account()->notifyChanged();
}

QString MailApp::secureJoinState(const QString &addr) const
{
    if (!account())
        return QStringLiteral("none");
    const auto j = mail::SecureJoin::joinFor(account()->ctx(), addr);
    if (!j)
        return QStringLiteral("none");
    switch (j->state) {
    case mail::SecureJoin::JoinState::Requested:
    case mail::SecureJoin::JoinState::AuthSent:
        return QStringLiteral("waiting");
    case mail::SecureJoin::JoinState::Done:
        return QStringLiteral("done");
    case mail::SecureJoin::JoinState::Failed:
        return QStringLiteral("failed");
    case mail::SecureJoin::JoinState::None:
        break;
    }
    return QStringLiteral("none");
}

qint64 MailApp::createContact(const QString &addr, const QString &name)
{
    if (!account())
        return 0;
    const auto id = mail::Contacts::create(account()->ctx(), addr, name);
    if (!id) {
        Q_EMIT notify(tr("%1 is not an email address, or is in your contacts already.").arg(addr.trimmed()));
        return 0;
    }
    account()->notifyChanged();
    return *id;
}

void MailApp::setContactDetails(qint64 id, const QVariantMap &details)
{
    ContactInfo d;
    d.name = details.value(QStringLiteral("name")).toString();
    d.organization = details.value(QStringLiteral("organization")).toString();
    d.title = details.value(QStringLiteral("title")).toString();
    d.notes = details.value(QStringLiteral("notes")).toString();
    d.birthday = details.value(QStringLiteral("birthday")).toString();
    for (const QVariant &v : details.value(QStringLiteral("phones")).toList()) {
        const QVariantMap m = v.toMap();
        d.phones.append({m.value(QStringLiteral("label")).toString(), m.value(QStringLiteral("number")).toString()});
    }
    WITH_ACCOUNT(mail::Contacts::setDetails(ctx, id, d));
}

void MailApp::removeContact(qint64 id)
{
    WITH_ACCOUNT(mail::Contacts::remove(ctx, id));
}

void MailApp::importContacts(const QUrl &file)
{
    if (!account())
        return;
    const auto data = FileIo::read(file);
    if (!data) {
        Q_EMIT notify(tr("Could not read %1.").arg(FileIo::displayName(file)));
        return;
    }
    const auto r = mail::Contacts::import(account()->ctx(), mail::VCard::parse(*data));
    account()->notifyChanged();
    QString msg = tr("%n contact(s) added", "", r.added) + QStringLiteral(", ") + tr("%n updated", "", r.updated);
    if (r.skipped)
        msg += QStringLiteral(", ") + tr("%n without an email address skipped", "", r.skipped);
    Q_EMIT notify(msg + u'.');
}

void MailApp::exportContacts(const QUrl &file, const QString &format)
{
    if (!account())
        return;
    const QList<ContactInfo> book = mail::Export::addressBook(account()->ctx());
    const bool ok = FileIo::write(file, format == QLatin1String("csv") ? mail::ContactsCsv::emit(book)
                                                                        : mail::VCard::emit(book));
    Q_EMIT notify(ok ? tr("%n contact(s) exported.", "", int(book.size()))
                     : tr("Could not write %1.").arg(FileIo::displayName(file)));
}

void MailApp::exportContact(qint64 id, const QUrl &file)
{
    if (!account())
        return;
    const auto c = mail::Contacts::byId(account()->ctx(), id);
    if (!c)
        return;
    const bool ok = FileIo::write(file, mail::VCard::emit({*c}));
    Q_EMIT notify(ok ? tr("Saved %1.").arg(FileIo::displayName(file))
                     : tr("Could not write %1.").arg(FileIo::displayName(file)));
}

QString MailApp::suggestedContactFileName(qint64 id) const
{
    if (!account())
        return {};
    const auto c = mail::Contacts::byId(account()->ctx(), id);
    if (!c)
        return {};
    return mail::Export::safeFileName(c->name.isEmpty() ? c->addr : c->name, QStringLiteral("contact"),
                                      QStringLiteral("vcf"));
}

void MailApp::setContactName(qint64 id, const QString &name)
{
    WITH_ACCOUNT(mail::Contacts::setName(ctx, id, name));
}

void MailApp::setContactEncryption(qint64 id, int mode)
{
    WITH_ACCOUNT(mail::Contacts::setEncryptionOverride(
        ctx, id, mode < 0 ? std::nullopt : std::optional<EncryptionMode>(EncryptionMode(qBound(0, mode, 2)))));
}

QStringList MailApp::completeAddress(const QString &prefix) const
{
    QStringList out;
    if (!account() || prefix.trimmed().size() < 2)
        return out;
    for (const ContactInfo &c : mail::Contacts::list(account()->ctx(), prefix.trimmed(), 8))
        out.append(mime::Address{c.name, c.addr}.display());
    return out;
}

QVariantMap MailApp::settings() const
{
    if (!account())
        return {};
    const Config &c = account()->ctx().config;
    const Account::Settings s = account()->settings();
    return {{QStringLiteral("addr"), s.addr},
            {QStringLiteral("displayName"), s.displayName},
            {QStringLiteral("protocol"), s.protocol},
            {QStringLiteral("inHost"), s.inHost},
            {QStringLiteral("inPort"), s.inPort},
            {QStringLiteral("inSecurity"), s.inSecurity},
            {QStringLiteral("smtpHost"), s.smtpHost},
            {QStringLiteral("smtpPort"), s.smtpPort},
            {QStringLiteral("smtpSecurity"), s.smtpSecurity},
            {QStringLiteral("secretLocation"), account()->secretLocation()},
            {QStringLiteral("atRest"), [this] {
                 switch (AtRest::guard(account()->dir())) {
                 case AtRest::Guard::Keyring: return QStringLiteral("keyring");
                 case AtRest::Guard::Passphrase: return QStringLiteral("passphrase");
                 case AtRest::Guard::None: break;
                 }
                 return QStringLiteral("none");
             }()},
            {QStringLiteral("inCert"), net::formatFingerprint(c.get(cfg::InCertSha256).toLatin1())},
            {QStringLiteral("smtpCert"), net::formatFingerprint(c.get(cfg::SmtpCertSha256).toLatin1())},
            {QStringLiteral("encryptionMode"), c.getInt(cfg::EncryptionMode)},
            {QStringLiteral("serverRetention"), c.get(cfg::ServerRetention)},
            {QStringLiteral("serverKeepDays"), c.getInt(cfg::ServerKeepDays)},
            {QStringLiteral("rawMimeDays"), c.getInt(cfg::RawMimeDays)},
            {QStringLiteral("trashPurgeDays"), c.getInt(cfg::TrashPurgeDays)},
            {QStringLiteral("unverifiedTrashDays"), c.getInt(cfg::UnverifiedTrashDays)},
            {QStringLiteral("gating"), c.getBool(cfg::Gating)},
            {QStringLiteral("mdnSend"), c.get(cfg::MdnSend)},
            {QStringLiteral("calendarReminder"), c.getInt(cfg::CalendarReminder)},
            {QStringLiteral("pollSeconds"), c.getInt(cfg::PollSeconds)},
            {QStringLiteral("fingerprint"), [&] {
                 QString f = c.get(cfg::SelfFingerprint);
                 for (int i = f.size() - 4; i > 0; i -= 4)
                     f.insert(i, u' ');
                 return f;
             }()},
            {QStringLiteral("dataDir"), Paths::dataDir()}};
}

QString MailApp::language() const
{
    return Languages::saved();
}

void MailApp::setLanguage(const QString &code)
{
    if (code == Languages::saved())
        return;
    Languages::save(code);
    Languages::install(code);
    Q_EMIT languageChanged();
    // Dates in the list are formatted here, in the new locale.
    reloadMessages();
    reloadThread();
}

QVariantList MailApp::languages() const
{
    QVariantList out;
    for (const Languages::Entry &e : Languages::available())
        out.append(QVariantMap{{QStringLiteral("code"), e.code}, {QStringLiteral("name"), e.name}});
    return out;
}

void MailApp::setSetting(const QString &key, const QVariant &value)
{
    // Only these are writable from the UI; server settings go through setup.
    static const QHash<QString, const char *> keys = {
        {QStringLiteral("displayName"), cfg::DisplayName},
        {QStringLiteral("encryptionMode"), cfg::EncryptionMode},
        {QStringLiteral("serverRetention"), cfg::ServerRetention},
        {QStringLiteral("serverKeepDays"), cfg::ServerKeepDays},
        {QStringLiteral("rawMimeDays"), cfg::RawMimeDays},
        {QStringLiteral("trashPurgeDays"), cfg::TrashPurgeDays},
        {QStringLiteral("unverifiedTrashDays"), cfg::UnverifiedTrashDays},
        {QStringLiteral("gating"), cfg::Gating},
        {QStringLiteral("mdnSend"), cfg::MdnSend},
        {QStringLiteral("calendarReminder"), cfg::CalendarReminder},
        {QStringLiteral("pollSeconds"), cfg::PollSeconds},
    };
    const char *k = keys.value(key);
    if (!k || !account())
        return;
    const QString v = value.typeId() == QMetaType::Bool ? QString::number(value.toBool() ? 1 : 0) : value.toString();
    mail::Preferences::set(account()->ctx(), k, v);
    if (key == QLatin1String("displayName"))
        Q_EMIT accountChanged();
    account()->notifyChanged();
}

void MailApp::reloadSignatures()
{
    QVariantList out;
    if (account()) {
        auto &ctx = account()->ctx();
        const QString def = mail::Signatures::defaultId(ctx);
        for (const mail::Signature &s : mail::Signatures::list(ctx)) {
            const QString first = s.text.trimmed().section(u'\n', 0, 0).trimmed();
            out.append(QVariantMap{{QStringLiteral("id"), s.id},
                                   {QStringLiteral("name"), s.name},
                                   {QStringLiteral("preview"), first},
                                   {QStringLiteral("isDefault"), s.id == def}});
        }
    }
    if (out != m_signatures) {
        m_signatures = out;
        Q_EMIT signaturesChanged();
    }
}

QString MailApp::defaultSignature() const
{
    for (const QVariant &v : m_signatures) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("isDefault")).toBool())
            return m.value(QStringLiteral("id")).toString();
    }
    return {};
}

void MailApp::setDefaultSignature(const QString &id)
{
    if (!account() || id == defaultSignature())
        return;
    mail::Signatures::setDefault(account()->ctx(), id);
    reloadSignatures();
}

void MailApp::loadSignature(const QString &id, QQuickTextDocument *doc) const
{
    if (!doc)
        return;
    QTextDocument *td = doc->textDocument();
    td->clear();
    const auto s = account() ? mail::Signatures::get(account()->ctx(), id) : std::nullopt;
    if (!s)
        return;
    if (s->html.isEmpty())
        td->setPlainText(s->text);
    else
        RichText::load(td, s->html);
}

QString MailApp::saveSignature(const QString &id, const QString &name, QQuickTextDocument *doc)
{
    if (!account() || !doc)
        return {};
    // The composer's whitelist decides what a signature may hold, so it is
    // exactly what can be sent.
    const RichText::Output out = RichText::emit(doc->textDocument());
    if (out.text.trimmed().isEmpty() && out.html.isEmpty()) {
        removeSignature(id);
        return {};
    }
    const QString saved = mail::Signatures::save(account()->ctx(), {id, name, out.html, out.text});
    reloadSignatures();
    return saved;
}

void MailApp::removeSignature(const QString &id)
{
    if (!account() || id.isEmpty())
        return;
    mail::Signatures::remove(account()->ctx(), id);
    reloadSignatures();
}

QString MailApp::signatureName(const QString &id) const
{
    const auto s = account() ? mail::Signatures::get(account()->ctx(), id) : std::nullopt;
    return s ? s->name : QString();
}
