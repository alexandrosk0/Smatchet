// omnibar_search_apply.test.cpp — bucket-E (ImGui Test Engine) coverage for the
// global Chrome-omnibox search bar's Enter-apply path (jql-omnibox plan, Stream B,
// slices 2b/2c). The bucket-A suite (tests/Core/OmnibarInputClassifier.test.cpp)
// already locks the pure Jql|TicketKey|TitleSearch *classification*; this TU closes
// the gap that classifier coverage cannot reach — that the live omnibar widget,
// drawn every frame by SmatchetUI::Draw, actually ROUTES a committed Enter to the
// right per-pane side effect for each of the three kinds.
//
// WHAT THIS LOCKS IN — typing into the real "##JQLEmbedded" input inside the
// "##SmatchetOmnibar" side-bar and pressing Enter:
//   1. TitleSearch (plain words) → fills the FOCUSED pane's grid filter box
//      (GridPane::gridFilterBuf), never mutating a saved view.
//   2. Jql (a query carrying an operator) → mirrors into cfg.JqlQuery AND the
//      dashboard editor buffer (the applyQueryToPaneView shared-core lock-step).
//   3. TicketKey (a bare key for the backend) → selects the already-loaded row in
//      the focused pane (SpreadsheetState::ActiveIssueId), the no-network jump.
//
// HOW IT REACHES PRODUCTION — there is no standalone omnibar widget to instantiate
// (drawOmnibar is a private SmatchetUI method); instead it drives the LIVE bar the
// booted app already renders, observing the result through the live UiDrawSession
// singleton `g_ui` (the same idiom as grid_pane_windows.test.cpp). The engine types
// into the production InputText and presses Enter; the production
// TrackerQueryAcp_InputTextCallback sets jqlWantsApplyFromEnter (proven to fire with
// no autocomplete row highlighted — ResolveAcpCommit, selected == -1), drawOmnibar
// then calls applyOmnibarEnter → the classified branch. No mock of the classifier.
//
// WHY THE ENTER ALWAYS APPLIES — the test never arrows the suggestion list, so
// jqlAcpListSelected stays -1; ResolveAcpCommit(n, -1, enter) yields
// wantsApplyFromEnter whether or not suggestion rows exist. Each variant clears the
// shared per-pane omnibar buffer first (via the production "x" clear button) because
// the buffer is session state that persists across sibling tests.
//
// Fixture-gated like every data-dependent bucket-E test: the deterministic Jira
// backend (SMATCHET_TEST_JIRA_BACKEND_FIXTURE → SMAT-1/SMAT-2) gives a loaded
// focused pane; without it the tests SKIP.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "GridPane.h"                          // GridPane, CachedTicket, gridFilterBuf, gridState
#include "SmatchetUiSession.h"                 // UiDrawSession, focusedPane(), omniJqlEditor, cfg

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace {

// Pump the test engine up to maxFrames, returning true as soon as pred() holds (false on timeout)
// — the bucket-E idiom for waiting on async UI / session state without a fixed sleep.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True when an ImGui window named `title` exists and is being drawn this frame (submitted + active).
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// True when the deterministic Jira backend fixture is injected (SMATCHET_TEST_JIRA_BACKEND_FIXTURE).
// Gates the suite so CI's fixture-less `--all` lane skip-passes these tests instead of failing.
bool FixtureEnvSet() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const bool set = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE") != nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return set;
}

// True once the focused pane's rendered snapshot carries `key`. The ticket-jump
// only takes its no-network in-grid branch (SetActiveIssue) when the row is
// already loaded; gate on this so Enter never falls through to OpenUrl (which
// would try to launch a browser in the headless harness).
bool FocusedSnapshotHasKey(const std::string& key) {
    const std::shared_ptr<const std::vector<CachedTicket>>& rows = g_ui.focusedPane().ticketsSnapshot;
    if (!rows) {
        return false;
    }
    for (const CachedTicket& ticket : *rows) {
        if (ticket.id == key) {
            return true;
        }
    }
    return false;
}

const char* kPrimaryWindow = "Smatchet - Active Project";
const char* kOmnibarWindow = "##SmatchetOmnibar";

