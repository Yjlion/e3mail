// SPDX-License-Identifier: MPL-2.0
#include "Export.h"

#include "Contacts.h"
#include "crypto/Pgp.h"
#include "mime/Part.h"
#include "mime/Source.h"
#include "store/BlobStore.h"
#include "store/Database.h"

#include <QRegularExpression>

namespace e3::mail {

Export::Source Export::source(MailContext &ctx, qint64 msgId)
{
    Source s;
    const auto blob = ctx.db.queryText("SELECT raw_blob FROM messages WHERE id=?", msgId);
    if (!blob)
        return s;
    s.retained = true;
    s.raw = ctx.blobs.get(*blob);
    const mime::Part p = mime::parse(s.raw);
    if (p.mimeType == "multipart/encrypted" && p.children.size() >= 2) {
        s.encrypted = true;
        try {
            s.inner = ctx.pgp.decrypt(p.children[1].body).data;
        } catch (const std::exception &e) {
            s.decryptError = QString::fromUtf8(e.what());
        }
    }
    return s;
}

QByteArray Export::eml(MailContext &ctx, qint64 msgId)
{
    const auto blob = ctx.db.queryText("SELECT raw_blob FROM messages WHERE id=?", msgId);
    return blob ? ctx.blobs.get(*blob) : QByteArray();
}

QByteArray Export::decryptedEml(MailContext &ctx, qint64 msgId)
{
    const Source s = source(ctx, msgId);
    if (s.inner.isEmpty())
        return {};
    return mime::Source::decryptedEml(s.raw, s.inner);
}

QString Export::fileName(MailContext &ctx, qint64 msgId, const QString &suffix)
{
    return safeFileName(ctx.db.queryText("SELECT subject FROM messages WHERE id=?", msgId).value_or(QString()),
                        QStringLiteral("message"), suffix);
}

QString Export::safeFileName(QString base, const QString &fallback, const QString &suffix)
{
    // Characters no file system takes, and control characters.
    static const QRegularExpression unsafe(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f]+)"));
    base = base.replace(unsafe, QStringLiteral(" ")).simplified().left(80).trimmed();
    while (base.startsWith(u'.'))
        base.remove(0, 1);
    return (base.isEmpty() ? fallback : base) + u'.' + suffix;
}

QList<ContactInfo> Export::addressBook(MailContext &ctx)
{
    QList<ContactInfo> book;
    for (const ContactInfo &c : Contacts::list(ctx, QString(), 1000000)) {
        if (c.isKnown())
            book.append(c);
    }
    return book;
}

} // namespace e3::mail
