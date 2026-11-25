// bulk_field_edit.test.cpp — bucket-E, end to end on the real ticket grid (deterministic Jira fixture
// backend): the "Set <Field> for N rows..." bulk edit (docs/plans/bulk-edit-rows.md, P1).
//
// What this pins:
//   1. The cell context menu offers "Set Priority for 5 rows..." on a choice-field cell inside a selection
//      of two or more rows, and does not on a single-row selection, an unselected row or a field the bulk
//      edit cannot set (text).
//   2. Picking a value in the popup sends every row through the normal commit pipeline (one backend
//      UpdateIssueFields per row), the job ends Done with the expected Saved count, the summary toast is
//      pushed, the cells show the new value, and the visible selection is rebuilt by issue id after the
//      snapshot revision bump cleared it (KeepIssueIds).
//   3. More than 10 rows ask for the confirm step first (nothing is sent until Apply), and rows that are not
//      editable or already hold the value are listed as skipped and are not sent.
//   4. With the tracker unreachable the rows go to the offline queue and the job ends with Queued rows.
//
// Drive: the real right-click menu, the real popup (type to filter, Enter to pick) and the real confirm button.
// The selection is set on the pane directly (RectSel.Rows, stamped with the grid's sort signature): that is the
// state a ctrl/shift-click on the ID column produces, without depending on that gesture's geometry.
//
// Needs the bulk-edit-rows fixture (12 issues, a priority catalog) named by SMATCHET_TEST_JIRA_BACKEND_FIXTURE;
// scripts/dev/test-ui-bulk-field-edit.sh generates it and sets the BulkFieldEdit filter. Skips cleanly without it.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "Config/ConfigManager.h"
#include "FakeNetworkSwitch.h"
#include "FakeTrackerClient.h"
#include "GridPane.h"
#include "SmatchetGridUiSupport.h" // ComputeGridSortSignature
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "Ui/BulkFieldEditPure.h"
#include "Ui/SmatchetToast.h"
#include "UiTestWriteScope.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

// A check whose failure must stop the test: later lines dereference what it guards.
#define BULK_REQUIRE(cond)                                                                                             \
    do {                                                                                                               \
        const bool bulkRequireOk_ = static_cast<bool>(cond);                                                           \
        IM_CHECK_NO_RET(bulkRequireOk_);                                                                               \
        if (!bulkRequireOk_) {                                                                                         \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

namespace {

using smatchet::grid::bulk::BulkFieldEditJob;
using smatchet::grid::bulk::BulkRowState;
using smatchet::grid::bulk::BulkSummary;

const char* const kGridWindow = "Smatchet - Active Project";
const char* const kPriorityKey = "field:priority";
const char* const kSummaryKey = "field:summary";
const char* const kLabelsKey = "field:labels";
const char* const kStatusKey = "field:status";
const char* const kPriorityField = "priority";
const char* const kLabelsField = "labels";
constexpr int kFixtureRows = 12;

// True when the app was booted with the bulk-edit-rows fixture; otherwise logs a SKIP.
bool BulkFixtureActive(ImGuiTestContext* ctx) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fixture != nullptr && std::strstr(fixture, "bulk-edit-rows") != nullptr) {
        return true;
    }
    ctx->LogInfo("SKIP: bulk-edit-rows fixture not active");
    return false;
}

// Steps frames until `done()` holds or `budget` frames have passed; returns the final verdict.
template <typename Done> bool StepUntil(ImGuiTestContext* ctx, int budget, Done done) {
    while (!done() && budget-- > 0) {
        ctx->Yield();
    }
    return done();
}

GridPane& Pane() { return g_ui.gridPanes.front(); }

// The primary pane's live TicketGrid table (its scrolling child is named "<window>/TicketGrid_<id>").
ImGuiTable* LiveTicketGrid() {
    ImGuiContext& g = *GImGui;
    for (int n = 0; n < g.Tables.GetMapSize(); ++n) {
        ImGuiTable* table = g.Tables.TryGetMapData(n);
        if (table == nullptr || table->InnerWindow == nullptr || table->OuterWindow == nullptr ||
            table->LastFrameActive < g.FrameCount - 1) {
            continue;
        }
        const ImGuiWindow* root = table->OuterWindow->RootWindow;
        if (std::strstr(table->InnerWindow->Name, "/TicketGrid_") != nullptr && root != nullptr &&
            std::strcmp(root->Name, kGridWindow) == 0) {
            return table;
        }
    }
    return nullptr;
}

const std::vector<std::string>& DrawnKeys() { return Pane().lastDrawnColumnKeys; }

int IndexOfKey(const std::string& key) {
    const std::vector<std::string>& keys = DrawnKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The view.* handlers hop to the UI thread and block on it, so they run on a worker while this test keeps
// stepping frames.
smatchet::cmd::CommandResult RunCommand(ImGuiTestContext* ctx, AppController* app, const std::string& name,
                                        const nlohmann::json& args, bool dryRun = false) {
    std::future<smatchet::cmd::CommandResult> pending = std::async(std::launch::async, [app, name, args, dryRun] {
        smatchet::cmd::CommandContext cmdCtx;
        cmdCtx.ConfirmedDestructive = true;
        cmdCtx.DryRun = dryRun;
        cmdCtx.Threading = app; // a real caller always carries it; this worker thread is not the UI thread
        return app->Commands().Dispatch(name, args, cmdCtx);
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        ctx->Yield();
    }
    return pending.get();
}

bool WaitForConnectivity(ImGuiTestContext* ctx, AppController& app, TrackerConnectivityState want) {
    return StepUntil(ctx, 600, [&app, want]() {
        if (app.GetLastTrackerConnectivityState() == want) {
            return true;
        }
        app.RequestTrackerProbeNow();
        return false;
    });
}

// The pane's issue id at sort-order row `row`, "" when there is none.
std::string IdAtRow(int row) {
    GridPane& pane = Pane();
    if (!pane.ticketsSnapshot || row < 0 || static_cast<size_t>(row) >= pane.filteredIndices.size()) {
        return std::string();
    }
    const size_t ticketIdx = pane.filteredIndices[static_cast<size_t>(row)];
    return ticketIdx < pane.ticketsSnapshot->size() ? (*pane.ticketsSnapshot)[ticketIdx].id : std::string();
}

std::set<std::string> IdsAtRows(const std::set<int>& rows) {
    std::set<std::string> ids;
    for (int row : rows) {
        ids.insert(IdAtRow(row));
    }
    return ids;
}

std::string PriorityOf(AppController& app, const std::string& issueId) {
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.GetActiveTicketsSnapshot();
    if (!tickets) {
        return std::string();
    }
    for (const CachedTicket& ticket : *tickets) {
        if (ticket.id == issueId) {
            return ticket.GetFieldValue(kPriorityField);
        }
    }
    return std::string();
}

// True when the cached priority is `name` (a row the fixture gave it, or one left alone) or `optionId` (a row the
// commit pipeline saved: it caches the picked option's id).
bool PriorityIs(AppController& app, const std::string& issueId, const char* name, const char* optionId) {
    const std::string value = PriorityOf(app, issueId);
    return value == name || value == optionId;
}

std::uint64_t CurrentSortSignature() {
    const GridPane& pane = Pane();
    return ComputeGridSortSignature(pane.cachedSortFingerprint, pane.snapshotRevision,
                                    pane.ticketsSnapshot ? pane.ticketsSnapshot->size() : 0);
}

// The whole-row selection an ID-column ctrl/shift-click produces, valid for the grid's current order.
void SelectRows(const std::set<int>& rows) {
    GridRectSelection& sel = Pane().gridState.RectSel;
    sel.ClearAll();
    sel.KeepIssueIds.clear();
    sel.KeepRows.clear();
    sel.Rows = rows;
    sel.PrimaryRow = rows.empty() ? -1 : *rows.begin();
    sel.SortSignature = CurrentSortSignature();
}

std::set<int> FirstRows(int count) {
    std::set<int> rows;
    for (int i = 0; i < count; ++i) {
        rows.insert(i);
    }
    return rows;
}

// Waits for sync, a reachable tracker, the priority catalog and a drawn grid that shows the priority and summary
// columns and all fixture rows.
bool PrepareGrid(ImGuiTestContext* ctx, AppController* app) {
    app->SyncWithBackend();
    if (!StepUntil(ctx, 600, [&] { return !app->IsSyncPendingOrActive(); })) {
        return false;
    }
    if (!WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable)) {
        return false;
    }
    if (app->FindFieldById(kPriorityField) == nullptr) {
        app->RefreshFieldCatalog(ConfigManager::Load());
    }
    if (app->FindFieldById(kPriorityField) == nullptr ||
        app->FindFieldById(kPriorityField)->AllowedValueOptions.empty()) {
        return false;
    }
    ctx->SetRef(kGridWindow);
    const bool live = StepUntil(ctx, 600, [&] {
        g_ui.requestActiveProjectFocus = true;
        return LiveTicketGrid() != nullptr && !g_ui.gridPanes.empty() && !DrawnKeys().empty() &&
               Pane().ticketsSnapshot && static_cast<int>(Pane().ticketsSnapshot->size()) == kFixtureRows &&
               static_cast<int>(Pane().filteredIndices.size()) == kFixtureRows;
    });
    if (!live) {
        return false;
    }
    {
        // Always narrow the view to the columns the cases drive, so all of them sit inside the grid window (a view that
        // merely contains all three, behind other columns, still pushes labels past the right edge, where no click can
        // reach it).
        nlohmann::json fields = nlohmann::json::array();
        fields.push_back("priority");
        fields.push_back("summary");
        fields.push_back("labels");
        if (!RunCommand(ctx, app, "view.update", {{"fields", fields}}).Ok) {
            return false;
        }
        if (!StepUntil(ctx, 300, [] {
                return IndexOfKey(kPriorityKey) >= 0 && IndexOfKey(kSummaryKey) >= 0 && IndexOfKey(kLabelsKey) >= 0;
            })) {
            return false;
        }
    }
    ctx->Yield(10); // let the layout settle before any coordinate is read
    return LiveTicketGrid() != nullptr;
}

// Screen position of the middle of the cell (`columnKey`, sort-order `row`). The column gives x; y comes from the
// row's key-column item (labelled with the issue id), because the rendered row pitch is the widget height plus
// padding, not a constant a test can recompute.
bool CellCenter(ImGuiTestContext* ctx, const std::string& columnKey, int row, ImVec2* out) {
    ImGuiTable* table = LiveTicketGrid();
    const int index = IndexOfKey(columnKey);
    if (table == nullptr || index < 0 || index >= table->ColumnsCount) {
        return false;
    }
    const std::string id = IdAtRow(row);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, (std::string("//") + kGridWindow).c_str(), 99);
    for (const ImGuiTestItemInfo& item : items) {
        if (id == item.DebugLabel && item.RectFull.Min.y >= table->InnerClipRect.Min.y &&
            item.RectFull.Max.y <= table->InnerClipRect.Max.y) {
            const ImGuiTableColumn& column = table->Columns[index];
            out->x = 0.5f * (column.MinX + column.MaxX);
            out->y = item.RectFull.GetCenter().y;
            return true;
        }
    }
    return false;
}

// Right-clicks a data cell and lets the cell menu finish appearing.
bool RightClickCell(ImGuiTestContext* ctx, const std::string& columnKey, int row) {
    ctx->PopupCloseAll();
    ctx->Yield(2);
    ImVec2 pos;
    if (!CellCenter(ctx, columnKey, row, &pos)) {
        IM_CHECK_NO_RET(false); // the cell is not on screen
        return false;
    }
    ctx->MouseMoveToPos(pos);
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Right);
    ctx->Yield(3);
    return true;
}

