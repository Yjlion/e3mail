// SPDX-License-Identifier: MPL-2.0
#include "mail/Ingest.h"
#include "mime/Builder.h"
#include "mime/Encoding.h"
#include "mime/Headers.h"
#include "mime/Html.h"
#include "mime/Part.h"
#include "util/LocalePatterns.h"

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

    // Every charset ICU knows decodes, whatever the Qt build (ADR 0014).
    void charsets_data()
    {
        QTest::addColumn<QByteArray>("charset");
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("text");
        QTest::newRow("iso-2022-jp") << QByteArray("iso-2022-jp") << QByteArray("\x1b\x24\x42\x46\x7c\x4b\x5c\x38\x6c\x24\x4e\x25\x61\x21\x3c\x25\x6b\x1b\x28\x42") << QStringLiteral("日本語のメール");
        QTest::newRow("shift_jis") << QByteArray("shift_jis") << QByteArray("\x93\xfa\x96\x7b\x8c\xea\x82\xcc\x83\x81\x81\x5b\x83\x8b") << QStringLiteral("日本語のメール");
        QTest::newRow("euc-jp") << QByteArray("euc-jp") << QByteArray("\xc6\xfc\xcb\xdc\xb8\xec") << QStringLiteral("日本語");
        QTest::newRow("gb2312") << QByteArray("gb2312") << QByteArray("\xbc\xf2\xcc\xe5\xd6\xd0\xce\xc4\xd3\xca\xbc\xfe") << QStringLiteral("简体中文邮件");
        QTest::newRow("gbk") << QByteArray("gbk") << QByteArray("\xbc\xf2\xcc\xe5\xd6\xd0\xce\xc4\xd3\xca\xbc\xfe") << QStringLiteral("简体中文邮件");
        QTest::newRow("gb18030") << QByteArray("gb18030") << QByteArray("\xbc\xf2\xcc\xe5\xd6\xd0\xce\xc4\xd3\xca\xbc\xfe") << QStringLiteral("简体中文邮件");
        QTest::newRow("big5") << QByteArray("big5") << QByteArray("\xc1\x63\xc5\xe9\xa4\xa4\xa4\xe5") << QStringLiteral("繁體中文");
        QTest::newRow("euc-kr") << QByteArray("euc-kr") << QByteArray("\xc7\xd1\xb1\xb9\xbe\xee") << QStringLiteral("한국어");
        QTest::newRow("koi8-r") << QByteArray("koi8-r") << QByteArray("\xf2\xd5\xd3\xd3\xcb\xcf\xc5\x20\xd0\xc9\xd3\xd8\xcd\xcf") << QStringLiteral("Русское письмо");
        QTest::newRow("windows-1251") << QByteArray("windows-1251") << QByteArray("\xd0\xf3\xf1\xf1\xea\xee\xe5\x20\xef\xe8\xf1\xfc\xec\xee") << QStringLiteral("Русское письмо");
        QTest::newRow("iso-8859-2") << QByteArray("iso-8859-2") << QByteArray("\x5a\x61\xbf\xf3\xb3\xe6\x20\x67\xea\xb6\x6c\xb1\x20\x6a\x61\xbc\xf1") << QStringLiteral("Zażółć gęślą jaźń");
        QTest::newRow("windows-1250") << QByteArray("windows-1250") << QByteArray("\x5a\x61\xbf\xf3\xb3\xe6\x20\x67\xea\x9c\x6c\xb9\x20\x6a\x61\x9f\xf1") << QStringLiteral("Zażółć gęślą jaźń");
        QTest::newRow("iso-8859-8") << QByteArray("iso-8859-8") << QByteArray("\xf9\xec\xe5\xed") << QStringLiteral("שלום");
        QTest::newRow("windows-1255") << QByteArray("windows-1255") << QByteArray("\xf9\xec\xe5\xed") << QStringLiteral("שלום");
        QTest::newRow("windows-1256") << QByteArray("windows-1256") << QByteArray("\xe3\xd1\xcd\xc8\xc7") << QStringLiteral("مرحبا");
        QTest::newRow("iso-8859-6") << QByteArray("iso-8859-6") << QByteArray("\xe5\xd1\xcd\xc8\xc7") << QStringLiteral("مرحبا");
        QTest::newRow("iso-8859-15") << QByteArray("iso-8859-15") << QByteArray("\x50\x72\x69\x78\x20\x3a\x20\x35\x20\xa4") << QStringLiteral("Prix : 5 €");
    }

    void charsets()
    {
        QFETCH(QByteArray, charset);
        QFETCH(QByteArray, bytes);
        QFETCH(QString, text);
        QCOMPARE(decodeCharset(bytes, charset), text);
        QCOMPARE(decodeCharset(bytes, charset.toUpper()), text);
    }

    void mislabelledCharsets()
    {
        const QByteArray utf8 = QStringLiteral("日本語").toUtf8();
        // Declared Shift_JIS, sent as UTF-8.
        QCOMPARE(decodeCharset(utf8, "shift_jis"), QStringLiteral("日本語"));
        // A charset nobody knows.
        QCOMPARE(decodeCharset(utf8, "x-no-such-charset"), QStringLiteral("日本語"));
        // A stateful encoding in an encoded word.
        QCOMPARE(decodeHeader("=?ISO-2022-JP?B?GyRCRnxLXDhsJE43b0w+GyhC?="), QStringLiteral("日本語の件名"));
    }

    void monthDayFollowsLocale()
    {
        const QDate d(2026, 10, 6);
        auto fmt = [&d](const char *name) {
            const QLocale l(QString::fromLatin1(name));
            return l.toString(d, e3::LocalePatterns::monthDay(l));
        };
        QCOMPARE(fmt("en_GB"), QStringLiteral("6 Oct"));
        QCOMPARE(fmt("ja_JP"), QStringLiteral("10月6日"));
        QCOMPARE(fmt("zh_CN"), QStringLiteral("10月6日"));
        QVERIFY(fmt("de_DE").startsWith(QStringLiteral("6. Okt")));
    }

    // A reply's preview stops at the attribution line, whatever language the
    // replier's client wrote it in.
    void previewSkipsQuoteInAnyLanguage_data()
    {
        QTest::addColumn<QString>("attribution");
        QTest::newRow("en") << QStringLiteral("On 6 Oct 2026 at 14:03, Bob wrote:");
        QTest::newRow("de") << QStringLiteral("Am 6. Oktober 2026 um 14:03 schrieb Bob:");
        QTest::newRow("ja") << QStringLiteral("2026年10月6日 14:03、Bob さんは書きました:");
        QTest::newRow("zh") << QStringLiteral("Bob 于 2026年10月6日 14:03 写道\uFF1A");
        QTest::newRow("he") << QStringLiteral("ב־6 באוקטובר 2026 בשעה 14:03, Bob כתב:");
    }

    void previewSkipsQuoteInAnyLanguage()
    {
        QFETCH(QString, attribution);
        const QString body = QStringLiteral("Sounds good.\n\n") + attribution + QStringLiteral("\n> the plan\n> is set\n");
        QCOMPARE(e3::mail::Ingest::makePreview(body), QStringLiteral("Sounds good."));
        // A colon that does not introduce a quote is text.
        QCOMPARE(e3::mail::Ingest::makePreview(QStringLiteral("Agenda:\n1. keys")), QStringLiteral("Agenda: 1. keys"));
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

    // Newsletters lay out with nested tables, which Qt's rich text squeezes
    // into a narrow column: those become blocks. Tables of data stay tables.
    void layoutTablesBecomeBlocks()
    {
        const QString h = sanitizeHtml(QStringLiteral(
                                           "<table role=\"presentation\"><tr><td>hero</td></tr></table>"
                                           "<table width=\"100%\"><tbody><tr><td>outer"
                                           "<table><tr><td>inner layout</td></tr></table>"
                                           "</td></tr></tbody></table>"
                                           "<table><tr><th>Plan</th><td colspan=\"2\">Price</td></tr></table>"))
                              .html;
        QVERIFY2(h.startsWith(QLatin1String("<div><div><div>hero</div></div></div>")), qPrintable(h));
        QVERIFY(!h.contains(QLatin1String("tbody")));
        // The table holding another one is layout and becomes blocks; the
        // inner one holds no table and stays a table.
        QVERIFY(h.contains(QLatin1String("<div><div><div>outer<table><tr><td>inner layout</td></tr></table></div></div></div>")));
        // A table of data, holding no table, stays a table.
        QVERIFY(h.contains(QLatin1String("<table><tr><th>Plan</th><td colspan=\"2\">Price</td></tr></table>")));
    }

    // Images the message carries itself keep a cid: source for the app to
    // resolve; remote images still become their alt text.
    void inlineImagesKeepCid()
    {
        const SanitizedHtml s = sanitizeHtml(QStringLiteral(
            "<img src=\"CID:logo@x\" width=\"1200px\" alt=\"Logo\" onerror=\"evil()\">"
            "<img src=\"cid:a&quot;b\" style=\"x\">"
            "<img src=\"https://t.example/p.gif\" alt=\"pixel\">"
            "<img src=\"data:image/png;base64,AAAA\" alt=\"data\">"));
        QCOMPARE(s.html, QStringLiteral("<img src=\"cid:logo@x\" width=\"600\" alt=\"Logo\">"
                                        "<img src=\"cid:a&quot;b\">[pixel][data]"));
        QCOMPARE(s.remoteBlocked, 1);
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
