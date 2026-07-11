#include "CliCommandRunner.h"
#include "CliCommandRunner_Internal.h"

#include "CliArgCoercion.h"
#include "StandaloneAppBootstrap.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SpawnOutLogBasename.h"
#include "ConfigManager.h"
#include "Json/BoundedJsonParse.h"
#include "SmatchetDefaults.h"

#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared CliCommandRunner-TU include block + cli::detail using-prologue is grandfathered across the god-file-split siblings (CliCommandRunner.cpp / CliArgs / CliSpawn / CliDispatch / CliHelpAndAttach) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared CliCommandRunner TU prologue header is introduced)
// clang-format on

#if defined(SMATCHET_WITH_MCP)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <bcrypt.h> // BCryptGenRandom — spawn-token CSPRNG (backlog 2026-06-28)
#pragma comment(lib, "bcrypt")
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
#if defined(__linux__)
#include <cerrno>
#include <sys/random.h> // getrandom — spawn-token CSPRNG (backlog 2026-06-28)
#endif
#endif

namespace fs = ghc::filesystem;

namespace smatchet {
namespace cli {

// Bring the promoted `cli::detail` helpers (declared in CliCommandRunner_Internal.h)
// into scope so the moved bodies below call them exactly as before, unqualified.
using namespace detail;

namespace detail {

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

std::string SafeString(const nlohmann::json& j, const char* key, const std::string& fallback) {
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
                                 const std::string& hint) {
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

// CoerceCliArgValue lives in CliArgCoercion.h / .cpp — pure helper unit-tested
// in tests/Core/CliArgCoercion.test.cpp. ParseArgs below calls it for
// every `--key=value` token so handlers receive typed JSON instead of strings.

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

} // namespace detail

} // namespace cli
} // namespace smatchet
