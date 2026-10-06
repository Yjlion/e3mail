// SPDX-License-Identifier: MPL-2.0
#include "RichText.h"

#include "mime/Html.h"

#include <QFont>
#include <QTextBlock>
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

QString inlineHtml(const QTextBlock &block, bool inCode, bool *formatted)
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
        if (f.fontWeight() >= QFont::Bold)
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
        const QString inner = inlineHtml(b, role == Code, &formatted);
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
        const int level = bf.headingLevel();
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

} // namespace RichText
