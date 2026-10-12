// SPDX-License-Identifier: MPL-2.0
#include "Composer.h"

#include "MailApp.h"
#include "RichText.h"
#include "engine/Account.h"
#include "mail/Compose.h"
#include "mail/Policy.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QTextDocument>

using namespace e3;

namespace {
e3::Account *currentAccount()
{
    MailApp *app = MailApp::create(nullptr, nullptr);
    return app ? app->account() : nullptr;
}
} // namespace

Composer::Composer(QObject *parent) : QObject(parent)
{
    m_evaluate.setSingleShot(true);
    m_evaluate.setInterval(250);
    connect(&m_evaluate, &QTimer::timeout, this, &Composer::evaluate);
    startNew();
}

QList<mime::Address> Composer::parse(const QString &s)
{
    return mime::parseAddressList(s.toUtf8());
}

QString Composer::format(const QList<mime::Address> &list)
{
    QStringList out;
    for (const auto &a : list)
        out.append(a.display());
    return out.join(QStringLiteral(", "));
}

void Composer::setTo(const QString &v)
{
    if (v == m_to)
        return;
    m_to = v;
    Q_EMIT recipientsChanged();
    m_evaluate.start();
}

void Composer::setCc(const QString &v)
{
    if (v == m_cc)
        return;
    m_cc = v;
    Q_EMIT recipientsChanged();
    m_evaluate.start();
}

void Composer::setBcc(const QString &v)
{
    if (v == m_bcc)
        return;
    m_bcc = v;
    Q_EMIT recipientsChanged();
    m_evaluate.start();
}

void Composer::setEncrypt(bool on)
{
    m_userSetPadlock = true;
    m_encrypt = on;
    evaluate();
}

QVariantList Composer::attachments() const
{
    QVariantList out;
    for (const auto &a : m_draft.attachments)
        out.append(QVariantMap{{QStringLiteral("name"), a.filename},
                               {QStringLiteral("size"), QLocale().formattedDataSize(a.data.size())}});
    return out;
}

void Composer::load(const Draft &d, const QString &title)
{
    m_draft = d;
    m_to = format(d.to);
    m_cc = format(d.cc);
    m_bcc = format(d.bcc);
    m_subject = d.subject;
    m_important = d.importance > 0;
    m_title = title;
    m_userSetPadlock = d.encryption != SendEncryption::Auto;
    m_encrypt = d.encryption == SendEncryption::Required;
    // Signature goes in the body where it can be seen and edited, not
    // appended invisibly at send time. A draft has its own already.
    m_initialSignature.reset();
    if (Account *a = currentAccount(); a && !d.id)
        m_initialSignature = mail::Signatures::defaultSignature(a->ctx());
    m_initialText = d.text;
    m_initialHtml = d.html;
    Q_EMIT recipientsChanged();
    Q_EMIT subjectChanged();
    Q_EMIT optionsChanged();
    Q_EMIT attachmentsChanged();
    Q_EMIT draftChanged();
    evaluate();
}

void Composer::startNew()
{
    load(Draft(), tr("New message"));
}

void Composer::startReply(qint64 msgId, bool all)
{
    if (Account *a = currentAccount())
        load(mail::Compose::reply(a->ctx(), msgId, all), all ? tr("Reply all") : tr("Reply"));
}

void Composer::startForward(qint64 msgId)
{
    if (Account *a = currentAccount())
        load(mail::Compose::forward(a->ctx(), msgId), tr("Forward"));
}

void Composer::openDraft(qint64 msgId)
{
    if (Account *a = currentAccount()) {
        Draft d = mail::Compose::loadDraft(a->ctx(), msgId);
        load(d, tr("Draft"));
    }
}

void Composer::attach(const QList<QUrl> &files)
{
    for (const QUrl &url : files) {
        QFile f(url.toLocalFile());
        if (!f.open(QIODevice::ReadOnly)) {
            Q_EMIT failed(tr("Cannot read %1.").arg(url.toLocalFile()));
            continue;
        }
        m_draft.attachments.append({QFileInfo(f).fileName(), QMimeDatabase().mimeTypeForFile(f.fileName()).name(),
                                    f.readAll()});
    }
    Q_EMIT attachmentsChanged();
}

void Composer::removeAttachment(int index)
{
    if (index >= 0 && index < m_draft.attachments.size()) {
        m_draft.attachments.removeAt(index);
        Q_EMIT attachmentsChanged();
    }
}

