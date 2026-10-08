// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mime/Headers.h"

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

namespace e3 {

class BlobStore;
class Config;
class Database;
class OpLog;
namespace crypto {
class Pgp;
}

enum class Direction { Incoming = 0, Outgoing = 1 };

enum class MessageState {
    Received = 0,
    Draft = 10,
    Pending = 11, // queued in the outbox
    Sent = 12,
    Failed = 13,
};

enum class TrashReason {
    User = 0,       // thrown away by hand
    Expired = 1,    // a disappearing-message timer fired
    Unaccepted = 2, // waited in Unverified past its window
    Blocked = 3,    // sender is on the blocklist
    Cleartext = 4,  // unencrypted mail arriving under strict mode
};

// Strictest wins when several apply to one message.
enum class EncryptionMode { Lenient = 0, Opportunistic = 1, Strict = 2 };

// The composer's padlock, per message.
enum class SendEncryption { Auto = 0, Required = 1, Plaintext = 2 };

// How a contact came to be known. Higher is more deliberate; a contact only
// ever moves up.
enum class ContactOrigin {
    IncomingCopy = 5,   // seen alongside us in To/Cc
    IncomingFrom = 10,  // wrote to us
    Outgoing = 20,      // we wrote to them
    Manual = 30,        // added or accepted by the user
};

enum class RecipientKind { To = 0, Cc = 1, Bcc = 2 };

namespace tag {
inline const QString Inbox = QStringLiteral("inbox");
inline const QString Unverified = QStringLiteral("unverified");
inline const QString Sent = QStringLiteral("sent");
inline const QString Drafts = QStringLiteral("drafts");
inline const QString Archive = QStringLiteral("archive");
inline const QString Trash = QStringLiteral("trash");
inline const QString All = QStringLiteral("all");
} // namespace tag

struct LabelInfo
{
    qint64 id = 0;
    QString name;
    QString color;
    bool system = false;
    int count = 0;
};

struct MessageSummary
{
    qint64 id = 0;
    qint64 threadId = 0;
    Direction direction = Direction::Incoming;
    MessageState state = MessageState::Received;
    mime::Address from;
    QList<mime::Address> to;
    QString subject;
    QString preview;
    QDateTime date;
    bool unread = false;
    bool encrypted = false;
    bool signedBySender = false;
    bool verified = false;
    int importance = 0;
    bool hasAttachments = false;
    QList<LabelInfo> labels; // user labels only
};

struct AttachmentInfo
{
    qint64 id = 0;
    QString filename;
    QString mimeType;
    qint64 size = 0;
    QString blob;
    QString contentId; // without <>; set for parts the HTML can show inline (cid:)
};

struct MessageDetail : MessageSummary
{
    QString messageId;
    QString inReplyTo;
    QStringList references;
    QList<mime::Address> cc;
    QList<mime::Address> bcc;
    QString replyTo;
    QString bodyText;
    QString bodyHtml;       // sanitized, or empty for text/plain mail
    int remoteBlocked = 0;
    QList<AttachmentInfo> attachments;
    QStringList undelivered;
    bool rawAvailable = false;
    QDateTime rawExpiresAt; // invalid: kept forever (if available)
    bool trashed = false;
    TrashReason trashReason = TrashReason::User;
    QDateTime purgeAt;
    bool held = false;
    QDateTime heldAt;
    bool archived = false;
    SendEncryption sendEncryption = SendEncryption::Auto;
};

struct SearchQuery
{
    QString tag = tag::Inbox; // a system tag name, or empty with labelId set
    qint64 labelId = 0;
    QString text;
    int limit = 1000;

    static SearchQuery forTag(const QString &t)
    {
        SearchQuery q;
        q.tag = t;
        return q;
    }
};

struct ContactPhone
{
    QString label;  // mobile | work | home | other
    QString number;
    bool operator==(const ContactPhone &) const = default;
};

struct ContactInfo
{
    qint64 id = 0;
    QString addr;
    QString name;
    // Address book (P10).
    QString organization;
    QString title;
    QString notes;
    QString birthday;          // YYYY-MM-DD, or --MM-DD without a year
    QList<ContactPhone> phones;
    ContactOrigin origin = ContactOrigin::IncomingFrom;
    QString fingerprint;
    bool verified = false;
    bool preferEncrypt = false;
    bool blocked = false;
    std::optional<EncryptionMode> encryptionOverride;
    QDateTime lastSeen;

    bool isKnown() const { return verified || int(origin) >= int(ContactOrigin::Outgoing); }
};

struct OutgoingAttachmentRef
{
    QString filename;
    QString mimeType;
    QByteArray data;
};

// What the composer hands the engine.
struct Draft
{
    qint64 id = 0; // existing draft to update or send, or 0
    QList<mime::Address> to;
    QList<mime::Address> cc;
    QList<mime::Address> bcc;
    QString subject;
    QString text;
    QString html;  // already whitelisted by the composer
    QList<OutgoingAttachmentRef> attachments;
    qint64 inReplyToId = 0;
    int importance = 0;
    SendEncryption encryption = SendEncryption::Auto;
};

// Everything a mail operation needs, constructed per thread: each thread has
// its own database connection and its own RNP keyring.
struct MailContext
{
    Database &db;
    BlobStore &blobs;
    crypto::Pgp &pgp;
    Config &config;
    OpLog &ops;
};

} // namespace e3

Q_DECLARE_METATYPE(e3::MessageSummary)
