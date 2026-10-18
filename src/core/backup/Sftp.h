// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Destination.h"

#include <QByteArray>
#include <stdexcept>

namespace e3::backup {

// The server's host key is not the one pinned for it, or none is pinned yet:
// shown to the person, who pins it (as ADR 0017 does for TLS).
class HostKeyError : public std::runtime_error
{
public:
    HostKeyError(const QByteArray &sha256Base64, bool changed)
        : std::runtime_error(changed ? "the server's host key changed" : "the server's host key is not trusted yet"),
          sha256(sha256Base64), changed(changed)
    {
    }
    QByteArray sha256; // "SHA256:…", as ssh-keygen -l shows it
    bool changed;
};

// A folder on an SSH server, through libssh's SFTP.
class SftpDestination : public Destination
{
public:
    struct Options
    {
        QString host;
        quint16 port = 22;
        QString user;
        QString path;       // the folder on the server
        QString password;   // or the key file's passphrase
        QString keyFile;    // a private key; empty for a password
        QByteArray hostKey; // the pinned "SHA256:…"
    };
    explicit SftpDestination(const Options &o);
    ~SftpDestination() override;
    void prepare() override;
    void put(const QString &name, const QByteArray &data) override;
    std::optional<QByteArray> get(const QString &name) override;
    QList<Entry> list(const QString &dir) override;
    void remove(const QString &name) override;

private:
    void open();
    QByteArray remote(const QString &name) const;
    struct Session;
    std::unique_ptr<Session> m_s;
    Options m_o;
};

} // namespace e3::backup
