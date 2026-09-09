// grid_search_apply.test.cpp — bucket-E (ImGui Test Engine) coverage for the grid header
// search box's Enter-apply path. The bucket-A suite
// (tests/Core/GridSearchInputClassifier.test.cpp) already locks the pure
// Jql|TicketKey|TitleSearch *classification*; this TU closes the gap that classifier
// coverage cannot reach — that the live search widget, drawn every frame by the pane
// header, actually ROUTES a committed Enter to the right per-pane side effect for each of
// the three kinds.
//
// WHAT THIS LOCKS IN — typing into the real "##GridFilter" input in the focused pane's
// header and pressing Enter:
//   1. TitleSearch (plain words) → filters the pane's loaded rows and NEVER mutates a saved
//      view (the text stays in the box; the filter is live, so Enter has nothing to apply).
//   2. Jql (a query carrying an operator) → mirrors into cfg.JqlQuery AND the dashboard
//      editor buffer (the applyQueryToPaneView shared-core lock-step), and does NOT leak
//      into the row filter (GridSearchFiltersRows is false for it — the regression that
//      would blank the grid right after a successful search).
//   3. TicketKey (a bare key for the backend) → narrows the rows as it is typed AND, on
//      Enter, selects the already-loaded row in the pane (SpreadsheetState::ActiveIssueId) —
//      the no-network jump.
//
// HOW IT REACHES PRODUCTION — there is no standalone widget to instantiate; instead it
// drives the LIVE box the booted app already renders, observing the result through the live
// UiDrawSession singleton `g_ui` (the same idiom as grid_pane_windows.test.cpp). The engine
// types into the production InputText and presses Enter; the production
// ImGuiInputTextFlags_EnterReturnsTrue commit latches PaneDeferredActionKind::GridSearchCommit,
// which drawGridPaneWindows drains after the pane loop into applyGridSearchEnter → the
// classified branch. No mock of the classifier.
//
// Fixture-gated like every data-dependent bucket-E test: the deterministic Jira backend
// (SMATCHET_TEST_JIRA_BACKEND_FIXTURE → SMAT-1/SMAT-2) gives a loaded focused pane; without
// it the tests SKIP.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "GridPane.h"                          // GridPane, CachedTicket, gridSearchBuf, gridState
#include "SmatchetGridUiSupport.h"             // GridSearchFiltersRows
#include "SmatchetUiSession.h"                 // UiDrawSession, focusedPane(), cfg

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

// Sync the deterministic backend, bring the primary grid pane live + focused so the
// focused pane resolves to a real loaded pane ("main"). Returns false (after a
// logged skip/check) when the backend never settled.
bool BootSyncFocusPrimary(ImGuiTestContext* ctx, AppController* app) {
    app->SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
    IM_CHECK_NO_RET(syncDone);
    // Past the fixture gate the deterministic backend MUST yield rows — a completed-but-empty
    // sync is a real failure, not a silent skip. Hard-assert it (mirroring
    // grid_pane_windows.test.cpp:85) so the tests can't pass green while exercising nothing.
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

// Replace the focused pane's search box with `text`, leaving its input active. The buffer is
// per-pane session state that persists across sibling tests, so it is zeroed first (directly:
// the production "Clear" button only exists while the box is non-empty).
void SetGridSearch(ImGuiTestContext* ctx, const char* text) {
    g_ui.focusedPane().gridSearchBuf[0] = '\0';
    ctx->SetRef(kPrimaryWindow);
    ctx->ItemInput("##GridFilter");
    ctx->KeyChars(text);
    ctx->Yield();
    ctx->Yield();
}

// Shared per-variant preamble: fixture gate, app gate, sync/focus.
// Returns the booted app, or nullptr after a logged skip/check (caller returns).
AppController* PrepareGridSearch(ImGuiTestContext* ctx) {
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
    // The search box lives in the pane header, which the live primary pane draws every frame.
    const bool boxLive = YieldUntil(ctx, [ctx] { return ctx->ItemExists("##GridFilter"); });
    IM_CHECK_NO_RET(boxLive);
    if (!boxLive) {
        return nullptr;
    }
    return app;
}

// Case 1/3 — TitleSearch: plain words filter the pane's loaded rows live, and Enter leaves the
// saved view alone.
void RegisterTitleSearchFiltersRows(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSearch", "TitleSearch_FiltersLoadedRows");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareGridSearch(ctx);
        if (app == nullptr) {
            return;
        }

        const std::string queryBefore = g_ui.cfg.JqlQuery;
        const char* kQuery = "zqxgrid probe"; // plain words: no operator / colon / key shape → TitleSearch
        SetGridSearch(ctx, kQuery);
        IM_CHECK_STR_EQ(g_ui.focusedPane().gridSearchBuf, kQuery); // typed text reached the live box

        // Plain words feed the row filter WITHOUT an Enter — the box filters as you type.
        const bool filters = YieldUntil(ctx, [&] { return GridSearchFiltersRows(g_ui.focusedPane()); });
        IM_CHECK(filters);

        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();
        // Enter on plain words is a no-op: the text stays put and no view query was rewritten.
        IM_CHECK_STR_EQ(g_ui.focusedPane().gridSearchBuf, kQuery);
        IM_CHECK(g_ui.cfg.JqlQuery == queryBefore);

        g_ui.focusedPane().gridSearchBuf[0] = '\0'; // teardown: leave the box clean for siblings
    };
}

