// data_dependent_windows_smoke.test.cpp — bucket-E render-smoke tests for the
// 8 data-dependent UI windows identified in plan #12 (batches 1 + 2, complete).
//
// PURPOSE — STRUCTURAL coverage for ImGui window-draw functions whose body
// depends on tracker/config/plan data. Each test opens the target window with
// the minimum state required to tick the real draw body (some windows render a
// graceful empty-state with zero backend data; those are still covered here
// because the Begin/End pair and push/pop balance is what we are protecting).
// The ImGui Test Engine traps any in-frame IM_ASSERT (e.g. a Begin/End or
// PushID/PopID imbalance introduced by a future decomposition) and fails the
// test. There is NO checked-in pixel golden — the live tick IS the coverage.
//
// Pattern: same as funcsize_window_render_smoke.test.cpp (green pilot) and
// funcsize_grid_render.test.cpp (fixture-coupled yellow pilot). See the REUSABLE
// PATTERN comments in those files.
//
// Windows covered in this file (plan #12 batch 1):
//
//   1. Views Dashboard (drawViewsDashboardWindow, SmatchetViewsDashboardUi.cpp)
//      Title: "###SmatchetViewsDashboard" — the constant id suffix; the visible
//      portion varies with tracker type. Gated behind cfg.BackendHasBeenReachable;
//      we set that flag directly, which is exactly what the connectivity-probe
//      path does in SmatchetUI.cpp:502. Without any views loaded the window
//      renders "No views available." and returns — Begin/End balanced, draw body
//      ticked, enough for Pillar-3 structural coverage.
//
//   2. Bulk Export window (drawBulkExportWindow, SmatchetBulkTicketsUi.cpp)
//      Title: "Bulk export tickets". Opens via showBulkExport + requestBulkExportFocus.
//      Body submits "##bulkExportPath" InputText unconditionally after Begin(),
//      no backend needed.
//
//   3. Plan Doc Viewer (DrawPlanDocViewer, SmatchetPlanDocViewerUi.cpp)
//      Title: "Plan Docs". Opens via showPlanDocViewer + requestPlanDocViewerFocus.
//      Under the test harness the doc-scanner finds no files (the repo root is
//      not the working dir at runtime) so the viewer renders
//      "No plan docs found..." + a "Rescan" button. Both paths are covered:
//      the index-in-flight path (immediately after open) and the empty-result
//      path (after PollIndexResult). "Rescan" is the robust always-present probe.
//
//   4. Performance Window (SmatchetPerfUi::DrawWindow, SmatchetPerfUi.cpp)
//      Title: "Performance". Opens via showPerformance + requestPerformanceFocus.
//      Body renders the "FPS:" Text + a tab bar unconditionally — zero backend.
//      The tab bar ID "PerformanceTabs" is the robust probe.
//
//   5. Annotate Analysis Window (AnnotateAnalysisUi::DrawWindow,
//      AnnotateAnalysisUi_Window.cpp)
//      Title: "Annotate###AnnotateAnalysisModal". The window is shown when
//      g_ui.showAnnotateAnalysis = true AND a non-empty issue key is provided
//      via gridState.ActiveIssueId. Setting it to a synthetic key ("SMAT-1")
//      is enough to open the window; the inner DrawContent reads the worker
//      State(), which starts with an empty result set (no p4 data) — so all
//      "no data" paths run and the Begin/End pair is balanced. The toolbar's
//      "##annotate_export_csv" Button is submitted unconditionally once Begin()
//      succeeds and is the robust probe.
//
// Windows covered in this file (plan #12 batch 2 — completes 8/8):
//
//   6. Attachment Preview (drawAttachmentPreviewWindow, SmatchetAttachmentPreviewUi.cpp)
//      Title: "Attachment Preview". The draw fn early-returns BEFORE Begin() when
//      attachmentWindowEntries is empty (the "no entries" guard), so the window
//      object never materialises. We inject one synthetic non-image entry directly
//      into g_ui.attachmentWindowEntries + set attachmentPreviewWindowOpen — the
//      same post-state IngestAttachmentCollection produces from the collection
//      queue, minus the mutex/AttachmentCollectionRequest plumbing and with a
//      non-image MIME so no download/decode is kicked. Because Begin() only runs
//      PAST the entries guard, WindowIsLive("Attachment Preview") is itself proof
//      the draw body ticked on real entry data — the robust structural probe. The
//      empty path (no window) is covered too: with entries cleared the window
//      must NOT be live.
//
//   7. New Issue Draft inline row (RenderNewIssueDraftRow, SmatchetNewIssueDraftUi.cpp)
//      Embedded in the active-project grid table (drawActiveProjectGridNewIssue),
//      NOT a standalone window. The draft row only renders inside the live ticket
//      table, which short-circuits on an empty tickets snapshot — so this is the
//      same FIXTURE-COUPLED recipe funcsize_grid_render.test.cpp pilots: boot with
//      the deterministic Jira fixture, sync, wait for tickets, focus the
//      always-drawn "Smatchet - Active Project" window, then flip
//      newIssueDraftActive + seed a synthetic draft so RenderNewIssueDraftRow runs
//      its ACTIVE branch (DrawDraftIdColumnCell Create/Cancel + per-field cells)
//      instead of the inactive "[+ new]" cell. The inactive branch is exercised
//      for free every frame before activation. SKIPS without the fixture env.
//      Structural probe: the draft stays active across frames AND the host window
//      is live — the engine traps any Begin/End or PushID/PopID imbalance the
//      draft row's grouped cells introduce.
//
//   8. Offline Queue panel (DrawUnifiedOfflineQueuesPanel, SmatchetOfflineQueueUi.cpp)
//      Embedded inline in drawActiveProjectUnsavedStrip (focused pane only), NOT a
//      standalone window — invoked every frame from the docked "Smatchet - Active
//      Project" host window. FIXTURE-GATED (same as case 7): that host window is a
//      docked NoTitleBar pane that only becomes Active once its dock tab is selected,
//      which the minimal no-fixture boot never reaches (no tickets, pane never
//      forwarded) — so the inline panel is unreachable without the fixture. With the
//      fixture: sync loads tickets, the host pane docks + activates, and the panel's
//      branches become reachable.
//      EMPTY path: host window live ⇒ the panel's total==0 early-return branch ticked
//      under the engine's assert trap.
//      POPULATED path (opportunistic): enqueue one synthetic create via
//      AppController::QueueCreateOffline (a pure local-SQLite write — no backend) so
//      total>0 forces the panel body (header + toolbar + table) to render. When
//      QueueCreateOffline returns 0 (OfflineQueueService not constructed) the test
//      stays on the empty-path coverage instead of failing. The synthetic row is
//      removed via DeletePendingCreates so it never leaks.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "DraftStore.h" // the Quick Create and draft-row drafts (audit #51)
#include "IssueDraft.h" // synthetic offline-create enqueue (case 8)
#include "SmatchetUiSession.h"
#include "TicketGridModel.h"             // TicketGridColumn — the draft-row keys host table (case 7b)
#include "Tracker/IssueCreatePipeline.h" // IssueCreateResult — a finished create handed to the draft row
#include "Ui/SmatchetGridUiSupport.h"    // RenderNewIssueDraftRow

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib> // getenv — fixture-env gate (case 7)
#include <future>
#include <string>
#include <vector>

