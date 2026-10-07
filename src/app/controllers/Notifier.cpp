// SPDX-License-Identifier: MPL-2.0
#include "Notifier.h"

#include <QTimer>

Notifier::Notifier(std::unique_ptr<Backend> backend, QObject *parent) : QObject(parent), m_backend(std::move(backend))
{
    if (m_backend) {
        connect(m_backend.get(), &Backend::clicked, this, [this](quint64 token) {
            // Not one of ours (the tray icon itself): just bring the window up.
            const auto it = m_targets.constFind(token);
            Q_EMIT activated(it != m_targets.cend() ? it->first : -1, it != m_targets.cend() ? it->second : 0);
        });
    }
}

Notifier::~Notifier() = default;

bool Notifier::available() const
{
    return m_backend && m_backend->available();
}

QString Notifier::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Off:
        return QStringLiteral("off");
    case Mode::Full:
        return QStringLiteral("full");
    case Mode::Sender:
        break;
    }
    return QStringLiteral("sender");
}

Notifier::Mode Notifier::modeFromString(const QString &s)
{
    if (s == QLatin1String("off"))
        return Mode::Off;
    if (s == QLatin1String("full"))
        return Mode::Full;
    return Mode::Sender;
}

void Notifier::arrived(int accountId, const QString &accountAddr, qint64 msgId, const QString &from,
                       const QString &subject)
{
    if (m_mode == Mode::Off || !available())
        return;
    Pending &p = m_pending[accountId];
    p.accountAddr = accountAddr;
    p.arrivals.append({msgId, from, subject});
    if (!p.timer) {
        p.timer = new QTimer(this);
        p.timer->setSingleShot(true);
        connect(p.timer, &QTimer::timeout, this, [this, accountId] { flush(accountId); });
    }
    if (!p.timer->isActive())
        p.timer->start(m_gatherMs);
}

void Notifier::flush(int accountId)
{
    auto it = m_pending.find(accountId);
    if (it == m_pending.end() || it->arrivals.isEmpty())
        return;
    const QList<Arrival> arrivals = std::exchange(it->arrivals, {});
    const QString accountAddr = it->accountAddr;
    // Looked at when it would be shown, not when mail came: the person may
    // have switched away meanwhile.
    if (m_mode == Mode::Off || (m_windowActive && m_windowActive()))
        return;

    QString title;
    QString body;
    if (arrivals.size() == 1) {
        const Arrival &a = arrivals.first();
        title = a.from;
        body = m_mode == Mode::Full && !a.subject.trimmed().isEmpty() ? a.subject : tr("New message");
    } else {
        title = tr("%n new messages", "", int(arrivals.size()));
        QStringList senders;
        for (const Arrival &a : arrivals) {
            if (!senders.contains(a.from))
                senders.append(a.from);
        }
        if (senders.size() > 3) {
            const int more = int(senders.size()) - 3;
            senders = senders.mid(0, 3);
            body = tr("From %1 and %n more", "", more).arg(senders.join(QStringLiteral(", ")));
        } else {
            body = tr("From %1").arg(senders.join(QStringLiteral(", ")));
        }
    }
    if (m_accountCount > 1)
        body += u'\n' + accountAddr;

    const quint64 token = m_nextToken++;
    m_targets.insert(token, {accountId, arrivals.size() == 1 ? arrivals.first().msgId : 0});
    m_targets.remove(token - 100); // old notifications are long gone from the screen
    m_backend->show(title, body, token);
}
