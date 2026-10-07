// SPDX-License-Identifier: MPL-2.0
//
// e3mail-cli: a headless driver for the same engine the app uses. One command
// per invocation, JSON on stdout, non-zero exit on error. `sync` runs one
// send-and-fetch pass and returns, so scripts and tests can step accounts
// deterministically.

#include "engine/AccountManager.h"
#include "engine/Autoconfig.h"
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "util/Paths.h"

#include <QCommandLineParser>
#include <QEventLoop>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>

#include <cstdio>

using namespace e3;

namespace {

void print(const QJsonValue &v)
{
    const QByteArray out = v.isObject() ? QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented)
                                        : QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented);
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
}

[[noreturn]] void fail(const QString &msg)
{
    std::fprintf(stderr, "e3mail-cli: %s\n", qPrintable(msg));
    std::exit(1);
}

QJsonArray addrs(const QList<mime::Address> &list)
{
    QJsonArray a;
    for (const auto &x : list)
        a.append(QJsonObject{{QStringLiteral("name"), x.name}, {QStringLiteral("addr"), x.addr}});
    return a;
}

QList<mime::Address> parseList(const QString &s)
{
    return s.isEmpty() ? QList<mime::Address>() : mime::parseAddressList(s.toUtf8());
}

QJsonObject summaryJson(const MessageSummary &m)
{
    return {{QStringLiteral("id"), m.id},
            {QStringLiteral("thread"), m.threadId},
            {QStringLiteral("from"), m.from.display()},
            {QStringLiteral("subject"), m.subject},
            {QStringLiteral("date"), m.date.toUTC().toString(Qt::ISODate)},
            {QStringLiteral("unread"), m.unread},
            {QStringLiteral("encrypted"), m.encrypted},
            {QStringLiteral("signed"), m.signedBySender},
            {QStringLiteral("verified"), m.verified},
            {QStringLiteral("importance"), m.importance},
            {QStringLiteral("attachments"), m.hasAttachments}};
}

// Asks every source, as the setup form does.
Autoconfig::Result discover(const QString &addr)
{
    Autoconfig ac;
    Autoconfig::Result result;
    QEventLoop loop;
    QObject::connect(&ac, &Autoconfig::finished, &loop, [&](const Autoconfig::Result &r) {
        result = r;
        loop.quit();
    });
    ac.lookup(addr);
    loop.exec();
    return result;
}

