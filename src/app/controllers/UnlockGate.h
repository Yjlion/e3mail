// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <QString>

// What Unlock.qml asks before any account is opened (ADR 0023): whether a
// passphrase opens the accounts under `root`, which then remember it.
class UnlockGate : public QObject
{
    Q_OBJECT
public:
    explicit UnlockGate(const QString &root, QObject *parent = nullptr) : QObject(parent), m_root(root) {}

    Q_INVOKABLE bool unlock(const QString &passphrase);
    bool isOpen() const { return m_open; }

    // Asks with Unlock.qml until the passphrase is right or the window is
    // closed. `given` (E3MAIL_PASSPHRASE) is tried first. Returns whether the
    // accounts may be opened.
    // With `grab`, saves a picture of the window there instead (screenshots).
    static bool run(const QString &root, const QString &given, const QString &grab = {});

Q_SIGNALS:
    void opened();

private:
    QString m_root;
    bool m_open = false;
};
