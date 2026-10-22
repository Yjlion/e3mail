// SPDX-License-Identifier: MPL-2.0
#include "Structured.h"

#include "Contacts.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>

namespace e3::mail {

namespace {

int depthOf(const QJsonValue &v, int limit)
{
    if (limit <= 0)
        return 1;
    int deepest = 0;
    if (v.isArray()) {
        for (const QJsonValue &c : v.toArray())
            deepest = qMax(deepest, depthOf(c, limit - 1));
        return deepest + 1;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            deepest = qMax(deepest, depthOf(it.value(), limit - 1));
        return deepest + 1;
    }
    return 0;
}

// Plain text, one line, bounded: nothing in a value can act as markup or
// run on for pages.
QString clean(QString s)
{
    QString out;
    out.reserve(qMin<qsizetype>(s.size(), Structured::kMaxText));
    for (const QChar c : std::as_const(s)) {
        if (out.size() >= Structured::kMaxText)
            break;
        if (c.isSpace())
            out += u' ';
        else if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format)
            out += c; // bidi overrides and other invisible marks are dropped
    }
    return out.simplified();
}

QStringList typesOf(const QJsonObject &o)
{
    const QJsonValue t = o.value(QLatin1String("@type"));
    QStringList out;
    if (t.isString())
        out.append(t.toString());
    for (const QJsonValue &v : t.toArray())
        out.append(v.toString());
    // "http://schema.org/Event" and "schema:Event" name the same type.
    for (QString &s : out)
        s = s.section(u'/', -1).section(u':', -1);
    return out;
}

// A text value: a string, or the name (or @value) of an object.
QString text(const QJsonValue &v)
{
    if (v.isString())
        return clean(v.toString());
    if (v.isDouble())
        return QString::number(v.toDouble());
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (const char *k : {"name", "@value", "iataCode", "value"}) {
            if (o.value(QLatin1String(k)).isString())
                return clean(o.value(QLatin1String(k)).toString());
        }
    }
    if (v.isArray() && !v.toArray().isEmpty())
        return text(v.toArray().first());
    return {};
}

QString textAt(const QJsonObject &o, const char *key)
{
    return text(o.value(QLatin1String(key)));
}

QJsonObject objectAt(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isArray())
        return v.toArray().first().toObject();
    return v.toObject();
}

QDateTime dateAt(const QJsonObject &o, const char *key, bool *dateOnly = nullptr)
{
    const QString s = o.value(QLatin1String(key)).toString().trimmed();
    if (s.isEmpty())
        return {};
    if (s.size() == 10) {
        const QDate d = QDate::fromString(s, Qt::ISODate);
        if (dateOnly)
            *dateOnly = d.isValid();
        return d.isValid() ? d.startOfDay() : QDateTime();
    }
    return QDateTime::fromString(s, Qt::ISODate);
}

QString placeOf(const QJsonValue &v)
{
    if (v.isString())
        return clean(v.toString());
    const QJsonObject o = v.toObject();
    QStringList parts;
    if (const QString n = textAt(o, "name"); !n.isEmpty())
        parts.append(n);
    const QJsonValue address = o.value(QLatin1String("address"));
    if (address.isString()) {
        parts.append(clean(address.toString()));
    } else if (address.isObject()) {
        const QJsonObject a = address.toObject();
        QStringList line;
        for (const char *k : {"streetAddress", "postalCode", "addressLocality", "addressCountry"}) {
            if (const QString t = textAt(a, k); !t.isEmpty())
                line.append(t);
        }
        if (!line.isEmpty())
            parts.append(line.join(QStringLiteral(", ")));
    }
    return clean(parts.join(QStringLiteral(", ")));
}

QString httpLink(const QString &s)
{
    const QUrl u(s.trimmed(), QUrl::StrictMode);
    const QString scheme = u.scheme().toLower();
    return u.isValid() && (scheme == QLatin1String("https") || scheme == QLatin1String("http")) && !u.host().isEmpty()
        ? u.toString(QUrl::FullyEncoded)
        : QString();
}

