#include "CliCommandRunner.h"

#include "CliArgCoercion.h"
#include "StandaloneAppBootstrap.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "AppController.h"
#include "Commands/Scenarios/IScenario.h"
#include "ConfigManager.h"
#include "Json/BoundedJsonParse.h"
#include "SmatchetDefaults.h"

#include <nlohmann/json.hpp>

#if defined(SMATCHET_WITH_MCP)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include <httplib.h>
#endif // SMATCHET_WITH_MCP

#include <ghc/filesystem.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath (CPP_CODE_AUDIT.md #33g — was only transitively included)
#endif
#endif

namespace fs = ghc::filesystem;

namespace smatchet {
namespace cli {

namespace {

[[noreturn]] static void CliTerminateHandler() {
    std::fprintf(stderr, // pre-logger-init — LOG_* unavailable
                 "{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"handler-error\","
                 "\"message\":\"CLI hit std::terminate (uncaught exception). No state change occurred.\"}}\n");
    std::_Exit(4);
}

// Exit code map (kept in sync with the plan / CliCommandRunner.h header).
constexpr int kExitOk = 0;
constexpr int kExitUnknownCommand = 2;
constexpr int kExitValidation = 3;
constexpr int kExitHandler = 4;
constexpr int kExitConfirmRequired = 5;
constexpr int kExitNotConnected = 6;
constexpr int kExitTransport = 7;
constexpr int kExitTimeout = 8;
// 9 (dry-run-unsupported) is reachable once dry-run lands.

// Safe JSON value extractors — never throw on type mismatch or missing keys.
// ParseArgs stores every --key=value as a string, and external responses can
// have any shape; defensive reads avoid type_error.302 from nlohmann.

bool SafeBool(const nlohmann::json& j, const char* key, bool fallback) {
    try {
        if (!j.is_object() || !j.contains(key))
            return fallback;
        const auto& v = j[key];
        if (v.is_boolean())
            return v.get<bool>();
        if (v.is_number_integer())
            return v.get<long long>() != 0;
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            return s == "true" || s == "1" || s == "yes" || s == "on";
        }
        return fallback;
    } catch (...) { // catch-all-ok: defensive JSON read - any access/type error yields the fallback
        return fallback;
    }
}

std::string SafeString(const nlohmann::json& j, const char* key, const std::string& fallback = {}) {
    try {
        if (!j.is_object() || !j.contains(key))
            return fallback;
        const auto& v = j[key];
        if (v.is_string())
            return v.get<std::string>();
        if (v.is_null())
            return fallback;
        // Numeric / boolean → stringify for display purposes.
        return v.dump();
    } catch (...) { // catch-all-ok: defensive JSON read - any access/type error yields the fallback
        return fallback;
    }
}

int SafeInt(const nlohmann::json& j, const char* key, int fallback) {
    try {
        if (!j.is_object() || !j.contains(key))
            return fallback;
        const auto& v = j[key];
        if (v.is_number_integer())
            return static_cast<int>(v.get<long long>());
        if (v.is_number_float())
            return static_cast<int>(v.get<double>());
        if (v.is_string()) {
            try {
                return std::stoi(v.get<std::string>());
            } catch (...) { // catch-all-ok: non-numeric string -> fallback int
                return fallback;
            }
        }
        return fallback;
    } catch (...) { // catch-all-ok: defensive JSON read - any access/type error yields the fallback
        return fallback;
    }
}

/// Return j[key] as a nested object, or an empty object on any mismatch.
nlohmann::json SafeObject(const nlohmann::json& j, const char* key) {
    try {
        if (!j.is_object() || !j.contains(key))
            return nlohmann::json::object();
        const auto& v = j[key];
        return v.is_object() ? v : nlohmann::json::object();
    } catch (...) { // catch-all-ok: defensive JSON read - non-object/access error yields an empty object
        return nlohmann::json::object();
    }
}

/// Try to parse a JSON document; returns true on success.
bool SafeParseJson(const std::string& text, nlohmann::json& out) {
    std::string parseErr;
    out = smatchet::json_safe::ParseBounded(text, parseErr);
    if (!parseErr.empty()) {
        out = nlohmann::json::object();
        return false;
    }
    return true;
}

/// Build a canonical error envelope without ever throwing.
nlohmann::json MakeErrorEnvelope(const std::string& command, const std::string& code, const std::string& message,
                                 const std::string& hint = {}) {
    nlohmann::json env;
    env["ok"] = false;
    env["command"] = command;
    nlohmann::json err;
    err["code"] = code;
    err["message"] = message;
    if (!hint.empty())
        err["hint"] = hint;
    env["error"] = std::move(err);
    return env;
}

int ExitCodeForErrorCode(const std::string& code) {
    if (code == "ok")
        return kExitOk;
    if (code == "unknown-command")
        return kExitUnknownCommand;
    if (code == "missing-required-arg")
        return kExitValidation;
    if (code == "validation-error")
        return kExitValidation;
    if (code == "handler-error")
        return kExitHandler;
    if (code == "backend-error")
        return kExitHandler;
    if (code == "not-found")
        return kExitHandler;
    if (code == "confirm-required")
        return kExitConfirmRequired;
    if (code == "not-connected")
        return kExitNotConnected;
    if (code == "dry-run-unsupported")
        return 9;
    if (code == "timeout")
        return 8;
    return kExitHandler;
}

struct ParsedArgs {
    std::string commandName;   ///< first positional after `cmd`
    bool wantHelp = false;     ///< --help / -h after a command name
    bool wantListHelp = false; ///< `cmd --help` with no command name
    bool pretty = false;
    bool quiet = false;
    bool yes = false;
    bool dryRun = false;                            ///< --dry-run → inject __dry_run:true
    bool tokens = false;                            ///< --tokens → estimate output size, print to stderr, no stdout
    bool spawn = false;                             ///< --spawn → launch app subprocess if no instance reachable
    int timeoutMs = 0;                              ///< --timeout=<ms> → passed as __timeout_ms; 0=no cap
    std::string mcpHost;                            ///< empty -> default
    int mcpPort = 0;                                ///< 0 -> resolve from env / default
    nlohmann::json args = nlohmann::json::object(); ///< collected --key=value arguments
};

// CoerceCliArgValue lives in CliArgCoercion.h / .cpp — pure helper unit-tested
// in tests/Core/CliArgCoercion.test.cpp. ParseArgs below calls it for
// every `--key=value` token so handlers receive typed JSON instead of strings.

/// Return the path to the running executable (for relaunching as a spawned instance).
std::string GetExePath() {
#if defined(_WIN32)
    char buf[4096] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
#elif defined(__linux__)
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string();
#elif defined(__APPLE__)
    char buf[4096] = {};
    uint32_t sz = sizeof(buf);
    return (_NSGetExecutablePath(buf, &sz) == 0) ? std::string(buf) : std::string();
#else
    return std::string();
#endif
}

// FindFreePort is used only by the --spawn ephemeral-MCP-instance path, and its
// socket symbols come from the MCP-guarded <winsock2.h> / httplib includes above.
// Gate it on the same flag so the Smatchet-light build (MCP OFF) compiles — and
// so it isn't an unused function there. Caught by CI 'Windows + MSVC (Smatchet light)'.
#if defined(SMATCHET_WITH_MCP)
/// Bind to port 0, read back the OS-assigned port, close the socket.
/// Small TOCTOU race is acceptable for ephemeral test scenarios.
int FindFreePort() {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    int len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    int port = ntohs(addr.sin_port);
    closesocket(s);
    return port;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(s);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        close(s);
        return 0;
    }
    int port = ntohs(addr.sin_port);
    close(s);
    return port;
#endif
}
#endif // SMATCHET_WITH_MCP — FindFreePort

/// Poll until the file at outPath exists and is non-empty, or timeoutMs elapses.
bool WaitForFile(const std::string& outPath, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (fs::exists(fs::path(outPath), ec) && fs::file_size(fs::path(outPath), ec) > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

std::string EnvOr(const char* name, std::string fallback) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* v = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return (v && *v) ? std::string(v) : std::move(fallback);
}

int EnvIntOr(const char* name, int fallback) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* v = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!v || !*v)
        return fallback;
    try {
        return std::stoi(std::string(v));
    } catch (...) { // catch-all-ok: malformed env override -> caller's fallback int
        return fallback;
    }
}

