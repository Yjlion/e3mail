// SPDX-License-Identifier: MPL-2.0
#include "RichText.h"

#include "mime/Html.h"

#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>

using namespace e3;

namespace RichText {

namespace {

bool isMono(const QTextCharFormat &f)
{
    return f.fontFixedPitch() || f.fontFamilies().toStringList().join(u',').contains(QLatin1String("mono"), Qt::CaseInsensitive);
}

// `inHeading`: headings are bold already, so bold is not written inside one.
QString inlineHtml(const QTextBlock &block, bool inCode, bool inHeading, bool *formatted)
{
    QString out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        if (!frag.isValid())
            continue;
        const QTextCharFormat f = frag.charFormat();
        QString text = mime::escapeHtml(frag.text());
        text.replace(QChar::LineSeparator, QStringLiteral("<br>"));
        QStringList open;
        if (f.fontWeight() >= QFont::Bold && !inHeading)
            open << QStringLiteral("b");
        if (f.fontItalic())
            open << QStringLiteral("i");
        if (f.fontUnderline() && !f.isAnchor())
            open << QStringLiteral("u");
        if (f.fontStrikeOut())
            open << QStringLiteral("s");
        if (!inCode && isMono(f))
            open << QStringLiteral("code");
        if (!open.isEmpty())
            *formatted = true;
        QString piece;
        for (const QString &t : open)
            piece += u'<' + t + u'>';
        piece += text;
        for (auto r = open.crbegin(); r != open.crend(); ++r)
            piece += QStringLiteral("</") + *r + u'>';
        if (f.isAnchor()) {
            const QString href = mime::safeHref(f.anchorHref());
            if (!href.isEmpty()) {
                piece = QStringLiteral("<a href=\"") + mime::escapeHtml(href) + QStringLiteral("\">") + piece
                    + QStringLiteral("</a>");
                *formatted = true;
            }
        }
        out += piece;
    }
    return out;
}

QString inlineText(const QTextBlock &block)
{
    QString out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        if (!frag.isValid())
            continue;
        QString t = frag.text();
        t.replace(QChar::LineSeparator, u'\n');
        out += t;
        const QString href = mime::safeHref(frag.charFormat().anchorHref());
        if (frag.charFormat().isAnchor() && !href.isEmpty() && t.trimmed() != href
            && QStringLiteral("mailto:") + t.trimmed() != href)
            out += QStringLiteral(" <") + href + u'>';
    }
    return out;
}

} // namespace

Output emit(const QTextDocument *doc)
{
    Output o;
    bool formatted = false;
    QStringList html;
    QStringList text;
    QTextList *currentList = nullptr;
    int openRole = Paragraph;

    auto closeList = [&] {
        if (currentList) {
            const bool ordered = currentList->format().style() == QTextListFormat::ListDecimal;
            html << (ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>"));
            currentList = nullptr;
        }
    };
    auto closeRole = [&] {
        if (openRole == Quote)
            html << QStringLiteral("</blockquote>");
        else if (openRole == Code)
            html << QStringLiteral("</code></pre>");
        openRole = Paragraph;
    };

    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        const int role = bf.intProperty(RoleProperty);
        QTextList *list = b.textList();
        if (list != currentList)
            closeList();
        if (role != openRole) {
            closeRole();
            if (role == Quote) {
                html << QStringLiteral("<blockquote>");
                formatted = true;
            } else if (role == Code) {
                html << QStringLiteral("<pre><code>");
                formatted = true;
            }
            openRole = role;
        }
        const int level = bf.headingLevel();
        const QString inner = inlineHtml(b, role == Code, level >= 1 && level <= 3 && !list, &formatted);
        const QString plain = inlineText(b);
        if (list) {
            const bool ordered = list->format().style() == QTextListFormat::ListDecimal;
            if (currentList != list) {
                html << (ordered ? QStringLiteral("<ol>") : QStringLiteral("<ul>"));
                currentList = list;
                formatted = true;
            }
            html << QStringLiteral("<li>") + inner + QStringLiteral("</li>");
            const int n = list->itemNumber(b) + 1;
            text << (ordered ? QString::number(n) + QStringLiteral(". ") : QStringLiteral("• ")) + plain;
            continue;
        }
        if (role == Code) {
            html << inner + QStringLiteral("\n");
            text << QStringLiteral("    ") + plain;
        } else if (level >= 1 && level <= 3) {
            const QString h = QStringLiteral("h%1").arg(level);
            html << u'<' + h + u'>' + inner + QStringLiteral("</") + h + u'>';
            text << plain;
            formatted = true;
        } else {
            html << (inner.isEmpty() ? QStringLiteral("<p><br></p>") : QStringLiteral("<p>") + inner + QStringLiteral("</p>"));
            text << (role == Quote ? QStringLiteral("> ") + plain : plain);
        }
    }
    closeList();
    closeRole();
    o.text = text.join(u'\n');
    if (formatted)
        o.html = html.join(QString());
    return o;
}

void load(QTextDocument *doc, const QString &html)
{
    doc->setHtml(mime::sanitizeHtml(html).html);
    QTextCursor c(doc);
    c.beginEditBlock();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat bf = b.blockFormat();
        const int role = bf.intProperty(QTextFormat::BlockQuoteLevel) > 0 ? Quote
            : bf.nonBreakableLines()                                     ? Code
                                                                          : Paragraph;
        // As Composer::setBlock lays them out.
        bf.setProperty(RoleProperty, role);
        bf.clearProperty(QTextFormat::BlockQuoteLevel);
        bf.setLeftMargin(role == Quote ? 16 : 0);
        bf.setRightMargin(0);
        // Lines as the composer's own: HTML's paragraph spacing would make
        // every line of a quote look like its own paragraph.
        if (bf.headingLevel() == 0) {
            bf.setTopMargin(0);
            bf.setBottomMargin(0);
        }
        QTextCursor bc(b);
        bc.setBlockFormat(bf);
        if (role == Code) {
            QTextCharFormat cf;
            cf.setFontFixedPitch(true);
            cf.setFontFamilies({QStringLiteral("monospace")});
            bc.select(QTextCursor::BlockUnderCursor);
            bc.mergeCharFormat(cf);
        }
    }
    // A <pre> ending in a line break leaves an empty line at the end of the
    // code, which emit would write back as a second one.
    for (QTextBlock b = doc->begin(); b.isValid();) {
        const QTextBlock next = b.next();
        const bool codeEnds = !next.isValid() || next.blockFormat().intProperty(RoleProperty) != Code;
        if (b.blockFormat().intProperty(RoleProperty) == Code && b.text().isEmpty() && codeEnds
            && b.previous().isValid() && b.previous().blockFormat().intProperty(RoleProperty) == Code) {
            QTextCursor del(b);
            del.deletePreviousChar(); // joins it to the line before
        }
        b = next;
    }
    // emit writes an empty line as <p><br></p>; read back, that is a line
    // holding a line break, which shows as two.
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.text() == QString(QChar::LineSeparator)) {
            QTextCursor bc(b);
            bc.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            bc.removeSelectedText();
        }
    }
    // Images: their placeholders would come out as text.
    for (QTextCursor f = doc->find(QString(QChar::ObjectReplacementCharacter)); !f.isNull();
         f = doc->find(QString(QChar::ObjectReplacementCharacter), f))
        f.removeSelectedText();
    c.endEditBlock();
}

} // namespace RichText