// A ViewAction or TrackAction's target, or a plain url field.
QString linkOf(const QJsonObject &o, const char *urlKey)
{
    QJsonArray actions = o.value(QLatin1String("potentialAction")).toArray();
    if (o.value(QLatin1String("potentialAction")).isObject())
        actions.append(o.value(QLatin1String("potentialAction")));
    for (const QJsonValue &a : std::as_const(actions)) {
        const QJsonObject action = a.toObject();
        const QStringList types = typesOf(action);
        if (!types.contains(QLatin1String("ViewAction")) && !types.contains(QLatin1String("TrackAction")))
            continue;
        const QJsonValue target = action.value(QLatin1String("target")).isUndefined() ? action.value(QLatin1String("url"))
                                                                                      : action.value(QLatin1String("target"));
        const QString url = target.isString() ? target.toString()
                                              : objectAt(action, "target").value(QLatin1String("urlTemplate")).toString();
        if (const QString l = httpLink(url); !l.isEmpty())
            return l;
    }
    return urlKey ? httpLink(o.value(QLatin1String(urlKey)).toString()) : QString();
}

void add(Structured::Item &item, const QString &label, const QString &value)
{
    if (!value.isEmpty())
        item.fields.append({label, value});
}

QString money(const QJsonObject &o, const char *priceKey)
{
    const QJsonValue p = o.value(QLatin1String(priceKey));
    if (p.isObject()) // a PriceSpecification
        return money(p.toObject(), "price");
    const QString amount = text(p);
    if (amount.isEmpty())
        return {};
    const QString currency = textAt(o, "priceCurrency");
    return currency.isEmpty() ? amount : amount + u' ' + currency;
}

