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

} // namespace

// ---------------------------------------------------------------------------
// OfflineFirst_Catalog_SurvivesTransportDown
// A field-catalog refresh that fails because the tracker is unreachable must keep the catalog the
// user already has, raise no catalog error and show a Warning (not Error) banner (S1 behaviour).
// ---------------------------------------------------------------------------
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

extern "C" void SmatchetRegisterOfflineFirstTests(ImGuiTestEngine* engine) {
    RegisterOfflineFirstCatalogSurvivesTransportDown(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
