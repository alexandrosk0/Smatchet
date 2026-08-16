// debug.* — log emission, MCP status, Lua-eval surface, thread/dock diagnostics,
// window resize / screenshot.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "Commands/PathConfinement.h"

// fan-in Phase 5: depend on the narrow IAppDebug facet (+ IMainThreadPoster via
// MainThreadDispatch.h for the dock/window UI-marshaling commands), not AppController.h.
#include "Interfaces/IAppDebug.h"
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "ConfigManager.h"
#include "Diagnostics/LogTailSelect.h"
#include "Diagnostics/SelfDump.h"
#include "Logger.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Process id for the on-demand dump filename, mirroring the per-PID log-file naming
// in Source/Standalone/main.cpp: two instances (a --spawn child plus a manual one)
// can be alive at once, so the pid is what keeps their dumps from colliding.
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// Pulled from SmatchetUI.cpp via extern — same singleton pattern used by
// ViewToggleCommands.cpp and the dock diagnostics block.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PInt;
using builtin_detail::PString;
using builtin_detail::ToLowerAscii;

namespace {

long long CurrentProcessIdForDump() {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

// --- debug.log --------------------------------------------------------------
static void RegisterDebugLogCommand(CommandRegistry& reg) {
    Command c = MakeCommand("debug.log", "Emit a one-shot Logger entry (info/warn/error). For agent breadcrumbs.",
                            [](const nlohmann::json& args, const CommandContext&) {
                                const std::string level = ToLowerAscii(args.value("level", std::string("info")));
                                const std::string msg = args.value("message", std::string());
                                if (level == "trace")
                                    LOG_TRACE("[cmd] %s", msg.c_str());
                                else if (level == "debug")
                                    LOG_DEBUG("[cmd] %s", msg.c_str());
                                else if (level == "warn")
                                    LOG_WARN("[cmd] %s", msg.c_str());
                                else if (level == "error")
                                    LOG_ERROR("[cmd] %s", msg.c_str());
                                else
                                    LOG_INFO("[cmd] %s", msg.c_str());
                                nlohmann::json out;
                                out["written"] = true;
                                return CommandResult::Success(std::move(out));
                            });
    ParamSpec level = PString("level", "trace|debug|info|warn|error");
    level.Default = std::make_shared<nlohmann::json>("info");
    level.Enum = {"trace", "debug", "info", "warn", "error"};
    c.Params = {
        std::move(level),
        PString("message", "Log message text.", /*required*/ true),
    };
    reg.Register(std::move(c));
}

// --- debug.mcp_status -------------------------------------------------------
static void RegisterDebugMcpStatusCommand(CommandRegistry& reg, IAppDebug& app) {
    Command c = MakeCommand("debug.mcp_status", "MCP server reachability and last-client-activity timestamp.",
                            [&app](const nlohmann::json&, const CommandContext&) {
                                nlohmann::json out;
#if defined(SMATCHET_WITH_MCP)
                                out["mcpEnabled"] = true;
                                std::chrono::steady_clock::time_point lastActivity;
                                const bool hasActivity = app.TryGetMcpLastClientHttpActivity(&lastActivity);
                                out["hasClientActivity"] = hasActivity;
                                if (hasActivity) {
                                    const auto elapsed = std::chrono::steady_clock::now() - lastActivity;
                                    out["lastActivityMsAgo"] = static_cast<long long>(
                                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                                }
                                out["activityLog"] = app.CopyMcpActivityLog();
#else
                                (void)app;
                                out["mcpEnabled"] = false;
#endif
                                return CommandResult::Success(std::move(out));
                            });
    reg.Register(std::move(c));
}

// --- debug.thread_dump ------------------------------------------------------
static void RegisterDebugThreadDumpCommand(CommandRegistry& reg) {
    Command c = MakeCommand("debug.thread_dump", "Return basic thread count and state information.",
                            [](const nlohmann::json&, const CommandContext&) {
                                // C++14 doesn't expose thread IDs without platform headers.
                                // Return what we can without OS-specific code in Source/Core.
                                nlohmann::json out;
                                out["note"] = "Thread dump requires OS-specific APIs; counts are unavailable via this "
                                              "command. For per-thread stacks, call debug.dump_self and triage the "
                                              "minidump with agents/scripts/core/dump-triage.sh.";
                                out["hardwareConcurrency"] = static_cast<int>(std::thread::hardware_concurrency());
                                out["selfDumpAvailable"] = smatchet::diagnostics::HasSelfDumpProvider();
                                return CommandResult::Success(std::move(out));
                            });
    c.Description = "Returns hardware_concurrency plus whether debug.dump_self can capture per-thread stacks on this "
                    "host. Full inline OS thread dump is not available cross-platform.";
    reg.Register(std::move(c));
}

// --- debug.log_tail ---------------------------------------------------------
// Read-back of the Logger's in-memory ring so an agent can pull recent log lines
// over CLI/MCP instead of locating %LOCALAPPDATA%\Smatchet\Smatchet-<pid>.log on
// disk (impossible for a purely-MCP client, e.g. a --spawn child).
//
// Deliberately NOT marshalled to the UI thread. This is one of the commands an
// agent reaches for precisely when the UI thread is wedged, and
// RunOnUiThreadAsCommandResult blocks with no timeout (MainThreadDispatch.h) —
// marshalling here would make the command useless in the situation it exists for.
// GetEntriesSnapshot takes only the Logger's own mutex, which the render loop
// never holds across a frame.
//
// No redaction happens here: Logger::Log applies privacy::RedactLogLine at ingest,
// so every ring entry is already redacted. LogTailSelect.h documents that and a
// test pins it, because the property is what makes the ring safe to hand to MCP.
static void RegisterDebugLogTailCommand(CommandRegistry& reg) {
    Command c =
        MakeCommand("debug.log_tail", "Return the tail of the in-memory runtime log (oldest first).",
                    [](const nlohmann::json& args, const CommandContext&) {
                        const long long requested =
                            args.value("lines", static_cast<long long>(smatchet::diagnostics::kLogTailDefaultLines));
                        const std::size_t maxLines = smatchet::diagnostics::ClampLogTailLines(requested);
                        const LogLevel minLevel =
                            Logger::ParseLogLevelString(args.value("minLevel", std::string("trace")), LogLevel::Trace);
                        const std::string contains = args.value("contains", std::string());

                        const std::vector<LogEntry> snapshot = Logger::Instance().GetEntriesSnapshot();
                        std::size_t totalMatched = 0;
                        const std::vector<LogEntry> selected =
                            smatchet::diagnostics::SelectLogTail(snapshot, minLevel, contains, maxLines, &totalMatched);

                        nlohmann::json lines = nlohmann::json::array();
                        for (std::size_t i = 0; i < selected.size(); ++i) {
                            nlohmann::json row;
                            row["ts"] = selected[i].timestampSeconds;
                            row["level"] = Logger::LogLevelToString(selected[i].level);
                            row["message"] = selected[i].message;
                            lines.push_back(std::move(row));
                        }

                        nlohmann::json out;
                        out["lines"] = std::move(lines);
                        out["returned"] = static_cast<long long>(selected.size());
                        out["totalMatched"] = static_cast<long long>(totalMatched);
                        out["ringSize"] = static_cast<long long>(snapshot.size());
                        out["truncated"] = totalMatched > selected.size();
                        return CommandResult::Success(std::move(out));
                    });
    // ValidateAndResolveArgs enforces MinInt/MaxInt centrally, so an out-of-range
    // request is a ValidationError naming the bound rather than a silent clamp.
    // ClampLogTailLines stays as the in-handler net for any path that resolves args
    // without the validator.
    ParamSpec lines = PInt("lines", "How many matching entries to return (newest kept).",
                           static_cast<long long>(smatchet::diagnostics::kLogTailDefaultLines));
    lines.MinInt = std::make_shared<long long>(static_cast<long long>(smatchet::diagnostics::kLogTailMinLines));
    lines.MaxInt = std::make_shared<long long>(static_cast<long long>(smatchet::diagnostics::kLogTailMaxLines));
    ParamSpec minLevel = PString("minLevel", "Lowest level to include: trace|debug|info|warn|error.");
    minLevel.Default = std::make_shared<nlohmann::json>("trace");
    minLevel.Enum = {"trace", "debug", "info", "warn", "error"};
    c.Params = {
        std::move(lines),
        std::move(minLevel),
        PString("contains", "Only entries whose message contains this substring."),
    };
    c.Title = "Log Tail";
    c.Description = "Returns {lines:[{ts,level,message}], returned, totalMatched, ringSize, truncated}. Entries are "
                    "oldest-first and already redacted. Filtering is applied BEFORE the tail, so lines=10 with "
                    "contains=sync yields the 10 most recent sync entries. Examples: "
                    "`Smatchet.exe cmd debug.log_tail --lines=50` | "
                    "`Smatchet.exe cmd debug.log_tail --minLevel=warn --contains=tracker`.";
    reg.Register(std::move(c));
}

// --- debug.dump_self --------------------------------------------------------
// Write a minidump of THIS process on demand. The crash pipeline only fires on a
// crash, so a wedged-but-alive process had no capture path at all; a minidump
// carries every thread's stack, and dump-triage.sh already reads it.
//
// Same no-marshalling rule as debug.log_tail, and for the same reason: the target
// case is a hung UI thread. See docs/adr/0024-self-minidump-over-in-process-stack-walk.md.
//
// No caller-supplied path: the filename is derived from clock + pid and joined under
// <userData>/crashes/, so this command has no path-injection surface at all (unlike
// debug.window.screenshot, which must confine an argument).
static void RegisterDebugDumpSelfCommand(CommandRegistry& reg) {
    Command c =
        MakeCommand("debug.dump_self", "Write a minidump of this process (every thread's stack) for hang diagnosis.",
                    [](const nlohmann::json&, const CommandContext& ctx) -> CommandResult {
                        // Availability is checked FIRST, before anything about configuration.
                        // On a host with no writer (DX12/Unreal, Android, the POSIX gate) the
                        // user-data dir is irrelevant, and "not available on this host" is the
                        // answer an agent needs in order to fall back to the out-of-process
                        // procdump recipe — reporting a config problem instead would send it
                        // chasing the wrong thing. Not an error: absence is a fact about the host.
                        if (!smatchet::diagnostics::HasSelfDumpProvider()) {
                            nlohmann::json out;
                            out["wrote"] = false;
                            out["available"] = false;
                            out["reason"] = "no self-dump provider installed on this host";
                            return CommandResult::Success(std::move(out));
                        }

                        const std::string userData = ConfigManager::GetUserDataDirectory();
                        if (userData.empty()) {
                            return CommandResult::Failure(ErrorCode::HandlerError,
                                                          "no user-data directory configured; cannot place the dump");
                        }
                        const long long epochMs =
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count());
                        const std::string fileName =
                            smatchet::diagnostics::MakeSelfDumpFileName(epochMs, CurrentProcessIdForDump());
                        const std::string absPath = userData + "/crashes/" + fileName;

                        if (ctx.DryRun) {
                            nlohmann::json dry;
                            dry["wouldWrite"] = absPath;
                            dry["available"] = true;
                            return CommandResult::Success(std::move(dry));
                        }

                        std::string err;
                        if (!smatchet::diagnostics::WriteSelfDump(absPath, err)) {
                            return CommandResult::Failure(ErrorCode::HandlerError, "self-dump failed: " + err);
                        }
                        LOG_INFO("debug.dump_self: wrote minidump to %s", absPath.c_str());
                        nlohmann::json out;
                        out["wrote"] = true;
                        out["available"] = true;
                        out["path"] = absPath;
                        return CommandResult::Success(std::move(out));
                    });
    c.Title = "Dump Self";
    c.Description = "Returns {wrote, available, path} on success, or {wrote:false, available:false, reason} on a host "
                    "with no writer (DX12/Unreal, Android). Writes <userData>/crashes/ondemand-<epochMs>-<pid>.dmp "
                    "using MiniDumpNormal — the same scope the crash handler uses, chosen so heap-resident secrets are "
                    "not swept into the dump. Triage with `bash agents/scripts/core/dump-triage.sh <path>`. Example: "
                    "`Smatchet.exe cmd debug.dump_self`.";
    c.Idempotent = false;
    c.DryRunSupported = true;
    reg.Register(std::move(c));
}

// --- debug.lua_log_test -----------------------------------------------------
// Headless validation harness for the persistent-Lua-error feature. Installs
// capture sinks via AddAutomationLogSink + AddAutomationErrorSink, runs the
// snippet through ExecuteLuaConsoleSnippet, returns captured lines + the
// scriptingWindowOpenRequested_ flag state. Sinks persist after the call —
// intended for ephemeral --spawn instances which exit between assertions.
static void RegisterDebugLuaLogTestCommand(CommandRegistry& reg, IAppDebug& app) {
    Command c =
        MakeCommand("debug.lua_log_test",
                    "Run a Lua snippet capturing the automation log + error sinks. Returns JSON {log_lines, err_lines, "
                    "window_requested} for automated validation.",
                    [&app](const nlohmann::json& args, const CommandContext& /*ctx*/) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                        const std::string code = args.value("code", std::string());
                        if (code.empty()) {
                            return CommandResult::Failure(ErrorCode::ValidationError, "code required");
                        }
                        auto logCaptured = std::make_shared<std::vector<std::string>>();
                        auto errCaptured = std::make_shared<std::vector<std::string>>();
                        app.AddAutomationLogSink([logCaptured](const std::string& m) { logCaptured->push_back(m); });
                        app.AddAutomationErrorSink([errCaptured](const std::string& m) { errCaptured->push_back(m); });
                        // Consume any prior request so we observe only this snippet's effect.
                        (void)app.ConsumeScriptingWindowRequest();
                        std::string outError;
                        std::string outResult;
                        const bool ok = app.ExecuteLuaConsoleSnippet(code, outError, outResult);
                        const bool windowReq = app.ConsumeScriptingWindowRequest();
                        nlohmann::json out;
                        out["ok_snippet"] = ok;
                        out["snippet_error"] = outError;
                        out["snippet_result"] = outResult;
                        out["log_lines"] = *logCaptured;
                        out["err_lines"] = *errCaptured;
                        out["window_requested"] = windowReq;
                        return CommandResult::Success(std::move(out));
#else
            (void)app;
            (void)args;
            return CommandResult::Failure(ErrorCode::HandlerError, "Lua automation is not enabled in this build.");
#endif
                    });
    c.Destructive = true;
    c.Idempotent = false;
    c.Params = {PString("code", "Lua code to evaluate (snippet form, sandboxed).", true)};
    reg.Register(std::move(c));
}

// --- debug.lua_eval ---------------------------------------------------------
static void RegisterDebugLuaEvalCommand(CommandRegistry& reg, IAppDebug& app) {
    Command c =
        MakeCommand("debug.lua_eval", "Evaluate a Lua code snippet and return the result summary.",
                    [&app](const nlohmann::json& args, const CommandContext& /*ctx*/) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                        const std::string code = args.value("code", std::string());
                        std::string outError;
                        std::string outResult;
                        const bool ok = app.ExecuteLuaConsoleSnippet(code, outError, outResult);
                        if (!ok) {
                            return CommandResult::Failure(ErrorCode::HandlerError, "Lua eval failed: " + outError);
                        }
                        return CommandResult::Success({{"result", outResult}});
#else
                        (void)app;
                        (void)args;
                        return CommandResult::Failure(ErrorCode::HandlerError,
                                                      "Lua automation is not enabled in this build.");
#endif
                    });
    c.Destructive = true;
    c.Idempotent = false;
    c.Params = {PString("code", "Lua code to evaluate.", true)};
    reg.Register(std::move(c));
}