std::optional<Structured::Item> itemFrom(const QJsonObject &o)
{
    const QStringList types = typesOf(o);
    const auto is = [&types](const char *t) { return types.contains(QLatin1String(t)); };
    Structured::Item item;
    const QString booking = textAt(o, "reservationNumber");
    if (is("FlightReservation")) {
        const QJsonObject f = objectAt(o, "reservationFor");
        item.kind = Structured::Kind::Flight;
        const QString airline = text(f.value(QLatin1String("airline")));
        const QString number = textAt(f, "flightNumber");
        const QJsonObject from = objectAt(f, "departureAirport");
        const QJsonObject to = objectAt(f, "arrivalAirport");
        const QString fromCode = textAt(from, "iataCode").isEmpty() ? text(from) : textAt(from, "iataCode");
        const QString toCode = textAt(to, "iataCode").isEmpty() ? text(to) : textAt(to, "iataCode");
        item.title = QCoreApplication::translate("Structured", "Flight %1").arg(clean(objectAt(f, "airline").value(QLatin1String("iataCode")).toString() + number));
        if (!fromCode.isEmpty() || !toCode.isEmpty())
            item.title += QStringLiteral(" · ") + fromCode + QStringLiteral(" → ") + toCode;
        item.start = dateAt(f, "departureTime");
        item.end = dateAt(f, "arrivalTime");
        add(item, QCoreApplication::translate("Structured", "Airline"), airline);
        add(item, QCoreApplication::translate("Structured", "From"), placeOf(from));
        add(item, QCoreApplication::translate("Structured", "To"), placeOf(to));
        add(item, QCoreApplication::translate("Structured", "Booking"), booking);
        add(item, QCoreApplication::translate("Structured", "Seat"), textAt(objectAt(objectAt(o, "reservedTicket"), "ticketedSeat"), "seatNumber"));
        add(item, QCoreApplication::translate("Structured", "Passenger"), text(o.value(QLatin1String("underName"))));
    } else if (is("TrainReservation") || is("BusReservation")) {
        const QJsonObject t = objectAt(o, "reservationFor");
        item.kind = Structured::Kind::Train;
        const QString from = text(t.value(QLatin1String(is("BusReservation") ? "departureBusStop" : "departureStation")));
        const QString to = text(t.value(QLatin1String(is("BusReservation") ? "arrivalBusStop" : "arrivalStation")));
        item.title = (from.isEmpty() && to.isEmpty()) ? QCoreApplication::translate("Structured", "Train") : from + QStringLiteral(" → ") + to;
        item.start = dateAt(t, "departureTime");
        item.end = dateAt(t, "arrivalTime");
        add(item, QCoreApplication::translate("Structured", "Train"), clean(textAt(t, "trainName") + u' ' + textAt(t, "trainNumber") + textAt(t, "busNumber")));
        add(item, QCoreApplication::translate("Structured", "Booking"), booking);
        add(item, QCoreApplication::translate("Structured", "Seat"), textAt(objectAt(objectAt(o, "reservedTicket"), "ticketedSeat"), "seatNumber"));
        add(item, QCoreApplication::translate("Structured", "Passenger"), text(o.value(QLatin1String("underName"))));
    } else if (is("LodgingReservation")) {
        const QJsonObject l = objectAt(o, "reservationFor");
        item.kind = Structured::Kind::Lodging;
        item.title = text(l).isEmpty() ? QCoreApplication::translate("Structured", "Stay") : text(l);
        bool dateOnly = false;
        item.start = dateAt(o, "checkinTime", &dateOnly);
        if (!item.start.isValid())
            item.start = dateAt(o, "checkinDate", &dateOnly);
        item.end = dateAt(o, "checkoutTime");
        if (!item.end.isValid())
            item.end = dateAt(o, "checkoutDate");
        item.allDay = dateOnly;
        item.place = placeOf(l);
        add(item, QCoreApplication::translate("Structured", "Booking"), booking);
        add(item, QCoreApplication::translate("Structured", "Guest"), text(o.value(QLatin1String("underName"))));
    } else if (is("EventReservation") || std::any_of(types.cbegin(), types.cend(), [](const QString &t) {
                   return t.endsWith(QLatin1String("Event"));
               })) {
        const QJsonObject e = is("EventReservation") ? objectAt(o, "reservationFor") : o;
        item.kind = Structured::Kind::Event;
        item.title = textAt(e, "name").isEmpty() ? QCoreApplication::translate("Structured", "Event") : textAt(e, "name");
        bool dateOnly = false;
        item.start = dateAt(e, "startDate", &dateOnly);
        item.end = dateAt(e, "endDate");
        item.allDay = dateOnly;
        item.place = placeOf(e.value(QLatin1String("location")));
        add(item, QCoreApplication::translate("Structured", "Booking"), booking);
        add(item, QCoreApplication::translate("Structured", "Name"), text(o.value(QLatin1String("underName"))));
        item.link = linkOf(o, "url");
        if (item.link.isEmpty())
            item.link = linkOf(e, "url");
    } else if (is("ParcelDelivery")) {
        item.kind = Structured::Kind::Parcel;
        const QString what = text(o.value(QLatin1String("itemShipped")));
        item.title = what.isEmpty() ? QCoreApplication::translate("Structured", "Parcel") : what;
        const QString carrier = text(o.value(QLatin1String("carrier"))).isEmpty() ? text(o.value(QLatin1String("provider")))
                                                                                 : text(o.value(QLatin1String("carrier")));
        add(item, QCoreApplication::translate("Structured", "Carrier"), carrier);
        add(item, QCoreApplication::translate("Structured", "Tracking number"), textAt(o, "trackingNumber"));
        bool dateOnly = false;
        item.start = dateAt(o, "expectedArrivalUntil", &dateOnly);
        item.allDay = dateOnly;
        item.place = placeOf(o.value(QLatin1String("deliveryAddress")));
        item.link = linkOf(o, "trackingUrl");
    } else if (is("Order")) {
        item.kind = Structured::Kind::Order;
        const QString seller = text(o.value(QLatin1String("seller"))).isEmpty() ? text(o.value(QLatin1String("merchant")))
                                                                               : text(o.value(QLatin1String("seller")));
        item.title = seller.isEmpty() ? QCoreApplication::translate("Structured", "Order") : QCoreApplication::translate("Structured", "Order from %1").arg(seller);
        add(item, QCoreApplication::translate("Structured", "Order number"), textAt(o, "orderNumber"));
        add(item, QCoreApplication::translate("Structured", "Status"), textAt(o, "orderStatus").section(u'/', -1).section(u':', -1));
        add(item, QCoreApplication::translate("Structured", "Total"), money(o, "price"));
        item.link = linkOf(o, "url");
    } else if (is("Invoice")) {
        item.kind = Structured::Kind::Invoice;
        const QString from = text(o.value(QLatin1String("provider")));
        item.title = from.isEmpty() ? QCoreApplication::translate("Structured", "Invoice") : QCoreApplication::translate("Structured", "Invoice from %1").arg(from);
        add(item, QCoreApplication::translate("Structured", "Amount due"), money(objectAt(o, "totalPaymentDue").isEmpty() ? o : objectAt(o, "totalPaymentDue"),
                                          objectAt(o, "totalPaymentDue").isEmpty() ? "totalPaymentDue" : "price"));
        bool dateOnly = false;
        item.start = dateAt(o, "paymentDueDate", &dateOnly);
        item.allDay = dateOnly;
        add(item, QCoreApplication::translate("Structured", "Reference"), textAt(o, "confirmationNumber"));
        item.link = linkOf(o, "url");
    } else {
        return std::nullopt;
    }
    // A ViewAction (or a plain url) on the reservation or order itself.
    if (item.link.isEmpty())
        item.link = linkOf(o, "url");
    if (item.end.isValid() && item.start.isValid() && item.end < item.start)
        item.end = QDateTime(); // nonsense: not shown
    return item;
}