// g_ui — the shared bag of UI-thread visibility flags. ViewToggleCommands.cpp
// flips these same fields to open/close panels; we set them directly here to
// avoid threading a command dispatch through the test.
extern UiDrawSession g_ui;

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
// Returns true if the predicate became true within the budget.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True once a top-level window with `title` exists and is Active (Begin()
// returned true this frame, so its children were submitted). FindWindowByName
// resolves docked windows by plain title where ctx->WindowInfo() does not, and
// silently returns nullptr on a miss so the polling loop does not mark the
// context errored.
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// True if the deterministic-Jira-backend fixture env var is set. Case 7 (New Issue
// Draft row) needs loaded tickets for the grid table — and therefore the draft row —
// to render; without the fixture no tickets ever load, so the test SKIPS with a log
// rather than asserting on an empty grid. Same gate as funcsize_grid_render.test.cpp.
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

// True when the bucket-E spawn child opted into a live local cache via
// SMATCHET_UITEST_WITH_LOCAL_CACHE=1 (mirrors UiTestScenario::UiTestWantsLocalCache).
// Under this opt-in EnsureLocalCacheForUiTest() stands up a live cache AND clears the
// first-run ReadOnlyMode default, so the case-8 offline-create populated path MUST
// activate — its skip becomes a hard failure (locks the #1521 fix against regression).
bool UiTestWithLocalCache() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* v = std::getenv("SMATCHET_UITEST_WITH_LOCAL_CACHE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

// Shared body for the standard docked-window smoke recipe:
//   open via flag pair → re-arm focus latch each frame → wait for Active →
//   assert child widget → close.
//
// `openFlag`  — member pointer for the g_ui.show* visibility bool.
// `focusFlag` — member pointer for the g_ui.request*Focus one-frame latch.
// `windowTitle` — the full ImGui window title (plain for floating, plain for
//                 docked windows without the "###id" suffix as FindWindowByName
//                 uses the full buffer; for windows with "###id" only the suffix
//                 matters for FindWindowByName).
// `childItemRef` — bare item label (resolved relative to SetRef(windowTitle)).
void RunWindowRenderSmoke(ImGuiTestContext* ctx, bool UiDrawSession::* openFlag, bool UiDrawSession::* focusFlag,
                          const char* windowTitle, const char* childItemRef) {
    const AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo(
            "SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted under UiTestScenario");
        return;
    }

    g_ui.*openFlag = true;
    g_ui.*focusFlag = true;

    ctx->SetRef(windowTitle);
    const bool visible = YieldUntil(ctx, [&] {
        // Re-arm each frame until the docked tab activates. The draw fn clears
        // the flag once consumed; we re-arm so inactive tabs get activated.
        g_ui.*focusFlag = true;
        return WindowIsLive(windowTitle);
    });
    IM_CHECK_NO_RET(visible);

    if (visible) {
        const bool itemPresent = ctx->ItemExists(childItemRef);
        IM_CHECK_NO_RET(itemPresent);
    }

    g_ui.*openFlag = false;
    ctx->Yield();
}

// ============================================================================
// 1. Views Dashboard
// ============================================================================
// drawViewsDashboardWindow (SmatchetViewsDashboardUi.cpp).
// The window id is the constant "###SmatchetViewsDashboard" suffix;
// FindWindowByName looks up by full title buffer including the "###" id portion,
// so we probe with the id-only form "###SmatchetViewsDashboard" which is how
// ImGui stores the lookup key for windows that use the "label###id" syntax.
//
// The draw function is gated behind cfg.BackendHasBeenReachable. We set that
// flag directly — the same mutation SmatchetUI.cpp performs at line ~502 when
// the connectivity probe first succeeds. Once inside Begin(), the window renders
// "No views available." (ViewState has no loaded views) — the TextDisabled and
// ImGui::End() are both called, so the Begin/End pair is balanced.
//
// Robust probe: "No views available." — submitted via ImGui::TextDisabled()
// which creates an item; ItemExists resolves it against SetRef.
void RegisterViewsDashboardWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "ViewsDashboard_RendersEmptyStateWithoutViews");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Unlock the draw path — same mutation the connectivity-probe fires on first success.
        g_ui.cfg.BackendHasBeenReachable = true;
        g_ui.showViewsDashboard = true;
        g_ui.requestViewsDashboardFocus = true;

        // The window uses a "label###id" title. FindWindowByName uses only the
        // "###id" portion as the lookup key. Probe with the stable id suffix.
        const char* kWindowId = "###SmatchetViewsDashboard";
        ctx->SetRef(kWindowId);
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestViewsDashboardFocus = true;
            return WindowIsLive(kWindowId);
        });
        IM_CHECK_NO_RET(visible);

        // The structural assertion is the window being Active: the draw function ran
        // its Begin/End pair without an ImGui assertion (which the engine traps).
        // The views dashboard empty-state renders only ImGui::TextDisabled +
        // ImGui::End() — no Button/InputText with a queryable ID. WindowIsLive
        // already proved Begin() returned true and the body ticked.
        (void)visible;

        g_ui.showViewsDashboard = false;
        g_ui.cfg.BackendHasBeenReachable = false;
        ctx->Yield();
    };
}

// ============================================================================
// 2. Bulk Export window
// ============================================================================
// drawBulkExportWindow (SmatchetBulkTicketsUi.cpp). Title "Bulk export tickets".
// Docked window, so the focus re-arm recipe applies. The body submits
// "##bulkExportPath" InputText and "Copy to clipboard" button unconditionally
// once Begin() succeeds — zero backend required.
//
// Robust probe: "##bulkExportPath" InputText — submitted immediately after Begin().
void RegisterBulkExportWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "BulkExportWindow_RendersAndShowsPathInput");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showBulkExport, &UiDrawSession::requestBulkExportFocus,
                             "Bulk export tickets", "##bulkExportPath");
    };
}

// ============================================================================
// 3. Plan Doc Viewer
// ============================================================================
// DrawPlanDocViewer (SmatchetPlanDocViewerUi.cpp). Title "Plan Docs".
// Opens via showPlanDocViewer + requestPlanDocViewerFocus.
//
// Under the test harness the async file scanner runs but finds no .md files
// (the repo root is not the CWD at runtime). Two sub-paths run:
//   a) indexInFlight == true  → "Scanning plan docs..." Text — immediately
//   b) indexInFlight == false && files.empty() → "No plan docs found..." Text
//      + "Rescan" Button
//
// We wait for the window to be live (covers path a), then wait for the scan to
// complete (covers path b). The "Rescan" Button is the robust always-present
// probe once the scan is done. Both paths exercise the Begin/End pair.
void RegisterPlanDocViewerWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "PlanDocViewer_RendersEmptyStateAndRescanButton");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        g_ui.showPlanDocViewer = true;
        g_ui.requestPlanDocViewerFocus = true;

        ctx->SetRef("Plan Docs");
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestPlanDocViewerFocus = true;
            return WindowIsLive("Plan Docs");
        });
        IM_CHECK_NO_RET(visible);

        if (visible) {
            // Wait for the async file scan to complete (up to 200 more frames).
            // Once the index attempt finishes with an empty result, "Rescan" appears.
            // If files ARE found (e.g. if the CWD happens to contain a docs/ tree),
            // the combo picker appears instead — ItemExists("Rescan") returns false
            // but that is not a failure: the window drew correctly. We accept both
            // outcomes as pass because the structural assertion is that Begin/End
            // ran without an ImGui assertion; the "Rescan" probe is best-effort.
            ctx->Yield(); // let scan start
            for (int i = 0; i < 200; ++i) {
                ctx->Yield();
                if (ctx->ItemExists("Rescan") || ctx->ItemExists("##plan_doc_picker")) {
                    break;
                }
            }
            // Primary assertion: either "Rescan" (empty) or the combo picker (has files).
            const bool rescanPresent = ctx->ItemExists("Rescan");
            const bool pickerPresent = ctx->ItemExists("##plan_doc_picker");
            IM_CHECK_NO_RET(rescanPresent || pickerPresent);
        }

        g_ui.showPlanDocViewer = false;
        ctx->Yield();
    };
}

