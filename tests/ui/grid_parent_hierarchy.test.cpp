// grid_parent_hierarchy.test.cpp — bucket-E smoke for the parent-issue hierarchy toggles
// (docs/plans/shipped/parent-issue-hierarchy.md, Slice 3).
//
// Drives the "Sort By" popup's two per-view checkboxes on the primary grid pane and asserts
// the observable session effects:
//   1. story-group ON  — the view flips dirty (persist path) and the pane's projection
//                        caches a depth entry per active ticket.
//   2. hide-parents ON — the view flips dirty again; the CI fixture (basic-grid.json) carries
//                        no parent links, so the row projection keeps every ticket.
//   3. both OFF        — the depth cache is cleared (the common path stays branch-free).
//
// Hierarchy semantics (missing-parent fetch, story-group order, depth, ancestor re-add) are
// covered by the pure + sync unit tests; this test pins the UI seam only, so it stays
// fixture-agnostic. Follows the grid_pane_windows.test.cpp boot recipe.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — live-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <string>

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

        // 2. HIDE PARENTS ON — view flips dirty; no parents in the fixture → every row survives.
        g_ui.viewsDirty = false;
        const bool hideParentsClicked = ClickSortByCheckbox(ctx, kHideParentsRef);
        IM_CHECK_NO_RET(hideParentsClicked);
        if (!hideParentsClicked) {
            return;
        }
        const bool hideParentsDirty = YieldUntil(ctx, [&] { return g_ui.viewsDirty; });
        IM_CHECK_NO_RET(hideParentsDirty);
        ctx->Yield(3);
        IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().filteredIndices.size(), ticketCount);
        IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().cachedDepths.size(), ticketCount);

        // 3. BOTH OFF — depth cache drains; projection still covers every row.
        g_ui.viewsDirty = false;
        const bool storyGroupOff = ClickSortByCheckbox(ctx, kStoryGroupRef);
        IM_CHECK_NO_RET(storyGroupOff);
        const bool hideParentsOff = ClickSortByCheckbox(ctx, kHideParentsRef);
        IM_CHECK_NO_RET(hideParentsOff);
        const bool offDirty = YieldUntil(ctx, [&] { return g_ui.viewsDirty; });
        IM_CHECK_NO_RET(offDirty);
        const bool depthsCleared = YieldUntil(ctx, [&] { return g_ui.gridPanes.front().cachedDepths.empty(); });
        IM_CHECK_NO_RET(depthsCleared);
        IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().filteredIndices.size(), ticketCount);
    };
}

extern "C" void SmatchetRegisterGridParentHierarchyTests(ImGuiTestEngine* engine) {
    RegisterSortByTogglesProjectionAndDirty(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
