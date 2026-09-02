#include "storage/SqliteDatabase.h"

#include "core/Logging.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <sqlite3.h>

namespace immichksync {

// MARK: - SqliteStatement

SqliteStatement::~SqliteStatement()
{
    if (m_statement) {
        sqlite3_finalize(m_statement);
    }
}

SqliteStatement::SqliteStatement(SqliteStatement &&other) noexcept
    : m_statement(other.m_statement)
    , m_database(other.m_database)
{
    other.m_statement = nullptr;
    other.m_database = nullptr;
}

SqliteStatement &SqliteStatement::operator=(SqliteStatement &&other) noexcept
{
    if (this != &other) {
        if (m_statement) {
            sqlite3_finalize(m_statement);
        }
        m_statement = other.m_statement;
        m_database = other.m_database;
        other.m_statement = nullptr;
        other.m_database = nullptr;
    }
    return *this;
}

void SqliteStatement::bind(int index, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    // SQLITE_TRANSIENT: sqlite copies the bytes, so the temporary dying at the end of
    // this call is fine. SQLITE_STATIC here would be a use-after-free.
    sqlite3_bind_text(m_statement, index, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
}

void SqliteStatement::bind(int index, qint64 value)
{
    sqlite3_bind_int64(m_statement, index, value);
}

void SqliteStatement::bind(int index, int value)
{
    sqlite3_bind_int(m_statement, index, value);
}

void SqliteStatement::bind(int index, double value)
{
    sqlite3_bind_double(m_statement, index, value);
}

void SqliteStatement::bind(int index, bool value)
{
    sqlite3_bind_int(m_statement, index, value ? 1 : 0);
}

void SqliteStatement::bindNull(int index)
{
    sqlite3_bind_null(m_statement, index);
}

bool SqliteStatement::exec(QString *errorMessage)
{
    const int result = sqlite3_step(m_statement);
    if (result == SQLITE_DONE || result == SQLITE_ROW) {
        return true;
    }
    if (errorMessage && m_database) {
        *errorMessage = QString::fromUtf8(sqlite3_errmsg(m_database));
    }
    return false;
}

bool SqliteStatement::next(QString *errorMessage)
{
    const int result = sqlite3_step(m_statement);
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result != SQLITE_DONE && errorMessage && m_database) {
        *errorMessage = QString::fromUtf8(sqlite3_errmsg(m_database));
    }
    return false;
}

QString SqliteStatement::columnText(int index) const
{
    const unsigned char *text = sqlite3_column_text(m_statement, index);
    return text ? QString::fromUtf8(reinterpret_cast<const char *>(text)) : QString();
}

qint64 SqliteStatement::columnInt64(int index) const
{
    return sqlite3_column_int64(m_statement, index);
}

int SqliteStatement::columnInt(int index) const
{
    return sqlite3_column_int(m_statement, index);
}

double SqliteStatement::columnDouble(int index) const
{
    return sqlite3_column_double(m_statement, index);
}

bool SqliteStatement::columnBool(int index) const
{
    return sqlite3_column_int(m_statement, index) != 0;
}

bool SqliteStatement::columnIsNull(int index) const
{
    return sqlite3_column_type(m_statement, index) == SQLITE_NULL;
}

// MARK: - SqliteDatabase

SqliteDatabase::~SqliteDatabase()
{
    close();
}

bool SqliteDatabase::open(const QString &path, QString *errorMessage)
{
    close();
    QMutexLocker locker(&m_mutex);

    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not create %1").arg(info.absolutePath());
        }
        return false;
    }

    const int result = sqlite3_open_v2(path.toUtf8().constData(),
                                       &m_database,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                           | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
    if (result != SQLITE_OK) {
        if (errorMessage) {
            *errorMessage = m_database ? QString::fromUtf8(sqlite3_errmsg(m_database))
                                       : QStringLiteral("sqlite3_open failed");
        }
        if (m_database) {
            sqlite3_close(m_database);
            m_database = nullptr;
        }
        return false;
    }
    m_path = path;

    // WAL keeps a long read (the album list the settings window asks for) from blocking
    // the engine's writes. `foreign_keys` is off by default in sqlite and the schema
    // relies on ON DELETE CASCADE, so it must be asked for on every connection.
    char *ignored = nullptr;
    for (const char *pragma : {"PRAGMA journal_mode = WAL",
                               "PRAGMA synchronous = NORMAL",
                               "PRAGMA foreign_keys = ON",
                               "PRAGMA busy_timeout = 5000"}) {
        sqlite3_exec(m_database, pragma, nullptr, nullptr, &ignored);
        if (ignored) {
            sqlite3_free(ignored);
            ignored = nullptr;
        }
    }
    return true;
}

void SqliteDatabase::close()
{
    QMutexLocker locker(&m_mutex);
    if (m_database) {
        sqlite3_close(m_database);
        m_database = nullptr;
    }
    m_path.clear();
}

QString SqliteDatabase::lastError() const
{
    return m_database ? QString::fromUtf8(sqlite3_errmsg(m_database)) : QString();
}

bool SqliteDatabase::execute(const QString &sql, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    if (!m_database) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The database is not open.");
        }
        return false;
    }
    char *raw = nullptr;
    const int result = sqlite3_exec(m_database, sql.toUtf8().constData(), nullptr, nullptr, &raw);
    if (result != SQLITE_OK) {
        if (errorMessage) {
            *errorMessage = raw ? QString::fromUtf8(raw) : lastError();
        }
        if (raw) {
            sqlite3_free(raw);
        }
        return false;
    }
    return true;
}

SqliteStatement SqliteDatabase::prepare(const QString &sql, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    if (!m_database) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The database is not open.");
        }
        return {};
    }
    sqlite3_stmt *statement = nullptr;
    const QByteArray utf8 = sql.toUtf8();
    if (sqlite3_prepare_v2(m_database, utf8.constData(), utf8.size(), &statement, nullptr)
        != SQLITE_OK) {
        if (errorMessage) {
            *errorMessage = lastError();
        }
        return {};
    }
    return SqliteStatement(statement, m_database);
}

bool SqliteDatabase::transaction(const std::function<bool()> &body, QString *errorMessage)
{
    // Held across the whole transaction: sqlite's own locking makes each statement
    // atomic, not the group of them, so without this a concurrent reader could observe
    // a half-applied migration.
    QMutexLocker locker(&m_mutex);
    if (!execute(QStringLiteral("BEGIN IMMEDIATE"), errorMessage)) {
        return false;
    }
    if (!body()) {
        execute(QStringLiteral("ROLLBACK"));
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("The transaction body reported a failure.");
        }
        return false;
    }
    return execute(QStringLiteral("COMMIT"), errorMessage);
}

int SqliteDatabase::schemaVersion() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_database) {
        return 0;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_database, "PRAGMA user_version", -1, &statement, nullptr) != SQLITE_OK) {
        return 0;
    }
    int version = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        version = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    return version;
}

bool SqliteDatabase::setSchemaVersion(int version, QString *errorMessage)
{
    // PRAGMA does not accept bound parameters, and `version` comes from our own
    // migration table rather than from anything a user or a server can influence.
    return execute(QStringLiteral("PRAGMA user_version = %1").arg(version), errorMessage);
}

qint64 SqliteDatabase::scalarInt64(const QString &sql, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    SqliteStatement statement = prepare(sql, errorMessage);
    if (!statement.isValid() || !statement.next(errorMessage)) {
        return 0;
    }
    return statement.columnInt64(0);
}

} // namespace immichksync