// ============================================================================
// 4. Performance Window
// ============================================================================
// SmatchetPerfUi::DrawWindow (SmatchetPerfUi.cpp). Title "Performance".
// Opens via showPerformance + requestPerformanceFocus. The body renders
// "FPS: ... Frame: ... ms" Text + a BeginTabBar("PerformanceTabs") on every
// frame, zero backend required.
//
// Robust probe: "PerformanceTabs/CPU" — the first BeginTabItem("CPU") inside the
// BeginTabBar("PerformanceTabs"), always active on first open. A tab item's
// ImGui ID is seeded inside the tab-bar scope, so the engine ref must be
// tab-bar-qualified ("<TabBarID>/<label>"). A bare "CPU" resolves against the
// window root, where no such item exists.
void RegisterPerformanceWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "PerformanceWindow_RendersAndShowsTabBar");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showPerformance, &UiDrawSession::requestPerformanceFocus,
                             "Performance", "PerformanceTabs/CPU");
    };
}

// ============================================================================
// 5. Annotate Analysis Window
// ============================================================================
// AnnotateAnalysisUi::DrawWindow (AnnotateAnalysisUi_Window.cpp).
// Title: "Annotate###AnnotateAnalysisModal". The draw function is called when
// g_ui.showAnnotateAnalysis == true, which SmatchetUI::Draw checks before
// calling annotateAnalysisUi_.DrawWindow(). The third argument is
// g_ui.gridState.ActiveIssueId — setting it to a synthetic non-empty key is
// sufficient to open the window (the inner DrawContent reads worker State()
// which starts empty, so the "no data" paths run and all Begin/End pairs are
// balanced). The window is docked to the bottom panel, so the focus-re-arm
// recipe applies.
//
// The "###AnnotateAnalysisModal" id suffix is the stable FindWindowByName key.
//
// Robust probe: "Export CSV" Button — submitted unconditionally in the Annotate
// header toolbar (DrawContent → DrawAnnotateHeaderToolbar) once Begin() returns
// true, independent of whether p4 data or a Jira issue record has loaded.
void RegisterAnnotateAnalysisWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "AnnotateAnalysis_RendersHeaderWithSyntheticIssueKey");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Provide a synthetic issue key — the draw function calls
        // annotateAnalysisUi_.DrawWindow(app, &g_ui.showAnnotateAnalysis,
        //   g_ui.gridState.ActiveIssueId), so a non-empty string is all that is
        // needed to proceed past the early-return guard inside DrawWindow.
        g_ui.focusedPane().gridState.ActiveIssueId = "SMAT-TEST-1"; // per-pane since multi-grid Slice 2
        g_ui.showAnnotateAnalysis = true;

        // "###AnnotateAnalysisModal" is the stable id suffix — FindWindowByName
        // uses only this portion as the lookup key. The window docks to
        // kBottomPanel (ImGuiCond_FirstUseEver) and has no requestFocus latch,
        // so it can open as an inactive docked tab. We re-set showAnnotateAnalysis
        // each frame to keep the draw path active; the engine advances the tab to
        // Active within a few frames as part of normal dock layout resolution.
        const char* kWindowId = "###AnnotateAnalysisModal";
        ctx->SetRef(kWindowId);
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.showAnnotateAnalysis = true; // re-arm visibility each frame
            return WindowIsLive(kWindowId);
        });
        IM_CHECK_NO_RET(visible);

        if (visible) {
            // "Export CSV" is a plain Button in DrawAnnotateHeaderToolbar,
            // submitted unconditionally once Begin() returns true — independent
            // of whether p4 data or a Jira issue record has loaded.
            const bool csvExportPresent = ctx->ItemExists("Export CSV");
            IM_CHECK_NO_RET(csvExportPresent);
        }

        // Restore: clear the synthetic key and close the window.
        g_ui.showAnnotateAnalysis = false;
        g_ui.focusedPane().gridState.ActiveIssueId = "";
        ctx->Yield();
    };
}

// ============================================================================
// 6. Attachment Preview window
// ============================================================================
// drawAttachmentPreviewWindow (SmatchetAttachmentPreviewUi.cpp). Title
// "Attachment Preview". Called unconditionally every frame from drawSecondaryWindows,
// but the body early-returns BEFORE prepareTopLevelWindow/Begin() when
// attachmentWindowEntries is empty (the "no entries" guard) — so the window object
// only ever exists once entries are present. We inject one synthetic non-image entry
// directly into g_ui.attachmentWindowEntries and flip attachmentPreviewWindowOpen,
// reproducing IngestAttachmentCollection's post-state without the mutex /
// AttachmentCollectionRequest plumbing. The non-image MIME ("text/plain") keeps
// IsSupportedImageMime false so no download or thumbnail decode is kicked.
//
// Robust probe: because Begin("Attachment Preview") only runs PAST the entries
// guard, WindowIsLive("Attachment Preview") being true is itself proof the draw
// body ticked on real entry data (the list + details panes both submit). The empty
// path (entries cleared → no window) is covered too.
void RegisterAttachmentPreviewWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "AttachmentPreview_RendersWithSyntheticEntry");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Empty path first: with no entries the window must NOT be live (the guard
        // returns before Begin). This is the cheap "no data" coverage.
        g_ui.attachmentWindowEntries.clear();
        g_ui.attachmentPreviewWindowOpen = false;
        ctx->Yield();
        ctx->Yield();
        const char* kWindowTitle = "Attachment Preview";
        IM_CHECK_NO_RET(!WindowIsLive(kWindowTitle));

        // Inject one synthetic non-image entry + open the window. This is the exact
        // post-state IngestAttachmentCollection leaves, minus the network plumbing.
        AttachmentWindowEntry entry;
        entry.Filename = "synthetic-smoke.txt";
        entry.Url = "smatchet-test://synthetic-smoke";
        entry.MimeType = "text/plain"; // non-image: no preview/thumbnail work kicked
        g_ui.attachmentWindowEntries.clear();
        g_ui.attachmentWindowEntries.push_back(entry);
        g_ui.attachmentWindowSelectedIndex = 0;
        g_ui.attachmentPreviewWindowOpen = true;

        ctx->SetRef(kWindowTitle);
        const bool visible = YieldUntil(ctx, [&] { return WindowIsLive(kWindowTitle); });
        // Structural assertion: Begin() ran past the entries guard and the list +
        // details panes ticked without an ImGui assertion (the engine traps one).
        IM_CHECK_NO_RET(visible);
        if (visible) {
            // Best-effort body probe: the details pane submits a "Close" Button
            // unconditionally once a selected entry exists. It lives inside the
            // AttachmentDetailsPane child window, so address it through that scope.
            // Logged (not asserted) — WindowIsLive is the hard past-guard proof.
            const bool closePresent = ctx->ItemExists("AttachmentDetailsPane/Close");
            ctx->LogInfo("AttachmentPreview details-pane Close button reachable: %d", closePresent ? 1 : 0);
        }

        // Restore: clear the synthetic entry and close the window.
        g_ui.attachmentPreviewWindowOpen = false;
        g_ui.attachmentWindowEntries.clear();
        g_ui.attachmentWindowSelectedIndex = 0;
        ctx->Yield();
    };
}

