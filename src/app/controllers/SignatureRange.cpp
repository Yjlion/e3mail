// SPDX-License-Identifier: MPL-2.0
#include "SignatureRange.h"

#include "RichText.h"

#include <QTextDocument>

void SignatureRange::place(QTextCursor &at, const QString &html, const QString &text)
{
    const int start = at.position();
    // The usual delimiter, as plain text, so it reads the same in both forms.
    at.setCharFormat(QTextCharFormat());
    at.insertText(QStringLiteral("-- \n"));
    RichText::insert(at, html, text);
    m_start = QTextCursor(at.document());
    m_start.setPosition(start);
    m_end = QTextCursor(at.document());
    m_end.setPosition(at.position());
    m_end.setKeepPositionOnInsert(true);
    m_text = current();
}

QString SignatureRange::current() const
{
    if (m_start.isNull() || m_end.isNull() || m_end.position() <= m_start.position())
        return {};
    QTextCursor range(m_start.document());
    range.setPosition(m_start.position());
    range.setPosition(m_end.position(), QTextCursor::KeepAnchor);
    return range.selectedText();
}

bool SignatureRange::untouched() const
{
    return !m_text.isEmpty() && current() == m_text;
}

bool SignatureRange::take(QTextCursor &at)
{
    if (!untouched() || at.document() != m_start.document())
        return false;
    at.setPosition(m_start.position());
    at.setPosition(m_end.position(), QTextCursor::KeepAnchor);
    at.removeSelectedText();
    clear();
    return true;
}

void SignatureRange::clear()
{
    m_start = m_end = QTextCursor();
    m_text.clear();
}