// --- debug.dock.dump --------------------------------------------------------
// Handler runs on the UI thread because ImGui::DockBuilderGetNode reads
// the imgui dock-node table which is owned by the UI thread.
static void RegisterDebugDockDumpCommand(CommandRegistry& reg, IMainThreadPoster& poster) {
    Command c = MakeCommand("debug.dock.dump", "Log all dock nodes to the runtime log.",
                            [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                return RunOnUiThreadAsCommandResult(poster, []() {
                                    static const ImGuiID kNodes[] = {
                                        SmatchetDockNodeIds::kPrimarySideBar,
                                        SmatchetDockNodeIds::kBottomPanel,
                                        SmatchetDockNodeIds::kSecondarySideBar,
                                    };
                                    static const char* const kNames[] = {
                                        "PrimarySideBar(0x4)",
                                        "BottomPanel(0xA)",
                                        "SecondarySideBar(0x10)",
                                    };
                                    for (int i = 0; i < 3; ++i) {
                                        ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(kNodes[i]);
                                        if (!node) {
                                            LOG_INFO("debug.dock.dump: %s NOT FOUND", kNames[i]);
                                        } else {
                                            LOG_INFO("debug.dock.dump: %s id=0x%08X size=(%.0f,%.0f) "
                                                     "flags=0x%X tabs=%d empty=%d",
                                                     kNames[i], static_cast<unsigned int>(node->ID), node->Size.x,
                                                     node->Size.y, static_cast<int>(node->LocalFlags),
                                                     node->Windows.Size, static_cast<int>(node->IsEmpty()));
                                        }
                                    }
                                    // Walk the full dockspace tree from the root node.
                                    const ImGuiID kDockSpaceId = 0x08BD597Du;
                                    ImGuiDockNode* root = ::ImGui::DockBuilderGetNode(kDockSpaceId);
                                    if (!root) {
                                        LOG_INFO("debug.dock.dump: dockspace root (0x%08X) NOT FOUND",
                                                 static_cast<unsigned int>(kDockSpaceId));
                                    } else {
                                        // Iterative depth-first walk (no recursion; C++14 safe).
                                        struct Frame {
                                            ImGuiDockNode* node;
                                            int depth;
                                        };
                                        std::vector<Frame> stack;
                                        stack.push_back({root, 0});
                                        while (!stack.empty()) {
                                            Frame frame = stack.back();
                                            stack.pop_back();
                                            ImGuiDockNode* n = frame.node;
                                            if (!n) {
                                                continue;
                                            }
                                            // Build depth-indent prefix.
                                            std::string indent(static_cast<size_t>(frame.depth * 2), ' ');
                                            LOG_INFO("debug.dock.dump: %sid=0x%08X size=(%.0f,%.0f) "
                                                     "flags=0x%X tabs=%d empty=%d",
                                                     indent.c_str(), static_cast<unsigned int>(n->ID), n->Size.x,
                                                     n->Size.y, static_cast<int>(n->LocalFlags), n->Windows.Size,
                                                     static_cast<int>(n->IsEmpty()));
                                            // Push right child first — stack is LIFO so left is processed first.
                                            if (n->ChildNodes[1]) {
                                                stack.push_back({n->ChildNodes[1], frame.depth + 1});
                                            }
                                            if (n->ChildNodes[0]) {
                                                stack.push_back({n->ChildNodes[0], frame.depth + 1});
                                            }
                                        }
                                    }
                                    nlohmann::json out;
                                    out["logged"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
                            });
    reg.Register(std::move(c));
}

// --- debug.dock.reset -------------------------------------------------------
// Must run on the UI thread — SmatchetUI_ResetLayoutToDefault touches ImGui + g_ui.
static void RegisterDebugDockResetCommand(CommandRegistry& reg, IMainThreadPoster& poster) {
    Command c = MakeCommand("debug.dock.reset", "Force reset dock layout to VS shell default.",
                            [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                return RunOnUiThreadAsCommandResult(poster, []() {
                                    SmatchetUI_ResetLayoutToDefault(g_ui);
                                    nlohmann::json out;
                                    out["reset"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
                            });
    c.Destructive = true;
    c.Idempotent = true;
    reg.Register(std::move(c));
}

// --- debug.window.resize ----------------------------------------------------
// Sets a transient flag; the main loop polls and calls glfwSetWindowSize.
// Standalone-only; on the Unreal/DX12 build the flag is set but never consumed (harmless).
static void RegisterDebugWindowResizeCommand(CommandRegistry& reg, IMainThreadPoster& poster) {
    Command c =
        MakeCommand("debug.window.resize", "Resize the GLFW window (standalone only).",
                    [&poster](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                        const int w = static_cast<int>(args.value("width", 0));
                        const int h = static_cast<int>(args.value("height", 0));
                        if (w <= 0 || h <= 0) {
                            return CommandResult::Failure(ErrorCode::ValidationError, "width and height must be > 0");
                        }
                        // Marshal the g_ui request-flag writes to the UI thread. When this
                        // command is dispatched from an MCP/Lua worker thread, writing the
                        // non-atomic int/bool request fields would race the standalone main
                        // loop that polls them each frame (data race / UB). The dock.* and
                        // bug.report handlers in this TU follow the same seam.
                        return RunOnUiThreadAsCommandResult(poster, [w, h]() {
                            g_ui.requestWindowWidth = w;
                            g_ui.requestWindowHeight = h;
                            g_ui.requestWindowResize = true;
                            nlohmann::json out;
                            out["width"] = w;
                            out["height"] = h;
                            return CommandResult::Success(std::move(out));
                        });
                    });
    c.Params = {
        PInt("width", "Window width in pixels.", 0),
        PInt("height", "Window height in pixels.", 0),
    };
    c.Params[0].Required = true;
    c.Params[1].Required = true;
    c.Idempotent = true;
    reg.Register(std::move(c));
}

// --- debug.window.screenshot ------------------------------------------------
// Sets a transient flag + path; the main loop polls and writes PNG/PPM.
// Standalone-only; on Unreal/DX12 the flag is set but never consumed.
static void RegisterDebugWindowScreenshotCommand(CommandRegistry& reg, IMainThreadPoster& poster) {
    Command c =
        MakeCommand("debug.window.screenshot", "Save a PNG screenshot of the current viewport.",
                    [&poster](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                        const std::string path = args.value("path", std::string());
                        if (path.empty()) {
                            return CommandResult::Failure(ErrorCode::ValidationError, "path is required");
                        }
                        // Confine the caller-supplied path under <userData>/screenshots/.
                        // This command is MCP- AND Lua-reachable; an unconfined path is an
                        // arbitrary-file-write primitive (the #1566 class — SECURITY_AUDIT.md).
                        // Reject absolute / '..' / escaping paths up front.
                        std::string confinedPath;
                        std::string confineErr;
                        if (!ConfinePathUnderSubdir(ConfigManager::GetUserDataDirectory(), "screenshots", path,
                                                    confinedPath, confineErr)) {
                            return CommandResult::Failure(ErrorCode::ValidationError, "path rejected: " + confineErr);
                        }
                        // Marshal to the UI thread: requestScreenshotPath is a std::string
                        // read by the standalone main loop each frame, so an unsynchronised
                        // write from an MCP/Lua worker thread is a genuine data race (the
                        // string buffer can reallocate under the reader — UB, not benign).
                        return RunOnUiThreadAsCommandResult(poster, [confinedPath]() {
                            g_ui.requestScreenshotPath = confinedPath;
                            g_ui.requestScreenshot = true;
                            nlohmann::json out;
                            out["path"] = confinedPath;
                            return CommandResult::Success(std::move(out));
                        });
                    });
    c.Params = {
        PString("path", "Output file path (PNG if extension is .png, PPM otherwise).", /*required*/ true),
    };
    c.Idempotent = false;
    reg.Register(std::move(c));
}

// --- debug.crash ------------------------------------------------------------
// Deliberately crashes the process to validate the phase-2 crash reporter.
// Next launch should open a pre-filled bug report. Destructive; requires --yes.
// kind selects segv null-deref, abort, or throw.
static void RegisterDebugCrashCommand(CommandRegistry& reg) {
    Command c = MakeCommand(
        "debug.crash", "Deliberately crash the process (tests the crash reporter). Requires --yes.",
        [](const nlohmann::json& args, const CommandContext& ctx) -> CommandResult {
            const std::string kind = args.value("kind", std::string("segv"));
            if (ctx.DryRun) {
                return CommandResult::Success({{"wouldCrash", kind}});
            }
            LOG_ERROR("debug.crash: intentionally crashing the process (kind=%s)", kind.c_str());
            if (kind == "abort") {
                std::abort();
            } else if (kind == "throw") {
                // DR27: CommandRegistry::Dispatch wraps every handler in
                // try/catch, so a direct throw here is swallowed and the crash
                // reporter never runs. Let the exception escape a worker thread's
                // top-level function instead: an exception propagating out of a
                // std::thread entry calls std::terminate (with the exception in
                // flight), firing the terminate handler installed by
                // InstallCrashHandlers — the unhandled-exception path this kind
                // exercises — with no noexcept-throws boundary that MSVC /WX
                // rejects as C4297.
                std::thread([]() { throw std::runtime_error("debug.crash: intentional unhandled exception"); }).join();
            }
            // Default: null dereference -> SIGSEGV / access violation.
            volatile int* p = nullptr;
            *p = 42;
            return CommandResult::Success({{"crashed", false}}); // unreachable
        });
    c.Params = {
        PString("kind", "Crash kind: segv (default) | abort | throw."),
    };
    c.Destructive = true;
    c.Idempotent = false;
    c.DryRunSupported = true;
    reg.Register(std::move(c));
}

} // namespace

void RegisterDebugCommands(CommandRegistry& reg, IAppDebug& app, IMainThreadPoster& poster) {
    RegisterDebugLogCommand(reg);
    RegisterDebugLogTailCommand(reg);
    RegisterDebugMcpStatusCommand(reg, app);
    RegisterDebugThreadDumpCommand(reg);
    RegisterDebugDumpSelfCommand(reg);
    RegisterDebugLuaLogTestCommand(reg, app);
    RegisterDebugLuaEvalCommand(reg, app);
    RegisterDebugDockDumpCommand(reg, poster);
    RegisterDebugDockResetCommand(reg, poster);
    RegisterDebugWindowResizeCommand(reg, poster);
    RegisterDebugWindowScreenshotCommand(reg, poster);
    RegisterDebugCrashCommand(reg);
}

} // namespace cmd
} // namespace smatchet
