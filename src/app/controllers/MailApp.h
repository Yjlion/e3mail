// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "models/MessageListModel.h"

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class Notifier;
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
    // Other devices of this account (ADR 0010).
    Q_PROPERTY(bool devicesAvailable READ devicesAvailable CONSTANT)
    // Pairing codes can be read with a camera: built with one, and one there.
    Q_PROPERTY(bool scanAvailable READ scanAvailable CONSTANT)
    // [{id, name, online, lastSeen}]
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY devicesChanged)
    // The device that removed this one from the account while it was away,
    // until the person has seen it; empty otherwise.
    Q_PROPERTY(QString removedBy READ removedBy NOTIFY devicesChanged)
    // "default", "off", or a relay URL.
    Q_PROPERTY(QString relay READ relay WRITE setRelay NOTIFY devicesChanged)
    // While the status is "cert": {server, fingerprint}; otherwise empty.
    Q_PROPERTY(QVariantMap refusedCertificate READ refusedCertificate NOTIFY statusChanged)
    Q_PROPERTY(QString currentTag READ currentTag NOTIFY viewChanged)
    Q_PROPERTY(qint64 currentLabelId READ currentLabelId NOTIFY viewChanged)
    Q_PROPERTY(QString searchText READ searchText NOTIFY viewChanged)
    Q_PROPERTY(MessageListModel *messages READ messages CONSTANT)
    Q_PROPERTY(QVariantList labels READ labels NOTIFY labelsChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    // [{id, name, preview, isDefault}], sorted by name.
    Q_PROPERTY(QVariantList signatures READ signatures NOTIFY signaturesChanged)
    // An id, or empty for no signature on new mail.
    Q_PROPERTY(QString defaultSignature READ defaultSignature WRITE setDefaultSignature NOTIFY signaturesChanged)
    Q_PROPERTY(qint64 selectedMessageId READ selectedMessageId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList thread READ thread NOTIFY selectionChanged)
    // The chosen interface language; empty follows the system.
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    // New-mail notifications on this device: off | sender | full.
    Q_PROPERTY(QString notificationMode READ notificationMode WRITE setNotificationMode NOTIFY notificationsChanged)
    Q_PROPERTY(bool notificationsAvailable READ notificationsAvailable CONSTANT)
    // Android: keep syncing while e3mail is not in front, through a
    // foreground service and its notification (ADR 0018). Per device.
    Q_PROPERTY(bool backgroundSync READ backgroundSync WRITE setBackgroundSync NOTIFY backgroundSyncChanged)
    Q_PROPERTY(bool backgroundSyncAvailable READ backgroundSyncAvailable CONSTANT)

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
    // A message's source for the source dialog: retained, encrypted,
    // original, decrypted, decryptError, subject, and outline/innerOutline
    // ([{depth, type, encoding, filename, size}]).
    Q_INVOKABLE QVariantMap messageSource(qint64 msgId) const;
    Q_INVOKABLE QString shortenSource(const QString &source) const;
    // What can be saved: {retained, encrypted}.
    Q_INVOKABLE QVariantMap exportInfo(qint64 msgId) const;
    // The message as a .eml: as received or sent, or decrypted.
    Q_INVOKABLE bool saveMessage(qint64 msgId, const QUrl &dest, bool decrypted);
    Q_INVOKABLE QString suggestedEmlName(qint64 msgId) const;
    Q_INVOKABLE void openLink(const QString &url);
    Q_INVOKABLE QString suggestedFileName(qint64 msgId, int index) const;

    Q_INVOKABLE QVariantList contacts(const QString &filter) const;
    Q_INVOKABLE QVariantMap contact(qint64 id) const;
    Q_INVOKABLE void setContactName(qint64 id, const QString &name);
    // -1 inherits the account's mode; 0 lenient, 1 opportunistic, 2 strict.
    Q_INVOKABLE void setContactEncryption(qint64 id, int mode);
    // The address book. createContact returns the new id, or 0.
    Q_INVOKABLE qint64 createContact(const QString &addr, const QString &name);
    // name, organization, title, notes, birthday, phones: [{label, number}]
    Q_INVOKABLE void setContactDetails(qint64 id, const QVariantMap &details);
    Q_INVOKABLE void removeContact(qint64 id);
    Q_INVOKABLE void importContacts(const QUrl &file);
    // The address book, format "vcf" or "csv".
    Q_INVOKABLE void exportContacts(const QUrl &file, const QString &format);
    Q_INVOKABLE void exportContact(qint64 id, const QUrl &file);
    Q_INVOKABLE QString suggestedContactFileName(qint64 id) const;
    Q_INVOKABLE QStringList completeAddress(const QString &prefix) const;

    Q_INVOKABLE QVariantMap settings() const;
    QVariantMap refusedCertificate() const;

    bool devicesAvailable() const;
    bool scanAvailable() const;
    QVariantList devices() const;
    QString deviceName() const;
    QString removedBy() const;
    Q_INVOKABLE void dismissRemoved();
    void setDeviceName(const QString &name);
    QString relay() const;
    void setRelay(const QString &relay);
    // Pairing, on the device that has the account: a code to show, or
    // empty if the endpoint did not start. Then pairingRequested, allow or
    // deny, and pairingFinished.
    Q_INVOKABLE QString startPairing();
    Q_INVOKABLE void allowPairing();
    Q_INVOKABLE void denyPairing();
    Q_INVOKABLE void cancelPairing();
    Q_INVOKABLE void removeDevice(const QString &id);
    // On the new device: the account a code is for (empty if it is not a
    // code), and joining it.
    Q_INVOKABLE QString codeAccount(const QString &code) const;
    Q_INVOKABLE bool joinDevice(const QString &code, const QString &deviceName);
    Q_INVOKABLE void cancelJoin();
    Q_INVOKABLE QString defaultDeviceName() const;
    Q_INVOKABLE void trustRefusedCertificate();
    // "in" or "smtp": back to TLS's own judgement.
    Q_INVOKABLE void forgetCertificate(const QString &server);
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);

    QVariantList signatures() const { return m_signatures; }
    QString defaultSignature() const;
    void setDefaultSignature(const QString &id);
    // The signature editor's document: filled from, and saved to, a
    // signature. saveSignature returns the id (new when `id` is empty).
    Q_INVOKABLE void loadSignature(const QString &id, QQuickTextDocument *doc) const;
    Q_INVOKABLE QString saveSignature(const QString &id, const QString &name, QQuickTextDocument *doc);
    Q_INVOKABLE void removeSignature(const QString &id);
    Q_INVOKABLE QString signatureName(const QString &id) const;

    QString language() const;
    void setLanguage(const QString &code);
    QString notificationMode() const;
    void setNotificationMode(const QString &mode);
    bool notificationsAvailable() const;
    bool backgroundSync() const;
    void setBackgroundSync(bool on);
    bool backgroundSyncAvailable() const;
    QVariantList languages() const;

