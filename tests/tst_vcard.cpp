// SPDX-License-Identifier: MPL-2.0
#include "mail/ContactsCsv.h"
#include "mail/VCard.h"

#include <QtTest>

using namespace e3;
using mail::VCard;

class TestVCard : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsVersion3()
    {
        const auto cards = VCard::parse("BEGIN:VCARD\r\nVERSION:3.0\r\n"
                                        "N:Lovelace;Ada;King;Countess;\r\n"
                                        "FN:Ada Lovelace\r\n"
                                        "ORG:Analytical Engines\\, Ltd;Research\r\n"
                                        "TITLE:Programmer\r\n"
                                        "EMAIL;TYPE=INTERNET,WORK:Ada@Example.ORG\r\n"
                                        "item1.EMAIL;type=INTERNET:ada.home@example.org\r\n"
                                        "TEL;TYPE=CELL:+44 20 7946 0000\r\n"
                                        "TEL;TYPE=WORK,VOICE:+44 20 7946 0001\r\n"
                                        "TEL:+44 20 7946 0002\r\n"
                                        "BDAY:1815-12-10\r\n"
                                        "NOTE:First line\\nsecond\\; with semicolon\r\n"
                                        "KEY;TYPE=PGP:aGVsbG8=\r\n"
                                        "END:VCARD\r\n");
        QCOMPARE(cards.size(), 1);
        const VCard::Card &c = cards.first();
        QCOMPARE(c.name, QStringLiteral("Ada Lovelace"));
        QCOMPARE(c.emails, (QStringList{QStringLiteral("ada@example.org"), QStringLiteral("ada.home@example.org")}));
        QCOMPARE(c.organization, QStringLiteral("Analytical Engines, Ltd, Research"));
        QCOMPARE(c.title, QStringLiteral("Programmer"));
        QCOMPARE(c.phones.size(), 3);
        QCOMPARE(c.phones[0], (ContactPhone{QStringLiteral("mobile"), QStringLiteral("+44 20 7946 0000")}));
        QCOMPARE(c.phones[1].label, QStringLiteral("work"));
        QCOMPARE(c.phones[2].label, QStringLiteral("other"));
        QCOMPARE(c.birthday, QStringLiteral("1815-12-10"));
        QCOMPARE(c.notes, QStringLiteral("First line\nsecond; with semicolon"));
    }

    void readsVersion21()
    {
        // Folded lines, bare TYPE parameters, quoted-printable in a charset,
        // and a name only in N.
        const auto cards = VCard::parse("BEGIN:VCARD\nVERSION:2.1\n"
                                        "N;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:M=FCller;J=FCrgen\n"
                                        "EMAIL;INTERNET;PREF:juergen@example.de\n"
                                        "TEL;CELL;VOICE:0170 1234\n"
                                        "NOTE;ENCODING=QUOTED-PRINTABLE;CHARSET=UTF-8:Gr=C3=BC=C3=9Fe aus =\n"
                                        "Berlin\n"
                                        "ADR;HOME:;;Street 1;Berlin;;10115;\n"
                                        "  Germany\n"
                                        "END:VCARD\n");
        QCOMPARE(cards.size(), 1);
        QCOMPARE(cards[0].name, QStringLiteral("Jürgen Müller"));
        QCOMPARE(cards[0].emails, QStringList{QStringLiteral("juergen@example.de")});
        QCOMPARE(cards[0].phones.value(0).label, QStringLiteral("mobile"));
        QCOMPARE(cards[0].notes, QStringLiteral("Grüße aus Berlin"));
    }

    void readsVersion4()
    {
        const auto cards = VCard::parse("BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Grace\r\n"
                                        "EMAIL;PREF=1:mailto:grace@navy.example\r\n"
                                        "TEL;VALUE=uri;TYPE=\"voice,home\":tel:+1-555-0100\r\n"
                                        "BDAY:--1209\r\nEND:VCARD\r\n"
                                        // A card without an address is still read; the importer skips it.
                                        "BEGIN:VCARD\r\nVERSION:4.0\r\nFN:No Mail\r\nEND:VCARD\r\n"
                                        // Garbage between cards is ignored.
                                        "this is not a property\r\n");
        QCOMPARE(cards.size(), 2);
        QCOMPARE(cards[0].emails, QStringList{QStringLiteral("grace@navy.example")});
        QCOMPARE(cards[0].phones.value(0), (ContactPhone{QStringLiteral("home"), QStringLiteral("+1-555-0100")}));
        QCOMPARE(cards[0].birthday, QStringLiteral("--12-09"));
        QVERIFY(cards[1].emails.isEmpty());
    }

    void birthdays()
    {
        QCOMPARE(VCard::normalizeBirthday(QStringLiteral("19900131")), QStringLiteral("1990-01-31"));
        QCOMPARE(VCard::normalizeBirthday(QStringLiteral("1990-01-31T00:00:00Z")), QStringLiteral("1990-01-31"));
        QCOMPARE(VCard::normalizeBirthday(QStringLiteral("--02-29")), QStringLiteral("--02-29"));
        QVERIFY(VCard::normalizeBirthday(QStringLiteral("1990-02-30")).isEmpty());
        QVERIFY(VCard::normalizeBirthday(QStringLiteral("next tuesday")).isEmpty());
    }

    void roundTrip()
    {
        ContactInfo c;
        c.addr = QStringLiteral("ada@example.org");
        c.name = QStringLiteral("Ada, Countess of Lovelace; née Byron");
        c.organization = QStringLiteral("Analytical Engines");
        c.title = QStringLiteral("Programmer");
        c.notes = QStringLiteral("Line one\nLine two with a long tail that goes well past seventy-five octets: "
                                 "ünïcödé çhäräctërs included so the fold must not split one.");
        c.birthday = QStringLiteral("--12-10");
        c.phones = {{QStringLiteral("mobile"), QStringLiteral("+44 1")}, {QStringLiteral("other"), QStringLiteral("+44 2")}};
        const QByteArray vcf = VCard::emit({c});
        for (const QByteArray &line : vcf.split('\n'))
            QVERIFY2(line.size() <= 76, line.constData()); // 75 octets plus the CR
        QVERIFY(QString::fromUtf8(vcf).isValidUtf16());
        QVERIFY(vcf.contains("VERSION:4.0\r\n"));
        QVERIFY(!vcf.contains("KEY"));
        const auto back = VCard::parse(vcf);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].name, c.name);
        QCOMPARE(back[0].emails, QStringList{c.addr});
        QCOMPARE(back[0].organization, c.organization);
        QCOMPARE(back[0].title, c.title);
        QCOMPARE(back[0].notes, c.notes);
        QCOMPARE(back[0].birthday, c.birthday);
        QCOMPARE(back[0].phones, c.phones);
    }

    void csv()
    {
        ContactInfo a;
        a.addr = QStringLiteral("ada@example.org");
        a.name = QStringLiteral("Lovelace, Ada \"The Countess\"");
        a.notes = QStringLiteral("two\nlines");
        a.birthday = QStringLiteral("1815-12-10");
        a.phones = {{QStringLiteral("mobile"), QStringLiteral("+44 1")},
                    {QStringLiteral("mobile"), QStringLiteral("+44 2")},
                    {QStringLiteral("work"), QStringLiteral("+44 3")},
                    {QStringLiteral("voice"), QStringLiteral("+44 4")}};
        ContactInfo b;
        b.addr = QStringLiteral("evil@x.test");
        b.name = QStringLiteral("=HYPERLINK(\"http://x.test\")");
        b.organization = QStringLiteral("Zürich AG");
        b.title = QStringLiteral("@SUM(A1)");

        const QByteArray out = mail::ContactsCsv::emit({a, b});
        QVERIFY(out.startsWith("\xEF\xBB\xBFName,Email,Organization,Title,Mobile Phone,Work Phone,Home Phone,"
                               "Other Phone,Birthday,Notes\r\n"));
        QVERIFY(out.contains("\"Lovelace, Ada \"\"The Countess\"\"\",ada@example.org,,,+44 1 ; +44 2,+44 3,,+44 4,"
                             "1815-12-10,\"two\nlines\"\r\n"));
        // A formula is text, quoted because it holds quotes; so is a leading minus.
        QVERIFY(out.contains("\"'=HYPERLINK(\"\"http://x.test\"\")\",evil@x.test,Zürich AG,'@SUM(A1),"));
        // A phone number is left alone.
        QVERIFY(out.contains(",+44 3,"));
        QVERIFY(out.endsWith(",,,,,,\r\n"));
        QCOMPARE(mail::ContactsCsv::emit({}).count("\r\n"), 1);
    }

    void singleCard()
    {
        ContactInfo c;
        c.addr = QStringLiteral("ada@example.org");
        c.name = QStringLiteral("Ada");
        const QByteArray vcf = VCard::emit({c});
        QCOMPARE(vcf.count("BEGIN:VCARD"), 1);
        QCOMPARE(VCard::parse(vcf).value(0).emails, QStringList{c.addr});
    }
};

QTEST_GUILESS_MAIN(TestVCard)
#include "tst_vcard.moc"
