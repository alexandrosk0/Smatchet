// grid_parent_hierarchy.test.cpp — bucket-E smoke for the parent-issue hierarchy toggles
// (docs/plans/shipped/parent-issue-hierarchy.md, Slice 3).
//
// Two tests in this file, each its own ImGui Test Engine group so each can be pointed at the
// fixture it actually needs (SMATCHET_TEST_JIRA_BACKEND_FIXTURE) without the other breaking:
//
// GridParentHierarchy / SortByToggles_ProjectionAndDirty (basic-grid.json, no parent links)
// drives the "Sort By" popup's two per-view checkboxes and asserts the observable session
// effects:
//   1. story-group ON  — the view flips dirty (persist path) and the pane's projection
//                        caches a depth entry per active ticket.
//   2. hide-parents ON — the view flips dirty again; no parents in this fixture, so the row
//                        projection keeps every ticket. (This assertion is specific to a
//                        parent-free fixture — a fixture with real parent links would have
//                        hide-parents drop the parent rows, which is exactly what
//                        DeepChain_DepthsAndOrderMatchAncestry below exercises instead. Do
//                        not point this test at parent-hierarchy-grid.json.)
//   3. both OFF        — the depth cache is cleared (the common path stays branch-free).
// This test pins the UI seam only (toggle -> dirty -> cache populated/cleared).
//
// GridParentHierarchyDeepChain / DeepChain_DepthsAndOrderMatchAncestry
// (parent-hierarchy-grid.json, an epic -> story -> task -> subtask chain landing in one
// streamed batch) asserts the actual per-ticket depths and the row order the live grid
// computed, so a multi-level ancestor chain rendering correctly has coverage beyond the pure
// ParentHierarchyPure/TicketSyncService unit tests. Run it with:
//   UI_TEST_FILTER=GridParentHierarchyDeepChain \
//   SMATCHET_TEST_JIRA_BACKEND_FIXTURE=tests/fixtures/jira_backend/parent-hierarchy-grid.json \
//   bash scripts/dev/test-ui-jira-deterministic-backend.sh
// It skips cleanly (does not fail) rather than asserting when run against a fixture that
// lacks its expected ticket keys, so an accidental default-fixture run is harmless — but the
// two tests' filters are deliberately kept separate so neither can silently run against the
// other's fixture in one invocation.
//
// Missing-parent fetch itself (multi-hop resolution across sync batches) is covered by
// TicketSyncService's own unit tests, not here — this file only pins the UI seam once the
// tickets are already in the active set. Follows the grid_pane_windows.test.cpp boot recipe.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h" // CachedTicket
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — live-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

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

// Primary grid-pane window title (the app's active-project window).
const char* kPrimary = "Smatchet - Active Project";

// Sort By popup opener (header button label) + the two hierarchy checkboxes inside it.
const char* kSortByButton = "Sort By \xE2\x86\x95";
const char* kStoryGroupRef = "**/Parent group";
const char* kHideParentsRef = "**/Hide parent stories";

// Boot preamble: fixture gate + backend sync + primary pane live/focused + panes-loaded check.
// Returns the booted app, or nullptr after a logged skip/check (the caller returns on nullptr).
AppController* BootSyncPrimaryLive(ImGuiTestContext* ctx) {
    if (!FixtureEnvSet()) {
        ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent");
        return nullptr;
    }
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return nullptr;
    }
    // Suppress the first-run Whisper setup banner so it cannot float over the header row and
    // swallow the Sort By click on a clean profile.
    g_ui.cfg.WhisperSetupCompleted = true;
    app->SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
    IM_CHECK_NO_RET(syncDone);
    IM_CHECK_NO_RET(!app->GetActiveTickets().empty());

    ctx->SetRef(kPrimary);
    const bool primaryLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kPrimary);
    });
    IM_CHECK_NO_RET(primaryLive);
    if (!primaryLive) {
        return nullptr;
    }
    IM_CHECK_NO_RET(g_ui.gridPanesLoaded);
    IM_CHECK_NO_RET(g_ui.gridPanes.size() == 1); // IM_CHECK_EQ_NO_RET returns void — invalid in this helper
    return app;
}

