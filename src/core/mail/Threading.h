// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

namespace e3::mail {

// Groups messages into threads by reference chain: Message-ID, In-Reply-To and
// References. Never by subject, which merges strangers' "Hello" into one
// conversation. A message arriving before its parent still joins it later,
// because every referenced id is recorded against the thread.
class Threading
{
public:
    // Returns the thread id the message now belongs to.
    static qint64 assign(MailContext &ctx, qint64 msgId, const QString &messageId, const QString &inReplyTo,
                         const QStringList &references, qint64 date);
};

} // namespace e3::mail