// Sync the deterministic backend, bring the primary grid pane live + focused so the
// focused pane resolves to a real loaded pane ("main"). Returns false (after a
// logged skip/check) when the backend never settled.
bool BootSyncFocusPrimary(ImGuiTestContext* ctx, AppController* app) {
    app->SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
    IM_CHECK_NO_RET(syncDone);
    // Past the fixture gate the deterministic backend MUST yield rows — a completed-but-empty
    // sync is a real failure, not a silent skip. Hard-assert it (mirroring
    // grid_pane_windows.test.cpp:85) so the three tests can't pass green while exercising nothing.
    const bool haveTickets = syncDone && !app->GetActiveTickets().empty();
    IM_CHECK_NO_RET(haveTickets);
    if (!haveTickets) {
        return false;
    }
    const bool primaryLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kPrimaryWindow);
    });
    IM_CHECK_NO_RET(primaryLive);
    IM_CHECK_NO_RET(g_ui.gridPanesLoaded); // a live primary should imply panes loaded — fail loudly if not
    return primaryLive && g_ui.gridPanesLoaded;
}

// Replace the omnibar's text with `text`, leaving its input active. Clears any text
// a sibling variant left behind via the production "x##ClearQueryEmbedded" button
// (the shared d.omniJqlEditor buffer is per-pane session state), then focuses the
// embedded JQL input and types the new query.
void SetOmnibarQuery(ImGuiTestContext* ctx, const char* text) {
    ctx->SetRef(kOmnibarWindow);
    ctx->ItemClick("x##ClearQueryEmbedded");
    ctx->Yield();
    ctx->ItemInput("##JQLEmbedded");
    ctx->KeyChars(text);
    ctx->Yield();
    ctx->Yield();
}

// Shared per-variant preamble: fixture gate, app gate, sync/focus, omnibar-live.
// Returns the booted app, or nullptr after a logged skip/check (caller returns).
AppController* PrepareOmnibar(ImGuiTestContext* ctx) {
    if (!FixtureEnvSet()) {
        ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent");
        return nullptr;
    }
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return nullptr;
    }
    if (!BootSyncFocusPrimary(ctx, app)) {
        return nullptr;
    }
    // The omnibar is drawn unconditionally in the desktop Draw path — it must be live.
    const bool omnibarLive = YieldUntil(ctx, [] { return WindowIsLive(kOmnibarWindow); });
    IM_CHECK_NO_RET(omnibarLive);
    if (!omnibarLive) {
        return nullptr;
    }
    return app;
}

// Case 1/3 — TitleSearch: typing plain words + Enter must fill the focused pane's grid
// filter box (gridFilterBuf) and never mutate a saved view.
void RegisterTitleSearchFillsGridFilter(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Omnibar", "TitleSearch_FillsFocusedPaneGridFilter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareOmnibar(ctx);
        if (app == nullptr) {
            return;
        }

        // Clear the pane's grid filter so the post-Enter assertion can't pass vacuously.
        g_ui.focusedPane().gridFilterBuf[0] = '\0';

        const char* kQuery = "zqxomni probe"; // plain words: no operator / colon / key shape → TitleSearch
        SetOmnibarQuery(ctx, kQuery);
        IM_CHECK_STR_EQ(g_ui.omniJqlEditor.buf, kQuery); // typed text reached the live omnibar buffer

        ctx->KeyPress(ImGuiKey_Enter);
        const bool filled = YieldUntil(ctx, [&] { return std::string(g_ui.focusedPane().gridFilterBuf) == kQuery; });
        IM_CHECK(filled); // TitleSearch Enter routed the words into the focused pane's grid filter
    };
}

// Case 2/3 — Jql: an operator-bearing query + Enter must replace the focused view's query,
// mirrored into both cfg.JqlQuery and the dashboard editor buffer (applyQueryToPaneView).
void RegisterJqlReplacesViewQuery(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Omnibar", "Jql_ReplacesFocusedViewQuery");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareOmnibar(ctx);
        if (app == nullptr) {
            return;
        }

        const char* kQuery = "summary ~ zqxomni"; // '~' marks a structured query → Jql
        SetOmnibarQuery(ctx, kQuery);
        IM_CHECK_STR_EQ(g_ui.omniJqlEditor.buf, kQuery);

        // Reset both observed sinks to a non-matching state BEFORE Enter so neither assertion
        // can pass vacuously on a same-process suite re-run (symmetric with the sibling tests,
        // which zero their observed field first) — only the production apply can restore kQuery.
        g_ui.cfg.JqlQuery.clear();
        g_ui.viewJqlEditor.buf[0] = '\0';

        ctx->KeyPress(ImGuiKey_Enter);
        // applyQueryToPaneView (Ok path) mirrors the query into BOTH cfg.JqlQuery and the
        // dashboard editor buffer — assert both so the apply can't pass on a partial write.
        const bool appliedToCfg = YieldUntil(ctx, [&] { return g_ui.cfg.JqlQuery == kQuery; });
        IM_CHECK(appliedToCfg);                          // Jql Enter replaced the focused view's query
        IM_CHECK_STR_EQ(g_ui.viewJqlEditor.buf, kQuery); // shared-core lock-step with the dashboard editor
    };
}

