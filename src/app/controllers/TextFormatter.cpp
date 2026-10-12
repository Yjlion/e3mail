// SPDX-License-Identifier: MPL-2.0
#include "TextFormatter.h"

#include "RichText.h"
#include "mime/Html.h"

#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>

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
    QTextCursor c(d);
    c.setPosition(qBound(0, m_selStart, last));
    c.setPosition(qBound(0, m_selEnd, last), QTextCursor::KeepAnchor);
    c.beginEditBlock();
    QTextBlockFormat bf = c.blockFormat();
    const int heading = kind.startsWith(u'h') ? kind.mid(1).toInt() : 0;
    const bool toggleOff = (heading && bf.headingLevel() == heading)
        || (kind == QLatin1String("quote") && bf.intProperty(RichText::RoleProperty) == RichText::Quote)
        || (kind == QLatin1String("pre") && bf.intProperty(RichText::RoleProperty) == RichText::Code);
    bf.setHeadingLevel(toggleOff ? 0 : heading);
    bf.setProperty(RichText::RoleProperty, toggleOff ? int(RichText::Paragraph)
                                           : kind == QLatin1String("quote") ? int(RichText::Quote)
                                           : kind == QLatin1String("pre")   ? int(RichText::Code)
                                                                            : int(RichText::Paragraph));
    bf.setLeftMargin(!toggleOff && kind == QLatin1String("quote") ? 16 : 0);
    if (kind == QLatin1String("bullet") || kind == QLatin1String("numbered")) {
        const auto style = kind == QLatin1String("bullet") ? QTextListFormat::ListDisc : QTextListFormat::ListDecimal;
        if (QTextList *list = c.currentList(); list && list->format().style() == style) {
            for (int i = list->count() - 1; i >= 0; --i)
                list->remove(list->item(i));
            bf.setIndent(0);
            c.setBlockFormat(bf);
        } else {
            c.setBlockFormat(bf);
            QTextListFormat lf;
            lf.setStyle(style);
            c.createList(lf);
        }
    } else {
        c.setBlockFormat(bf);
        QTextCharFormat cf;
        if (heading)
            cf.setFontWeight(toggleOff ? QFont::Normal : QFont::Bold);
        if (kind == QLatin1String("pre")) {
            cf.setFontFixedPitch(!toggleOff);
            cf.setFontFamilies({toggleOff ? QString() : QStringLiteral("monospace")});
        }
        if (heading || kind == QLatin1String("pre"))
            c.mergeBlockCharFormat(cf);
    }
    c.endEditBlock();
    refresh();
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
