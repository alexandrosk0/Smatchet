// preferences_tracker_switch.test.cpp — bucket-E coverage for the Preferences
// tracker-backend switch (the last open backlog item of
// docs/plans/shipped/github-tracker-backend.md § Remaining). Guards the
// TicketSyncService::SwapBackendIfTrackerChanged contract the Preferences
// "Save & Sync" flow drives: a TrackerType change must swap the live backend
// INSTANCE to the requested kind and clear the in-memory ActiveTickets so the
// old backend's rows never linger in the grid — the cross-kind stale-grid bug
// once latent for Jira↔Plane; a future fifth tracker missing its swap branch
// regresses exactly here.
//
// Registered under the "JiraDeterministic" group so the existing
// scripts/dev/test-ui-jira-deterministic-backend.sh driver + CI bucket-E
// Jira-fixture lane run it with the SMATCHET_TEST_JIRA_BACKEND_FIXTURE boot they
// already provide. Under that fixture the ScriptedTrackerBackendFactory serves the
// deterministic Jira client for "Jira" and a bare FakeTrackerClient(<kind>) for
// any other kind — so the switch is fully observable: the GitHub-kind backend
// reports GetTrackerType()=="GitHub" and fetches zero tickets, and switching back
// re-serves the fixture rows (CreateClient() is fresh-per-call).
//
// APP-STATE-COUPLED like the sibling deterministic-backend tests: it drives
// AppController::SyncWithBackend with a TrackerType-changed config override — the
// exact call SmatchetUI::onPreferencesSaveAndSync makes after the dropdown edit —
// and asserts on BackendShared() / GetActiveTickets(), not on ImGui item labels
// (the Save & Sync footer button's composed icon-leading ID is documented as a
// non-stable probe in funcsize_window_render_smoke.test.cpp).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "ConfigManager.h"
#include "ITrackerBackend.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True if the fixture env var was set — tests skip without it.
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

bool HasTicket(const std::vector<CachedTicket>& tickets, const char* id) {
    for (const auto& row : tickets) {
        if (row.id == id) {
            return true;
        }
    }
    return false;
}

std::string LiveBackendType(AppController* app) {
    const std::shared_ptr<ITrackerBackend> backend = app->BackendShared();
    return backend ? backend->Connectivity().GetTrackerType() : std::string();
}

// Run one backend switch through the exact entry point the Preferences Save & Sync
// flow uses, then wait for the resulting sync to settle.
void SwitchTrackerTo(ImGuiTestContext* ctx, AppController* app, const char* trackerType) {
    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = trackerType;
    app->SyncWithBackend(&cfg);
    // The swap itself is synchronous inside SyncWithBackend; the fetch settles async.
    IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); }));
}

} // namespace

// ---------------------------------------------------------------------------
// TrackerSwitch_SwapsBackendAndClearsStaleTickets
// Round-trip: Jira fixture rows in the grid → switch to GitHub → the live
// backend instance is a NEW object of the requested kind and no Jira row
// lingers → switch back to Jira → the fixture rows return. Round-trip keeps
// the test self-restoring for whatever runs after it in this group.
// ---------------------------------------------------------------------------
static void RegisterTrackerSwitchSwapsBackend(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "JiraDeterministic", "TrackerSwitch_SwapsBackendAndClearsStaleTickets");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }

        // Phase 1 — baseline: the Jira fixture populates the grid.
        app->SyncWithBackend();
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); }));
        IM_CHECK_NO_RET(HasTicket(app->GetActiveTickets(), "SMAT-1"));
        const std::shared_ptr<ITrackerBackend> jiraBackend = app->BackendShared();
        IM_CHECK_NO_RET(jiraBackend != nullptr);

        // Phase 2 — switch to GitHub. The backend must be a NEW instance of the
        // requested kind (the swap the Preferences dropdown drives), and the old
        // backend's rows must not linger: the GitHub-kind fake fetches nothing and
        // the GitHub cache namespace is fresh, so a correctly-cleared grid is empty.
        SwitchTrackerTo(ctx, app, "GitHub");
        const std::shared_ptr<ITrackerBackend> githubBackend = app->BackendShared();
        IM_CHECK_NO_RET(githubBackend != nullptr);
        IM_CHECK_NO_RET(githubBackend != jiraBackend);
        IM_CHECK_NO_RET(LiveBackendType(app) == "GitHub");
        IM_CHECK_NO_RET(!HasTicket(app->GetActiveTickets(), "SMAT-1"));
        IM_CHECK_NO_RET(app->GetActiveTickets().empty());

        // Phase 3 — switch back to Jira: the swap works in both directions and the
        // fixture rows return (CreateClient() re-serves the scripted fetch).
        SwitchTrackerTo(ctx, app, "Jira");
        IM_CHECK_NO_RET(LiveBackendType(app) == "Jira");
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return HasTicket(app->GetActiveTickets(), "SMAT-1"); }));
    };
}

extern "C" void SmatchetRegisterPreferencesTrackerSwitchTests(ImGuiTestEngine* engine) {
    RegisterTrackerSwitchSwapsBackend(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
