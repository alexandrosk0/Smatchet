// grid_undo_wrong_target.test.cpp — bucket-E, grid undo after the pane's tracker changed under it.
//
// What this pins, on the live app with the deterministic Jira fixture backend: an edit is saved, the pane is then
// switched to another Jira site (which shows the same issue ids), and neither the bulk toast's Undo
// (UndoGridEntryById) nor the grid's Ctrl+Z path (UndoNewestGridEntry) writes anything: not to the site the edit was
// made on, and not to the one shown now. The pane's entries leave the history. Each path gets a round of its own,
// since either one drops all of the pane's entries. A pane that switched away and back keeps its entry.
//
// In the GridSpreadsheet group so scripts/dev/test-ui-grid-spreadsheet.sh runs it on the grid-spreadsheet fixture.
// Skips cleanly without that fixture.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "GridPane.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "Ui/BulkFieldEditPure.h"
#include "UiTestWriteScope.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace {

constexpr std::size_t kUndoFixtureRows = 60;

bool UndoFixtureActive() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return fixture != nullptr && std::strstr(fixture, "grid-spreadsheet") != nullptr;
}

template <typename Done> bool YieldUntil(ImGuiTestContext* ctx, int budget, Done done) {
    while (!done() && budget-- > 0) {
        ctx->Yield();
    }
    return done();
}

std::string SummaryInPane(const AppController& app, const std::string& paneId, const std::string& issueId) {
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.GetPaneTicketsSnapshot(paneId);
    if (tickets) {
        for (const CachedTicket& ticket : *tickets) {
            if (ticket.id == issueId) {
                return ticket.GetFieldValue("summary");
            }
        }
    }
    return std::string();
}

bool EditQueueIdle() { return g_ui.queuedFieldEdits.empty() && !g_ui.hasInFlightEdit; }

// Points the session at `domain` and waits until the pane shows that site's rows under its cache key and the sync
// that fetched them has ended, so it cannot overwrite a later edit.
bool ShowSite(ImGuiTestContext* ctx, AppController& app, const std::string& paneId, const std::string& domain) {
    const std::string before = app.LatchPendingActionTargetForPane(paneId).BackendKey;
    g_ui.cfg.Domain = domain;
    app.SyncWithBackend(&g_ui.cfg);
    ctx->Yield(10);
    return YieldUntil(ctx, 900, [&] {
        const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.GetPaneTicketsSnapshot(paneId);
        return app.LatchPendingActionTargetForPane(paneId).BackendKey != before && tickets &&
               tickets->size() == kUndoFixtureRows && !app.IsStreamingSyncActive();
    });
}

// Saves one cell edit and returns its history entry's id; 0 when it did not save or recorded no single-cell entry.
std::uint64_t SaveSingleEdit(ImGuiTestContext* ctx, AppController& app, const std::string& paneId,
                             const TrackerField& field, const std::string& issueId, const std::string& value) {
    smatchet::grid::bulk::BulkRowInput row;
    row.IssueId = issueId;
    row.Values = {value};
    if (!QueueSingleCellFieldEdit(app, g_ui, paneId, field, row, false) ||
        !YieldUntil(ctx, 900, [&] { return EditQueueIdle() && SummaryInPane(app, paneId, issueId) == value; })) {
        return 0;
    }
    const smatchet::grid::undo::Entry* entry = g_ui.gridUndo.PeekNewest(paneId);
    return entry != nullptr && entry->BulkJobId == 0 ? entry->Id : 0;
}

// Puts the session back on the site it started on, however the test ends.
struct SiteRestore {
    AppController& App;
    const std::string Domain;
    ~SiteRestore() {
        if (g_ui.cfg.Domain != Domain) {
            g_ui.cfg.Domain = Domain;
            App.SyncWithBackend(&g_ui.cfg);
        }
    }
};

} // namespace

