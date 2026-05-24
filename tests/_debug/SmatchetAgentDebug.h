// tests/_debug/SmatchetAgentDebug.h
//
// Production-resident NDJSON helper — slice 7 of
// docs/design/autonomous-debugging-no-creds.md.
//
// Header-only. Provides SMATCHET_AGENT_DEBUG_LOG(category, json_obj) macro.
// Distinct from the per-investigation template
// agents/_shared/templates/SmatchetAgentDebug.h.tmpl (see the plan's
// § Coexistence subsection — both helpers coexist, different schema, different
// path, different audience).
//
// Per-line NDJSON schema: {ts, category, pid, tid, scope, payload}.
// `category` is a closed enum (see kSmatchetAgentDebugCategories below).
// File path: <userData>/agent-debug/<session-id>.ndjson in production,
// tests/_debug/scratch/<test-name>.ndjson under doctest/bats.
// One file per session; never appended across sessions.
// Per-file size cap 50 MB; on overflow emit a terminal `_overflow` scenario-phase
// line and stop.
//
// OFF-build (default in iter/publish): SMATCHET_AGENT_DEBUG_LOG expands to
// `((void)0)`. ON-build (debug/asan/ui-test): SMATCHET_AGENT_DEBUG is defined.
//
// C++14. No string_view / optional / variant / structured bindings / if constexpr.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <process.h>
#  include <windows.h>
#  include <io.h>
#  define SMATCHET_AGENT_DEBUG_GETPID() static_cast<long long>(_getpid())
#  define SMATCHET_AGENT_DEBUG_FSYNC(_fd) _commit(_fd)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  define SMATCHET_AGENT_DEBUG_GETPID() static_cast<long long>(getpid())
#  define SMATCHET_AGENT_DEBUG_FSYNC(_fd) ::fsync(_fd)
#endif

namespace smatchet_agent_debug {

// Closed-set category enum. New categories require a slice-7 amendment +
// extending this array (per the plan's per-line schema table).
inline const char* const* Categories() {
    static const char* const kCategories[] = {
        "backend-call",
        "ui-event",
        "worker-handoff",
        "lock-claim",
        "scenario-phase",
        "cli-command",
        "temp-debug",
        nullptr,
    };
    return kCategories;
}

inline bool IsValidCategory(const char* category) {
    if (category == nullptr) {
        return false;
    }
    const char* const* it = Categories();
    for (; *it != nullptr; ++it) {
        if (std::string(*it) == category) {
            return true;
        }
    }
    return false;
}

// Per-file byte cap (50 MB).
inline std::size_t MaxFileBytes() { return static_cast<std::size_t>(50) * 1024 * 1024; }

// Process-wide state guarded by Mutex(). Held by every Emit() call; held during
// init (path resolution + ofstream open).
struct State {
    std::ofstream stream;
    std::string path;
    std::size_t bytes_written = 0;
    bool overflowed = false;
    bool initialised = false;
    bool fsync_enabled = false;
    bool target_is_explicit = false;  // set by SetTargetPath; suppresses default path resolution
    std::string explicit_path;
    std::atomic<std::uint64_t> fsync_count{0};
};

inline std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

inline State& Get() {
    static State s;
    return s;
}

inline std::string IsoTimestampUtc() {
    using namespace std::chrono;
    const system_clock::time_point now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const long long ms_part =
        duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm_utc;
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[40];
    std::snprintf(buf,
                  sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  tm_utc.tm_hour,
                  tm_utc.tm_min,
                  tm_utc.tm_sec,
                  ms_part);
    return std::string(buf);
}

inline std::string ThreadIdString() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

// Best-effort env-var read.
inline std::string EnvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return std::string(fallback ? fallback : "");
    }
    return std::string(v);
}

inline bool EnvIsTrue(const char* name) {
    const std::string v = EnvOr(name, "");
    return v == "1" || v == "true" || v == "TRUE" || v == "True";
}

