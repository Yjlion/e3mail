// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "models/MessageListModel.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace e3 {
class Account;
class AccountManager;
} // namespace e3

// Everything the UI reads and does, for the selected account. Per-account
// state (selection, thread, labels, counts) is reset in one place,
// switchAccount(), so a label id or message id from one account can never be
// shown against another.
class MailApp : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool portable READ portable CONSTANT)
    Q_PROPERTY(bool firstRunAcknowledged READ firstRunAcknowledged NOTIFY firstRunChanged)
    Q_PROPERTY(bool hasAccounts READ hasAccounts NOTIFY accountsChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(int currentAccountId READ currentAccountId NOTIFY accountChanged)
    Q_PROPERTY(QString accountAddr READ accountAddr NOTIFY accountChanged)
    Q_PROPERTY(QString accountName READ accountName NOTIFY accountChanged)
    Q_PROPERTY(QString accountStatus READ accountStatus NOTIFY statusChanged)
    Q_PROPERTY(QString accountStatusDetail READ accountStatusDetail NOTIFY statusChanged)
    // Older mail the first sync is still downloading, newest first.
    Q_PROPERTY(int olderRemaining READ olderRemaining NOTIFY statusChanged)
    Q_PROPERTY(QString currentTag READ currentTag NOTIFY viewChanged)
    Q_PROPERTY(qint64 currentLabelId READ currentLabelId NOTIFY viewChanged)
    Q_PROPERTY(QString searchText READ searchText NOTIFY viewChanged)
    Q_PROPERTY(MessageListModel *messages READ messages CONSTANT)
    Q_PROPERTY(QVariantList labels READ labels NOTIFY labelsChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    Q_PROPERTY(qint64 selectedMessageId READ selectedMessageId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList thread READ thread NOTIFY selectionChanged)
    // The chosen interface language; empty follows the system.
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)

public:
    explicit MailApp(e3::AccountManager *manager, QObject *parent = nullptr);

    static void setInstance(MailApp *app) { s_instance = app; }
    static MailApp *create(QQmlEngine *, QJSEngine *);

    QString version() const;
    bool portable() const;
    bool firstRunAcknowledged() const;
    bool hasAccounts() const;
    QVariantList accounts() const;
    int currentAccountId() const;
    QString accountAddr() const;
    QString accountName() const;
    QString accountStatus() const;
    QString accountStatusDetail() const;
    int olderRemaining() const;
    QString currentTag() const { return m_tag; }
    qint64 currentLabelId() const { return m_labelId; }
    QString searchText() const { return m_search; }
    MessageListModel *messages() { return &m_messages; }
    QVariantList labels() const { return m_labels; }
    QVariantMap counts() const { return m_counts; }
    qint64 selectedMessageId() const { return m_selected; }
    QVariantList thread() const { return m_thread; }

    e3::Account *account() const;
    // After an account is configured: it now counts, and is shown.
    void accountConfigured();
    e3::AccountManager *manager() const { return m_mgr; }

    Q_INVOKABLE void acknowledgeFirstRun();
    Q_INVOKABLE void selectAccount(int id);
    Q_INVOKABLE void removeAccount(int id);
    Q_INVOKABLE void selectTag(const QString &tag);
    Q_INVOKABLE void selectLabel(qint64 labelId);
    Q_INVOKABLE void setSearch(const QString &text);
    Q_INVOKABLE void selectMessage(qint64 id);
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void refresh();

    Q_INVOKABLE void markRead(qint64 id, bool read);
    Q_INVOKABLE void trash(qint64 id);
    Q_INVOKABLE void restore(qint64 id);
    Q_INVOKABLE void purge(qint64 id);
    Q_INVOKABLE void archive(qint64 id, bool archived);
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE void accept(const QString &addr);
    Q_INVOKABLE void block(const QString &pattern);
    Q_INVOKABLE void unblock(const QString &pattern);
    Q_INVOKABLE QStringList blocklist() const;

    Q_INVOKABLE void setLabel(qint64 msgId, qint64 labelId, bool on);
    Q_INVOKABLE qint64 createLabel(const QString &name, const QString &color);
    Q_INVOKABLE void renameLabel(qint64 labelId, const QString &name, const QString &color);
    Q_INVOKABLE void deleteLabel(qint64 labelId);

    Q_INVOKABLE bool saveAttachment(qint64 msgId, int index, const QUrl &dest);
    Q_INVOKABLE QString viewSource(qint64 msgId) const;
    Q_INVOKABLE void openLink(const QString &url);
    Q_INVOKABLE QString suggestedFileName(qint64 msgId, int index) const;

    Q_INVOKABLE QVariantList contacts(const QString &filter) const;
    Q_INVOKABLE QVariantMap contact(qint64 id) const;
    Q_INVOKABLE void setContactName(qint64 id, const QString &name);
    // -1 inherits the account's mode; 0 lenient, 1 opportunistic, 2 strict.
    Q_INVOKABLE void setContactEncryption(qint64 id, int mode);
    Q_INVOKABLE QStringList completeAddress(const QString &prefix) const;

    Q_INVOKABLE QVariantMap settings() const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE QString signature() const;

    QString language() const;
    void setLanguage(const QString &code);
    QVariantList languages() const;

Q_SIGNALS:
    void firstRunChanged();
    void accountsChanged();
    void accountChanged();
    void statusChanged();
    void viewChanged();
    void labelsChanged();
    void countsChanged();
    void selectionChanged();
    void notify(const QString &message);
    void newMailArrived(const QString &from, const QString &subject);
    // Translators were replaced: the QML engine must retranslate.
    void languageChanged();

private:
    void switchAccount();
    void reloadMessages();
    void reloadLabels();
    void reloadCounts();
    void reloadThread();
    QVariantMap messageMap(qint64 id) const;
    void scheduleRefresh();

    static MailApp *s_instance;
    e3::AccountManager *m_mgr;
    QPointer<e3::Account> m_connected;
    MessageListModel m_messages;
    QString m_tag;
    qint64 m_labelId = 0;
    QString m_search;
    QVariantList m_labels;
    QVariantMap m_counts;
    qint64 m_selected = 0;
    QVariantList m_thread;
    QTimer m_refresh;
};