// Open the Sort By popup (if not already open) and click one of its checkboxes. Returns false
// when the checkbox never materialised (popup failed to open).
bool ClickSortByCheckbox(ImGuiTestContext* ctx, const char* checkboxRef) {
    if (!ctx->ItemExists(checkboxRef)) {
        ctx->ItemClick(kSortByButton);
        const bool popupOpen = YieldUntil(ctx, [&] { return ctx->ItemExists(checkboxRef); }, 60);
        if (!popupOpen) {
            return false;
        }
    }
    ctx->ItemClick(checkboxRef);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// GridParentHierarchy / SortByToggles_ProjectionAndDirty
// ---------------------------------------------------------------------------
static void RegisterSortByTogglesProjectionAndDirty(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridParentHierarchy", "SortByToggles_ProjectionAndDirty");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = BootSyncPrimaryLive(ctx);
        if (app == nullptr) {
            return;
        }
        const size_t ticketCount = app->GetActiveTickets().size();

        // Baseline: neither toggle set → no depth cache, projection covers every row.
        const bool baselineProjected =
            YieldUntil(ctx, [&] { return g_ui.gridPanes.front().filteredIndices.size() == ticketCount; });
        IM_CHECK_NO_RET(baselineProjected);
        IM_CHECK_NO_RET(g_ui.gridPanes.front().cachedDepths.empty());

        // 1. STORY GROUP ON — view flips dirty; the projection caches one depth per ticket.
        g_ui.viewsDirty = false;
        const bool storyGroupClicked = ClickSortByCheckbox(ctx, kStoryGroupRef);
        IM_CHECK_NO_RET(storyGroupClicked);
        if (!storyGroupClicked) {
            return;
        }
        const bool storyGroupDirty = YieldUntil(ctx, [&] { return g_ui.viewsDirty; });
        IM_CHECK_NO_RET(storyGroupDirty);
        const bool depthsCached =
            YieldUntil(ctx, [&] { return g_ui.gridPanes.front().cachedDepths.size() == ticketCount; });
        IM_CHECK_NO_RET(depthsCached);
        IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().filteredIndices.size(), ticketCount);

        // 2. HIDE PARENTS ON (with Story group still on) — view flips dirty; no parents in the
        //    fixture → every row survives, but hide-parents flattens the tree decorations: the
        //    depth cache is cleared even though story-group order is kept (SmatchetActiveProjectGridTable.cpp).
        g_ui.viewsDirty = false;
        const bool hideParentsClicked = ClickSortByCheckbox(ctx, kHideParentsRef);
        IM_CHECK_NO_RET(hideParentsClicked);
        if (!hideParentsClicked) {
            return;
        }
        const bool hideParentsDirty = YieldUntil(ctx, [&] { return g_ui.viewsDirty; });
        IM_CHECK_NO_RET(hideParentsDirty);
        const bool depthsCleared = YieldUntil(ctx, [&] { return g_ui.gridPanes.front().cachedDepths.empty(); });
        IM_CHECK_NO_RET(depthsCleared);
        IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().filteredIndices.size(), ticketCount);

        // 3. BOTH OFF — depth cache drains; projection still covers every row.
        g_ui.viewsDirty = false;
        const bool storyGroupOff = ClickSortByCheckbox(ctx, kStoryGroupRef);
        IM_CHECK_NO_RET(storyGroupOff);
        const bool hideParentsOff = ClickSortByCheckbox(ctx, kHideParentsRef);
        IM_CHECK_NO_RET(hideParentsOff);
        const bool offDirty = YieldUntil(ctx, [&] { return g_ui.viewsDirty; });
        IM_CHECK_NO_RET(offDirty);
        const bool depthsClearedAgain = YieldUntil(ctx, [&] { return g_ui.gridPanes.front().cachedDepths.empty(); });
        IM_CHECK_NO_RET(depthsClearedAgain);
        IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().filteredIndices.size(), ticketCount);
    };
}