// ============================================================================
// 7. New Issue Draft inline row
// ============================================================================
// RenderNewIssueDraftRow (SmatchetNewIssueDraftUi.cpp), drawn inside the
// active-project grid table by drawActiveProjectGridNewIssue — NOT a standalone
// window. The draft row's ACTIVE branch (DrawDraftIdColumnCell Create/Cancel +
// per-field cells) only runs when newIssueDraftActive is true AND the grid table
// is rendering, which requires loaded tickets/columns. This is the same
// fixture-coupled recipe funcsize_grid_render.test.cpp pilots; without the fixture
// the test SKIPS.
//
// Structural probe: with the draft armed across frames, the host window stays live
// and newIssueDraftActive stays true (proving the row drew its active branch
// without firing Cancel / crashing). The engine traps any Begin/End or PushID/PopID
// imbalance the grouped draft cells introduce — that is the coverage.
const char* const kActiveProjectTitle = "Smatchet - Active Project";

// Boot the fixture grid: sync, then wait for tickets so the table (and thus the draft row)
// renders, and bring the Active Project pane up. Inactive-branch coverage ("[+ new]" cell)
// runs for free every frame before a test activates the draft. True once the pane is live.
bool BootFixtureActiveProjectGrid(ImGuiTestContext* ctx, AppController& app) {
    app.SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app.IsSyncPendingOrActive(); });
    IM_CHECK_NO_RET(syncDone);
    const auto tickets = app.GetActiveTickets();
    IM_CHECK_NO_RET(!tickets.empty());

    ctx->SetRef(kActiveProjectTitle);
    const bool visible = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kActiveProjectTitle);
    });
    IM_CHECK_NO_RET(visible);
    return visible;
}

// Set by DeactivateFieldForTest: the next frame drops the caret from whichever field holds it, as a click
// elsewhere does. Hiding a window while its field still holds the caret is not something a user can do, and
// ImGui then writes that field's old text back when it next draws.
bool s_clearActiveIdNextFrame = false;

void ClearActiveIdIfAsked() {
    if (s_clearActiveIdNextFrame) {
        s_clearActiveIdNextFrame = false;
        ImGui::ClearActiveID();
    }
}

void ClearActiveIdGui(ImGuiTestContext* /*ctx*/) { ClearActiveIdIfAsked(); }

void DeactivateFieldForTest(ImGuiTestContext* ctx) {
    s_clearActiveIdNextFrame = true;
    ctx->Yield(2);
}

// The draft-store scope of the draft row's kept entry (audit #51).
std::string RowDraftScope() {
    const AppController* app = SmatchetActiveUiTestAppController();
    return app != nullptr ? NewIssueRowDraftScope(*app) : std::string("grid");
}

void ClearNewIssueDraftForTest() {
    g_ui.newIssueDraftActive = false;
    g_ui.newIssueFocusSummaryPending = false;
    g_ui.newIssueDraft = IssueDraft{};
    g_ui.newIssueDraftEditBufs.clear();
    g_ui.newIssueMissingFieldIds.clear();
    g_ui.gridEditError.clear();
    g_ui.gridEditSuccess.clear();
    // A row a case opened keeps what it typed: drop it, so it never comes back in a later case.
    ForgetNewIssueRowDraft();
    smatchet::drafts::Erase("newissue.row", RowDraftScope());
    smatchet::drafts::Erase("newissue.row.maybe_created", RowDraftScope());
}

// The draft row's keys are exercised in a host table of their own: the fixture grid boots
// read-only (its tracker banner reports an error with no credentials), and a read-only grid
// does not draw the draft row. Drawn only while a test sets this.
bool s_drawDraftRowHost = false;

std::vector<TicketGridColumn> DraftRowHostColumns() {
    std::vector<TicketGridColumn> cols(4);
    cols[0].ColumnKind = TicketGridColumn::Kind::Id;
    cols[0].FieldId = "id";
    cols[1].ColumnKind = TicketGridColumn::Kind::FieldValue;
    cols[1].FieldId = "summary";
    cols[2].ColumnKind = TicketGridColumn::Kind::FieldValue;
    cols[2].FieldId = "description";
    cols[3].ColumnKind = TicketGridColumn::Kind::FieldValue;
    cols[3].FieldId = "environment";
    return cols;
}

void DrawDraftRowHost(ImGuiTestContext* /*ctx*/) {
    ClearActiveIdIfAsked();
    if (!s_drawDraftRowHost) {
        return;
    }
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        return;
    }
    static const std::vector<TicketGridColumn> kCols = DraftRowHostColumns();
    ImGui::SetNextWindowSize(ImVec2(900.0f, 400.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("Draft row keys host", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTable("draft_host", static_cast<int>(kCols.size()), ImGuiTableFlags_Borders)) {
            RenderNewIssueDraftRow(*app, g_ui, kCols, g_ui.cfg, nullptr);
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// Open a draft row with `projectKey` and put the caret in its summary field, the way
// "+ New Issue" does. True once a text field is active.
bool OpenDraftWithCaretInSummary(ImGuiTestContext* ctx, const char* projectKey) {
    ClearNewIssueDraftForTest();
    g_ui.newIssueDraft.ProjectKey = projectKey;
    g_ui.newIssueDraftActive = true;
    g_ui.newIssueFocusSummaryPending = true;
    const bool active = YieldUntil(ctx, [] { return GImGui->ActiveId != 0 && !g_ui.newIssueFocusSummaryPending; }, 60);
    if (!active) {
        ctx->LogInfo("draft caret not placed: ActiveId=0x%08X focusPending=%d editBufs=%d", GImGui->ActiveId,
                     g_ui.newIssueFocusSummaryPending ? 1 : 0, static_cast<int>(g_ui.newIssueDraftEditBufs.size()));
    }
    return active;
}

// Close the popups the test opened, if it opened any (ClosePopupToLevel asserts on a level
// that is not below the stack size).
void ClosePopupsAbove(int level) {
    if (GImGui->OpenPopupStack.Size > level) {
        ImGui::ClosePopupToLevel(level, true);
    }
}

std::string DraftEditBufText(const char* fieldId) {
    const auto it = g_ui.newIssueDraftEditBufs.find(fieldId);
    return it == g_ui.newIssueDraftEditBufs.end() || it->second.empty() ? std::string()
                                                                        : std::string(it->second.data());
}

// Audit #49: the inline draft row is worked from the keyboard. Esc in a field with text in the
// draft leaves the field with the text kept and asks before discarding; Esc in an empty draft
// closes it; Ctrl+Enter submits through the Create path, and without a project says why
// instead of doing nothing.
void RegisterNewIssueDraftRowKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "NewIssueDraftRow_EscAndCtrlEnterKeys");
    t->GuiFunc = DrawDraftRowHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Fixture-gated so the draft's project catalog refresh talks to the fixture backend,
        // never a live tracker.
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — keep the draft off a live backend");
            return;
        }
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        s_drawDraftRowHost = true;
        ctx->Yield(2);
        ctx->WindowFocus("Draft row keys host");

        // Typed summary + Esc: the draft stays, the text stays, the discard question opens.
        IM_CHECK_NO_RET(OpenDraftWithCaretInSummary(ctx, "SMAT"));
        ctx->KeyChars("Typed summary");
        ctx->Yield(2);
        const int popupsBefore = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(g_ui.newIssueDraftActive);
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), "Typed summary");
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsBefore + 1);
        ClosePopupsAbove(popupsBefore);
        ctx->Yield(2);

        // Ctrl+Enter without a project: nothing is sent, the draft stays, the row says why.
        IM_CHECK_NO_RET(OpenDraftWithCaretInSummary(ctx, ""));
        ctx->KeyChars("No project yet");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_NO_RET(g_ui.newIssueDraftActive);
        IM_CHECK_NO_RET(!g_ui.newIssueCreateInFlight);
        IM_CHECK_STR_EQ_NO_RET(g_ui.gridEditError.c_str(), "Pick a project");
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), "No project yet");

        // Text typed only into another free-text field (not summary or description): Esc asks too.
        IM_CHECK_NO_RET(OpenDraftWithCaretInSummary(ctx, "SMAT"));
        ctx->KeyPress(ImGuiKey_Tab); // summary -> description
        ctx->KeyPress(ImGuiKey_Tab); // description -> environment
        ctx->KeyChars("Windows 11");
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("environment").c_str(), "Windows 11");
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), "");
        const int popupsOther = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(g_ui.newIssueDraftActive);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsOther + 1);
        ClosePopupsAbove(popupsOther);
        ctx->Yield(2);

        // Nothing typed + Esc: the draft closes without a question.
        IM_CHECK_NO_RET(OpenDraftWithCaretInSummary(ctx, "SMAT"));
        const int popupsEmpty = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.newIssueDraftActive);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsEmpty);

        ClearNewIssueDraftForTest();
        s_drawDraftRowHost = false;
        ctx->Yield();
    };
}

