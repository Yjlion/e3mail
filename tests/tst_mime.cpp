// SPDX-License-Identifier: MPL-2.0
#include "mime/Builder.h"
#include "mime/Encoding.h"
#include "mime/Headers.h"
#include "mime/Html.h"
#include "mime/Part.h"

#include <QtTest>

using namespace e3::mime;

class TestMime : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void encodedWords()
    {
        QCOMPARE(decodeHeader("=?utf-8?q?Gr=C3=BC=C3=9Fe?= aus =?ISO-8859-1?Q?M=FCnchen?="),
                 QStringLiteral("Grüße aus München"));
        // whitespace between adjacent encoded words disappears
        QCOMPARE(decodeHeader("=?UTF-8?B?SGVs?= =?UTF-8?B?bG8=?="), QStringLiteral("Hello"));
        QCOMPARE(decodeHeader("plain subject"), QStringLiteral("plain subject"));
        QCOMPARE(decodeHeader("=?utf-8?q?a_b?="), QStringLiteral("a b"));
        // raw UTF-8 in a header
        QCOMPARE(decodeHeader(QStringLiteral("Café").toUtf8()), QStringLiteral("Café"));
    }

    void encodeHeaderRoundTrip()
    {
        const QString s = QStringLiteral("Ünïcödé — a fairly long subject line with emoji 🎉 in it");
        const QByteArray enc = encodeHeader(s);
        QVERIFY(isAscii(enc));
        QCOMPARE(decodeHeader(enc), s);
        QCOMPARE(encodeHeader(QStringLiteral("ascii")), QByteArray("ascii"));
    }

    void quotedPrintable()
    {
        QCOMPARE(decodeQuotedPrintable("caf=C3=A9 =\r\nsoft"), QByteArray("caf\xc3\xa9 soft"));
        const QByteArray text = QStringLiteral("Línea con acentos y una línea muy muy muy muy muy muy muy "
                                               "muy muy muy muy larga que debe partirse.\nTrailing space \n")
                                    .toUtf8();
        const QByteArray enc = encodeQuotedPrintable(text);
        for (const QByteArray &line : enc.split('\n'))
            QVERIFY2(line.size() <= 77, line.constData());
        QByteArray expected = text;
        expected.replace("\n", "\r\n");
        QCOMPARE(decodeQuotedPrintable(enc), expected);
    }

    void params()
    {
        const HeaderParams p = parseParams(
            "attachment; filename*0*=UTF-8''%E2%82%AC%20rates; filename*1*=%20.pdf; size=12");
        QCOMPARE(p.value, QByteArray("attachment"));
        QCOMPARE(p.params.value("filename"), QStringLiteral("€ rates .pdf"));
        const HeaderParams q = parseParams("text/plain; charset=\"iso-8859-1\"; format=flowed");
        QCOMPARE(q.value, QByteArray("text/plain"));
        QCOMPARE(q.params.value("charset"), QStringLiteral("iso-8859-1"));
        QCOMPARE(parseParams(formatParam("filename", QStringLiteral("naïve.txt"))
                                 .prepend("attachment; "))
                     .params.value("filename"),
                 QStringLiteral("naïve.txt"));
    }

    void addresses()
    {
        const auto list = parseAddressList(
            "\"Doe, Jane\" <Jane@Example.com>, bob@example.org (Bob B), "
            "=?utf-8?q?J=C3=B6rg?= <jorg@example.de>, Friends: a@x.io, b@y.io;");
        QCOMPARE(list.size(), 5);
        QCOMPARE(list[0].name, QStringLiteral("Doe, Jane"));
        QCOMPARE(list[0].addr, QStringLiteral("Jane@Example.com"));
        QCOMPARE(list[1].addr, QStringLiteral("bob@example.org"));
        QCOMPARE(list[1].name, QStringLiteral("Bob B"));
        QCOMPARE(list[2].name, QStringLiteral("Jörg"));
        QCOMPARE(list[3].addr, QStringLiteral("a@x.io"));
        QCOMPARE(list[4].addr, QStringLiteral("b@y.io"));
        const QByteArray formatted = formatAddressList(list);
        QCOMPARE(parseAddressList(formatted), list);
    }

    void dates()
    {
        const QDateTime d = parseDate("Tue, 1 Jul 2003 10:52:37 +0200");
        QVERIFY(d.isValid());
        QCOMPARE(d.toUTC(), QDateTime(QDate(2003, 7, 1), QTime(8, 52, 37), QTimeZone::UTC));
        QCOMPARE(parseDate("1 Jul 03 10:52 EST (comment)").toUTC(),
                 QDateTime(QDate(2003, 7, 1), QTime(15, 52), QTimeZone::UTC));
        QVERIFY(!parseDate("garbage").isValid());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(parseDate(formatDate(now)).toSecsSinceEpoch(), now.toSecsSinceEpoch());
    }

    void messageIds()
    {
        QCOMPARE(parseMessageIds("<a@b> (c) <c@d>\r\n <a@b>"), (QStringList{"a@b", "c@d"}));
        QCOMPARE(parseMessageIds("bare@id"), QStringList{"bare@id"});
    }

    void parseMultipart()
    {
        const QByteArray raw =
            "From: Ana <ana@example.com>\r\n"
            "Subject: =?utf-8?q?Hi_=F0=9F=91=8B?=\r\n"
            "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
            "\r\n"
            "preamble\r\n"
            "--outer\r\n"
            "Content-Type: multipart/alternative; boundary=inner\r\n"
            "\r\n"
            "--inner\r\n"
            "Content-Type: text/plain; charset=iso-8859-1\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "Ol=E1\r\n"
            "--inner\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<p>Ol&aacute;</p>\r\n"
            "--inner--\r\n"
            "--outer\r\n"
            "Content-Type: application/pdf; name=\"doc.pdf\"\r\n"
            "Content-Disposition: attachment\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "\r\n"
            "JVBERi0=\r\n"
            "--outer--\r\n"
            "epilogue\r\n";
        const Part p = parse(raw);
        QCOMPARE(p.headers.text("Subject"), QStringLiteral("Hi 👋"));
        QCOMPARE(p.mimeType, QByteArray("multipart/mixed"));
        QCOMPARE(p.children.size(), 2);
        const Part &alt = p.children[0];
        QCOMPARE(alt.children.size(), 2);
        QCOMPARE(alt.children[0].text(), QStringLiteral("Olá"));
        QCOMPARE(alt.children[1].mimeType, QByteArray("text/html"));
        QCOMPARE(p.children[1].filename, QStringLiteral("doc.pdf"));
        QCOMPARE(p.children[1].body, QByteArray("%PDF-"));
    }

    void buildAndReparse()
    {
        OutgoingMail m;
        m.from = {QStringLiteral("Zoë"), QStringLiteral("zoe@example.com")};
        m.to = {{QString(), QStringLiteral("a@example.org")}};
        m.cc = {{QStringLiteral("Bé"), QStringLiteral("b@example.org")}};
        m.subject = QStringLiteral("Résumé attached");
        m.date = QDateTime::currentDateTimeUtc();
        m.messageId = generateMessageId(m.from.addr);
        m.inReplyTo = QStringLiteral("parent@example.org");
        m.references = {QStringLiteral("root@example.org"), QStringLiteral("parent@example.org")};
        m.text = QStringLiteral("Hello,\n\nsee attached.\n.\nFrom here on\n");
        m.html = QStringLiteral("<p>Hello,</p><p>see <b>attached</b>.</p>");
        m.importance = 1;
        m.attachments = {{QStringLiteral("cv.txt"), "text/plain", "plain bytes \x01\x02"}};

        const Part p = parse(Builder::build(m));
        QCOMPARE(p.headers.text("Subject"), m.subject);
        QCOMPARE(parseAddressList(p.headers.raw("Cc")), m.cc);
        QCOMPARE(parseMessageIds(p.headers.raw("Message-ID")).value(0), m.messageId);
        QCOMPARE(parseMessageIds(p.headers.raw("References")), m.references);
        QCOMPARE(p.headers.raw("Importance"), QByteArray("high"));
        QCOMPARE(p.mimeType, QByteArray("multipart/mixed"));
        QCOMPARE(p.children[0].children[0].text().replace(QStringLiteral("\r\n"), QStringLiteral("\n")), m.text);
        QCOMPARE(p.children[1].body, m.attachments[0].data);
        QVERIFY(!p.headers.has("Bcc"));
    }

    void sanitizer()
    {
        const SanitizedHtml s = sanitizeHtml(QStringLiteral(
            "<html><head><style>body{background:url(http://t.example/x)}</style></head>"
            "<body onload=\"evil()\"><p style=\"color:red\">Hi <img src=\"https://track.example/p.gif\" alt=\"logo\">"
            "<script>alert(1)</script><a href=\"javascript:alert(2)\">click</a> "
            "<a href=\"https://ok.example/\">ok</a><iframe src=\"http://x\">inside</iframe></p>"
            "<table background=\"http://bg\"><tr><td colspan=2>cell</td></tr></table></body></html>"));
        QVERIFY(!s.html.contains(QLatin1String("script")));
        QVERIFY(!s.html.contains(QLatin1String("alert")));
        QVERIFY(!s.html.contains(QLatin1String("track.example")));
        QVERIFY(!s.html.contains(QLatin1String("javascript")));
        QVERIFY(!s.html.contains(QLatin1String("style")));
        QVERIFY(!s.html.contains(QLatin1String("inside")));
        QVERIFY(!s.html.contains(QLatin1String("onload")));
        QVERIFY(s.html.contains(QLatin1String("<a href=\"https://ok.example/\">ok</a>")));
        QVERIFY(s.html.contains(QLatin1String("[logo]")));
        QVERIFY(s.html.contains(QLatin1String("click")));
        QVERIFY(s.html.contains(QLatin1String("colspan=\"2\"")));
        QCOMPARE(s.remoteBlocked, 3); // img, table background (the style element is dropped with its content)
    }

    void sanitizerEscapesText()
    {
        const SanitizedHtml s = sanitizeHtml(QStringLiteral("a &lt;script&gt; b <unknown attr=1>x</unknown>"));
        QCOMPARE(s.html, QStringLiteral("a &lt;script&gt; b x"));
    }

    void htmlToTextBasics()
    {
        const QString t = htmlToText(QStringLiteral(
            "<p>Hello&nbsp;<b>world</b></p><ul><li>one</li><li>two</li></ul>"
            "<ol><li>first</li></ol><a href=\"https://x.example/\">site</a><style>p{}</style>"));
        QCOMPARE(t, QStringLiteral("Hello world\n\n• one\n• two\n1. first\nsite <https://x.example/>"));
    }
};

QTEST_GUILESS_MAIN(TestMime)
#include "tst_mime.moc"