bool ParseArgs(int argc, char** argv, ParsedArgs& out, std::string& outError) {
    // argv[0]=exe, argv[1]=cmd, argv[2]=<name>...
    int i = 2;
    if (i >= argc) {
        out.wantListHelp = true;
        return true;
    }
    const std::string firstAfterCmd = argv[i];
    if (firstAfterCmd == "--help" || firstAfterCmd == "-h") {
        out.wantListHelp = true;
        return true;
    }
    out.commandName = firstAfterCmd;
    ++i;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            out.wantHelp = true;
            continue;
        }
        if (a == "--pretty") {
            out.pretty = true;
            continue;
        }
        if (a == "--quiet" || a == "-q") {
            out.quiet = true;
            continue;
        }
        if (a == "--yes") {
            out.yes = true;
            continue;
        }
        if (a == "--dry-run") {
            out.dryRun = true;
            continue;
        }
        if (a == "--tokens") {
            out.tokens = true;
            continue;
        }
        if (a == "--spawn") {
            out.spawn = true;
            continue;
        }
#if defined(SMATCHET_WITH_MCP)
        if (a.rfind("--mcp-host=", 0) == 0) {
            out.mcpHost = a.substr(11);
            if (out.mcpHost.empty()) {
                outError = "invalid --mcp-host value (must not be empty)";
                return false;
            }
            continue;
        }
        if (a.rfind("--mcp-port=", 0) == 0) {
            try {
                out.mcpPort = std::stoi(a.substr(11));
            } catch (...) { // catch-all-ok: non-integer --mcp-port -> validation error returned to caller below
                outError = "invalid --mcp-port value";
                return false;
            }
            if (!IsValidMcpPort(out.mcpPort)) {
                outError = "invalid --mcp-port value (out of range 1-65535)";
                return false;
            }
            continue;
        }
#endif
        if (a.rfind("--timeout=", 0) == 0) {
            try {
                out.timeoutMs = std::stoi(a.substr(10));
            } catch (...) { // catch-all-ok: non-integer --timeout -> validation error returned to caller below
                outError = "invalid --timeout value (expected integer ms)";
                return false;
            }
            if (out.timeoutMs < 0) {
                outError = "invalid --timeout value (must be >= 0)";
                return false;
            }
            continue;
        }
        // --key=value or --flag
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            const std::string body = a.substr(2);
            const auto eq = body.find('=');
            if (eq == std::string::npos) {
                out.args[body] = true;
            } else {
                out.args[body.substr(0, eq)] = CoerceCliArgValue(body.substr(eq + 1));
            }
            continue;
        }
        outError = "unexpected argument: " + a;
        return false;
    }
    return true;
}

void EmitEnvelope(const nlohmann::json& envelope, bool pretty, bool quiet) {
    if (!quiet) {
        try {
            std::fprintf(stdout, "%s\n",
                         pretty ? envelope.dump(2).c_str()
                                : envelope.dump().c_str()); // CLI stdout — product output, not logging
        } catch (...) {          // catch-all-ok: serialize failure emits a handler-error envelope to stdout
            std::fprintf(stdout, // CLI stdout — product output, not logging
                         "{\"ok\":false,\"error\":{\"code\":\"handler-error\","
                         "\"message\":\"failed to serialize envelope\"}}\n");
        }
        return;
    }
    if (!SafeBool(envelope, "ok", false)) {
        return;
    }
    nlohmann::json data = SafeObject(envelope, "data");
    if (data.contains("items") && data["items"].is_array()) {
        for (const auto& it : data["items"]) {
            if (it.is_string()) {
                std::fprintf(stdout, "%s\n", it.get<std::string>().c_str()); // CLI stdout — product output, not logging
            } else if (it.is_object() && it.contains("id") && it["id"].is_string()) {
                std::fprintf(stdout, "%s\n",
                             it["id"].get<std::string>().c_str()); // CLI stdout — product output, not logging
            } else if (it.is_object() && it.contains("name") && it["name"].is_string()) {
                std::fprintf(stdout, "%s\n",
                             it["name"].get<std::string>().c_str()); // CLI stdout — product output, not logging
            } else {
                std::fprintf(stdout, "%s\n", it.dump().c_str()); // CLI stdout — product output, not logging
            }
        }
        return;
    }
    if (data.is_string()) {
        std::fprintf(stdout, "%s\n", data.get<std::string>().c_str()); // CLI stdout — product output, not logging
        return;
    }
    if (data.is_number() || data.is_boolean() || data.is_null()) {
        std::fprintf(stdout, "%s\n", data.dump().c_str()); // CLI stdout — product output, not logging
        return;
    }
    std::fprintf(stdout, "%s\n", data.dump().c_str()); // CLI stdout — product output, not logging
}

void EmitErrorToStderr(const nlohmann::json& envelope) {
    std::fprintf(stderr, "%s\n", envelope.dump().c_str());
} // CLI stdout — product output, not logging

#if defined(SMATCHET_WITH_MCP)
/// `draws * 8` lowercase hex chars from std::random_device. random_device is
/// non-deterministic on every supported host and each draw yields 32 bits, so the
/// result carries `draws * 32` bits of entropy. Shared by the spawn-log filename
/// suffix and the per-spawn MCP auth token so the two don't duplicate the
/// random-device / hex-formatting pattern (DRY — no second copy vs origin/develop).
std::string RandomHexToken(int draws) {
    std::random_device rd;
    std::string out;
    out.reserve(static_cast<size_t>(draws) * 8);
    char buf[9];
    for (int i = 0; i < draws; ++i) {
        std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned int>(rd()));
        out += buf;
    }
    return out;
}

/// 16 hex chars (64 bits) of randomness for the spawn-log filename. A predictable
/// `<pid>-<port>` name let any same-user-or-not process pre-plant a file or
/// symlink at the known path that the spawn redirect then wrote through (audit #19).
std::string SpawnLogRandomToken() { return RandomHexToken(2); }

/// 32 hex chars (128 bits) of per-spawn entropy for the ephemeral MCP auth token.
/// The --spawn parent hands this to its child via SMATCHET_MCP_SPAWN_TOKEN so the
/// child (which has no persisted mcp_auth_token) requires + accepts exactly this
/// token under the secure McpRequireTokenOnLoopback default — every other local
/// caller is still denied. Ephemeral, never persisted, cleared from the parent
/// env right after the child is spawned.
std::string SpawnAuthToken() { return RandomHexToken(4); }

/// Compute a per-spawn log file path so child stdout/stderr survive the
/// parent's --spawn redirection. Format:
/// $TMPDIR/Smatchet-spawn-<pid>-<port>-<rand>.log. The random suffix makes the
/// path unpredictable so it can't be pre-planted (audit #19); the open below
/// pairs it with an exclusive / no-follow create as the actual race guard.
std::string ComputeSpawnLogPath(int port) {
#if defined(_WIN32)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* tmpEnv = std::getenv("TMP");
    if (!tmpEnv)
        tmpEnv = std::getenv("TEMP");
    if (!tmpEnv)
        tmpEnv = "C:\\Windows\\Temp";
    const DWORD parentPid = GetCurrentProcessId();
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
    const char* tmpEnv = std::getenv("TMPDIR");
    if (!tmpEnv)
        tmpEnv = "/tmp";
    const pid_t parentPid = getpid();
#endif
    return std::string(tmpEnv) + "/Smatchet-spawn-" + std::to_string(static_cast<long long>(parentPid)) + "-" +
           std::to_string(port) + "-" + SpawnLogRandomToken() + ".log";
}

