#include "Logger.h"

#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <cctype>

namespace {
std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
} // namespace

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

LogLevel Logger::ParseLogLevelString(const std::string& s, LogLevel fallback) {
    const std::string k = ToLowerAscii(s);
    if (k == "trace") {
        return LogLevel::Trace;
    }
    if (k == "debug") {
        return LogLevel::Debug;
    }
    if (k == "info") {
        return LogLevel::Info;
    }
    if (k == "warn" || k == "warning") {
        return LogLevel::Warn;
    }
    if (k == "error") {
        return LogLevel::Error;
    }
    return fallback;
}

const char* Logger::LogLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

void Logger::SetMinLevel(LogLevel minLevel) {
    m_minLevelInt.store(static_cast<int>(minLevel), std::memory_order_release);
}

LogLevel Logger::GetMinLevel() const { return static_cast<LogLevel>(m_minLevelInt.load(std::memory_order_acquire)); }

void Logger::SetLogTrackerHttpBodies(bool enabled) { m_logTrackerHttpBodies.store(enabled, std::memory_order_release); }

bool Logger::GetLogTrackerHttpBodies() const { return m_logTrackerHttpBodies.load(std::memory_order_acquire); }

void Logger::SetLogP4Io(bool enabled) { m_logP4Io.store(enabled, std::memory_order_release); }

bool Logger::GetLogP4Io() const { return m_logP4Io.load(std::memory_order_acquire); }

bool Logger::ShouldLog(LogLevel level) const {
    return static_cast<int>(level) >= m_minLevelInt.load(std::memory_order_relaxed);
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (!ShouldLog(level)) {
        return;
    }

    using namespace std::chrono;
    const double nowSeconds = duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (static_cast<int>(level) < m_minLevelInt.load(std::memory_order_relaxed)) {
        return;
    }

    if (m_entries.size() >= kMaxEntries) {
        m_entries.erase(m_entries.begin());
    }

    LogEntry entry;
    entry.timestampSeconds = nowSeconds;
    entry.level = level;
    entry.message = message;
    m_entries.push_back(entry);
    m_revision.fetch_add(1, std::memory_order_release);
}

void Logger::Logf(LogLevel level, const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    if (!ShouldLog(level)) {
        return;
    }

    char buffer[4096];

    va_list args;
    va_start(args, fmt);
#if defined(_MSC_VER)
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
#else
    vsnprintf(buffer, sizeof(buffer), fmt, args);
#endif
    va_end(args);

    Log(level, std::string(buffer));
}

std::vector<LogEntry> Logger::GetEntriesSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

std::uint64_t Logger::GetRevision() const { return m_revision.load(std::memory_order_acquire); }

void Logger::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_revision.fetch_add(1, std::memory_order_release);
}






