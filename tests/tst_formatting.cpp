// SPDX-License-Identifier: MPL-2.0
// The composer's formatting against a real TextArea: keys typed into the
// editor, not text put into the document behind its back, because what is
// tested is what the editor's own cursor does with a pending format.
#include "RichText.h"
#include "TextFormatter.h"

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QtTest>

class TestFormatting : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<QQuickView> m_view;
    QQuickItem *m_area = nullptr;
    TextFormatter *m_fmt = nullptr;

    QTextDocument *doc() const
    {
        return m_area->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    }
    // The format of the character at `i`.
    QTextCharFormat at(int i) const
    {
        QTextCursor c(doc());
        c.setPosition(i);
        c.setPosition(i + 1, QTextCursor::KeepAnchor);
        return c.charFormat();
    }
    bool boldAt(int i) const { return at(i).fontWeight() >= QFont::Bold; }
    QString text() const { return doc()->toPlainText(); }
    void type(const QString &s)
    {
        for (const QChar ch : s)
            QTest::keyClick(m_view.get(), ch.toLatin1());
        QCoreApplication::processEvents();
    }
    void moveTo(int pos)
    {
        m_area->setProperty("cursorPosition", pos);
        QCoreApplication::processEvents();
    }
    void select(int from, int to)
    {
        QMetaObject::invokeMethod(m_area, "select", Q_ARG(int, from), Q_ARG(int, to));
        QCoreApplication::processEvents();
    }
    // Paragraphs typed one after another, Enter between them.
    void typeLines(const QStringList &lines)
    {
        for (qsizetype i = 0; i < lines.size(); ++i) {
            if (i)
                QTest::keyClick(m_view.get(), Qt::Key_Return);
            type(lines[i]);
        }
    }
    QTextBlock blockAt(int n) const { return doc()->findBlockByNumber(n); }
    QString html() const { return RichText::emit(doc()).html; }

