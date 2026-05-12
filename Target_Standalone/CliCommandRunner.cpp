#include "CliCommandRunner.h"

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
#include <nlohmann/json.hpp>

#include "ConfigManager.h"
#include "SmatchetDefaults.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace smatchet {
namespace cli {

namespace {

// Exit code map (kept in sync with the plan / CliCommandRunner.h header).
constexpr int kExitOk = 0;
constexpr int kExitUnknownCommand = 2;
constexpr int kExitValidation = 3;
constexpr int kExitHandler = 4;
constexpr int kExitConfirmRequired = 5;
constexpr int kExitNotConnected = 6;
constexpr int kExitTransport = 7;
// 8 (timeout) + 9 (dry-run-unsupported) are reachable once spawn/dry-run land.

int ExitCodeForErrorCode(const std::string& code) {
    if (code == "ok") return kExitOk;
    if (code == "unknown-command") return kExitUnknownCommand;
    if (code == "missing-required-arg") return kExitValidation;
    if (code == "validation-error") return kExitValidation;
    if (code == "handler-error") return kExitHandler;
    if (code == "backend-error") return kExitHandler;
    if (code == "not-found") return kExitHandler;
    if (code == "confirm-required") return kExitConfirmRequired;
    if (code == "not-connected") return kExitNotConnected;
    if (code == "dry-run-unsupported") return 9;
    if (code == "timeout") return 8;
    return kExitHandler;
}

struct ParsedArgs {
    std::string commandName;     ///< first positional after `cmd`
    bool wantHelp = false;       ///< --help / -h after a command name
    bool wantListHelp = false;   ///< `cmd --help` with no command name
    bool pretty = false;
    bool quiet = false;
    bool yes = false;
    bool dryRun = false;         ///< --dry-run → inject __dry_run:true
    bool tokens = false;         ///< --tokens → estimate output size, print to stderr, no stdout
    int  timeoutMs = 0;          ///< --timeout=<ms> → passed as __timeout_ms; 0=no cap
    std::string mcpHost;         ///< empty -> default
    int mcpPort = 0;             ///< 0 -> resolve from env / default
    nlohmann::json args = nlohmann::json::object();  ///< collected --key=value arguments
};

std::string EnvOr(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}

int EnvIntOr(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::stoi(std::string(v));
    } catch (...) {
        return fallback;
    }
}

void PrintCliHelp(std::FILE* out) {
    std::fprintf(out,
        "Smatchet CLI — unified Command System front-end.\n"
        "\n"
        "Usage:\n"
        "  SmatchetStandalone.exe cmd <name> [--key=value ...] [flags]\n"
        "  SmatchetStandalone.exe cmd <name> --help        Show command schema + examples.\n"
        "  SmatchetStandalone.exe cmd commands.list        List all available commands.\n"
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
        "  - Stdout is always JSON (or bare scalars under --quiet). Logs go to stderr.\n"
        "  - Discovery: cmd commands.list  /  cmd commands.help --name=<name>\n",
        SmatchetDefaults::Mcp::kDefaultPort);
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
        if (a == "--help" || a == "-h")       { out.wantHelp = true; continue; }
        if (a == "--pretty")                    { out.pretty   = true; continue; }
        if (a == "--quiet" || a == "-q")        { out.quiet    = true; continue; }
        if (a == "--yes")                       { out.yes      = true; continue; }
        if (a == "--dry-run")                   { out.dryRun   = true; continue; }
        if (a == "--tokens")                    { out.tokens   = true; continue; }
        if (a.rfind("--mcp-host=", 0) == 0) { out.mcpHost = a.substr(11); continue; }
        if (a.rfind("--mcp-port=", 0) == 0) {
            try { out.mcpPort = std::stoi(a.substr(11)); }
            catch (...) { outError = "invalid --mcp-port value"; return false; }
            continue;
        }
        if (a.rfind("--timeout=", 0) == 0) {
            try { out.timeoutMs = std::stoi(a.substr(10)); }
            catch (...) { outError = "invalid --timeout value (expected integer ms)"; return false; }
            continue;
        }
        // --key=value or --flag
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            const std::string body = a.substr(2);
            const auto eq = body.find('=');
            if (eq == std::string::npos) {
                out.args[body] = true;
            } else {
                out.args[body.substr(0, eq)] = body.substr(eq + 1);
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
        std::fprintf(stdout, "%s\n", pretty ? envelope.dump(2).c_str() : envelope.dump().c_str());
        return;
    }
    // Quiet: extract a primary value for shell pipelines. Heuristic:
    //   - if data.items[] is a list of strings: emit one-per-line
    //   - else if data.items[] is a list of objects with `id`: emit id one-per-line
    //   - else if data is a scalar: emit it bare
    //   - else fall back to compact JSON (still parseable)
    if (!envelope.value("ok", false)) {
        // Errors still go through stderr below; nothing to extract.
        return;
    }
    const nlohmann::json& data = envelope.value("data", nlohmann::json::object());
    if (data.contains("items") && data["items"].is_array()) {
        for (const auto& it : data["items"]) {
            if (it.is_string()) {
                std::fprintf(stdout, "%s\n", it.get<std::string>().c_str());
            } else if (it.is_object() && it.contains("id") && it["id"].is_string()) {
                std::fprintf(stdout, "%s\n", it["id"].get<std::string>().c_str());
            } else if (it.is_object() && it.contains("name") && it["name"].is_string()) {
                std::fprintf(stdout, "%s\n", it["name"].get<std::string>().c_str());
            } else {
                std::fprintf(stdout, "%s\n", it.dump().c_str());
            }
        }
        return;
    }
    if (data.is_string()) {
        std::fprintf(stdout, "%s\n", data.get<std::string>().c_str());
        return;
    }
    if (data.is_number() || data.is_boolean() || data.is_null()) {
        std::fprintf(stdout, "%s\n", data.dump().c_str());
        return;
    }
    // Fallback — compact one-line JSON for `--quiet` so the contract (stdout is parseable) holds.
    std::fprintf(stdout, "%s\n", data.dump().c_str());
}

void EmitErrorToStderr(const nlohmann::json& envelope) {
    std::fprintf(stderr, "%s\n", envelope.dump().c_str());
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
    if (result->contains("content") && (*result)["content"].is_array() &&
        !(*result)["content"].empty() && (*result)["content"][0].contains("text")) {
        const std::string text = (*result)["content"][0]["text"].get<std::string>();
        try {
            return nlohmann::json::parse(text);
        } catch (...) {
            nlohmann::json env;
            env["ok"] = true;
            env["data"] = text;
            return env;
        }
    }
    return *result;
}

}  // namespace