std::string MenuItemRef(const char* fieldName, int rows) {
    return std::string("//$FOCUSED/") +
           SmatchetLocalization::Format("grid.bulk.menu_set", "Set %s for %d rows...", fieldName, rows);
}

// True when a right-click on (`columnKey`, `row`) offers the bulk-edit item for `rows` selected rows.
bool MenuOffersBulkItem(ImGuiTestContext* ctx, const std::string& columnKey, int row, const char* fieldName, int rows) {
    if (!RightClickCell(ctx, columnKey, row)) {
        return false;
    }
    const bool offered = ctx->ItemInfo(MenuItemRef(fieldName, rows).c_str(), ImGuiTestOpFlags_NoError).ID != 0;
    ctx->PopupCloseAll();
    ctx->Yield(2);
    return offered;
}

// Opens the popup through the menu (the selection must already be set) and returns once it is on screen.
bool OpenBulkPopup(ImGuiTestContext* ctx, const char* fieldName, int rows, int clickedRow,
                   const char* columnKey = kPriorityKey) {
    if (!RightClickCell(ctx, columnKey, clickedRow)) {
        return false;
    }
    const std::string ref = MenuItemRef(fieldName, rows);
    if (ctx->ItemInfo(ref.c_str(), ImGuiTestOpFlags_NoError).ID == 0) {
        IM_CHECK_NO_RET(false); // the menu item is missing
        ctx->PopupCloseAll();
        return false;
    }
    ctx->ItemClick(ref.c_str());
    ctx->Yield();
    const bool open =
        StepUntil(ctx, 60, [] { return g_ui.bulkEditPopupRequest.Active && !g_ui.bulkEditPopupRequest.OpenPending; });
    ctx->Yield(2);
    return open;
}

// Types `text` into the picker's filter and presses Enter on the first match.
void PickByTyping(ImGuiTestContext* ctx, const char* text) {
    ctx->KeyChars(text);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(3);
}

const BulkFieldEditJob* FindJob(std::uint64_t id) {
    const auto it = g_ui.bulkFieldEditJobs.find(id);
    return it == g_ui.bulkFieldEditJobs.end() ? nullptr : &it->second;
}

bool WaitJobDone(ImGuiTestContext* ctx, std::uint64_t id) {
    return StepUntil(ctx, 900, [id] {
        const BulkFieldEditJob* job = FindJob(id);
        return job != nullptr && smatchet::grid::bulk::Summarize(*job).Done;
    });
}

smatchet_tests::FakeTrackerClient* FakeBackend(AppController& app) {
    const std::shared_ptr<ITrackerBackend> backend = app.BackendShared();
    return dynamic_cast<smatchet_tests::FakeTrackerClient*>(backend.get());
}

// The newest toast whose title is `title`, or null.
const ToastHistoryEntry* LastToastTitled(const std::string& title) {
    const std::vector<ToastHistoryEntry>& history = SmatchetToastManager::Instance().History();
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->Title == title) {
            return &*it;
        }
    }
    return nullptr;
}

std::string BulkToastTitle(const char* fieldName) {
    return SmatchetLocalization::Format("grid.bulk.done_title", "Bulk edit: %s", fieldName);
}

std::string CountText(const char* key, const char* fallback, int count) {
    return SmatchetLocalization::Format(key, fallback, count);
}

// The first `count` sort-order rows, leaving out SMAT-12: an earlier case marks it not editable (its type is its own,
// so the editmeta change stays on that one issue), and these cases want every selected row to save.
std::set<int> EditableRows(int count) {
    std::set<int> rows;
    for (int row = 0; row < kFixtureRows && static_cast<int>(rows.size()) < count; ++row) {
        if (IdAtRow(row) != "SMAT-12") {
            rows.insert(row);
        }
    }
    return rows;
}