bool LaunchEphemeralInstance(const std::string& exePath, int port, std::string* outLogPath,
                             const std::string& authToken) {
    if (exePath.empty())
        return false;
    const std::string portStr = std::to_string(port);
    const std::string logPath = ComputeSpawnLogPath(port);
    if (outLogPath)
        *outLogPath = logPath;
        // main.cpp parses `--mcp-port <port>` as two separate argv entries (space-separated),
        // NOT `--mcp-port=<port>` — using the equals form would silently fall through.
#if defined(_WIN32)
    // CommandLineToArgvW handles quoted whitespace; pass space-separated tokens.
    std::string cmdLine = "\"" + exePath + "\" --ephemeral --mcp-port " + portStr;
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    // CREATE_NEW fails if the path already exists, so a pre-planted file or
    // symlink at the (now random) path cannot be hijacked (audit #19). The
    // random suffix makes a spurious collision negligible.
    HANDLE hLog =
        CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        // Log capture failure is non-fatal — fall back to inheriting parent
        // handles so the spawn still works, just with no captured diagnostics.
        si.dwFlags &= ~STARTF_USESTDHANDLES;
    } else {
        si.hStdOutput = hLog;
        si.hStdError = hLog;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi = {};
    // Hand the per-spawn MCP token to the child via its inherited environment
    // (lpEnvironment=nullptr → child snapshots the parent's current env block).
    // Set immediately before CreateProcessA to minimise the window, then clear it
    // from the parent's own env right after — the child already has its copy, and
    // the short-lived parent shouldn't keep the secret in its environment.
    if (!authToken.empty())
        SetEnvironmentVariableA("SMATCHET_MCP_SPAWN_TOKEN", authToken.c_str());
    BOOL ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (!authToken.empty())
        SetEnvironmentVariableA("SMATCHET_MCP_SPAWN_TOKEN", nullptr);
    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog); // child holds its own dup; parent can release.
    }
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        // Redirect stdout + stderr to the log file. open() returns -1 on
        // failure; in that case stdio falls through to whatever the parent had.
        // O_EXCL|O_CREAT refuses to open a path that already exists, and
        // O_NOFOLLOW refuses a symlink at the final component, so a pre-planted
        // file or symlink at the (now random) path can't redirect the write
        // (audit #19 — symlink race). O_NOFOLLOW is POSIX.1-2008; guard it for
        // the rare host that lacks the macro.
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
        int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        // Hand the per-spawn MCP token to the child via its environment. Done in
        // the forked child (before execv) so the parent's own env is never touched.
        if (!authToken.empty())
            setenv("SMATCHET_MCP_SPAWN_TOKEN", authToken.c_str(), 1);
        const char* args[] = {exePath.c_str(), "--ephemeral", "--mcp-port", portStr.c_str(), nullptr};
        execv(exePath.c_str(), const_cast<char* const*>(args));
        _exit(1);
    }
    return true;
#endif
}

/// Outcome of the MCP-ready probe: distinguishes a genuine startup timeout from a
/// server that is up but rejected our auth token (a real handshake misconfig that
/// must surface immediately, not be masked as a slow startup).
enum class McpReadyStatus { Ready, Timeout, AuthRejected };

/// Poll until the MCP endpoint at host:port becomes reachable or timeoutMs elapses.
/// Sends the per-spawn auth token so the secure-default child (which now requires a
/// token) accepts the probe. A received HTTP 401 means the server is up but rejected
/// the token — fast-fail rather than poll to the full timeout and mask the cause.
McpReadyStatus WaitForMcpReady(const std::string& host, int port, int timeoutMs, const std::string& authToken) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(0, 200000); // 200 ms
        cli.set_read_timeout(1, 0);
        if (!authToken.empty())
            cli.set_default_headers({{"X-Smatchet-Token", authToken}});
        nlohmann::json body;
        body["name"] = "app.version";
        body["arguments"] = nlohmann::json::object();
        auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
        if (res && res->status >= 200 && res->status < 300)
            return McpReadyStatus::Ready;
        // The server answered but rejected the token (auth gate). This is a real
        // handshake failure, not a slow boot — surface it now.
        if (res && res->status == 401)
            return McpReadyStatus::AuthRejected;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return McpReadyStatus::Timeout;
}

// Try to fetch the live catalog and print a categorised summary. Returns true on
// success, false if no running instance is reachable — caller can show a hint.
bool TryAppendLiveCatalogToHelp(std::FILE* out, const std::string& host, int port) {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(3, 0);
    nlohmann::json body;
    body["name"] = "commands.list";
    body["arguments"] = nlohmann::json::object();
    auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
    if (!res || res->status < 200 || res->status >= 300)
        return false;
    try {
        std::string parseErr;
        const auto parsed = smatchet::json_safe::ParseBounded(res->body, parseErr);
        if (!parseErr.empty())
            return false;
        if (!parsed.contains("content") || !parsed["content"].is_array() || parsed["content"].empty() ||
            !parsed["content"][0].contains("text")) {
            return false;
        }
        std::string envErr;
        const auto envelope =
            smatchet::json_safe::ParseBounded(parsed["content"][0]["text"].get<std::string>(), envErr);
        if (!envErr.empty())
            return false;
        if (!envelope.value("ok", false))
            return false;
        const auto items = envelope["data"]["items"];
        if (!items.is_array() || items.empty())
            return false;

        // Group by category, preserving sort order from the registry (alphabetical).
        std::string lastCategory;
        std::fprintf(out, // CLI stdout — product output, not logging
                     "\nAvailable commands (%d total — fetched from running instance):\n",
                     envelope["data"].value("total", static_cast<int>(items.size())));
        for (const auto& item : items) {
            const std::string category = item.value("category", "?");
            const std::string name = item.value("name", "?");
            const std::string summary = item.value("summary", "");
            const bool destructive = item.value("destructive", false);
            if (category != lastCategory) {
                std::fprintf(out, "\n  [%s]\n", category.c_str()); // CLI stdout — product output, not logging
                lastCategory = category;
            }
            std::fprintf(out, "    %-32s %s%s\n", name.c_str(), destructive ? "(destructive) " : "",
                         summary.c_str()); // CLI stdout — product output, not logging
        }
        std::fprintf(out, // CLI stdout — product output, not logging
                     "\nFor full schema:    Smatchet.exe cmd commands.help --name=<name>\n"
                     "All commands + schema: Smatchet.exe cmd commands.list --full --pretty\n");
        return true;
    } catch (...) { // catch-all-ok: best-effort live-catalog append - any failure leaves help static
        return false;
    }
}