QByteArray icsText(const QString &s)
{
    QString out = s;
    out.replace(u'\\', QStringLiteral("\\\\"));
    out.replace(u';', QStringLiteral("\\;"));
    out.replace(u',', QStringLiteral("\\,"));
    out.replace(u'\n', QStringLiteral("\\n"));
    return out.toUtf8();
}

// Folded at 75 octets (RFC 5545, 3.1), never inside a UTF-8 sequence.
QByteArray icsLine(const QByteArray &line)
{
    QByteArray out;
    qsizetype start = 0;
    int limit = 75;
    while (line.size() - start > limit) {
        qsizetype cut = start + limit;
        while (cut > start && (quint8(line[cut]) & 0xC0) == 0x80)
            --cut;
        out += line.mid(start, cut - start) + "\r\n ";
        start = cut;
        limit = 74;
    }
    return out + line.mid(start) + "\r\n";
}

QByteArray icsTime(const QDateTime &t, bool allDay)
{
    if (allDay)
        return ";VALUE=DATE:" + t.date().toString(QStringLiteral("yyyyMMdd")).toLatin1();
    return ":" + t.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

} // namespace

bool Structured::actionable(MailContext &ctx, const MessageDetail &m)
{
    return m.direction == Direction::Incoming && !m.held && !m.trashed && (m.signedBySender || m.verified)
        && Contacts::isTrusted(ctx, m.from.addr);
}

QString Structured::accept(const QByteArray &json)
{
    if (json.trimmed().isEmpty() || json.size() > kMaxBytes)
        return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || doc.isNull())
        return {};
    const QJsonValue root = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
    if (depthOf(root, kMaxDepth + 1) > kMaxDepth)
        return {};
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

QByteArray Structured::fromHtml(const QString &html)
{
    static const QRegularExpression script(
        QStringLiteral("<script[^>]*type\\s*=\\s*[\"']?application/ld\\+json[\"']?[^>]*>(.*?)</script\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const auto m = script.match(html.left(4 * kMaxBytes));
    return m.hasMatch() ? m.captured(1).trimmed().toUtf8() : QByteArray();
}

QList<Structured::Item> Structured::parse(const QString &json)
{
    QList<Item> out;
    if (json.isEmpty() || json.size() > kMaxBytes)
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonArray nodes = doc.isArray() ? doc.array() : QJsonArray{doc.object()};
    // A @graph holds the nodes of one document.
    if (doc.isObject() && doc.object().value(QLatin1String("@graph")).isArray())
        nodes = doc.object().value(QLatin1String("@graph")).toArray();
    for (const QJsonValue &v : std::as_const(nodes)) {
        if (out.size() >= kMaxItems)
            break;
        if (const auto item = itemFrom(v.toObject()))
            out.append(*item);
    }
    return out;
}

QByteArray Structured::toIcs(const Item &item, const QString &uid)
{
    if (!item.start.isValid())
        return {};
    const QDateTime end = item.end.isValid() ? item.end : item.start.addSecs(item.allDay ? 86400 : 3600);
    QByteArray out = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//e3mail//structured email//EN\r\nBEGIN:VEVENT\r\n";
    out += icsLine("UID:" + icsText(uid));
    out += icsLine("DTSTAMP:" + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1());
    out += icsLine("DTSTART" + icsTime(item.start, item.allDay));
    out += icsLine("DTEND" + icsTime(end, item.allDay));
    out += icsLine("SUMMARY:" + icsText(item.title));
    if (!item.place.isEmpty())
        out += icsLine("LOCATION:" + icsText(item.place));
    QStringList lines;
    for (const Field &f : item.fields)
        lines.append(f.label + QStringLiteral(": ") + f.value);
    if (!lines.isEmpty())
        out += icsLine("DESCRIPTION:" + icsText(lines.join(u'\n')));
    if (!item.link.isEmpty())
        out += icsLine("URL:" + item.link.toUtf8());
    out += "END:VEVENT\r\nEND:VCALENDAR\r\n";
    return out;
}

} // namespace e3::mail
