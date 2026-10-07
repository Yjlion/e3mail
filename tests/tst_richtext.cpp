// SPDX-License-Identifier: MPL-2.0
#include "RichText.h"

#include <QGuiApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QtTest>

class TestRichText : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void plainStaysPlain()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("Hello <b>not markup</b>\nsecond line"));
        const auto out = RichText::emit(&doc);
        QVERIFY(out.html.isEmpty());
        QCOMPARE(out.text, QStringLiteral("Hello <b>not markup</b>\nsecond line"));
    }

    void formattingIsWhitelisted()
    {
        QTextDocument doc;
        // Whatever the editor or a paste put in the document, only the
        // whitelist comes out.
        doc.setHtml(QStringLiteral(
            "<p style=\"color:red\"><b>bold</b> <i>it</i> <span style=\"font-size:40px\">big</span></p>"
            "<ul><li>one</li><li>two</li></ul>"
            "<p><a href=\"javascript:alert(1)\">evil</a> <a href=\"https://ok.example/\">ok</a></p>"
            "<table><tr><td>cell</td></tr></table><img src=\"https://x/y.png\">"));
        const auto out = RichText::emit(&doc);
        QVERIFY(out.html.contains(QLatin1String("<b>bold</b>")));
        QVERIFY(out.html.contains(QLatin1String("<i>it</i>")));
        QVERIFY(out.html.contains(QLatin1String("<ul><li>one</li><li>two</li></ul>")));
        QVERIFY(out.html.contains(QLatin1String("<a href=\"https://ok.example/\">ok</a>")));
        QVERIFY(!out.html.contains(QLatin1String("javascript")));
        QVERIFY(!out.html.contains(QLatin1String("style")));
        QVERIFY(!out.html.contains(QLatin1String("<img")));
        QVERIFY(!out.html.contains(QLatin1String("<table")));
        QVERIFY(out.html.contains(QLatin1String("evil")));
        QVERIFY2(out.text.contains(QStringLiteral("• one")), qPrintable(out.text));
        QVERIFY(out.text.contains(QLatin1String("ok <https://ok.example/>")));
    }

    void quoteAndCodeRoles()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("quoted\ncode"));
        QTextBlockFormat q;
        q.setProperty(RichText::RoleProperty, int(RichText::Quote));
        QTextCursor(doc.begin()).setBlockFormat(q);
        QTextBlockFormat c;
        c.setProperty(RichText::RoleProperty, int(RichText::Code));
        QTextCursor(doc.begin().next()).setBlockFormat(c);
        const auto out = RichText::emit(&doc);
        QVERIFY(out.html.contains(QLatin1String("<blockquote><p>quoted</p></blockquote>")));
        QVERIFY(out.html.contains(QLatin1String("<pre><code>code")));
        QCOMPARE(out.text, QStringLiteral("> quoted\n    code"));
    }

    void loadIsTheInverseOfEmit()
    {
        // What emit writes, load reads back to the same thing.
        const QString html = QStringLiteral(
            "<p><b>bold</b> <i>it</i> <u>under</u> <s>gone</s> <a href=\"https://ok.example/\">link</a></p>"
            "<h2>Heading</h2><ul><li>one</li><li>two</li></ul><ol><li>first</li></ol>"
            "<blockquote><p>quoted <b>text</b></p></blockquote><pre><code>code\n</code></pre><p><br></p><p>after</p>");
        QTextDocument doc;
        RichText::load(&doc, html);
        QCOMPARE(RichText::emit(&doc).html, html);
        // The empty line is an empty line, not a line holding a line break.
        QCOMPARE(doc.lastBlock().previous().text(), QString());
    }

    void loadQuotesAndDropsTheRest()
    {
        QTextDocument doc;
        RichText::load(&doc, QStringLiteral(
                                 "<p>On Monday, Ada wrote:</p><blockquote><p>see <b>this</b></p>"
                                 "<blockquote><p>nested</p></blockquote>"
                                 "<table><tr><td>cell</td></tr></table><p>pic<img src=\"cid:a\">end</p>"
                                 "<script>alert(1)</script></blockquote>"));
        int quoted = 0;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            quoted += b.blockFormat().intProperty(RichText::RoleProperty) == RichText::Quote;
        QVERIFY(quoted >= 2);
        const auto out = RichText::emit(&doc);
        QVERIFY2(out.html.contains(QLatin1String("<blockquote><p>see <b>this</b></p>")), qPrintable(out.html));
        QVERIFY(out.html.contains(QLatin1String("nested")));
        QVERIFY(out.html.contains(QLatin1String("cell"))); // the table flattens to its text
        QVERIFY(out.html.contains(QLatin1String("<p>picend</p>")));
        QVERIFY(!out.html.contains(QLatin1String("alert")));
        QVERIFY(!out.html.contains(QChar(QChar::ObjectReplacementCharacter)));
        QVERIFY(out.text.contains(QLatin1String("> see this")));
    }

    void escapesText()
    {
        QTextDocument doc;
        QTextCursor cur(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        cur.insertText(QStringLiteral("<script>"), bold);
        const auto out = RichText::emit(&doc);
        QCOMPARE(out.html, QStringLiteral("<p><b>&lt;script&gt;</b></p>"));
    }
};

QTEST_MAIN(TestRichText)
#include "tst_richtext.moc"
