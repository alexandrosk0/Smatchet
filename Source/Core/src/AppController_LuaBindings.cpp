#include "AppController.h"
#include "AppControllerImpl.h" // AppController::Impl — cold sol2/automation member storage (pImpl #19b)
#include "ILuaBindingHost.h"
#include "LuaAutomationHost.h"
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls app_.Cache-> methods.

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AiAssistantController.h"
#include "AiLuaPromptRateLimit.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IssueTableSerializer.h"
#include "LuaAutomationHookPolicyPure.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <exception>
#include <future>
#include <ghc/filesystem.hpp>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <utility> // std::move (AsciiLowerCopy forwarder)
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <iterator>

#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared AppController_LuaBindings-TU include prologue is grandfathered across the god-file-split siblings (AppController_LuaBindings.cpp / _Ui / _Ai / _Tickets) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared AppController_LuaBindings TU prologue header is introduced)
// clang-format on

#include "Json/BoundedJsonParse.h"

#include "imgui.h"
#include "Logger.h"
#include "SmatchetFieldIconRender.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"
#include "UiPerfMonitor.h"

#include "AppController_LuaBindings_detail.h"

// ---------------------------------------------------------------------------
// File-scope definitions — shared with AppController_LuaBindings_Draw.cpp via
// AppController_LuaBindings_detail.h.  Must NOT be in an anonymous namespace.
// ---------------------------------------------------------------------------

// Thread-local: false during cached cell / window recording. The imgui.* glue
// checks this and luaL_errors if Lua tries to draw immediate-mode UI while a
// cached recording is active — otherwise the script would draw once on cache
// miss and silently vanish on the next replay.
// Per docs/plans/shipped/lua-recorded-cmd-list.md decision #5 + finding #6.
thread_local bool g_luaImmediateModeAllowed = true;

std::string TruncateForTrace(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) {
        return s;
    }
    return s.substr(0, maxLen) + "...";
}

bool LuaTruthy(const sol::object& o) {
    if (!o.valid()) {
        return false;
    }
    const sol::type t = o.get_type();
    if (t == sol::type::lua_nil) {
        return false;
    }
    if (t == sol::type::boolean) {
        return o.as<bool>();
    }
    return true;
}

// Forwards to the shared Core helper (gate-blind-spot-sweep Slice 2) — the body used to be a
// third copy of the same std::transform. The NAME stays: it is declared in
// AppController_LuaBindings_detail.h and called from the _Tickets / _Ui / _Draw sibling TUs.
std::string AsciiLowerCopy(std::string s) { return ToLowerAsciiCopy(std::move(s)); }

