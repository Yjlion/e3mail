// SPDX-License-Identifier: MPL-2.0
#include "MailWorker.h"

#include "crypto/Pgp.h"
#include "mail/Compose.h"
#include "mail/Ingest.h"
#include "mail/Keyring.h"
#include "mail/Organize.h"
#include "net/Imap.h"
#include "net/Pop3.h"
#include "net/Smtp.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"
#include "util/Log.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QEventLoop>
#include <QTimer>

#include <memory>

namespace e3 {

namespace {
constexpr qint64 kDayMs = 24LL * 3600 * 1000;
constexpr int kIdleMs = 9 * 60 * 1000; // re-issue IDLE before servers' 30 minute cutoff
constexpr qint64 kHousekeepingMs = 3600 * 1000;
constexpr int kBackfillPage = 100; // older messages per round, between which new mail and the outbox go first
} // namespace

// Everything a pass needs, opened on the worker's own thread.
class MailWorker::Session
{
public:
    explicit Session(const QString &dir)
        : db(dir + QStringLiteral("/mail.db")), blobs(dir + QStringLiteral("/blobs")), config(db), ops(db),
          ctx{db, blobs, pgp, config, ops}
    {
        mail::Keyring::loadAll(ctx);
    }

    Database db;
    BlobStore blobs;
    crypto::Pgp pgp;
    Config config;
    OpLog ops;
    MailContext ctx;
    std::unique_ptr<net::ImapClient> imap;
    // Older UIDs still to fetch, ascending, listed once per connection.
    QList<quint32> backfill;
    bool backfillListed = false;
    qint64 lastHousekeeping = 0;

    // Returns the number of messages the server refused nothing of.
    int sendOutbox(const Credentials &creds, MailWorker *w)
    {
        const QList<mail::Compose::Outbox> due = mail::Compose::dueOutbox(ctx);
        if (due.isEmpty())
            return 0;
        net::SmtpClient smtp;
        try {
            smtp.connect(creds.smtp);
        } catch (const net::NetError &e) {
            for (const auto &o : due)
                mail::Compose::markFailed(ctx, o, e.message());
            throw;
        }
        int sentCount = 0;
        for (const auto &o : due) {
            try {
                const QStringList refused = smtp.send(o.from, o.rcpts, o.raw);
                mail::Compose::markSent(ctx, o, refused);
                ++sentCount;
                if (w)
                    Q_EMIT w->sent(o.msgId);
            } catch (const net::NetError &e) {
                qCWarning(lcEngine) << "send failed:" << e.what();
                mail::Compose::markFailed(ctx, o, e.message());
            }
        }
        smtp.quit();
        return sentCount;
    }

    // Records a server message and returns whether it is new to us.
    bool isKnown(const QString &key)
    {
        return db.queryInt("SELECT 1 FROM server_msgs WHERE remote_key=?", key).has_value();
    }

    // `preexisting`: it was on the server before e3mail first looked, so
    // retention never deletes it and nobody is told it is new. A message we
    // already have keeps what its first copy was: re-reading a mailbox under
    // a new UIDVALIDITY neither makes old mail deletable nor exempts new mail
    // that retention is still keeping.
    qint64 ingest(const QString &key, const QByteArray &raw, bool preexisting, MailWorker *w)
    {
        const mail::Ingest::Result r = mail::Ingest::process(ctx, raw);
        if (r.duplicate) {
            if (const auto first = db.queryInt("SELECT preexisting FROM server_msgs WHERE msg_id=? "
                                               "ORDER BY first_seen, rowid LIMIT 1",
                                               r.msgId))
                preexisting = *first != 0;
        }
        Transaction tx(db);
        db.run("INSERT OR IGNORE INTO server_msgs(remote_key, first_seen, msg_id, preexisting) VALUES(?, ?, ?, ?)",
               key, QDateTime::currentMSecsSinceEpoch(), r.msgId, preexisting);
        // Tells the other devices this one has it, so whichever deletes it
        // from the server knows when everyone has (ADR 0013).
        if (db.changes())
            ops.record(op::ServerAck, {{QStringLiteral("key"), key}});
        tx.commit();
        if (w && !r.duplicate) {
            Q_EMIT w->mailChanged();
            if (!r.held && !r.trashed && !preexisting)
                Q_EMIT w->newMail(r.msgId);
        }
        return r.msgId;
    }

