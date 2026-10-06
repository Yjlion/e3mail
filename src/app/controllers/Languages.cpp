// SPDX-License-Identifier: MPL-2.0
#include "Languages.h"

#include "util/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include <algorithm>
#include <memory>

namespace {

constexpr char kKey[] = "language";

QSettings settings()
{
    return QSettings(e3::Paths::dataDir() + QStringLiteral("/app.ini"), QSettings::IniFormat);
}

std::unique_ptr<QTranslator> &appTranslator()
{
    static std::unique_ptr<QTranslator> t;
    return t;
}

std::unique_ptr<QTranslator> &qtTranslator()
{
    static std::unique_ptr<QTranslator> t;
    return t;
}

// "zh_CN" for the file e3mail_zh_CN.qm.
QString codeOf(const QString &file)
{
    return file.mid(7, file.size() - 7 - 3);
}

} // namespace

QList<Languages::Entry> Languages::available()
{
    QList<Entry> out{{QStringLiteral("en"), QStringLiteral("English")}};
    const QStringList files = QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("e3mail_*.qm")}, QDir::Files);
    for (const QString &f : files) {
        const QString code = codeOf(f);
        if (code == QLatin1String("en"))
            continue;
        const QLocale l(code);
        QString name = l.nativeLanguageName();
        if (!name.isEmpty())
            name[0] = name[0].toUpper();
        out.append({code, name});
    }
    std::sort(out.begin() + 1, out.end(), [](const Entry &a, const Entry &b) { return a.code < b.code; });
    return out;
}

QString Languages::saved()
{
    return settings().value(QLatin1String(kKey)).toString();
}

void Languages::save(const QString &code)
{
    QSettings s = settings();
    if (code.isEmpty())
        s.remove(QLatin1String(kKey));
    else
        s.setValue(QLatin1String(kKey), code);
}

QString Languages::install(const QString &code)
{
    for (auto *t : {&appTranslator(), &qtTranslator()}) {
        if (*t) {
            QCoreApplication::removeTranslator(t->get());
            t->reset();
        }
    }
    const QLocale wanted = code.isEmpty() ? QLocale::system() : QLocale(code);
    auto app = std::make_unique<QTranslator>();
    // Tries each of the locale's UI languages in turn, so de_AT finds de.
    // English has a file too, for its plurals.
    if (!app->load(wanted, QStringLiteral("e3mail"), QStringLiteral("_"), QStringLiteral(":/i18n"))
        && !app->load(QStringLiteral(":/i18n/e3mail_en.qm")))
        app.reset();
    const QString language = app ? app->language() : QStringLiteral("en");
    if (app) {
        QCoreApplication::installTranslator(app.get());
        appTranslator() = std::move(app);
    }
    // Qt's own strings (standard buttons, shortcuts), where Qt ships them.
    auto qt = std::make_unique<QTranslator>();
    if (qt->load(QLocale(language), QStringLiteral("qtbase"), QStringLiteral("_"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qt.get());
        qtTranslator() = std::move(qt);
    }

    // Dates and numbers follow the chosen language, or the system's
    // conventions when following the system. The layout follows the
    // language the interface is actually shown in.
    QLocale::setDefault(code.isEmpty() ? QLocale::system() : QLocale(code));
    QGuiApplication::setLayoutDirection(QLocale(language).textDirection());
    return language;
}
