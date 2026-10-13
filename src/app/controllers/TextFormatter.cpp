// SPDX-License-Identifier: MPL-2.0
#include "TextFormatter.h"

#include "RichText.h"
#include "mime/Html.h"

#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>

#include <algorithm>

using namespace e3;

namespace {

// The change `kind` makes to `current`: on becomes off and off becomes on.
QTextCharFormat flipped(const QTextCharFormat &current, const QString &kind)
{
    QTextCharFormat f;
    if (kind == QLatin1String("bold"))
        f.setFontWeight(current.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
    else if (kind == QLatin1String("italic"))
        f.setFontItalic(!current.fontItalic());
    else if (kind == QLatin1String("underline"))
        f.setFontUnderline(!current.fontUnderline());
    else if (kind == QLatin1String("strike"))
        f.setFontStrikeOut(!current.fontStrikeOut());
    else if (kind == QLatin1String("code")) {
        const bool mono = current.fontFixedPitch();
        f.setFontFixedPitch(!mono);
        f.setFontFamilies({mono ? QString() : QStringLiteral("monospace")});
    }
    return f;
}

} // namespace

TextFormatter::TextFormatter(QObject *parent) : QObject(parent) {}

QTextDocument *TextFormatter::doc() const
{
    return m_document ? m_document->textDocument() : nullptr;
}

void TextFormatter::setDocument(QQuickTextDocument *document)
{
    if (document == m_document)
        return;
    disconnect(m_contents);
    m_document = document;
    clearPending();
    if (QTextDocument *d = doc())
        m_contents = connect(d, &QTextDocument::contentsChange, this, &TextFormatter::contentsChange);
    Q_EMIT documentChanged();
    refresh();
}

void TextFormatter::setCursorPosition(int pos)
{
    if (pos == m_cursor)
        return;
    m_cursor = pos;
    // Typing moves the cursor too, and the editor may report that before or
    // after the text itself; decide once both have arrived.
    if (m_pendingPos >= 0 && pos != m_pendingPos)
        QMetaObject::invokeMethod(this, &TextFormatter::dropPendingIfMoved, Qt::QueuedConnection);
    Q_EMIT cursorChanged();
    refresh();
}

void TextFormatter::setSelectionStart(int pos)
{
    if (pos == m_selStart)
        return;
    m_selStart = pos;
    Q_EMIT cursorChanged();
    refresh();
}

void TextFormatter::setSelectionEnd(int pos)
{
    if (pos == m_selEnd)
        return;
    m_selEnd = pos;
    Q_EMIT cursorChanged();
    refresh();
}

void TextFormatter::dropPendingIfMoved()
{
    if (m_pendingPos >= 0 && m_cursor != m_pendingPos) {
        clearPending();
        refresh();
    }
}

void TextFormatter::clearPending()
{
    m_pending = QTextCharFormat();
    m_pendingPos = -1;
}

void TextFormatter::contentsChange(int position, int removed, int added)
{
    QTextDocument *d = doc();
    if (m_applying || !d || m_pendingPos < 0)
        return;
    if (added > 0 && position == m_pendingPos) {
        QTextCursor c(d);
        c.setPosition(position);
        c.setPosition(qMin(position + added, d->characterCount() - 1), QTextCursor::KeepAnchor);
        // Enter alone opens a paragraph; the format waits for its first text.
        if (c.selectedText() == QString(added, QChar::ParagraphSeparator)) {
            m_pendingPos = position + added;
            return;
        }
        m_applying = true;
        c.mergeCharFormat(m_pending);
        m_applying = false;
        clearPending();
        refresh();
        return;
    }
    // Text typed or deleted somewhere else; a change of format alone (a
    // heading, say) keeps it.
    if (added != removed) {
        clearPending();
        refresh();
    }
}

void TextFormatter::toggle(const QString &kind)
{
    QTextDocument *d = doc();
    if (!d)
        return;
    QTextCursor c(d);
    const int last = d->characterCount() - 1;
    if (m_selStart != m_selEnd) {
        c.setPosition(qBound(0, m_selStart, last));
        c.setPosition(qBound(0, m_selEnd, last), QTextCursor::KeepAnchor);
        c.mergeCharFormat(flipped(c.charFormat(), kind));
    } else {
        if (m_pendingPos != m_cursor)
            clearPending();
        QTextCharFormat current = m_state;
        m_pending.merge(flipped(current, kind));
        m_pendingPos = m_cursor;
    }
    refresh();
}

void TextFormatter::block(const QString &kind)
{
    QTextDocument *d = doc();
    if (!d)
        return;
    const int last = d->characterCount() - 1;
    // The paragraphs the selection touches; the first decides on or off.
    QList<QTextBlock> blocks;
    const QTextBlock end = d->findBlock(qBound(0, qMax(m_selStart, m_selEnd), last));
    for (QTextBlock b = d->findBlock(qBound(0, qMin(m_selStart, m_selEnd), last)); b.isValid(); b = b.next()) {
        blocks.append(b);
        if (b == end)
            break;
    }
    if (blocks.isEmpty())
        return;
    const QTextBlockFormat first = blocks.first().blockFormat();
    const int heading = kind.startsWith(u'h') ? kind.mid(1).toInt() : 0;
    const bool listKind = kind == QLatin1String("bullet") || kind == QLatin1String("numbered");

    QTextCursor c(d);
    c.beginEditBlock();
    if (listKind) {
        const auto style = kind == QLatin1String("bullet") ? QTextListFormat::ListDisc : QTextListFormat::ListDecimal;
        QTextList *const list = blocks.first().textList();
        const bool allInList = std::all_of(blocks.cbegin(), blocks.cend(),
                                           [list](const QTextBlock &b) { return list && b.textList() == list; });
        if (allInList && list->format().style() == style) {
            for (const QTextBlock &b : blocks)
                unlist(b);
        } else if (allInList) {
            // The whole list changes kind, as in any editor.
            QTextListFormat lf = list->format();
            lf.setStyle(style);
            list->setFormat(lf);
        } else {
            for (const QTextBlock &b : blocks) {
                unlist(b);
                setRole(b, 0, RichText::Paragraph);
            }
            c.setPosition(blocks.first().position());
            c.setPosition(blocks.last().position(), QTextCursor::KeepAnchor);
            QTextListFormat lf;
            lf.setStyle(style);
            c.createList(lf);
        }
    } else {
        const int role = kind == QLatin1String("quote") ? RichText::Quote
                       : kind == QLatin1String("pre")   ? RichText::Code
                                                        : RichText::Paragraph;
        const bool off = (heading && first.headingLevel() == heading)
            || (role != RichText::Paragraph && first.intProperty(RichText::RoleProperty) == role);
        for (const QTextBlock &b : blocks) {
            unlist(b);
            setRole(b, off ? 0 : heading, off ? int(RichText::Paragraph) : role);
        }
    }
    c.endEditBlock();
    refresh();
}

void TextFormatter::unlist(const QTextBlock &b)
{
    QTextList *list = b.textList();
    if (!list)
        return;
    list->remove(b);
    QTextBlockFormat bf = b.blockFormat();
    bf.setIndent(0);
    bf.setObjectIndex(-1);
    QTextCursor(b).setBlockFormat(bf);
}

void TextFormatter::setRole(const QTextBlock &b, int heading, int role)
{
    const QTextBlockFormat old = b.blockFormat();
    const bool wasHeading = old.headingLevel() > 0;
    const bool wasCode = old.intProperty(RichText::RoleProperty) == RichText::Code;
    QTextBlockFormat bf = old;
    bf.setHeadingLevel(heading);
    bf.setProperty(RichText::RoleProperty, role);
    bf.setLeftMargin(role == RichText::Quote ? 16 : 0);
    QTextCursor c(b);
    c.setBlockFormat(bf);
    // Headings and code look the part in the editor; emit writes them as
    // <h2> and <pre>, whatever the characters say.
    QTextCharFormat cf;
    bool change = false;
    if (heading || wasHeading) {
        cf.setFontWeight(heading ? QFont::Bold : QFont::Normal);
        // As Qt's own HTML import sizes an <h2>.
        cf.setProperty(QTextFormat::FontSizeAdjustment, heading ? 4 - heading : 0);
        change = true;
    }
    if (role == RichText::Code || wasCode) {
        const bool code = role == RichText::Code;
        cf.setFontFixedPitch(code);
        cf.setFontFamilies({code ? QStringLiteral("monospace") : QString()});
        change = true;
    }
    if (!change)
        return;
    c.mergeBlockCharFormat(cf);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    c.mergeCharFormat(cf);
}

void TextFormatter::link(const QString &url)
{
    QTextDocument *d = doc();
    if (!d || m_selStart == m_selEnd)
        return;
    // The scheme is supplied here, where the user typed the address, so that
    // safeHref stays a whitelist with no guessing in it.
    QString target = url.trimmed();
    if (target.contains(u'@') && !target.contains(u':'))
        target.prepend(QStringLiteral("mailto:"));
    else if (!target.contains(QLatin1String("://")) && !target.startsWith(QLatin1String("mailto:")))
        target.prepend(QStringLiteral("https://"));
    const QString safe = mime::safeHref(target);
    const int last = d->characterCount() - 1;
    QTextCursor c(d);
    c.setPosition(qBound(0, m_selStart, last));
    c.setPosition(qBound(0, m_selEnd, last), QTextCursor::KeepAnchor);
    QTextCharFormat f;
    f.setAnchor(!safe.isEmpty());
    f.setAnchorHref(safe);
    f.setFontUnderline(!safe.isEmpty());
    c.mergeCharFormat(f);
    refresh();
}

void TextFormatter::refresh()
{
    QTextDocument *d = doc();
    QTextCharFormat state;
    bool heading = false, quote = false, bullet = false, numbered = false;
    if (d) {
        const int last = d->characterCount() - 1;
        QTextCursor c(d);
        if (m_selStart != m_selEnd) {
            c.setPosition(qBound(0, m_selStart, last));
            c.setPosition(qBound(0, m_selEnd, last), QTextCursor::KeepAnchor);
        } else {
            c.setPosition(qBound(0, m_cursor, last));
        }
        state = c.charFormat();
        if (m_selStart == m_selEnd && m_pendingPos == m_cursor)
            state.merge(m_pending);
        const QTextBlockFormat bf = c.blockFormat();
        heading = bf.headingLevel() > 0;
        quote = bf.intProperty(RichText::RoleProperty) == RichText::Quote;
        if (const QTextList *list = c.currentList()) {
            bullet = list->format().style() == QTextListFormat::ListDisc;
            numbered = list->format().style() == QTextListFormat::ListDecimal;
        }
    }
    m_state = state;
    m_heading = heading;
    m_quote = quote;
    m_bullet = bullet;
    m_numbered = numbered;
    Q_EMIT stateChanged();
}