namespace {

// JSON <-> Lua marshalling moved to the shared Json/LuaJsonConvert.h leaf
// (reached via AppController_LuaBindings_detail.h). The public JsonToLua /
// LuaToJson are global-scope inline there; call sites below are unchanged.
// decode_json's threat-model rationale now lives above LuaDecodeJsonBind.

std::string SanitizeLogText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\t' || uc == '\n') {
            out.push_back(c);
        } else if (uc < 0x20 || uc == 0x7F) {
            out.push_back('?');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

sol::environment CreateSandboxEnvironment(sol::state& lua) {
    // Lua semantics gotcha: `sandbox["X"] = nil` is equivalent to `rawset(sandbox, "X", nil)`
    // which REMOVES the key, allowing the metatable `__index = lua.globals()` fallback to
    // resolve to the *real* function. So nilling doesn't block — it merely unbinds locally.
    // Use `false` (a non-nil-but-non-callable sentinel) so direct lookups hit it AND any
    // `something()` call errors with "attempt to call a boolean value".
    sol::environment sandbox(lua, sol::create, lua.globals());
    const auto block = [&](const char* name) { sandbox[name] = false; };
    // Sandbox escapes — primitives that load / execute external code.
    block("dofile");
    block("loadfile");
    block("load");
    block("loadstring");
    block("require");
    block("collectgarbage");
    block("io");      // file / process I/O
    block("package"); // module loader (could load shared libs)
    block("debug");   // bytecode / locals introspection
    // Bytecode dump is mostly inert without `load`, but strip as defense-in-depth.
    sandbox["string"] = lua.globals()["string"]; // shadow + then patch the local copy
    // Cannot mutate the shared global `string` table (would leak to non-sandboxed paths
    // and break Lua-internal users of string.dump). Build a per-sandbox copy with the
    // dangerous fns blocked. Cheap — string is a small table of function refs.
    sol::table stringSafe = lua.create_table();
    sol::table stringGlobal = lua.globals()["string"];
    if (stringGlobal.valid()) {
        for (auto& kv : stringGlobal) {
            const std::string key = kv.first.as<std::string>();
            if (key == "dump")
                continue; // strip bytecode dumper
            stringSafe[key] = kv.second;
        }
    }
    sandbox["string"] = stringSafe;
    // Strict mode (defense-in-depth): metatable + raw-table accessors. A script with
    // these can hijack the sandbox env's bindings — `rawset(_G, "log_info", fake)` would
    // replace the log binding. Hook patterns don't need these.
    block("setmetatable");
    block("getmetatable");
    block("rawset");
    block("rawget");
    block("rawequal");
    block("rawlen");
    // `os` is intentionally NOT blocked — InitLuaCore replaced the standard lib with a
    // whitelist of safe time/date functions (time, clock, difftime, date). The global
    // `os` table is the safe one; the sandbox metatable falls through to it naturally.
    return sandbox;
}

namespace smatchet_lua_init_detail {

/** GCC + sol2: lambdas with the same signature can share one demangled metatable name → heap corruption in Lua.
 *  All InitLua callables here are plain functions with distinct symbols (plus scoped AppController* for Ticket glue).
 */

// UI glues resolve through `__smatchet_app_ui` (an AppController*), set by
// InitLuaUi. The Core key `__smatchet_app` stores an `ILuaBindingHost*` after
// the interface lift (see ILuaBindingHost.h + AppController_LuaBindingsCore.cpp).
// Sol2 v2.20.6's `get<T*>` does not perform a type-safe downcast through
// multiple inheritance offsets, so resolving an AppController* from a
// stored ILuaBindingHost* would corrupt with the wrong base offset. The
// dedicated UI key keeps the cast site straightforward.
AppController::Impl* ResolveApp(sol::this_state L) {
    sol::state_view lua(L);
    const sol::object appObj = lua["__smatchet_app_ui"];
    if (!appObj.valid() || appObj.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    return appObj.as<AppController::Impl*>();
}

} // namespace smatchet_lua_init_detail

void AppController::InitLua() {
    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    impl_->InitLuaCore(lua);      // #19c: Init* relocated onto Impl
    impl_->InitLuaUi(lua);
}

// Lifted to `smatchet::lua::InitLuaCore(state, host)` in AppController_LuaBindingsCore.cpp.
// This forwarder keeps the existing call sites (`AppController::InitLua` + the
// AutomationWorkerLoop `InitLuaCore(bgState)`) source-compatible. The host
// pointer passed through is `this` (AppController inherits from ILuaBindingHost
// when SMATCHET_WITH_LUA_AUTOMATION is on).
void AppController::Impl::InitLuaCore(sol::state& state) { smatchet::lua::InitLuaCore(state, this); }

void AppController::Impl::InitLuaUi(sol::state& state) {
    // Mirror of `__smatchet_app` (ILuaBindingHost*) -- this slot holds the concrete
    // AppController* so UI glues in this TU (which still call AppController-only
    // members like LuaRegisterFieldDisplayCachedBind / LuaUiRegisterWindowBind)
    // can resolve through ResolveApp without a multiple-inheritance offset hazard.
    // Sol2 v2.20.6's `get<T*>` does not retag through base offsets, so storing
    // both pointers explicitly is safer than downcasting at lookup time.
    state["__smatchet_app_ui"] = this;

    // Cached-cmd-list cell renderer. Provider receives a 7th arg `draw` -- a recorder; the
    // returned recording replays every frame until cache-key inputs change. See plan §Cells.
    state.set_function("register_field_display_cached", &smatchet_lua_init_detail::LuaRegisterFieldDisplayCachedGlue);
    state.set_function("unregister_field_display_cached",
                       &smatchet_lua_init_detail::LuaUnregisterFieldDisplayCachedGlue);
    state.set_function("register_field_display_cached_by_name",
                       &smatchet_lua_init_detail::LuaRegisterFieldDisplayCachedByNameGlue);
    state.set_function("unregister_field_display_cached_by_name",
                       &smatchet_lua_init_detail::LuaUnregisterFieldDisplayCachedByNameGlue);
    state.set_function("register_field_icon_map", &smatchet_lua_init_detail::LuaRegisterFieldIconMapGlue);
    state.set_function("unregister_field_icon_map", &smatchet_lua_init_detail::LuaUnregisterFieldIconMapGlue);

    // Recorder usertype: only methods on this object may be called inside cached providers
    // and window draw fns. Constructors disabled — the C++ side hands a shared_ptr to Lua at
    // recording time and Deactivate()s it when done. Stashing the recorder past that point
    // errors cleanly via the active_ flag.
    // Lua-side construction of SmatchetDrawList is intentionally not exposed; C++ hands the
    // recorder to providers via std::shared_ptr at recording time. Omitting `sol::call_constructor`
    // is enough — sol2 won't synthesize a constructor binding from this list.
    state.new_usertype<LuaDrawList>(
        "SmatchetDrawList", "text", &LuaDrawList::Text, "text_unformatted", &LuaDrawList::TextUnformatted, "image",
        &LuaDrawList::Image, "progress_bar", &LuaDrawList::ProgressBar, "same_line", &LuaDrawList::SameLine,
        "separator", &LuaDrawList::Separator, "dummy", &LuaDrawList::Dummy, "push_color", &LuaDrawList::PushColor,
        "pop_color", &LuaDrawList::PopColor, "set_tooltip", &LuaDrawList::SetTooltip, "button", &LuaDrawList::Button,
        "input_text", &LuaDrawList::InputText, "on_deactivated", &LuaDrawList::OnDeactivated,
        "on_deactivated_after_edit", &LuaDrawList::OnDeactivatedAfterEdit);

    sol::table imgui = state.create_table();
    imgui.set_function("progress_bar", &smatchet_lua_init_detail::ImGuiProgressBarGlue);
    imgui.set_function("text", &smatchet_lua_init_detail::LuaImGuiTextGlue);
    imgui.set_function("text_unformatted", &smatchet_lua_init_detail::LuaImGuiTextUnformattedGlue);
    imgui.set_function("get_content_region_avail", &smatchet_lua_init_detail::ImGuiGetContentRegionAvailGlue);
    imgui.set_function("button", &smatchet_lua_init_detail::ImGuiButtonGlue);
    imgui.set_function("same_line", &smatchet_lua_init_detail::ImGuiSameLineGlue);
    imgui.set_function("separator", &smatchet_lua_init_detail::ImGuiSeparatorGlue);
    imgui.set_function("image", &smatchet_lua_init_detail::LuaImGuiImageGlue);
    state["imgui"] = imgui;

    sol::table ui = state.create_table();
    ui.set_function("register_window", &smatchet_lua_init_detail::LuaUiRegisterWindowGlue);
    ui.set_function("unregister_window", &smatchet_lua_init_detail::LuaUiUnregisterWindowGlue);
    ui.set_function("invalidate_window", &smatchet_lua_init_detail::LuaUiInvalidateWindowGlue);
    ui.set_function("invalidate_field_cache", &smatchet_lua_init_detail::LuaUiInvalidateFieldCacheGlue);
    ui.set_function("invalidate_field_cache_for", &smatchet_lua_init_detail::LuaUiInvalidateFieldCacheGlue);
    ui.set_function("register_ticket_action", &smatchet_lua_init_detail::LuaUiRegisterTicketActionGlue);
    ui.set_function("register_global_action", &smatchet_lua_init_detail::LuaUiRegisterGlobalActionGlue);
    state["ui"] = ui;

    // Phase E: `ai.*` surface. Registered here (not in `InitLuaCore`) because the
    // glues resolve through `__smatchet_app_ui` (an `AppController*`); the Core
    // table only stores an `ILuaBindingHost*`. See `ResolveApp` note above.
    sol::table aiTbl = state.create_table();
    aiTbl.set_function("add_context", &smatchet_lua_init_detail::LuaAiAddContextGlue);
    aiTbl.set_function("clear_context", &smatchet_lua_init_detail::LuaAiClearContextGlue);
    aiTbl.set_function("prompt", &smatchet_lua_init_detail::LuaAiPromptGlue);
    state["ai"] = aiTbl;
}

void AppController::Impl::LuaLogInfoBind(const std::string& msg) {
    const std::string clean = SanitizeLogText(msg);
    if (luaHost_ && !luaHost_->SnapshotLogSinks().empty()) {
        for (const auto& sink : luaHost_->SnapshotLogSinks()) {
            sink(clean);
        }
    } else {
        LOG_INFO("[LUA] %s", clean.c_str());
    }
}

smatchet::cmd::CommandRegistry& AppController::Impl::LuaCommands() { return app_.Commands(); }

// Facet accessors for the invoke glue's command context. The upcasts happen
// here, where the concrete app type is complete; the glue TU sees only the
// forward-declared facets.
IAppScenarioHost* AppController::Impl::ScenarioHostForCommandContext() { return &app_; }

IAppThreading* AppController::Impl::ThreadingForCommandContext() { return &app_; }

void AppController::Impl::ParseMcpToolDef(const sol::table& toolDef, smatchet::lua::McpToolDefinition& out) {
    out.name = toolDef.get_or<std::string>("name", "");
    out.description = toolDef.get_or<std::string>("description", "");

    sol::object params = toolDef["parameters"];
    if (params.is<sol::table>()) {
        out.parametersSchema = LuaToJson(params);
    } else {
        std::string schemaStr = toolDef.get_or<std::string>("parameters_json", "{}");
        // Route through the shared depth/node-bounded parser, NOT a bare
        // nlohmann::json::parse (issue #1287). The parser builds iteratively, so a
        // deeply-nested schema string does not overflow while parsing — but the
        // resulting deep DOM stack-overflows on destruction (`~json` recurses), and a
        // try/catch around json::parse could NOT trap that overflow. ParseBounded
        // aborts the bounded SAX build before such a DOM exists, degrading to an empty
        // schema instead of crashing. schemaStr is same-user input (a setup .lua tool
        // def), so this is defense-in-depth, not a remote-reachable sink.
        // Default 4 MiB byte cap (json_safe::ParseBounded) is intentionally accepted here —
        // it mirrors kMaxDecodeBytes at the decode_json site, and a tool-def schema string is
        // never anywhere near that large.
        std::string parseErr;
        nlohmann::json parsed = smatchet::json_safe::ParseBounded(schemaStr, parseErr);
        if (parseErr.empty()) {
            out.parametersSchema = std::move(parsed);
        } else {
            LOG_DEBUG("Lua MCP tool: parameters_json rejected (%s); using empty schema", parseErr.c_str());
            out.parametersSchema = nlohmann::json::object();
        }
    }
}

void AppController::Impl::LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) {
    if (!toolDef.valid() || !callback.valid()) {
        return;
    }
    smatchet::lua::McpToolDefinition def;
    ParseMcpToolDef(toolDef, def);
    def.callback = sol::protected_function(std::move(callback));

    std::lock_guard<std::mutex> lock(luaMcpToolsMutex_);
    luaMcpTools_.erase(std::remove_if(luaMcpTools_.begin(), luaMcpTools_.end(),
                                      [&](const smatchet::lua::McpToolDefinition& d) { return d.name == def.name; }),
                       luaMcpTools_.end());

    luaMcpTools_.push_back(std::move(def));
}

void AppController::ClearLuaTicketContextGlue() {
    // Clear every container that holds sol::protected_function refs BEFORE nulling the
    // __smatchet_app pointer. RAII reverse-declaration destruction already destroys these
    // containers before `lua` (member-order invariant inside AppController::Impl), but
    // belt-and-suspenders: explicitly drop the handles here so a future re-ordering can't
    // turn this into a UAF. See plan §Shutdown ordering.
    impl_->luaFieldCache_.clear();
    impl_->fieldDisplayCachedProviders_.clear();
    impl_->fieldDisplayCachedProvidersByName_.clear();
    impl_->luaWindows_.clear();
    impl_->pendingLuaWindowOps_.clear();
    impl_->lua["__smatchet_app"] = sol::lua_nil;
    impl_->lua["__smatchet_app_ui"] = sol::lua_nil;
}

void AppController::RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds,
                                  bool processAll) {
    std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
    impl_->automationJobs_.push_back({AutomationJob::Type::RunAutoScript, scriptPath, selectedIds, "", processAll});
    impl_->automationJobCv_.notify_one();
}