// The cached labels of `issueId`, as the grid shows them.
std::set<std::string> LabelsOf(AppController& app, const std::string& issueId) {
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.GetActiveTicketsSnapshot();
    std::set<std::string> out;
    if (!tickets) {
        return out;
    }
    for (const CachedTicket& ticket : *tickets) {
        if (ticket.id != issueId) {
            continue;
        }
        const std::string csv = ticket.GetFieldValue(kLabelsField);
        std::size_t start = 0;
        while (start <= csv.size()) {
            std::size_t comma = csv.find(',', start);
            if (comma == std::string::npos) {
                comma = csv.size();
            }
            std::string part = csv.substr(start, comma - start);
            const std::size_t first = part.find_first_not_of(' ');
            const std::size_t last = part.find_last_not_of(' ');
            if (first != std::string::npos) {
                out.insert(part.substr(first, last - first + 1));
            }
            start = comma + 1;
        }
    }
    return out;
}

// The values one recorded bulk call sent to the backend.
std::set<std::string> SentValues(const nlohmann::json& fields) {
    std::set<std::string> out;
    if (fields.is_object() && fields.contains("values") && fields["values"].is_array()) {
        for (const nlohmann::json& value : fields["values"]) {
            if (value.is_string()) {
                out.insert(value.get<std::string>());
            }
        }
    }
    return out;
}

// `holders` rows that carry `label` and `others` rows that do not, picked by what they hold so a sort saved in the
// profile cannot change the mix (SMAT-12 left out, as in EditableRows).
std::set<int> RowsByLabel(AppController& app, const std::string& label, int holders, int others) {
    std::set<int> rows;
    int haveHolders = 0;
    int haveOthers = 0;
    for (int row = 0; row < kFixtureRows; ++row) {
        const std::string id = IdAtRow(row);
        if (id.empty() || id == "SMAT-12") {
            continue;
        }
        const bool holds = LabelsOf(app, id).count(label) != 0;
        if (holds && haveHolders < holders) {
            rows.insert(row);
            ++haveHolders;
        } else if (!holds && haveOthers < others) {
            rows.insert(row);
            ++haveOthers;
        }
    }
    return rows;
}

// A button / radio of the open picker, addressed the way the app labels it (text + ### id).
std::string PopupItemRef(const char* key, const char* fallback, const char* id) {
    return std::string("//$FOCUSED/") + SmatchetLocalization::Label(key, fallback, id);
}

bool PopupItemExists(ImGuiTestContext* ctx, const std::string& ref) {
    return ctx->ItemInfo(ref.c_str(), ImGuiTestOpFlags_NoError).ID != 0;
}

// The item is drawn and is not disabled (an item that vanished is neither enabled nor disabled).
bool PopupItemEnabled(ImGuiTestContext* ctx, const std::string& ref) {
    const ImGuiTestItemInfo info = ctx->ItemInfo(ref.c_str(), ImGuiTestOpFlags_NoError);
    return info.ID != 0 && (info.ItemFlags & ImGuiItemFlags_Disabled) == 0;
}

bool PopupItemDisabled(ImGuiTestContext* ctx, const std::string& ref) {
    const ImGuiTestItemInfo info = ctx->ItemInfo(ref.c_str(), ImGuiTestOpFlags_NoError);
    return info.ID != 0 && (info.ItemFlags & ImGuiItemFlags_Disabled) != 0;
}

std::string AddModeRef() { return PopupItemRef("grid.bulk.mode_add", "Add", "bulk_mode_add"); }
std::string RemoveModeRef() { return PopupItemRef("grid.bulk.mode_remove", "Remove", "bulk_mode_remove"); }
std::string ReplaceModeRef() { return PopupItemRef("grid.bulk.mode_replace", "Replace", "bulk_mode_replace"); }
std::string PickApplyRef() { return PopupItemRef("grid.bulk.apply", "Apply", "bulk_pick_apply"); }
std::string PickCancelRef() { return PopupItemRef("grid.bulk.cancel", "Cancel", "bulk_pick_cancel"); }

// Everything a test needs to start from the same state: the grid is up and the popup request is idle.
struct Env {
    AppController* App = nullptr;
    smatchet_tests::FakeTrackerClient* Fake = nullptr;
};

bool Setup(ImGuiTestContext* ctx, Env* env) {
    if (!BulkFixtureActive(ctx)) {
        return false;
    }
    env->App = SmatchetActiveUiTestAppController();
    IM_CHECK_NO_RET(env->App != nullptr);
    if (env->App == nullptr) {
        return false;
    }
    env->Fake = FakeBackend(*env->App);
    IM_CHECK_NO_RET(env->Fake != nullptr);
    if (env->Fake == nullptr) {
        return false;
    }
    smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
    g_ui.bulkEditPopupRequest = BulkEditPopupRequest();
    const bool ready = PrepareGrid(ctx, env->App);
    IM_CHECK_NO_RET(ready);
    return ready;
}

} // namespace

// BulkFieldEdit/Menu_ItemShownOnlyForMultiRowChoiceField: the item needs a selection of two or more rows that holds
// the clicked row, and a column the bulk edit can set.
static void RegisterMenuItemVisibility(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Menu_ItemShownOnlyForMultiRowChoiceField");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);

        // Control: the five selected rows offer it on the priority cell of the middle row.
        SelectRows(FirstRows(5));
        IM_CHECK_NO_RET(MenuOffersBulkItem(ctx, kPriorityKey, 2, "Priority", 5));
        IM_CHECK_NO_RET(Pane().gridState.RectSel.Rows.size() == 5); // the right-click left the selection alone

        // Geometry control: the click lands on row 2 and no other row (selected {2,3} and {1,2} both offer it).
        SelectRows({2, 3});
        IM_CHECK_NO_RET(MenuOffersBulkItem(ctx, kPriorityKey, 2, "Priority", 2));
        SelectRows({1, 2});
        IM_CHECK_NO_RET(MenuOffersBulkItem(ctx, kPriorityKey, 2, "Priority", 2));

        // A single selected row: no item.
        SelectRows({2});
        IM_CHECK_NO_RET(!MenuOffersBulkItem(ctx, kPriorityKey, 2, "Priority", 1));

        // A text field (summary) cannot be set in bulk: no item although five rows are selected.
        SelectRows(FirstRows(5));
        IM_CHECK_NO_RET(!MenuOffersBulkItem(ctx, kSummaryKey, 2, "Summary", 5));

        // A click on a row outside the selection: no item.
        IM_CHECK_NO_RET(!MenuOffersBulkItem(ctx, kPriorityKey, 8, "Priority", 5));
        SelectRows({});
    };
}

