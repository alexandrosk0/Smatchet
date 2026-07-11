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

#if defined(SMATCHET_WITH_MCP)

namespace {

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

} // namespace

namespace detail {

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
    // Present the configured MCP token on the direct attach path (finding DR12b), mirroring how
    // the --spawn path sets X-Smatchet-Token. Without it, a server guarding loopback with an
    // operator token 401s this request (res is set, so the !res --spawn fallback never engages).
    {
        std::string hdrName;
        std::string hdrValue;
        if (McpAttachAuthHeader(ConfigManager::Load().McpAuthToken, hdrName, hdrValue)) {
            cli.set_default_headers({{hdrName, hdrValue}});
        }
    }

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

} // namespace detail

#endif // SMATCHET_WITH_MCP

} // namespace cli
} // namespace smatchet
