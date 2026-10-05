// SPDX-License-Identifier: MPL-2.0
#include "Database.h"

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
    if (v.isNull()) {
        // A null QString is "no value"; an empty one is the empty string.
        sqlite3_bind_null(m_stmt, idx);
        return *this;
    }
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

Database::Database(const QString &path) : m_path(path)
{
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.toUtf8().constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        const std::string msg = m_db ? sqlite3_errmsg(m_db) : "out of memory";
        sqlite3_close(m_db);
        m_db = nullptr;
        throw DbError("cannot open " + path.toStdString() + ": " + msg);
    }
    sqlite3_busy_timeout(m_db, 10000);
    exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL;");
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
