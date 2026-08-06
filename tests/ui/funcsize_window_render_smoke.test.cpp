// funcsize_window_render_smoke.test.cpp — Phase-0 pilot of the
// full-function-size-compliance program (docs/plans/shipped/full-function-size-compliance.md).
//
// PURPOSE — STRUCTURAL coverage for ImGui window-draw functions slated for
// decomposition in Phase 5. Each test boots the REAL app (via UiTestScenario),
// opens a target top-level window so its REAL draw function ticks every frame,
// waits for the window to become visible, and asserts an expected child widget
// is present. There is deliberately NO checked-in pixel golden — a golden would
// trip the approve-golden user-pause this program is trying to avoid. The
// coverage is the live tick: if a future decomposition introduces a Begin/End
// or PushID/PopID imbalance, the ImGui Test Engine traps the assertion inside
// the running frame loop and the test fails.
//
// REUSABLE PATTERN (copy this for the remaining window-draw functions):
//   1. boot-check     — SmatchetActiveUiTestAppController(); skip-with-log if null.
//   2. open-window    — set the g_ui visibility flag the View menu / view.toggle
//                       command would flip (e.g. g_ui.showLogWindow = true).
//                       This is exactly what ViewToggleCommands.cpp does
//                       internally, so it drives the production code path.
//   3. tick-until     — SetRef(<window title>) + YieldUntil(ImGui::FindWindowByName(title)
//                       is non-null and ->Active). Use FindWindowByName, NOT
//                       ctx->WindowInfo() — WindowInfo() does not resolve a docked
//                       window by plain title (see the inline note below). Re-arm the
//                       request-focus flag every yielded frame or the window opens as an
//                       inactive tab where Begin() returns false. The real draw fn runs
//                       each yielded frame.
//   4. assert-widget  — ctx->ItemExists("<robust label>") with a BARE ref (no
//                       "<window>/" prefix — it double-prefixes against SetRef); IM_CHECK it.
//   5. close-window   — clear the flag so the next test starts clean.
//
// Windows covered in this pilot (all three open with ZERO backend state):
//   - Log            (drawLogWindow,   SmatchetUtilityWindowsUi.cpp) — title "Log"
//   - Backend Audit  (drawAuditWindow, SmatchetAuditUi.cpp)         — title "Backend Audit"
//   - Preferences    (drawPreferencesWindow, SmatchetPreferencesUi.cpp) — title "Preferences"

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// g_ui — the shared bag of UI-thread visibility flags. ViewToggleCommands.cpp
// flips these same fields to open/close panels; we set them directly here to
// avoid threading a command dispatch through the test.
extern UiDrawSession g_ui;

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
// Returns true if the predicate became true within the budget.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True once a top-level window with `title` exists and is Active (i.e. Begin()
// returned true this frame, so its children were submitted). FindWindowByName
// is the probe the existing real-window bucket-E tests use
// (ai_assistant_panel_dock_swap.test.cpp:134) — it resolves docked windows by
// their plain title where ctx->WindowInfo() does not, and silently returns
// nullptr on a miss so the polling loop doesn't mark the context errored.
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// Shared body: boot-check, open via flag, tick until visible, assert a robust
// child widget exists, then close. `openFlag` points at the g_ui visibility
// bool for the target window; `focusFlag` points at the matching focus-request
// bool the View menu sets so the window's draw fn calls SetNextWindowFocus().
//
// Why focusFlag matters: these top-level windows dock into a default slot via
// prepareTopLevelWindow(). When the slot already hosts another default window,
// the target opens as an INACTIVE tab — Begin() returns false, no children are
// submitted, and WindowInfo()/ItemExists() never resolve. Re-arming the
// focus-request flag every frame until the window is live activates the tab
// (the draw fn consumes the flag in one frame, so we re-arm in the predicate),
// exactly mirroring how the View-menu open path brings a docked panel forward.
void RunWindowRenderSmoke(ImGuiTestContext* ctx, bool UiDrawSession::* openFlag, bool UiDrawSession::* focusFlag,
                          const char* windowTitle, const char* childItemRef) {
    const AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo(
            "SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted under UiTestScenario");
        return;
    }

    // Open + focus the window the same way the View menu / view.toggle command
    // does (ViewToggleCommands.cpp flips the same flags).
    g_ui.*openFlag = true;
    g_ui.*focusFlag = true;

    ctx->SetRef(windowTitle);
    const bool visible = YieldUntil(ctx, [&] {
        // Re-arm focus each frame until the docked tab activates and the
        // window resolves; the draw fn clears the flag once it has consumed it.
        g_ui.*focusFlag = true;
        return WindowIsLive(windowTitle);
    });
    IM_CHECK_NO_RET(visible);

    // The real draw function has now ticked for several frames. Assert an
    // expected child widget exists — this confirms the body ran past Begin()
    // and submitted its toolbar. `childItemRef` is resolved relative to the
    // SetRef(windowTitle) above, so pass the bare item label (not a
    // "Window/Item" path — that would double-prefix against the ref).
    // ItemExists also implicitly validates no ImGui assertion fired mid-draw,
    // since the engine would have trapped it.
    if (visible) {
        const bool itemPresent = ctx->ItemExists(childItemRef);
        IM_CHECK_NO_RET(itemPresent);
    }

    // Close so the next test starts from a clean visibility state.
    g_ui.*openFlag = false;
    ctx->Yield();
}

// --- Log window ----------------------------------------------------------
// drawLogWindow (SmatchetUtilityWindowsUi.cpp). Title "Log". The "Clear Log"
// button is a stable, non-localized toolbar label submitted unconditionally
// once Begin() succeeds.
void RegisterLogWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeWindowRender", "LogWindow_RendersAndShowsClearButton");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, "Log", "Clear Log");
    };
}

