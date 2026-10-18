// SPDX-License-Identifier: MPL-2.0
#include "Sftp.h"
#include "net/SyncSocket.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QTimeZone>

#include <fcntl.h>
#include <sys/stat.h>

namespace e3::backup {

struct SftpDestination::Session
{
    ssh_session ssh = nullptr;
    sftp_session sftp = nullptr;
    ~Session()
    {
        if (sftp)
            sftp_free(sftp);
        if (ssh) {
            ssh_disconnect(ssh);
            ssh_free(ssh);
        }
    }
};

SftpDestination::SftpDestination(const Options &o) : m_o(o) {}
SftpDestination::~SftpDestination() = default;

QByteArray SftpDestination::remote(const QString &name) const
{
    QString base = m_o.path;
    while (base.endsWith(u'/') && base.size() > 1)
        base.chop(1);
    return (base.isEmpty() ? name : base + u'/' + name).toUtf8();
}

void SftpDestination::open()
{
    if (m_s)
        return;
    auto s = std::make_unique<Session>();
    s->ssh = ssh_new();
    const QByteArray host = m_o.host.toUtf8(), user = m_o.user.toUtf8();
    unsigned int port = m_o.port;
    long timeout = 30;
    ssh_options_set(s->ssh, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(s->ssh, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s->ssh, SSH_OPTIONS_USER, user.constData());
    ssh_options_set(s->ssh, SSH_OPTIONS_TIMEOUT, &timeout);
    if (ssh_connect(s->ssh) != SSH_OK)
        throw net::NetError(QStringLiteral("SFTP: cannot connect to %1: %2").arg(m_o.host, QString::fromUtf8(ssh_get_error(s->ssh))));

    // The host key, by its SHA-256, against the one pinned: never the user's
    // known_hosts, which the person may not keep for this server.
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(s->ssh, &key) != SSH_OK)
        throw net::NetError(QStringLiteral("SFTP: the server shows no host key"));
    unsigned char *hash = nullptr;
    size_t len = 0;
    const int rc = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &len);
    ssh_key_free(key);
    if (rc != 0)
        throw net::NetError(QStringLiteral("SFTP: cannot read the host key"));
    const QByteArray fingerprint = "SHA256:" + QByteArray(reinterpret_cast<const char *>(hash), qsizetype(len))
                                                   .toBase64(QByteArray::OmitTrailingEquals);
    ssh_clean_pubkey_hash(&hash);
    if (fingerprint != m_o.hostKey)
        throw HostKeyError(fingerprint, !m_o.hostKey.isEmpty());

    const QByteArray secret = m_o.password.toUtf8();
    int auth = SSH_AUTH_DENIED;
    if (!m_o.keyFile.isEmpty()) {
        ssh_key priv = nullptr;
        if (ssh_pki_import_privkey_file(m_o.keyFile.toUtf8().constData(), secret.isEmpty() ? nullptr : secret.constData(),
                                        nullptr, nullptr, &priv) != SSH_OK)
            throw net::AuthError(QStringLiteral("SFTP: cannot read the key file %1").arg(m_o.keyFile));
        auth = ssh_userauth_publickey(s->ssh, nullptr, priv);
        ssh_key_free(priv);
    } else {
        auth = ssh_userauth_password(s->ssh, nullptr, secret.constData());
    }
    if (auth != SSH_AUTH_SUCCESS)
        throw net::AuthError(QStringLiteral("SFTP: %1 refused the user name, password or key").arg(m_o.host));
    s->sftp = sftp_new(s->ssh);
    if (!s->sftp || sftp_init(s->sftp) != SSH_OK)
        throw net::NetError(QStringLiteral("SFTP: the server offers no SFTP"));
    m_s = std::move(s);
}

void SftpDestination::prepare()
{
    open();
    for (const QString &d : {QString(), QStringLiteral("objects"), QStringLiteral("snapshots")}) {
        const QByteArray p = d.isEmpty() ? remote(QString()).chopped(remote(QString()).endsWith('/') ? 1 : 0) : remote(d);
        if (p.isEmpty())
            continue;
        if (sftp_mkdir(m_s->sftp, p.constData(), 0700) != SSH_OK && sftp_get_error(m_s->sftp) != SSH_FX_FILE_ALREADY_EXISTS) {
            sftp_attributes a = sftp_stat(m_s->sftp, p.constData());
            if (!a)
                throw net::NetError(QStringLiteral("SFTP: cannot create %1").arg(QString::fromUtf8(p)));
            sftp_attributes_free(a);
        }
    }
}

void SftpDestination::put(const QString &name, const QByteArray &data)
{
    open();
    // Written beside, then renamed: a broken connection never leaves half an
    // object under the real name.
    const QByteArray path = remote(name), part = path + ".part";
    sftp_file f = sftp_open(m_s->sftp, part.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!f)
        throw net::NetError(QStringLiteral("SFTP: cannot store %1").arg(name));
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t n = sftp_write(f, data.constData() + done, size_t(qMin<qsizetype>(data.size() - done, 64 * 1024)));
        if (n <= 0) {
            sftp_close(f);
            throw net::NetError(QStringLiteral("SFTP: writing %1 failed").arg(name));
        }
        done += n;
    }
    sftp_close(f);
    sftp_unlink(m_s->sftp, path.constData()); // SFTP's rename does not replace
    if (sftp_rename(m_s->sftp, part.constData(), path.constData()) != SSH_OK)
        throw net::NetError(QStringLiteral("SFTP: cannot store %1").arg(name));
}

std::optional<QByteArray> SftpDestination::get(const QString &name)
{
    open();
    sftp_file f = sftp_open(m_s->sftp, remote(name).constData(), O_RDONLY, 0);
    if (!f) {
        if (sftp_get_error(m_s->sftp) == SSH_FX_NO_SUCH_FILE)
            return std::nullopt;
        throw net::NetError(QStringLiteral("SFTP: cannot read %1").arg(name));
    }
    QByteArray out;
    char buf[64 * 1024];
    while (true) {
        const ssize_t n = sftp_read(f, buf, sizeof buf);
        if (n < 0) {
            sftp_close(f);
            throw net::NetError(QStringLiteral("SFTP: reading %1 failed").arg(name));
        }
        if (n == 0)
            break;
        out.append(buf, n);
    }
    sftp_close(f);
    return out;
}

QList<Entry> SftpDestination::list(const QString &dir)
{
    open();
    QList<Entry> out;
    sftp_dir d = sftp_opendir(m_s->sftp, remote(dir).constData());
    if (!d)
        return out;
    while (sftp_attributes a = sftp_readdir(m_s->sftp, d)) {
        const QString name = QString::fromUtf8(a->name);
        if (a->type == SSH_FILEXFER_TYPE_REGULAR && !name.endsWith(QLatin1String(".part")))
            out.append({name, qint64(a->size), QDateTime::fromSecsSinceEpoch(qint64(a->mtime), QTimeZone::UTC)});
        sftp_attributes_free(a);
    }
    sftp_closedir(d);
    return out;
}

void SftpDestination::remove(const QString &name)
{
    open();
    sftp_unlink(m_s->sftp, remote(name).constData());
}

} // namespace e3::backup