// GridSpreadsheet/Undo_AfterThePaneSwitchesSiteNothingIsWritten
static void RegisterUndoAfterSiteSwitch(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Undo_AfterThePaneSwitchesSiteNothingIsWritten");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!UndoFixtureActive()) {
            ctx->LogInfo("SKIP: grid-spreadsheet fixture not active");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        IM_CHECK(app != nullptr);
        app->SyncWithBackend();
        IM_CHECK(YieldUntil(ctx, 900, [&] {
            return !g_ui.gridPanes.empty() && g_ui.gridPanes.front().ticketsSnapshot &&
                   g_ui.gridPanes.front().ticketsSnapshot->size() == kUndoFixtureRows && app->GetAvailableFields() &&
                   !app->GetAvailableFields()->empty();
        }));
        const std::string paneId = g_ui.pane().id;
        TrackerField summary;
        const std::shared_ptr<const std::vector<TrackerField>> fields = app->GetAvailableFields();
        for (const TrackerField& field : *fields) {
            if (field.Id == "summary") {
                summary = field;
            }
        }
        IM_CHECK(summary.Id == "summary");
        const std::shared_ptr<const std::vector<CachedTicket>> tickets = app->GetPaneTicketsSnapshot(paneId);
        IM_CHECK(tickets != nullptr && tickets->size() >= 5);
        const std::string single = (*tickets)[0].id;
        const std::vector<std::string> bulk = {(*tickets)[1].id, (*tickets)[2].id};
        const std::string originalDomain = g_ui.cfg.Domain;
        const std::string otherDomain = "undo-other-site.example.atlassian.net";

        BucketE::UiTestWriteScope writeScope;
        const SiteRestore siteRestore{*app, originalDomain};
        SmatchetToastManager::Instance().DismissAllLive();

        // Round 1, the bulk toast's Undo: a bulk entry with a single-cell entry on top of it.
        const std::uint64_t jobId =
            StartBulkFieldEdit(*app, g_ui, paneId, summary, bulk, std::vector<std::string>{"Bulk before switch"},
                               smatchet::grid::bulk::BulkMode::Replace, false);
        IM_CHECK(jobId != 0);
        const std::uint64_t bulkEntry = g_ui.bulkFieldEditJobs[jobId].UndoEntryId;
        IM_CHECK(bulkEntry != 0);
        IM_CHECK(YieldUntil(
            ctx, 900, [&] { return EditQueueIdle() && SummaryInPane(*app, paneId, bulk[1]) == "Bulk before switch"; }));
        const std::uint64_t first = SaveSingleEdit(ctx, *app, paneId, summary, single, "Single before switch");
        IM_CHECK(first != 0 && first != bulkEntry);
        IM_CHECK(g_ui.gridUndo.Contains(bulkEntry));
        SmatchetToastManager::Instance().DismissAllLive();

        IM_CHECK(ShowSite(ctx, *app, paneId, otherDomain));
        const std::size_t jobs = g_ui.bulkFieldEditJobs.size();
        UndoGridEntryById(*app, g_ui, bulkEntry);
        IM_CHECK_NO_RET(EditQueueIdle());
        IM_CHECK_NO_RET(g_ui.bulkFieldEditJobs.size() == jobs);
        IM_CHECK_NO_RET(!g_ui.gridUndo.Contains(bulkEntry));
        IM_CHECK_NO_RET(!g_ui.gridUndo.Contains(first));
        IM_CHECK(ShowSite(ctx, *app, paneId, originalDomain));

        // Round 2, the grid's Ctrl+Z.
        const std::uint64_t second = SaveSingleEdit(ctx, *app, paneId, summary, (*tickets)[3].id, "Second round");
        IM_CHECK(second != 0);
        SmatchetToastManager::Instance().DismissAllLive();
        IM_CHECK(ShowSite(ctx, *app, paneId, otherDomain));
        IM_CHECK_NO_RET(!UndoNewestGridEntry(*app, g_ui, paneId));
        IM_CHECK_NO_RET(EditQueueIdle());
        IM_CHECK_NO_RET(!g_ui.gridUndo.Contains(second));
        IM_CHECK(ShowSite(ctx, *app, paneId, originalDomain));

        // Round 3, away and back under a new client: the entry is still the pane's to undo.
        const std::string cell = (*tickets)[4].id;
        const std::string before = SummaryInPane(*app, paneId, cell);
        const std::uint64_t third = SaveSingleEdit(ctx, *app, paneId, summary, cell, "Third round");
        IM_CHECK(third != 0);
        SmatchetToastManager::Instance().DismissAllLive();
        IM_CHECK(ShowSite(ctx, *app, paneId, otherDomain));
        IM_CHECK(ShowSite(ctx, *app, paneId, originalDomain));
        // The fixture's new client serves the old value again, so save the value once more for the cell to show what
        // the entry wrote; its undo must then put the value from before it back.
        IM_CHECK(SaveSingleEdit(ctx, *app, paneId, summary, cell, "Third round") != 0);
        IM_CHECK_NO_RET(g_ui.gridUndo.Contains(third));
        UndoGridEntryById(*app, g_ui, third);
        IM_CHECK_NO_RET(!g_ui.gridUndo.Contains(third));
        IM_CHECK_NO_RET(
            YieldUntil(ctx, 900, [&] { return EditQueueIdle() && SummaryInPane(*app, paneId, cell) == before; }));
        SmatchetToastManager::Instance().DismissAllLive();
    };
}

extern "C" void SmatchetRegisterGridUndoWrongTargetTests(ImGuiTestEngine* engine) {
    RegisterUndoAfterSiteSwitch(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
