// linear_deterministic_backend.test.cpp — bucket-E tests for the deterministic
// Linear fixture backend (Slice 4 of docs/plans/active/linear-tracker-backend.md).
//
// All tests require the app to have been booted with a fixture-backed factory via
// SMATCHET_TEST_LINEAR_BACKEND_FIXTURE (the AppController::Initialize hook). If
// that env var was not set the fixture factory is absent and the tests skip with
// an informational log rather than fail — the same convention the Jira
// deterministic backend tests use.
//
// Tests are APP-STATE-COUPLED (not replica): they call into the live AppController
// via SmatchetActiveUiTestAppController() and assert on GetActiveTickets() rather
// than on ImGui item labels — the ticket-grid ImGui surface is not driven here to
// avoid brittle label assertions.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"

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
    const bool set = std::getenv("SMATCHET_TEST_LINEAR_BACKEND_FIXTURE") != nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return set;
}

} // namespace

// ---------------------------------------------------------------------------
// LinearDeterministicSync_LoadsIssuesIntoGrid
// Boot with the basic-grid fixture, trigger a sync, wait for completion, assert
// the fixture's issues (LIN-1, LIN-2) are present in the active-tickets vector.
// ---------------------------------------------------------------------------
static void RegisterLinearDeterministicSyncLoadsIssues(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "LinearDeterministic", "Sync_LoadsIssuesIntoGrid");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_LINEAR_BACKEND_FIXTURE not set — fixture backend absent");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr); // surfaces as a test failure with context
            return;
        }

        app->SyncWithBackend();

        const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
        IM_CHECK_NO_RET(syncDone);

        const auto tickets = app->GetActiveTickets();
        IM_CHECK_NO_RET(!tickets.empty());

        // The basic-grid fixture seeds LIN-1 and LIN-2 (Linear identifiers).
        bool foundLin1 = false;
        bool foundLin2 = false;
        for (const auto& row : tickets) {
            if (row.id == "LIN-1") {
                foundLin1 = true;
            }
            if (row.id == "LIN-2") {
                foundLin2 = true;
            }
        }
        IM_CHECK_NO_RET(foundLin1);
        IM_CHECK_NO_RET(foundLin2);
    };
}

// ---------------------------------------------------------------------------
// LinearDeterministicSync_MapsLinearSpecificFields
// The mapper carries Linear-native fields (priority label, team key, assignee
// display name) end-to-end. Assert one row exposes them after sync — guards the
// CachedTicket field-id contract the grid columns depend on.
// ---------------------------------------------------------------------------
static void RegisterLinearDeterministicSyncMapsFields(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "LinearDeterministic", "Sync_MapsLinearSpecificFields");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_LINEAR_BACKEND_FIXTURE not set — fixture backend absent");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }

        app->SyncWithBackend();
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); }));

        const auto tickets = app->GetActiveTickets();
        const CachedTicket* lin1 = nullptr;
        for (const auto& row : tickets) {
            if (row.id == "LIN-1") {
                lin1 = &row;
                break;
            }
        }
        IM_CHECK_NO_RET(lin1 != nullptr);
        if (lin1) {
            IM_CHECK_NO_RET(lin1->GetFieldValue("summary") == "First deterministic Linear issue");
            IM_CHECK_NO_RET(lin1->GetFieldValue("status") == "In Progress");
            IM_CHECK_NO_RET(lin1->GetFieldValue("priority") == "High");
            IM_CHECK_NO_RET(lin1->GetFieldValue("assignee") == "Alice Tester");
            IM_CHECK_NO_RET(lin1->GetFieldValue("team") == "LIN");
        }
    };
}

extern "C" void SmatchetRegisterLinearDeterministicBackendTests(ImGuiTestEngine* engine) {
    RegisterLinearDeterministicSyncLoadsIssues(engine);
    RegisterLinearDeterministicSyncMapsFields(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