    // Which of the given server keys retention says to delete now. Mail that
    // was already on the server before e3mail first looked is never deleted:
    // retention is not retroactive. With several devices (multi-client), a
    // message is deleted only once every paired device has acknowledged it;
    // with one device that is satisfied as soon as it is stored here.
    QStringList dueForDeletion(const QStringList &present)
    {
        const QString policy = config.get(cfg::ServerRetention);
        if (policy == QLatin1String("never"))
            return {};
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 keepMs = policy == QLatin1String("keep") ? qint64(config.getInt(cfg::ServerKeepDays)) * kDayMs : 0;
        QStringList out;
        for (const QString &key : present) {
            Statement st(db, "SELECT first_seen FROM server_msgs s WHERE remote_key=? AND preexisting=0 AND deleted=0 "
                             "AND msg_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM devices d WHERE NOT EXISTS "
                             "(SELECT 1 FROM server_acks a WHERE a.remote_key = s.remote_key AND a.device = d.id))");
            st.bind(1, key);
            if (st.step() && st.int64(0) + keepMs <= now)
                out.append(key);
        }
        return out;
    }

    void markDeleted(const QStringList &keys)
    {
        for (const QString &k : keys)
            db.run("UPDATE server_msgs SET deleted=1 WHERE remote_key=?", k);
    }

    quint32 cursor(const char *key) const { return config.get(key).toUInt(); }
    void setCursor(const char *key, quint32 v) { config.set(key, QString::number(v)); }

    // IMAP: keep the connection; fetch what is new, then one page of older
    // mail, newest first; apply retention. Returns how many older messages
    // are still to fetch.
    int syncImap(const Credentials &creds, MailWorker *w)
    {
        if (!imap) {
            imap = std::make_unique<net::ImapClient>();
            imap->connect(creds.incoming);
            backfill.clear();
            backfillListed = false;
        }
        const net::ImapClient::Mailbox mb = imap->select();
        const QString validity = QString::number(mb.uidValidity);
        if (config.get(cfg::ImapUidValidity) != validity || !config.isSet(cfg::ImapBaseline)) {
            if (config.isSet(cfg::ImapUidValidity) && config.get(cfg::ImapUidValidity) != validity)
                qCWarning(lcEngine) << "UIDVALIDITY changed; re-reading the mailbox (duplicates are skipped)";
            config.set(cfg::ImapUidValidity, validity);
            // Without UIDNEXT, everything there now counts as found there.
            quint32 next = mb.uidNext;
            if (!next) {
                const QList<quint32> all = imap->uids(1);
                next = all.isEmpty() ? 1 : all.last() + 1;
            }
            setCursor(cfg::ImapBaseline, next);
            setCursor(cfg::ImapHigh, next - 1);
            setCursor(cfg::ImapLow, next);
            backfill.clear();
            backfillListed = false;
        }
        const quint32 baseline = cursor(cfg::ImapBaseline);
        auto keyOf = [&](quint32 uid) { return QStringLiteral("imap:%1:%2").arg(validity).arg(uid); };

        // New mail first, oldest to newest.
        quint32 high = cursor(cfg::ImapHigh);
        if (!mb.uidNext || mb.uidNext > high + 1) {
            for (quint32 uid : imap->uids(high + 1)) {
                if (!isKnown(keyOf(uid))) {
                    if (const auto raw = imap->fetch(uid))
                        ingest(keyOf(uid), *raw, uid < baseline, w);
                }
                setCursor(cfg::ImapHigh, high = uid);
                if (w && w->shouldStop())
                    return 0;
            }
        }

        // Then a page of what was there before, newest first.
        quint32 low = cursor(cfg::ImapLow);
        if (low > 1) {
            if (!backfillListed) {
                backfill = imap->uids(1, low - 1);
                backfillListed = true;
            }
            for (int n = 0; n < kBackfillPage && !backfill.isEmpty(); ++n) {
                const quint32 uid = backfill.takeLast();
                if (!isKnown(keyOf(uid))) {
                    if (const auto raw = imap->fetch(uid))
                        ingest(keyOf(uid), *raw, true, w);
                }
                setCursor(cfg::ImapLow, low = uid);
                if (w && w->shouldStop())
                    return int(backfill.size());
            }
            if (backfill.isEmpty())
                setCursor(cfg::ImapLow, low = 1);
        }
        const int remaining = low > 1 ? int(backfill.size()) : 0;
        if (!remaining)
            config.setInt(cfg::FirstSyncDone, 1);

        // Retention, over what we fetched from this mailbox and may delete.
        QStringList candidates;
        Statement st(db, "SELECT remote_key FROM server_msgs WHERE remote_key LIKE ? AND deleted=0 AND preexisting=0");
        st.bind(1, QStringLiteral("imap:%1:%").arg(validity));
        while (st.step())
            candidates.append(st.text(0));
        const QStringList del = dueForDeletion(candidates);
        if (!del.isEmpty()) {
            QList<quint32> delUids;
            for (const QString &k : del)
                delUids.append(k.section(u':', 2).toUInt());
            imap->remove(delUids);
            markDeleted(del);
        }
        return remaining;
    }

