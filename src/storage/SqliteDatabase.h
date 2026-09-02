#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>

#include <functional>

struct sqlite3;
struct sqlite3_stmt;

namespace immichksync {

/// A prepared statement with the binding and stepping this app actually needs.
///
/// RAII over `sqlite3_stmt`: a statement that escapes a scope without being finalised
/// holds a read transaction open, which on a database this small shows up as a write
/// that mysteriously fails to commit.
class SqliteStatement {
public:
    SqliteStatement() = default;
    ~SqliteStatement();
    SqliteStatement(SqliteStatement &&other) noexcept;
    SqliteStatement &operator=(SqliteStatement &&other) noexcept;
    Q_DISABLE_COPY(SqliteStatement)

    bool isValid() const { return m_statement != nullptr; }

    void bind(int index, const QString &value);
    void bind(int index, qint64 value);
    void bind(int index, int value);
    void bind(int index, double value);
    void bind(int index, bool value);
    void bindNull(int index);

    /// Runs the statement to completion. For anything that returns no rows.
    bool exec(QString *errorMessage = nullptr);
    /// Steps to the next row; false at the end of the result set or on error.
    bool next(QString *errorMessage = nullptr);

    QString columnText(int index) const;
    qint64 columnInt64(int index) const;
    int columnInt(int index) const;
    double columnDouble(int index) const;
    bool columnBool(int index) const;
    bool columnIsNull(int index) const;

private:
    friend class SqliteDatabase;
    SqliteStatement(sqlite3_stmt *statement, sqlite3 *database)
        : m_statement(statement)
        , m_database(database)
    {
    }

    sqlite3_stmt *m_statement = nullptr;
    sqlite3 *m_database = nullptr;
};

/// The local reconciliation database.
///
/// A thin wrapper over the sqlite3 C API rather than QtSql: `PRAGMA user_version`
/// migrations and explicit transaction control are the whole point of this layer, and
/// QSqlDatabase's per-thread connection registry would add a failure mode without
/// removing any code.
///
/// One connection, shared between the engine thread and the settings window, guarded
/// by a mutex. SQLite is compiled serialised on every distribution this ships to, but
/// the mutex is what makes a multi-statement transaction atomic against a concurrent
/// reader — sqlite's own locking would only protect each statement.
class SqliteDatabase {
public:
    SqliteDatabase() = default;
    ~SqliteDatabase();
    Q_DISABLE_COPY_MOVE(SqliteDatabase)

    /// Opens (creating if needed) the database at `path`, making parent directories.
    bool open(const QString &path, QString *errorMessage);
    void close();
    bool isOpen() const { return m_database != nullptr; }
    QString path() const { return m_path; }

    bool execute(const QString &sql, QString *errorMessage = nullptr);
    SqliteStatement prepare(const QString &sql, QString *errorMessage = nullptr);

    /// Runs `body` inside a transaction, rolling back if it returns false.
    bool transaction(const std::function<bool()> &body, QString *errorMessage = nullptr);

    int schemaVersion() const;
    bool setSchemaVersion(int version, QString *errorMessage = nullptr);

    /// Convenience for the several `SELECT COUNT(*)`-shaped queries the store runs.
    qint64 scalarInt64(const QString &sql, QString *errorMessage = nullptr);

    QString lastError() const;

    /// The lock every public call on `SyncStore` takes. Exposed so a caller that needs
    /// several statements to be one atomic unit can hold it across all of them.
    QRecursiveMutex &mutex() { return m_mutex; }

private:
    sqlite3 *m_database = nullptr;
    QString m_path;
    /// Recursive because `transaction()` calls `execute()`, which locks again.
    mutable QRecursiveMutex m_mutex;
};

} // namespace immichksync
