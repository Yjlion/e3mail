// SPDX-License-Identifier: MPL-2.0
#include "MailApp.h"
#include "Languages.h"
#include "InlineImages.h"
#include "NotifierBackends.h"

#include "crypto/Pgp.h"
#include "engine/AccountManager.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "mail/VCard.h"
#include "mime/Html.h"
#include "mime/Part.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "util/Paths.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QSettings>
#include <QJSEngine>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

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
    connect(m_mgr, &AccountManager::accountsChanged, this, &MailApp::accountsChanged);
    connect(m_mgr, &AccountManager::selectedChanged, this, &MailApp::switchAccount);
    for (Account *a : m_mgr->accounts()) {
        connect(a, &Account::statusChanged, this, &MailApp::accountsChanged);
    }

    m_notifier = new Notifier(makeSystemNotifierBackend(), this);
    m_notifier->setMode(Notifier::modeFromString(appSettings().value(QStringLiteral("notifications")).toString()));
    m_notifier->setWindowActive([] { return QGuiApplication::applicationState() == Qt::ApplicationActive; });
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
        connect(m_connected, &Account::newMail, this, [this](qint64 id) {
            if (!account())
                return;
            if (const auto d = mail::Search::detail(account()->ctx(), id))
                Q_EMIT newMailArrived(d->from.name.isEmpty() ? d->from.addr : d->from.name, d->subject);
        });
    }
    Q_EMIT accountChanged();
    Q_EMIT statusChanged();
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
    };
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
    QSaveFile f(dest.toLocalFile());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(account()->ctx().blobs.get(d->attachments[index].blob));
    const bool ok = f.commit();
    Q_EMIT notify(ok ? tr("Saved %1.").arg(d->attachments[index].filename) : tr("Could not save the attachment."));
    return ok;
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

QString MailApp::viewSource(qint64 msgId) const
{
    if (!account())
        return {};
    MailContext &ctx = account()->ctx();
    const auto blob = ctx.db.queryText("SELECT raw_blob FROM messages WHERE id=?", msgId);
    if (!blob)
        return tr("The original of this message is no longer kept. Change how long originals are kept in Settings.");
    const QByteArray raw = ctx.blobs.get(*blob);
    QString out = QString::fromUtf8(raw);
    const mime::Part p = mime::parse(raw);
    if (p.mimeType == "multipart/encrypted" && p.children.size() >= 2) {
        try {
            out += tr("\n\n──── decrypted content ────\n\n")
                + QString::fromUtf8(ctx.pgp.decrypt(p.children[1].body).data);
        } catch (const std::exception &e) {
            out += tr("\n\n(cannot decrypt: %1)").arg(QString::fromUtf8(e.what()));
        }
    }
    return out;
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
            {QStringLiteral("fingerprint"), fpr},
            {QStringLiteral("verified"), c->verified},
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
    QFile f(file.toLocalFile());
    if (!f.open(QIODevice::ReadOnly)) {
        Q_EMIT notify(tr("Could not read %1.").arg(file.fileName()));
        return;
    }
    const auto r = mail::Contacts::import(account()->ctx(), mail::VCard::parse(f.readAll()));
    account()->notifyChanged();
    QString msg = tr("%n contact(s) added", "", r.added) + QStringLiteral(", ") + tr("%n updated", "", r.updated);
    if (r.skipped)
        msg += QStringLiteral(", ") + tr("%n without an email address skipped", "", r.skipped);
    Q_EMIT notify(msg + u'.');
}

void MailApp::exportContacts(const QUrl &file)
{
    if (!account())
        return;
    // The address book: people added, accepted or written to, not everyone
    // ever seen on a message.
    QList<ContactInfo> book;
    for (const ContactInfo &c : mail::Contacts::list(account()->ctx(), QString(), 1000000)) {
        if (c.isKnown())
            book.append(c);
    }
    QSaveFile f(file.toLocalFile());
    const bool ok = f.open(QIODevice::WriteOnly) && f.write(mail::VCard::emit(book)) >= 0 && f.commit();
    Q_EMIT notify(ok ? tr("%n contact(s) exported.", "", int(book.size())) : tr("Could not write %1.").arg(file.fileName()));
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
            {QStringLiteral("signature"), c.get(cfg::Signature)},
            {QStringLiteral("protocol"), s.protocol},
            {QStringLiteral("inHost"), s.inHost},
            {QStringLiteral("inPort"), s.inPort},
            {QStringLiteral("inSecurity"), s.inSecurity},
            {QStringLiteral("smtpHost"), s.smtpHost},
            {QStringLiteral("smtpPort"), s.smtpPort},
            {QStringLiteral("smtpSecurity"), s.smtpSecurity},
            {QStringLiteral("secretLocation"), account()->secretLocation()},
            {QStringLiteral("encryptionMode"), c.getInt(cfg::EncryptionMode)},
            {QStringLiteral("serverRetention"), c.get(cfg::ServerRetention)},
            {QStringLiteral("serverKeepDays"), c.getInt(cfg::ServerKeepDays)},
            {QStringLiteral("rawMimeDays"), c.getInt(cfg::RawMimeDays)},
            {QStringLiteral("trashPurgeDays"), c.getInt(cfg::TrashPurgeDays)},
            {QStringLiteral("unverifiedTrashDays"), c.getInt(cfg::UnverifiedTrashDays)},
            {QStringLiteral("gating"), c.getBool(cfg::Gating)},
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
        {QStringLiteral("signature"), cfg::Signature},
        {QStringLiteral("encryptionMode"), cfg::EncryptionMode},
        {QStringLiteral("serverRetention"), cfg::ServerRetention},
        {QStringLiteral("serverKeepDays"), cfg::ServerKeepDays},
        {QStringLiteral("rawMimeDays"), cfg::RawMimeDays},
        {QStringLiteral("trashPurgeDays"), cfg::TrashPurgeDays},
        {QStringLiteral("unverifiedTrashDays"), cfg::UnverifiedTrashDays},
        {QStringLiteral("gating"), cfg::Gating},
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

QString MailApp::signature() const
{
    return account() ? account()->ctx().config.get(cfg::Signature) : QString();
}