// BulkFieldEdit/Pick_SavesEveryRowAndKeepsSelection: five rows, pick "Trivial" in the popup.
static void RegisterPickSavesEveryRow(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Pick_SavesEveryRowAndKeepsSelection");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SmatchetToastManager::Instance().ClearHistory();

        const std::set<int> rows = FirstRows(5);
        SelectRows(rows);
        const std::set<std::string> selectedIds = IdsAtRows(rows);
        IM_CHECK_NO_RET(selectedIds.size() == 5);
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t revisionBefore = Pane().snapshotRevision;
        const std::uint64_t jobId = g_ui.nextBulkJobId;
        std::set<std::string> otherIds;
        for (int row = 5; row < kFixtureRows; ++row) {
            otherIds.insert(IdAtRow(row));
        }
        std::vector<std::string> otherBefore;
        for (const std::string& id : otherIds) {
            otherBefore.push_back(PriorityOf(*env.App, id));
        }

        BULK_REQUIRE(OpenBulkPopup(ctx, "Priority", 5, 2));
        const BulkEditPopupRequest& req = g_ui.bulkEditPopupRequest;
        IM_CHECK_NO_RET(req.Active);
        IM_CHECK_NO_RET(req.Field.Id == kPriorityField);
        IM_CHECK_NO_RET(req.PaneId == Pane().id);
        IM_CHECK_NO_RET(std::set<std::string>(req.IssueIds.begin(), req.IssueIds.end()) == selectedIds);

        PickByTyping(ctx, "Triv");
        BULK_REQUIRE(FindJob(jobId) != nullptr); // five rows: no confirm step, the job started
        BULK_REQUIRE(WaitJobDone(ctx, jobId));
        const BulkFieldEditJob job = *FindJob(jobId);
        const BulkSummary summary = smatchet::grid::bulk::Summarize(job);
        IM_CHECK_NO_RET(summary.Total == 5);
        IM_CHECK_NO_RET(summary.Saved == 5);
        IM_CHECK_NO_RET(summary.Queued == 0);
        IM_CHECK_NO_RET(summary.Failed == 0);
        IM_CHECK_NO_RET(summary.Skipped == 0);
        std::set<std::string> jobIds;
        for (const auto& row : job.Rows) {
            jobIds.insert(row.IssueId);
        }
        IM_CHECK_NO_RET(jobIds == selectedIds);

        // Every row went through the normal commit pipeline: one backend update each, none for the others.
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + 5);
        std::set<std::string> sentIds;
        for (std::size_t i = callsBefore; i < env.Fake->UpdateIssueFieldsCalls().size(); ++i) {
            const auto& call = env.Fake->UpdateIssueFieldsCalls()[i];
            sentIds.insert(call.IssueId);
            IM_CHECK_NO_RET(call.Fields.is_object() &&
                            call.Fields.contains("values")); // the picked value, one entry per row
        }
        IM_CHECK_NO_RET(sentIds == selectedIds);
        IM_CHECK_NO_RET(g_ui.queuedFieldEdits.empty());
        IM_CHECK_NO_RET(!g_ui.hasInFlightEdit);

        // The cells show the new value; the unselected rows are untouched.
        for (const std::string& id : selectedIds) {
            IM_CHECK_NO_RET(PriorityIs(*env.App, id, "Trivial", "5"));
        }
        std::size_t k = 0;
        for (const std::string& id : otherIds) {
            IM_CHECK_NO_RET(PriorityOf(*env.App, id) == otherBefore[k++]);
        }

        // One summary toast with the counts.
        const ToastHistoryEntry* toast = LastToastTitled(BulkToastTitle("Priority"));
        IM_CHECK_NO_RET(toast != nullptr);
        if (toast != nullptr) {
            IM_CHECK_NO_RET(toast->Message.find(CountText("grid.bulk.n_saved", "%d saved", 5)) != std::string::npos);
        }

        // The first landed edit bumped the snapshot revision, which cleared the sort-order selection; the same five
        // issues are selected again, by id, in the new order.
        ctx->Yield(10);
        IM_CHECK_NO_RET(Pane().snapshotRevision != revisionBefore);
        const GridRectSelection& sel = Pane().gridState.RectSel;
        IM_CHECK_NO_RET(sel.Rows.size() == 5);
        IM_CHECK_NO_RET(IdsAtRows(sel.Rows) == selectedIds);
        IM_CHECK_NO_RET(sel.SortSignature == CurrentSortSignature());
        SelectRows({});
    };
}

// BulkFieldEdit/Over10Rows_ConfirmThenApplySkipsUneditableAndUnchanged: all twelve rows, pick "High".
static void RegisterConfirmStep(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Over10Rows_ConfirmThenApplySkipsUneditableAndUnchanged");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SmatchetToastManager::Instance().ClearHistory();

        const std::set<int> rows = FirstRows(kFixtureRows);
        SelectRows(rows);
        const std::set<std::string> selectedIds = IdsAtRows(rows);
        IM_CHECK_NO_RET(selectedIds.size() == static_cast<std::size_t>(kFixtureRows));

        // One row the tracker says is not editable (its editmeta lacks priority), and every row that already
        // holds "High": none of them is sent. SMAT-12 is the only issue of its type (fixture), because editmeta is
        // cached per type: marking a Task read-only would make every row read-only.
        const std::string uneditableId = "SMAT-12";
        std::set<std::string> expectedSkipped;
        IM_CHECK_NO_RET(selectedIds.count(uneditableId) == 1 && PriorityOf(*env.App, uneditableId) != "High");
        env.Fake->SetIssueEditMetaSuccess(uneditableId, {{kPriorityField, false}});
        IM_CHECK_NO_RET(env.App->EnsureIssueEditMetaLoaded(uneditableId).has_value());
        IM_CHECK_NO_RET(!env.App->CanEditFieldForIssue(uneditableId, kPriorityField, nullptr));
        for (const std::string& id : selectedIds) {
            if (id == uneditableId || PriorityOf(*env.App, id) == "High") {
                expectedSkipped.insert(id);
            }
        }
        const int expectedSaved = kFixtureRows - static_cast<int>(expectedSkipped.size());
        IM_CHECK_NO_RET(expectedSaved > 0);

        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t jobId = g_ui.nextBulkJobId;
        BULK_REQUIRE(OpenBulkPopup(ctx, "Priority", kFixtureRows, 2));

        // The pick step already lists the rows it will skip, each with its reason.
        {
            const BulkEditPopupRequest& req = g_ui.bulkEditPopupRequest;
            IM_CHECK_NO_RET(req.Preview.Rows.size() == static_cast<std::size_t>(kFixtureRows));
            bool uneditableListed = false;
            for (const auto& row : req.Preview.Rows) {
                if (row.State == BulkRowState::Skipped && row.IssueId == uneditableId) {
                    uneditableListed = row.Message == SmatchetLocalization::T("grid.bulk.skip_not_editable",
                                                                              "not editable on this issue");
                }
            }
            IM_CHECK_NO_RET(uneditableListed);
        }

        PickByTyping(ctx, "High");

        // More than 10 rows: the confirm step is up and nothing was sent yet.
        {
            const BulkEditPopupRequest& req = g_ui.bulkEditPopupRequest;
            IM_CHECK_NO_RET(req.Active);
            IM_CHECK_NO_RET(req.CurrentStage == BulkEditPopupRequest::Stage::Confirm);
            IM_CHECK_NO_RET(FindJob(jobId) == nullptr);
            IM_CHECK_NO_RET(g_ui.nextBulkJobId == jobId);
            IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore);
            std::set<std::string> listed;
            for (const auto& row : req.Preview.Rows) {
                if (row.State == BulkRowState::Skipped) {
                    listed.insert(row.IssueId);
                }
            }
            IM_CHECK_NO_RET(listed == expectedSkipped);
            const BulkSummary preview = smatchet::grid::bulk::Summarize(req.Preview);
            IM_CHECK_NO_RET(preview.Remaining == expectedSaved);
            IM_CHECK_NO_RET(preview.Skipped == static_cast<int>(expectedSkipped.size()));
        }

        const std::string applyRef =
            std::string("//$FOCUSED/") + SmatchetLocalization::Label("grid.bulk.apply", "Apply", "bulk_confirm_apply");
        IM_CHECK_NO_RET(ctx->ItemInfo(applyRef.c_str(), ImGuiTestOpFlags_NoError).ID != 0);
        ctx->ItemClick(applyRef.c_str());
        ctx->Yield(2);
        BULK_REQUIRE(FindJob(jobId) != nullptr);
        BULK_REQUIRE(WaitJobDone(ctx, jobId));

        const BulkFieldEditJob job = *FindJob(jobId);
        const BulkSummary summary = smatchet::grid::bulk::Summarize(job);
        IM_CHECK_NO_RET(summary.Total == kFixtureRows);
        IM_CHECK_NO_RET(summary.Saved == expectedSaved);
        IM_CHECK_NO_RET(summary.Skipped == static_cast<int>(expectedSkipped.size()));
        IM_CHECK_NO_RET(summary.Failed == 0);
        std::set<std::string> skippedInJob;
        for (const auto& row : job.Rows) {
            if (row.State == BulkRowState::Skipped) {
                skippedInJob.insert(row.IssueId);
            }
        }
        IM_CHECK_NO_RET(skippedInJob == expectedSkipped);

        // Only the saved rows reached the backend.
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() ==
                        callsBefore + static_cast<std::size_t>(expectedSaved));
        for (std::size_t i = callsBefore; i < env.Fake->UpdateIssueFieldsCalls().size(); ++i) {
            IM_CHECK_NO_RET(expectedSkipped.count(env.Fake->UpdateIssueFieldsCalls()[i].IssueId) == 0);
        }
        for (const std::string& id : selectedIds) {
            // Every row but the uneditable one now holds "High"; the uneditable one kept its old value.
            IM_CHECK_NO_RET(PriorityIs(*env.App, id, "High", "2") == (id != uneditableId));
        }

        const ToastHistoryEntry* toast = LastToastTitled(BulkToastTitle("Priority"));
        IM_CHECK_NO_RET(toast != nullptr);
        if (toast != nullptr) {
            IM_CHECK_NO_RET(toast->Message.find(CountText("grid.bulk.n_saved", "%d saved", expectedSaved)) !=
                            std::string::npos);
            IM_CHECK_NO_RET(toast->Message.find(CountText("grid.bulk.n_skipped", "%d skipped",
                                                          static_cast<int>(expectedSkipped.size()))) !=
                            std::string::npos);
        }
        ctx->Yield(10);
        IM_CHECK_NO_RET(IdsAtRows(Pane().gridState.RectSel.Rows) == selectedIds);
        SelectRows({});
    };
}

