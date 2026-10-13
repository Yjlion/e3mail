// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <QPointer>
#include <QTextCharFormat>
#include <QtQml/qqmlregistration.h>

#include <QQuickTextDocument>

class QTextBlock;
class QTextDocument;

// Formatting for a rich-text TextArea: the composer's body and the signature
// editor. QML does not expose the editor's own cursor, so a format chosen with
// nothing selected is kept as pending and laid onto the next text typed at
// that spot; what is typed after that inherits it from the character before,
// as in any editor.
class TextFormatter : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY cursorChanged)
    Q_PROPERTY(int selectionStart READ selectionStart WRITE setSelectionStart NOTIFY cursorChanged)
    Q_PROPERTY(int selectionEnd READ selectionEnd WRITE setSelectionEnd NOTIFY cursorChanged)
    // What the next character typed will look like, or the selection.
    Q_PROPERTY(bool bold READ bold NOTIFY stateChanged)
    Q_PROPERTY(bool italic READ italic NOTIFY stateChanged)
    Q_PROPERTY(bool underline READ underline NOTIFY stateChanged)
    Q_PROPERTY(bool strike READ strike NOTIFY stateChanged)
    Q_PROPERTY(bool code READ code NOTIFY stateChanged)
    Q_PROPERTY(bool heading READ heading NOTIFY stateChanged)
    Q_PROPERTY(bool quote READ quote NOTIFY stateChanged)
    Q_PROPERTY(bool bullet READ bullet NOTIFY stateChanged)
    Q_PROPERTY(bool numbered READ numbered NOTIFY stateChanged)

public:
    explicit TextFormatter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *doc);
    int cursorPosition() const { return m_cursor; }
    void setCursorPosition(int pos);
    int selectionStart() const { return m_selStart; }
    void setSelectionStart(int pos);
    int selectionEnd() const { return m_selEnd; }
    void setSelectionEnd(int pos);

    bool bold() const { return m_state.fontWeight() >= QFont::Bold; }
    bool italic() const { return m_state.fontItalic(); }
    bool underline() const { return m_state.fontUnderline() && !m_state.isAnchor(); }
    bool strike() const { return m_state.fontStrikeOut(); }
    bool code() const { return m_state.fontFixedPitch(); }
    bool heading() const { return m_heading; }
    bool quote() const { return m_quote; }
    bool bullet() const { return m_bullet; }
    bool numbered() const { return m_numbered; }

    // "bold", "italic", "underline", "strike", "code".
    Q_INVOKABLE void toggle(const QString &kind);
    // "h2", "quote", "pre", "bullet", "numbered"; again turns it off.
    Q_INVOKABLE void block(const QString &kind);
    Q_INVOKABLE void link(const QString &url);

    // Whether a format is waiting for the next character typed.
    bool hasPending() const { return m_pendingPos >= 0; }

Q_SIGNALS:
    void documentChanged();
    void cursorChanged();
    void stateChanged();

private:
    QTextDocument *doc() const;
    void contentsChange(int position, int removed, int added);
    void dropPendingIfMoved();
    void clearPending();
    void refresh();
    // Takes a paragraph out of its list, if it is in one.
    static void unlist(const QTextBlock &b);
    // Heading level (0 for none) and role, with the look that goes with them.
    static void setRole(const QTextBlock &b, int heading, int role);

    QPointer<QQuickTextDocument> m_document;
    QMetaObject::Connection m_contents;
    int m_cursor = 0, m_selStart = 0, m_selEnd = 0;
    QTextCharFormat m_pending;
    int m_pendingPos = -1;
    bool m_applying = false;
    QTextCharFormat m_state;
    bool m_heading = false, m_quote = false, m_bullet = false, m_numbered = false;
};
