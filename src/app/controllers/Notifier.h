// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QTimer;

// Where notifications go: the freedesktop notification service on Linux, the
// system tray elsewhere, a recorder in tests.
class NotifierBackend : public QObject
{
    Q_OBJECT
public:
    virtual bool available() const = 0;
    // `token` comes back through clicked() when the person clicks it.
    virtual void show(const QString &title, const QString &body, quint64 token) = 0;
Q_SIGNALS:
    void clicked(quint64 token);
};

// Desktop notifications for new mail. Arrivals are gathered per account for a
// moment, so a burst becomes one notification. Nothing is shown while the
// window is active, or when the person turned notifications off. What the
// notification says is their choice: subjects of encrypted mail are protected
// on the wire, and a notification puts them into the system's history.
class Notifier : public QObject
{
    Q_OBJECT
public:
    enum class Mode { Off, Sender, Full }; // Full: sender and subject

    using Backend = NotifierBackend;

    static constexpr int kGatherMs = 2000;

    explicit Notifier(std::unique_ptr<Backend> backend, QObject *parent = nullptr);
    ~Notifier() override;

    bool available() const;
    Mode mode() const { return m_mode; }
    void setMode(Mode mode) { m_mode = mode; }
    static QString modeToString(Mode mode);
    static Mode modeFromString(const QString &s); // unknown: Sender
    // Whether the app's window is in front; nothing is shown then.
    void setWindowActive(std::function<bool()> active) { m_windowActive = std::move(active); }
    // With several accounts, notifications name the account.
    void setAccountCount(int n) { m_accountCount = n; }
    void setGatherMs(int ms) { m_gatherMs = ms; }

    void arrived(int accountId, const QString &accountAddr, qint64 msgId, const QString &from, const QString &subject);

Q_SIGNALS:
    // The person clicked: open this message, or the account's Inbox (msgId 0).
    // accountId is -1 for the tray icon itself: just bring the window up.
    void activated(int accountId, qint64 msgId);

private:
    struct Arrival
    {
        qint64 msgId;
        QString from, subject;
    };
    struct Pending
    {
        QString accountAddr;
        QList<Arrival> arrivals;
        QTimer *timer = nullptr;
    };
    void flush(int accountId);

    std::unique_ptr<Backend> m_backend;
    Mode m_mode = Mode::Sender;
    std::function<bool()> m_windowActive;
    int m_accountCount = 1;
    int m_gatherMs = kGatherMs;
    QHash<int, Pending> m_pending;
    quint64 m_nextToken = 1;
    QHash<quint64, QPair<int, qint64>> m_targets; // token -> account, message
};
