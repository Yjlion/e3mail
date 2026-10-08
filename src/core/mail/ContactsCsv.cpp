// SPDX-License-Identifier: MPL-2.0
#include "ContactsCsv.h"

#include <QRegularExpression>

namespace e3::mail {

namespace {

// Only digits, spaces and phone punctuation: "+44 20 …" cannot be a formula.
bool isPhoneText(const QString &v)
{
    static const QRegularExpression phone(QStringLiteral(R"(^[+0-9 ()./;-]*$)"));
    return phone.match(v).hasMatch();
}

QByteArray cell(QString v)
{
    if (!v.isEmpty() && QStringLiteral("=+-@\t\r").contains(v.front()) && !isPhoneText(v))
        v.prepend(u'\'');
    QByteArray b = v.toUtf8();
    if (b.contains(',') || b.contains('"') || b.contains('\n') || b.contains('\r')) {
        b.replace('"', "\"\"");
        b = '"' + b + '"';
    }
    return b;
}

QByteArray row(const QStringList &cells)
{
    QByteArray out;
    for (qsizetype i = 0; i < cells.size(); ++i) {
        if (i)
            out += ',';
        out += cell(cells[i]);
    }
    return out + "\r\n";
}

} // namespace

QByteArray ContactsCsv::emit(const QList<ContactInfo> &contacts)
{
    // The header row is English on purpose: spreadsheets and address books
    // that import CSV match these names.
    QByteArray out = "\xEF\xBB\xBF";
    out += row({QStringLiteral("Name"), QStringLiteral("Email"), QStringLiteral("Organization"),
                QStringLiteral("Title"), QStringLiteral("Mobile Phone"), QStringLiteral("Work Phone"),
                QStringLiteral("Home Phone"), QStringLiteral("Other Phone"), QStringLiteral("Birthday"),
                QStringLiteral("Notes")});
    for (const ContactInfo &c : contacts) {
        QStringList mobile, work, home, other;
        for (const ContactPhone &p : c.phones) {
            if (p.label == QLatin1String("mobile"))
                mobile.append(p.number);
            else if (p.label == QLatin1String("work"))
                work.append(p.number);
            else if (p.label == QLatin1String("home"))
                home.append(p.number);
            else
                other.append(p.number);
        }
        const QString sep = QStringLiteral(" ; ");
        out += row({c.name, c.addr, c.organization, c.title, mobile.join(sep), work.join(sep), home.join(sep),
                    other.join(sep), c.birthday, c.notes});
    }
    return out;
}

} // namespace e3::mail