// BulkFieldEdit/Offline_RowsQueueAndJobEndsQueued: the tracker is unreachable, so every row is saved to the offline
// queue instead of being sent, and the job reports them as queued.
static void RegisterOfflineQueues(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Offline_RowsQueueAndJobEndsQueued");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        BucketE::UiTestWriteScope writeScope; // the queue write must land: a rejected write would pass vacuously
        ctx->SetRef(kGridWindow);
        SmatchetToastManager::Instance().ClearHistory();

        const std::set<int> rows = FirstRows(3);
        SelectRows(rows);
        const std::set<std::string> selectedIds = IdsAtRows(rows);
        IM_CHECK_NO_RET(selectedIds.size() == 3);
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t jobId = g_ui.nextBulkJobId;

        // Open the popup while online (the menu needs no network), then take the tracker down and pick.
        BULK_REQUIRE(OpenBulkPopup(ctx, "Priority", 3, 1));
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *env.App, TrackerConnectivityState::TransportDown));
        PickByTyping(ctx, "Crit");
        BULK_REQUIRE(FindJob(jobId) != nullptr);
        BULK_REQUIRE(WaitJobDone(ctx, jobId));

        const BulkSummary summary = smatchet::grid::bulk::Summarize(*FindJob(jobId));
        IM_CHECK_NO_RET(summary.Total == 3);
        IM_CHECK_NO_RET(summary.Queued == 3);
        IM_CHECK_NO_RET(summary.Saved == 0);
        IM_CHECK_NO_RET(summary.Failed == 0);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore); // nothing reached the tracker

        // The three rows wait in the offline queue, one per issue.
        std::set<std::string> queuedIds;
        std::vector<std::int64_t> queuedRowIds;
        for (const PendingFieldEditRecord& rec : env.App->GetPendingFieldEdits()) {
            if (rec.FieldId == kPriorityField && selectedIds.count(rec.IssueKey) != 0) {
                queuedIds.insert(rec.IssueKey);
                queuedRowIds.push_back(rec.Id);
            }
        }
        // Never leak the rows into a retry or a later test.
        if (!queuedRowIds.empty()) {
            env.App->DeletePendingFieldEdits(queuedRowIds);
        }
        IM_CHECK_NO_RET(queuedIds == selectedIds);

        const ToastHistoryEntry* toast = LastToastTitled(BulkToastTitle("Priority"));
        IM_CHECK_NO_RET(toast != nullptr);
        if (toast != nullptr) {
            IM_CHECK_NO_RET(toast->Message.find(CountText("grid.bulk.n_queued", "%d queued", 3)) != std::string::npos);
        }
        SelectRows({});
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *env.App, TrackerConnectivityState::AuthenticatedReachable));
    };
}

// BulkFieldEdit/Command_DryRunThenSetOnAWorkerThread: ticket.set_field_bulk from a non-UI thread (the CLI / MCP shape)
// plans from the cache on --dry-run without touching the tracker, then writes every ticket and reports per row.
static void RegisterCommandPath(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Command_DryRunThenSetOnAWorkerThread");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const TrackerField* priority = env.App->FindFieldById(kPriorityField);
        BULK_REQUIRE(priority != nullptr);
        std::string trivialId;
        for (const TrackerFieldOption& option : priority->AllowedValueOptions) {
            if (option.Value == "Trivial") {
                trivialId = option.Id;
            }
        }
        BULK_REQUIRE(!trivialId.empty());
        nlohmann::json ids = nlohmann::json::array();
        for (const std::string& id : IdsAtRows(FirstRows(3))) {
            ids.push_back(id);
        }
        const nlohmann::json args = {{"ids", ids}, {"field", kPriorityField}, {"value", trivialId}};
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();

        const smatchet::cmd::CommandResult dry = RunCommand(ctx, env.App, "ticket.set_field_bulk", args, true);
        BULK_REQUIRE(dry.Ok && dry.Data != nullptr);
        const nlohmann::json& planned = (*dry.Data)["wouldDo"]["rows"];
        IM_CHECK_NO_RET(planned.is_array() && planned.size() == 3);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore); // a dry run sends nothing

        // A ticket that already holds the value is planned as skipped, not written again: "High" is on the odd keys.
        {
            const smatchet::cmd::CommandResult same = RunCommand(
                ctx, env.App, "ticket.set_field_bulk", {{"ids", ids}, {"field", kPriorityField}, {"value", "2"}}, true);
            BULK_REQUIRE(same.Ok && same.Data != nullptr);
            for (const nlohmann::json& entry : (*same.Data)["wouldDo"]["rows"]) {
                const std::string id = entry["id"].get<std::string>();
                const bool wasHigh = std::stoi(id.substr(id.find('-') + 1)) % 2 == 1;
                IM_CHECK_NO_RET(entry.contains("skip") == wasHigh);
            }
        }

        const smatchet::cmd::CommandResult run = RunCommand(ctx, env.App, "ticket.set_field_bulk", args);
        BULK_REQUIRE(run.Ok && run.Data != nullptr);
        IM_CHECK_NO_RET((*run.Data)["saved"].get<int>() + (*run.Data)["queued"].get<int>() == 3);
        IM_CHECK_NO_RET((*run.Data)["failed"].get<int>() == 0);
        IM_CHECK_NO_RET((*run.Data)["rows"].size() == 3);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + 3);

        // Bad input is refused before anything is sent.
        const smatchet::cmd::CommandResult bad =
            RunCommand(ctx, env.App, "ticket.set_field_bulk",
                       {{"ids", nlohmann::json::array()}, {"field", kPriorityField}, {"value", "1"}});
        IM_CHECK_NO_RET(!bad.Ok);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + 3);
    };
}

