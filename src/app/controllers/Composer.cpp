// SPDX-License-Identifier: MPL-2.0
#include "Composer.h"

#include "MailApp.h"
#include "RichText.h"
#include "engine/Account.h"
#include "mail/Compose.h"
#include "mail/Policy.h"
#include "mime/Html.h"
#include "store/Config.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>

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
    // appended invisibly at send time.
    QString sig;
    if (Account *a = currentAccount(); a && !d.id) {
        const QString configured = a->ctx().config.get(cfg::Signature);
        if (!configured.trimmed().isEmpty())
            sig = QStringLiteral("\n\n-- \n") + configured;
    }
    m_initialText = sig + d.text;
    // With HTML (a reply to HTML mail, a formatted draft), the signature goes
    // in as plain lines above it.
    m_initialHtml = d.html;
    m_initialSignature = sig;
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
        m_initialText = d.text; // its signature is already in the body
        Q_EMIT draftChanged();
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
    if (m_initialHtml.isEmpty()) {
        doc->textDocument()->setPlainText(m_initialText);
    } else {
        RichText::load(doc->textDocument(), m_initialHtml);
        if (!m_initialSignature.isEmpty()) {
            QTextCursor s(doc->textDocument());
            s.movePosition(QTextCursor::Start);
            s.insertText(m_initialSignature); // its last line ends in the empty line it lands on
        }
    }
    QTextCursor c(doc->textDocument());
    c.movePosition(QTextCursor::Start);
}

void Composer::toggleFormat(QQuickTextDocument *doc, int start, int end, const QString &kind)
{
    if (!doc || start == end)
        return;
    QTextCursor c(doc->textDocument());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    const QTextCharFormat current = c.charFormat();
    QTextCharFormat f;
    if (kind == QLatin1String("bold"))
        f.setFontWeight(current.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
    else if (kind == QLatin1String("italic"))
        f.setFontItalic(!current.fontItalic());
    else if (kind == QLatin1String("underline"))
        f.setFontUnderline(!current.fontUnderline());
    else if (kind == QLatin1String("strike"))
        f.setFontStrikeOut(!current.fontStrikeOut());
    else if (kind == QLatin1String("code")) {
        const bool mono = current.fontFixedPitch();
        f.setFontFixedPitch(!mono);
        f.setFontFamilies({mono ? QString() : QStringLiteral("monospace")});
    }
    c.mergeCharFormat(f);
}

void Composer::setBlock(QQuickTextDocument *doc, int start, int end, const QString &kind)
{
    if (!doc)
        return;
    QTextCursor c(doc->textDocument());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.beginEditBlock();
    QTextBlockFormat bf = c.blockFormat();
    const int heading = kind.startsWith(u'h') ? kind.mid(1).toInt() : 0;
    const bool toggleOff = (heading && bf.headingLevel() == heading)
        || (kind == QLatin1String("quote") && bf.intProperty(RichText::RoleProperty) == RichText::Quote)
        || (kind == QLatin1String("pre") && bf.intProperty(RichText::RoleProperty) == RichText::Code);
    bf.setHeadingLevel(toggleOff ? 0 : heading);
    bf.setProperty(RichText::RoleProperty, toggleOff ? int(RichText::Paragraph)
                                           : kind == QLatin1String("quote") ? int(RichText::Quote)
                                           : kind == QLatin1String("pre")   ? int(RichText::Code)
                                                                            : int(RichText::Paragraph));
    bf.setLeftMargin(!toggleOff && kind == QLatin1String("quote") ? 16 : 0);
    if (kind == QLatin1String("bullet") || kind == QLatin1String("numbered")) {
        const auto style = kind == QLatin1String("bullet") ? QTextListFormat::ListDisc : QTextListFormat::ListDecimal;
        if (QTextList *list = c.currentList(); list && list->format().style() == style) {
            for (int i = list->count() - 1; i >= 0; --i)
                list->remove(list->item(i));
            bf.setIndent(0);
            c.setBlockFormat(bf);
        } else {
            c.setBlockFormat(bf);
            QTextListFormat lf;
            lf.setStyle(style);
            c.createList(lf);
        }
    } else {
        c.setBlockFormat(bf);
        QTextCharFormat cf;
        if (heading && !toggleOff)
            cf.setFontWeight(QFont::Bold);
        if (kind == QLatin1String("pre")) {
            cf.setFontFixedPitch(!toggleOff);
            cf.setFontFamilies({toggleOff ? QString() : QStringLiteral("monospace")});
        }
        if (heading || kind == QLatin1String("pre"))
            c.mergeBlockCharFormat(cf);
    }
    c.endEditBlock();
}

void Composer::setLink(QQuickTextDocument *doc, int start, int end, const QString &url)
{
    if (!doc || start == end)
        return;
    // The scheme is supplied here, where the user typed the address, so that
    // safeHref stays a whitelist with no guessing in it.
    QString target = url.trimmed();
    if (target.contains(u'@') && !target.contains(u':'))
        target.prepend(QStringLiteral("mailto:"));
    else if (!target.contains(QLatin1String("://")) && !target.startsWith(QLatin1String("mailto:")))
        target.prepend(QStringLiteral("https://"));
    const QString safe = mime::safeHref(target);
    QTextCursor c(doc->textDocument());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    QTextCharFormat f;
    f.setAnchor(!safe.isEmpty());
    f.setAnchorHref(safe);
    f.setFontUnderline(!safe.isEmpty());
    c.mergeCharFormat(f);
}

void Composer::insertSignature(QQuickTextDocument *doc, int position)
{
    Account *a = currentAccount();
    if (!doc || !a)
        return;
    const QString sig = a->ctx().config.get(cfg::Signature);
    if (sig.trimmed().isEmpty()) {
        Q_EMIT failed(tr("No signature is set. Add one in Settings."));
        return;
    }
    QTextCursor c(doc->textDocument());
    c.setPosition(qBound(0, position, doc->textDocument()->characterCount() - 1));
    c.insertText(QStringLiteral("\n-- \n") + sig + u'\n');
}

QString Composer::formatAt(QQuickTextDocument *doc, int position) const
{
    if (!doc)
        return {};
    QTextCursor c(doc->textDocument());
    c.setPosition(qBound(0, position, doc->textDocument()->characterCount() - 1));
    const QTextCharFormat f = c.charFormat();
    QStringList on;
    if (f.fontWeight() >= QFont::Bold)
        on << QStringLiteral("bold");
    if (f.fontItalic())
        on << QStringLiteral("italic");
    if (f.fontUnderline())
        on << QStringLiteral("underline");
    if (f.fontStrikeOut())
        on << QStringLiteral("strike");
    return on.join(u' ');
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
