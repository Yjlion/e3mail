// SPDX-License-Identifier: MPL-2.0
#include "DeviceClone.h"
#include "FakeCloud.h"
#include "FakeOAuthServer.h"
#include "backup/Remote.h"
#include "backup/S3.h"
#include "backup/WebDav.h"
#include "FakeMailServer.h"
#include "backup/Crypto.h"
#include "backup/Repository.h"
#include "engine/AccountManager.h"
#include "engine/Backups.h"
#include "engine/MailWorker.h"
#include "mail/Compose.h"
#include "mail/Search.h"
#include "store/Config.h"
#include "store/Database.h"
#include "store/OpLog.h"

#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

using namespace e3;

namespace {

// Light enough for tests; real repositories use the defaults (64 MiB).
backup::crypto::KdfParams light()
{
    backup::crypto::KdfParams p;
    p.memoryKiB = 1024;
    p.iterations = 1;
    return p;
}

QStringList files(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        out.append(it.next());
    return out;
}

} // namespace

class TestBackup : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeMailServer> m_srv;
    std::unique_ptr<AccountManager> m_mgr;
    Account *m_a = nullptr;
    QString m_repo;

    QJsonObject folder() const { return {{QStringLiteral("type"), QStringLiteral("folder")}, {QStringLiteral("path"), m_repo}}; }

    void sync(Account *a) { MailWorker::runOnce(a->dir(), a->credentials()); }

    void write(const QString &subject, const QString &text)
    {
        Draft d;
        d.to = {{QString(), QStringLiteral("alice@x.test")}};
        d.subject = subject;
        d.text = text;
        mail::Compose::queue(m_a->ctx(), d);
        sync(m_a);
        sync(m_a);
    }

    static qint64 idOf(Account *a, const QString &subject)
    {
        // Strangers' mail waits in Unverified, outside All mail.
        for (const QString &t : {tag::All, tag::Unverified})
            for (const auto &m : mail::Search::list(a->ctx(), SearchQuery::forTag(t)))
                if (m.subject == subject)
                    return m.id;
        return 0;
    }

    backups::Outcome backUp(Account *a)
    {
        backups::Outcome failed;
        failed.error = QStringLiteral("timed out");
        auto f = backups::run(*a);
        // The fakes answer on this thread: keep it running meanwhile.
        QDeadlineTimer deadline(60000);
        while (!f.isFinished() && !deadline.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!f.isFinished())
            return failed;
        const backups::Outcome o = f.result();
        backups::record(*a, o);
        return o;
    }

