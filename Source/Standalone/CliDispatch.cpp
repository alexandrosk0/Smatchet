#include "CliCommandRunner.h"
#include "CliCommandRunner_Internal.h"

#include "CliArgCoercion.h"
#include "CliResultFileRead.h"
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
/// userScreenshotPath: C1 parent-fulfill. When non-empty, the trusted --spawn parent
/// asked for a screenshot at this (often absolute) path but sent the child a confine-safe
/// basename instead (the child rejects absolute / '..' paths). After the child reports its
/// confined capture path, copy it back to the user's requested location and rewrite the
/// emitted screenshotPath so the envelope still names the path the caller asked for.
int SpawnAndRunHandleAsync(const ParsedArgs& pa, httplib::Client& cli, const std::string& commandName,
                           const std::string& outPath, int frames, int scenarioWaitMs,
                           const std::string& requestedOutLog, const std::string& userScreenshotPath) {
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

    // Read the result file and build an envelope around it. Bounded read via the shared leaf
    // (command-input-hardening Phase 1.3): a corrupt/oversized result file must not balloon parent
    // memory before ParseBounded's 4 MiB cap rejects it.
    std::string content;
    switch (smatchet::cli::ReadResultFileBounded(outPath, 4u * 1024u * 1024u, content)) {
    case smatchet::cli::ResultFileReadStatus::OpenFailed: {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = commandName;
        errEnv["error"] = {{"code", "handler-error"}, {"message", "--spawn: could not read result file: " + outPath}};
        EmitErrorToStderr(errEnv);
        PostAppQuitBestEffort(cli);
        return kExitHandler;
    }
    case smatchet::cli::ResultFileReadStatus::TooLarge: {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = commandName;
        errEnv["error"] = {{"code", "handler-error"},
                           {"message", "--spawn: result file too large (> 4 MiB): " + outPath}};
        EmitErrorToStderr(errEnv);
        PostAppQuitBestEffort(cli);
        return kExitHandler;
    }
    case smatchet::cli::ResultFileReadStatus::Ok:
        break;
    }
    try {
        std::string parseErr;
        nlohmann::json fileData = smatchet::json_safe::ParseBounded(content, parseErr);
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
            // C1 parent-fulfill copy-back: the child confined the capture under its own
            // <userData>/screenshots/ (screenshotPath); the trusted parent honours the user's
            // requested location by copying it there + reporting that path. On copy failure we
            // keep the confined path in the envelope so the caller still has a valid file.
            if (!userScreenshotPath.empty() && userScreenshotPath != screenshotPath) {
                std::error_code ec;
                const fs::path destParent = fs::path(userScreenshotPath).parent_path();
                if (!destParent.empty())
                    fs::create_directories(destParent, ec);
                ec.clear();
                fs::copy_file(screenshotPath, userScreenshotPath, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::fprintf(stderr, "[spawn] WARN: could not copy capture to %s (%s); reporting confined path\n",
                                 userScreenshotPath.c_str(),
                                 ec.message().c_str()); // CLI stdout — product output, not logging
                } else {
                    fileData["screenshotPath"] = userScreenshotPath;
                }
            }
        }

        // Relocate the child's confinement-anchored outLog to the caller's requested path.
        // The child wrote it under <userData>/ui-tests/ (basename swapped in before forwarding,
        // satisfying #1566); the trusted parent now fulfils the caller's absolute target.
        RelocateChildOutLog(requestedOutLog, SafeString(fileData, "outLog"));

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

} // namespace

namespace detail {

// Parse the MCP JSON-RPC `result.content[0].text` field back into the envelope.
nlohmann::json ExtractEnvelopeFromMcpResult(const nlohmann::json& body) {
    // REST shape: a content array whose first text element carries the envelope JSON as a string.
    // JSON-RPC shape: that same content array nested under result, or an error object instead.
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
        // outPath + a relative outLog are forwarded RAW — child-side confinement resolves them
        // under <userData>, which both processes agree on without CWD translation (the old CLI-CWD
        // normalization here was actively wrong, so #1566/H1 removed it). Two trusted-parent
        // exceptions honour a user's ABSOLUTE path the child would otherwise reject: screenshotPath
        // (swap→confine-safe basename, copy child's capture back in SpawnAndRunHandleAsync — the
        // #1566 arbitrary-file-write class fix) and, in parallel, an absolute outLog (swap via
        // SwapOutLogForConfineSafeBasename, relocate via RelocateChildOutLog). Both helpers carry
        // the full rationale at their definitions; a relative value leaves the swap a no-op.
        nlohmann::json argsToSend = argsToSendRaw;
        std::string userScreenshotPath;
        if (argsToSend.contains("screenshotPath") && argsToSend["screenshotPath"].is_string()) {
            const std::string requested = argsToSend["screenshotPath"].get<std::string>();
            if (!requested.empty()) {
                userScreenshotPath = requested;
                const std::string ext = fs::path(requested).extension().string();
                argsToSend["screenshotPath"] =
                    "spawn-shot-" + SpawnLogRandomToken() + (ext.empty() ? std::string(".png") : ext);
            }
        }
        const std::string requestedOutLog = SwapOutLogForConfineSafeBasename(argsToSend);

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
        const int scenarioWaitMs = ScenarioWaitMs(pa.timeoutMs, frames);

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
            resultCode = SpawnAndRunHandleAsync(pa, cli, commandName, outPath, frames, scenarioWaitMs, requestedOutLog,
                                                userScreenshotPath);
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

} // namespace detail

#endif // SMATCHET_WITH_MCP

} // namespace cli
} // namespace smatchet