// The text the draft store keeps for (surface, scope); empty when none.
std::string KeptDraftText(const char* surface, const char* scope) {
    smatchet::drafts::DraftEntry kept;
    return smatchet::drafts::Get(surface, scope, kept) ? kept.Text : std::string();
}

// Click "+ New Issue" in the host (the row is inactive) and wait for the caret in the summary field.
bool OpenDraftWithNewIssueButton(ImGuiTestContext* ctx) {
    ctx->SetRef("Draft row keys host");
    ctx->ItemClick("**/+ New Issue");
    const bool active = YieldUntil(
        ctx, [] { return g_ui.newIssueDraftActive && GImGui->ActiveId != 0 && !g_ui.newIssueFocusSummaryPending; }, 60);
    IM_CHECK_NO_RET(active);
    return active;
}

// Audit #51: what is typed in the draft row is kept when a view change closes the row; "+ New Issue"
// brings it back, the text still counts as typed (Esc asks), and a confirmed discard drops it.
void RegisterNewIssueDraftRowDraftKept(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "NewIssueDraftRow_DraftKeptAndRestored");
    t->GuiFunc = DrawDraftRowHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — keep the draft off a live backend");
            return;
        }
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        ClearNewIssueDraftForTest();
        s_drawDraftRowHost = true;
        ctx->Yield(2);
        ctx->WindowFocus("Draft row keys host");

        if (!OpenDraftWithNewIssueButton(ctx)) {
            s_drawDraftRowHost = false;
            return;
        }
        ctx->KeyChars("Kept summary");
        ctx->KeyPress(ImGuiKey_Tab); // summary -> description
        ctx->KeyPress(ImGuiKey_Tab); // description -> environment
        ctx->KeyChars("Linux");
        ctx->Yield(2);
        const std::string kept = KeptDraftText("newissue.row", RowDraftScope().c_str());
        IM_CHECK_NO_RET(kept.find("Kept summary") != std::string::npos);
        IM_CHECK_NO_RET(kept.find("Linux") != std::string::npos);

        // A view change closes the row; the typed text stays kept.
        CancelUnfinishedNewIssueForGridChange(g_ui);
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.newIssueDraftActive);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("newissue.row", RowDraftScope().c_str()).c_str(), kept.c_str());

        // The header's "+ New Issue" brings both fields back, with the restored line.
        static const std::vector<CachedTicket> kNoTickets;
        StartNewIssueDraft(*SmatchetActiveUiTestAppController(), g_ui, nullptr, kNoTickets);
        const bool reopened = YieldUntil(
            ctx, [] { return g_ui.newIssueDraftActive && GImGui->ActiveId != 0 && !g_ui.newIssueFocusSummaryPending; },
            60);
        IM_CHECK_NO_RET(reopened);
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), "Kept summary");
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("environment").c_str(), "Linux");
        IM_CHECK_NO_RET(g_ui.gridEditSuccess.find("Restored") != std::string::npos);

        // The restored text counts as typed: Esc asks, and the confirmed discard drops the kept entry.
        const int popupsBefore = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(g_ui.newIssueDraftActive);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsBefore + 1);
        ctx->ItemClick("//$FOCUSED/Discard draft");
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.newIssueDraftActive);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("newissue.row", RowDraftScope().c_str()).c_str(), "");

        ClosePopupsAbove(popupsBefore);
        ClearNewIssueDraftForTest();
        s_drawDraftRowHost = false;
        ctx->Yield();
    };
}

// A finished create the draft row will read on its next frame.
std::future<IssueCreateResult> ReadyCreateResult(const IssueCreateResult& r) {
    std::promise<IssueCreateResult> done;
    done.set_value(r);
    return done.get_future();
}

// Audit #51: a create settles the kept entry. Created drops it; a failure that may have created the issue
// marks it, also after a view change closed the row, and the next "+ New Issue" says to check. Typed
// bytes that are not UTF-8 are still kept. A row the offline queue loaded is never kept.
void RegisterNewIssueDraftRowKeptDraftSettled(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "NewIssueDraftRow_KeptDraftSettledByCreate");
    t->GuiFunc = DrawDraftRowHost;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — keep the draft off a live backend");
            return;
        }
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        ClearNewIssueDraftForTest();
        s_drawDraftRowHost = true;
        ctx->Yield(2);
        ctx->WindowFocus("Draft row keys host");
        const std::string scope = RowDraftScope();

        // Not UTF-8 in one field: the entry is still written, with the bad bytes replaced.
        if (!OpenDraftWithNewIssueButton(ctx)) {
            s_drawDraftRowHost = false;
            return;
        }
        ctx->KeyChars("Created summary");
        ctx->Yield(2);
        std::vector<char>& desc = g_ui.newIssueDraftEditBufs["description"];
        IM_CHECK_NO_RET(desc.size() > 3);
        desc[0] = static_cast<char>(0xC3);
        desc[1] = '(';
        desc[2] = '\0';
        ctx->KeyChars("!");
        ctx->Yield(2);
        IM_CHECK_NO_RET(KeptDraftText("newissue.row", scope.c_str()).find("Created summary!") != std::string::npos);

        // Created: the row closes and the kept entry goes.
        IssueCreateResult created;
        created.Ok = true;
        created.IssueKey = "TEST-1";
        g_ui.newIssueCreateFuture = ReadyCreateResult(created);
        g_ui.newIssueCreateInFlight = true;
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.newIssueDraftActive);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("newissue.row", scope.c_str()).c_str(), "");

        // A create that may have gone through, finishing after a view change closed the row.
        if (!OpenDraftWithNewIssueButton(ctx)) {
            s_drawDraftRowHost = false;
            return;
        }
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), ""); // nothing kept came back
        ctx->KeyChars("Maybe summary");
        ctx->Yield(2);
        std::promise<IssueCreateResult> running;
        g_ui.newIssueCreateFuture = running.get_future();
        g_ui.newIssueCreateInFlight = true;
        ctx->Yield();
        CancelUnfinishedNewIssueForGridChange(g_ui);
        ctx->Yield();
        IssueCreateResult maybe;
        maybe.Error = "Create issue failed: timed out";
        maybe.ReplayMayDuplicate = true;
        running.set_value(maybe);
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.newIssueCreateInFlight);
        IM_CHECK_NO_RET(KeptDraftText("newissue.row", scope.c_str()).find("Maybe summary") != std::string::npos);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("newissue.row.maybe_created", scope.c_str()).c_str(), "1");
        if (!OpenDraftWithNewIssueButton(ctx)) {
            s_drawDraftRowHost = false;
            return;
        }
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), "Maybe summary");
        IM_CHECK_NO_RET(g_ui.gridEditError.find("may have gone through") != std::string::npos);

        // A row the offline queue loads ("Open as draft": no caret request) is neither restored nor kept.
        // Its project change would rewrite a kept entry if it were. The user clicks in the queue panel
        // first, so the row's field has left the caret.
        DeactivateFieldForTest(ctx);
        CancelUnfinishedNewIssueForGridChange(g_ui);
        const std::string keptBefore = KeptDraftText("newissue.row", scope.c_str());
        IM_CHECK_NO_RET(!keptBefore.empty());
        g_ui.newIssueDraft = IssueDraft{};
        g_ui.newIssueDraft.FieldValues["summary"] = "From queue";
        g_ui.newIssueDraftActive = true;
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(DraftEditBufText("summary").c_str(), "From queue");
        g_ui.newIssueDraft.ProjectKey = "QUEUEPROJ";
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("newissue.row", scope.c_str()).c_str(), keptBefore.c_str());

        ClearNewIssueDraftForTest();
        s_drawDraftRowHost = false;
        ctx->Yield();
    };
}