private Q_SLOTS:
    void init()
    {
        qputenv("E3MAIL_NO_KEYCHAIN", "1");
        m_dir = std::make_unique<QTemporaryDir>();
        m_srv = std::make_unique<FakeMailServer>();
        m_mgr = std::make_unique<AccountManager>(m_dir->path() + QStringLiteral("/A"));
        m_repo = m_dir->path() + QStringLiteral("/repo");
        m_srv->addUser(QStringLiteral("alice@x.test"), QStringLiteral("pw"));
        m_a = m_mgr->create();
        Account::Settings s;
        s.addr = QStringLiteral("alice@x.test");
        s.protocol = QStringLiteral("imap");
        s.inHost = s.smtpHost = QStringLiteral("127.0.0.1");
        s.inPort = m_srv->imapPort();
        s.smtpPort = m_srv->smtpPort();
        s.inSecurity = s.smtpSecurity = QStringLiteral("plain");
        s.inPassword = QStringLiteral("pw");
        m_a->configure(s);
        m_a->ctx().config.set(cfg::DeviceName, QStringLiteral("Laptop"));
        // Mail to oneself is encrypted (to one's own key).
        write(QStringLiteral("a note to self"), QStringLiteral("the secret plan"));
        QVERIFY(idOf(m_a, QStringLiteral("a note to self")));
    }

    void cleanup()
    {
        m_mgr.reset();
        m_srv.reset();
        m_dir.reset();
    }

    // Backed up, then restored on a new device: the mail, readable, the
    // passwords, and a device of its own.
    void roundTrip()
    {
        backups::setUp(*m_a, folder(), QString(), "correct horse battery staple", light());
        const backups::Outcome o = backUp(m_a);
        QVERIFY2(o.ok, qPrintable(o.error));
        QVERIFY(o.result.uploaded > 0);
        QVERIFY(!m_a->ctx().config.get(cfg::BackupLast).isEmpty());

        backup::FolderDestination dest(m_repo);
        AccountManager mgrB(m_dir->path() + QStringLiteral("/B"));
        int last = 0;
        Account *b = backups::restore(mgrB, dest, "correct horse battery staple", o.result.snapshot,
                                      QStringLiteral("Phone"), [&last](int p) { last = p; });
        QVERIFY(b);
        QCOMPARE(last, 100);
        QCOMPARE(b->addr(), QStringLiteral("alice@x.test"));
        QCOMPARE(b->settings().inPassword, QStringLiteral("pw"));
        QVERIFY(b->ctx().ops.deviceId() != m_a->ctx().ops.deviceId());
        QCOMPARE(b->ctx().config.get(cfg::DeviceName), QStringLiteral("Phone"));
        QVERIFY(b->ctx().config.get(cfg::P2pSecret).isEmpty() || b->ctx().config.get(cfg::P2pSecret) != m_a->ctx().config.get(cfg::P2pSecret));
        QCOMPARE(b->ctx().db.queryInt("SELECT count(*) FROM devices").value_or(-1), 0);
        const qint64 id = idOf(b, QStringLiteral("a note to self"));
        QVERIFY(id);
        QCOMPARE(mail::Search::detail(b->ctx(), id)->bodyText.trimmed(), QStringLiteral("the secret plan"));
        // And it works on its own: it fetches and sends.
        m_srv->deliver(QStringLiteral("alice@x.test"), "From: alice@x.test\r\nTo: alice@x.test\r\nSubject: after the backup\r\n\r\nx\r\n");
        Draft d;
        d.to = {{QString(), QStringLiteral("carol@x.test")}};
        d.subject = QStringLiteral("from the restored device");
        mail::Compose::queue(b->ctx(), d);
        const auto sentBefore = m_srv->envelopes().size();
        sync(b);
        QVERIFY(idOf(b, QStringLiteral("after the backup")));
        QCOMPARE(m_srv->envelopes().size(), sentBefore + 1);
    }

    // The second backup sends only what changed.
    void incremental()
    {
        backups::setUp(*m_a, folder(), QString(), "pass", light());
        const auto first = backUp(m_a);
        QVERIFY(first.ok);
        write(QStringLiteral("one more"), QStringLiteral("y"));
        const auto second = backUp(m_a);
        QVERIFY2(second.ok, qPrintable(second.error));
        // The new message (and the database, which changed); every old blob
        // is reused.
        QVERIFY2(second.result.uploaded <= 3, qPrintable(QString::number(second.result.uploaded)));
        QVERIFY(second.result.reused >= first.result.uploaded - 1);
        // One a day is kept: the earlier one of today went with the prune,
        // its objects stay for a day in case a backup elsewhere needs them.
        backup::FolderDestination dest(m_repo);
        backup::Repository repo(dest);
        repo.unlock("pass");
        QCOMPARE(repo.snapshots().size(), 1);
        QCOMPARE(repo.snapshots().first().name, second.result.snapshot);
        repo.verify(second.result.snapshot, 1000);
    }

    // The destination learns nothing from names: no blob's hash appears.
    void namesHideContent()
    {
        backups::setUp(*m_a, folder(), QString(), "pass", light());
        QVERIFY(backUp(m_a).ok);
        const QStringList blobs = QDir(m_a->dir() + QStringLiteral("/blobs")).entryList(QDir::Files);
        QVERIFY(!blobs.isEmpty());
        const QString all = files(m_repo).join(u'\n');
        for (const QString &b : blobs)
            QVERIFY2(!all.contains(b), qPrintable(b));
        // Nor the address, nor the text, in any file.
        for (const QString &f : files(m_repo)) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray data = file.readAll();
            QVERIFY(!data.contains("alice@x.test"));
            QVERIFY(!data.contains("the secret plan"));
        }
    }

    void refusesWrongOrDamaged()
    {
        backups::setUp(*m_a, folder(), QString(), "right", light());
        const auto o = backUp(m_a);
        QVERIFY(o.ok);
        backup::FolderDestination dest(m_repo);
        backup::Repository repo(dest);
        QVERIFY_THROWS_EXCEPTION(backup::BadPassphrase, repo.unlock("wrong"));
        repo.unlock("right");
        repo.verify(o.result.snapshot);

        // One byte changed in one object.
        const QStringList objects = QDir(m_repo + QStringLiteral("/objects")).entryList(QDir::Files);
        QVERIFY(objects.size() >= 2);
        QFile f(m_repo + QStringLiteral("/objects/") + objects[0]);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QByteArray data = f.readAll();
        data[data.size() / 2] = char(data[data.size() / 2] ^ 1);
        f.seek(0);
        f.write(data);
        f.close();
        QVERIFY_THROWS_EXCEPTION(backup::Damaged, repo.verify(o.result.snapshot, 1000));
        AccountManager mgrB(m_dir->path() + QStringLiteral("/B"));
        QVERIFY_THROWS_EXCEPTION(backup::Damaged,
                                 backups::restore(mgrB, dest, "right", o.result.snapshot, QStringLiteral("x")));
        QVERIFY(mgrB.accounts().isEmpty());
        QVERIFY(QDir(mgrB.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());

        // An intact object put in another's place: it is bound to its name.
        QFile::remove(f.fileName());
        QVERIFY(QFile::copy(m_repo + QStringLiteral("/objects/") + objects[1], f.fileName()));
        QVERIFY_THROWS_EXCEPTION(backup::Damaged, repo.verify(o.result.snapshot, 1000));
    }

    // Retention per device, and only unneeded objects past the grace period
    // go.
    void prune()
    {
        backups::setUp(*m_a, folder(), QString(), "pass", light());
        backup::FolderDestination dest(m_repo);
        backup::Repository repo(dest);
        repo.unlock("pass");
        const QDateTime now = QDateTime::currentDateTimeUtc();
        backup::Contents about;
        about.device = m_a->ctx().ops.deviceId();
        // A backup a day for 40 days.
        for (int day = 40; day >= 0; --day) {
            about.created = now.addDays(-day);
            repo.backup(m_a->ctx().db, m_a->dir() + QStringLiteral("/blobs"), about);
        }
        QCOMPARE(repo.snapshots().size(), 41);
        // An object nothing needs, old, and one nothing needs yet, new.
        dest.put(QStringLiteral("objects/aaaa"), "old");
        dest.put(QStringLiteral("objects/bbbb"), "new");
        QFile old(m_repo + QStringLiteral("/objects/aaaa"));
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(now.addDays(-3), QFileDevice::FileModificationTime));
        old.close();

        const int removed = repo.prune({}, now);
        const QList<backup::Snapshot> left = repo.snapshots();
        // 7 days, then weekly and monthly ones among the 41.
        QVERIFY2(left.size() >= 7 && left.size() <= 7 + 4 + 6, qPrintable(QString::number(left.size())));
        QCOMPARE(left.first().created.date(), now.date()); // the newest stays
        QVERIFY(removed >= 1);
        QVERIFY(!dest.get(QStringLiteral("objects/aaaa")));
        QVERIFY(dest.get(QStringLiteral("objects/bbbb")));
        // What is left still restores.
        repo.verify(left.last().name, 1000);
        repo.verify(left.first().name, 1000);
    }

    // Two devices of one account back up into one repository.
    void twoDevices()
    {
        backups::setUp(*m_a, folder(), QString(), "shared", light());
        QVERIFY(cloneDevice(m_a, m_dir->path() + QStringLiteral("/C")));
        AccountManager mgrC(m_dir->path() + QStringLiteral("/C"));
        Account *c = mgrC.account(m_a->id());
        QVERIFY(c);
        QVERIFY(!backups::isSetUp(*c)); // device-only: nothing came along
        QVERIFY_THROWS_EXCEPTION(backup::BadPassphrase, backups::setUp(*c, folder(), QString(), "other", light()));
        backups::setUp(*c, folder(), QString(), "shared", light());
        QVERIFY(backUp(m_a).ok);
        QVERIFY(backUp(c).ok);
        backup::FolderDestination dest(m_repo);
        backup::Repository repo(dest);
        repo.unlock("shared");
        const auto snaps = repo.snapshots();
        QCOMPARE(snaps.size(), 2);
        QVERIFY(snaps[0].device != snaps[1].device);
    }

    // Google Drive and OneDrive, against fakes of their APIs: a whole backup
    // and restore, a message large enough for OneDrive's upload sessions,
    // listings over several pages, and an access token that runs out.
    void cloud_data()
    {
        QTest::addColumn<QString>("type");
        QTest::newRow("google drive") << QStringLiteral("gdrive");
        QTest::newRow("onedrive") << QStringLiteral("onedrive");
    }
    void cloud()
    {
        QFETCH(QString, type);
        FakeOAuthServer provider;
        FakeCloud cloud;
        provider.issued = [&cloud](const QString &, const QString &access) { cloud.tokens.insert(access); };
        provider.addRefreshToken(QStringLiteral("backup-refresh"), QStringLiteral("alice@x.test"));
        // 5 MiB that do not compress.
        QByteArray big(5 << 20, Qt::Uninitialized);
        QRandomGenerator(1).fillRange(reinterpret_cast<quint32 *>(big.data()), big.size() / 4);
        QByteArray body;
        const QByteArray b64 = big.toBase64();
        for (qsizetype i = 0; i < b64.size(); i += 76)
            body += b64.mid(i, 76) + "\r\n";
        m_srv->deliver(QStringLiteral("alice@x.test"),
                       "From: x@x.test\r\nSubject: big\r\nContent-Type: application/octet-stream\r\n"
                       "Content-Transfer-Encoding: base64\r\n\r\n" + body);
        sync(m_a);
        QVERIFY(idOf(m_a, QStringLiteral("big")));

        const QJsonObject c{{QStringLiteral("type"), type},
                            {QStringLiteral("api"), type == QLatin1String("gdrive") ? cloud.base() : cloud.base() + QStringLiteral("/v1.0")},
                            {QStringLiteral("clientId"), QStringLiteral("test-client")},
                            {QStringLiteral("tokenUrl"), provider.provider().tokenUrl.toString()}};
        backups::setUp(*m_a, c, QStringLiteral("backup-refresh"), "cloudy", light());
        cloud.tokens.clear(); // the access token runs out: renewed once
        const auto o = backUp(m_a);
        QVERIFY2(o.ok, qPrintable(o.error));
        QVERIFY(provider.refreshes >= 2);
        if (type == QLatin1String("onedrive"))
            QVERIFY(cloud.uploadSessions > 0);
        else
            QVERIFY(cloud.drive.size() > 4);

        auto dest = backups::destination(c, QStringLiteral("backup-refresh"));
        AccountManager mgrB(m_dir->path() + QStringLiteral("/B"));
        Account *b = backups::restore(mgrB, *dest, "cloudy", o.result.snapshot, QStringLiteral("Phone"));
        QVERIFY(b);
        QVERIFY(idOf(b, QStringLiteral("big")));
        QVERIFY(mail::Search::detail(b->ctx(), idOf(b, QStringLiteral("a note to self"))));
    }

    // AWS's Signature Version 4 test suite: get-vanilla and
    // get-vanilla-query-order-key-case.
    void sigv4()
    {
        const QDateTime when = QDateTime::fromString(QStringLiteral("20150830T123600Z"), QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
        QDateTime utc = when;
        utc.setTimeZone(QTimeZone::UTC);
        const QByteArray empty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        const QList<QPair<QByteArray, QByteArray>> h = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", "20150830T123600Z"}};
        const QString key = QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
        QCOMPARE(backup::S3Destination::authorization("GET", QUrl(QStringLiteral("https://example.amazonaws.com/")), h, empty, utc,
                                                      QStringLiteral("us-east-1"), QStringLiteral("service"),
                                                      QStringLiteral("AKIDEXAMPLE"), key),
                 QByteArray("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
                            "SignedHeaders=host;x-amz-date, "
                            "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"));
        QCOMPARE(backup::S3Destination::authorization("GET", QUrl(QStringLiteral("https://example.amazonaws.com/?Param2=value2&Param1=value1")),
                                                      h, empty, utc, QStringLiteral("us-east-1"), QStringLiteral("service"),
                                                      QStringLiteral("AKIDEXAMPLE"), key),
                 QByteArray("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
                            "SignedHeaders=host;x-amz-date, "
                            "Signature=b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500"));
    }

    // A PROPFIND answer as Nextcloud gives it.
    void webdavListing()
    {
        const QByteArray xml =
            "<?xml version=\"1.0\"?>\n"
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\">\n"
            " <d:response><d:href>/remote.php/dav/files/me/e3/objects/</d:href>\n"
            "  <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>\n"
            " <d:response><d:href>/remote.php/dav/files/me/e3/objects/ab%20cd</d:href>\n"
            "  <d:propstat><d:prop><d:getcontentlength>42</d:getcontentlength>\n"
            "  <d:getlastmodified>Fri, 16 Oct 2026 09:00:00 GMT</d:getlastmodified><d:resourcetype/></d:prop></d:propstat></d:response>\n"
            "</d:multistatus>";
        const auto entries = backup::WebDavDestination::parseMultistatus(xml);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].name, QStringLiteral("ab cd"));
        QCOMPARE(entries[0].size, 42);
        QCOMPARE(entries[0].modified, QDateTime(QDate(2026, 10, 16), QTime(9, 0), QTimeZone::UTC));
    }

    // Real servers, when E3MAIL_TEST_DESTINATIONS names a JSON file of
    // [{config, secret}] (scripts/backup-e2e.py makes one for
    // server/compose's backup profile).
    void liveDestinations()
    {
        const QString file = qEnvironmentVariable("E3MAIL_TEST_DESTINATIONS");
        if (file.isEmpty())
            QSKIP("E3MAIL_TEST_DESTINATIONS is not set");
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonArray list = QJsonDocument::fromJson(f.readAll()).array();
        QVERIFY(!list.isEmpty());
        int n = 0;
        for (const QJsonValue &v : list) {
            const QJsonObject c = v.toObject().value(QLatin1String("config")).toObject();
            const QString secret = v.toObject().value(QLatin1String("secret")).toString();
            const QString type = c.value(QLatin1String("type")).toString();
            qInfo() << "destination" << type;
            auto dest = backups::destination(c, secret);
            QVERIFY2(dest, qPrintable(type));
            dest->prepare();
            // The basics, then a whole backup and restore.
            const QString name = QStringLiteral("objects/e3test%1").arg(n++);
            dest->put(name, "one");
            dest->put(name, "two"); // replaced
            QCOMPARE(*dest->get(name), QByteArray("two"));
            QVERIFY(!dest->get(QStringLiteral("objects/not-there")));
            bool listed = false;
            for (const backup::Entry &e : dest->list(QStringLiteral("objects")))
                if (e.name == name.mid(8)) {
                    listed = true;
                    QCOMPARE(e.size, 3);
                    QVERIFY(e.modified.isValid());
                }
            QVERIFY2(listed, qPrintable(type));
            dest->remove(name);
            QVERIFY(!dest->get(name));

            backups::forget(*m_a);
            backups::setUp(*m_a, c, secret, "live", light());
            const auto o = backUp(m_a);
            QVERIFY2(o.ok, qPrintable(type + u": " + o.error));
            AccountManager mgr(m_dir->path() + QStringLiteral("/live-") + type);
            Account *b = backups::restore(mgr, *dest, "live", o.result.snapshot, QStringLiteral("Phone"));
            QCOMPARE(mail::Search::detail(b->ctx(), idOf(b, QStringLiteral("a note to self")))->bodyText.trimmed(),
                     QStringLiteral("the secret plan"));
        }
    }

    void cryptoBasics()
    {
        const QByteArray key = backup::crypto::random(32);
        const QByteArray sealed = backup::crypto::seal(key, "hello", "name");
        QCOMPARE(*backup::crypto::open(key, sealed, "name"), QByteArray("hello"));
        QVERIFY(!backup::crypto::open(key, sealed, "other name"));
        QVERIFY(!backup::crypto::open(backup::crypto::random(32), sealed, "name"));
        QVERIFY(backup::crypto::seal(key, "hello", "name") != sealed); // a new nonce each time
        backup::crypto::KdfParams p = light();
        p.salt = QByteArray(16, 'a');
        QCOMPARE(backup::crypto::argon2id("pw", p), backup::crypto::argon2id("pw", p));
        QVERIFY(backup::crypto::argon2id("pw", p) != backup::crypto::argon2id("pW", p));
        // RFC 4231 test case 2.
        QCOMPARE(backup::crypto::hmac("Jefe", "what do ya want for nothing?").toHex(),
                 QByteArray("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    }
};

QTEST_GUILESS_MAIN(TestBackup)
#include "tst_backup.moc"
