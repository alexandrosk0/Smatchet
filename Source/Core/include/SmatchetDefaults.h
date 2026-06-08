#pragma once

#include <cstdlib>
#include <string>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace SmatchetDefaults {

/** getenv wrapper: MSVC CRT marks getenv deprecated (C4996); use _dupenv_s on MSVC. */
inline std::string GetEnvString(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name) != 0 || buf == nullptr) {
        return {};
    }
    std::string out(buf);
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

constexpr char kDefaultDbPath[] = "Smatchet_LocalCache.sqlite";
constexpr char kDefaultBackendType[] = "Jira";

// Bug-report relay — seeded as the first-run default so a fresh install can
// submit bug/crash reports with zero setup. The relay holds the GitHub token
// server-side; the key below is a low-value shared access key (abuse
// mitigation, rate-limited + rotatable on the worker — NOT a credential).
// Users can point at their own relay or clear it in Preferences.
constexpr char kDefaultBugReportRelayUrl[] = "https://smatchet-bug-report-relay.smatchet.workers.dev/report";
constexpr char kDefaultBugReportRelayKey[] = "SmatchetKey";

namespace Mcp {
constexpr int kDefaultPort = 42360;
constexpr char kBindLocalhost[] = "127.0.0.1";
constexpr char kBindAny[] = "0.0.0.0";
constexpr char kSsePath[] = "/mcp/sse";
} // namespace Mcp
} // namespace SmatchetDefaults