void AppController::RunFlatScriptAsync(const std::string& scriptPath) {
    std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
    impl_->automationJobs_.push_back({AutomationJob::Type::RunFlatScript, scriptPath, {}, ""});
    impl_->automationJobCv_.notify_one();
}

void AppController::Impl::PrepareFreshLuaState(sol::state& state) {
    InitLuaCore(state);
    // The shutdown-watchdog hook in RunAutomationJob resolves `__smatchet_app_ui`
    // as AppController::Impl* to read `app_.shuttingDown_`. InitLuaUi is intentionally not run
    // on these off-UI-thread states (no ImGui surface) — UI-mutating bindings stay
    // no-ops here (see smatchet::lua::InitLuaCore) — so set the UI alias directly.
    state["__smatchet_app_ui"] = this;
}

void AppController::Impl::ReplayActiveSetupScripts(sol::state& state, sol::environment& sandbox) {
    // Snapshot activeSetupScripts_ under the same mutex used by RunLuaSetupScript so the
    // iteration below sees a stable view even if the UI thread mutates the vector mid-job.
    std::vector<std::string> setupScriptsSnapshot;
    {
        std::lock_guard<std::mutex> lock(automationJobMutex_);
        setupScriptsSnapshot = activeSetupScripts_;
    }

    // Load and run setup scripts so global actions / mcp.register_tool definitions are
    // present for this job.
    //
    // Lifecycle contract (backlog #34): callers use a *fresh* sol::state per job/call for
    // isolation, so every top-level statement in a setup script re-fires on every job.
    // Setup scripts MUST therefore be defining-only — declaring functions, tables, constants —
    // and avoid side effects at module-load (no `tracker.create_issue(...)` at the top level,
    // no `os.execute(...)` outside of a function body, etc.). Wrap any such side-effecting work
    // in a function that the job explicitly invokes, and the re-execution becomes harmless.
    // Caching compiled sol::function refs across jobs would not change this: bytecode bound to
    // a destroyed state cannot be replayed, and persisting the state across jobs would lose the
    // isolation guarantee.
    for (const auto& path : setupScriptsSnapshot) {
        std::string resolved = app_.ResolveLuaScriptPath(path);
        if (resolved.empty()) {
            continue;
        }
        // Consent gate: re-verify on every replay (worker thread → non-interactive). A setup
        // script whose content changed since approval is refused here even though it was in the
        // active list, so a swapped Scripts/*.lua cannot ride the replay path.
        std::string consentReason;
        if (!app_.IsLuaScriptConsented(resolved, /*interactive=*/false, consentReason)) {
            const std::string bare = "[LUA setup-bg] " + path + ": " + consentReason;
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : app_.errorSinks_) {
                sink(bare);
            }
            app_.scriptingWindowOpenRequested_.store(true);
            continue;
        }
        auto script = state.load_file(resolved);
        if (!script.valid()) {
            sol::error err = script;
            const std::string bare = "[LUA setup-bg] " + path + ": " + err.what();
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : app_.errorSinks_) {
                sink(bare);
            }
            app_.scriptingWindowOpenRequested_.store(true);
            continue;
        }
        sol::protected_function func = script;
        sandbox.set_on(func);
        sol::protected_function_result res = func();
        if (!res.valid()) {
            sol::error err = res;
            const std::string bare = "[LUA setup-bg] " + path + ": " + err.what();
            LuaLogInfoBind(std::string("[ERROR] ") + bare);
            for (const auto& sink : app_.errorSinks_) {
                sink(bare);
            }
            app_.scriptingWindowOpenRequested_.store(true);
        }
    }
}

