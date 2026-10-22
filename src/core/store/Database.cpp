// SPDX-License-Identifier: MPL-2.0
#include "Database.h"

#include <QFile>

#include <sqlite3.h>

namespace e3 {

Statement::Statement(Database &db, const char *sql) : m_db(db)
{
    if (sqlite3_prepare_v2(db.handle(), sql, -1, &m_stmt, nullptr) != SQLITE_OK)
        db.raise(sql);
}

Statement::~Statement()
{
    sqlite3_finalize(m_stmt);
}

Statement &Statement::bind(int idx, std::nullptr_t)
{
    sqlite3_bind_null(m_stmt, idx);
    return *this;
}

Statement &Statement::bind(int idx, qint64 v)
{
    sqlite3_bind_int64(m_stmt, idx, v);
    return *this;
}

Statement &Statement::bind(int idx, double v)
{
    sqlite3_bind_double(m_stmt, idx, v);
    return *this;
}

Statement &Statement::bind(int idx, const QString &v)
{
    // Always text, never NULL: a QString that happens to be null is still a
    // string. Bind std::optional<QString> or nullptr to mean SQL NULL.
    const QByteArray utf8 = v.toUtf8();
    sqlite3_bind_text(m_stmt, idx, utf8.constData(), int(utf8.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement &Statement::bind(int idx, const QByteArray &v)
{
    if (v.isNull()) {
        sqlite3_bind_null(m_stmt, idx);
        return *this;
    }
    sqlite3_bind_blob(m_stmt, idx, v.constData(), int(v.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement &Statement::bind(int idx, const QVariant &v)
{
    if (!v.isValid() || v.isNull())
        return bind(idx, nullptr);
    switch (v.typeId()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return bind(idx, v.toLongLong());
    case QMetaType::Double:
        return bind(idx, v.toDouble());
    case QMetaType::QByteArray:
        return bind(idx, v.toByteArray());
    default:
        return bind(idx, v.toString());
    }
}

bool Statement::step()
{
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    m_db.raise(sqlite3_sql(m_stmt));
}

void Statement::exec()
{
    while (step()) {
    }
}

void Statement::reset()
{
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

bool Statement::isNull(int col) const
{
    return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
}

qint64 Statement::int64(int col) const
{
    return sqlite3_column_int64(m_stmt, col);
}

double Statement::real(int col) const
{
    return sqlite3_column_double(m_stmt, col);
}

QString Statement::text(int col) const
{
    const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, col));
    if (!p)
        return QString();
    return QString::fromUtf8(p, sqlite3_column_bytes(m_stmt, col));
}

QByteArray Statement::blob(int col) const
{
    const void *p = sqlite3_column_blob(m_stmt, col);
    const int n = sqlite3_column_bytes(m_stmt, col);
    if (!p)
        return isNull(col) ? QByteArray() : QByteArray("");
    return QByteArray(static_cast<const char *>(p), n);
}

namespace {
// ChaCha20-Poly1305 (SQLite3 Multiple Ciphers' default cipher) under a
// random 256-bit key: no key stretching is needed, so one round.
QByteArray cipherPragmas(const QByteArray &key, const char *verb)
{
    return "PRAGMA cipher='chacha20'; PRAGMA kdf_iter=1; PRAGMA " + QByteArray(verb) + "='" + key.toHex() + "';";
}
} // namespace

bool Database::isPlaintext(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(16) == QByteArray("SQLite format 3\0", 16);
}

void Database::rekey(const QByteArray &key)
{
    // Pending WAL pages are folded in first, so none is left in the old form,
    // and the rekey itself runs with a rollback journal: SQLite3 Multiple
    // Ciphers cannot encrypt or decrypt a database in WAL mode. Only ever
    // done on a connection that has the file to itself.
    exec("PRAGMA wal_checkpoint(TRUNCATE); PRAGMA journal_mode=DELETE;");
    exec((key.isEmpty() ? QByteArray("PRAGMA rekey='';") : cipherPragmas(key, "rekey")).constData());
    exec("PRAGMA journal_mode=WAL;");
}

Database::Database(const QString &path, const QByteArray &key) : m_path(path)
{
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.toUtf8().constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        const std::string msg = m_db ? sqlite3_errmsg(m_db) : "out of memory";
        sqlite3_close(m_db);
        m_db = nullptr;
        throw DbError("cannot open " + path.toStdString() + ": " + msg);
    }
    sqlite3_busy_timeout(m_db, 10000);
    try {
        if (!key.isEmpty()) {
            exec(cipherPragmas(key, "key").constData());
            // The key is checked on first read; read now, to fail here.
            exec("SELECT count(*) FROM sqlite_master");
        }
        exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL;");
    } catch (...) {
        sqlite3_close(m_db);
        m_db = nullptr;
        throw;
    }
}

Database::~Database()
{
    sqlite3_close_v2(m_db);
}

void Database::exec(const char *sql)
{
    char *err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DbError(msg + " in: " + sql);
    }
}

qint64 Database::lastInsertId() const
{
    return sqlite3_last_insert_rowid(m_db);
}

int Database::changes() const
{
    return sqlite3_changes(m_db);
}

int Database::userVersion()
{
    return int(queryInt("PRAGMA user_version").value_or(0));
}

void Database::setUserVersion(int v)
{
    exec(QStringLiteral("PRAGMA user_version=%1").arg(v));
}

void Database::raise(const char *what) const
{
    throw DbError(std::string(sqlite3_errmsg(m_db)) + " in: " + (what ? what : "?"));
}

Transaction::Transaction(Database &db) : m_db(db)
{
    m_db.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction()
{
    if (!m_done) {
        try {
            m_db.exec("ROLLBACK");
        } catch (...) {
        }
    }
}

void Transaction::commit()
{
    m_db.exec("COMMIT");
    m_done = true;
}

} // namespace e3
