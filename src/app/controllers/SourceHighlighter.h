// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>

// Colours a message's source in the source dialog: header names, MIME
// boundaries, OpenPGP armour, and the markers mime::Source::shorten leaves.
// Colours come from the Theme, so it follows light and dark.
class SourceHighlighter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QColor nameColor MEMBER m_name NOTIFY colorsChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY colorsChanged)
    Q_PROPERTY(QColor armorColor MEMBER m_armor NOTIFY colorsChanged)

public:
    explicit SourceHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_doc; }
    void setDocument(QQuickTextDocument *doc);

Q_SIGNALS:
    void documentChanged();
    void colorsChanged();

private:
    class Highlighter;
    QPointer<QQuickTextDocument> m_doc;
    QPointer<QSyntaxHighlighter> m_hl; // owned by the document
    QColor m_name, m_muted, m_armor;
};
