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

namespace smatchet_lua_init_detail {

// --- AI assistant glues (Phase E) ----------------------------------------------
// Resolve via `__smatchet_app_ui` (AppController*) and call the always-on stubs
// `AddAiContext` / `ClearAiContext` / `PromptAi` shipped Phase B. Those stubs
// no-op when `SMATCHET_WITH_AI=0`, so the glues need no extra gating here.
//
// **UI-thread-only by construction, not by a runtime guard** (Issue #1678 /
// agent-audit finding B5 — investigated and closed as a confirmed-safe
// invariant, now hardened + tested rather than left implicit). The Lua global
// `ai` is bound HERE, inside `InitLuaUi` — never inside `InitLuaCore`. Every
// off-UI-thread Lua state (the background automation worker's per-job
// `bgState` built by `AutomationWorkerLoop`, and the MCP `run_lua` /
// registered-tool fresh states from `ExecuteLuaSnippetForMcp` /
// `ExecuteLuaMcpTool`) is built via `PrepareFreshLuaState`, which calls
// `InitLuaCore` ONLY (see that function's comment for why `InitLuaUi` is
// intentionally skipped there — no ImGui surface off the UI thread). So a
// worker/MCP script referencing the global `ai` sees `nil` and errors out
// ("attempt to index a nil value") before ever reaching these glues —
// `ResolveApp` / `AiAssistantController::Submit` etc. are never invoked off
// the UI thread. Same isolation pattern `tests/ui/mcp_lua_fresh_state_race.
// test.cpp` proved for the MCP run_lua path; the table-absence half of that
// contract is locked in by `tests/Lua/LuaBindings.test.cpp`
// ("ai.* table is absent without InitLuaUi (off-UI-thread contract)") so a
// future refactor that moves `ai` registration into `InitLuaCore` fails a
// test instead of silently reopening this race. If that ever becomes
// necessary, `ai.*` would need an explicit hop through
// `MainThreadDispatcher::PostToMainThread` — see the `RunOnUiThreadAsCommandResult`
// precedent used by `Commands/AppViewCommands.cpp` / `Commands/PaneCommands.cpp`
// / `Commands/Builtin/BuiltinCommands_Debug.cpp` — or `luaL_error`-ing stubs on
// non-UI states.

// Build an `AiContextBlock` from a Lua table { name=string, body=string,
// kind=("active_ticket"|"multi_selected_tickets"|"visible_grid_rows"|
//        "active_view"|"audit_trail") }. Missing/invalid kind defaults to
// `ActiveTicket` (matches `AiContextBlock` default ctor).
static AiContextBlock LuaTableToAiContextBlock(const sol::table& tbl) {
    AiContextBlock block;
    const sol::object nameObj = tbl["name"];
    if (nameObj.valid() && nameObj.is<std::string>()) {
        block.Name = nameObj.as<std::string>();
    }
    const sol::object bodyObj = tbl["body"];
    if (bodyObj.valid() && bodyObj.is<std::string>()) {
        block.Body = bodyObj.as<std::string>();
    }
    const sol::object kindObj = tbl["kind"];
    if (kindObj.valid() && kindObj.is<std::string>()) {
        const std::string k = kindObj.as<std::string>();
        if (k == "multi_selected_tickets") {
            block.Kind = AiContextBlockKind::MultiSelectedTickets;
        } else if (k == "visible_grid_rows") {
            block.Kind = AiContextBlockKind::VisibleGridRows;
        } else if (k == "active_view") {
            block.Kind = AiContextBlockKind::ActiveView;
        } else if (k == "audit_trail") {
            block.Kind = AiContextBlockKind::AuditTrail;
        } else {
            block.Kind = AiContextBlockKind::ActiveTicket; // also covers "active_ticket"
        }
    }
    return block;
}

void LuaAiAddContextGlue(sol::this_state L, sol::table blockTbl) {
    AppController::Impl* app = ResolveApp(L);
    if (!app)
        return;
    app->app_.AddAiContext(LuaTableToAiContextBlock(blockTbl));
}

void LuaAiClearContextGlue(sol::this_state L) {
    AppController::Impl* app = ResolveApp(L);
    if (app)
        app->app_.ClearAiContext();
}

void LuaAiPromptGlue(sol::this_state L, const std::string& prompt, sol::optional<sol::table> extraBlocks) {
    AppController::Impl* app = ResolveApp(L);
    if (!app)
        return;
    // Rate-limit + consent gate (security audit H5 / E6). The instruction-count
    // lua_sethook does NOT cover the outbound HTTP this kicks off, so reject a
    // re-entrant or <5 s-spaced call BEFORE any context mutation / submit, and
    // fire the one-time consent toast on the first accepted call. luaL_error
    // raises a Lua error (caught by the protected call) rather than blocking the
    // UI thread — no sleep/spin.
    std::string gateError;
    if (!app->TryBeginLuaAiPromptTurn(gateError)) {
        luaL_error(L, "%s", gateError.c_str());
        return;
    }
    // Optional extra context blocks: appended to the controller's context vector
    // before Submit, matching the panel's "Send-with-context" path. Each element
    // is treated as an `AiContextBlock` table.
    if (extraBlocks) {
        sol::table arr = extraBlocks.value();
        for (std::size_t i = 1;; ++i) {
            const sol::object el = arr[i];
            if (!el.valid() || el.get_type() == sol::type::lua_nil)
                break;
            if (!el.is<sol::table>())
                break;
            app->app_.AddAiContext(LuaTableToAiContextBlock(el.as<sol::table>()));
        }
    }
    app->app_.PromptAi(prompt);
    // Submit() hands the turn to the AI worker thread; the synchronous glue work
    // is done, so release the in-flight slot. The 5 s spacing rule (stamped at
    // TryBegin) now guards the next call. Re-entrancy is still blocked for the
    // duration of THIS call (a context-builder that re-entered ai.prompt would
    // hit the in-flight reject above).
    app->EndLuaAiPromptTurn();
}

} // namespace smatchet_lua_init_detail