void PrintCliHelp(std::FILE* out, const std::string& mcpHost, int mcpPort) {
    std::fprintf(out, // CLI stdout — product output, not logging
                 "Smatchet CLI — unified Command System front-end.\n"
                 "\n"
                 "Usage:\n"
                 "  Smatchet.exe cmd <name> [--key=value ...] [flags]\n"
                 "  Smatchet.exe cmd <name> --help        Show command schema + examples.\n"
                 "  Smatchet.exe cmd commands.list        List all available commands.\n"
                 "  Smatchet.exe cmd commands.list --full Include params per command.\n"
                 "  Smatchet.exe cmd commands.search --query=<q>  Fuzzy-find a command.\n"
                 "\n"
                 "Output flags:\n"
                 "  --pretty                Indent stdout JSON (2 spaces).\n"
                 "  --quiet, -q             Bare scalar(s) on stdout (id/name per line for lists).\n"
                 "  --yes                   Confirm a destructive command (no prompt).\n"
                 "  --dry-run               Preview a mutation without applying it (exit 9 if unsupported).\n"
                 "  --tokens                Estimate output token count; print to stderr, no stdout data.\n"
                 "  --timeout=<ms>          Cap async wait; 0=no cap (default: SMATCHET_SPAWN_TIMEOUT_MS or 0).\n"
                 "  --mcp-host=<host>       Override MCP host (env SMATCHET_MCP_HOST / 127.0.0.1).\n"
                 "  --mcp-port=<int>        Override MCP port (env SMATCHET_MCP_PORT / instance.json / %d).\n"
                 "\n"
                 "Exit codes: 0=ok 2=unknown-command 3=validation 4=handler 5=confirm-required\n"
                 "            6=not-connected 7=transport 8=timeout 9=dry-run-unsupported\n"
                 "\n"
                 "Notes:\n"
                 "  - Requires a running Smatchet instance with MCP enabled.\n"
                 "  - Stdout is always JSON (or bare scalars under --quiet). Logs go to stderr.\n",
                 SmatchetDefaults::Mcp::kDefaultPort);

    // Try to fetch a live command summary so the help is informative on first run.
    if (!TryAppendLiveCatalogToHelp(out, mcpHost, mcpPort)) {
        std::fprintf(out, // CLI stdout — product output, not logging
                     "\n(No running instance detected at %s:%d — start Smatchet with mcp_enabled\n"
                     " to see the live command catalog here. See CLI_GUIDE.md for the full reference.)\n",
                     mcpHost.c_str(), mcpPort);
    }
}

// Parse the MCP JSON-RPC `result.content[0].text` field back into the envelope.
nlohmann::json ExtractEnvelopeFromMcpResult(const nlohmann::json& body) {
    // REST shape: { "content": [{"type":"text","text":"<envelope-json>"}] }
    // JSON-RPC shape: { "result": { "content": [...] } } or { "error": {...} }
    if (body.contains("error")) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = "";
        env["error"] = body["error"];
        return env;
    }
    const nlohmann::json* result = nullptr;
    if (body.contains("result")) {
        result = &body["result"];
    } else {
        result = &body;
    }
    try {
        if (result->contains("content") && (*result)["content"].is_array() && !(*result)["content"].empty() &&
            (*result)["content"][0].is_object() && (*result)["content"][0].contains("text") &&
            (*result)["content"][0]["text"].is_string()) {
            const std::string text = (*result)["content"][0]["text"].get<std::string>();
            nlohmann::json parsed;
            if (SafeParseJson(text, parsed))
                return parsed;
            nlohmann::json env;
            env["ok"] = true;
            env["data"] = text;
            return env;
        }
    } catch (...) { // catch-all-ok: unexpected MCP result shape -> return result as-is below
        // Fall through — return result as-is below.
    }
    return *result;
}

void PostAppQuitBestEffort(httplib::Client& cli) {
    try {
        nlohmann::json quitBody;
        quitBody["name"] = "app.quit";
        quitBody["arguments"] = {{"__confirm", true}}; // app.quit is Destructive
        cli.Post("/mcp/tools/call", quitBody.dump(), "application/json");
    } catch (...) { // catch-all-ok: spawn teardown must not overwrite the already-emitted command result.
    }
}

void PostAppQuitBestEffort(const std::string& host, int port, const std::string& authToken) {
    if (host.empty() || port <= 0)
        return;
    try {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(1, 0);
        if (!authToken.empty())
            cli.set_default_headers({{"X-Smatchet-Token", authToken}});
        PostAppQuitBestEffort(cli);
    } catch (...) { // catch-all-ok: exception cleanup is best-effort after a spawn failure.
    }
}

/// Phase 1 of SpawnAndRun: discover exe path, bind a free port, launch ephemeral instance,
/// and wait until its MCP endpoint is reachable. Returns kExitOk on success; an error exit
/// code on failure (error envelope already emitted to stderr). Fills host and port out-params.
/// `provisionToken` is injected into the child env (empty when an operator-configured token is
/// reused, so the child keeps its own); `requestToken` is the token the child will actually
/// require, sent on the readiness probe.
int SpawnAndRunSetup(const std::string& commandName, std::string& outHost, int& outPort,
                     const std::string& provisionToken, const std::string& requestToken) {
    const std::string exePath = GetExePath();
    if (exePath.empty()) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = commandName;
        env["error"] = {{"code", "not-connected"}, {"message", "--spawn: could not determine exe path to relaunch."}};
        EmitErrorToStderr(env);
        return kExitNotConnected;
    }

    const int port = FindFreePort();
    if (port == 0) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = commandName;
        env["error"] = {{"code", "not-connected"}, {"message", "--spawn: could not bind a free TCP port."}};
        EmitErrorToStderr(env);
        return kExitNotConnected;
    }

    std::fprintf(stderr, "[spawn] launching ephemeral instance on port %d ...\n",
                 port); // CLI stdout — product output, not logging
    std::string childLogPath;
    if (!LaunchEphemeralInstance(exePath, port, &childLogPath, provisionToken)) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = commandName;
        env["error"] = {{"code", "not-connected"}, {"message", "--spawn: failed to launch subprocess."}};
        EmitErrorToStderr(env);
        return kExitNotConnected;
    }
    if (!childLogPath.empty()) {
        std::fprintf(stderr, "[spawn] child stdout/stderr → %s\n",
                     childLogPath.c_str()); // CLI stdout — product output, not logging
    }

    // Bumped 15s → 30s post Phase D/E AI merges: the addition of OpenAi, Anthropic,
    // Ollama, AiNdjsonParser, and Lua glue pushed startup past the prior ready window
    // and bucket-E gates regressed on develop tip. Env override SMATCHET_SPAWN_READY_MS
    // allows tightening on faster runners or raising further on cold-cache CI.
    // Lazy-loading AI clients is the architectural follow-up — entry remains open
    // in docs/self-improvement/categories/tooling.md.
    int readyTimeoutMs = 30000;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    if (const char* envOverride = std::getenv("SMATCHET_SPAWN_READY_MS")) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        try {
            int v = std::stoi(envOverride);
            if (v > 0)
                readyTimeoutMs = v;
        } catch (...) { // catch-all-ok: malformed SMATCHET_SPAWN_READY_MS -> default ready timeout
            // Ignore malformed override; fall through to default.
        }
    }
    const std::string host = "127.0.0.1";
    std::fprintf(stderr, "[spawn] waiting for MCP ready (up to %d s) ...\n",
                 readyTimeoutMs / 1000); // CLI stdout — product output, not logging
    const McpReadyStatus ready = WaitForMcpReady(host, port, readyTimeoutMs, requestToken);
    if (ready == McpReadyStatus::AuthRejected) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = commandName;
        nlohmann::json err;
        err["code"] = "not-connected";
        err["message"] = "--spawn: ephemeral instance rejected the MCP auth token (HTTP 401) — "
                         "spawn token handshake failed.";
        env["error"] = err;
        EmitErrorToStderr(env);
        return kExitNotConnected;
    }
    if (ready != McpReadyStatus::Ready) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = commandName;
        env["error"] = {{"code", "timeout"},
                        {"message", "--spawn: ephemeral instance did not become reachable in time."}};
        EmitErrorToStderr(env);
        return kExitTimeout;
    }

    outHost = host;
    outPort = port;
    return kExitOk;
}

