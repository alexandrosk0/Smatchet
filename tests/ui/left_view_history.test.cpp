// left_view_history.test.cpp — bucket E for the window listing issues that left a view. Run by
// scripts/dev/test-ui-left-view-history.sh against the mock Jira fixture
// tests/fixtures/jira_backend/left-view-history.json, in an isolated profile: the first sync lists three issues,
// the next lists one. Real path under test: sync -> listing diff -> SQLite -> reason lookup -> window.
// Reason wording and every edge case are covered in tests/Core/LeftViewHistoryService.test.cpp.

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "LeftViewHistoryService.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <cstring>
#include <string>

extern UiDrawSession g_ui;

namespace {

const char* const kWindowTitle = "Issues That Left Views###LeftViewHistory";

bool FixtureActive(ImGuiTestContext* ctx) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fixture != nullptr && std::strstr(fixture, "left-view-history") != nullptr) {
        return true;
    }
    ctx->LogInfo("SKIP: left-view-history fixture not active");
    return false;
}

const smatchet::LeftViewRecord* FindRow(const smatchet::LeftViewHistoryService& history, const char* key) {
    for (const smatchet::LeftViewRecord& row : history.Rows()) {
        if (row.issueKey == key) {
            return &row;
        }
    }
    return nullptr;
}

bool ReasonSettled(const smatchet::LeftViewHistoryService& history, const char* key) {
    const smatchet::LeftViewRecord* row = FindRow(history, key);
    return row != nullptr && row->reasonKind != smatchet::LeftViewReasonKind::Pending;
}

bool WindowIsLive() {
    const ImGuiWindow* window = ImGui::FindWindowByName(kWindowTitle);
    return window != nullptr && window->Active;
}

} // namespace

extern "C" void SmatchetRegisterLeftViewHistoryTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "LeftViewHistory", "SyncRecordsDeparturesAndWindowShowsThem");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr - app not booted");
            return;
        }
        if (!FixtureActive(ctx)) {
            return;
        }
        smatchet::LeftViewHistoryService* history = app->LeftViewHistory();
        IM_CHECK(history != nullptr);

        // The fixture's second answer is sticky, so syncing until both departures settle is deterministic however
        // many automatic syncs the boot already ran.
        bool settled = false;
        for (int i = 0; i < 1800 && !settled; ++i) {
            if (i % 120 == 0) {
                app->SyncWithBackend();
            }
            ctx->Yield();
            settled = ReasonSettled(*history, "LEFT-2") && ReasonSettled(*history, "LEFT-3");
        }
        IM_CHECK_NO_RET(settled);

        const smatchet::LeftViewRecord* finished = FindRow(*history, "LEFT-2");
        IM_CHECK(finished != nullptr);
        IM_CHECK_NO_RET(finished->summary == "Gets finished");
        IM_CHECK_NO_RET(finished->reason.find("Status: In Progress -> Done") != std::string::npos);
        const smatchet::LeftViewRecord* unreachable = FindRow(*history, "LEFT-3");
        IM_CHECK(unreachable != nullptr);
        IM_CHECK_NO_RET(unreachable->reason == "Deleted or no longer visible");
        IM_CHECK_NO_RET(FindRow(*history, "LEFT-1") == nullptr);

        // The window draws those rows and the toolbar; details draw beside them.
        g_ui.showLeftViewHistory = true;
        g_ui.requestLeftViewHistoryFocus = true;
        bool live = false;
        for (int i = 0; i < 300 && !live; ++i) {
            ctx->Yield();
            live = WindowIsLive();
        }
        IM_CHECK_NO_RET(live);
        ctx->SetRef(kWindowTitle);
        IM_CHECK_NO_RET(ctx->ItemExists("Open details"));
        IM_CHECK_NO_RET(ctx->ItemExists("Clear all"));

        history->RequestDetails(*finished, g_ui.cfg);
        const std::int64_t finishedId = finished->id;
        ctx->Yield(10);
        IM_CHECK_NO_RET(history->Details().recordId == finishedId);
        IM_CHECK_NO_RET(ctx->ItemExists("Close details"));

        // Dismiss one row through the service, then the two-step clear through the buttons a user presses.
        const std::size_t before = history->Rows().size();
        history->Dismiss(finishedId);
        ctx->Yield(2);
        IM_CHECK_NO_RET(history->Rows().size() == before - 1);
        IM_CHECK_NO_RET(history->Details().recordId == 0);
        ctx->ItemClick("Clear all");
        IM_CHECK_NO_RET(!history->Rows().empty()); // the first click only arms it
        ctx->ItemClick("Confirm clear all");
        IM_CHECK_NO_RET(history->Rows().empty());

        g_ui.showLeftViewHistory = false;
        ctx->Yield(2);
        IM_CHECK_NO_RET(!WindowIsLive());
    };
}