// Case 3/3 — TicketKey: a bare backend key + Enter must select the already-loaded row in the
// focused pane (gridState.ActiveIssueId) — the no-network in-grid jump.
void RegisterTicketKeyJumpsToLoadedRow(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Omnibar", "TicketKey_JumpsToLoadedRow");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareOmnibar(ctx);
        if (app == nullptr) {
            return;
        }

        const std::string kKey = "SMAT-1"; // a deterministic-fixture Jira key → TicketKey
        const bool rowLoaded = YieldUntil(ctx, [&] { return FocusedSnapshotHasKey(kKey); });
        IM_CHECK_NO_RET(rowLoaded); // require the loaded-row branch (no OpenUrl in the harness)
        if (!rowLoaded) {
            return;
        }
        g_ui.focusedPane().gridState.ActiveIssueId.clear(); // pre-condition: nothing selected

        SetOmnibarQuery(ctx, kKey.c_str());
        IM_CHECK_STR_EQ(g_ui.omniJqlEditor.buf, kKey.c_str());

        ctx->KeyPress(ImGuiKey_Enter);
        const bool jumped = YieldUntil(ctx, [&] { return g_ui.focusedPane().gridState.ActiveIssueId == kKey; });
        IM_CHECK(jumped); // TicketKey Enter selected the loaded row in the focused pane
    };
}

// Case 4/4 — focus-on-open consumer. The omnibar sets jqlAcpWantsJqlInputFocus on its
// ImGui::IsWindowAppearing() edge so the first keystroke lands in the JQL input. IsWindowAppearing
// fires once at app start (the bar is drawn unconditionally) and cannot be re-triggered from a test,
// so this drives the load-bearing CONSUMER half: with the input NOT the active item, setting the
// same flag must route the next synthetic keystroke into the omnibar buffer — proving
// DrawJqlQueryEditorEmbedded's SetKeyboardFocusHere consumed the flag and granted keyboard focus.
void RegisterOmnibarInputFocusOnFlag(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Omnibar", "InputFocus_WantsFocusFlag_FirstKeystrokeLands");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareOmnibar(ctx);
        if (app == nullptr) {
            return; // skip (fixture absent) or a logged check already failed
        }
        ctx->SetRef(kOmnibarWindow);
        // Clear the buffer and move focus OFF the input via the production clear button, so the JQL
        // input is not the active item when we set the focus-want flag.
        ctx->ItemClick("x##ClearQueryEmbedded");
        ctx->Yield();
        ctx->Yield();

        // Reproduce the open-edge focus grab: set the exact flag IsWindowAppearing sets. The next
        // DrawJqlQueryEditorEmbedded consumes it via SetKeyboardFocusHere, focusing the input.
        g_ui.omniJqlEditor.jqlAcpWantsJqlInputFocus = true;
        ctx->Yield();
        ctx->Yield();

        // Type WITHOUT an explicit ItemInput/click — only the focus grant can route the keystroke
        // into the input. If it lands in the buffer, the input received keyboard focus from the flag.
        ctx->KeyChars("z");
        ctx->Yield();
        ctx->Yield();
        const std::string buf(g_ui.omniJqlEditor.buf);
        if (buf.find('z') == std::string::npos) {
            ctx->LogError("first keystroke did not land in the omnibar input — jqlAcpWantsJqlInputFocus "
                          "did not grant keyboard focus (SetKeyboardFocusHere consumer missing?)");
            IM_CHECK(false);
        }

        // Teardown: clear the buffer so a sibling variant starts clean.
        ctx->ItemClick("x##ClearQueryEmbedded");
        ctx->Yield();
    };
}

} // namespace

// Registration entry point — called once from SmatchetRegisterAllUiTests (ui_tests_registry.cpp)
// to enroll the omnibar apply-path + focus tests into the engine.
extern "C" void SmatchetRegisterOmnibarSearchApplyTests(ImGuiTestEngine* engine) {
    RegisterTitleSearchFillsGridFilter(engine);
    RegisterJqlReplacesViewQuery(engine);
    RegisterTicketKeyJumpsToLoadedRow(engine);
    RegisterOmnibarInputFocusOnFlag(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
