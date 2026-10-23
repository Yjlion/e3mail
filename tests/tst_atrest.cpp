// SPDX-License-Identifier: MPL-2.0
#include "engine/AccountManager.h"
#include "engine/AtRest.h"
#include "mail/Compose.h"
#include "store/BlobStore.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/Schema.h"
#include "store/Snapshot.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace e3;

// The database and blobs encrypted at rest (ADR 0023).
class TestAtRest : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;

    static QByteArray secret() { return "the plan is under the third stone"; }
    QString accounts() const { return m_dir->path() + QStringLiteral("/accounts"); }

    // Every byte under `dir`, files concatenated.
    static QByteArray everything(const QString &dir)
    {
        QByteArray all;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile f(it.next());
            if (f.open(QIODevice::ReadOnly))
                all += f.readAll();
        }
        return all;
    }

    static QJsonObject keyFile(const QString &dir)
    {
        QFile f(dir + QStringLiteral("/key.json"));
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    }

    static qint64 draftWith(Account *a, const QString &text, const QByteArray &attachment)
    {
        Draft d;
        d.to = {{QString(), QStringLiteral("bob@x.test")}};
        d.subject = QStringLiteral("plans");
        d.text = text;
        d.attachments = {{QStringLiteral("plan.txt"), QStringLiteral("text/plain"), attachment}};
        return mail::Compose::saveDraft(a->ctx(), d);
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        AtRest::setPassphrase(QString());
        m_dir = std::make_unique<QTemporaryDir>();
    }

    void cleanup()
    {
        AtRest::setPassphrase(QString());
        m_dir.reset();
    }

    void newAccountIsEncrypted()
    {
        AccountManager mgr(m_dir->path());
        Account *a = mgr.create();
        draftWith(a, QString::fromLatin1(secret()), "attached " + secret());
        a->ctx().db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        const QString db = a->dir() + QStringLiteral("/mail.db");
        QVERIFY(!Database::isPlaintext(db));
        // Neither the text nor the attachment, nor the attachment's hash as
        // a file name, anywhere on disk.
        const QByteArray disk = everything(a->dir());
        QVERIFY(!disk.contains(secret()));
        const QString sha = *a->ctx().db.queryText("SELECT blob FROM attachments LIMIT 1");
        QVERIFY(!QFile::exists(a->dir() + QStringLiteral("/blobs/") + sha));
        QCOMPARE(a->ctx().blobs.get(sha), "attached " + secret());
        QCOMPARE(a->ctx().blobs.size(sha), qint64(("attached " + secret()).size()));
        // No keyring here: the key is in key.json, and it says so.
        QCOMPARE(AtRest::guard(a->dir()), AtRest::Guard::None);
        QCOMPARE(QByteArray::fromHex(keyFile(a->dir()).value(QLatin1String("key")).toString().toLatin1()), a->dbKey());
    }

    void wrongKeyFails()
    {
        AccountManager mgr(m_dir->path());
        Account *a = mgr.create();
        const QString path = a->dir() + QStringLiteral("/mail.db");
        QVERIFY_THROWS_EXCEPTION(DbError, Database(path, QByteArray(32, 'x')));
        QVERIFY_THROWS_EXCEPTION(DbError, Database{path});
        Database right(path, a->dbKey());
        QVERIFY(right.userVersion() > 0);
    }

    // What an older version left in the clear is encrypted on first open.
    void upgradeFromPlaintext()
    {
        const QString root = m_dir->path();
        const QString dir = root + QStringLiteral("/accounts/1");
        QDir().mkpath(dir);
        QString sha;
        {
            Database db(dir + QStringLiteral("/mail.db"));
            Schema::migrate(db);
            Config(db).set(cfg::Addr, QStringLiteral("old@x.test"));
            sha = BlobStore(dir + QStringLiteral("/blobs")).put("old attachment " + secret());
            db.run("INSERT INTO messages(message_id, direction, state, subject, from_addr, date, received_at, body_text) "
                   "VALUES('old@x.test', 0, 1, 'old', 'a@x.test', 0, 0, ?)",
                   QString::fromLatin1(secret()));
            const qint64 id = db.lastInsertId();
            db.run("INSERT INTO attachments(msg_id, idx, filename, mime_type, size, blob) VALUES(?, 0, 'a', 'text/plain', 1, ?)",
                   id, sha);
        }
        QVERIFY(everything(dir).contains(secret()));
        QFile accounts(root + QStringLiteral("/accounts/accounts.json"));
        QVERIFY(accounts.open(QIODevice::WriteOnly));
        accounts.write(QJsonDocument(QJsonObject{{QStringLiteral("accounts"), QJsonArray{1}},
                                                 {QStringLiteral("next_id"), 2}})
                           .toJson());
        accounts.close();

        AccountManager mgr(root);
        Account *a = mgr.account(1);
        QVERIFY(a);
        QCOMPARE(a->ctx().config.get(cfg::Addr), QStringLiteral("old@x.test"));
        QCOMPARE(a->ctx().db.queryText("SELECT body_text FROM messages WHERE message_id='old@x.test'").value_or(QString()),
                 QString::fromLatin1(secret()));
        QCOMPARE(a->ctx().blobs.get(sha), "old attachment " + secret());
        a->ctx().db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        QVERIFY(!Database::isPlaintext(dir + QStringLiteral("/mail.db")));
        QVERIFY(!everything(dir).contains(secret()));
        QVERIFY(!QFile::exists(dir + QStringLiteral("/blobs/") + sha));
        QVERIFY(keyFile(dir).value(QLatin1String("blobs")).toBool());
    }

    void blobsAreSealedAndChecked()
    {
        AccountManager mgr(m_dir->path());
        Account *a = mgr.create();
        BlobStore &blobs = a->ctx().blobs;
        const QString name = blobs.put("sealed " + secret());
        QFile f(blobs.path(name));
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.seek(f.size() - 1));
        f.write("!");
        f.close();
        QVERIFY(blobs.get(name).isEmpty());
        // Another account's key does not open it either.
        Account *b = mgr.create();
        const QString other = b->ctx().blobs.put("other " + secret());
        QVERIFY(QFile::copy(b->ctx().blobs.path(other), BlobStore(a->dir() + QStringLiteral("/blobs"), a->dbKey()).path(other)));
        QVERIFY(blobs.get(other).isEmpty());
    }

    // A copy for a backup or a new device carries a key of its own.
    void snapshotHasItsOwnKey()
    {
        AccountManager mgr(m_dir->path());
        Account *a = mgr.create();
        draftWith(a, QStringLiteral("snap"), "x");
        const QString path = m_dir->path() + QStringLiteral("/snap.db");
        const QByteArray one = QByteArray(32, 'k');
        snapshotDatabase(a->ctx().db, path, a->dbKey(), one);
        QVERIFY(!Database::isPlaintext(path));
        QVERIFY_THROWS_EXCEPTION(DbError, Database(path, a->dbKey()));
        Database snap(path, one);
        QCOMPARE(snap.queryInt("SELECT count(*) FROM messages WHERE body_text='snap'").value_or(0), 1);
    }

    void passphraseGuards()
    {
        AtRest::setPassphrase(QStringLiteral("correct horse"));
        QString dir;
        QByteArray key;
        {
            AccountManager mgr(m_dir->path());
            Account *a = mgr.create();
            dir = a->dir();
            key = a->dbKey();
        }
        QCOMPARE(AtRest::guard(dir), AtRest::Guard::Passphrase);
        QVERIFY(!keyFile(dir).contains(QLatin1String("key")));
        QVERIFY(!everything(dir).contains(key.toHex()));
        QVERIFY(AtRest::passphraseRequired(accounts()));
        QVERIFY(AtRest::checkPassphrase(accounts(), QStringLiteral("correct horse")));
        QVERIFY(!AtRest::checkPassphrase(accounts(), QStringLiteral("wrong horse")));

        // A new start without it: the account does not open.
        AtRest::setPassphrase(QString());
        QVERIFY_THROWS_EXCEPTION(AtRestError, AtRest::open(dir, 1));
        AtRest::setPassphrase(QStringLiteral("wrong horse"));
        QVERIFY_THROWS_EXCEPTION(AtRestError, AtRest::open(dir, 1));
        AtRest::setPassphrase(QStringLiteral("correct horse"));
        QCOMPARE(AtRest::open(dir, 1), key);

        // Removing it: the same key, guarded as the machine allows (here,
        // with no keyring, by nothing).
        AtRest::setPassphrase(QString());
        AtRest::reguard(dir, 1, key);
        QCOMPARE(AtRest::guard(dir), AtRest::Guard::None);
        QVERIFY(!AtRest::passphraseRequired(accounts()));
        QCOMPARE(AtRest::open(dir, 1), key);
    }
};

QTEST_GUILESS_MAIN(TestAtRest)
#include "tst_atrest.moc"
