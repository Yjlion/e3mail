// SPDX-License-Identifier: MPL-2.0
//
// e3mail-cli: a headless driver for the same engine the app uses. One command
// per invocation, JSON on stdout, non-zero exit on error. `sync` runs one
// send-and-fetch pass and returns, so scripts and tests can step accounts
// deterministically.

#include "crypto/Pgp.h"
#include "engine/AccountManager.h"
#include "engine/AtRest.h"
#include "engine/Autoconfig.h"
#ifdef E3MAIL_HAVE_P2P
#include "engine/Pairing.h"
#include "sync/IrohTransport.h"
#endif
#include "mail/Compose.h"
#include "mail/Contacts.h"
#include "mail/ContactsCsv.h"
#include "mail/Export.h"
#include "mail/Keyring.h"
#include "mail/Organize.h"
#include "mail/Preferences.h"
#include "mail/Search.h"
#include "mail/SecureJoin.h"
#include "mail/VCard.h"
#include "SignIn.h"
#include "backup/Repository.h"
#ifdef E3MAIL_HAVE_SFTP
#include "backup/Sftp.h"
#endif
#include "engine/Backups.h"
#include "net/Imap.h"
#include "net/OAuth.h"
#include "net/Pop3.h"
#include "net/Smtp.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Paths.h"

