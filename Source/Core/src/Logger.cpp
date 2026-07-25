#include "Logger.h"

#include "Privacy/TextRedaction.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>


// Out-of-line definition required for ODR-use under C++14 (e.g. when passed by
// reference to std::chrono::milliseconds() in an unoptimized translation unit).
constexpr int Logger::kFileSinkErrorBackoffMs;

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lifeLock(m_fileSinkLifecycleMutex);
    StopFileSinkWorkerLocked();
}

LogLevel Logger::ParseLogLevelString(const std::string& s, LogLevel fallback) {
    const std::string k = ToLowerAsciiCopy(s);
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
    // Redact at ingestion so EVERY sink stores the scrubbed line — the in-memory ring buffer
    // feeds the in-app log viewer (whose contents get copied into bug reports and screenshots),
    // not just the on-disk file. Redacting here also keeps the file-sink guarantee (audit
    // 2026-06-13 LOW) with a single pass instead of one per sink. Call sites that log large
    // untrusted bodies already pre-redact via RedactHttpBodyForLog; typical LOG_* lines are
    // short, so the regex cost on the calling thread is negligible.
    entry.message = smatchet::privacy::RedactLogLine(message);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (static_cast<int>(level) < m_minLevelInt.load(std::memory_order_relaxed)) {
            return;
        }

        if (m_entries.size() >= kMaxEntries) {
            m_entries.pop_front(); // O(1) on deque, was O(N) on vector::erase(begin()).
        }
        m_entries.push_back(entry); // copy; `entry` still owned for the file-sink push below
        m_revision.fetch_add(1, std::memory_order_release);
    }

    // File sink push: re-check path under m_fileSinkMutex. If the sink was stopped between the
    // in-memory push above and this lock acquisition, m_fileSinkPath is empty (cleared inside
    // StopFileSinkWorkerLocked) and we skip — no entry leak into a dead-worker queue.
    bool notify = false;
    {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        if (m_fileSinkPath.empty()) {
            return;
        }
        if (m_fileSinkQueue.size() >= kFileSinkQueueMax) {
            m_fileSinkQueue.pop_front(); // bounded queue, drop oldest under sustained pressure
        }
        m_fileSinkQueue.push_back(std::move(entry));
        notify = true;
    }
    if (notify) {
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
    // Lifecycle mutex held for the entire stop+restart sequence. Concurrent callers serialize
    // here so we cannot assign over a still-joinable std::thread (which would std::terminate).
    std::lock_guard<std::mutex> lifeLock(m_fileSinkLifecycleMutex);

    {
        // Idempotency check under the queue mutex: the previously-set path is owned by the
        // queue mutex, so we must read it there. Returning early avoids stop+restart churn that
        // would otherwise lose buffered entries.
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        if (m_fileSinkPath == path) {
            return;
        }
    }

    StopFileSinkWorkerLocked();

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
    // Lifecycle mutex prevents the worker from being torn down between our request bump and
    // the ack-wait below. If no worker is running we have nothing to flush.
    std::unique_lock<std::mutex> lifeLock(m_fileSinkLifecycleMutex);
    if (!m_fileSinkThread.joinable()) {
        return;
    }
    if (m_fileSinkShutdown.load(std::memory_order_acquire)) {
        return;
    }

    std::uint64_t targetGen;
    {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        if (m_fileSinkPath.empty()) {
            return;
        }
        targetGen = ++m_fileSinkFlushRequestedGen;
    }
    m_fileSinkCv.notify_one();

    // Wait for the worker to publish ack >= targetGen. The lifecycle mutex is still held so the
    // worker cannot be joined out from under us; if shutdown is requested mid-wait we wake on
    // the same condvar and exit.
    std::unique_lock<std::mutex> sinkLock(m_fileSinkMutex);
    m_fileSinkAckCv.wait(sinkLock, [this, targetGen] {
        return m_fileSinkFlushAckedGen >= targetGen || m_fileSinkShutdown.load(std::memory_order_acquire);
    });
}

