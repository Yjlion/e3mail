// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>
#include <QVariant>

#include <memory>
#include <optional>
#include <stdexcept>

struct sqlite3;
struct sqlite3_stmt;

namespace e3 {

// For nullable text columns: empty means NULL.
inline std::optional<QString> nullIfEmpty(const QString &s)
{
    return s.isEmpty() ? std::nullopt : std::optional<QString>(s);
}

class DbError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Database;

// A prepared statement. Bind by 1-based index, step, read columns by 0-based
// index. Throws DbError on any SQLite failure: a store that half-fails quietly
// is how mail gets lost.
class Statement
{
public:
    Statement(Database &db, const char *sql);
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    Statement &bind(int idx, std::nullptr_t);
    Statement &bind(int idx, qint64 v);
    Statement &bind(int idx, int v) { return bind(idx, qint64(v)); }
    Statement &bind(int idx, bool v) { return bind(idx, qint64(v ? 1 : 0)); }
    Statement &bind(int idx, double v);
    Statement &bind(int idx, const QString &v);   // always TEXT, even when null
    Statement &bind(int idx, const QByteArray &v); // as BLOB
    Statement &bind(int idx, const char *v) { return bind(idx, QString::fromUtf8(v)); }
    Statement &bind(int idx, const QVariant &v);
    template<typename T>
    Statement &bind(int idx, const std::optional<T> &v)
    {
        return v ? bind(idx, *v) : bind(idx, nullptr);
    }

    // Binds all arguments in order starting at 1.
    template<typename... Args>
    Statement &bindAll(Args &&...args)
    {
        [[maybe_unused]] int i = 1;
        (bind(i++, std::forward<Args>(args)), ...);
        return *this;
    }

    // Returns true while a row is available.
    bool step();
    // Runs to completion, for statements that return no rows.
    void exec();
    void reset();

    bool isNull(int col) const;
    qint64 int64(int col) const;
    int integer(int col) const { return int(int64(col)); }
    double real(int col) const;
    QString text(int col) const;
    QByteArray blob(int col) const;

private:
    Database &m_db;
    sqlite3_stmt *m_stmt = nullptr;
};

class Database
{
public:
    // Opens (creating if needed) and enables WAL, foreign keys and a busy
    // timeout so a fetch thread and the UI thread can share one file.
    explicit Database(const QString &path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    // For one-off statements and scripts with several statements.
    void exec(const char *sql);
    void exec(const QString &sql) { exec(sql.toUtf8().constData()); }

    template<typename... Args>
    void run(const char *sql, Args &&...args)
    {
        Statement st(*this, sql);
        st.bindAll(std::forward<Args>(args)...);
        st.exec();
    }

    // Convenience: first column of the first row, or nullopt.
    template<typename... Args>
    std::optional<qint64> queryInt(const char *sql, Args &&...args)
    {
        Statement st(*this, sql);
        st.bindAll(std::forward<Args>(args)...);
        if (st.step() && !st.isNull(0))
            return st.int64(0);
        return std::nullopt;
    }
    template<typename... Args>
    std::optional<QString> queryText(const char *sql, Args &&...args)
    {
        Statement st(*this, sql);
        st.bindAll(std::forward<Args>(args)...);
        if (st.step() && !st.isNull(0))
            return st.text(0);
        return std::nullopt;
    }

    qint64 lastInsertId() const;
    int changes() const;
    int userVersion();
    void setUserVersion(int v);
    QString path() const { return m_path; }

    sqlite3 *handle() const { return m_db; }
    [[noreturn]] void raise(const char *what) const;

private:
    sqlite3 *m_db = nullptr;
    QString m_path;
};

// RAII transaction: rolls back unless commit() was called.
class Transaction
{
public:
    explicit Transaction(Database &db);
    ~Transaction();
    void commit();

private:
    Database &m_db;
    bool m_done = false;
};

} // namespace e3