/// Phase 2 of SpawnAndRun: POST the command and extract the response envelope.
/// Returns kExitOk with envelope filled on success; otherwise emits an error to stderr,
/// sends a best-effort app.quit, and returns the appropriate exit code.
int SpawnAndRunDispatch(httplib::Client& cli, const std::string& commandName, const nlohmann::json& argsToSend,
                        nlohmann::json& outEnvelope) {
    nlohmann::json body;
    body["name"] = commandName;
    body["arguments"] = argsToSend;

    auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
    if (!res || res->status < 200 || res->status >= 300) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = commandName;
        env["error"] = {{"code", "transport"}, {"message", "--spawn: command dispatch failed after launch."}};
        EmitErrorToStderr(env);
        // Best-effort quit so a reachable-but-erroring instance isn't orphaned.
        PostAppQuitBestEffort(cli);
        return kExitTransport;
    }

    nlohmann::json parsedBody;
    if (!SafeParseJson(res->body, parsedBody)) {
        nlohmann::json env = MakeErrorEnvelope(commandName, "transport",
                                               "--spawn: server returned non-JSON response (status " +
                                                   std::to_string(res->status) + ").");
        EmitErrorToStderr(env);
        // Best-effort quit.
        PostAppQuitBestEffort(cli);
        return kExitTransport;
    }

    try {
        outEnvelope = ExtractEnvelopeFromMcpResult(parsedBody);
    } catch (...) { // catch-all-ok: extract failure -> transport error envelope
        outEnvelope = MakeErrorEnvelope(commandName, "transport", "--spawn: failed to extract envelope from response.");
    }
    return kExitOk;
}

/// Phase 3 of SpawnAndRun: handle async scenario.run result.
/// Waits for the output file, reads it, and emits the result envelope.
/// Returns kExitOk on success; 8 (timeout) or kExitHandler on failure.
/// Sends app.quit on every failure path so the orchestrator's early-return is safe.
int SpawnAndRunHandleAsync(const ParsedArgs& pa, httplib::Client& cli, const std::string& commandName,
                           const std::string& outPath, int frames, int scenarioWaitMs) {
    std::fprintf(stderr, "[spawn] scenario running (%d frames / ~%d s) ...\n", frames,
                 frames / 60); // CLI stdout — product output, not logging
    const bool fileReady = WaitForFile(outPath, scenarioWaitMs);
    // Small delay to ensure fwrite+fclose has fully flushed before we read.
    // The writer calls dump(2) → fwrite → fclose, so once size>0 it's usually complete,
    // but kernel write buffers can briefly show a partial file on some filesystems.
    if (fileReady)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!fileReady) {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = commandName;
        errEnv["error"] = {{"code", "timeout"},
                           {"message", "--spawn: scenario did not finish within expected time."},
                           {"hint", "Try --timeout=<larger-ms> or --frames=<smaller-n>"}};
        EmitErrorToStderr(errEnv);
        // Still try to quit the spawned app.
        PostAppQuitBestEffort(cli);
        return kExitTimeout;
    }

    // Read the result file and build an envelope around it.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // fopen: cross-platform — fopen_s is MSVC-only
#endif
    std::FILE* f = std::fopen(outPath.c_str(), "rb");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!f) {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = commandName;
        errEnv["error"] = {{"code", "handler-error"}, {"message", "--spawn: could not read result file: " + outPath}};
        EmitErrorToStderr(errEnv);
        PostAppQuitBestEffort(cli);
        return kExitHandler;
    }

    std::string content;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        content.append(buf, n);
    std::fclose(f);
    try {
        std::string parseErr;
        const nlohmann::json fileData = smatchet::json_safe::ParseBounded(content, parseErr);
        if (!parseErr.empty()) {
            nlohmann::json errEnv;
            errEnv["ok"] = false;
            errEnv["command"] = commandName;
            errEnv["error"] = {{"code", "handler-error"},
                               {"message", "--spawn: result file is not valid JSON: " + outPath}};
            EmitErrorToStderr(errEnv);
            // Send app.quit on THIS failure path too (mirrors the timeout + file-read
            // branches above) — honour the documented invariant at this function's header that
            // every failure path quits the spawned app. Omitting it orphaned the ephemeral
            // instance (TCP port held) whenever the result file existed but wasn't valid JSON.
            PostAppQuitBestEffort(cli);
            return kExitHandler;
        }
        const bool captureRequested = SafeBool(fileData, "captureRequested", false);
        const std::string screenshotPath = SafeString(fileData, "screenshotPath");
        if (captureRequested && !screenshotPath.empty()) {
            std::fprintf(stderr,
                         "[spawn] waiting for screenshot capture ...\n"); // CLI stdout — product output, not logging
            WaitForFile(screenshotPath, 2000);
        }

        nlohmann::json resultEnv;
        resultEnv["ok"] = true;
        resultEnv["command"] = commandName;
        resultEnv["data"] = fileData;
        EmitEnvelope(resultEnv, pa.pretty, pa.quiet);
    } catch (...) { // catch-all-ok: result file not valid JSON -> handler-error envelope + app.quit
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = commandName;
        errEnv["error"] = {{"code", "handler-error"},
                           {"message", "--spawn: result file is not valid JSON: " + outPath}};
        EmitErrorToStderr(errEnv);
        PostAppQuitBestEffort(cli);
        return kExitHandler;
    }
    return kExitOk;
}

/// Phase 4 of SpawnAndRun: handle a synchronous (non-async) command result.
/// Emits the envelope and returns an exit code. On error, also sends app.quit.
int SpawnAndRunHandleSync(const ParsedArgs& pa, httplib::Client& cli, const nlohmann::json& envelope) {
    EmitEnvelope(envelope, pa.pretty, pa.quiet);
    if (!SafeBool(envelope, "ok", false)) {
        const nlohmann::json errObj = SafeObject(envelope, "error");
        const std::string code = SafeString(errObj, "code");
        PostAppQuitBestEffort(cli);
        return ExitCodeForErrorCode(code);
    }
    return kExitOk;
}