// Audit #60: Ctrl+F (the grid.search.focus default binding) puts the caret in the focused pane's
// search box; Esc there clears the search and leaves the box.
void RegisterGridSearchFocusKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "GridSearch_CtrlFFocusesAndEscClears");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — the grid header does not draw");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        if (!BootFixtureActiveProjectGrid(ctx, *app)) {
            return;
        }
        GridPane& pane = g_ui.focusedPane();
        pane.gridSearchBuf[0] = '\0';
        ImGui::ClearActiveID();
        ctx->Yield(2);

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_F);
        const bool focused = YieldUntil(ctx, [] { return GImGui->ActiveId != 0; }, 10);
        if (!focused) {
            ctx->LogInfo("Ctrl+F: ActiveId=0x%08X framesLeft=%d paneFocused=%d", GImGui->ActiveId,
                         g_ui.focusedPane().gridSearchFocusFramesLeft, g_ui.focusedPane().focused ? 1 : 0);
        }
        IM_CHECK_NO_RET(focused);
        IM_CHECK_NO_RET(g_ui.focusedPane().gridSearchFocusFramesLeft == 0);
        ctx->KeyChars("zz-no-such-title");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(g_ui.focusedPane().gridSearchBuf, "zz-no-such-title");

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(g_ui.focusedPane().gridSearchBuf, "");
        IM_CHECK_NO_RET(GImGui->ActiveId == 0);

        g_ui.focusedPane().gridSearchBuf[0] = '\0';
        ctx->Yield();
    };
}

std::string QuickCreateSummaryText() {
    return g_ui.quickCreateSummaryBuf.empty() ? std::string() : std::string(g_ui.quickCreateSummaryBuf.data());
}

// Open the quick-create popup the way issue.quick_create.open does. True once the summary
// field (focused on open) holds the caret.
bool OpenQuickCreateWithCaret(ImGuiTestContext* ctx) {
    g_ui.showQuickCreateIssue = true;
    g_ui.quickCreateOpenLatch = true;
    return YieldUntil(ctx, [] { return !g_ui.quickCreateOpenLatch && GImGui->ActiveId != 0; }, 30);
}

void EraseQuickCreateDraftForTest() {
    smatchet::drafts::Erase("quickcreate.summary", "popup");
    smatchet::drafts::Erase("quickcreate.description", "popup");
    smatchet::drafts::Erase("quickcreate.maybe_created", "popup");
}

std::string QuickCreateDescriptionText() {
    return g_ui.quickCreateDescBuf.empty() ? std::string() : std::string(g_ui.quickCreateDescBuf.data());
}

// Audit #51: what is typed in Quick Create survives closing the window. It comes back at the next open
// with the restored line; Clear drops it, and so does a confirmed Discard.
void RegisterQuickCreateDraftKept(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "QuickCreate_DraftKeptAndRestored");
    t->GuiFunc = ClearActiveIdGui;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_ui.quickCreateArgSummary.clear();
        g_ui.quickCreateArgDescription.clear();
        EraseQuickCreateDraftForTest();
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->KeyChars("Kept title");
        ctx->SetRef("Quick Create Issue");
        ctx->ItemClick("##quickcreate_desc");
        ctx->KeyChars("Kept body");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "Kept title");
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.description", "popup").c_str(), "Kept body");

        // Closed the way the window's X closes it, then reopened: both come back, with the restored line.
        DeactivateFieldForTest(ctx); // the X is clicked: the field leaves the caret first
        g_ui.showQuickCreateIssue = false;
        ctx->Yield(2);
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "Kept title");
        IM_CHECK_STR_EQ_NO_RET(QuickCreateDescriptionText().c_str(), "Kept body");
        IM_CHECK_NO_RET(g_ui.quickCreateDescUserEdited); // the context prefill must not replace it
        ctx->SetRef("Quick Create Issue");
        IM_CHECK_NO_RET(ctx->ItemExists("Clear"));

        // Clear empties both fields and the kept entry.
        ctx->ItemClick("Clear");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "");
        IM_CHECK_STR_EQ_NO_RET(QuickCreateDescriptionText().c_str(), "");
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "");
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.description", "popup").c_str(), "");

        // Typed again, then a confirmed Discard: nothing is kept.
        ctx->ItemClick("##quickcreate_summary");
        ctx->KeyChars("Throwaway");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "Throwaway");
        const int popupsBefore = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsBefore + 1);
        ctx->ItemClick("//$FOCUSED/Discard");
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.showQuickCreateIssue);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "");

        ClosePopupsAbove(popupsBefore);
        EraseQuickCreateDraftForTest();
        g_ui.showQuickCreateIssue = false;
        ctx->Yield();
    };
}

