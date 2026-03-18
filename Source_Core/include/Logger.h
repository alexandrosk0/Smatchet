#pragma once

#include <string>
#include <vector>
#include <mutex>
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

    // Clear all log entries.
    void Clear();

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
    static constexpr std::size_t kMaxEntries = 1000;
};

// printf‑style macros for convenience.
#define LOG_TRACE(fmt, ...) Logger::Instance().Logf(LogLevel::Trace, (fmt), ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Logger::Instance().Logf(LogLevel::Debug, (fmt), ##__VA_ARGS__)
#define LOG_INFO(fmt,  ...) Logger::Instance().Logf(LogLevel::Info,  (fmt), ##__VA_ARGS__)
#define LOG_WARN(fmt,  ...) Logger::Instance().Logf(LogLevel::Warn,  (fmt), ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::Instance().Logf(LogLevel::Error, (fmt), ##__VA_ARGS__)