bool AppController::Impl::TryBeginLuaAiPromptTurn(std::string& outError) {
    std::lock_guard<std::mutex> lk(aiPromptGateMutex_);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::int64_t lastMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(aiPromptLastCallAt_.time_since_epoch()).count();

    const smatchet::ai::AiPromptGateDecision decision =
        smatchet::ai::DecideAiPromptGate(aiPromptInFlight_, aiPromptEverCalled_, lastMs, nowMs);
    if (decision == smatchet::ai::AiPromptGateDecision::RejectReentrant) {
        outError = "ai.prompt rejected: a previous prompt is still in flight (re-entrant call blocked)";
        return false;
    }
    if (decision == smatchet::ai::AiPromptGateDecision::RejectTooSoon) {
        outError = "ai.prompt rejected: rate limit — wait at least 5 s between prompts";
        return false;
    }

    // Accepted — claim the in-flight slot + stamp the timestamp under the lock.
    aiPromptInFlight_ = true;
    aiPromptLastCallAt_ = now;
    aiPromptEverCalled_ = true;

    // One-time-per-session consent toast naming the outbound provider host, so a
    // user who pasted-and-ran a script knows ai.prompt just reached off-host.
    if (!aiPromptConsentShown_) {
        aiPromptConsentShown_ = true;
        std::string provider = "the configured AI provider";
#if defined(SMATCHET_WITH_AI)
        if (aiAssistant_) {
            const std::string name = aiAssistant_->GetActiveProviderName();
            if (!name.empty()) {
                provider = name;
            }
        }
#endif
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.ai_prompt_from_lua", "AI prompt from Lua"),
            SmatchetLocalization::Format("toast.ai_prompt_from_lua_body",
                                         "A Lua script called ai.prompt — sending your AI context to %s.",
                                         provider.c_str()),
            ToastType::Warning, 8000);
        LOG_INFO("[LUA] ai.prompt invoked from Lua for the first time this session (provider=%s)", provider.c_str());
    }
    return true;
}

void AppController::Impl::EndLuaAiPromptTurn() {
    std::lock_guard<std::mutex> lk(aiPromptGateMutex_);
    aiPromptInFlight_ = false;
}