private Q_SLOTS:
    void initTestCase()
    {
        qmlRegisterType<TextFormatter>("E3test", 1, 0, "TextFormatter");
        QFile f(m_dir.filePath(QStringLiteral("Editor.qml")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"(
import QtQuick
import QtQuick.Controls.Basic
import E3test
Item {
    width: 400; height: 300
    TextArea {
        id: body
        objectName: "body"
        anchors.fill: parent
        textFormat: TextEdit.RichText
        persistentSelection: true
    }
    TextFormatter {
        objectName: "formatter"
        document: body.textDocument
        cursorPosition: body.cursorPosition
        selectionStart: body.selectionStart
        selectionEnd: body.selectionEnd
    }
}
)");
        f.close();
    }

    void init()
    {
        m_view = std::make_unique<QQuickView>();
        m_view->setSource(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("Editor.qml"))));
        QCOMPARE(m_view->status(), QQuickView::Ready);
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view.get()));
        m_area = m_view->rootObject()->findChild<QQuickItem *>(QStringLiteral("body"));
        m_fmt = m_view->rootObject()->findChild<TextFormatter *>(QStringLiteral("formatter"));
        QVERIFY(m_area && m_fmt);
        m_area->forceActiveFocus();
        QVERIFY(m_area->hasActiveFocus());
    }

    void cleanup() { m_view.reset(); }

    void boldWhileTyping()
    {
        type(QStringLiteral("ab"));
        m_fmt->toggle(QStringLiteral("bold"));
        QVERIFY(m_fmt->bold());
        type(QStringLiteral("cd"));
        QVERIFY(m_fmt->bold());
        m_fmt->toggle(QStringLiteral("bold"));
        QVERIFY(!m_fmt->bold());
        type(QStringLiteral("ef"));
        QCOMPARE(text(), QStringLiteral("abcdef"));
        const QList<bool> expected = {false, false, true, true, false, false};
        for (int i = 0; i < expected.size(); ++i)
            QVERIFY2(boldAt(i) == expected[i], qPrintable(QStringLiteral("character %1").arg(i)));
    }

    void severalAtOnce()
    {
        m_fmt->toggle(QStringLiteral("bold"));
        m_fmt->toggle(QStringLiteral("italic"));
        type(QStringLiteral("x"));
        QVERIFY(boldAt(0));
        QVERIFY(at(0).fontItalic());
        // Pressing again before typing undoes it.
        m_fmt->toggle(QStringLiteral("underline"));
        m_fmt->toggle(QStringLiteral("underline"));
        type(QStringLiteral("y"));
        QVERIFY(!at(1).fontUnderline());
        QVERIFY(boldAt(1));
    }

    void movingAwayDropsIt()
    {
        type(QStringLiteral("abc"));
        m_fmt->toggle(QStringLiteral("bold"));
        QVERIFY(m_fmt->hasPending());
        moveTo(1);
        QVERIFY(!m_fmt->hasPending());
        QVERIFY(!m_fmt->bold());
        type(QStringLiteral("x"));
        QVERIFY(!boldAt(1));
    }

    void survivesANewParagraph()
    {
        type(QStringLiteral("ab"));
        m_fmt->toggle(QStringLiteral("bold"));
        QTest::keyClick(m_view.get(), Qt::Key_Return);
        QCoreApplication::processEvents();
        QVERIFY(m_fmt->bold());
        type(QStringLiteral("cd"));
        QVERIFY(boldAt(3));
        QVERIFY(boldAt(4));
        QVERIFY(!boldAt(1));
    }

    void selectionStillToggles()
    {
        type(QStringLiteral("hello"));
        QMetaObject::invokeMethod(m_area, "select", Q_ARG(int, 1), Q_ARG(int, 4));
        QCoreApplication::processEvents();
        m_fmt->toggle(QStringLiteral("bold"));
        QVERIFY(m_fmt->bold());
        QVERIFY(!boldAt(0));
        QVERIFY(boldAt(1) && boldAt(3));
        QVERIFY(!boldAt(4));
        QVERIFY(!m_fmt->hasPending());
    }

    void stateFollowsTheCursor()
    {
        m_fmt->toggle(QStringLiteral("bold"));
        type(QStringLiteral("ab"));
        m_fmt->toggle(QStringLiteral("bold"));
        type(QStringLiteral("cd"));
        moveTo(1);
        QVERIFY(m_fmt->bold());
        moveTo(4);
        QVERIFY(!m_fmt->bold());
    }

    // The block buttons act on the selected paragraphs, and only on them.
    void listOffOnlyThatItem()
    {
        typeLines({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
        select(0, 13);
        m_fmt->block(QStringLiteral("bullet"));
        QVERIFY(blockAt(0).textList() && blockAt(0).textList() == blockAt(2).textList());
        moveTo(5); // in "two"
        QVERIFY(m_fmt->bullet());
        m_fmt->block(QStringLiteral("bullet"));
        QVERIFY(!blockAt(1).textList());
        QVERIFY(!m_fmt->bullet());
        QVERIFY(blockAt(0).textList());
        QVERIFY(blockAt(2).textList());
        QCOMPARE(blockAt(1).blockFormat().indent(), 0);
    }

    void listOffOneItem()
    {
        type(QStringLiteral("one"));
        m_fmt->block(QStringLiteral("numbered"));
        QVERIFY(m_fmt->numbered());
        m_fmt->block(QStringLiteral("numbered"));
        QVERIFY(!m_fmt->numbered());
        QVERIFY(!blockAt(0).textList());
        QCOMPARE(html(), QString());
    }

    void bulletToNumbered()
    {
        typeLines({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
        select(0, 13);
        m_fmt->block(QStringLiteral("bullet"));
        moveTo(5);
        m_fmt->block(QStringLiteral("numbered"));
        QTextList *list = blockAt(0).textList();
        QVERIFY(list);
        QCOMPARE(list->count(), 3);
        QCOMPARE(blockAt(1).textList(), list);
        QCOMPARE(blockAt(2).textList(), list);
        QCOMPARE(list->format().style(), QTextListFormat::ListDecimal);
        QCOMPARE(html(), QStringLiteral("<ol><li>one</li><li>two</li><li>three</li></ol>"));
    }

    void headingOnListItem()
    {
        typeLines({QStringLiteral("one"), QStringLiteral("two")});
        select(0, 7);
        m_fmt->block(QStringLiteral("bullet"));
        moveTo(1);
        m_fmt->block(QStringLiteral("h2"));
        QVERIFY(m_fmt->heading());
        QVERIFY(!m_fmt->bullet());
        QVERIFY(!blockAt(0).textList());
        QVERIFY(blockAt(1).textList());
        QCOMPARE(html(), QStringLiteral("<h2>one</h2><ul><li>two</li></ul>"));
    }

    void quoteOnListItem()
    {
        type(QStringLiteral("one"));
        m_fmt->block(QStringLiteral("bullet"));
        m_fmt->block(QStringLiteral("quote"));
        QVERIFY(m_fmt->quote());
        QVERIFY(!blockAt(0).textList());
        QCOMPARE(html(), QStringLiteral("<blockquote><p>one</p></blockquote>"));
    }

    void listOnHeadingOrQuote()
    {
        typeLines({QStringLiteral("one"), QStringLiteral("two")});
        moveTo(1);
        m_fmt->block(QStringLiteral("h2"));
        moveTo(5);
        m_fmt->block(QStringLiteral("quote"));
        select(0, 7);
        m_fmt->block(QStringLiteral("bullet"));
        QVERIFY(!m_fmt->heading());
        QVERIFY(!m_fmt->quote());
        QVERIFY(!boldAt(0));
        QCOMPARE(html(), QStringLiteral("<ul><li>one</li><li>two</li></ul>"));
    }

    void headingToQuoteIsNotBold()
    {
        type(QStringLiteral("title"));
        m_fmt->block(QStringLiteral("h2"));
        QVERIFY(boldAt(0));
        m_fmt->block(QStringLiteral("quote"));
        QVERIFY(m_fmt->quote());
        QVERIFY(!m_fmt->heading());
        QVERIFY(!boldAt(0));
        QCOMPARE(html(), QStringLiteral("<blockquote><p>title</p></blockquote>"));
    }

    void headingOffIsNotBold()
    {
        type(QStringLiteral("title"));
        m_fmt->block(QStringLiteral("h2"));
        m_fmt->block(QStringLiteral("h2"));
        QVERIFY(!m_fmt->heading());
        QVERIFY(!boldAt(0));
    }

    // Decided by the first paragraph selected: a quote across a quoted and a
    // plain paragraph turns off only when the first is quoted.
    void decidedByTheFirstParagraph()
    {
        typeLines({QStringLiteral("one"), QStringLiteral("two")});
        moveTo(1);
        m_fmt->block(QStringLiteral("quote"));
        select(0, 7);
        m_fmt->block(QStringLiteral("quote"));
        QCOMPARE(blockAt(0).blockFormat().intProperty(RichText::RoleProperty), int(RichText::Paragraph));
        QCOMPARE(blockAt(1).blockFormat().intProperty(RichText::RoleProperty), int(RichText::Paragraph));
        moveTo(5);
        m_fmt->block(QStringLiteral("quote"));
        select(0, 7);
        m_fmt->block(QStringLiteral("quote"));
        QCOMPARE(blockAt(0).blockFormat().intProperty(RichText::RoleProperty), int(RichText::Quote));
        QCOMPARE(blockAt(1).blockFormat().intProperty(RichText::RoleProperty), int(RichText::Quote));
    }

    void oneUndo()
    {
        typeLines({QStringLiteral("one"), QStringLiteral("two")});
        select(0, 7);
        m_fmt->block(QStringLiteral("bullet"));
        doc()->undo();
        QVERIFY(!blockAt(0).textList());
        QVERIFY(!blockAt(1).textList());
    }
};

QTEST_MAIN(TestFormatting)
#include "tst_formatting.moc"