// Audit #51: an open that passes its own text (summary=) neither restores nor rewrites the kept entry until
// the user edits; a create that may have gone through marks the entry, and the next open says to check.
void RegisterQuickCreateDraftArgsAndMaybeCreated(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "QuickCreate_DraftArgsAndMaybeCreated");
    t->GuiFunc = ClearActiveIdGui;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_ui.quickCreateArgSummary.clear();
        g_ui.quickCreateArgDescription.clear();
        EraseQuickCreateDraftForTest();
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->KeyChars("Mine");
        ctx->Yield(2);
        DeactivateFieldForTest(ctx); // the X is clicked: the field leaves the caret first
        g_ui.showQuickCreateIssue = false;
        ctx->Yield(2);

        // Opened with summary=: the caller's text shows, and the kept entry stays as it was.
        g_ui.quickCreateArgSummary = "From the caller";
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "From the caller");
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "Mine");
        ctx->SetRef("Quick Create Issue");
        IM_CHECK_NO_RET(!ctx->ItemExists("Clear"));

        // Discarded without an edit: the kept draft this open never showed stays kept.
        const int popupsBefore = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsBefore + 1);
        ctx->ItemClick("//$FOCUSED/Discard");
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.showQuickCreateIssue);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "Mine");
        ClosePopupsAbove(popupsBefore);

        // Sent with the caller's text and edited while that create runs: its result still leaves the kept draft
        // alone. (Every check in the block is _NO_RET, so the promise always gets its value.)
        g_ui.quickCreateArgSummary = "From the caller";
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->Yield(2);
        {
            std::promise<IssueCreateResult> sent;
            g_ui.quickCreateFuture = sent.get_future();
            g_ui.quickCreateInFlight = true; // what Submit does
            ctx->KeyPress(ImGuiKey_End);
            ctx->KeyChars(" too");
            ctx->Yield(2);
            IssueCreateResult landed;
            landed.Ok = true;
            landed.IssueKey = "TEST-2";
            sent.set_value(landed);
            // The result closes the window with the caret still in the summary. ImGui hands that field's edited
            // text back to an input drawn within a frame of its deactivation, which no user reopen is fast enough
            // to hit, so let those frames pass before the next open.
            ctx->Yield(4);
        }
        IM_CHECK_NO_RET(!g_ui.showQuickCreateIssue);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "Mine");

        g_ui.quickCreateArgSummary = "From the caller";
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->Yield(2);
        // Once the user edits, the popup's text is theirs and is kept. (The open selects the summary; End
        // keeps it.)
        ctx->KeyPress(ImGuiKey_End);
        ctx->KeyChars("!");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "From the caller!");

        // A create that may have gone through: the entry is marked; the next open restores it and says so.
        IssueCreateResult maybe;
        maybe.Error = "Create issue failed: timed out";
        maybe.ReplayMayDuplicate = true;
        std::promise<IssueCreateResult> done;
        done.set_value(maybe);
        g_ui.quickCreateFuture = done.get_future();
        g_ui.quickCreateInFlight = true;
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.maybe_created", "popup").c_str(), "1");
        DeactivateFieldForTest(ctx); // the X is clicked: the field leaves the caret first
        g_ui.showQuickCreateIssue = false;
        ctx->Yield(2);
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "From the caller!");
        ctx->SetRef("Quick Create Issue");
        IM_CHECK_NO_RET(ctx->ItemExists("Clear"));
        ctx->ItemClick("Clear");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "");
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.maybe_created", "popup").c_str(), "");

        // Reopened while its create still runs: the sent text shows as sent, nothing is restored over it, the
        // create is still polled, and its result erases the entry. (Every check below is _NO_RET, so the promise
        // always gets its value: a broken promise would throw from the poll.)
        DeactivateFieldForTest(ctx);
        g_ui.showQuickCreateIssue = false;
        ctx->Yield(2);
        EraseQuickCreateDraftForTest();
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->KeyChars("Sent");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "Sent");
        std::promise<IssueCreateResult> running;
        g_ui.quickCreateFuture = running.get_future();
        g_ui.quickCreateInFlight = true; // what Submit does
        DeactivateFieldForTest(ctx);
        g_ui.showQuickCreateIssue = false;
        ctx->Yield(2);
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->Yield(2);
        IM_CHECK_NO_RET(g_ui.quickCreateInFlight);
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "Sent");
        ctx->SetRef("Quick Create Issue");
        IM_CHECK_NO_RET(!ctx->ItemExists("Clear"));
        IssueCreateResult created;
        created.Ok = true;
        created.IssueKey = "TEST-1";
        running.set_value(created);
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.quickCreateInFlight);
        IM_CHECK_NO_RET(!g_ui.showQuickCreateIssue);
        IM_CHECK_STR_EQ_NO_RET(KeptDraftText("quickcreate.summary", "popup").c_str(), "");

        EraseQuickCreateDraftForTest();
        g_ui.quickCreateError.clear();
        g_ui.showQuickCreateIssue = false;
        ctx->Yield();
    };
}

// Audit #50: Esc in the quick-create popup with a summary typed keeps the text and asks before
// discarding; Esc with nothing typed closes it.
void RegisterQuickCreateEscKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "QuickCreate_EscAsksWhenTyped");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_ui.quickCreateArgSummary.clear();
        g_ui.quickCreateArgDescription.clear();
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ctx->KeyChars("Typed title");
        ctx->Yield(2);
        const int popupsBefore = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(g_ui.showQuickCreateIssue);
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "Typed title");
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsBefore + 1);
        // Esc on the question closes it, and only it: the popup stays open with the text, and
        // the question does not come straight back.
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsBefore);
        IM_CHECK_NO_RET(g_ui.showQuickCreateIssue);
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "Typed title");
        ClosePopupsAbove(popupsBefore);
        g_ui.showQuickCreateIssue = false;
        ctx->Yield(2);

        // Reopened with nothing typed (the kept draft is dropped; QuickCreate_DraftKeptAndRestored covers it):
        // Esc closes it without a question.
        EraseQuickCreateDraftForTest();
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        IM_CHECK_STR_EQ_NO_RET(QuickCreateSummaryText().c_str(), "");
        const int popupsEmpty = GImGui->OpenPopupStack.Size;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.showQuickCreateIssue);
        IM_CHECK_NO_RET(GImGui->OpenPopupStack.Size == popupsEmpty);

        g_ui.showQuickCreateIssue = false;
        ctx->Yield();
    };
}

// A window that is on screen and is neither the quick-create popup nor one of its popups.
ImGuiWindow* UnrelatedLiveWindow(ImGuiWindow* quickCreate) {
    for (ImGuiWindow* w : GImGui->Windows) {
        if (w != nullptr && w->WasActive && (w->Flags & ImGuiWindowFlags_Popup) == 0 &&
            w->RootWindow != quickCreate->RootWindow) {
            return w;
        }
    }
    return nullptr;
}

// A popup that belongs to another window is open: Esc in quick-create still closes quick-create. Only the popups
// quick-create opened itself (a dropdown, the discard question) hold the Esc back for the frame that closes them.
void RegisterQuickCreateEscIgnoresOtherWindowsPopups(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "QuickCreate_EscIgnoresOtherWindowsPopups");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_ui.quickCreateArgSummary.clear();
        g_ui.quickCreateArgDescription.clear();
        IM_CHECK_NO_RET(OpenQuickCreateWithCaret(ctx));
        ImGuiWindow* quickCreate = ImGui::FindWindowByName("Quick Create Issue");
        IM_CHECK_NO_RET(quickCreate != nullptr);
        ImGuiWindow* other = quickCreate ? UnrelatedLiveWindow(quickCreate) : nullptr;
        IM_CHECK_NO_RET(other != nullptr);
        if (other == nullptr) {
            g_ui.showQuickCreateIssue = false;
            return;
        }
        const int popupsBefore = GImGui->OpenPopupStack.Size;
        // The popup entry of the other window, as OpenPopup would leave it.
        ImGuiPopupData fake;
        fake.PopupId = ImGui::GetID("##quickcreate_test_other_window_popup");
        fake.Window = other;
        fake.OpenFrameCount = GImGui->FrameCount;
        GImGui->OpenPopupStack.push_back(fake);
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_NO_RET(!g_ui.showQuickCreateIssue);
        ClosePopupsAbove(popupsBefore);
        g_ui.showQuickCreateIssue = false;
        ctx->Yield();
    };
}

void RegisterNewIssueDraftRowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "NewIssueDraftRow_RendersActiveBranchOnFixtureData");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo(
                "SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — no tickets load, grid table short-circuits");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const char* kWindowTitle = kActiveProjectTitle;
        const bool visible = BootFixtureActiveProjectGrid(ctx, *app);

        if (visible) {
            // Arm the draft. Seed a synthetic project + summary so the active branch
            // has content; re-arm newIssueDraftActive each frame (a grid-context
            // change frame could otherwise clear it) and let the row tick.
            g_ui.newIssueDraft = IssueDraft{};
            g_ui.newIssueDraft.ProjectKey = "SMAT";
            g_ui.newIssueDraft.FieldValues["summary"] = "Draft row render smoke";
            g_ui.newIssueDraftActive = true;
            for (int i = 0; i < 30; ++i) {
                g_ui.newIssueDraftActive = true;
                ctx->Yield();
            }
            // The active draft row drew across all those frames without the engine
            // trapping an in-frame assertion. newIssueDraftActive surviving proves the
            // row ran its active branch (Cancel would have cleared it).
            IM_CHECK_NO_RET(g_ui.newIssueDraftActive);
            IM_CHECK_NO_RET(WindowIsLive(kWindowTitle));
        }

        // Restore: disarm the draft and clear the synthetic payload.
        g_ui.newIssueDraftActive = false;
        g_ui.newIssueDraft = IssueDraft{};
        g_ui.newIssueDraftEditBufs.clear();
        g_ui.newIssueMissingFieldIds.clear();
        ctx->Yield();
    };
}

