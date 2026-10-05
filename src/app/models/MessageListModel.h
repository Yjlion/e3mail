// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mail/Types.h"

#include <QAbstractListModel>
#include <QtQml/qqmlregistration.h>

class MessageListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("provided by MailApp")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        FromRole,
        FromAddrRole,
        ToRole,
        SubjectRole,
        PreviewRole,
        DateRole,
        UnreadRole,
        EncryptedRole,
        SignedRole,
        VerifiedRole,
        ImportanceRole,
        AttachmentsRole,
        LabelsRole,
        OutgoingRole,
        StateRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setMessages(const QList<e3::MessageSummary> &messages);
    Q_INVOKABLE int indexOf(qint64 id) const;
    Q_INVOKABLE qint64 idAt(int row) const;

    static QString formatDate(const QDateTime &dt);

Q_SIGNALS:
    void countChanged();

private:
    QList<e3::MessageSummary> m_items;
};