// ---------------------------------------------------------------------------
// GridParentHierarchyDeepChain / DeepChain_DepthsAndOrderMatchAncestry
//
// A separate test GROUP from GridParentHierarchy above (deliberately — see the file header)
// so it never runs against basic-grid.json by accident. Drives the same "Parent group"
// toggle against parent-hierarchy-grid.json (an epic -> story -> task -> subtask chain, all
// four issues present in one streamed batch, plus one unrelated top-level issue) and checks
// the actual per-ticket depths and relative ordering the grid computed, so a real multi-level
// ancestor chain landing correctly in the live grid has test coverage beyond the pure
// ParentHierarchyPure/TicketSyncService unit tests.
// ---------------------------------------------------------------------------
static void RegisterDeepChainDepthsAndOrderMatchAncestry(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridParentHierarchyDeepChain", "DeepChain_DepthsAndOrderMatchAncestry");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = BootSyncPrimaryLive(ctx);
        if (app == nullptr) {
            return;
        }

        const std::vector<CachedTicket> tickets = app->GetActiveTickets();
        // This test needs the deep-chain fixture specifically (parent-hierarchy-grid.json);
        // skip cleanly if the run was pointed at a different fixture (e.g. basic-grid.json,
        // shared by SortByToggles_ProjectionAndDirty above under the same test filter).
        auto findIndex = [&](const char* key) -> int {
            for (size_t i = 0; i < tickets.size(); ++i) {
                if (tickets[i].id == key) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        const int epicIdx = findIndex("PH-EPIC-1");
        const int storyIdx = findIndex("PH-STORY-1");
        const int taskIdx = findIndex("PH-TASK-1");
        const int subtaskIdx = findIndex("PH-SUBTASK-1");
        const int leafIdx = findIndex("PH-LEAF-1");
        if (epicIdx < 0 || storyIdx < 0 || taskIdx < 0 || subtaskIdx < 0 || leafIdx < 0) {
            ctx->LogInfo("SKIP: parent-hierarchy-grid.json fixture tickets not found — "
                         "run with SMATCHET_TEST_JIRA_BACKEND_FIXTURE=tests/fixtures/jira_backend/"
                         "parent-hierarchy-grid.json");
            return;
        }

        // Turn on Parent group; leave Hide parent stories off so depths/tint stay populated.
        g_ui.viewsDirty = false;
        const bool storyGroupClicked = ClickSortByCheckbox(ctx, kStoryGroupRef);
        IM_CHECK_NO_RET(storyGroupClicked);
        if (!storyGroupClicked) {
            return;
        }
        const bool depthsCached =
            YieldUntil(ctx, [&] { return g_ui.gridPanes.front().cachedDepths.size() == tickets.size(); });
        IM_CHECK_NO_RET(depthsCached);

        const std::vector<int>& depths = g_ui.gridPanes.front().cachedDepths;
        // The full ancestor chain resolves within the fixture's single streamed batch (every
        // level is already present), so depth is exactly the ancestor count: root = 0.
        IM_CHECK_EQ_NO_RET(depths[static_cast<size_t>(epicIdx)], 0);
        IM_CHECK_EQ_NO_RET(depths[static_cast<size_t>(storyIdx)], 1);
        IM_CHECK_EQ_NO_RET(depths[static_cast<size_t>(taskIdx)], 2);
        IM_CHECK_EQ_NO_RET(depths[static_cast<size_t>(subtaskIdx)], 3);
        IM_CHECK_EQ_NO_RET(depths[static_cast<size_t>(leafIdx)], 0); // unrelated issue: also a root

        // Story-group order: each ancestor must appear before its descendant in the grid's
        // row order, matching StoryGroupOrder's "parent immediately followed by descendants"
        // contract (grandchildren still land after their parent, transitively).
        const std::vector<std::size_t>& order = g_ui.gridPanes.front().filteredIndices;
        auto positionOf = [&](int ticketIdx) -> int {
            for (size_t pos = 0; pos < order.size(); ++pos) {
                if (order[pos] == static_cast<std::size_t>(ticketIdx)) {
                    return static_cast<int>(pos);
                }
            }
            return -1;
        };
        const int epicPos = positionOf(epicIdx);
        const int storyPos = positionOf(storyIdx);
        const int taskPos = positionOf(taskIdx);
        const int subtaskPos = positionOf(subtaskIdx);
        IM_CHECK_NO_RET(epicPos >= 0 && storyPos >= 0 && taskPos >= 0 && subtaskPos >= 0);
        IM_CHECK_NO_RET(epicPos < storyPos);
        IM_CHECK_NO_RET(storyPos < taskPos);
        IM_CHECK_NO_RET(taskPos < subtaskPos);
    };
}

extern "C" void SmatchetRegisterGridParentHierarchyTests(ImGuiTestEngine* engine) {
    RegisterSortByTogglesProjectionAndDirty(engine);
    RegisterDeepChainDepthsAndOrderMatchAncestry(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