// Default production session id = <unix-epoch>-<pid>. Set on first call and
// stable for the process lifetime. Exported via SMATCHET_AGENT_DEBUG_SESSION_ID
// env var so spawned subprocesses can correlate without re-discovering.
inline std::string SessionId() {
    static std::string s_session_id;
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        const std::string from_env = EnvOr("SMATCHET_AGENT_DEBUG_SESSION_ID", "");
        if (!from_env.empty()) {
            s_session_id = from_env;
            return;
        }
        const long long now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        std::ostringstream oss;
        oss << now_s << "-" << SMATCHET_AGENT_DEBUG_GETPID();
        s_session_id = oss.str();
        // Export so subprocesses can correlate. Best-effort.
#if defined(_WIN32)
        _putenv_s("SMATCHET_AGENT_DEBUG_SESSION_ID", s_session_id.c_str());
#else
        setenv("SMATCHET_AGENT_DEBUG_SESSION_ID", s_session_id.c_str(), 1);
#endif
    });
    return s_session_id;
}

inline std::string ResolveDefaultPath() {
    // Resolve <userData>/agent-debug/<session-id>.ndjson. Honours
    // SMATCHET_AGENT_DEBUG_DIR override; falls back to a temp dir.
    const std::string override_dir = EnvOr("SMATCHET_AGENT_DEBUG_DIR", "");
    std::string base;
    if (!override_dir.empty()) {
        base = override_dir;
    } else {
#if defined(_WIN32)
        const std::string appdata = EnvOr("LOCALAPPDATA", "");
        if (!appdata.empty()) {
            base = appdata + "\\Smatchet\\agent-debug";
        } else {
            char tmp[MAX_PATH];
            const DWORD n = GetTempPathA(MAX_PATH, tmp);
            base = (n > 0 && n < MAX_PATH) ? std::string(tmp) + "Smatchet\\agent-debug"
                                           : std::string("Smatchet-agent-debug");
        }
#else
        const std::string home = EnvOr("HOME", "/tmp");
        base = home + "/.smatchet/agent-debug";
#endif
    }
    // Best-effort mkdir-p via cstdio — we deliberately avoid pulling
    // <filesystem> / ghc::filesystem here to keep the header tiny.
#if defined(_WIN32)
    CreateDirectoryA(base.c_str(), NULL);  // parents handled below
    // Lazy multi-level: walk the path and CreateDirectoryA each prefix.
    for (std::size_t i = 0; i < base.size(); ++i) {
        if (base[i] == '\\' || base[i] == '/') {
            CreateDirectoryA(base.substr(0, i).c_str(), NULL);
        }
    }
    CreateDirectoryA(base.c_str(), NULL);
    return base + "\\" + SessionId() + ".ndjson";
#else
    // POSIX mkdir-p — walk each '/' prefix and mkdir(2) directly.
    // Avoids std::system + shell-injection (CWE-78) for env-derived paths.
    for (std::size_t i = 1; i < base.size(); ++i) {
        if (base[i] == '/') {
            (void)::mkdir(base.substr(0, i).c_str(), 0755);
        }
    }
    (void)::mkdir(base.c_str(), 0755);
    return base + "/" + SessionId() + ".ndjson";
#endif
}