void AppController::Impl::AutomationWorkerLoop() {
    // Per-iteration try/catch wrapping all sol2/JSON/STL paths. Without this, a single throw
    // (sol::error from a malformed script, std::bad_alloc from a runaway capture, etc.) escapes
    // the thread function and triggers std::terminate. The error is logged and the worker
    // continues serving the next job — same liveness contract as a UI-thread exception handler.
    while (true) {
        AppController::AutomationJob job;
        {
            std::unique_lock<std::mutex> lock(automationJobMutex_);
            automationJobCv_.wait(lock, [this]() {
                return app_.shuttingDown_.load() || automationWorkerShuttingDown_.load() || !automationJobs_.empty();
            });
            if (app_.shuttingDown_.load() || automationWorkerShuttingDown_.load()) {
                break;
            }
            job = std::move(automationJobs_.front());
            automationJobs_.pop_front();
        }

        try {
            // Fresh per-job sol::state for isolation — see PrepareFreshLuaState /
            // ReplayActiveSetupScripts. Same pattern the MCP run_lua / tool handlers use.
            sol::state bgState;
            PrepareFreshLuaState(bgState);
            sol::environment sandbox = CreateSandboxEnvironment(bgState);
            ReplayActiveSetupScripts(bgState, sandbox);
            RunAutomationJob(bgState, sandbox, job);
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::AutomationWorkerLoop: exception escaped job '%s': %s",
                      job.scriptPathOrActionName.c_str(), ex.what());
        } catch (...) {
            LOG_ERROR("AppController::AutomationWorkerLoop: unknown exception escaped job '%s'",
                      job.scriptPathOrActionName.c_str());
        }
    }
    // Signal a clean loop exit so the dtor's bounded shutdown wait can distinguish
    // "worker finished" from "worker still stuck in blocking glue" (see
    // automationWorkerExited_ in AppControllerImpl.h). Set BEFORE the thread
    // function returns; the subsequent join in the dtor is the happens-before
    // barrier, so a relaxed-visible store here is observed there.
    automationWorkerExited_.store(true);
    automationJobCv_.notify_all();
}