/// Full spawn-attach-run flow invoked when --spawn is set and no instance is reachable.
/// Launches a hidden ephemeral app instance, sends the command, waits for results, quits the app.
int SpawnAndRun(const ParsedArgs& pa, const std::string& commandName, const nlohmann::json& argsToSendRaw) {
    std::string host;
    int port = 0;
    bool spawnedReady = false;
    // Per-spawn MCP auth token, split into two roles so --spawn works under the secure
    // McpRequireTokenOnLoopback default for BOTH config shapes:
    //   * No operator token configured (fresh / secure default): mint a 128-bit
    //     ephemeral token, inject it into the child env (provisionToken) so the child
    //     adopts + requires it, and send it on every request (requestToken).
    //   * Operator token configured: the child requires THAT token, so send the
    //     configured token and provision nothing — injecting a different token would
    //     make the child 401 the parent (the configured-token regression CR flagged).
    const TrackerConfig spawnCfg = ConfigManager::Load();
    const bool haveConfiguredToken = !spawnCfg.McpAuthToken.empty();
    const std::string requestToken = haveConfiguredToken ? spawnCfg.McpAuthToken : SpawnAuthToken();
    const std::string provisionToken = haveConfiguredToken ? std::string() : requestToken;
    try {
        // `outPath`/`outLog` (ui_test.run, scenario.run, perf.dump) are no longer
        // CLI-CWD-relative — every handler that reads them confines the value under a
        // fixed <userData> subdir (SECURITY_AUDIT.md path-confinement sweep, #1566) and
        // REJECTS an absolute path outright. There used to be a normalization step here
        // that resolved a relative outPath/outLog to absolute against the CLI's CWD "so
        // both processes agree on location" — that predates the confinement change and is
        // now actively wrong: it silently turned a valid relative value into one every
        // confined handler rejects, breaking --spawn for any caller passing a relative
        // outPath/outLog (CPP_CODE_AUDIT.md #8/#9 follow-up; see
        // scripts/dev/test-ui-jira-deterministic-backend.sh's outLog handling). Confinement
        // resolves relative to <userData>, which both processes agree on without any CWD
        // translation, so argsToSendRaw is forwarded to the spawned child unmodified.
        const nlohmann::json& argsToSend = argsToSendRaw;

        // Phase 1: launch ephemeral instance and wait for MCP ready.
        const int setupResult = SpawnAndRunSetup(commandName, host, port, provisionToken, requestToken);
        if (setupResult != kExitOk)
            return setupResult;
        spawnedReady = true;

        // For scenario.run, compute a wait timeout from the frames param.
        // ParseArgs stores --key=value pairs as strings; coerce defensively.
        int frames = 600;
        if (argsToSend.contains("frames")) {
            frames = ScenarioFramesFromJson(argsToSend["frames"], 600);
        }
        const int scenarioWaitMs = (frames / 60 + 30) * 1000;

        // Phase 2: dispatch the command and extract the response envelope.
        httplib::Client cli(host, port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(30, 0);
        // The default header rides every cli.Post below — dispatch, the async/sync
        // result handlers, and the app.quit teardown — so each carries the token.
        cli.set_default_headers({{"X-Smatchet-Token", requestToken}});
        nlohmann::json envelope;
        const int dispatchResult = SpawnAndRunDispatch(cli, commandName, argsToSend, envelope);
        if (dispatchResult != kExitOk)
            return dispatchResult;

        // Phase 3 / 4: handle async or synchronous result.
        nlohmann::json envData = SafeObject(envelope, "data");
        const bool isAsync = SafeBool(envelope, "ok", false) && SafeBool(envData, "running", false) &&
                             envData.contains("outPath") && envData["outPath"].is_string();
        int resultCode = kExitOk;
        if (isAsync) {
            // Phase 3: wait for the output file and emit the result.
            const std::string outPath = SafeString(envData, "outPath");
            resultCode = SpawnAndRunHandleAsync(pa, cli, commandName, outPath, frames, scenarioWaitMs);
            if (resultCode != kExitOk)
                return resultCode; // async helper sends app.quit on every failure path
        } else {
            // Phase 4: synchronous command — emit directly.
            resultCode = SpawnAndRunHandleSync(pa, cli, envelope);
            if (resultCode != kExitOk)
                return resultCode; // sync helper already sent app.quit on error
        }

        // Phase 5 (quit): send app.quit to the spawned instance (app.quit is Destructive → __confirm:true).
        std::fprintf(stderr, "[spawn] sending app.quit ...\n"); // CLI stdout — product output, not logging
        PostAppQuitBestEffort(cli);
        return kExitOk;
    } catch (const std::exception& e) {
        if (spawnedReady)
            PostAppQuitBestEffort(host, port, requestToken);
        nlohmann::json env =
            MakeErrorEnvelope(commandName, "handler-error", std::string("--spawn: internal error: ") + e.what());
        try {
            EmitErrorToStderr(env);
        } catch (...) { // catch-all-ok: already handling a spawn exception; preserve handler exit code.
        }
        return kExitHandler;
    } catch (...) { // catch-all-ok: unknown spawn-flow exception -> error JSON to stderr + app.quit
        if (spawnedReady)
            PostAppQuitBestEffort(host, port, requestToken);
        std::fprintf(stderr, // CLI stdout — product output, not logging
                     "{\"ok\":false,\"command\":\"%s\",\"error\":{\"code\":\"handler-error\","
                     "\"message\":\"--spawn: unknown internal exception.\"}}\n",
                     commandName.c_str());
        return kExitHandler;
    }
}

#endif // SMATCHET_WITH_MCP

#if !defined(SMATCHET_WITH_MCP)

void PrintCliHelpInProcess(std::FILE* out) {
    std::fprintf(out,
                 "Smatchet CLI — in-process Command System (light build).\n" // CLI stdout — product output, not logging
                 "\n"
                 "Usage:\n"
                 "  Smatchet-Light.exe cmd <name> [--key=value ...] [flags]\n"
                 "  Smatchet-Light.exe cmd commands.list        List registered commands.\n"
                 "\n"
                 "Output flags:\n"
                 "  --pretty                Indent stdout JSON (2 spaces).\n"
                 "  --quiet, -q             Bare scalar(s) on stdout.\n"
                 "  --yes                   Confirm a destructive command.\n"
                 "  --dry-run               Preview a mutation without applying it.\n"
                 "\n"
                 "Notes:\n"
                 "  - Boots a hidden instance per invocation (no MCP attach).\n"
                 "  - --spawn is ignored on light builds.\n"
                 "  - Stdout is always JSON. Logs go to stderr.\n");
}

bool IsTier1MetaCommand(const std::string& name) {
    static const char* const kAllow[] = {"commands.list", "commands.help", "commands.search", "perf.reset",
                                         "perf.snapshot"};
    return std::any_of(std::begin(kAllow), std::end(kAllow), [&name](const char* allowed) { return name == allowed; });
}

// Drive an async scenario command to completion in-process: render-loop until
// the scenario finishes (or the deadline lapses), wait for its out-file, read it,
// and fold the parsed result back into `envelope` as the command data. Extracted
// verbatim from RunCmdInProcessImpl's async branch. Returns a non-negative exit
// code when it has already emitted an error + shut down `boot` (caller returns it
// immediately), or -1 to signal success — caller proceeds to the shared emit path.
static int RunAsyncScenarioInProcess(standalone::BootstrapContext& boot, const std::string& toolName,
                                     const nlohmann::json& argsToSend, const ParsedArgs& pa,
                                     const nlohmann::json& envData, nlohmann::json& envelope) {
    const std::string outPath = SafeString(envData, "outPath");
    int frames = 600;
    if (argsToSend.contains("frames")) {
        frames = ScenarioFramesFromJson(argsToSend["frames"], 600);
    }
    const int scenarioWaitMs = (pa.timeoutMs > 0) ? pa.timeoutMs : ((frames / 60 + 30) * 1000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(scenarioWaitMs);

    standalone::RunRenderLoop(boot, [&boot, deadline]() {
        return !boot.app->Scenarios().Active() || std::chrono::steady_clock::now() >= deadline;
    });

    const auto now = std::chrono::steady_clock::now();
    const auto remainingMs =
        (deadline > now) ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() : 0;
    const bool fileReady = WaitForFile(outPath, static_cast<int>(remainingMs));
    if (fileReady) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (!fileReady) {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = toolName;
        errEnv["error"] = {{"code", "timeout"}, {"message", "Scenario did not finish within expected time."}};
        EmitErrorToStderr(errEnv);
        standalone::Shutdown(boot);
        return 8;
    }

    std::FILE* f = std::fopen(outPath.c_str(), "rb");
    if (!f) {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = toolName;
        errEnv["error"] = {{"code", "handler-error"}, {"message", "Could not read scenario result file: " + outPath}};
        EmitErrorToStderr(errEnv);
        standalone::Shutdown(boot);
        return kExitHandler;
    }
    std::string content;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    std::fclose(f);
    try {
        std::string parseErr;
        nlohmann::json scenarioData = smatchet::json_safe::ParseBounded(content, parseErr);
        if (!parseErr.empty()) {
            nlohmann::json errEnv;
            errEnv["ok"] = false;
            errEnv["command"] = toolName;
            errEnv["error"] = {{"code", "handler-error"},
                               {"message", "Scenario result file is not valid JSON: " + outPath}};
            EmitErrorToStderr(errEnv);
            standalone::Shutdown(boot);
            return kExitHandler;
        }
        envelope["ok"] = true;
        envelope["command"] = toolName;
        envelope["data"] = std::move(scenarioData);
    } catch (...) { // catch-all-ok: malformed scenario file → handler-error envelope below
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = toolName;
        errEnv["error"] = {{"code", "handler-error"},
                           {"message", "Scenario result file is not valid JSON: " + outPath}};
        EmitErrorToStderr(errEnv);
        standalone::Shutdown(boot);
        return kExitHandler;
    }
    return -1; // success — caller proceeds to the shared emit path
}

int RunCmdInProcessImpl(int argc, char** argv) {
    std::set_terminate(&CliTerminateHandler);
    try {
        ParsedArgs pa;
        std::string parseErr;
        if (!ParseArgs(argc, argv, pa, parseErr)) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = "";
            env["error"] = {{"code", "validation-error"}, {"message", parseErr}};
            EmitErrorToStderr(env);
            return kExitValidation;
        }

        (void)pa.spawn;

        if (pa.wantListHelp) {
            PrintCliHelpInProcess(stdout);
            return kExitOk;
        }

        nlohmann::json argsToSend = pa.args;
        std::string toolName = pa.commandName;
        if (pa.wantHelp) {
            toolName = "commands.help";
            argsToSend = nlohmann::json::object();
            argsToSend["name"] = pa.commandName;
        }
        if (pa.yes) {
            argsToSend["__confirm"] = true;
        }
        if (pa.dryRun) {
            argsToSend["__dry_run"] = true;
        }
        if (pa.timeoutMs > 0) {
            argsToSend["__timeout_ms"] = pa.timeoutMs;
        }

        const bool tier1 = IsTier1MetaCommand(toolName) || pa.wantHelp;
        standalone::BootstrapContext boot;
        std::string bootErr;
        if (!standalone::Initialize(
                boot, argc, argv,
                tier1 ? standalone::HeadlessCliMode::MetaCommand : standalone::HeadlessCliMode::ScenarioRun, bootErr)) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = toolName;
            env["error"] = {{"code", "handler-error"}, {"message", "In-process boot failed: " + bootErr}};
            EmitErrorToStderr(env);
            standalone::Shutdown(boot);
            return kExitHandler;
        }

        smatchet::cmd::CommandContext ctx;
        ctx.App = boot.app.get();
        ctx.Source = smatchet::cmd::CommandSource::Cli;
        ctx.ConfirmedDestructive = pa.yes;
        ctx.DryRun = pa.dryRun;
        ctx.TimeoutMs = pa.timeoutMs;

        smatchet::cmd::CommandResult dispatchResult = boot.app->Commands().Dispatch(toolName, argsToSend, ctx);

        nlohmann::json envelope = dispatchResult.ToWireJson(toolName, pa.dryRun);
        const nlohmann::json envData = SafeObject(envelope, "data");
        const bool isAsyncScenario = !tier1 && SafeBool(envelope, "ok", false) && SafeBool(envData, "running", false) &&
                                     envData.contains("outPath") && envData["outPath"].is_string();

        if (isAsyncScenario) {
            const int asyncRc = RunAsyncScenarioInProcess(boot, toolName, argsToSend, pa, envData, envelope);
            if (asyncRc >= 0) {
                return asyncRc;
            }
        }

        if (SafeBool(envelope, "ok", false)) {
            EmitEnvelope(envelope, pa.pretty, pa.quiet);
            standalone::Shutdown(boot);
            return kExitOk;
        }

        EmitErrorToStderr(envelope);
        const std::string code = SafeString(SafeObject(envelope, "error"), "code", "handler-error");
        standalone::Shutdown(boot);
        return ExitCodeForErrorCode(code);
    } catch (const std::exception& e) {
        nlohmann::json env = MakeErrorEnvelope("", "handler-error", std::string("CLI internal error: ") + e.what());
        EmitErrorToStderr(env);
        return kExitHandler;
    } catch (...) {          // catch-all-ok: unknown CLI exception -> error JSON to stderr (pre-logger-init)
        std::fprintf(stderr, // pre-logger-init — LOG_* unavailable
                     "{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"handler-error\","
                     "\"message\":\"CLI internal error: unknown exception.\"}}\n");
        return kExitHandler;
    }
}

