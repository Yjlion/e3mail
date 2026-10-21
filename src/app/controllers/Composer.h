// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "SignatureRange.h"
#include "mail/Signatures.h"
#include "mail/Types.h"

#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <QQuickTextDocument>

#include <optional>

class Composer : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString to READ to WRITE setTo NOTIFY recipientsChanged)
    Q_PROPERTY(QString cc READ cc WRITE setCc NOTIFY recipientsChanged)
    Q_PROPERTY(QString bcc READ bcc WRITE setBcc NOTIFY recipientsChanged)
    Q_PROPERTY(QString subject MEMBER m_subject NOTIFY subjectChanged)
    Q_PROPERTY(bool important MEMBER m_important NOTIFY optionsChanged)
    // Read receipt asked for, and how long it lives once read (0: kept).
    Q_PROPERTY(bool requestReceipt MEMBER m_requestReceipt NOTIFY optionsChanged)
    Q_PROPERTY(int disappearSeconds MEMBER m_disappearSeconds NOTIFY optionsChanged)
    Q_PROPERTY(bool encrypt READ encrypt WRITE setEncrypt NOTIFY readinessChanged)
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY attachmentsChanged)
    Q_PROPERTY(qint64 draftId READ draftId NOTIFY draftChanged)
    Q_PROPERTY(QString initialText READ initialText NOTIFY draftChanged)
    Q_PROPERTY(QString title READ title NOTIFY draftChanged)
    // Readiness, recomputed as the user types.
    Q_PROPERTY(bool canSend READ canSend NOTIFY readinessChanged)
    Q_PROPERTY(bool padlockLocked READ padlockLocked NOTIFY readinessChanged)
    Q_PROPERTY(QString readiness READ readiness NOTIFY readinessChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit Composer(QObject *parent = nullptr);

    QString to() const { return m_to; }
    QString cc() const { return m_cc; }
    QString bcc() const { return m_bcc; }
    void setTo(const QString &v);
    void setCc(const QString &v);
    void setBcc(const QString &v);
    bool encrypt() const { return m_encrypt; }
    void setEncrypt(bool on);
    QVariantList attachments() const;
    qint64 draftId() const { return m_draft.id; }
    QString initialText() const { return m_initialText; }
    QString title() const { return m_title; }
    bool canSend() const { return m_canSend; }
    bool padlockLocked() const { return m_padlockLocked; }
    QString readiness() const { return m_readiness; }
    bool busy() const { return m_busy; }

    Q_INVOKABLE void startNew();
    Q_INVOKABLE void startReply(qint64 msgId, bool all);
    Q_INVOKABLE void startForward(qint64 msgId);
    Q_INVOKABLE void openDraft(qint64 msgId);
    Q_INVOKABLE void attach(const QList<QUrl> &files);
    Q_INVOKABLE void removeAttachment(int index);

    // The text document lives in QML; these act on it. Formatting is
    // TextFormatter's.
    Q_INVOKABLE void loadInto(QQuickTextDocument *doc);
    // Swaps the signature this composer put in for another (an empty id
    // takes it out), if it is still as it was put in; otherwise puts the
    // signature in at `position`, leaving what was edited alone.
    Q_INVOKABLE void useSignature(QQuickTextDocument *doc, const QString &id, int position);

    Q_INVOKABLE bool saveDraft(QQuickTextDocument *doc);
    Q_INVOKABLE bool send(QQuickTextDocument *doc);
    Q_INVOKABLE void discard();

Q_SIGNALS:
    void recipientsChanged();
    void subjectChanged();
    void optionsChanged();
    void attachmentsChanged();
    void draftChanged();
    void readinessChanged();
    void busyChanged();
    void sent();
    void failed(const QString &message);

private:
    void load(const e3::Draft &d, const QString &title);
    e3::Draft collect(QQuickTextDocument *doc) const;
    void evaluate();
    static QList<e3::mime::Address> parse(const QString &s);
    static QString format(const QList<e3::mime::Address> &list);

    e3::Draft m_draft;
    QString m_to, m_cc, m_bcc, m_subject, m_initialText, m_title;
    QString m_initialHtml; // a draft or reply with HTML
    std::optional<e3::mail::Signature> m_initialSignature;
    SignatureRange m_signature;
    bool m_important = false;
    bool m_requestReceipt = false;
    int m_disappearSeconds = 0;
    bool m_encrypt = false;
    bool m_userSetPadlock = false;
    bool m_canSend = false;
    bool m_padlockLocked = false;
    bool m_busy = false;
    QString m_readiness;
    QTimer m_evaluate;
};
