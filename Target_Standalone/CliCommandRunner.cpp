#include "CliCommandRunner.h"

#include "CliArgCoercion.h"
#include "StandaloneAppBootstrap.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "AppController.h"
#include "Commands/Scenarios/IScenario.h"
#include "ConfigManager.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
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
// 8 (timeout) + 9 (dry-run-unsupported) are reachable once spawn/dry-run land.

// ============================================================================
// Safe JSON value extractors — never throw on type mismatch or missing keys.
// ParseArgs stores every --key=value as a string, and external responses can
// have any shape; defensive reads avoid type_error.302 from nlohmann.
// ============================================================================

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
    } catch (...) {
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
    } catch (...) {
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
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    } catch (...) {
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
    } catch (...) {
        return nlohmann::json::object();
    }
}

/// Try to parse a JSON document; returns true on success.
bool SafeParseJson(const std::string& text, nlohmann::json& out) {
    try {
        out = nlohmann::json::parse(text);
        return true;
    } catch (...) {
        out = nlohmann::json::object();
        return false;
    }
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
// in tests/Source_Core/CliArgCoercion.test.cpp. ParseArgs below calls it for
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
    } catch (...) {
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
            continue;
        }
        if (a.rfind("--mcp-port=", 0) == 0) {
            try {
                out.mcpPort = std::stoi(a.substr(11));
            } catch (...) {
                outError = "invalid --mcp-port value";
                return false;
            }
            continue;
        }
#endif
        if (a.rfind("--timeout=", 0) == 0) {
            try {
                out.timeoutMs = std::stoi(a.substr(10));
            } catch (...) {
                outError = "invalid --timeout value (expected integer ms)";
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
            std::fprintf(stdout, "%s\n", pretty ? envelope.dump(2).c_str() : envelope.dump().c_str()); // CLI stdout — product output, not logging
        } catch (...) {
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
                std::fprintf(stdout, "%s\n", it["id"].get<std::string>().c_str()); // CLI stdout — product output, not logging
            } else if (it.is_object() && it.contains("name") && it["name"].is_string()) {
                std::fprintf(stdout, "%s\n", it["name"].get<std::string>().c_str()); // CLI stdout — product output, not logging
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

void EmitErrorToStderr(const nlohmann::json& envelope) { std::fprintf(stderr, "%s\n", envelope.dump().c_str()); } // CLI stdout — product output, not logging

#if defined(SMATCHET_WITH_MCP)
bool LaunchEphemeralInstance(const std::string& exePath, int port) {
    if (exePath.empty())
        return false;
    const std::string portStr = std::to_string(port);
    // main.cpp parses `--mcp-port <port>` as two separate argv entries (space-separated),
    // NOT `--mcp-port=<port>` — using the equals form would silently fall through.
#if defined(_WIN32)
    // CommandLineToArgvW handles quoted whitespace; pass space-separated tokens.
    std::string cmdLine = "\"" + exePath + "\" --ephemeral --mcp-port " + portStr;
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
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
        const char* args[] = {exePath.c_str(), "--ephemeral", "--mcp-port", portStr.c_str(), nullptr};
        execv(exePath.c_str(), const_cast<char* const*>(args));
        _exit(1);
    }
    return true;
#endif
}

/// Poll until the MCP endpoint at host:port becomes reachable or timeoutMs elapses.
bool WaitForMcpReady(const std::string& host, int port, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(0, 200000); // 200 ms
        cli.set_read_timeout(1, 0);
        nlohmann::json body;
        body["name"] = "app.version";
        body["arguments"] = nlohmann::json::object();
        auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
        if (res && res->status >= 200 && res->status < 300)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
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
        const auto parsed = nlohmann::json::parse(res->body);
        if (!parsed.contains("content") || !parsed["content"].is_array() || parsed["content"].empty() ||
            !parsed["content"][0].contains("text")) {
            return false;
        }
        const auto envelope = nlohmann::json::parse(parsed["content"][0]["text"].get<std::string>());
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
            std::fprintf(out, "    %-32s %s%s\n", name.c_str(), destructive ? "(destructive) " : "", summary.c_str()); // CLI stdout — product output, not logging
        }
        std::fprintf(out, // CLI stdout — product output, not logging
                     "\nFor full schema:    Smatchet.exe cmd commands.help --name=<name>\n"
                     "All commands + schema: Smatchet.exe cmd commands.list --full --pretty\n");
        return true;
    } catch (...) {
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
    } catch (...) {
        // Fall through — return result as-is below.
    }
    return *result;
}