#endif // !SMATCHET_WITH_MCP

} // namespace

#if !defined(SMATCHET_WITH_MCP)
int RunCmdInProcess(int argc, char** argv) { return RunCmdInProcessImpl(argc, argv); }
#endif

bool ArgvHasCmdSubcommand(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "cmd") == 0)
            return true;
    }
    return false;
}

bool IsEphemeralMode(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ephemeral") == 0)
            return true;
    }
    return false;
}

#if defined(SMATCHET_WITH_MCP)
// The RunCmdAttach phase helpers below dispatch over MCP/httplib and reference
// MCP-only symbols (PrintCliHelp, SpawnAndRun, ExtractEnvelopeFromMcpResult).
// They are only ever called from RunCmdAttach's MCP-on branch, so they must be
// excluded from the light (MCP-off) build — which otherwise fails to compile.
/// Phase 1 of RunCmdAttach: parse argv and discover the MCP host/port.
/// Priority: explicit flag > env > instance.json (PID-verified) > default.
/// Returns false and emits an error envelope if ParseArgs fails.
bool RunCmdAttachResolveHostPort(int argc, char** argv, ParsedArgs& outPa, std::string& outHost, int& outPort) {
    std::string parseErr;
    if (!ParseArgs(argc, argv, outPa, parseErr)) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = "";
        env["error"] = {{"code", "validation-error"}, {"message", parseErr}};
        EmitErrorToStderr(env);
        return false;
    }

    outHost = outPa.mcpHost.empty() ? EnvOr("SMATCHET_MCP_HOST", "127.0.0.1") : outPa.mcpHost;
    outPort = outPa.mcpPort > 0 ? outPa.mcpPort : EnvIntOr("SMATCHET_MCP_PORT", 0);
    if (outPort == 0) {
        const std::string instPath = ConfigManager::GetUserDataDirectory() + "instance.json";
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // fopen: cross-platform — fopen_s is MSVC-only
#endif
        std::FILE* f = std::fopen(instPath.c_str(), "rb");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (f) {
            std::string json;
            char buf[512];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                json.append(buf, n);
            std::fclose(f);
            nlohmann::json j;
            if (SafeParseJson(json, j) && j.is_object())
                try {
                    const int instPort = SafeInt(j, "port", 0);
                    const long long instPid = static_cast<long long>(SafeInt(j, "pid", 0));
                    // Verify the PID is still alive before trusting this port.
                    bool pidAlive = false;
#if defined(_WIN32)
                    if (instPid > 0) {
                        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(instPid));
                        if (h) {
                            DWORD exitCode = 0;
                            pidAlive = GetExitCodeProcess(h, &exitCode) && (exitCode == STILL_ACTIVE);
                            CloseHandle(h);
                        }
                    }
#else
                    if (instPid > 0) {
                        pidAlive = (::kill(static_cast<pid_t>(instPid), 0) == 0);
                    }
#endif
                    if (pidAlive && instPort > 0 && instPort <= 65535) {
                        outPort = instPort;
                    }
                } catch (...) { // catch-all-ok: best-effort port discovery - any failure leaves outPort at default
                    // Best-effort instance.json port discovery: any failure here
                    // (bad JSON shape, OS probe error) leaves outPort unset so the
                    // caller falls through to the default port. Non-fatal by design.
                }
        }
    }
    if (outPort == 0)
        outPort = SmatchetDefaults::Mcp::kDefaultPort;
    return true;
}

/// Phase 2 of RunCmdAttach: resolve the effective tool name and args, injecting
/// protocol-extension flags (--yes, --dry-run, --timeout) into argsToSend.
/// Returns true when --help or --list-help was handled and the caller should return kExitOk.
bool RunCmdAttachHandleHelp(const ParsedArgs& pa, const std::string& host, int port, std::string& outToolName,
                            nlohmann::json& outArgs) {
    // Top-level --help: print flag summary + fetch live catalog if possible.
    // Discovery is much more useful when the user sees what commands actually exist
    // on first run, so we do this after host/port resolution.
    if (pa.wantListHelp) {
        PrintCliHelp(stdout, host, port);
        return true;
    }

    outArgs = pa.args;
    outToolName = pa.commandName;
    if (pa.wantHelp) {
        // --help for a single command — call commands.help over the wire.
        outToolName = "commands.help";
        outArgs = nlohmann::json::object();
        outArgs["name"] = pa.commandName;
    }

    // Inject protocol extensions for flags.
    if (pa.yes)
        outArgs["__confirm"] = true;
    if (pa.dryRun)
        outArgs["__dry_run"] = true;
    if (pa.timeoutMs > 0)
        outArgs["__timeout_ms"] = pa.timeoutMs;
    return false;
}