bool ArgvHasCmdSubcommand(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "cmd") == 0) return true;
    }
    return false;
}

int RunCmdAttach(int argc, char** argv) {
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

    if (pa.wantListHelp) {
        PrintCliHelp(stdout);
        return kExitOk;
    }

    // Host/port discovery: explicit flag > env > instance.json (PID-verified) > default.
    std::string host = pa.mcpHost.empty() ? EnvOr("SMATCHET_MCP_HOST", "127.0.0.1") : pa.mcpHost;
    int port = pa.mcpPort > 0 ? pa.mcpPort : EnvIntOr("SMATCHET_MCP_PORT", 0);
    if (port == 0) {
        const std::string instPath = ConfigManager::GetUserDataDirectory() + "instance.json";
        std::FILE* f = std::fopen(instPath.c_str(), "rb");
        if (f) {
            std::string json;
            char buf[512];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
            std::fclose(f);
            try {
                const auto j = nlohmann::json::parse(json);
                const int instPort = j.value("port", 0);
                const long long instPid = j.value("pid", 0LL);
                // Verify the PID is still alive before trusting this port.
                bool pidAlive = false;
#if defined(_WIN32)
                if (instPid > 0) {
                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                           static_cast<DWORD>(instPid));
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
            } catch (...) {}
        }
    }
    if (port == 0) port = SmatchetDefaults::Mcp::kDefaultPort;

    // --help for a single command — call commands.help over the wire.
    nlohmann::json argsToSend = pa.args;
    std::string toolName = pa.commandName;
    if (pa.wantHelp) {
        toolName = "commands.help";
        argsToSend = nlohmann::json::object();
        argsToSend["name"] = pa.commandName;
    }

    // Inject protocol extensions for flags.
    if (pa.yes)     argsToSend["__confirm"]    = true;
    if (pa.dryRun)  argsToSend["__dry_run"]    = true;
    if (pa.timeoutMs > 0) argsToSend["__timeout_ms"] = pa.timeoutMs;

    // --tokens: run the command, measure JSON output size, print estimate to stderr, no stdout.
    const bool doTokenEstimate = pa.tokens && !pa.wantHelp;

    nlohmann::json body;
    body["name"] = toolName;
    body["arguments"] = argsToSend;

    // Timeout: default from env for async commands.
    const int envSpawnTimeout = EnvIntOr("SMATCHET_SPAWN_TIMEOUT_MS", 0);
    const int readTimeoutSec = (pa.timeoutMs > 0) ? (pa.timeoutMs / 1000 + 5)
                             : (envSpawnTimeout > 0) ? (envSpawnTimeout / 1000 + 5) : 30;

    httplib::Client cli(host, port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(readTimeoutSec, 0);

    auto res = cli.Post("/mcp/tools/call", body.dump(), "application/json");
    if (!res) {
        nlohmann::json env;
        env["ok"] = false;
        env["command"] = toolName;
        env["error"] = {
            {"code", "not-connected"},
            {"message", "Could not reach Smatchet MCP at " + host + ":" + std::to_string(port) + "."},
            {"hint", "Start Smatchet (with MCP enabled) or pass --mcp-host / --mcp-port. "
                     "See backlog/COMMAND_SYSTEM_PLAN.md."},
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

    nlohmann::json envelope = ExtractEnvelopeFromMcpResult(parsed);
    if (!envelope.contains("ok")) {
        nlohmann::json wrap;
        wrap["ok"] = true;
        wrap["command"] = toolName;
        wrap["data"] = std::move(envelope);
        envelope = std::move(wrap);
    }

    // --tokens: estimate size from serialized data, print to stderr, produce no stdout.
    if (doTokenEstimate && envelope.value("ok", false)) {
        const std::string dataStr = envelope.contains("data")
                                        ? envelope["data"].dump()
                                        : std::string("{}");
        const long long bytes  = static_cast<long long>(dataStr.size());
        const long long tokens = (bytes + 3) / 4;  // rough ASCII heuristic (±30%)
        nlohmann::json estimate;
        estimate["tokens_estimate"] = tokens;
        estimate["bytes"] = bytes;
        std::fprintf(stderr, "%s\n", estimate.dump().c_str());
        return kExitOk;
    }

    if (envelope.value("ok", false)) {
        EmitEnvelope(envelope, pa.pretty, pa.quiet);
        return kExitOk;
    }

    // Error path — envelope goes to stderr; exit code from error.code.
    EmitErrorToStderr(envelope);
    const std::string code = envelope.contains("error") && envelope["error"].contains("code")
                                 ? envelope["error"]["code"].get<std::string>()
                                 : std::string("handler-error");
    return ExitCodeForErrorCode(code);
}

}  // namespace cli
}  // namespace smatchet
