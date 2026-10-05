// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Account.h"

#include <QList>
#include <QObject>

namespace e3 {

// The accounts in the data directory, each in accounts/<id>/. Every
// configured account fetches, not only the one on screen: mail arriving for a
// mailbox nobody is looking at is still mail that has arrived.
class AccountManager : public QObject
{
    Q_OBJECT
public:
    explicit AccountManager(const QString &dataDir, QObject *parent = nullptr);
    ~AccountManager() override;

    QList<Account *> accounts() const { return m_accounts; }
    Account *account(int id) const;
    Account *selected() const;
    int selectedId() const { return m_selected; }
    void select(int id);

    Account *create();
    void remove(int id);
    void startAll();
    void stopAll();

Q_SIGNALS:
    void accountsChanged();
    void selectedChanged();

private:
    void load();
    void save() const;
    QString m_root;
    QList<Account *> m_accounts;
    int m_selected = 0;
    int m_nextId = 1;
};

} // namespace e3
