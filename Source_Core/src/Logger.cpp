#include "Logger.h"

#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <chrono>

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::Log(LogLevel level, const std::string& message) {
    using namespace std::chrono;
    const double nowSeconds =
        duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_entries.size() >= kMaxEntries) {
        m_entries.erase(m_entries.begin()); // simple ring‑buffer behavior
    }

    LogEntry entry;
    entry.timestampSeconds = nowSeconds;
    entry.level            = level;
    entry.message          = message;
    m_entries.push_back(entry);
}

void Logger::Logf(LogLevel level, const char* fmt, ...) {
    if (!fmt) {
        return;
    }

    char buffer[1024];

    va_list args;
    va_start(args, fmt);
#if defined(_MSC_VER)
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
#else
    vsnprintf(buffer, sizeof(buffer), fmt, args);
#endif
    va_end(args);

    Log(level, buffer);
}

std::vector<LogEntry> Logger::GetEntriesSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

void Logger::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

