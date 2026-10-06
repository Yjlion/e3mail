// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QList>
#include <QString>

// The interface language. Translations are compiled in under :/i18n; the
// choice is kept in app.ini in the data directory, and empty means "follow
// the system". Installing a language also sets the default QLocale (dates,
// numbers) and the layout direction (Arabic, Hebrew and Yiddish are
// right to left).
class Languages
{
public:
    struct Entry
    {
        QString code;
        QString name; // in the language itself
    };

    static QList<Entry> available();
    static QString saved();
    static void save(const QString &code);

    // Replaces the installed translators. Returns the language now in effect.
    static QString install(const QString &code);
};
