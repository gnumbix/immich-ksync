#pragma once

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QString>
#include <QVector>

namespace immichksync {

/// Reverse-DNS identifier used for the config file, the D-Bus name and the log.
inline constexpr const char *kAppId = IMMICHKSYNC_APPID;

enum class LogCategory {
    App,
    Api,
    Sync,
    Storage,
    FileSystem,
    Credentials,
};

QString displayName(LogCategory category);
QString rawName(LogCategory category);

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Notice = 2,
    Warning = 3,
    Error = 4,
};

QString displayName(LogLevel level);
/// Breeze icon name, used by the log tab.
QString iconName(LogLevel level);

struct LogEntry {
    quint64 id = 0;
    QDateTime date;
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::App;
    QString message;
};

/// Fan-out sink: every line goes to a ring buffer (for the log tab), stderr (for
/// `make run`) and a rotating file (so "Open Log Folder" has something to show after
/// a restart).
///
/// Synchronous and mutex-guarded rather than queued: logging that had to be awaited
/// would reorder lines and force a callback into every call site.
class LogSink {
public:
    static LogSink &instance();

    static QString directory();
    static QString filePath();

    void log(LogLevel level, LogCategory category, const QString &message);

    LogLevel minimumLevel() const;
    void setMinimumLevel(LogLevel level);

    QVector<LogEntry> entries() const;
    void clear();

private:
    LogSink();
    ~LogSink();
    Q_DISABLE_COPY_MOVE(LogSink)

    void openLogFile();
    void appendToFile(const LogEntry &entry);
    void rotateLogFile();

    static constexpr int kRingCapacity = 2000;
    static constexpr qint64 kMaxFileBytes = 4 * 1024 * 1024;

    mutable QMutex m_mutex;
    QVector<LogEntry> m_entries;
    quint64 m_nextId = 0;
    LogLevel m_minimumLevel = LogLevel::Info;
    QFile m_file;
    qint64 m_bytesWritten = 0;
};

/// Category-bound facade. Declared once per subsystem as a file-scope constant, so a
/// call site names only the level and the message.
class AppLogger {
public:
    explicit constexpr AppLogger(LogCategory category) : m_category(category) {}

    void debug(const QString &message) const { emit_(LogLevel::Debug, message); }
    void info(const QString &message) const { emit_(LogLevel::Info, message); }
    void notice(const QString &message) const { emit_(LogLevel::Notice, message); }
    void warning(const QString &message) const { emit_(LogLevel::Warning, message); }
    void error(const QString &message) const { emit_(LogLevel::Error, message); }

private:
    void emit_(LogLevel level, const QString &message) const
    {
        LogSink::instance().log(level, m_category, message);
    }

    LogCategory m_category;
};

namespace log {
inline constexpr AppLogger app{LogCategory::App};
inline constexpr AppLogger api{LogCategory::Api};
inline constexpr AppLogger sync{LogCategory::Sync};
inline constexpr AppLogger storage{LogCategory::Storage};
inline constexpr AppLogger fileSystem{LogCategory::FileSystem};
inline constexpr AppLogger credentials{LogCategory::Credentials};
} // namespace log

} // namespace immichksync
