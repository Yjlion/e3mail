// SPDX-License-Identifier: MPL-2.0
#include "MessageListModel.h"

#include "util/LocalePatterns.h"

#include <QLocale>

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QString MessageListModel::formatDate(const QDateTime &dt)
{
    const QDateTime local = dt.toLocalTime();
    const QDate today = QDate::currentDate();
    const QLocale loc;
    if (local.date() == today)
        return loc.toString(local.time(), QLocale::ShortFormat);
    if (local.date().year() == today.year())
        return loc.toString(local.date(), e3::LocalePatterns::monthDay(loc));
    return loc.toString(local.date(), QLocale::ShortFormat);
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const e3::MessageSummary &m = m_items[index.row()];
    switch (role) {
    case IdRole:
        return m.id;
    case FromRole:
        return m.from.name.isEmpty() ? m.from.addr : m.from.name;
    case FromAddrRole:
        return m.from.addr;
    case ToRole: {
        QStringList names;
        for (const auto &a : m.to)
            names.append(a.name.isEmpty() ? a.addr : a.name);
        return names.join(QStringLiteral(", "));
    }
    case SubjectRole:
        return m.subject.isEmpty() ? tr("(no subject)") : m.subject;
    case PreviewRole:
        return m.preview;
    case DateRole:
        return formatDate(m.date);
    case UnreadRole:
        return m.unread;
    case EncryptedRole:
        return m.encrypted;
    case SignedRole:
        return m.signedBySender;
    case VerifiedRole:
        return m.verified;
    case ImportanceRole:
        return m.importance;
    case AttachmentsRole:
        return m.hasAttachments;
    case LabelsRole: {
        QVariantList out;
        for (const auto &l : m.labels)
            out.append(QVariantMap{{QStringLiteral("name"), l.name}, {QStringLiteral("color"), l.color}});
        return out;
    }
    case OutgoingRole:
        return m.direction == e3::Direction::Outgoing;
    case StateRole:
        return int(m.state);
    }
    return {};
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    return {{IdRole, "messageId"},       {FromRole, "from"},         {FromAddrRole, "fromAddr"},
            {ToRole, "to"},              {SubjectRole, "subject"},   {PreviewRole, "preview"},
            {DateRole, "date"},          {UnreadRole, "unread"},     {EncryptedRole, "encrypted"},
            {SignedRole, "signed"},      {VerifiedRole, "verified"}, {ImportanceRole, "importance"},
            {AttachmentsRole, "hasAttachments"}, {LabelsRole, "labels"}, {OutgoingRole, "outgoing"},
            {StateRole, "state"}};
}

void MessageListModel::setMessages(const QList<e3::MessageSummary> &messages)
{
    beginResetModel();
    m_items = messages;
    endResetModel();
    Q_EMIT countChanged();
}

int MessageListModel::indexOf(qint64 id) const
{
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id)
            return int(i);
    }
    return -1;
}

qint64 MessageListModel::idAt(int row) const
{
    return row >= 0 && row < m_items.size() ? m_items[row].id : 0;
}