// Automation count-hook tuning (security finding #13). The hook fires every kHookCountInterval
// Lua instructions; kAutomationInstructionBudget caps a single automation job so a runaway pure-Lua
// loop self-aborts even with no shutdown signal (exposure B). Budget chosen to allow substantial
// automation (~5e8 instructions ≈ 10000 hook ticks) while still terminating a true infinite loop.
// The abort/keep-running decision is the pure LuaAutomationHookPolicyPure predicate (unit-tested).
namespace {
constexpr int kHookCountInterval = 50000;
constexpr unsigned long long kAutomationInstructionBudget = 500000000ULL;

// Per-job accumulated instruction count. The worker is single-threaded and runs one job at a
// time on a fresh sol::state; reset in RunAutomationJob before each job. thread_local (not a
// member) so the count-hook — a plain C function pointer with no closure — can reach it.
thread_local unsigned long long g_automationInstructionsElapsed = 0ULL;
} // namespace

void AppController::Impl::RunAutomationJob(sol::state& state, sol::environment& env,
                                           const AppController::AutomationJob& job) {
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);

    g_automationInstructionsElapsed = 0ULL;
    lua_sethook(
        state.lua_state(),
        [](lua_State* L, lua_Debug* /*ar*/) {
            g_automationInstructionsElapsed += static_cast<unsigned long long>(kHookCountInterval);
            sol::state_view sv(L);
            // `__smatchet_app_ui` is the AppController::Impl* alias (see ResolveApp comment).
            // The Core `__smatchet_app` now holds an `ILuaBindingHost*`; resolving it
            // as Impl* would corrupt under multiple inheritance.
            const sol::object appObj = sv["__smatchet_app_ui"];
            AppController::Impl* app = nullptr;
            if (appObj.valid() && appObj.get_type() != sol::type::lua_nil) {
                app = appObj.as<AppController::Impl*>();
            }
            // Shutdown abort must observe automationWorkerShuttingDown_ — it is raised BEFORE the
            // dtor's blocking automationWorker_.join(), whereas shuttingDown_ is set only AFTER the
            // join, so checking shuttingDown_ alone never released a long job during exit (#13 A).
            const bool shuttingDown = app && app->app_.shuttingDown_.load();
            const bool workerShuttingDown = app && app->automationWorkerShuttingDown_.load();
            const LuaAutomationHookPolicyPure::AbortReason reason = LuaAutomationHookPolicyPure::DecideAutomationAbort(
                shuttingDown, workerShuttingDown, g_automationInstructionsElapsed, kAutomationInstructionBudget);
            if (reason != LuaAutomationHookPolicyPure::AbortReason::kNone) {
                luaL_error(L, "%s", LuaAutomationHookPolicyPure::AbortReasonMessage(reason));
            }
        },
        LUA_MASKCOUNT, kHookCountInterval);

    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string bare = std::string(prefix) + detail;
        // Route through the normal info sink so the console shows it, but also
        // through dedicated error sinks (persistent error panel + window-open).
        LuaLogInfoBind(std::string("[ERROR] ") + bare);
        for (const auto& sink : app_.errorSinks_) {
            sink(bare);
        }
        app_.scriptingWindowOpenRequested_.store(true);
    };

    if (job.type == AutomationJob::Type::RunAutoScript) {
        RunAutomationAutoScript(state, job, logErr);
    } else if (job.type == AutomationJob::Type::TicketAction) {
        RunAutomationActionCall(env, job, true, logErr);
    } else if (job.type == AutomationJob::Type::GlobalAction) {
        RunAutomationActionCall(env, job, false, logErr);
    } else if (job.type == AutomationJob::Type::RunFlatScript) {
        RunAutomationFlatScript(state, job, logErr);
    }

    lua_sethook(state.lua_state(), nullptr, 0, 0);
}

