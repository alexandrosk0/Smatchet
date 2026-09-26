// offline_first.test.cpp — bucket-E tests for Quality Pillar 6 (offline-first).
//
// Runs against the offline-first fixture (tests/fixtures/jira_backend/offline-first.json) booted via
// SMATCHET_TEST_JIRA_BACKEND_FIXTURE; scripts/dev/test-ui-offline-first.sh sets it and the
// OfflineFirst filter. The fixture's FakeTrackerClient is attached to the process-wide
// GlobalFakeNetwork() switch (tests/support/FakeNetworkSwitch.h), so a test takes the tracker offline
// by flipping that switch and every network-shaped call fails like a real outage. Each test restores
// the switch with ScopedFakeNetworkReset.
//
// Tests are APP-STATE-COUPLED (like jira_deterministic_backend.test.cpp): they call the live
// AppController through SmatchetActiveUiTestAppController() and assert on its state, not on ImGui
// labels. Under any other fixture they skip with an informational log.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "Config/ConfigManager.h"
#include "FakeNetworkSwitch.h"
#include "Types/ConnectivityTypes.h"
#include "Types/TransitionsTypes.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// True when the app was booted with the offline-first fixture; otherwise logs a SKIP.
bool OfflineFirstFixtureActive(ImGuiTestContext* ctx) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fixture != nullptr && std::strstr(fixture, "offline-first") != nullptr) {
        return true;
    }
    ctx->LogInfo("SKIP: offline-first fixture not active");
    return false;
}

// Yield one frame at a time until `done()` holds or `maxFrames` pass; returns the final `done()`.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, int maxFrames, Pred done) {
    for (int i = 0; i < maxFrames && !done(); ++i) {
        ctx->Yield();
    }
    return done();
}

// Pull the connectivity probe forward every frame until the app reports `want` (the probe interval
// would otherwise make the test wait for the next scheduled probe).
bool WaitForConnectivity(ImGuiTestContext* ctx, AppController& app, TrackerConnectivityState want) {
    return YieldUntil(ctx, 600, [&app, want]() {
        if (app.GetLastTrackerConnectivityState() == want) {
            return true;
        }
        app.RequestTrackerProbeNow();
        return false;
    });
}

TransitionsQuery MakeTransitionsQuery(const char* issueId, const char* issueType) {
    TransitionsQuery q;
    q.IssueId = issueId;
    q.ProjectKey = "OFF";
    q.IssueTypeKey = issueType;
    q.FromStatusKey = "1"; // "To Do" in the fixture catalog
    return q;
}

} // namespace

// OfflineFirst/Catalog_SurvivesTransportDown: a field-catalog refresh that fails because the tracker
// is unreachable keeps the catalog the user already has, raises no catalog error and shows a Warning
// (not Error) banner.
static void RegisterOfflineFirstCatalogSurvivesTransportDown(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Catalog_SurvivesTransportDown");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr); // surfaces as a test failure with context
            return;
        }
        const TrackerConfig cfg = ConfigManager::Load();

        // Online: the fixture's scripted catalog loads.
        IM_CHECK_NO_RET(app->RefreshFieldCatalog(cfg));
        IM_CHECK_NO_RET(!app->GetAvailableFields().empty());
        IM_CHECK_NO_RET(app->GetFieldCatalogError().empty());

        // Offline: the refresh fails at the transport level...
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(!app->RefreshFieldCatalog(cfg));
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() >= 1);

        // ...but the catalog the user already had survives, with a warning rather than an error.
        IM_CHECK_NO_RET(!app->GetAvailableFields().empty());
        IM_CHECK_NO_RET(app->GetFieldCatalogError().empty());
        IM_CHECK_NO_RET(app->GetTrackerConnectivityBannerForUi(nullptr).Kind ==
                        TrackerConnectivityBannerForUi::Level::Warning);
    };
}

// OfflineFirst/StatusCombo_OfflineShowsOptions: the status combo's transitions lookup never needs the
// network once a workflow edge was seen. Online, OFF-1's live transitions load (Fresh) and are
// remembered for (OFF, bug, To Do); offline, OFF-2 with the same project, type and status gets the
// remembered targets without any network call, and an unseen edge yields no options (the combo then
// lists every catalog status, covered by StatusComboOptionsPure). Drives the service through the app
// API, so no ImGui cell ids are needed.
static void RegisterOfflineFirstStatusComboOfflineShowsOptions(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "StatusCombo_OfflineShowsOptions");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        using smatchet::offline::DataFreshness;
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));

        // Online: the live transitions load in the background and are remembered.
        const TransitionsQuery online = MakeTransitionsQuery("OFF-1", "bug");
        app->EnsureIssueTransitionsLoaded(online);
        IM_CHECK_NO_RET(YieldUntil(ctx, 300, [app, &online]() {
            return app->GetAvailableTransitionsForIssue(online).freshness == DataFreshness::Fresh;
        }));
        const TransitionsLookup live = app->GetAvailableTransitionsForIssue(online);
        IM_CHECK_NO_RET(live.applicable);
        IM_CHECK_NO_RET(live.options.size() == 2);

        // Offline: wait until the app knows it, then look up an issue that was never fetched.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();

        const TransitionsQuery sameEdge = MakeTransitionsQuery("OFF-2", "bug");
        app->EnsureIssueTransitionsLoaded(sameEdge);
        const TransitionsQuery unseenEdge = MakeTransitionsQuery("OFF-9", "task");
        app->EnsureIssueTransitionsLoaded(unseenEdge);
        ctx->Yield(10);

        const TransitionsLookup learned = app->GetAvailableTransitionsForIssue(sameEdge);
        IM_CHECK_NO_RET(learned.applicable);
        IM_CHECK_NO_RET(learned.fromLearned);
        IM_CHECK_NO_RET(learned.options.size() == 2);
        IM_CHECK_NO_RET(learned.freshness == DataFreshness::CachedOffline);

        const TransitionsLookup unseen = app->GetAvailableTransitionsForIssue(unseenEdge);
        IM_CHECK_NO_RET(unseen.applicable);
        IM_CHECK_NO_RET(unseen.options.empty());

        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);

        // Leave the app online for the next test.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
    };
}

extern "C" void SmatchetRegisterOfflineFirstTests(ImGuiTestEngine* engine) {
    RegisterOfflineFirstCatalogSurvivesTransportDown(engine);
    RegisterOfflineFirstStatusComboOfflineShowsOptions(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
