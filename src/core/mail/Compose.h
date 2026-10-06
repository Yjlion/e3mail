// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <stdexcept>

namespace e3::mail {

class ComposeError : public std::runtime_error
{
public:
    explicit ComposeError(const QString &msg) : std::runtime_error(msg.toStdString()) {}
    QString message() const { return QString::fromStdString(what()); }
};

class Compose
{
public:
    // Inserts or updates a draft and returns its id.
    static qint64 saveDraft(MailContext &ctx, const Draft &draft);
    static Draft loadDraft(MailContext &ctx, qint64 msgId);
    static void deleteDraft(MailContext &ctx, qint64 msgId);

    // Applies the encryption policy, renders the message and queues it in the
    // outbox. The message becomes Sent once the transport delivers it.
    // Throws ComposeError when the policy refuses.
    static qint64 queue(MailContext &ctx, const Draft &draft);

    // Prefilled drafts. Reply goes to Reply-To or From, never to the reader's
    // own address taken from the original's To line.
    static Draft reply(MailContext &ctx, qint64 msgId, bool all);
    static Draft forward(MailContext &ctx, qint64 msgId);

    struct Outbox
    {
        qint64 id;
        qint64 msgId;
        QByteArray raw;
        QString from;
        QStringList rcpts;
        int attempts;
    };
    static QList<Outbox> dueOutbox(MailContext &ctx);
    static void markSent(MailContext &ctx, const Outbox &o, const QStringList &refused);
    static void markFailed(MailContext &ctx, const Outbox &o, const QString &error);
};

} // namespace e3::mail