void AppController::Impl::RunAutomationAutoScript(sol::state& state, const AppController::AutomationJob& job,
                                                  const AutomationErrorSink& logErr) {
    const std::string path = app_.ResolveLuaScriptPath(job.scriptPathOrActionName);
    if (path.empty()) {
        logErr("[LUA auto] ", "invalid script path");
        return;
    }
    std::string consentReason;
    if (!app_.IsLuaScriptConsented(path, /*interactive=*/false, consentReason)) {
        logErr("[LUA auto] ", consentReason);
        return;
    }

    sol::load_result script = state.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        logErr("[LUA auto] ", err.what());
        return;
    }

    sol::protected_function func = script;
    sol::environment runEnv = CreateSandboxEnvironment(state);
    runEnv.set_on(func);

    sol::protected_function_result init_pfr = func();
    if (!init_pfr.valid()) {
        sol::error err = init_pfr;
        logErr("[LUA auto] ", err.what());
        return;
    }

    sol::protected_function process_func = runEnv["process_ticket"];
    if (!process_func.valid()) {
        logErr("[LUA auto] ", "script must define function process_ticket(ticket)");
        return;
    }

    const auto snap = app_.GetActiveTicketsSnapshot();
    std::unordered_set<std::string> selectedSet(job.selectedIds.begin(), job.selectedIds.end());

    // Issue #824: an empty selection must require explicit intent. Without process_all we refuse
    // to run — never a silent mass-modify, never a silent no-op. With process_all set, the
    // selection filter is bypassed and every loaded ticket is processed.
    if (selectedSet.empty() && !job.processAll) {
        logErr("[LUA auto] ", "empty selection and process_all not set — refusing to run; "
                              "pass process_all=true to run across all loaded tickets");
        return;
    }

    for (auto& ticket : *snap) {
        // processAll bypasses the selection filter and runs across every ticket in the snapshot.
        if (!job.processAll && selectedSet.find(ticket.id) == selectedSet.end()) {
            continue;
        }

        // Copy ticket so we don't modify the snapshot elements in-place directly without protection
        CachedTicket ticketCopy = ticket;
        sol::protected_function_result pfr = process_func(&ticketCopy);
        if (!pfr.valid()) {
            sol::error err = pfr;
            logErr("[LUA auto] ", err.what());
        }
    }
}

