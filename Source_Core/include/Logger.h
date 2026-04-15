#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstddef>

// Simple log4cxx-style levels.
enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error
};

struct LogEntry {
    double    timestampSeconds;
    LogLevel  level;
    std::string message;
};

class Logger {
public:
    static Logger& Instance();

    /** Parse "trace"|"debug"|"info"|"warn"|"error" (case-insensitive); fallback @p fallback. */
    static LogLevel ParseLogLevelString(const std::string& s, LogLevel fallback = LogLevel::Info);
    static const char* LogLevelToString(LogLevel level);

    void SetMinLevel(LogLevel minLevel);
    LogLevel GetMinLevel() const;

    /** When true, JiraClient may log truncated HTTP response bodies at Trace. */
    void SetLogJiraHttpBodies(bool enabled);
    bool GetLogJiraHttpBodies() const;

    /** When true, P4Blame may log truncated p4 stdout at Trace (stderr always logged on failure). */
    void SetLogP4Io(bool enabled);
    bool GetLogP4Io() const;

    bool ShouldLog(LogLevel level) const;

    // Core logging API
    void Log(LogLevel level, const std::string& message);
    void Logf(LogLevel level, const char* fmt, ...);

    // Convenience helpers
    void Trace(const std::string& msg) { Log(LogLevel::Trace, msg); }
    void Debug(const std::string& msg) { Log(LogLevel::Debug, msg); }
    void Info(const std::string& msg)  { Log(LogLevel::Info,  msg); }
    void Warn(const std::string& msg)  { Log(LogLevel::Warn,  msg); }
    void Error(const std::string& msg) { Log(LogLevel::Error, msg); }

    // Thread‑safe snapshot of current log entries.
    std::vector<LogEntry> GetEntriesSnapshot() const;
    std::uint64_t GetRevision() const;

    // Clear all log entries.
    void Clear();

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
    std::atomic<int> m_minLevelInt{static_cast<int>(LogLevel::Info)};
    std::atomic<bool> m_logJiraHttpBodies{false};
    std::atomic<bool> m_logP4Io{false};
    std::atomic<std::uint64_t> m_revision{0};
    static constexpr std::size_t kMaxEntries = 1000;
};

// printf‑style macros for convenience.
#define LOG_TRACE(fmt, ...) Logger::Instance().Logf(LogLevel::Trace, (fmt), ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Logger::Instance().Logf(LogLevel::Debug, (fmt), ##__VA_ARGS__)
#define LOG_INFO(fmt,  ...) Logger::Instance().Logf(LogLevel::Info,  (fmt), ##__VA_ARGS__)
#define LOG_WARN(fmt,  ...) Logger::Instance().Logf(LogLevel::Warn,  (fmt), ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::Instance().Logf(LogLevel::Error, (fmt), ##__VA_ARGS__)