#include <QCommandLineParser>
#include <QEventLoop>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QSysInfo>
#include <QTimer>

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
        "  add                            add an account (--addr, --password or --oauth, server options)\n"
        "  sync                           send the outbox and fetch once (new mail, then a page of older)\n"
        "  list                           list messages (--tag, --search)\n"
        "  show <id>                      show a message (--raw for the original)\n"
        "  read <id>                      mark it read, as opening it in the app does (a receipt, a timer)\n"
        "  send                           queue a message (--to, --subject, --body ...); run sync to deliver\n"
        "  accept <addr>                  accept a sender held in Unverified\n"
        "  block <pattern> | unblock <pattern>\n"
        "  trash <id> | restore <id> | archive <id>\n"
        "  contacts                       list contacts\n"
        "  contact-trust <addr> on|off|auto\n"
        "                                 trust their mail or not, or leave it to the address book\n"
        "  contact-add                    add a contact (--addr, --name)\n"
        "  contact-remove <addr>          remove a contact\n"
        "  vcard-import <file>            import contacts from a vCard file\n"
        "  contacts-export [file]         export the address book as vCard 4.0, or CSV with --csv\n"
        "                                 (vcard-export is the same)\n"
        "  config <key> [value]           read or write an account setting\n"
        "  housekeeping                   run deadlines now\n"
        "  key                            this account's public key, armoured\n"
        "  at-rest                        what guards each account's encryption key on this device\n"
        "  set-passphrase                 guard every account with E3MAIL_NEW_PASSPHRASE (empty: remove it);\n"
        "                                 E3MAIL_PASSPHRASE opens accounts that already have one\n"
        "  verify-invite [--renew]        this account's SecureJoin code, for someone to verify you with\n"
        "  verify-join <code>             verify the person whose code this is (then sync, both sides)\n"
        "  verify-status                  SecureJoin verifications this account started\n"
        "  run [seconds]                  run the account as the app does (mail and device sync) for a while\n"
        "  devices                        this device and the devices paired with it\n"
        "  fallback-now                   mail this device's changes to the account now, if a paired\n"
        "                                 device lacks them, and send it (the mailbox fallback)\n"
        "  pair-offer [--yes]             print a code for a new device, wait for it, and send it a copy\n"
        "  pair-join <code> [--device-name name]\n"
        "                                 become a device of the account the code is for\n"
        "  backup-setup --dest <json> [--dest-secret] --passphrase\n"
        "                                 back this account up there, encrypted (signs in for gdrive/onedrive)\n"
        "  backup-now                     back up now, check what was written, prune old backups\n"
        "  backup-list                    the backups there\n"
        "  backup-forget                  stop backing up from this device\n"
        "  restore --dest <json> [--dest-secret] --passphrase [--snapshot] [--device-name]\n"
        "                                 a new account from a backup\n"
        "  cert --in-host --in-port --in-security [--protocol imap|pop3|smtp]\n"
        "                                 a server's certificate and its SHA-256, to check before\n"
        "                                 trusting it with add --in-cert / --smtp-cert"));
    p.addHelpOption();
    p.addVersionOption();
    const QList<QCommandLineOption> opts = {
        {QStringLiteral("data-dir"), QStringLiteral("Data directory."), QStringLiteral("dir")},
        {QStringLiteral("account"), QStringLiteral("Account id (default: selected)."), QStringLiteral("id")},
        {QStringLiteral("addr"), QStringLiteral("Email address."), QStringLiteral("addr")},
        {QStringLiteral("name"), QStringLiteral("Display name."), QStringLiteral("name")},
        {QStringLiteral("password"), QStringLiteral("Password."), QStringLiteral("password")},
        {QStringLiteral("oauth"), QStringLiteral("With add: sign in with google or microsoft in a browser instead of a password."),
         QStringLiteral("provider")},
        {QStringLiteral("protocol"), QStringLiteral("imap or pop3."), QStringLiteral("protocol"), QStringLiteral("imap")},
        {QStringLiteral("in-host"), QStringLiteral("Incoming server."), QStringLiteral("host")},
        {QStringLiteral("in-port"), QStringLiteral("Incoming port."), QStringLiteral("port")},
        {QStringLiteral("in-security"), QStringLiteral("ssl, starttls or plain."), QStringLiteral("mode")},
        {QStringLiteral("smtp-host"), QStringLiteral("SMTP server."), QStringLiteral("host")},
        {QStringLiteral("smtp-port"), QStringLiteral("SMTP port."), QStringLiteral("port")},
        {QStringLiteral("smtp-security"), QStringLiteral("ssl, starttls or plain."), QStringLiteral("mode")},
        {QStringLiteral("insecure"), QStringLiteral("Accept invalid TLS certificates (test servers only).")},
        {QStringLiteral("yes"), QStringLiteral("With pair-offer: allow the device that asks, without asking.")},
        {QStringLiteral("device-name"), QStringLiteral("With pair-join: what the other devices call this one."), QStringLiteral("name")},
        {QStringLiteral("in-cert"), QStringLiteral("Trust this certificate (SHA-256) for the incoming server."), QStringLiteral("sha256")},
        {QStringLiteral("smtp-cert"), QStringLiteral("Trust this certificate (SHA-256) for the SMTP server."), QStringLiteral("sha256")},
        {QStringLiteral("tag"), QStringLiteral("inbox, unverified, sent, drafts, archive, trash, all."), QStringLiteral("tag"), QStringLiteral("inbox")},
        {QStringLiteral("search"), QStringLiteral("Full-text search."), QStringLiteral("text")},
        {QStringLiteral("raw"), QStringLiteral("Show the original message.")},
        {QStringLiteral("decrypted"), QStringLiteral("With --raw: the message decrypted, as a cleartext .eml.")},
        {QStringLiteral("csv"), QStringLiteral("With contacts-export: CSV instead of vCard.")},
        {QStringLiteral("to"), QStringLiteral("Recipients."), QStringLiteral("list")},
        {QStringLiteral("cc"), QStringLiteral("Cc recipients."), QStringLiteral("list")},
        {QStringLiteral("bcc"), QStringLiteral("Bcc recipients."), QStringLiteral("list")},
        {QStringLiteral("subject"), QStringLiteral("Subject."), QStringLiteral("text")},
        {QStringLiteral("body"), QStringLiteral("Body text."), QStringLiteral("text")},
        {QStringLiteral("attach"), QStringLiteral("Attach a file (repeatable)."), QStringLiteral("file")},
        {QStringLiteral("encrypt"), QStringLiteral("auto, required or plaintext."), QStringLiteral("mode"), QStringLiteral("auto")},
        {QStringLiteral("important"), QStringLiteral("Mark as important.")},
        {QStringLiteral("receipt"), QStringLiteral("With send: ask for a read receipt.")},
        {QStringLiteral("disappear"), QStringLiteral("With send: the message goes to Trash this long after it is read."),
         QStringLiteral("seconds")},
        {QStringLiteral("reply-to"), QStringLiteral("Message id this replies to."), QStringLiteral("id")},
        {QStringLiteral("dest"), QStringLiteral("Backups: where, as JSON ({\"type\":\"folder\",\"path\":…}, webdav, s3, sftp, gdrive, onedrive)."),
         QStringLiteral("json")},
        {QStringLiteral("dest-secret"), QStringLiteral("Backups: the destination's password or secret key."), QStringLiteral("secret")},
        {QStringLiteral("passphrase"), QStringLiteral("Backups: the passphrase that opens them."), QStringLiteral("text")},
        {QStringLiteral("snapshot"), QStringLiteral("With restore: which backup (default: the newest)."), QStringLiteral("name")},
        {QStringLiteral("renew"), QStringLiteral("With verify-invite: a new code; earlier ones stop working.")},
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
        if (cmd == QLatin1String("cert")) {
            // A server's certificate, to compare before trusting it.
            net::ServerSettings s;
            s.host = p.value(QStringLiteral("in-host"));
            s.port = quint16(p.value(QStringLiteral("in-port")).toUInt());
            s.security = net::securityFromString(p.value(QStringLiteral("in-security")));
            s.pinnedSha256 = QByteArray(64, '0'); // matches nothing, so the certificate is always reported
            if (s.host.isEmpty() || !s.port || s.security == net::Security::Plain)
                fail(QStringLiteral("cert needs --in-host, --in-port and --in-security ssl or starttls"));
            const QString protocol = p.value(QStringLiteral("protocol"));
            try {
                if (protocol == QLatin1String("smtp"))
                    net::SmtpClient().connect(s);
                else if (protocol == QLatin1String("pop3"))
                    net::Pop3Client().connect(s);
                else
                    net::ImapClient().connect(s);
                fail(QStringLiteral("no certificate was presented"));
            } catch (const net::CertError &e) {
                print(QJsonObject{{QStringLiteral("sha256"), QString::fromLatin1(e.info.sha256)},
                                  {QStringLiteral("fingerprint"), net::formatFingerprint(e.info.sha256)},
                                  {QStringLiteral("subject"), e.info.subject},
                                  {QStringLiteral("issuer"), e.info.issuer},
                                  {QStringLiteral("expires"), e.info.expires.toUTC().toString(Qt::ISODate)},
                                  {QStringLiteral("problems"), QJsonArray::fromStringList(e.info.problems)},
                                  {QStringLiteral("trusted_by_tls"), e.info.problems.isEmpty()}});
            }
            return 0;
        }
        // A passphrase guards the accounts (ADR 0023): from the environment,
        // never the command line, which other users can read.
        const QString accountsRoot = Paths::dataDir() + QStringLiteral("/accounts");
        if (AtRest::passphraseRequired(accountsRoot)) {
            const QString given = qEnvironmentVariable("E3MAIL_PASSPHRASE");
            if (given.isEmpty() || !AtRest::checkPassphrase(accountsRoot, given))
                fail(QStringLiteral("the accounts are guarded by a passphrase; set E3MAIL_PASSPHRASE"));
            AtRest::setPassphrase(given);
        }
        AccountManager mgr(Paths::dataDir());
        if (cmd == QLatin1String("set-passphrase")) {
            // The new one from E3MAIL_NEW_PASSPHRASE; empty removes it.
            AtRest::setPassphrase(qEnvironmentVariable("E3MAIL_NEW_PASSPHRASE"));
            for (Account *acc : mgr.accounts())
                AtRest::reguard(acc->dir(), acc->id(), acc->dbKey());
        }
        if (cmd == QLatin1String("at-rest") || cmd == QLatin1String("set-passphrase")) {
            QJsonArray a;
            for (Account *acc : mgr.accounts()) {
                const AtRest::Guard g = AtRest::guard(acc->dir());
                a.append(QJsonObject{{QStringLiteral("id"), acc->id()},
                                     {QStringLiteral("addr"), acc->addr()},
                                     {QStringLiteral("key"), g == AtRest::Guard::Keyring      ? QStringLiteral("keyring")
                                                             : g == AtRest::Guard::Passphrase ? QStringLiteral("passphrase")
                                                                                              : QStringLiteral("beside the mail")}});
            }
            print(a);
            return 0;
        }
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
        if (cmd == QLatin1String("restore")) {
            QJsonObject dest = QJsonDocument::fromJson(p.value(QStringLiteral("dest")).toUtf8()).object();
            const QString secret = p.value(QStringLiteral("dest-secret"));
            auto d = backups::destination(dest, secret);
            if (!d)
                fail(QStringLiteral("restore needs --dest (a JSON object with a known type)"));
            const QByteArray passphrase = p.value(QStringLiteral("passphrase")).toUtf8();
            backup::Repository repo(*d);
            try {
                repo.unlock(passphrase);
                const auto snaps = repo.snapshots();
                if (snaps.isEmpty())
                    fail(QStringLiteral("there are no backups there"));
                const QString snapshot = p.isSet(QStringLiteral("snapshot")) ? p.value(QStringLiteral("snapshot")) : snaps.first().name;
                const QString name = p.isSet(QStringLiteral("device-name")) ? p.value(QStringLiteral("device-name"))
                                                                            : QSysInfo::machineHostName();
                Account *a = backups::restore(mgr, *d, passphrase, snapshot, name);
                print(QJsonObject{{QStringLiteral("id"), a->id()}, {QStringLiteral("addr"), a->addr()},
                                  {QStringLiteral("snapshot"), snapshot}});
            } catch (const backup::BadPassphrase &) {
                fail(QStringLiteral("wrong passphrase"));
            } catch (const std::exception &e) {
                fail(QString::fromUtf8(e.what()));
            }
            return 0;
        }

        if (cmd == QLatin1String("add")) {
            const QString addr = p.value(QStringLiteral("addr"));
            const bool oauth = p.isSet(QStringLiteral("oauth"));
            if (!addr.contains(u'@') || (!p.isSet(QStringLiteral("password")) && !oauth))
                fail(QStringLiteral("add needs --addr and --password (or --oauth google|microsoft)"));
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
            if (oauth) {
                // The browser comes back to a listener on 127.0.0.1 (ADR 0019).
                const auto provider = net::oauth::provider(p.value(QStringLiteral("oauth")));
                if (!provider)
                    fail(QStringLiteral("no client ID for %1 in this build; set E3MAIL_OAUTH_%2_ID")
                             .arg(p.value(QStringLiteral("oauth")), p.value(QStringLiteral("oauth")).toUpper()));
                SignIn signIn(*provider, addr);
                QEventLoop loop;
                QString error;
                QObject::connect(&signIn, &SignIn::openBrowser, &loop, [](const QUrl &url) {
                    std::fprintf(stderr, "Open this in a browser on this machine to sign in:\n\n%s\n\n",
                                 url.toString(QUrl::FullyEncoded).toUtf8().constData());
                });
                QObject::connect(&signIn, &SignIn::finished, &loop, [&](const QString &e) {
                    error = e;
                    loop.quit();
                });
                signIn.start();
                loop.exec();
                if (!error.isEmpty())
                    fail(error);
                if (!signIn.email().isEmpty() && signIn.email().compare(addr, Qt::CaseInsensitive) != 0)
                    fail(QStringLiteral("signed in as %1, not %2").arg(signIn.email(), addr));
                s.auth = QStringLiteral("oauth:") + provider->id;
                s.oauthRefresh = signIn.refreshToken();
                s.oauthClientId = provider->clientId;
                s.oauthClientSecret = provider->clientSecret;
                s.oauthTokenUrl = provider->tokenUrl.toString();
            }
            s.acceptInvalidCertificates = p.isSet(QStringLiteral("insecure"));
            for (const auto &[opt, pin] : {std::pair{QStringLiteral("in-cert"), &s.inCertSha256},
                                           std::pair{QStringLiteral("smtp-cert"), &s.smtpCertSha256}}) {
                if (!p.isSet(opt))
                    continue;
                *pin = net::normalizeFingerprint(p.value(opt));
                if (pin->isEmpty())
                    fail(QStringLiteral("--%1 needs a SHA-256 fingerprint (64 hex digits)").arg(opt));
            }
            Account *acc = mgr.create();
            acc->configure(s);
            print(QJsonObject{{QStringLiteral("id"), acc->id()}, {QStringLiteral("addr"), acc->addr()},
                              {QStringLiteral("password_stored_in"), acc->secretLocation()}});
            return 0;
        }

#ifdef E3MAIL_HAVE_P2P
        if (cmd == QLatin1String("pair-join")) {
            const QString name = p.isSet(QStringLiteral("device-name")) ? p.value(QStringLiteral("device-name"))
                                                                        : QSysInfo::machineHostName();
            PairingJoin join(mgr, args.value(1), name);
            if (!join.isValid())
                fail(QStringLiteral("not a pairing code, or this account is already here"));
            QEventLoop loop;
            int code = 1;
            QObject::connect(&join, &PairingJoin::waiting, &loop, [&] {
                std::fprintf(stderr, "waiting for %s to be allowed on the other device\n", qPrintable(join.account()));
            });
            QObject::connect(&join, &PairingJoin::finished, &loop, [&](bool ok, const QString &message, int id) {
                print(QJsonObject{{QStringLiteral("paired"), ok}, {QStringLiteral("message"), message},
                                  {QStringLiteral("id"), id}});
                code = ok ? 0 : 1;
                loop.quit();
            });
            loop.exec();
            return code;
        }
#endif

        Account *acc = p.isSet(QStringLiteral("account")) ? mgr.account(p.value(QStringLiteral("account")).toInt())
                                                          : mgr.selected();
        if (!acc)
            fail(QStringLiteral("no such account; add one first"));
        MailContext &ctx = acc->ctx();
        const qint64 idArg = args.value(1).toLongLong();

        if (cmd == QLatin1String("sync")) {
            const int older = MailWorker::runOnce(acc->dir(), acc->credentials());
            print(QJsonObject{{QStringLiteral("older_remaining"), older},
                              {QStringLiteral("inbox"), mail::Search::count(ctx, tag::Inbox, false)},
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
                const QByteArray raw = p.isSet(QStringLiteral("decrypted")) ? mail::Export::decryptedEml(ctx, idArg)
                                                                            : mail::Export::eml(ctx, idArg);
                if (raw.isEmpty())
                    fail(p.isSet(QStringLiteral("decrypted"))
                             ? QStringLiteral("message %1 is not encrypted, not retained, or cannot be decrypted").arg(idArg)
                             : QStringLiteral("the original of message %1 is not retained").arg(idArg));
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
            if (d->trashed)
                o.insert(QStringLiteral("trash_reason"), int(d->trashReason));
            o.insert(QStringLiteral("receipt_to"), d->receiptTo);
            QJsonArray readBy;
            for (const auto &r : d->readBy)
                readBy.append(QJsonObject{{QStringLiteral("addr"), r.addr},
                                          {QStringLiteral("at"), r.at.toUTC().toString(Qt::ISODate)}});
            o.insert(QStringLiteral("read_by"), readBy);
            if (d->ephemeralSeconds)
                o.insert(QStringLiteral("disappear_seconds"), d->ephemeralSeconds);
            if (d->expiresAt.isValid())
                o.insert(QStringLiteral("expires"), d->expiresAt.toUTC().toString(Qt::ISODate));
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
            // The CLI writes plain text: a reply quotes the plain body, never
            // an HTML quote without the new text in it.
            d.html.clear();
            d.importance = p.isSet(QStringLiteral("important")) ? 1 : 0;
            d.requestReceipt = p.isSet(QStringLiteral("receipt"));
            d.ephemeralSeconds = p.value(QStringLiteral("disappear")).toInt();
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
        } else if (cmd == QLatin1String("read")) {
            mail::Organize::markRead(ctx, idArg, true);
            print(QJsonObject{{QStringLiteral("read"), idArg}});
        } else if (cmd == QLatin1String("contact-trust")) {
            const QString v = args.value(2);
            if (v != QLatin1String("on") && v != QLatin1String("off") && v != QLatin1String("auto"))
                fail(QStringLiteral("contact-trust <addr> on|off|auto"));
            mail::Contacts::setTrust(ctx, args.value(1),
                                     v == QLatin1String("auto") ? std::nullopt : std::optional<bool>(v == QLatin1String("on")));
            print(QJsonObject{{QStringLiteral("addr"), args.value(1)},
                              {QStringLiteral("trusted"), mail::Contacts::isTrusted(ctx, args.value(1))}});
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
            for (const ContactInfo &c : mail::Contacts::list(ctx)) {
                QJsonArray phones;
                for (const ContactPhone &ph : c.phones)
                    phones.append(QJsonObject{{QStringLiteral("label"), ph.label}, {QStringLiteral("number"), ph.number}});
                a.append(QJsonObject{{QStringLiteral("addr"), c.addr},
                                     {QStringLiteral("name"), c.name},
                                     {QStringLiteral("known"), c.isKnown()},
                                     {QStringLiteral("key"), c.fingerprint},
                                     {QStringLiteral("verified"), c.verified},
                                     {QStringLiteral("blocked"), c.blocked},
                                     {QStringLiteral("organization"), c.organization},
                                     {QStringLiteral("title"), c.title},
                                     {QStringLiteral("birthday"), c.birthday},
                                     {QStringLiteral("notes"), c.notes},
                                     {QStringLiteral("phones"), phones}});
            }
            print(a);
        } else if (cmd == QLatin1String("contact-add")) {
            const auto id = mail::Contacts::create(ctx, p.value(QStringLiteral("addr")), p.value(QStringLiteral("name")));
            if (!id)
                fail(QStringLiteral("not an address, or a contact already"));
            print(QJsonObject{{QStringLiteral("id"), *id}});
        } else if (cmd == QLatin1String("contact-remove")) {
            const auto c = mail::Contacts::get(ctx, args.value(1));
            if (!c)
                fail(QStringLiteral("no such contact"));
            mail::Contacts::remove(ctx, c->id);
            print(QJsonObject{{QStringLiteral("removed"), c->addr}});
        } else if (cmd == QLatin1String("vcard-import")) {
            QFile f(args.value(1));
            if (!f.open(QIODevice::ReadOnly))
                fail(QStringLiteral("cannot read %1").arg(args.value(1)));
            const auto r = mail::Contacts::import(ctx, mail::VCard::parse(f.readAll()));
            print(QJsonObject{{QStringLiteral("added"), r.added},
                              {QStringLiteral("updated"), r.updated},
                              {QStringLiteral("skipped"), r.skipped}});
        } else if (cmd == QLatin1String("contacts-export") || cmd == QLatin1String("vcard-export")) {
            const QList<ContactInfo> book = mail::Export::addressBook(ctx);
            const QByteArray vcf = p.isSet(QStringLiteral("csv")) ? mail::ContactsCsv::emit(book) : mail::VCard::emit(book);
            if (args.size() > 1) {
                QFile f(args.value(1));
                if (!f.open(QIODevice::WriteOnly) || f.write(vcf) != vcf.size())
                    fail(QStringLiteral("cannot write %1").arg(args.value(1)));
            } else {
                std::fwrite(vcf.constData(), 1, size_t(vcf.size()), stdout);
            }
        } else if (cmd == QLatin1String("config")) {
            const QByteArray key = args.value(1).toUtf8();
            if (key.isEmpty())
                fail(QStringLiteral("config needs a key"));
            if (key.contains("password"))
                fail(QStringLiteral("passwords are not shown or set here"));
            if (args.size() > 2)
                mail::Preferences::set(ctx, key.constData(), args.value(2));
            print(QJsonObject{{QString::fromUtf8(key), ctx.config.get(key.constData())}});
        } else if (cmd == QLatin1String("run")) {
            const int seconds = args.size() > 1 ? args.value(1).toInt() : 30;
            acc->start();
            QEventLoop loop;
            QTimer::singleShot(seconds * 1000, &loop, &QEventLoop::quit);
            loop.exec();
            // How each connected device was reached at the end: direct or relay.
            QJsonObject paths;
#ifdef E3MAIL_HAVE_P2P
            if (auto *t = qobject_cast<sync::IrohTransport *>(acc->deviceTransport()))
                for (const QString &d : t->connectedDevices())
                    paths.insert(d, t->path(d));
#endif
            acc->stop();
            acc->stopDevices();
            print(QJsonObject{{QStringLiteral("status"), acc->status()}, {QStringLiteral("detail"), acc->statusDetail()},
                              {QStringLiteral("paths"), paths}});
        } else if (cmd == QLatin1String("fallback-now")) {
            const bool queued = acc->mailFallbackNow();
            if (queued)
                MailWorker::runOnce(acc->dir(), acc->credentials());
            print(QJsonObject{{QStringLiteral("queued"), queued}});
        } else if (cmd == QLatin1String("backup-setup")) {
            QJsonObject dest = QJsonDocument::fromJson(p.value(QStringLiteral("dest")).toUtf8()).object();
            QString secret = p.value(QStringLiteral("dest-secret"));
            const QString type = dest.value(QLatin1String("type")).toString();
            if ((type == QLatin1String("gdrive") || type == QLatin1String("onedrive")) && secret.isEmpty()) {
                // Signing in for the storage, with the scopes for backups.
                const auto provider = net::oauth::forBackups(type == QLatin1String("gdrive") ? QStringLiteral("google")
                                                                                              : QStringLiteral("microsoft"));
                if (!provider)
                    fail(QStringLiteral("no OAuth client ID in this build for %1").arg(type));
                SignIn signIn(*provider, QString());
                QEventLoop loop;
                QString error;
                QObject::connect(&signIn, &SignIn::openBrowser, &loop, [](const QUrl &url) {
                    std::fprintf(stderr, "Open this in a browser on this machine to sign in:\n\n%s\n\n",
                                 url.toString(QUrl::FullyEncoded).toUtf8().constData());
                });
                QObject::connect(&signIn, &SignIn::finished, &loop, [&](const QString &e) {
                    error = e;
                    loop.quit();
                });
                signIn.start();
                loop.exec();
                if (!error.isEmpty())
                    fail(error);
                secret = signIn.refreshToken();
                dest.insert(QStringLiteral("clientId"), provider->clientId);
                dest.insert(QStringLiteral("clientSecret"), provider->clientSecret);
                dest.insert(QStringLiteral("tokenUrl"), provider->tokenUrl.toString());
            }
            try {
                backups::setUp(*acc, dest, secret, p.value(QStringLiteral("passphrase")).toUtf8());
                print(QJsonObject{{QStringLiteral("set_up"), true}});
            } catch (const backup::BadPassphrase &) {
                fail(QStringLiteral("there is a backup there already, and the passphrase does not open it"));
#ifdef E3MAIL_HAVE_SFTP
            } catch (const backup::HostKeyError &e) {
                // As for a self-signed certificate: shown, then pinned by the person.
                print(QJsonObject{{QStringLiteral("host_key"), QString::fromLatin1(e.sha256)},
                                  {QStringLiteral("changed"), e.changed},
                                  {QStringLiteral("hint"), QStringLiteral("check it, then add \"hostKey\" to --dest")}});
                return 3;
#endif
            } catch (const net::CertError &e) {
                print(QJsonObject{{QStringLiteral("certificate"), QString::fromLatin1(e.info.sha256)},
                                  {QStringLiteral("hint"), QStringLiteral("check it, then add \"pin\" to --dest")}});
                return 3;
            } catch (const std::exception &e) {
                fail(QString::fromUtf8(e.what()));
            }
        } else if (cmd == QLatin1String("backup-now")) {
            if (!backups::isSetUp(*acc))
                fail(QStringLiteral("backups are not set up on this device; see backup-setup"));
            const backups::Outcome o = backups::run(*acc).result();
            backups::record(*acc, o);
            print(QJsonObject{{QStringLiteral("ok"), o.ok}, {QStringLiteral("error"), o.error},
                              {QStringLiteral("snapshot"), o.result.snapshot},
                              {QStringLiteral("uploaded"), o.result.uploaded}, {QStringLiteral("reused"), o.result.reused},
                              {QStringLiteral("bytes"), o.result.bytes}});
            return o.ok ? 0 : 1;
        } else if (cmd == QLatin1String("backup-list")) {
            if (!backups::isSetUp(*acc))
                fail(QStringLiteral("backups are not set up on this device; see backup-setup"));
            auto d = backups::destination(backups::config(*acc), acc->secret("backup-dest", cfg::BackupDestSecret));
            backup::Repository repo(*d);
            repo.setKey(QByteArray::fromBase64(acc->secret("backup", cfg::BackupKey).toLatin1()));
            QJsonArray list;
            try {
                for (const backup::Snapshot &s : repo.snapshots())
                    list.append(QJsonObject{{QStringLiteral("snapshot"), s.name},
                                            {QStringLiteral("created"), s.created.toString(Qt::ISODate)},
                                            {QStringLiteral("this_device"), s.device == repo.deviceTag(ctx.ops.deviceId())}});
            } catch (const std::exception &e) {
                fail(QString::fromUtf8(e.what()));
            }
            print(QJsonObject{{QStringLiteral("snapshots"), list},
                              {QStringLiteral("last"), ctx.config.get(cfg::BackupLast)},
                              {QStringLiteral("last_error"), ctx.config.get(cfg::BackupLastError)}});
        } else if (cmd == QLatin1String("backup-forget")) {
            backups::forget(*acc);
            print(QJsonObject{{QStringLiteral("set_up"), false}});
        } else if (cmd == QLatin1String("devices")) {
            QJsonArray devices;
            Statement st(ctx.db, "SELECT id, name, node_id, last_seen FROM devices ORDER BY paired_at");
            while (st.step())
                devices.append(QJsonObject{{QStringLiteral("device"), st.text(0)},
                                           {QStringLiteral("name"), st.text(1)},
                                           {QStringLiteral("endpoint"), st.text(2)},
                                           {QStringLiteral("last_seen"), st.isNull(3) ? QJsonValue()
                                                : QJsonValue(QDateTime::fromMSecsSinceEpoch(st.int64(3)).toUTC().toString(Qt::ISODate))}});
            print(QJsonObject{{QStringLiteral("device"), ctx.ops.deviceId()},
                              {QStringLiteral("name"), ctx.config.get(cfg::DeviceName)},
                              {QStringLiteral("sync_built_in"), Account::devicesAvailable()},
                              {QStringLiteral("devices"), devices}});
#ifdef E3MAIL_HAVE_P2P
        } else if (cmd == QLatin1String("pair-offer")) {
            PairingOffer offer(*acc);
            if (!offer.isReady())
                fail(QStringLiteral("the device endpoint did not start"));
            print(QJsonObject{{QStringLiteral("code"), offer.code()}});
            std::fflush(stdout);
            QEventLoop loop;
            int code = 1;
            QObject::connect(&offer, &PairingOffer::requested, &loop, [&](const QString &name) {
                bool yes = p.isSet(QStringLiteral("yes"));
                if (!yes) {
                    std::fprintf(stderr, "Allow \"%s\" to become a device of %s? [y/N] ", qPrintable(name),
                                 qPrintable(acc->addr()));
                    char answer[8] = {};
                    yes = std::fgets(answer, sizeof answer, stdin) && (answer[0] == 'y' || answer[0] == 'Y');
                }
                yes ? offer.allow() : offer.deny();
            });
            QObject::connect(&offer, &PairingOffer::finished, &loop, [&](bool ok, const QString &message) {
                print(QJsonObject{{QStringLiteral("paired"), ok}, {QStringLiteral("message"), message}});
                code = ok ? 0 : 1;
                // Let the new device's first sync start before leaving.
                QTimer::singleShot(ok ? 3000 : 0, &loop, &QEventLoop::quit);
            });
            loop.exec();
            return code;
#endif
        } else if (cmd == QLatin1String("key")) {
            const QString fpr = mail::Keyring::selfFingerprint(ctx);
            std::fputs(ctx.pgp.exportPublic(fpr, true).constData(), stdout);
        } else if (cmd == QLatin1String("verify-invite")) {
            std::puts(qPrintable(p.isSet(QStringLiteral("renew")) ? mail::SecureJoin::renewInvite(ctx)
                                                                    : mail::SecureJoin::invite(ctx)));
        } else if (cmd == QLatin1String("verify-join")) {
            const auto inv = mail::SecureJoin::parse(args.value(1));
            if (!inv)
                fail(QStringLiteral("not a SecureJoin code"));
            mail::SecureJoin::join(ctx, *inv);
            print(QJsonObject{{QStringLiteral("joining"), inv->addr}, {QStringLiteral("fingerprint"), inv->fpr}});
        } else if (cmd == QLatin1String("verify-status")) {
            static const QStringList kStates{QStringLiteral("failed"), QStringLiteral("none"), QStringLiteral("requested"),
                                             QStringLiteral("token-sent"), QStringLiteral("done")};
            QJsonArray a;
            for (const auto &j : mail::SecureJoin::joins(ctx))
                a.append(QJsonObject{{QStringLiteral("addr"), j.addr},
                                     {QStringLiteral("fingerprint"), j.fpr},
                                     {QStringLiteral("state"), kStates.value(int(j.state) + 1)},
                                     {QStringLiteral("error"), j.error}});
            print(a);
        } else if (cmd == QLatin1String("housekeeping")) {
            const auto r = mail::Organize::housekeeping(ctx);
            print(QJsonObject{{QStringLiteral("purged"), r.purged}, {QStringLiteral("swept"), r.swept},
                              {QStringLiteral("raw_expired"), r.rawExpired}, {QStringLiteral("expired"), r.expired}});
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