// --- Backend Audit window ------------------------------------------------
// drawAuditWindow (SmatchetAuditUi.cpp). Title "Backend Audit". The "Refresh"
// button is a stable toolbar label submitted unconditionally once Begin()
// succeeds (it reads cached rows, so no live backend is required).
void RegisterAuditWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeWindowRender", "AuditWindow_RendersAndShowsRefreshButton");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showAuditTrail, &UiDrawSession::requestAuditTrailFocus,
                             "Backend Audit", "Refresh");
    };
}

// --- Preferences window --------------------------------------------------
// drawPreferencesWindow (SmatchetPreferencesUi.cpp). Title "Preferences".
//
// Probe: the "##PrefsSearch" settings-search InputText, submitted
// unconditionally right after the window's buffers load — before the nav rail /
// category dispatch — so it exists in both the rail and the narrow-width combo
// nav modes. The "Save & Sync" footer button is NOT a stable probe:
// SmatchetIconLeadingButton composes "<glyph> <translated>##Save & Sync" and
// ImGui seeds the item ID from the WHOLE string (`##` truncates the display but
// does NOT reset the ID hash — only `###` does, per ImHashStr), so the ID is
// glyph- and locale-dependent. "##PrefsSearch" is a constant literal — its hash
// is locale-independent. Reaching the search box confirms the body ran past
// Begin() and submitted its content, needing no backend state.
void RegisterPreferencesWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeWindowRender", "PreferencesWindow_RendersAndShowsSaveButton");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showPreferences, &UiDrawSession::requestPreferencesFocus,
                             "Preferences", "##PrefsSearch");
    };
}

#if defined(SMATCHET_WITH_MCP)
// --- MCP Server window ---------------------------------------------------
// SmatchetDrawMcpServerPanel (SmatchetMcpServerUi.cpp), drawn by
// SmatchetDrawMcpServerWindow. Title "MCP Server". Docked window, so the
// focus re-arm is required exactly like Log/Audit/Preferences. The panel body
// renders unconditionally (it reads the in-process plugin-host status, which
// is valid with no tracker backend); "##mcp_panel_split" is an unconditional
// InvisibleButton with a stable ID — the body has no labelled button/checkbox,
// so this is the robust child probe. Guarded on SMATCHET_WITH_MCP because the
// showMcpServerWindow flag only exists in that build.
void RegisterMcpServerWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeWindowRender", "McpServerWindow_RendersAndShowsPanelSplit");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showMcpServerWindow, &UiDrawSession::requestMcpServerFocus,
                             "MCP Server", "##mcp_panel_split");
    };
}
#endif // SMATCHET_WITH_MCP

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
// --- Scripting (Lua console) window --------------------------------------
// LuaConsolePlugin::OnDraw (Source/Plugins/LuaConsole/LuaConsolePlugin.cpp).
// Title "Scripting". Docked window opened via g_ui.showLuaAutomationWindow +
// g_ui.requestLuaAutomationFocus, so it reuses the focus-re-arm recipe exactly
// like Log/Audit/Preferences/MCP. Once Begin() succeeds the plugin submits the
// "##lua_script_pane" child unconditionally (it hosts the Scripts/Tools tab
// bar), so that child's stable ID is the robust probe — the body has no
// always-present labelled button at the top. Renders with ZERO backend (it
// lists scripts from disk, which is empty/absent under the test harness without
// short-circuiting the draw). Guarded on SMATCHET_WITH_LUA_AUTOMATION because
// the showLuaAutomationWindow flag only exists in that build.
void RegisterScriptingWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeWindowRender", "ScriptingWindow_RendersAndShowsScriptPane");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showLuaAutomationWindow, &UiDrawSession::requestLuaAutomationFocus,
                             "Scripting", "##lua_script_pane");
    };
}
#endif // SMATCHET_WITH_LUA_AUTOMATION

// --- Bug Report window ---------------------------------------------------
// SmatchetBugReportUi_Draw (SmatchetBugReportUi.cpp). Title "Report a Bug".
// Unlike the docked windows above this is a plain FLOATING Begin() (no docking
// prep, no requestXFocus latch) — so Begin() returns true on the first frame
// and there is no inactive-tab problem to defeat. It opens via showBugReport +
// the one-frame bugReportOpenLatch (the latch seeds the description buffer on
// first paint). The "Attach screenshot (text redacted)" checkbox is submitted
// unconditionally once Begin() succeeds and needs no backend. Because the open
// recipe differs (latch, not focus flag), this test does not reuse
// RunWindowRenderSmoke.
void RegisterBugReportWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeWindowRender", "BugReportWindow_RendersAndShowsAttachCheckbox");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        g_ui.showBugReport = true;
        g_ui.bugReportOpenLatch = true;
        ctx->SetRef("Report a Bug");
        const bool visible = YieldUntil(ctx, [&] { return WindowIsLive("Report a Bug"); });
        IM_CHECK_NO_RET(visible);

        if (visible) {
            const bool itemPresent = ctx->ItemExists("Attach screenshot (text redacted)");
            IM_CHECK_NO_RET(itemPresent);
        }

        g_ui.showBugReport = false;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterFuncSizeWindowRenderSmokeTests(ImGuiTestEngine* engine) {
    RegisterLogWindowRenderSmoke(engine);
    RegisterAuditWindowRenderSmoke(engine);
    RegisterPreferencesWindowRenderSmoke(engine);
#if defined(SMATCHET_WITH_MCP)
    RegisterMcpServerWindowRenderSmoke(engine);
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    RegisterScriptingWindowRenderSmoke(engine);
#endif
    RegisterBugReportWindowRenderSmoke(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
