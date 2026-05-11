#include "Logger.h"

#include <algorithm>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <cctype>
#include <fstream>
#include <vector>

namespace {
std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}
} // namespace

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    StopFileSinkWorker();
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

    LogEntry entry;
    entry.timestampSeconds = nowSeconds;
    entry.level = level;
    entry.message = message;

    bool forwardToFileSink = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (static_cast<int>(level) < m_minLevelInt.load(std::memory_order_relaxed)) {
            return;
        }

        if (m_entries.size() >= kMaxEntries) {
            m_entries.pop_front(); // O(1) on deque, was O(N) on vector::erase(begin()).
        }
        m_entries.push_back(entry);
        m_revision.fetch_add(1, std::memory_order_release);

        // File sink decision needs a separate snapshot — checked under its own mutex below.
        forwardToFileSink = true;
    }

    if (forwardToFileSink) {
        std::unique_lock<std::mutex> sinkLock(m_fileSinkMutex);
        if (m_fileSinkPath.empty()) {
            return;
        }
        if (m_fileSinkQueue.size() >= kFileSinkQueueMax) {
            // Drop oldest to bound memory under sustained log pressure with a slow disk.
            m_fileSinkQueue.pop_front();
        }
        m_fileSinkQueue.push_back(std::move(entry));
        sinkLock.unlock();
        m_fileSinkCv.notify_one();
    }
}

void Logger::Logf(LogLevel level, const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    if (!ShouldLog(level)) {
        return;
    }

    // First pass: try the stack buffer. Most log lines fit comfortably.
    // Second pass: heap-allocate when truncation would occur, so HTTP body
    // trace logs are not silently clipped at 4 KB.
    constexpr int kStackBufBytes = 4096;
    char stackBuf[kStackBufBytes];

    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
#if defined(_MSC_VER)
    const int needed = _vscprintf(fmt, args);
    if (needed >= 0 && needed < kStackBufBytes) {
        _vsnprintf_s(stackBuf, sizeof(stackBuf), _TRUNCATE, fmt, argsCopy);
        va_end(argsCopy);
        va_end(args);
        Log(level, std::string(stackBuf));
        return;
    }
#else
    const int needed = vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
    if (needed >= 0 && needed < kStackBufBytes) {
        va_end(argsCopy);
        va_end(args);
        Log(level, std::string(stackBuf));
        return;
    }
#endif

    if (needed <= 0) {
        va_end(argsCopy);
        va_end(args);
        return;
    }

    std::vector<char> heapBuf(static_cast<std::size_t>(needed) + 1u);
#if defined(_MSC_VER)
    _vsnprintf_s(heapBuf.data(), heapBuf.size(), _TRUNCATE, fmt, argsCopy);
#else
    vsnprintf(heapBuf.data(), heapBuf.size(), fmt, argsCopy);
#endif
    va_end(argsCopy);
    va_end(args);
    Log(level, std::string(heapBuf.data()));
}

std::vector<LogEntry> Logger::GetEntriesSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<LogEntry>(m_entries.begin(), m_entries.end());
}

std::uint64_t Logger::GetRevision() const { return m_revision.load(std::memory_order_acquire); }

void Logger::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_revision.fetch_add(1, std::memory_order_release);
}

void Logger::SetFileSinkPath(const std::string& path) {
    {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        if (m_fileSinkPath == path) {
            return; // idempotent
        }
    }
    // Stop any existing worker before swapping the path.
    StopFileSinkWorker();

    if (path.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        m_fileSinkPath = path;
        m_fileSinkShutdown.store(false, std::memory_order_release);
    }
    m_fileSinkThread = std::thread(&Logger::FileSinkWorker, this);
}

void Logger::FlushFileSink() {
    // Best-effort: wake the worker and let it drain. The worker writes synchronously inside the
    // loop; here we just nudge it. For a true sync flush callers should StopFileSinkWorker().
    m_fileSinkCv.notify_one();
}

void Logger::StopFileSinkWorker() {
    if (!m_fileSinkThread.joinable()) {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        m_fileSinkPath.clear();
        m_fileSinkQueue.clear();
        return;
    }
    m_fileSinkShutdown.store(true, std::memory_order_release);
    m_fileSinkCv.notify_all();
    m_fileSinkThread.join();
    std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
    m_fileSinkPath.clear();
    m_fileSinkQueue.clear();
}

void Logger::FileSinkWorker() {
    std::string path;
    {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        path = m_fileSinkPath;
    }
    if (path.empty()) {
        return;
    }

    // Append mode so previous runs are preserved. Binary to avoid CRLF translation on Windows
    // where the in-process line endings already match the platform expectation.
    std::ofstream out(path.c_str(), std::ios::out | std::ios::app | std::ios::binary);
    if (!out.is_open()) {
        return; // file inaccessible — silent (no log-on-log loop)
    }

    while (true) {
        std::deque<LogEntry> batch;
        {
            std::unique_lock<std::mutex> sinkLock(m_fileSinkMutex);
            m_fileSinkCv.wait(sinkLock, [this] {
                return m_fileSinkShutdown.load(std::memory_order_acquire) || !m_fileSinkQueue.empty();
            });
            if (m_fileSinkShutdown.load(std::memory_order_acquire) && m_fileSinkQueue.empty()) {
                break;
            }
            batch.swap(m_fileSinkQueue);
        }

        for (const LogEntry& e : batch) {
            // Plain text line; format `t=<seconds> level=<name> message`.
            // Keep the format stable so external log readers can parse it without ceremony.
            out << "t=" << e.timestampSeconds
                << " level=" << LogLevelToString(e.level)
                << ' ' << e.message
                << '\n';
        }
        out.flush(); // flush after every batch so a crash mid-session loses at most one batch
        if (!out.good()) {
            // I/O failed; give up to avoid spinning on a bad disk. The in-memory ring still works.
            return;
        }
    }
}
