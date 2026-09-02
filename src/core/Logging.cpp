#include "core/Logging.h"

#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

namespace immichksync {

QString displayName(LogCategory category)
{
    switch (category) {
    case LogCategory::App: return QStringLiteral("App");
    case LogCategory::Api: return QStringLiteral("API");
    case LogCategory::Sync: return QStringLiteral("Sync");
    case LogCategory::Storage: return QStringLiteral("Storage");
    case LogCategory::FileSystem: return QStringLiteral("Files");
    case LogCategory::Credentials: return QStringLiteral("Credentials");
    }
    return QStringLiteral("App");
}

QString rawName(LogCategory category)
{
    switch (category) {
    case LogCategory::App: return QStringLiteral("app");
    case LogCategory::Api: return QStringLiteral("api");
    case LogCategory::Sync: return QStringLiteral("sync");
    case LogCategory::Storage: return QStringLiteral("storage");
    case LogCategory::FileSystem: return QStringLiteral("fileSystem");
    case LogCategory::Credentials: return QStringLiteral("credentials");
    }
    return QStringLiteral("app");
}

QString displayName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("Debug");
    case LogLevel::Info: return QStringLiteral("Info");
    case LogLevel::Notice: return QStringLiteral("Notice");
    case LogLevel::Warning: return QStringLiteral("Warning");
    case LogLevel::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Info");
}

QString iconName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("tools-report-bug");
    case LogLevel::Info: return QStringLiteral("dialog-information");
    case LogLevel::Notice: return QStringLiteral("dialog-messages");
    case LogLevel::Warning: return QStringLiteral("dialog-warning");
    case LogLevel::Error: return QStringLiteral("dialog-error");
    }
    return QStringLiteral("dialog-information");
}

namespace {

QString fileTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("DEBUG ");
    case LogLevel::Info: return QStringLiteral("INFO  ");
    case LogLevel::Notice: return QStringLiteral("NOTICE");
    case LogLevel::Warning: return QStringLiteral("WARN  ");
    case LogLevel::Error: return QStringLiteral("ERROR ");
    }
    return QStringLiteral("INFO  ");
}

} // namespace

LogSink &LogSink::instance()
{
    static LogSink sink;
    return sink;
}

QString LogSink::directory()
{
    // GenericStateLocation is $XDG_STATE_HOME; a log is state, not data the user
    // would ever want backed up or synced.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    return QDir(base).filePath(QStringLiteral("immichksync"));
}

QString LogSink::filePath()
{
    return QDir(directory()).filePath(QStringLiteral("immichksync.log"));
}

LogSink::LogSink()
{
    openLogFile();
}

LogSink::~LogSink()
{
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_file.close();
    }
}

LogLevel LogSink::minimumLevel() const
{
    QMutexLocker locker(&m_mutex);
    return m_minimumLevel;
}

void LogSink::setMinimumLevel(LogLevel level)
{
    QMutexLocker locker(&m_mutex);
    m_minimumLevel = level;
}

QVector<LogEntry> LogSink::entries() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

void LogSink::clear()
{
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
}

void LogSink::log(LogLevel level, LogCategory category, const QString &message)
{
    bool needsRotation = false;
    {
        // One lock for the whole line: splitting the ring append from the file write
        // would let two threads interleave and put the log file out of order with the
        // buffer the log tab shows.
        QMutexLocker locker(&m_mutex);
        if (level < m_minimumLevel) {
            return;
        }

        LogEntry entry;
        entry.id = m_nextId++;
        entry.date = QDateTime::currentDateTime();
        entry.level = level;
        entry.category = category;
        entry.message = message;

        m_entries.append(entry);
        if (m_entries.size() > kRingCapacity) {
            m_entries.remove(0, m_entries.size() - kRingCapacity);
        }

        std::fprintf(stderr, "%s [%s] %s\n",
                     qUtf8Printable(fileTag(level).trimmed()),
                     qUtf8Printable(rawName(category)),
                     qUtf8Printable(message));

        if (m_file.isOpen()) {
            const QString line = QStringLiteral("%1  %2  [%3] %4\n")
                                     .arg(entry.date.toString(Qt::ISODateWithMs),
                                          fileTag(level),
                                          rawName(category),
                                          message);
            const QByteArray bytes = line.toUtf8();
            m_file.write(bytes);
            m_file.flush();
            m_bytesWritten += bytes.size();
            needsRotation = m_bytesWritten > kMaxFileBytes;
        }
    }

    if (needsRotation) {
        rotateLogFile();
    }
}

void LogSink::openLogFile()
{
    QDir().mkpath(directory());
    m_file.setFileName(filePath());
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    m_bytesWritten = m_file.size();
}

void LogSink::rotateLogFile()
{
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_file.close();
    }
    const QString archived = QDir(directory()).filePath(QStringLiteral("immichksync.previous.log"));
    QFile::remove(archived);
    QFile::rename(filePath(), archived);
    m_bytesWritten = 0;
    openLogFile();
}

} // namespace immichksync