/// Normalize a relative outPath argument to an absolute path resolved against the CLI's CWD,
/// since the spawned instance may have a different working directory. Pass-through for absolute paths.
nlohmann::json NormalizeOutPath(const nlohmann::json& argsToSend) {
    nlohmann::json out = argsToSend;
    if (!out.contains("outPath") || !out["outPath"].is_string())
        return out;
    const std::string p = out["outPath"].get<std::string>();
    if (p.empty())
        return out;
    fs::path path(p);
    if (path.is_absolute())
        return out;
    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    if (!ec) {
        out["outPath"] = abs.string();
    }
    return out;
}

/// Full spawn-attach-run flow invoked when --spawn is set and no instance is reachable.
/// Launches a hidden ephemeral app instance, sends the command, waits for results, quits the app.
int SpawnAndRun(const ParsedArgs& pa, const std::string& commandName, const nlohmann::json& argsToSendRaw) {
    try {
        // Normalize outPath: relative paths must be made absolute so both processes agree on location.
        const nlohmann::json argsToSend = NormalizeOutPath(argsToSendRaw);
        const std::string exePath = GetExePath();
        if (exePath.empty()) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = commandName;
            env["error"] = {{"code", "not-connected"},
                            {"message", "--spawn: could not determine exe path to relaunch."}};
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

        std::fprintf(stderr, "[spawn] launching ephemeral instance on port %d ...\n", port); // CLI stdout — product output, not logging
        if (!LaunchEphemeralInstance(exePath, port)) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = commandName;
            env["error"] = {{"code", "not-connected"}, {"message", "--spawn: failed to launch subprocess."}};
            EmitErrorToStderr(env);
            return kExitNotConnected;
        }

        const std::string host = "127.0.0.1";
        // Bumped 15s → 30s post Phase D/E AI merges (OpenAi + Anthropic + Ollama clients
        // + AiNdjsonParser + Lua glue pushed startup over the prior 15s ready window;
        // bucket-E gates regressed on develop tip). Env override SMATCHET_SPAWN_READY_MS
        // allows tightening on faster runners or raising further on cold-cache CI.
        // Lazy-loading AI clients is the architectural follow-up — entry remains open
        // in docs/backlog/agent-self-improvement/tooling.md.
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
            } catch (...) {
                // Ignore malformed override; fall through to default.
            }
        }
        std::fprintf(stderr, "[spawn] waiting for MCP ready (up to %d s) ...\n", readyTimeoutMs / 1000); // CLI stdout — product output, not logging
        if (!WaitForMcpReady(host, port, readyTimeoutMs)) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = commandName;
            env["error"] = {{"code", "timeout"},
                            {"message", "--spawn: ephemeral instance did not become reachable in time."}};
            EmitErrorToStderr(env);
            return kExitNotConnected;
        }

        // Send the actual command.
        nlohmann::json body;
        body["name"] = commandName;
        body["arguments"] = argsToSend;

        // For scenario.run, compute a wait timeout from the frames param.
        // ParseArgs stores --key=value pairs as strings; coerce defensively.
        int frames = 600;
        if (argsToSend.contains("frames")) {
            const auto& v = argsToSend["frames"];
            if (v.is_number()) {
                frames = v.get<int>();
            } else if (v.is_string()) {
                try {
                    frames = std::stoi(v.get<std::string>());
                } catch (...) {
                }
            }
        }
        const int scenarioWaitMs = (frames / 60 + 30) * 1000;

        httplib::Client cli(host, port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(30, 0);
        auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
        if (!res || res->status < 200 || res->status >= 300) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = commandName;
            env["error"] = {{"code", "transport"}, {"message", "--spawn: command dispatch failed after launch."}};
            EmitErrorToStderr(env);
            return kExitTransport;
        }

        nlohmann::json parsedBody;
        if (!SafeParseJson(res->body, parsedBody)) {
            nlohmann::json env = MakeErrorEnvelope(commandName, "transport",
                                                   "--spawn: server returned non-JSON response (status " +
                                                       std::to_string(res->status) + ").");
            EmitErrorToStderr(env);
            // Best-effort quit.
            nlohmann::json qb;
            qb["name"] = "app.quit";
            qb["arguments"] = {{"__confirm", true}};
            cli.Post("/mcp/tools/call", qb.dump(), "application/json");
            return kExitTransport;
        }

        nlohmann::json envelope;
        try {
            envelope = ExtractEnvelopeFromMcpResult(parsedBody);
        } catch (...) {
            envelope =
                MakeErrorEnvelope(commandName, "transport", "--spawn: failed to extract envelope from response.");
        }

        nlohmann::json envData = SafeObject(envelope, "data");
        const bool isAsync = SafeBool(envelope, "ok", false) && SafeBool(envData, "running", false) &&
                             envData.contains("outPath") && envData["outPath"].is_string();

        // If scenario.run returned {running:true, outPath:...}, wait for the file then emit it.
        if (isAsync) {
            const std::string outPath = SafeString(envData, "outPath");
            std::fprintf(stderr, "[spawn] scenario running (%d frames / ~%d s) ...\n", frames, frames / 60); // CLI stdout — product output, not logging
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
                nlohmann::json quitBody;
                quitBody["name"] = "app.quit";
                quitBody["arguments"] = {{"__confirm", true}}; // app.quit is Destructive
                cli.Post("/mcp/tools/call", quitBody.dump(), "application/json");
                return 8;
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
                errEnv["error"] = {{"code", "handler-error"},
                                   {"message", "--spawn: could not read result file: " + outPath}};
                EmitErrorToStderr(errEnv);
            } else {
                std::string content;
                char buf[4096];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    content.append(buf, n);
                std::fclose(f);
                try {
                    const nlohmann::json fileData = nlohmann::json::parse(content);
                    const bool captureRequested = SafeBool(fileData, "captureRequested", false);
                    const std::string screenshotPath = SafeString(fileData, "screenshotPath");
                    if (captureRequested && !screenshotPath.empty()) {
                        std::fprintf(stderr, "[spawn] waiting for screenshot capture ...\n"); // CLI stdout — product output, not logging
                        WaitForFile(screenshotPath, 2000);
                    }

                    nlohmann::json resultEnv;
                    resultEnv["ok"] = true;
                    resultEnv["command"] = commandName;
                    resultEnv["data"] = fileData;
                    EmitEnvelope(resultEnv, pa.pretty, pa.quiet);
                } catch (...) {
                    nlohmann::json errEnv;
                    errEnv["ok"] = false;
                    errEnv["command"] = commandName;
                    errEnv["error"] = {{"code", "handler-error"},
                                       {"message", "--spawn: result file is not valid JSON: " + outPath}};
                    EmitErrorToStderr(errEnv);
                }
            }
        } else {
            // Non-async command: emit envelope directly.
            EmitEnvelope(envelope, pa.pretty, pa.quiet);
            if (!SafeBool(envelope, "ok", false)) {
                const nlohmann::json errObj = SafeObject(envelope, "error");
                const std::string code = SafeString(errObj, "code");
                nlohmann::json quitBody;
                quitBody["name"] = "app.quit";
                quitBody["arguments"] = {{"__confirm", true}}; // app.quit is Destructive
                cli.Post("/mcp/tools/call", quitBody.dump(), "application/json");
                return ExitCodeForErrorCode(code);
            }
        }

        // Quit the spawned instance (app.quit is Destructive → __confirm:true).
        std::fprintf(stderr, "[spawn] sending app.quit ...\n"); // CLI stdout — product output, not logging
        nlohmann::json quitBody;
        quitBody["name"] = "app.quit";
        quitBody["arguments"] = {{"__confirm", true}};
        cli.Post("/mcp/tools/call", quitBody.dump(), "application/json");
        return kExitOk;
    } catch (const std::exception& e) {
        nlohmann::json env =
            MakeErrorEnvelope(commandName, "handler-error", std::string("--spawn: internal error: ") + e.what());
        try {
            EmitErrorToStderr(env);
        } catch (...) {
        }
        return kExitHandler;
    } catch (...) {
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
    std::fprintf(out, "Smatchet CLI — in-process Command System (light build).\n" // CLI stdout — product output, not logging
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
            const std::string outPath = SafeString(envData, "outPath");
            int frames = 600;
            if (argsToSend.contains("frames")) {
                const auto& v = argsToSend["frames"];
                if (v.is_number()) {
                    frames = v.get<int>();
                } else if (v.is_string()) {
                    try {
                        frames = std::stoi(v.get<std::string>());
                    } catch (...) {
                    }
                }
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
                errEnv["error"] = {{"code", "handler-error"},
                                   {"message", "Could not read scenario result file: " + outPath}};
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
                envelope["ok"] = true;
                envelope["command"] = toolName;
                envelope["data"] = nlohmann::json::parse(content);
            } catch (...) {
                nlohmann::json errEnv;
                errEnv["ok"] = false;
                errEnv["command"] = toolName;
                errEnv["error"] = {{"code", "handler-error"},
                                   {"message", "Scenario result file is not valid JSON: " + outPath}};
                EmitErrorToStderr(errEnv);
                standalone::Shutdown(boot);
                return kExitHandler;
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
    } catch (...) {
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

int RunCmdAttach(int argc, char** argv) {
#if !defined(SMATCHET_WITH_MCP)
    return RunCmdInProcess(argc, argv);
#else
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

        // Host/port discovery: explicit flag > env > instance.json (PID-verified) > default.
        std::string host = pa.mcpHost.empty() ? EnvOr("SMATCHET_MCP_HOST", "127.0.0.1") : pa.mcpHost;
        int port = pa.mcpPort > 0 ? pa.mcpPort : EnvIntOr("SMATCHET_MCP_PORT", 0);
        if (port == 0) {
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
                            HANDLE h =
                                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(instPid));
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
                            port = instPort;
                        }
                    } catch (...) {
                    }
            }
        }
        if (port == 0)
            port = SmatchetDefaults::Mcp::kDefaultPort;

        // Top-level --help: print flag summary + fetch live catalog if possible.
        // Discovery is much more useful when the user sees what commands actually exist
        // on first run, so we do this after host/port resolution.
        if (pa.wantListHelp) {
            PrintCliHelp(stdout, host, port);
            return kExitOk;
        }

        // --help for a single command — call commands.help over the wire.
        nlohmann::json argsToSend = pa.args;
        std::string toolName = pa.commandName;
        if (pa.wantHelp) {
            toolName = "commands.help";
            argsToSend = nlohmann::json::object();
            argsToSend["name"] = pa.commandName;
        }

        // Inject protocol extensions for flags.
        if (pa.yes)
            argsToSend["__confirm"] = true;
        if (pa.dryRun)
            argsToSend["__dry_run"] = true;
        if (pa.timeoutMs > 0)
            argsToSend["__timeout_ms"] = pa.timeoutMs;

        // --tokens: run the command, measure JSON output size, print estimate to stderr, no stdout.
        const bool doTokenEstimate = pa.tokens && !pa.wantHelp;

        nlohmann::json body;
        body["name"] = toolName;
        body["arguments"] = argsToSend;

        // Timeout: default from env for async commands.
        const int envSpawnTimeout = EnvIntOr("SMATCHET_SPAWN_TIMEOUT_MS", 0);
        const int readTimeoutSec = (pa.timeoutMs > 0)      ? (pa.timeoutMs / 1000 + 5)
                                   : (envSpawnTimeout > 0) ? (envSpawnTimeout / 1000 + 5)
                                                           : 30;

        httplib::Client cli(host, port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(readTimeoutSec, 0);

        auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
        if (!res) {
            // --spawn: launch an ephemeral instance and retry in-process.
            if (pa.spawn) {
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
        try {
            parsed = nlohmann::json::parse(res->body);
        } catch (const std::exception& e) {
            nlohmann::json env;
            env["ok"] = false;
            env["command"] = toolName;
            env["error"] = {
                {"code", "handler-error"},
                {"message", std::string("Could not parse MCP response: ") + e.what()},
            };
            EmitErrorToStderr(env);
            return kExitTransport;
        }

        nlohmann::json envelope;
        try {
            envelope = ExtractEnvelopeFromMcpResult(parsed);
        } catch (...) {
            envelope = MakeErrorEnvelope(toolName, "transport", "Failed to extract envelope from MCP response.");
        }
        if (!envelope.is_object())
            envelope = nlohmann::json::object();
        if (!envelope.contains("ok")) {
            nlohmann::json wrap;
            wrap["ok"] = true;
            wrap["command"] = toolName;
            wrap["data"] = std::move(envelope);
            envelope = std::move(wrap);
        }

        // --tokens: estimate size from serialized data, print to stderr, produce no stdout.
        if (doTokenEstimate && SafeBool(envelope, "ok", false)) {
            std::string dataStr;
            try {
                dataStr = envelope.contains("data") ? envelope["data"].dump() : std::string("{}");
            } catch (...) {
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

    } catch (const std::exception& e) {
        // Catch anything that escaped a lower-level handler. Emit a clean envelope and exit.
        nlohmann::json env =
            MakeErrorEnvelope("", "handler-error", std::string("CLI internal error: ") + e.what(),
                              "This is a bug — bad input should produce a structured error, not throw.");
        try {
            EmitErrorToStderr(env);
        } catch (...) {
        }
        return kExitHandler;
    } catch (...) {
        std::fprintf(stderr, // pre-logger-init — LOG_* unavailable
                     "{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"handler-error\","
                     "\"message\":\"CLI internal error: unknown exception.\"}}\n");
        return kExitHandler;
    }
#endif // SMATCHET_WITH_MCP
}

} // namespace cli
} // namespace smatchet