void Composer::loadInto(QQuickTextDocument *doc)
{
    if (!doc)
        return;
    QTextDocument *td = doc->textDocument();
    if (m_initialHtml.isEmpty())
        td->setPlainText(m_initialText);
    else
        RichText::load(td, m_initialHtml);
    m_signature.clear();
    if (m_initialSignature) {
        // Two empty lines to write in, then the signature, then the quote
        // (which starts with empty lines of its own).
        QTextCursor c(td);
        c.beginEditBlock();
        c.insertText(QStringLiteral("\n\n"));
        m_signature.place(c, m_initialSignature->html, m_initialSignature->text);
        c.endEditBlock();
    }
}

void Composer::useSignature(QQuickTextDocument *doc, const QString &id, int position)
{
    Account *a = currentAccount();
    if (!doc || !a)
        return;
    const std::optional<mail::Signature> sig = mail::Signatures::get(a->ctx(), id);
    if (!id.isEmpty() && !sig)
        return;
    QTextDocument *td = doc->textDocument();
    QTextCursor c(td);
    c.beginEditBlock();
    if (m_signature.take(c)) {
        if (sig)
            m_signature.place(c, sig->html, sig->text);
    } else if (sig) {
        // Edited, or never put in here: a new one goes in at the cursor.
        c.setPosition(qBound(0, position, td->characterCount() - 1));
        c.insertBlock();
        m_signature.place(c, sig->html, sig->text);
        c.insertBlock();
    }
    c.endEditBlock();
}

Draft Composer::collect(QQuickTextDocument *doc) const
{
    Draft d = m_draft;
    d.to = parse(m_to);
    d.cc = parse(m_cc);
    d.bcc = parse(m_bcc);
    d.subject = m_subject.trimmed();
    d.importance = m_important ? 1 : 0;
    // The composer always decides; it never sends "auto".
    d.encryption = m_encrypt ? SendEncryption::Required : SendEncryption::Plaintext;
    if (doc) {
        const RichText::Output out = RichText::emit(doc->textDocument());
        d.text = out.text;
        d.html = out.html;
    }
    return d;
}

void Composer::evaluate()
{
    Account *a = currentAccount();
    if (!a)
        return;
    QStringList rcpts;
    for (const auto &list : {parse(m_to), parse(m_cc), parse(m_bcc)})
        for (const auto &x : list)
            rcpts.append(x.addr);
    // What would policy alone do? That is the padlock's default position.
    const auto autoR = mail::Policy::evaluate(a->ctx(), rcpts, SendEncryption::Auto);
    m_padlockLocked = autoR.padlockLocked;
    if (!m_userSetPadlock || m_padlockLocked)
        m_encrypt = autoR.willEncrypt || autoR.padlockLocked;
    const auto r = mail::Policy::evaluate(a->ctx(), rcpts,
                                          m_encrypt ? SendEncryption::Required : SendEncryption::Plaintext);
    m_canSend = r.canSend && !rcpts.isEmpty();
    if (rcpts.isEmpty())
        m_readiness = tr("Add a recipient.");
    else if (!r.canSend)
        m_readiness = r.refusal;
    else if (m_encrypt)
        m_readiness = tr("Will be sent end-to-end encrypted.");
    else if (!r.missingKeys.isEmpty())
        m_readiness = tr("Will be sent unencrypted: no key for %1.").arg(r.missingKeys.join(QStringLiteral(", ")));
    else
        m_readiness = tr("Will be sent unencrypted.");
    Q_EMIT readinessChanged();
}

bool Composer::saveDraft(QQuickTextDocument *doc)
{
    Account *a = currentAccount();
    if (!a)
        return false;
    try {
        m_draft.id = mail::Compose::saveDraft(a->ctx(), collect(doc));
        a->notifyChanged();
        Q_EMIT draftChanged();
        return true;
    } catch (const std::exception &e) {
        Q_EMIT failed(QString::fromUtf8(e.what()));
        return false;
    }
}

bool Composer::send(QQuickTextDocument *doc)
{
    Account *a = currentAccount();
    if (!a)
        return false;
    try {
        mail::Compose::queue(a->ctx(), collect(doc));
        a->notifyChanged();
        a->syncNow();
        Q_EMIT sent();
        return true;
    } catch (const mail::ComposeError &e) {
        Q_EMIT failed(e.message());
    } catch (const std::exception &e) {
        Q_EMIT failed(QString::fromUtf8(e.what()));
    }
    return false;
}

void Composer::discard()
{
    if (Account *a = currentAccount(); a && m_draft.id) {
        mail::Compose::deleteDraft(a->ctx(), m_draft.id);
        a->notifyChanged();
    }
    startNew();
}
