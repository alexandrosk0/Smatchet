#include "CliCommandRunner.h"
#include "CliCommandRunner_Internal.h"

#include "CliArgCoercion.h"
#include "CliResultFileRead.h"
#include "StandaloneAppBootstrap.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "AppController.h"
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

namespace {

[[noreturn]] static void CliTerminateHandler() {
    std::fprintf(stderr, // pre-logger-init — LOG_* unavailable
                 "{\"ok\":false,\"command\":\"\",\"error\":{\"code\":\"handler-error\","
                 "\"message\":\"CLI hit std::terminate (uncaught exception). No state change occurred.\"}}\n");
    std::_Exit(4);
}

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
    const int scenarioWaitMs = ScenarioWaitMs(pa.timeoutMs, frames);
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

    // Bounded read via the shared leaf (command-input-hardening Phase 1.3): a corrupt/oversized
    // result file must not balloon parent memory before ParseBounded's 4 MiB cap rejects it.
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=the read leaf itself is now shared (CliResultFileRead.h); only the per-surface error-envelope emit remains cloned with CliDispatch.cpp's --spawn reader and differs irreducibly by command var (toolName vs commandName), message prefix, and shutdown call (standalone::Shutdown(boot) vs PostAppQuitBestEffort(cli)) — a shared emitter would take all three as params for a 4-line body, worse coupling than the DRY gate doc endorses exempting; owner=orchestrator; revisit=if a source-tagged CLI error-envelope emitter lands)
    // clang-format on
    std::string content;
    switch (smatchet::cli::ReadResultFileBounded(outPath, 4u * 1024u * 1024u, content)) {
    case smatchet::cli::ResultFileReadStatus::OpenFailed: {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = toolName;
        errEnv["error"] = {{"code", "handler-error"}, {"message", "Could not read scenario result file: " + outPath}};
        EmitErrorToStderr(errEnv);
        standalone::Shutdown(boot);
        return kExitHandler;
    }
    case smatchet::cli::ResultFileReadStatus::TooLarge: {
        nlohmann::json errEnv;
        errEnv["ok"] = false;
        errEnv["command"] = toolName;
        errEnv["error"] = {{"code", "handler-error"},
                           {"message", "Scenario result file too large (> 4 MiB): " + outPath}};
        EmitErrorToStderr(errEnv);
        standalone::Shutdown(boot);
        return kExitHandler;
    }
    case smatchet::cli::ResultFileReadStatus::Ok:
        break;
    }
    try {
        std::string parseErr;
        // clang-format off
        // SMATCHET_DEVIATION(rule=duplication; reason=the ParseBounded-then-emit-envelope tail is a pre-existing clone of CliDispatch.cpp's --spawn reader; the in-process and --spawn scenario result paths share this shape by construction and the god-file-split moved them into separate TUs, differing irreducibly by command var + shutdown call; owner=orchestrator; revisit=if a source-tagged CLI result-envelope emitter lands)
        // clang-format on
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
        ctx.ScenarioHost = boot.app.get();
        ctx.Threading = boot.app.get();
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

namespace detail {

/// Return the path to the running executable (for relaunching as a spawned instance).
std::string GetExePath() {
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=the platform exe-path idiom (GetModuleFileNameA / readlink /proc/self/exe / _NSGetExecutablePath) is a pre-existing clone of SmatchetPreferencesUi_Local.cpp's GetCurrentExePath that the god-file-split merely relocated within CliCommandRunner.cpp; a shared os-exe-path leaf is tracked separately; owner=orchestrator; revisit=when that leaf lands)
    // clang-format on
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

} // namespace detail

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
