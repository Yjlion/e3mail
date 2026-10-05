// SPDX-License-Identifier: MPL-2.0
#include "AccountManager.h"

#include "SecretStore.h"
#include "util/Log.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace e3 {

AccountManager::AccountManager(const QString &dataDir, QObject *parent)
    : QObject(parent), m_root(dataDir + QStringLiteral("/accounts"))
{
    QDir().mkpath(m_root);
    load();
}

AccountManager::~AccountManager()
{
    stopAll();
}

void AccountManager::load()
{
    QFile f(m_root + QStringLiteral("/accounts.json"));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly))
        o = QJsonDocument::fromJson(f.readAll()).object();
    m_nextId = qMax(1, o.value(QStringLiteral("next_id")).toInt(1));
    for (const QJsonValue &v : o.value(QStringLiteral("accounts")).toArray()) {
        const int id = v.toInt();
        try {
            m_accounts.append(new Account(id, m_root + u'/' + QString::number(id), this));
        } catch (const std::exception &e) {
            qCCritical(lcEngine) << "cannot open account" << id << e.what();
        }
        m_nextId = qMax(m_nextId, id + 1);
    }
    m_selected = o.value(QStringLiteral("selected")).toInt();
    if (!account(m_selected))
        m_selected = m_accounts.isEmpty() ? 0 : m_accounts.first()->id();
}

void AccountManager::save() const
{
    QJsonArray ids;
    for (const Account *a : m_accounts)
        ids.append(a->id());
    const QJsonObject o{{QStringLiteral("accounts"), ids},
                        {QStringLiteral("selected"), m_selected},
                        {QStringLiteral("next_id"), m_nextId}};
    QSaveFile f(m_root + QStringLiteral("/accounts.json"));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson());
        f.commit();
    }
}

Account *AccountManager::account(int id) const
{
    for (Account *a : m_accounts) {
        if (a->id() == id)
            return a;
    }
    return nullptr;
}

Account *AccountManager::selected() const
{
    return account(m_selected);
}

void AccountManager::select(int id)
{
    if (id == m_selected || !account(id))
        return;
    m_selected = id;
    save();
    Q_EMIT selectedChanged();
}

Account *AccountManager::create()
{
    const int id = m_nextId++;
    auto *a = new Account(id, m_root + u'/' + QString::number(id), this);
    m_accounts.append(a);
    m_selected = id;
    save();
    Q_EMIT accountsChanged();
    Q_EMIT selectedChanged();
    return a;
}

void AccountManager::remove(int id)
{
    Account *a = account(id);
    if (!a)
        return;
    a->stop();
    const QString dir = a->dir();
    m_accounts.removeOne(a);
    delete a;
    SecretStore::remove(QStringLiteral("account-%1-in").arg(id));
    SecretStore::remove(QStringLiteral("account-%1-smtp").arg(id));
    QDir(dir).removeRecursively();
    if (m_selected == id)
        m_selected = m_accounts.isEmpty() ? 0 : m_accounts.first()->id();
    save();
    Q_EMIT accountsChanged();
    Q_EMIT selectedChanged();
}

void AccountManager::startAll()
{
    // One account's failure must not stop the others from starting.
    for (Account *a : std::as_const(m_accounts)) {
        try {
            a->start();
        } catch (const std::exception &e) {
            qCWarning(lcEngine) << "account" << a->id() << "did not start:" << e.what();
        }
    }
}

void AccountManager::stopAll()
{
    for (Account *a : std::as_const(m_accounts))
        a->stop();
}

} // namespace e3