/// Phase 3 of RunCmdAttach: POST the command to the MCP endpoint and extract the response envelope.
/// On connection failure, delegates to SpawnAndRun if pa.spawn is set.
/// Returns kExitOk with envelope filled; otherwise emits an error to stderr and returns exit code.
/// `outSpawnHandled` is set true ONLY when the --spawn branch ran: SpawnAndRun is terminal — it
/// emits its own result envelope to stdout and returns the FINAL process exit code. The caller must
/// then return that code directly and must NOT post-process `outEnvelope` (which stays empty on this
/// path); doing so re-maps a clean `ok:true` child into a spurious handler-error exit (kExitHandler/4).
/// See infra `spawn-smoke-teardown-exit4`: the Mesa-GL launch-smoke booted fine, answered
/// app.version ok:true, yet the parent exited 4 because the empty envelope fell through to the
/// RunCmdAttachProcessResult error path.
int RunCmdAttachDispatch(const ParsedArgs& pa, const std::string& host, int port, const std::string& toolName,
                         const nlohmann::json& argsToSend, nlohmann::json& outEnvelope, bool& outSpawnHandled) {
    outSpawnHandled = false;
    const int envSpawnTimeout = EnvIntOr("SMATCHET_SPAWN_TIMEOUT_MS", 0);
    const int readTimeoutSec = (pa.timeoutMs > 0)      ? (pa.timeoutMs / 1000 + 5)
                               : (envSpawnTimeout > 0) ? (envSpawnTimeout / 1000 + 5)
                                                       : 30;
    nlohmann::json body;
    body["name"] = toolName;
    body["arguments"] = argsToSend;

    httplib::Client cli(host, port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(readTimeoutSec, 0);

    auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
    if (!res) {
        // --spawn: launch an ephemeral instance and retry in-process. SpawnAndRun is terminal —
        // it emits the result envelope itself and returns the final exit code, so flag the caller
        // to return that code directly instead of re-processing the (still-empty) outEnvelope.
        if (pa.spawn) {
            outSpawnHandled = true;
            return SpawnAndRun(pa, toolName, argsToSend);
        }
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = toolName;
        env["error"] = {
            {"code", "not-connected"},
            {"message", "Could not reach Smatchet MCP at " + host + ":" + std::to_string(port) + "."},
            {"hint", "Start Smatchet (with MCP enabled), or pass --spawn to launch automatically."},
        };
        EmitErrorToStderr(env);
        return kExitNotConnected;
    }
    if (res->status < 200 || res->status >= 300) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = toolName;
        env["error"] = {
            {"code", "handler-error"},
            {"message", "MCP server returned HTTP " + std::to_string(res->status) + "."},
            {"details", {{"body", res->body}}},
        };
        EmitErrorToStderr(env);
        return kExitTransport;
    }

    nlohmann::json parsed;
    {
        std::string parseErr;
        parsed = smatchet::json_safe::ParseBounded(res->body, parseErr);
        if (!parseErr.empty()) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = toolName;
            env["error"] = {
                {"code", "handler-error"},
                {"message", std::string("Could not parse MCP response: ") + parseErr},
            };
            EmitErrorToStderr(env);
            return kExitTransport;
        }
    }

    try {
        outEnvelope = ExtractEnvelopeFromMcpResult(parsed);
    } catch (...) { // catch-all-ok: extract failure -> transport error envelope
        outEnvelope = MakeErrorEnvelope(toolName, "transport", "Failed to extract envelope from MCP response.");
    }
    if (!outEnvelope.is_object())
        outEnvelope = nlohmann::json::object();
    if (!outEnvelope.contains("ok")) {
        nlohmann::json wrap;
        wrap["ok"] = true;
        wrap["command"] = toolName;
        wrap["data"] = std::move(outEnvelope);
        outEnvelope = std::move(wrap);
    }
    return kExitOk;
}

/// Phase 4 of RunCmdAttach: emit the response envelope (or token estimate) and return exit code.
int RunCmdAttachProcessResult(const ParsedArgs& pa, const nlohmann::json& envelope, bool doTokenEstimate) {
    // --tokens: estimate size from serialized data, print to stderr, produce no stdout.
    if (doTokenEstimate && SafeBool(envelope, "ok", false)) {
        std::string dataStr;
        try {
            dataStr = envelope.contains("data") ? envelope["data"].dump() : std::string("{}");
        } catch (...) { // catch-all-ok: serialize failure -> empty-object byte estimate
            dataStr = "{}";
        }
        const long long bytes = static_cast<long long>(dataStr.size());
        const long long tokens = (bytes + 3) / 4; // rough ASCII heuristic (±30%)
        nlohmann::json estimate;
        estimate["tokens_estimate"] = tokens;
        estimate["bytes"] = bytes;
        std::fprintf(stderr, "%s\n", estimate.dump().c_str()); // CLI stdout — product output, not logging
        return kExitOk;
    }

    if (SafeBool(envelope, "ok", false)) {
        EmitEnvelope(envelope, pa.pretty, pa.quiet);
        return kExitOk;
    }

    // Error path — envelope goes to stderr; exit code from error.code.
    EmitErrorToStderr(envelope);
    const nlohmann::json errObj = SafeObject(envelope, "error");
    const std::string code = SafeString(errObj, "code", "handler-error");
    return ExitCodeForErrorCode(code);
}
#endif // SMATCHET_WITH_MCP — RunCmdAttach phase helpers

int RunCmdAttach(int argc, char** argv) {
#if !defined(SMATCHET_WITH_MCP)
    return RunCmdInProcess(argc, argv);
#else
    std::set_terminate(&CliTerminateHandler);

    try {
        // Phase 1: parse args and discover host/port.
        ParsedArgs pa;
        std::string host;
        int port = 0;
        if (!RunCmdAttachResolveHostPort(argc, argv, pa, host, port))
            return kExitValidation;

        // Phase 2: resolve tool name/args and handle --help early-exit.
        std::string toolName;
        nlohmann::json argsToSend;
        if (RunCmdAttachHandleHelp(pa, host, port, toolName, argsToSend))
            return kExitOk;

        const bool doTokenEstimate = pa.tokens && !pa.wantHelp;

        // Phase 3: POST command and extract envelope.
        nlohmann::json envelope;
        bool spawnHandled = false;
        const int dispatchResult = RunCmdAttachDispatch(pa, host, port, toolName, argsToSend, envelope, spawnHandled);
        // --spawn is terminal: SpawnAndRun already emitted the result envelope and computed the
        // final exit code. Return it directly — `envelope` is empty on this path, so feeding it to
        // RunCmdAttachProcessResult would mis-map a clean ok:true child to kExitHandler (exit 4).
        if (spawnHandled)
            return dispatchResult;
        if (dispatchResult != kExitOk)
            return dispatchResult;

        // Phase 4: emit result or token estimate.
        return RunCmdAttachProcessResult(pa, envelope, doTokenEstimate);

    } catch (const std::exception& e) {
        // Catch anything that escaped a lower-level handler. Emit a clean envelope and exit.
        nlohmann::json env =
            MakeErrorEnvelope("", "handler-error", std::string("CLI internal error: ") + e.what(),
                              "This is a bug — bad input should produce a structured error, not throw.");
        try {
            EmitErrorToStderr(env);
        } catch (...) { // catch-all-ok: already handling a CLI exception; preserve handler exit code.
        }
        return kExitHandler;
    } catch (...) {          // catch-all-ok: unknown CLI exception -> error JSON to stderr (pre-logger-init)
        std::fprintf(stderr, // pre-logger-init — LOG_* unavailable
                     "{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"handler-error\","
                     "\"message\":\"CLI internal error: unknown exception.\"}}\n");
        return kExitHandler;
    }
#endif // SMATCHET_WITH_MCP
}

} // namespace cli
} // namespace smatchet
