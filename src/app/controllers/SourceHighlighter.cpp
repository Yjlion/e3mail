// SPDX-License-Identifier: MPL-2.0
#include "SourceHighlighter.h"

#include <QRegularExpression>
#include <QTextDocument>

// Block states: in the headers of an entity, or in a body.
enum State { Headers = 0, Body = 1 };

class SourceHighlighter::Highlighter : public QSyntaxHighlighter
{
public:
    Highlighter(SourceHighlighter *owner, QTextDocument *doc) : QSyntaxHighlighter(doc), m_owner(owner) {}

protected:
    void highlightBlock(const QString &text) override
    {
        static const QRegularExpression field(QStringLiteral("^([!-9;-~]+):"));
        static const QRegularExpression boundary(QStringLiteral("^--\\S+$"));
        static const QRegularExpression armor(QStringLiteral("^-----(BEGIN|END) PGP [A-Z ]+-----$"));
        static const QRegularExpression marker(QStringLiteral("^\\s*\u2068.*\u2069$"));
        static const QRegularExpression divider(QStringLiteral("^──── .* ────$"));

        // The first block starts in headers; every entity after a boundary does too.
        int state = previousBlockState() < 0 ? Headers : previousBlockState();
        QTextCharFormat name, muted, armour;
        name.setForeground(m_owner->m_name);
        name.setFontWeight(QFont::DemiBold);
        muted.setForeground(m_owner->m_muted);
        armour.setForeground(m_owner->m_armor);

        if (boundary.match(text).hasMatch()) {
            setFormat(0, int(text.size()), muted);
            state = Headers;
        } else if (armor.match(text).hasMatch()) {
            setFormat(0, int(text.size()), armour);
        } else if (marker.match(text).hasMatch() || divider.match(text).hasMatch()) {
            setFormat(0, int(text.size()), muted);
            if (divider.match(text).hasMatch())
                state = Headers;
        } else if (state == Headers) {
            if (text.trimmed().isEmpty()) {
                state = Body;
            } else if (text.front().isSpace()) {
                setFormat(0, int(text.size()), muted); // a folded continuation
            } else if (const auto m = field.match(text); m.hasMatch()) {
                setFormat(0, int(m.capturedLength(0)), name);
            }
        }
        setCurrentBlockState(state);
    }

private:
    SourceHighlighter *m_owner;
};

SourceHighlighter::SourceHighlighter(QObject *parent) : QObject(parent)
{
    connect(this, &SourceHighlighter::colorsChanged, this, [this] {
        if (m_hl)
            m_hl->rehighlight();
    });
}

void SourceHighlighter::setDocument(QQuickTextDocument *doc)
{
    if (doc == m_doc)
        return;
    delete m_hl;
    m_hl = nullptr;
    m_doc = doc;
    if (doc)
        m_hl = new Highlighter(this, doc->textDocument());
    Q_EMIT documentChanged();
}
