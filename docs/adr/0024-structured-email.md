# 0024 — Structured email: shown as text, with buttons only for the trusted and signed

**Status:** Accepted — 2026-10-21

## Context
Some senders describe their message in JSON-LD: airlines, hotels, shops,
carriers. Mail clients turn that into a card with "add to calendar" or
"track". The IETF SML working group is writing this down:
- draft-ietf-sml-structured-email-06: an `application/ld+json` part marked
  `Content-Purpose: Machine-readable`, either as an alternative to the text
  or beside it;
- draft-ietf-sml-trust-01: process structured data only from a trusted sender
  with a valid signature, or where doing so adds no attack surface.

e3mail renders no web content, loads nothing remote (ADR 0003, ADR 0009), and
holds mail from strangers in Unverified (ADR 0008).

## Decision
- **Receive only.** e3mail reads structured data and does not send any.
- **Where it is read:**
  - an `application/ld+json` part in a `multipart/alternative` (full
    representation);
  - such a part elsewhere, unless it is an attachment without
    `Content-Purpose: Machine-readable`, since a person may attach a `.json`
    file;
  - in HTML mail without such a part, the first
    `<script type="application/ld+json">`, as senders wrote it before SML
    (read before the sanitizer drops it).
  - The part itself is not listed as an attachment.
  - Microdata is not read.
- **Limits.** At most 64 KiB, nesting depth 12, eight items, 200 characters a
  value. JSON beyond the limits is not read at all.
  - Values are plain text: no markup, and no control or bidi formatting
    characters.
  - The JSON is kept as received (migration 7) and read again when the
    message is shown.
- **What is recognised** (schema.org): Event and EventReservation;
  FlightReservation; TrainReservation and BusReservation; LodgingReservation;
  ParcelDelivery; Order; Invoice. A ViewAction or TrackAction target, or a
  url field, is the item's link, and only if it is http or https. Any other
  type is ignored.
- **Trusted or inert** (`Structured::actionable`):
  - **Actions** are offered only for incoming mail that is neither held nor
    in Trash, signed by the sender's known key, from a sender who is trusted
    (ADR 0022, so the person's switch counts). The actions are:
    - "Add to calendar…" saves an iCalendar file through the save dialog;
    - "Open…" goes through the link dialog (ADR 0009), which shows the real
      target.
  - **Anything else** shows the same facts as text, with a note that buttons
    appear only for signed mail from trusted senders, and to check the facts
    against the message.
  - The app checks the rule again when asked to save, not only when it draws
    the button.
- **Nothing is fetched.** Images, logos and "follow your nose" references
  are ignored (trust-01, 3.5 and 4.2).

## Consequences
- **Inert cards still render.** A stranger's structured data shows as text,
  which the trust draft allows for display that adds no attack surface. A
  sender can still mimic a card in the HTML body, so the inert note tells
  the reader to check the facts against the message.
- **Most real bookings are inert.** Few commercial senders sign their mail
  with OpenPGP, so most cards from airlines and shops have no buttons. That
  is what the draft recommends, not an accident.
- **Nothing is processed automatically.** There is no adding to a calendar
  on arrival and no Sieve rule.
- The SML drafts are drafts. When they settle, the part rules may change;
  the item list stays e3mail's own.