Q_SIGNALS:
    void firstRunChanged();
    void accountsChanged();
    void accountChanged();
    void statusChanged();
    void viewChanged();
    void labelsChanged();
    void countsChanged();
    void signaturesChanged();
    void devicesChanged();
    void pairingRequested(const QString &deviceName);
    void pairingProgress(int percent);
    void pairingFinished(bool ok, const QString &message);
    void joinWaiting();
    void joinProgress(int percent);
    void joinFinished(bool ok, const QString &message);
    void selectionChanged();
    void notify(const QString &message);
    void newMailArrived(const QString &from, const QString &subject);
    // Translators were replaced: the QML engine must retranslate.
    void languageChanged();
    void notificationsChanged();
    void backgroundSyncChanged();
    // A notification was clicked: the window should come to the front.
    void raiseRequested();

private:
    void switchAccount();
    void reloadMessages();
    void reloadLabels();
    void reloadCounts();
    void reloadThread();
    void reloadSignatures();
    QVariantMap messageMap(qint64 id) const;
    void scheduleRefresh();
    void wireNotifications();

    static MailApp *s_instance;
    e3::AccountManager *m_mgr;
    QPointer<e3::Account> m_connected;
    MessageListModel m_messages;
    QString m_tag;
    qint64 m_labelId = 0;
    QString m_search;
    QVariantList m_labels;
    QVariantMap m_counts;
    QVariantList m_signatures;
    QPointer<QObject> m_offer; // a PairingOffer
    QPointer<QObject> m_join;  // a PairingJoin
    qint64 m_selected = 0;
    QVariantList m_thread;
    QTimer m_refresh;
    Notifier *m_notifier;
};
