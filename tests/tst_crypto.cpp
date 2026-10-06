// SPDX-License-Identifier: MPL-2.0
#include "crypto/Autocrypt.h"
#include "crypto/Pgp.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace e3::crypto;

class TestCrypto : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void roundTripBetweenTwoKeyrings()
    {
        Pgp alice, bob;
        const QString a = alice.generate(QStringLiteral("<alice@x.test>"));
        const QString b = bob.generate(QStringLiteral("<bob@x.test>"));
        QCOMPARE(a.size(), 40);
        QVERIFY(alice.hasSecret(a));

        // Exchange the minimal Autocrypt keys, as headers would.
        const QByteArray header = Autocrypt::format(QStringLiteral("bob@x.test"),
                                                    bob.exportAutocrypt(b, QStringLiteral("bob@x.test")), true);
        const auto parsed = Autocrypt::select({header}, QStringLiteral("Bob@X.test"));
        QVERIFY(parsed.has_value());
        QVERIFY(parsed->preferEncrypt);
        QCOMPARE(alice.import(parsed->keydata), QStringList{b});
        QVERIFY(alice.canEncryptTo(b));
        QVERIFY(!alice.hasSecret(b));
        bob.import(alice.exportPublic(a));

        const QByteArray secret = "Subject: inner\r\n\r\nthe plan\r\n";
        const QByteArray armored = alice.encrypt(secret, {b, a}, a);
        QVERIFY(armored.startsWith("-----BEGIN PGP MESSAGE-----"));
        QVERIFY(!armored.contains("the plan"));

        const Pgp::Decrypted d = bob.decrypt(armored);
        QCOMPARE(d.data, secret);
        QVERIFY(d.encrypted);
        QCOMPARE(d.validSigners, QStringList{a});
        // the sender's copy decrypts for the sender too
        QCOMPARE(alice.decrypt(armored).data, secret);
    }

    void cannotDecryptWithoutKey()
    {
        Pgp alice, bob, eve;
        const QString b = bob.generate(QStringLiteral("<bob@x.test>"));
        alice.import(bob.exportPublic(b));
        const QByteArray armored = alice.encrypt("x", {b}, QString());
        QVERIFY_THROWS_EXCEPTION(PgpError, eve.decrypt(armored));
    }

    void unknownSignerIsNotValid()
    {
        Pgp alice, bob;
        const QString a = alice.generate(QStringLiteral("<alice@x.test>"));
        const QString b = bob.generate(QStringLiteral("<bob@x.test>"));
        alice.import(bob.exportPublic(b));
        // bob never learned alice's key, so her signature cannot count
        const Pgp::Decrypted d = bob.decrypt(alice.encrypt("hello", {b}, a));
        QCOMPARE(d.data, QByteArray("hello"));
        QVERIFY(d.validSigners.isEmpty());
    }

    void autocryptRejectsCriticalUnknownAndDuplicates()
    {
        QVERIFY(!Autocrypt::parse("addr=a@x; keydata=AAAA; unknown=1").has_value());
        QVERIFY(Autocrypt::parse("addr=a@x; _ignored=1; keydata=AAAA").has_value());
        QVERIFY(!Autocrypt::parse("addr=a@x").has_value());
        QVERIFY(!Autocrypt::select({"addr=a@x; keydata=AAAA", "addr=a@x; keydata=BBBB"}, QStringLiteral("a@x")));
        QVERIFY(!Autocrypt::select({"addr=b@x; keydata=AAAA"}, QStringLiteral("a@x")));
    }

    // A second OpenPGP implementation must read what we write.
    void gnupgDecryptsOurMessage()
    {
        const QString gpg = QStandardPaths::findExecutable(QStringLiteral("gpg"));
        if (gpg.isEmpty())
            QSKIP("gpg not installed");
        QTemporaryDir home;
        // Our keys are unprotected, so no passphrase options. The home
        // directory is given relative to gpg's working directory: the MSYS
        // gpg that Git for Windows ships reads "C:\..." as a relative path.
        auto run = [&](const QStringList &args, const QByteArray &input = {}) {
            QProcess p;
            p.setWorkingDirectory(home.path());
            p.start(gpg, QStringList{QStringLiteral("--homedir"), QStringLiteral("."), QStringLiteral("--batch"),
                                     QStringLiteral("--yes")}
                        + args);
            p.write(input);
            p.closeWriteChannel();
            p.waitForFinished(60000);
            const QByteArray out = p.readAllStandardOutput() + p.readAllStandardError();
            if (p.exitCode() != 0)
                qWarning("gpg %s exited %d: %s", qPrintable(args.join(u' ')), p.exitCode(), out.constData());
            return std::make_pair(p.exitCode(), out);
        };
        Pgp us, them;
        const QString ours = us.generate(QStringLiteral("<us@x.test>"));
        const QString theirs = them.generate(QStringLiteral("<them@x.test>"));
        us.import(them.exportPublic(theirs));
        QCOMPARE(run({QStringLiteral("--import")}, them.exportSecret(theirs)).first, 0);
        QCOMPARE(run({QStringLiteral("--import")}, us.exportPublic(ours)).first, 0);
        const auto [code, out] = run({QStringLiteral("--status-fd"), QStringLiteral("2"), QStringLiteral("--decrypt")},
                                     us.encrypt("interop body", {theirs}, ours));
        QVERIFY2(code == 0, out.constData());
        QVERIFY(out.contains("interop body"));
        QVERIFY(out.contains("DECRYPTION_OKAY"));
        QVERIFY(out.contains("VALIDSIG"));
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "tst_crypto.moc"