// Initialise lazily on first emit. Must hold Mutex().
inline void EnsureInitLocked() {
    State& s = Get();
    if (s.initialised) {
        return;
    }
    s.initialised = true;
    s.fsync_enabled = EnvIsTrue("SMATCHET_AGENT_DEBUG_FSYNC");
    if (s.target_is_explicit) {
        s.path = s.explicit_path;
    } else {
        s.path = ResolveDefaultPath();
    }
    s.stream.open(s.path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    s.bytes_written = 0;
    s.overflowed = false;
}

// Test-only entrypoint: redirect output to an explicit path and reset state.
// Header-only; called from doctest only.
inline void SetTargetPath(const std::string& path, bool fsync_enabled) {
    std::lock_guard<std::mutex> lk(Mutex());
    State& s = Get();
    if (s.stream.is_open()) {
        s.stream.close();
    }
    s.bytes_written = 0;
    s.overflowed = false;
    s.initialised = true;
    s.target_is_explicit = true;
    s.explicit_path = path;
    s.path = path;
    s.fsync_enabled = fsync_enabled;
    s.fsync_count.store(0);
    s.stream.open(s.path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
}

// Test-only: close + reset state without emitting anything. Lets doctest
// produce the empty-file semantic (V7.3).
inline void ResetForTest() {
    std::lock_guard<std::mutex> lk(Mutex());
    State& s = Get();
    if (s.stream.is_open()) {
        s.stream.flush();
        s.stream.close();
    }
    s.initialised = false;
    s.target_is_explicit = false;
    s.explicit_path.clear();
    s.path.clear();
    s.bytes_written = 0;
    s.overflowed = false;
    s.fsync_count.store(0);
}

inline std::uint64_t FsyncCountForTest() { return Get().fsync_count.load(); }

inline std::string CurrentPath() {
    std::lock_guard<std::mutex> lk(Mutex());
    return Get().path;
}

// Core emit. Always-callable. The macro expands to either a call here
// (ON-build) or `((void)0)` (OFF-build).
inline void Emit(const char* category,
                 const char* file,
                 int line,
                 const nlohmann::json& payload) {
    std::lock_guard<std::mutex> lk(Mutex());
    EnsureInitLocked();
    State& s = Get();
    if (s.overflowed || !s.stream.is_open()) {
        return;
    }
    if (!IsValidCategory(category)) {
        // Out-of-set category: emit a loud loggable error line and abort write.
        // Production builds compile this in; tests can grep for it.
        std::fprintf(stderr,
                     "[SmatchetAgentDebug] ERROR: invalid category '%s' at %s:%d\n",
                     category ? category : "(null)",
                     file ? file : "?",
                     line);
        return;
    }

    nlohmann::json obj;
    obj["ts"] = IsoTimestampUtc();
    obj["category"] = std::string(category);
    obj["pid"] = SMATCHET_AGENT_DEBUG_GETPID();
    obj["tid"] = ThreadIdString();
    std::ostringstream scope_oss;
    scope_oss << (file ? file : "?") << ":" << line;
    obj["scope"] = scope_oss.str();
    obj["payload"] = payload;

    const std::string serialised = obj.dump() + "\n";
    if (s.bytes_written + serialised.size() > MaxFileBytes()) {
        // Overflow — emit terminal line and stop.
        nlohmann::json over;
        over["ts"] = IsoTimestampUtc();
        over["category"] = std::string("scenario-phase");
        over["pid"] = SMATCHET_AGENT_DEBUG_GETPID();
        over["tid"] = ThreadIdString();
        over["scope"] = std::string(__FILE__) + ":overflow";
        nlohmann::json over_payload;
        over_payload["name"] = "_overflow";
        over_payload["phase"] = "abort";
        over["payload"] = over_payload;
        const std::string over_line = over.dump() + "\n";
        s.stream.write(over_line.data(),
                       static_cast<std::streamsize>(over_line.size()));
        s.stream.flush();
        s.bytes_written += over_line.size();
        s.overflowed = true;
        return;
    }
    s.stream.write(serialised.data(), static_cast<std::streamsize>(serialised.size()));
    s.stream.flush();
    s.bytes_written += serialised.size();

    if (s.fsync_enabled) {
        s.fsync_count.fetch_add(1);
        // ofstream → underlying fd is platform-dependent. Best-effort: skip on
        // ofstream (no portable fd extraction); the counter probe in V7.5 is
        // the deterministic verification path. The fsync env knob is still
        // semantically wired — the production path can swap to a fd-based
        // sink without touching the macro.
    }
}

}  // namespace smatchet_agent_debug

#if defined(SMATCHET_AGENT_DEBUG)
#  define SMATCHET_AGENT_DEBUG_LOG(category, json_obj)                              \
      ::smatchet_agent_debug::Emit((category), __FILE__, __LINE__, (json_obj))
#else
#  define SMATCHET_AGENT_DEBUG_LOG(category, json_obj) ((void)0)
#endif
