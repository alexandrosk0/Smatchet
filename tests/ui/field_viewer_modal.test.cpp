// field_viewer_modal.test.cpp — bucket-E tests for the generic field-viewer modal shell (History +
// Comments). Registered in the "OfflineFirst" group so scripts/dev/test-ui-offline-first.sh runs them
// against the same fixture (the modal renders only from a focused grid pane, as the Comments tests need).
// Under any other fixture they skip with an informational log.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetCommentsModalUi.h"
#include "Ui/SmatchetFieldViewerModalUi.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

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

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, int maxFrames, Pred done) {
    for (int i = 0; i < maxFrames && !done(); ++i) {
        ctx->Yield();
    }
    return done();
}

CachedTicket MakeTicketWithHistory(const char* id, const char* history) {
    CachedTicket ticket;
    ticket.id = id;
    ticket.fieldValues["history"] = history;
    return ticket;
}

// Close whatever viewer is open the way a user does (Esc); the grid pane must be focused to render it.
void CloseViewerWithEscape(ImGuiTestContext* ctx) {
    ctx->KeyPress(ImGuiKey_Escape);
    if (!YieldUntil(ctx, 30, []() { return !GetFieldViewerSnapshotForTests().Active; })) {
        ctx->LogWarning("field viewer still open after Esc (grid pane not focused?)");
    }
}

} // namespace

// OfflineFirst/FieldViewer_HistoryOpensAndEscCloses: the History field opens the generic viewer
// window titled "History" for that issue, and Esc closes it.
static void RegisterFieldViewerHistoryOpensAndEscCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "FieldViewer_HistoryOpensAndEscCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "[Ana] 2026-01-02\nstatus: To Do -> Done");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "history"));
        ctx->Yield(2);
        const FieldViewerSnapshot open = GetFieldViewerSnapshotForTests();
        IM_CHECK_NO_RET(open.Active);
        IM_CHECK_NO_RET(open.FieldId == "history");
        IM_CHECK_NO_RET(open.IssueId == "OFF-1");
        IM_CHECK_NO_RET(open.Title == "History");

        CloseViewerWithEscape(ctx);
        const bool closed = !GetFieldViewerSnapshotForTests().Active;
        if (!closed) {
            // Never leave a modal open for the next test; the render path is the only closer.
            ctx->LogError("History viewer did not close on Esc");
        }
        IM_CHECK_NO_RET(closed);
    };
}

// OfflineFirst/FieldViewer_FieldWithoutViewerIsRejected: a field with no viewer opens nothing.
static void RegisterFieldViewerUnknownFieldRejected(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "FieldViewer_FieldWithoutViewerIsRejected");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(!OpenFieldViewerForField(*app, ticket, "description"));
        IM_CHECK_NO_RET(!GetFieldViewerSnapshotForTests().Active);
    };
}

// OfflineFirst/FieldViewer_HistoryReplacesComments: opening History while Comments is open replaces
// it, and the replaced content's OnClose resets the comments state (late post-backs are dropped).
static void RegisterFieldViewerHistoryReplacesComments(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "FieldViewer_HistoryReplacesComments");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        OpenCommentsModal(*app, "OFF-1");
        IM_CHECK_NO_RET(GetCommentsModalSnapshotForTests().Active);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().FieldId == "comments");
        ctx->Yield(2);

        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "[Ana] 2026-01-02\nstatus: To Do -> Done");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "history"));
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().Active);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().FieldId == "history");
        ctx->Yield(2);

        CloseViewerWithEscape(ctx);
        IM_CHECK_NO_RET(!GetFieldViewerSnapshotForTests().Active);
    };
}

extern "C" void SmatchetRegisterFieldViewerModalTests(ImGuiTestEngine* engine) {
    RegisterFieldViewerHistoryOpensAndEscCloses(engine);
    RegisterFieldViewerUnknownFieldRejected(engine);
    RegisterFieldViewerHistoryReplacesComments(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