QJsonObject serverJson(const Autoconfig::Server &s)
{
    return {{QStringLiteral("protocol"), s.protocol},
            {QStringLiteral("host"), s.host},
            {QStringLiteral("port"), s.port},
            {QStringLiteral("security"), net::securityToString(s.security)},
            {QStringLiteral("username"), s.username}};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("e3mail"));
    QCoreApplication::setApplicationVersion(QStringLiteral(E3MAIL_VERSION));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral(
        "Headless e3mail.\n\nCommands:\n"
        "  accounts                       list accounts\n"
        "  discover                       find server settings for --addr\n"
        "  add                            add an account (--addr, --password, server options)\n"
        "  sync                           send the outbox and fetch once\n"
        "  list                           list messages (--tag, --search)\n"
        "  show <id>                      show a message (--raw for the original)\n"
        "  send                           queue a message (--to, --subject, --body ...); run sync to deliver\n"
        "  accept <addr>                  accept a sender held in Unverified\n"
        "  block <pattern> | unblock <pattern>\n"
        "  trash <id> | restore <id> | archive <id>\n"
        "  contacts                       list contacts\n"
        "  config <key> [value]           read or write an account setting\n"
        "  housekeeping                   run deadlines now"));
    p.addHelpOption();
    p.addVersionOption();
    const QList<QCommandLineOption> opts = {
        {QStringLiteral("data-dir"), QStringLiteral("Data directory."), QStringLiteral("dir")},
        {QStringLiteral("account"), QStringLiteral("Account id (default: selected)."), QStringLiteral("id")},
        {QStringLiteral("addr"), QStringLiteral("Email address."), QStringLiteral("addr")},
        {QStringLiteral("name"), QStringLiteral("Display name."), QStringLiteral("name")},
        {QStringLiteral("password"), QStringLiteral("Password."), QStringLiteral("password")},
        {QStringLiteral("protocol"), QStringLiteral("imap or pop3."), QStringLiteral("protocol"), QStringLiteral("imap")},
        {QStringLiteral("in-host"), QStringLiteral("Incoming server."), QStringLiteral("host")},
        {QStringLiteral("in-port"), QStringLiteral("Incoming port."), QStringLiteral("port")},
        {QStringLiteral("in-security"), QStringLiteral("ssl, starttls or plain."), QStringLiteral("mode")},
        {QStringLiteral("smtp-host"), QStringLiteral("SMTP server."), QStringLiteral("host")},
        {QStringLiteral("smtp-port"), QStringLiteral("SMTP port."), QStringLiteral("port")},
        {QStringLiteral("smtp-security"), QStringLiteral("ssl, starttls or plain."), QStringLiteral("mode")},
        {QStringLiteral("insecure"), QStringLiteral("Accept invalid TLS certificates (test servers only).")},
        {QStringLiteral("tag"), QStringLiteral("inbox, unverified, sent, drafts, archive, trash, all."), QStringLiteral("tag"), QStringLiteral("inbox")},
        {QStringLiteral("search"), QStringLiteral("Full-text search."), QStringLiteral("text")},
        {QStringLiteral("raw"), QStringLiteral("Show the original message.")},
        {QStringLiteral("to"), QStringLiteral("Recipients."), QStringLiteral("list")},
        {QStringLiteral("cc"), QStringLiteral("Cc recipients."), QStringLiteral("list")},
        {QStringLiteral("bcc"), QStringLiteral("Bcc recipients."), QStringLiteral("list")},
        {QStringLiteral("subject"), QStringLiteral("Subject."), QStringLiteral("text")},
        {QStringLiteral("body"), QStringLiteral("Body text."), QStringLiteral("text")},
        {QStringLiteral("attach"), QStringLiteral("Attach a file (repeatable)."), QStringLiteral("file")},
        {QStringLiteral("encrypt"), QStringLiteral("auto, required or plaintext."), QStringLiteral("mode"), QStringLiteral("auto")},
        {QStringLiteral("important"), QStringLiteral("Mark as important.")},
        {QStringLiteral("reply-to"), QStringLiteral("Message id this replies to."), QStringLiteral("id")},
    };
    p.addOptions(opts);
    p.addPositionalArgument(QStringLiteral("command"), QStringLiteral("What to do."));
    p.process(app);

    const QStringList args = p.positionalArguments();
    if (args.isEmpty())
        p.showHelp(1);
    const QString cmd = args.first();
    if (p.isSet(QStringLiteral("data-dir")))
        Paths::setDataDir(p.value(QStringLiteral("data-dir")));

    try {
        if (cmd == QLatin1String("discover")) {
            const QString addr = p.value(QStringLiteral("addr"));
            if (!addr.contains(u'@'))
                fail(QStringLiteral("discover needs --addr"));
            const Autoconfig::Result r = discover(addr);
            QJsonArray incoming;
            for (const Autoconfig::Server &x : r.incoming)
                incoming.append(serverJson(x));
            print(QJsonObject{{QStringLiteral("source"), r.source},
                              {QStringLiteral("needs_review"), r.needsReview},
                              {QStringLiteral("incoming"), incoming},
                              {QStringLiteral("smtp"), serverJson(r.smtp)}});
            return 0;
        }
        AccountManager mgr(Paths::dataDir());
        if (cmd == QLatin1String("accounts")) {
            QJsonArray a;
            for (Account *acc : mgr.accounts())
                a.append(QJsonObject{{QStringLiteral("id"), acc->id()},
                                     {QStringLiteral("addr"), acc->addr()},
                                     {QStringLiteral("protocol"), acc->settings().protocol},
                                     {QStringLiteral("selected"), acc->id() == mgr.selectedId()}});
            print(a);
            return 0;
        }
        if (cmd == QLatin1String("add")) {
            const QString addr = p.value(QStringLiteral("addr"));
            if (!addr.contains(u'@') || !p.isSet(QStringLiteral("password")))
                fail(QStringLiteral("add needs --addr and --password"));
            const bool given = p.isSet(QStringLiteral("in-host")) && p.isSet(QStringLiteral("smtp-host"));
            const Autoconfig::Result guess = given ? Autoconfig::guess(addr) : discover(addr);
            const QString protocol = p.value(QStringLiteral("protocol"));
            Autoconfig::Server in = guess.incoming.value(0);
            for (const Autoconfig::Server &x : guess.incoming) {
                if (x.protocol == protocol) {
                    in = x;
                    break;
                }
            }
            Account::Settings s;
            s.addr = addr;
            s.displayName = p.value(QStringLiteral("name"));
            s.protocol = protocol;
            s.inHost = p.isSet(QStringLiteral("in-host")) ? p.value(QStringLiteral("in-host")) : in.host;
            s.inPort = quint16(p.isSet(QStringLiteral("in-port")) ? p.value(QStringLiteral("in-port")).toUInt() : in.port);
            s.inSecurity = p.isSet(QStringLiteral("in-security")) ? p.value(QStringLiteral("in-security"))
                                                                  : net::securityToString(in.security);
            s.smtpHost = p.isSet(QStringLiteral("smtp-host")) ? p.value(QStringLiteral("smtp-host")) : guess.smtp.host;
            s.smtpPort = quint16(p.isSet(QStringLiteral("smtp-port")) ? p.value(QStringLiteral("smtp-port")).toUInt()
                                                                      : guess.smtp.port);
            s.smtpSecurity = p.isSet(QStringLiteral("smtp-security")) ? p.value(QStringLiteral("smtp-security"))
                                                                      : net::securityToString(guess.smtp.security);
            if (!p.isSet(QStringLiteral("in-host")))
                s.inUser = in.username;
            if (!p.isSet(QStringLiteral("smtp-host")))
                s.smtpUser = guess.smtp.username;
            s.inPassword = p.value(QStringLiteral("password"));
            s.acceptInvalidCertificates = p.isSet(QStringLiteral("insecure"));
            Account *acc = mgr.create();
            acc->configure(s);
            print(QJsonObject{{QStringLiteral("id"), acc->id()}, {QStringLiteral("addr"), acc->addr()},
                              {QStringLiteral("password_stored_in"), acc->secretLocation()}});
            return 0;
        }

        Account *acc = p.isSet(QStringLiteral("account")) ? mgr.account(p.value(QStringLiteral("account")).toInt())
                                                          : mgr.selected();
        if (!acc)
            fail(QStringLiteral("no such account; add one first"));
        MailContext &ctx = acc->ctx();
        const qint64 idArg = args.value(1).toLongLong();

        if (cmd == QLatin1String("sync")) {
            MailWorker::runOnce(acc->dir(), acc->credentials());
            print(QJsonObject{{QStringLiteral("inbox"), mail::Search::count(ctx, tag::Inbox, false)},
                              {QStringLiteral("unread"), mail::Search::count(ctx, tag::Inbox, true)},
                              {QStringLiteral("unverified"), mail::Search::count(ctx, tag::Unverified, false)}});
        } else if (cmd == QLatin1String("list")) {
            SearchQuery q = SearchQuery::forTag(p.value(QStringLiteral("tag")));
            q.text = p.value(QStringLiteral("search"));
            QJsonArray a;
            for (const MessageSummary &m : mail::Search::list(ctx, q))
                a.append(summaryJson(m));
            print(a);
        } else if (cmd == QLatin1String("show")) {
            const auto d = mail::Search::detail(ctx, idArg);
            if (!d)
                fail(QStringLiteral("no message %1").arg(idArg));
            if (p.isSet(QStringLiteral("raw"))) {
                const auto blob = ctx.db.queryText("SELECT raw_blob FROM messages WHERE id=?", idArg);
                if (!blob)
                    fail(QStringLiteral("the original of message %1 is not retained").arg(idArg));
                const QByteArray raw = ctx.blobs.get(*blob);
                std::fwrite(raw.constData(), 1, size_t(raw.size()), stdout);
                return 0;
            }
            QJsonObject o = summaryJson(*d);
            o.insert(QStringLiteral("message_id"), d->messageId);
            o.insert(QStringLiteral("to"), addrs(d->to));
            o.insert(QStringLiteral("cc"), addrs(d->cc));
            o.insert(QStringLiteral("bcc"), addrs(d->bcc));
            o.insert(QStringLiteral("body"), d->bodyText);
            o.insert(QStringLiteral("held"), d->held);
            o.insert(QStringLiteral("trashed"), d->trashed);
            o.insert(QStringLiteral("undelivered"), QJsonArray::fromStringList(d->undelivered));
            QJsonArray att;
            for (const auto &a : d->attachments)
                att.append(QJsonObject{{QStringLiteral("name"), a.filename}, {QStringLiteral("size"), a.size},
                                       {QStringLiteral("type"), a.mimeType}});
            o.insert(QStringLiteral("attachments"), att);
            print(o);
        } else if (cmd == QLatin1String("send")) {
            Draft d;
            if (p.isSet(QStringLiteral("reply-to")))
                d = mail::Compose::reply(ctx, p.value(QStringLiteral("reply-to")).toLongLong(), false);
            if (p.isSet(QStringLiteral("to")))
                d.to = parseList(p.value(QStringLiteral("to")));
            d.cc = parseList(p.value(QStringLiteral("cc")));
            d.bcc = parseList(p.value(QStringLiteral("bcc")));
            if (p.isSet(QStringLiteral("subject")))
                d.subject = p.value(QStringLiteral("subject"));
            d.text = p.value(QStringLiteral("body")) + d.text;
            d.importance = p.isSet(QStringLiteral("important")) ? 1 : 0;
            const QString enc = p.value(QStringLiteral("encrypt"));
            d.encryption = enc == QLatin1String("required") ? SendEncryption::Required
                : enc == QLatin1String("plaintext")         ? SendEncryption::Plaintext
                                                            : SendEncryption::Auto;
            for (const QString &path : p.values(QStringLiteral("attach"))) {
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly))
                    fail(QStringLiteral("cannot read %1").arg(path));
                d.attachments.append({QFileInfo(path).fileName(), QMimeDatabase().mimeTypeForFile(path).name(), f.readAll()});
            }
            const qint64 id = mail::Compose::queue(ctx, d);
            print(QJsonObject{{QStringLiteral("id"), id},
                              {QStringLiteral("encrypted"), mail::Search::detail(ctx, id)->encrypted}});
        } else if (cmd == QLatin1String("accept")) {
            print(QJsonObject{{QStringLiteral("released"), mail::Organize::accept(ctx, args.value(1))}});
        } else if (cmd == QLatin1String("block")) {
            mail::Contacts::block(ctx, args.value(1));
            print(QJsonArray::fromStringList(mail::Contacts::blocklist(ctx)));
        } else if (cmd == QLatin1String("unblock")) {
            mail::Contacts::unblock(ctx, args.value(1));
            print(QJsonArray::fromStringList(mail::Contacts::blocklist(ctx)));
        } else if (cmd == QLatin1String("trash")) {
            mail::Organize::trash(ctx, idArg, TrashReason::User);
            print(QJsonObject{{QStringLiteral("trashed"), idArg}});
        } else if (cmd == QLatin1String("restore")) {
            mail::Organize::restore(ctx, idArg);
            print(QJsonObject{{QStringLiteral("restored"), idArg}});
        } else if (cmd == QLatin1String("archive")) {
            mail::Organize::archive(ctx, idArg, true);
            print(QJsonObject{{QStringLiteral("archived"), idArg}});
        } else if (cmd == QLatin1String("contacts")) {
            QJsonArray a;
            for (const ContactInfo &c : mail::Contacts::list(ctx))
                a.append(QJsonObject{{QStringLiteral("addr"), c.addr},
                                     {QStringLiteral("name"), c.name},
                                     {QStringLiteral("known"), c.isKnown()},
                                     {QStringLiteral("key"), c.fingerprint},
                                     {QStringLiteral("verified"), c.verified},
                                     {QStringLiteral("blocked"), c.blocked}});
            print(a);
        } else if (cmd == QLatin1String("config")) {
            const QByteArray key = args.value(1).toUtf8();
            if (key.isEmpty())
                fail(QStringLiteral("config needs a key"));
            if (key.contains("password"))
                fail(QStringLiteral("passwords are not shown or set here"));
            if (args.size() > 2)
                mail::Preferences::set(ctx, key.constData(), args.value(2));
            print(QJsonObject{{QString::fromUtf8(key), ctx.config.get(key.constData())}});
        } else if (cmd == QLatin1String("housekeeping")) {
            const auto r = mail::Organize::housekeeping(ctx);
            print(QJsonObject{{QStringLiteral("purged"), r.purged}, {QStringLiteral("swept"), r.swept},
                              {QStringLiteral("raw_expired"), r.rawExpired}});
        } else {
            fail(QStringLiteral("unknown command: %1").arg(cmd));
        }
    } catch (const mail::ComposeError &e) {
        fail(e.message());
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
    }
    return 0;
}