// BulkFieldEdit/Labels_ModeToggleGatesApply: the Replace / Add / Remove radios set the request's mode, and Apply stays
// disabled until something is ticked (or, in Replace mode, <clear all> is chosen).
static void RegisterLabelsModeToggle(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Labels_ModeToggleGatesApply");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const std::set<int> rows = EditableRows(4);
        SelectRows(rows);
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();

        BULK_REQUIRE(OpenBulkPopup(ctx, "Labels", 4, *rows.begin(), kLabelsKey));
        const BulkEditPopupRequest& req = g_ui.bulkEditPopupRequest;
        IM_CHECK_NO_RET(req.Mode == smatchet::grid::bulk::BulkMode::Replace);
        IM_CHECK_NO_RET(PopupItemExists(ctx, ReplaceModeRef()));
        IM_CHECK_NO_RET(PopupItemDisabled(ctx, PickApplyRef())); // nothing ticked yet

        ctx->ItemClick(AddModeRef().c_str());
        ctx->Yield(2);
        IM_CHECK_NO_RET(req.Mode == smatchet::grid::bulk::BulkMode::Add);
        IM_CHECK_NO_RET(PopupItemDisabled(ctx, PickApplyRef()));

        ctx->ItemClick(RemoveModeRef().c_str());
        ctx->Yield(2);
        IM_CHECK_NO_RET(req.Mode == smatchet::grid::bulk::BulkMode::Remove);

        ctx->ItemClick(ReplaceModeRef().c_str());
        ctx->Yield(2);
        IM_CHECK_NO_RET(req.Mode == smatchet::grid::bulk::BulkMode::Replace);

        // A ticked label enables Apply in every mode.
        ctx->KeyChars("zeta");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_NO_RET(PopupItemEnabled(ctx, PickApplyRef()));
        ctx->ItemClick(AddModeRef().c_str());
        ctx->Yield(2);
        IM_CHECK_NO_RET(PopupItemEnabled(ctx, PickApplyRef()));

        // Cancel closes the request and sends nothing.
        ctx->ItemClick(PickCancelRef().c_str());
        ctx->Yield(3);
        IM_CHECK_NO_RET(!g_ui.bulkEditPopupRequest.Active);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore);
        SelectRows({});
    };
}

// BulkFieldEdit/Labels_AddModeMergesIntoEachRow: type a new label, tick it with Enter, switch to Add, press Apply.
// Every row keeps the labels it had and gains the new one.
static void RegisterLabelsAddMode(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Labels_AddModeMergesIntoEachRow");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SmatchetToastManager::Instance().ClearHistory();

        const std::set<int> rows = EditableRows(4);
        SelectRows(rows);
        const std::set<std::string> selectedIds = IdsAtRows(rows);
        IM_CHECK_NO_RET(selectedIds.size() == 4);
        std::map<std::string, std::set<std::string>> before;
        for (const std::string& id : selectedIds) {
            before[id] = LabelsOf(*env.App, id);
        }
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t jobId = g_ui.nextBulkJobId;

        BULK_REQUIRE(OpenBulkPopup(ctx, "Labels", 4, *rows.begin(), kLabelsKey));
        ctx->ItemClick(AddModeRef().c_str());
        ctx->Yield(2);
        ctx->KeyChars("beta");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        {
            const BulkEditPopupRequest& req = g_ui.bulkEditPopupRequest;
            IM_CHECK_NO_RET(req.Active);
            IM_CHECK_NO_RET(req.Picked.size() == 1 && req.Picked.front() == "beta");
            IM_CHECK_NO_RET(FindJob(jobId) == nullptr); // Enter ticked the label; nothing was sent yet
        }
        BULK_REQUIRE(PopupItemExists(ctx, PickApplyRef()));
        ctx->ItemClick(PickApplyRef().c_str());
        ctx->Yield(2);
        BULK_REQUIRE(FindJob(jobId) != nullptr);
        BULK_REQUIRE(WaitJobDone(ctx, jobId));

        const BulkSummary summary = smatchet::grid::bulk::Summarize(*FindJob(jobId));
        IM_CHECK_NO_RET(summary.Total == 4);
        IM_CHECK_NO_RET(summary.Saved == 4);
        IM_CHECK_NO_RET(summary.Failed == 0);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + 4);
        for (std::size_t i = callsBefore; i < env.Fake->UpdateIssueFieldsCalls().size(); ++i) {
            const auto& call = env.Fake->UpdateIssueFieldsCalls()[i];
            std::set<std::string> want = before[call.IssueId]; // Add keeps what the row had, plus the new label
            want.insert("beta");
            IM_CHECK_NO_RET(SentValues(call.Fields) == want);
        }
        for (const std::string& id : selectedIds) {
            std::set<std::string> want = before[id];
            want.insert("beta");
            IM_CHECK_NO_RET(LabelsOf(*env.App, id) == want);
        }
        const ToastHistoryEntry* toast = LastToastTitled(BulkToastTitle("Labels"));
        IM_CHECK_NO_RET(toast != nullptr);
        SelectRows({});
    };
}

// BulkFieldEdit/Labels_RemoveModeSkipsRowsWithoutTheLabel: Remove "alpha" from four rows. Rows that hold it are saved
// without it; the others have nothing to remove, so they are skipped and not sent.
static void RegisterLabelsRemoveMode(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Labels_RemoveModeSkipsRowsWithoutTheLabel");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);

        const std::set<int> rows = RowsByLabel(*env.App, "alpha", 2, 2);
        IM_CHECK_NO_RET(rows.size() == 4); // fixture must offer 2 alpha holders + 2 other rows
        SelectRows(rows);
        const std::set<std::string> selectedIds = IdsAtRows(rows);
        std::set<std::string> holders;
        std::map<std::string, std::set<std::string>> before;
        for (const std::string& id : selectedIds) {
            before[id] = LabelsOf(*env.App, id);
            if (before[id].count("alpha") != 0) {
                holders.insert(id);
            }
        }
        IM_CHECK_NO_RET(!holders.empty() && holders.size() < selectedIds.size()); // the fixture mixes both kinds
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t jobId = g_ui.nextBulkJobId;

        BULK_REQUIRE(OpenBulkPopup(ctx, "Labels", 4, *rows.begin(), kLabelsKey));
        ctx->ItemClick(RemoveModeRef().c_str());
        ctx->Yield(2);
        ctx->KeyChars("alpha");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        BULK_REQUIRE(PopupItemExists(ctx, PickApplyRef()));
        ctx->ItemClick(PickApplyRef().c_str());
        ctx->Yield(2);
        BULK_REQUIRE(FindJob(jobId) != nullptr);
        BULK_REQUIRE(WaitJobDone(ctx, jobId));

        const BulkSummary summary = smatchet::grid::bulk::Summarize(*FindJob(jobId));
        IM_CHECK_NO_RET(summary.Saved == static_cast<int>(holders.size()));
        IM_CHECK_NO_RET(summary.Skipped == static_cast<int>(selectedIds.size() - holders.size()));
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + holders.size());
        for (std::size_t i = callsBefore; i < env.Fake->UpdateIssueFieldsCalls().size(); ++i) {
            const auto& call = env.Fake->UpdateIssueFieldsCalls()[i];
            IM_CHECK_NO_RET(holders.count(call.IssueId) == 1);
            std::set<std::string> want = before[call.IssueId]; // what the backend got: the row's labels minus alpha
            want.erase("alpha");
            IM_CHECK_NO_RET(call.Fields.contains("values")); // an empty `want` must not match a missing payload
            IM_CHECK_NO_RET(SentValues(call.Fields) == want);
        }
        for (const std::string& id : selectedIds) {
            std::set<std::string> want = before[id];
            want.erase("alpha");
            IM_CHECK_NO_RET(LabelsOf(*env.App, id) == want);
        }
        SelectRows({});
    };
}