void AppController::Impl::RunAutomationActionCall(sol::environment& env, const AppController::AutomationJob& job,
                                                  bool passTargetId, const AutomationErrorSink& logErr) {
    sol::protected_function func = env[job.scriptPathOrActionName];
    if (!func.valid()) {
        logErr("[LUA action] ", "Function not found: " + job.scriptPathOrActionName);
        return;
    }
    sol::protected_function_result pfr = passTargetId ? func(job.targetIssueId) : func();
    if (!pfr.valid()) {
        sol::error err = pfr;
        logErr("[LUA action] ", err.what());
    }
}

void AppController::Impl::RunAutomationFlatScript(sol::state& state, const AppController::AutomationJob& job,
                                                  const AutomationErrorSink& logErr) {
    const std::string path = app_.ResolveLuaScriptPath(job.scriptPathOrActionName);
    if (path.empty()) {
        logErr("[LUA run] ", "invalid script path");
        return;
    }
    std::string consentReason;
    if (!app_.IsLuaScriptConsented(path, /*interactive=*/false, consentReason)) {
        logErr("[LUA run] ", consentReason);
        return;
    }

    sol::load_result script = state.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        logErr("[LUA run] ", err.what());
        return;
    }

    sol::protected_function func = script;
    sol::environment runEnv = CreateSandboxEnvironment(state);
    runEnv.set_on(func);

    sol::protected_function_result pfr = func();
    if (!pfr.valid()) {
        sol::error err = pfr;
        logErr("[LUA run] ", err.what());
    } else {
        LOG_TRACE("RunAutomationJob: flat script finished.");
    }
}