void Logger::StopFileSinkWorkerLocked() {
    // Caller must hold m_fileSinkLifecycleMutex. We signal shutdown, join the worker (releasing
    // any cv wait), then clear queue + path. This is the only path that joins the thread so
    // it cannot race with FlushFileSink or another SetFileSinkPath.
    if (!m_fileSinkThread.joinable()) {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        m_fileSinkPath.clear();
        m_fileSinkQueue.clear();
        return;
    }
    m_fileSinkShutdown.store(true, std::memory_order_release);
    m_fileSinkCv.notify_all();
    m_fileSinkAckCv.notify_all();
    m_fileSinkThread.join();

    std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
    m_fileSinkPath.clear();
    m_fileSinkQueue.clear();
    m_fileSinkFlushRequestedGen = 0;
    m_fileSinkFlushAckedGen = 0;
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

    auto openSinkFile = [&path]() {
        // Append + binary: preserves prior runs; avoids CRLF translation on Windows where
        // in-process line endings are already correct.
        return std::ofstream(path.c_str(), std::ios::out | std::ios::app | std::ios::binary);
    };

    std::ofstream out = openSinkFile();
    int consecutiveErrors = 0;

    while (true) {
        std::deque<LogEntry> batch;
        std::uint64_t pendingFlushGen = 0;
        {
            std::unique_lock<std::mutex> sinkLock(m_fileSinkMutex);
            m_fileSinkCv.wait(sinkLock, [this] {
                return m_fileSinkShutdown.load(std::memory_order_acquire) || !m_fileSinkQueue.empty() ||
                       m_fileSinkFlushRequestedGen > m_fileSinkFlushAckedGen;
            });
            if (m_fileSinkShutdown.load(std::memory_order_acquire) && m_fileSinkQueue.empty()) {
                break;
            }
            batch.swap(m_fileSinkQueue);
            pendingFlushGen = m_fileSinkFlushRequestedGen;
        }

        bool batchOk = true;
        if (out.is_open()) {
            for (const LogEntry& e : batch) {
                // Plain text line; format `t=<seconds> level=<name> message`.
                // e.message is already redacted at ingestion (Logger::Log): secret/PII
                // shapes are scrubbed and CR/LF/ANSI is stripped there, so a
                // server-controlled message cannot leak a credential or forge a
                // log line in the on-disk file (audit 2026-06-13 LOW) or in the
                // in-app viewer's ring buffer.
                out << "t=" << e.timestampSeconds << " level=" << LogLevelToString(e.level) << ' ' << e.message << '\n';
            }
            out.flush();
            if (!out.good()) {
                batchOk = false;
            }
        } else {
            batchOk = false;
        }

        if (!batchOk) {
            ++consecutiveErrors;
            if (consecutiveErrors >= kFileSinkMaxConsecutiveErrors) {
                // Persistent failure: close + sleep + retry-open. The queue continues to be
                // drained (entries are dropped from this batch but new ones still enqueue;
                // bounded queue keeps memory in check). Never `return` permanently — that
                // would deadlock any FlushFileSink waiters and leave the sink unusable until
                // SetFileSinkPath is re-issued.
                out.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(kFileSinkErrorBackoffMs));
                out = openSinkFile();
                consecutiveErrors = out.is_open() ? 0 : kFileSinkMaxConsecutiveErrors;
            }
        } else {
            consecutiveErrors = 0;
        }

        // Publish ack (even on error batches — the ack means "the worker advanced past this
        // generation"; FlushFileSink is documented as best-effort under I/O failure).
        {
            std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
            if (pendingFlushGen > m_fileSinkFlushAckedGen) {
                m_fileSinkFlushAckedGen = pendingFlushGen;
            }
        }
        m_fileSinkAckCv.notify_all();
    }

    // Final ack on shutdown so any FlushFileSink waiter releases.
    {
        std::lock_guard<std::mutex> sinkLock(m_fileSinkMutex);
        if (m_fileSinkFlushRequestedGen > m_fileSinkFlushAckedGen) {
            m_fileSinkFlushAckedGen = m_fileSinkFlushRequestedGen;
        }
    }
    m_fileSinkAckCv.notify_all();
}

// LOG_AGENT_DEBUG bridge implementation. Only compiled when the helper is
// active (SMATCHET_AGENT_DEBUG). Bridges from the always-callable
// LOG_AGENT_DEBUG macro to the NDJSON Emit() sink so callers don't pull
// nlohmann/json into every TU. See Logger.h and tests/_debug/SmatchetAgentDebug.h.
#if defined(SMATCHET_AGENT_DEBUG)
#include "../../../tests/_debug/SmatchetAgentDebug.h"

void SmatchetAgentDebugLogBridge(const char* category, const char* file, int line, const std::string& msg) {
    nlohmann::json payload;
    payload["msg"] = msg;
    ::smatchet_agent_debug::Emit(category, file, line, payload);
}
#endif