// Case 2/3 — Jql: an operator-bearing query + Enter must replace the pane view's query,
// mirrored into both cfg.JqlQuery and the dashboard editor buffer (applyQueryToPaneView) —
// and must NOT be applied as a row filter while it sits in the box.
void RegisterJqlReplacesViewQuery(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSearch", "Jql_ReplacesPaneViewQuery");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareGridSearch(ctx);
        if (app == nullptr) {
            return;
        }

        const char* kQuery = "summary ~ zqxgrid"; // '~' marks a structured query → Jql
        SetGridSearch(ctx, kQuery);
        IM_CHECK_STR_EQ(g_ui.focusedPane().gridSearchBuf, kQuery);
        // The regression this guards: a query left in the box must never be substring-matched
        // against the rows (it would blank the grid the moment its own Enter refilled it).
        IM_CHECK(!GridSearchFiltersRows(g_ui.focusedPane()));

        // Reset both observed sinks to a non-matching state BEFORE Enter so neither assertion
        // can pass vacuously on a same-process suite re-run (symmetric with the sibling tests,
        // which zero their observed field first) — only the production apply can restore kQuery.
        g_ui.cfg.JqlQuery.clear();
        g_ui.viewJqlEditor.buf[0] = '\0';

        ctx->KeyPress(ImGuiKey_Enter);
        // applyQueryToPaneView (Ok path) mirrors the query into BOTH cfg.JqlQuery and the
        // dashboard editor buffer — assert both so the apply can't pass on a partial write.
        const bool appliedToCfg = YieldUntil(ctx, [&] { return g_ui.cfg.JqlQuery == kQuery; });
        IM_CHECK(appliedToCfg);                          // Jql Enter replaced the pane view's query
        IM_CHECK_STR_EQ(g_ui.viewJqlEditor.buf, kQuery); // shared-core lock-step with the dashboard editor

        g_ui.focusedPane().gridSearchBuf[0] = '\0';
    };
}

// Case 3/3 — TicketKey: a bare backend key + Enter must select the already-loaded row in the
// focused pane (gridState.ActiveIssueId) — the no-network in-grid jump.
void RegisterTicketKeyJumpsToLoadedRow(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSearch", "TicketKey_JumpsToLoadedRow");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = PrepareGridSearch(ctx);
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

        SetGridSearch(ctx, kKey.c_str());
        IM_CHECK_STR_EQ(g_ui.focusedPane().gridSearchBuf, kKey.c_str());
        // A key narrows the rows as it is typed (the box's pre-existing behaviour) AND jumps
        // on Enter — both, unlike a structured query, which never reaches the row filter.
        IM_CHECK(GridSearchFiltersRows(g_ui.focusedPane()));

        ctx->KeyPress(ImGuiKey_Enter);
        const bool jumped = YieldUntil(ctx, [&] { return g_ui.focusedPane().gridState.ActiveIssueId == kKey; });
        IM_CHECK(jumped); // TicketKey Enter selected the loaded row in the focused pane

        g_ui.focusedPane().gridSearchBuf[0] = '\0';
    };
}

} // namespace

// Registration entry point — called once from SmatchetRegisterAllUiTests (ui_tests_registry.cpp)
// to enroll the grid-search apply-path tests into the engine.
extern "C" void SmatchetRegisterGridSearchApplyTests(ImGuiTestEngine* engine) {
    RegisterTitleSearchFiltersRows(engine);
    RegisterJqlReplacesViewQuery(engine);
    RegisterTicketKeyJumpsToLoadedRow(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