void AppController::RunLuaSetupScript(const std::string& scriptPath) {
    auto logErr = [this](const char* prefix, const std::string& detail) {
        const std::string bare = std::string(prefix) + detail;
        const std::string decorated = std::string("[ERROR] ") + bare;
        if (impl_->luaHost_ && !impl_->luaHost_->SnapshotLogSinks().empty()) {
            for (const auto& sink : impl_->luaHost_->SnapshotLogSinks()) {
                sink(decorated);
            }
        } else {
            LOG_ERROR("%s", decorated.c_str());
        }
        for (const auto& sink : errorSinks_) {
            sink(bare);
        }
        scriptingWindowOpenRequested_.store(true);
    };

    const std::string path = ResolveLuaScriptPath(scriptPath);
    if (path.empty()) {
        logErr("[LUA setup] ", "invalid script path");
        return;
    }
    // Consent gate BEFORE registering the script as an active setup script — a blocked script
    // must not enter activeSetupScripts_ (it would otherwise be retried on every worker job).
    // UI-thread / user-triggered path → interactive, so the scripting window is surfaced.
    {
        std::string consentReason;
        if (!IsLuaScriptConsented(path, /*interactive=*/true, consentReason)) {
            logErr("[LUA setup] ", consentReason);
            return;
        }
    }

    // activeSetupScripts_ is read by AutomationWorkerLoop on the worker thread — every mutation
    // must take automationJobMutex_ so the worker's snapshot copy sees a consistent vector.
    {
        std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);
        if (std::find(impl_->activeSetupScripts_.begin(), impl_->activeSetupScripts_.end(), scriptPath) ==
            impl_->activeSetupScripts_.end()) {
            impl_->activeSetupScripts_.push_back(scriptPath);
        }
    }

    LOG_TRACE("RunLuaSetupScript: begin path=%s scriptPath=%s", path.c_str(), scriptPath.c_str());
    FieldEditAuditSource::ScopedOverride luaSource(FieldEditAuditSource::kLua);

    sol::state& lua = impl_->lua; // pImpl #19b: the sol::state member now lives in AppController::Impl
    sol::environment sandbox = CreateSandboxEnvironment(lua);
    sol::load_result script = lua.load_file(path);
    if (!script.valid()) {
        sol::error err = script;
        logErr("[LUA setup] ", err.what());
        LOG_TRACE("RunLuaSetupScript: load_error path=%s %s", path.c_str(), err.what());
        return;
    }
    sol::protected_function func = script;
    sandbox.set_on(func);

    lua_sethook(
        lua.lua_state(), [](lua_State* L, lua_Debug* /*ar*/) { luaL_error(L, "Script execution timeout exceeded."); },
        LUA_MASKCOUNT, 100000);

    auto res = func();

    lua_sethook(lua.lua_state(), nullptr, 0, 0);

    if (!res.valid()) {
        sol::error err = res;
        logErr("[LUA setup] ", err.what());
        LOG_TRACE("RunLuaSetupScript: failed path=%s", path.c_str());
    } else {
        LOG_TRACE("RunLuaSetupScript: ok path=%s", path.c_str());
    }
}