// ============================================================================
// 8. Offline Queue panel
// ============================================================================
// DrawUnifiedOfflineQueuesPanel (SmatchetOfflineQueueUi.cpp), drawn inline by
// drawActiveProjectUnsavedStrip (focused pane only) — NOT a standalone window. The
// panel is invoked every frame from the docked "Smatchet - Active Project" window;
// it early-returns when the offline queue is empty (total==0).
//
// FIXTURE-GATED setup (mirrors grid_pane_windows + case 7): "Smatchet - Active
// Project" is the main grid pane, drawn with ImGuiWindowFlags_NoTitleBar and docked
// (SmatchetActiveProjectGridUi.cpp:455-471). Its Begin() only returns true — making
// the window Active / WindowIsLive — once its dock tab is the selected one, which
// the minimal no-fixture boot never reaches (no tickets ⇒ pane never forwarded ⇒
// SetWindowFocus() at line 469-471 is skipped because Begin() bailed first: a
// catch-22). So like every sibling that drives this window we gate on the fixture
// env + run a real backend sync to load tickets before asserting the window live.
//
// Two paths, both covered (post-fixture):
//   a) EMPTY (always): the host window being live proves the panel's empty-guard
//      branch (FetchOfflineQueueData → total==0 → return false) ticked under the
//      engine's in-frame assert trap. This is the HARD assertion.
//   b) POPULATED (opportunistic): we enqueue one synthetic create via
//      AppController::QueueCreateOffline (a pure local-SQLite write — no backend) so
//      total>0 forces the panel BODY (header + toolbar + table) to render. The
//      OfflineQueueService is only constructed when the local cache is live, which
//      the minimal UiTestScenario boot does not always provide — so when
//      QueueCreateOffline returns 0 we log it and stay on the empty-path coverage
//      rather than failing (the test self-upgrades the day the harness gains a live
//      cache; see the tooling backlog entry). When it succeeds we clean up the row
//      via DeletePendingCreates so it never leaks into a later test.
void RegisterOfflineQueuePanelRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "OfflineQueuePanel_RendersInlineInActiveProject");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — the docked Active Project host "
                         "window does not become live under the minimal boot (same gate as NewIssueDraftRow / "
                         "grid_pane_windows); the inline offline-queue panel is unreachable without it");
            return;
        }

        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Load the fixture grid so the host pane docks + activates (mirrors case 7).
        app->SyncWithBackend();
        const bool syncDone = YieldUntil(ctx, [&] { return !app->IsSyncPendingOrActive(); });
        IM_CHECK_NO_RET(syncDone);
        IM_CHECK_NO_RET(!app->GetActiveTickets().empty());

        const char* kWindowTitle = "Smatchet - Active Project";
        ctx->SetRef(kWindowTitle);
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestActiveProjectFocus = true;
            return WindowIsLive(kWindowTitle);
        });
        // Empty-path coverage: the host window ticked, so the inline panel's
        // total==0 early-return branch ran without an ImGui assertion.
        IM_CHECK_NO_RET(visible);
        if (!visible) {
            return;
        }

        // Populated-path coverage (opportunistic — needs a live local cache).
        IssueDraft draft;
        draft.ProjectKey = "SMAT";
        draft.IssueTypeName = "Task";
        draft.FieldValues["summary"] = "Offline queue panel render smoke";
        const std::int64_t queuedId = app->QueueCreateOffline(draft);
        const bool withLocalCache = UiTestWithLocalCache();
        if (queuedId <= 0) {
            if (withLocalCache) {
                // Opt-in is set: EnsureLocalCacheForUiTest() stood up a live cache AND
                // cleared the first-run ReadOnlyMode default, so QueueCreateOffline MUST
                // have returned a real pending-create id. A 0/negative return here means
                // a guard re-fired (read-only default crept back, or the cache is dead) —
                // the exact #1521 regression. Hard-fail so it can never silently SKIP back.
                ctx->LogError("OfflineQueue populated path FAILED to activate under "
                              "SMATCHET_UITEST_WITH_LOCAL_CACHE=1: QueueCreateOffline returned %lld "
                              "(expected a positive pending-create id). Regression of #1521.",
                              static_cast<long long>(queuedId));
                IM_CHECK_NO_RET(queuedId > 0);
                return;
            }
            ctx->LogInfo("OfflineQueue populated path SKIPPED: QueueCreateOffline returned %lld "
                         "(OfflineQueueService not constructed — no live local cache under this harness). "
                         "Empty-guard path covered above.",
                         static_cast<long long>(queuedId));
            return;
        }
        // Populated path activated: a real pending-create exists, so the panel body must
        // render past the total==0 guard. Under the opt-in this is the locked assertion.
        ctx->LogInfo("OfflineQueue populated path ACTIVATED: QueueCreateOffline returned id=%lld "
                     "(withLocalCache=%d)",
                     static_cast<long long>(queuedId), withLocalCache ? 1 : 0);
        // Precondition for the panel body: total>0, so it must render past the guard.
        IM_CHECK_NO_RET(!app->GetPendingCreates().empty());
        const bool stillVisible = YieldUntil(ctx, [&] {
            g_ui.requestActiveProjectFocus = true;
            return WindowIsLive(kWindowTitle);
        });
        IM_CHECK_NO_RET(stillVisible);
        if (stillVisible) {
            // The panel wraps its toolbar in PushID("unifiedOfflineQueues"); address
            // the Copy button through that scope. Best-effort (logged, not asserted) —
            // the host-window-live + non-empty-queue pair is the hard past-guard proof.
            const bool copyBtnPresent = ctx->ItemExists("unifiedOfflineQueues/Copy selected##unifiedoff");
            ctx->LogInfo("OfflineQueue toolbar Copy button reachable: %d", copyBtnPresent ? 1 : 0);
        }

        // Restore: delete the synthetic row so it never leaks into a later test.
        app->DeletePendingCreates({queuedId});
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterDataDependentWindowsSmokeTests(ImGuiTestEngine* engine) {
    RegisterViewsDashboardWindowRenderSmoke(engine);
    RegisterBulkExportWindowRenderSmoke(engine);
    RegisterPlanDocViewerWindowRenderSmoke(engine);
    RegisterPerformanceWindowRenderSmoke(engine);
    RegisterAnnotateAnalysisWindowRenderSmoke(engine);
    RegisterAttachmentPreviewWindowRenderSmoke(engine);
    RegisterNewIssueDraftRowRenderSmoke(engine);
    RegisterNewIssueDraftRowKeys(engine);
    RegisterNewIssueDraftRowDraftKept(engine);
    RegisterNewIssueDraftRowKeptDraftSettled(engine);
    RegisterGridSearchFocusKeys(engine);
    RegisterQuickCreateEscKeys(engine);
    RegisterQuickCreateDraftKept(engine);
    RegisterQuickCreateDraftArgsAndMaybeCreated(engine);
    RegisterQuickCreateEscIgnoresOtherWindowsPopups(engine);
    RegisterOfflineQueuePanelRenderSmoke(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