// BulkFieldEdit/GridSearchFocusRequestKeepsDialogOpen: grid.search.focus (Ctrl+F or /) asks the grid header to take the
// caret. While a popup is open the request is dropped: focusing the grid window would close the bulk dialog.
static void RegisterGridSearchFocusKeepsModal(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "GridSearchFocusRequestKeepsDialogOpen");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const std::set<int> rows = EditableRows(3);
        SelectRows(rows);

        BULK_REQUIRE(OpenBulkPopup(ctx, "Labels", 3, *rows.begin(), kLabelsKey));
        g_ui.focusedPane().gridSearchFocusFramesLeft = 3; // what grid.search.focus sets
        ctx->Yield(4);
        IM_CHECK_NO_RET(g_ui.bulkEditPopupRequest.Active);
        IM_CHECK_NO_RET(PopupItemExists(ctx, PickApplyRef())); // the dialog is still drawn

        ctx->ItemClick(PickCancelRef().c_str());
        ctx->Yield(3);
        IM_CHECK_NO_RET(!g_ui.bulkEditPopupRequest.Active);
        SelectRows({});
    };
}

// BulkFieldEdit/Labels_EnterInTickBoxListKeepsModalOpen: the first Enter in the modal's tick-box list ticks the match
// and leaves the dialog open with its picks (the shared dropdown used to close the modal on Enter and lose them); the
// next Enter, with no row left to act on, finishes the list, and the dialog treats that as Apply.
static void RegisterLabelsEnterKeepsModal(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Labels_EnterInTickBoxListKeepsModalOpen");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const std::set<int> rows = EditableRows(3);
        SelectRows(rows);
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t jobId = g_ui.nextBulkJobId;

        BULK_REQUIRE(OpenBulkPopup(ctx, "Labels", 3, *rows.begin(), kLabelsKey));
        ctx->KeyChars("gam");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        {
            const BulkEditPopupRequest& req = g_ui.bulkEditPopupRequest;
            IM_CHECK_NO_RET(req.Active);
            IM_CHECK_NO_RET(req.CurrentStage == BulkEditPopupRequest::Stage::Pick);
            IM_CHECK_NO_RET(req.Picked.size() == 1 && req.Picked.front() == "gamma");
            IM_CHECK_NO_RET(PopupItemExists(ctx, PickApplyRef())); // the dialog is still drawn
            IM_CHECK_NO_RET(FindJob(jobId) == nullptr);
            IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore);
        }

        // A pick already made and nothing new matched: Enter only finishes the list, which applies the pick.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        BULK_REQUIRE(FindJob(jobId) != nullptr);
        BULK_REQUIRE(WaitJobDone(ctx, jobId));
        const BulkSummary summary = smatchet::grid::bulk::Summarize(*FindJob(jobId));
        IM_CHECK_NO_RET(summary.Saved == 3);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + 3);
        IM_CHECK_NO_RET(!g_ui.bulkEditPopupRequest.Active);
        SelectRows({});
    };
}

// BulkFieldEdit/Labels_EnterWithNothingTickedKeepsModalOpen: Enter in an empty tick-box list has nothing to apply, so
// the dialog stays open and sends nothing.
static void RegisterLabelsEnterNothingTicked(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Labels_EnterWithNothingTickedKeepsModalOpen");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const std::set<int> rows = EditableRows(3);
        SelectRows(rows);
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        const std::uint64_t jobId = g_ui.nextBulkJobId;

        BULK_REQUIRE(OpenBulkPopup(ctx, "Labels", 3, *rows.begin(), kLabelsKey));
        for (int pass = 0; pass < 2; ++pass) { // Replace, then Add
            ctx->KeyPress(ImGuiKey_Enter);
            ctx->Yield(3);
            IM_CHECK_NO_RET(g_ui.bulkEditPopupRequest.Active);
            IM_CHECK_NO_RET(g_ui.bulkEditPopupRequest.Picked.empty());
            IM_CHECK_NO_RET(PopupItemExists(ctx, PickApplyRef()));
            IM_CHECK_NO_RET(FindJob(jobId) == nullptr);
            IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore);
            if (pass == 0) {
                ctx->ItemClick(AddModeRef().c_str());
                ctx->Yield(2);
            }
        }

        ctx->ItemClick(PickCancelRef().c_str());
        ctx->Yield(3);
        IM_CHECK_NO_RET(!g_ui.bulkEditPopupRequest.Active);
        SelectRows({});
    };
}

// BulkFieldEdit/Status_OffersOnlyMovesEveryRowAllows: the status picker lists the moves every selected row can make, as
// one ticket's status combo does for itself, not every status of the catalog. Three rows script different moves; only
// "Done" is common to all of them.
static void RegisterStatusCommonMoves(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Status_OffersOnlyMovesEveryRowAllows");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        // Status and summary only: a third column would push the status cell toward the edge the clicks must reach.
        nlohmann::json fields = nlohmann::json::array();
        fields.push_back("status");
        fields.push_back("summary");
        BULK_REQUIRE(RunCommand(ctx, env.App, "view.update", {{"fields", fields}}).Ok);
        BULK_REQUIRE(StepUntil(ctx, 300, [] { return IndexOfKey(kStatusKey) >= 0 && IndexOfKey(kSummaryKey) >= 0; }));
        ctx->Yield(10);

        const std::set<int> rows = FirstRows(3);
        SelectRows(rows);
        std::vector<std::string> ids;
        for (int row : rows) {
            ids.push_back(IdAtRow(row));
        }
        BULK_REQUIRE(ids.size() == 3 && !ids[0].empty() && !ids[1].empty() && !ids[2].empty());
        const auto move = [](const char* id, const char* name) {
            TrackerFieldOption option;
            option.Id = id;
            option.Value = name;
            return option;
        };
        env.Fake->SetSupportsIssueTransitions(true);
        env.Fake->SetIssueTransitions(ids[0], {move("11", "In Progress"), move("21", "Done"), move("31", "Closed")});
        env.Fake->SetIssueTransitions(ids[1], {move("11", "In Progress"), move("21", "Done")});
        env.Fake->SetIssueTransitions(ids[2], {move("21", "Done"), move("31", "Closed")});

        BULK_REQUIRE(OpenBulkPopup(ctx, "Status", 3, *rows.begin(), kStatusKey));
        BULK_REQUIRE(StepUntil(ctx, 300, [&] { return env.Fake->FetchIssueTransitionsCalls() >= 3; })); // one per row
        // Until every row's answer is live: the picker refreshes every few frames, and a catalog list would match too.
        BULK_REQUIRE(StepUntil(ctx, 300, [] {
            const BulkEditPopupRequest::StatusPickCache& c = g_ui.bulkEditPopupRequest.StatusCache;
            return c.Applicable && c.From == smatchet::statuscombo::StatusOptionsSource::Live;
        }));
        const std::uint64_t jobId = g_ui.nextBulkJobId;
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();

        // "Closed" is a move of two rows only: no match in the list, so Enter picks nothing and the picker stays open.
        ctx->KeyChars("Clos");
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_NO_RET(g_ui.bulkEditPopupRequest.Active);
        IM_CHECK_NO_RET(FindJob(jobId) == nullptr);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore);

        // "Done" is common: the same filter box now finds it, and the pick moves all three rows.
        ctx->KeyPress(ImGuiKey_Backspace, 4);
        PickByTyping(ctx, "Done");
        BULK_REQUIRE(FindJob(jobId) != nullptr);
        BULK_REQUIRE(WaitJobDone(ctx, jobId));
        const smatchet::grid::bulk::BulkSummary summary = smatchet::grid::bulk::Summarize(*FindJob(jobId));
        IM_CHECK_NO_RET(summary.Total == 3);
        IM_CHECK_NO_RET(summary.Saved == 3);
        IM_CHECK_NO_RET(summary.Failed == 0);
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore + 3);
        SelectRows({});
    };
}

