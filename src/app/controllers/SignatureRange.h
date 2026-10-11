// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QTextCursor>

class QTextDocument;

// The signature the composer put into a message, while it can still be told
// apart: its ends move with edits around it, text typed right after it is not
// part of it, and it counts as untouched only while its text is what was put
// in. Only an untouched signature is swapped for another.
class SignatureRange
{
public:
    // Puts "-- " and the signature in at `at`, which ends up after it.
    void place(QTextCursor &at, const QString &html, const QString &text);
    bool untouched() const;
    // Takes an untouched signature out; `at` is left where it was, ready for
    // place(). False, and nothing changed, if there is none.
    bool take(QTextCursor &at);
    void clear();

private:
    QString current() const;

    QTextCursor m_start, m_end;
    QString m_text;
};
