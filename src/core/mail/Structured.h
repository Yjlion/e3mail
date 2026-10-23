// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace e3::mail {

// Structured email (ADR 0024, after draft-ietf-sml-structured-email-06 and
// draft-ietf-sml-trust-01): JSON-LD a message carries about itself, read
// for a short list of schema.org types and shown as a card. Every value is
// plain text, nothing is fetched, and only a trusted, signed sender's card
// offers actions.
class Structured
{
public:
    // Anything larger, deeper or longer is not read at all.
    static constexpr qsizetype kMaxBytes = 64 * 1024;
    static constexpr int kMaxDepth = 12;
    static constexpr int kMaxItems = 8;
    static constexpr int kMaxText = 200;

    enum class Kind { Event, Flight, Train, Lodging, Parcel, Order, Invoice };
    struct Field
    {
        QString label; // translated
        QString value;
    };
    struct Item
    {
        Kind kind = Kind::Event;
        QString title;
        QDateTime start; // invalid when not given
        QDateTime end;
        bool allDay = false;
        QString place;
        QList<Field> fields;
        QString link; // http or https only
    };

    // The JSON-LD to keep with a message: well formed and within the limits,
    // or empty.
    static QString accept(const QByteArray &json);
    // JSON-LD in <script type="application/ld+json"> of HTML mail, as
    // senders wrote it before SML.
    static QByteArray fromHtml(const QString &html);
    // The items recognised, in order. Other types are left out.
    static QList<Item> parse(const QString &json);
    // An iCalendar event (RFC 5545) for an item that has a start, or empty.
    static QByteArray toIcs(const Item &item, const QString &uid);

    // May the card offer actions? Only for mail from someone trusted, not
    // held, and signed by their key (draft-ietf-sml-trust-01, 4.1). Anything
    // else is shown inert: the facts as text, no buttons.
    static bool actionable(MailContext &ctx, const MessageDetail &m);
};

} // namespace e3::mail