// BulkFieldEdit/Status_NoCommonMoveOffersNoneOfThem: two rows script moves that share nothing (the rows' own status,
// Open, aside), so neither "In Progress" nor "Closed" is offered and an Enter on either picks nothing.
static void RegisterStatusNoCommonMove(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Status_NoCommonMoveOffersNoneOfThem");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        nlohmann::json fields = nlohmann::json::array();
        fields.push_back("status");
        fields.push_back("summary");
        BULK_REQUIRE(RunCommand(ctx, env.App, "view.update", {{"fields", fields}}).Ok);
        BULK_REQUIRE(StepUntil(ctx, 300, [] { return IndexOfKey(kStatusKey) >= 0 && IndexOfKey(kSummaryKey) >= 0; }));
        ctx->Yield(10);

        const std::set<int> rows = FirstRows(2);
        SelectRows(rows);
        std::vector<std::string> ids;
        for (int row : rows) {
            ids.push_back(IdAtRow(row));
        }
        BULK_REQUIRE(ids.size() == 2 && !ids[0].empty() && !ids[1].empty());
        const auto move = [](const char* id, const char* name) {
            TrackerFieldOption option;
            option.Id = id;
            option.Value = name;
            return option;
        };
        env.Fake->SetSupportsIssueTransitions(true);
        env.Fake->SetIssueTransitions(ids[0], {move("11", "In Progress")});
        env.Fake->SetIssueTransitions(ids[1], {move("31", "Closed")});

        BULK_REQUIRE(OpenBulkPopup(ctx, "Status", 2, *rows.begin(), kStatusKey));
        BULK_REQUIRE(StepUntil(ctx, 300, [&] { return env.Fake->FetchIssueTransitionsCalls() >= 2; }));
        // Until every row's answer is live: the picker refreshes every few frames, and a catalog list would match too.
        BULK_REQUIRE(StepUntil(ctx, 300, [] {
            const BulkEditPopupRequest::StatusPickCache& c = g_ui.bulkEditPopupRequest.StatusCache;
            return c.Applicable && c.From == smatchet::statuscombo::StatusOptionsSource::Live;
        }));
        const std::uint64_t jobId = g_ui.nextBulkJobId;
        const std::size_t callsBefore = env.Fake->UpdateIssueFieldsCallCount();
        for (const char* typed : {"Prog", "Clos"}) {
            ctx->KeyChars(typed);
            ctx->Yield(3);
            ctx->KeyPress(ImGuiKey_Enter);
            ctx->Yield(3);
            IM_CHECK_NO_RET(g_ui.bulkEditPopupRequest.Active);
            IM_CHECK_NO_RET(FindJob(jobId) == nullptr);
            ctx->KeyPress(ImGuiKey_Backspace, static_cast<int>(std::string(typed).size()));
        }
        IM_CHECK_NO_RET(env.Fake->UpdateIssueFieldsCallCount() == callsBefore);
        g_ui.bulkEditPopupRequest.Active = false;
        SelectRows({});
    };
}

// BulkFieldEdit/Status_OfflineOffersWhatIsRemembered: with the tracker unreachable no transitions are asked for, and
// the picker still lists options (the remembered workflow or the catalog, never an empty list) with a cue that says
// the list is not live, so the rows can still be queued.
static void RegisterStatusOfflineCatalog(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Status_OfflineOffersWhatIsRemembered");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        nlohmann::json fields = nlohmann::json::array();
        fields.push_back("status");
        fields.push_back("summary");
        BULK_REQUIRE(RunCommand(ctx, env.App, "view.update", {{"fields", fields}}).Ok);
        BULK_REQUIRE(StepUntil(ctx, 300, [] { return IndexOfKey(kStatusKey) >= 0 && IndexOfKey(kSummaryKey) >= 0; }));
        ctx->Yield(10);

        const std::set<int> rows = FirstRows(3);
        SelectRows(rows);
        env.Fake->SetSupportsIssueTransitions(true);
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *env.App, TrackerConnectivityState::TransportDown));
        const std::size_t fetchesBefore = env.Fake->FetchIssueTransitionsCalls();

        BULK_REQUIRE(OpenBulkPopup(ctx, "Status", 3, *rows.begin(), kStatusKey));
        ctx->Yield(15);                                                           // past the picker's refresh interval
        IM_CHECK_NO_RET(env.Fake->FetchIssueTransitionsCalls() == fetchesBefore); // offline: nothing is asked for
        const BulkEditPopupRequest::StatusPickCache& cache = g_ui.bulkEditPopupRequest.StatusCache;
        IM_CHECK_NO_RET(g_ui.bulkEditPopupRequest.StatusQueries.size() == 3);
        IM_CHECK_NO_RET(cache.Valid && cache.Applicable);
        IM_CHECK_NO_RET(!cache.Options.empty()); // the rows' current status is always offered
        IM_CHECK_NO_RET(cache.From != smatchet::statuscombo::StatusOptionsSource::Live);
        IM_CHECK_NO_RET(cache.Freshness != smatchet::offline::DataFreshness::Fresh);
        g_ui.bulkEditPopupRequest.Active = false;
        SelectRows({});
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *env.App, TrackerConnectivityState::AuthenticatedReachable));
    };
}

// BulkFieldEdit/Status_LargeSelectionFetchesOncePerGroup: above a handful of rows the picker asks the tracker about
// one row per (project, issue type, status) group, not one per row; the fixture's twelve rows hold two groups (eleven
// Tasks and one Bug, all Open).
static void RegisterStatusFetchesPerGroup(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "BulkFieldEdit", "Status_LargeSelectionFetchesOncePerGroup");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Env env;
        if (!Setup(ctx, &env)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        nlohmann::json fields = nlohmann::json::array();
        fields.push_back("status");
        fields.push_back("summary");
        BULK_REQUIRE(RunCommand(ctx, env.App, "view.update", {{"fields", fields}}).Ok);
        BULK_REQUIRE(StepUntil(ctx, 300, [] { return IndexOfKey(kStatusKey) >= 0 && IndexOfKey(kSummaryKey) >= 0; }));
        ctx->Yield(10);

        const std::set<int> rows = FirstRows(12);
        SelectRows(rows);
        BULK_REQUIRE(rows.size() == 12);
        TrackerFieldOption done;
        done.Id = "21";
        done.Value = "Done";
        env.Fake->SetSupportsIssueTransitions(true);
        for (int row : rows) {
            env.Fake->SetIssueTransitions(IdAtRow(row), {done});
        }
        const std::size_t fetchesBefore = env.Fake->FetchIssueTransitionsCalls();

        BULK_REQUIRE(OpenBulkPopup(ctx, "Status", 12, *rows.begin(), kStatusKey));
        BULK_REQUIRE(StepUntil(ctx, 300, [] {
            const BulkEditPopupRequest::StatusPickCache& c = g_ui.bulkEditPopupRequest.StatusCache;
            return c.Applicable && !c.Options.empty();
        }));
        ctx->Yield(40); // several refresh passes: nothing more may be asked for
        const std::size_t fetches = env.Fake->FetchIssueTransitionsCalls() - fetchesBefore;
        IM_CHECK_NO_RET(fetches >= 1);
        IM_CHECK_NO_RET(fetches <= 2); // the Task group and the Bug group, never one per row
        g_ui.bulkEditPopupRequest.Active = false;
        SelectRows({});
    };
}

extern "C" void SmatchetRegisterBulkFieldEditTests(ImGuiTestEngine* engine) {
    RegisterCommandPath(engine);
    RegisterGridSearchFocusKeepsModal(engine);
    RegisterMenuItemVisibility(engine);
    RegisterPickSavesEveryRow(engine);
    RegisterConfirmStep(engine);
    RegisterOfflineQueues(engine);
    RegisterLabelsModeToggle(engine);
    RegisterLabelsAddMode(engine);
    RegisterLabelsRemoveMode(engine);
    RegisterLabelsEnterKeepsModal(engine);
    RegisterLabelsEnterNothingTicked(engine);
    RegisterStatusCommonMoves(engine);
    RegisterStatusNoCommonMove(engine);
    RegisterStatusOfflineCatalog(engine);
    RegisterStatusFetchesPerGroup(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