    // POP3: a fresh session per poll; deletions commit at QUIT.
    void syncPop3(const Credentials &creds, MailWorker *w)
    {
        net::Pop3Client pop;
        pop.connect(creds.incoming);
        const bool firstSync = !config.getBool(cfg::FirstSyncDone);
        const QList<net::Pop3Client::Entry> entries = pop.list();
        QStringList present;
        QHash<QString, int> numberOf;
        for (const auto &e : entries) {
            const QString key = QStringLiteral("pop3:") + e.uidl;
            present.append(key);
            numberOf.insert(key, e.number);
        }
        // Newest first, so recent mail shows while a large first download runs.
        for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
            const QString key = QStringLiteral("pop3:") + it->uidl;
            if (isKnown(key))
                continue;
            ingest(key, pop.retrieve(it->number), firstSync, w);
            if (w && w->shouldStop())
                break;
        }
        config.setInt(cfg::FirstSyncDone, 1);
        const QStringList del = dueForDeletion(present);
        for (const QString &k : del)
            pop.remove(numberOf.value(k));
        pop.quit();
        markDeleted(del);
    }

    void housekeepingIfDue(MailWorker *w)
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastHousekeeping < kHousekeepingMs)
            return;
        lastHousekeeping = now;
        const auto r = mail::Organize::housekeeping(ctx);
        if (w && (r.purged || r.swept))
            Q_EMIT w->mailChanged();
    }
};

MailWorker::MailWorker(const QString &accountDir, const Credentials &creds, QObject *parent)
    : QThread(parent), m_dir(accountDir), m_creds(creds)
{
}

MailWorker::~MailWorker()
{
    requestStop();
    wait();
}

void MailWorker::nap(int ms)
{
    QElapsedTimer t;
    t.start();
    while (!m_stop && !m_wake && t.elapsed() < ms) {
        QEventLoop loop;
        QTimer::singleShot(qMin<qint64>(250, ms - t.elapsed()), &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void MailWorker::run()
{
    std::unique_ptr<Session> s;
    try {
        s = std::make_unique<Session>(m_dir);
    } catch (const std::exception &e) {
        Q_EMIT status(QStringLiteral("error"), QString::fromUtf8(e.what()));
        return;
    }
    const bool pop3 = m_creds.protocol == QLatin1String("pop3");
    int backoffMs = 15000;
    while (!m_stop) {
        m_wake = false;
        try {
            s->housekeepingIfDue(this);
            if (s->sendOutbox(m_creds, this))
                Q_EMIT mailChanged();
            int older = 0;
            if (pop3)
                s->syncPop3(m_creds, this);
            else
                older = s->syncImap(m_creds, this);
            Q_EMIT status(QStringLiteral("online"), QString());
            Q_EMIT backfillProgress(older);
            backoffMs = 15000;
            // Wait for news. Anything queued meanwhile wakes us. While older
            // mail is still coming, go straight on to the next page.
            if (older) {
                continue;
            } else if (pop3) {
                nap(qMax(30, s->config.getInt(cfg::PollSeconds)) * 1000);
            } else if (!m_stop && !m_wake) {
                s->imap->idle(kIdleMs, [this] { return m_stop || m_wake; });
            }
        } catch (const net::AuthError &e) {
            s->imap.reset();
            Q_EMIT status(QStringLiteral("auth"), e.message());
            nap(30 * 60 * 1000); // until the user fixes the password and we are restarted
        } catch (const std::exception &e) {
            s->imap.reset();
            qCWarning(lcEngine) << "account pass failed:" << e.what();
            Q_EMIT status(QStringLiteral("offline"), QString::fromUtf8(e.what()));
            nap(backoffMs);
            backoffMs = qMin(backoffMs * 2, 5 * 60 * 1000);
        }
    }
    // Stopping (the app quitting or the account changing): close without
    // LOGOUT, so a slow or unreachable server cannot hold the quit up.
    if (s && s->imap)
        s->imap->close();
}

int MailWorker::runOnce(const QString &accountDir, const Credentials &creds)
{
    Session s(accountDir);
    s.lastHousekeeping = 0;
    s.housekeepingIfDue(nullptr);
    s.sendOutbox(creds, nullptr);
    if (creds.protocol == QLatin1String("pop3")) {
        s.syncPop3(creds, nullptr);
        return 0;
    }
    const int older = s.syncImap(creds, nullptr);
    s.imap->logout();
    return older;
}

} // namespace e3
