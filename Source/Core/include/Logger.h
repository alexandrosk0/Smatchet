#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <cstddef>
#include <cstdint>

// Simple log4cxx-style levels.
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

struct LogEntry {
    double timestampSeconds;
    LogLevel level;
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

    /** When true, tracker backends may log truncated HTTP response bodies at Trace. */
    void SetLogTrackerHttpBodies(bool enabled);
    bool GetLogTrackerHttpBodies() const;

    /** When true, P4Annotate may log truncated p4 stdout at Trace (stderr always logged on failure). */
    void SetLogP4Io(bool enabled);
    bool GetLogP4Io() const;

    bool ShouldLog(LogLevel level) const;

    // Core logging API
    void Log(LogLevel level, const std::string& message);
    void Logf(LogLevel level, const char* fmt, ...);

    // Convenience helpers
    void Trace(const std::string& msg) { Log(LogLevel::Trace, msg); }
    void Debug(const std::string& msg) { Log(LogLevel::Debug, msg); }
    void Info(const std::string& msg) { Log(LogLevel::Info, msg); }
    void Warn(const std::string& msg) { Log(LogLevel::Warn, msg); }
    void Error(const std::string& msg) { Log(LogLevel::Error, msg); }

    // Thread‑safe snapshot of current log entries.
    std::vector<LogEntry> GetEntriesSnapshot() const;
    std::uint64_t GetRevision() const;

    // Clear all log entries.
    void Clear();

    /**
     * Optional async file sink — when set, every log entry is also queued for an internal
     * worker thread that flushes to @p path. Idempotent (calling with the same path is a no-op).
     * Passing an empty path stops the worker and closes the file. Disk I/O errors are retried
     * with bounded backoff; persistent errors degrade to a no-op sink but never permanently
     * deadlock the worker. Safe to call from any thread.
     *
     * All lifecycle operations (start, stop, path change, synchronous flush) are serialized
     * under `m_fileSinkLifecycleMutex` so concurrent callers cannot race a still-joinable
     * `std::thread` (previously triggered `std::terminate`).
     */
    void SetFileSinkPath(const std::string& path);

    /**
     * Synchronously drain pending file-sink entries to disk. Blocks until the worker has
     * acknowledged a flush generation issued after this call; returns immediately if no sink
     * is configured or the worker is shutting down. Safe to call from any thread.
     */
    void FlushFileSink();

  private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void FileSinkWorker();
    /// Must be called with m_fileSinkLifecycleMutex held.
    void StopFileSinkWorkerLocked();

    mutable std::mutex m_mutex;
    // deque gives O(1) pop_front for the overflow path; previous std::vector::erase(begin())
    // was O(N) and copied 999 entries on every log call after the cap.
    std::deque<LogEntry> m_entries;
    std::atomic<int> m_minLevelInt{static_cast<int>(LogLevel::Info)};
    std::atomic<bool> m_logTrackerHttpBodies{false};
    std::atomic<bool> m_logP4Io{false};
    std::atomic<std::uint64_t> m_revision{0};
    static constexpr std::size_t kMaxEntries = 1000;

    // Async file sink lifecycle. m_fileSinkLifecycleMutex is acquired by SetFileSinkPath / dtor /
    // FlushFileSink and serializes worker thread creation, destruction, and synchronous flush.
    // m_fileSinkMutex protects the queue + path + flush-generation counters; held briefly by
    // Log() (queue push) and by the worker (queue drain / ack publish). Lock order if both are
    // taken: lifecycle FIRST, then m_fileSinkMutex.
    mutable std::mutex m_fileSinkLifecycleMutex;
    mutable std::mutex m_fileSinkMutex;
    std::condition_variable m_fileSinkCv;    // worker wakes on queue/shutdown/flush request
    std::condition_variable m_fileSinkAckCv; // FlushFileSink waits on this for ack publish
    std::deque<LogEntry> m_fileSinkQueue;
    std::string m_fileSinkPath;
    std::thread m_fileSinkThread;
    std::atomic<bool> m_fileSinkShutdown{false};
    // Monotonic counters: callers bump RequestedGen; worker publishes AckedGen after writing
    // every batch. FlushFileSink waits until Acked >= the gen it requested.
    std::uint64_t m_fileSinkFlushRequestedGen{0};
    std::uint64_t m_fileSinkFlushAckedGen{0};
    static constexpr std::size_t kFileSinkQueueMax = 4096;
    static constexpr int kFileSinkMaxConsecutiveErrors = 5;
    static constexpr int kFileSinkErrorBackoffMs = 1000;
};

// printf‑style macros for convenience.
#define LOG_TRACE(fmt, ...) Logger::Instance().Logf(LogLevel::Trace, (fmt), ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Logger::Instance().Logf(LogLevel::Debug, (fmt), ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) Logger::Instance().Logf(LogLevel::Info, (fmt), ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) Logger::Instance().Logf(LogLevel::Warn, (fmt), ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::Instance().Logf(LogLevel::Error, (fmt), ##__VA_ARGS__)

// LOG_AGENT_DEBUG — bridge to the boundary-crossing NDJSON helper
// (tests/_debug/SmatchetAgentDebug.h). Single resolution per debug-off
// contract (plan slice 7): when SMATCHET_AGENT_DEBUG is ON, the call
// routes to the NDJSON sink with `payload = {"msg": <msg>}`; when OFF,
// the call routes to LOG_DEBUG so the message is still visible in the
// in-process log. The macro itself is always-callable — only the
// destination changes. `category` must be a closed-set string literal
// (see kSmatchetAgentDebugCategories in SmatchetAgentDebug.h).
#if defined(SMATCHET_AGENT_DEBUG)
// Indirection through a helper function so callers don't have to pull
// nlohmann/json + the SmatchetAgentDebug.h header into every TU. The
// helper is defined in Logger.cpp when SMATCHET_AGENT_DEBUG is ON.
void SmatchetAgentDebugLogBridge(const char* category, const char* file, int line, const std::string& msg);
#define LOG_AGENT_DEBUG(category, msg) ::SmatchetAgentDebugLogBridge((category), __FILE__, __LINE__, (msg))
#else
#define LOG_AGENT_DEBUG(category, msg) LOG_DEBUG("[agent-debug:%s] %s", (category), (msg).c_str())
#endif
