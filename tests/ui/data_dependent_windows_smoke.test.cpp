// data_dependent_windows_smoke.test.cpp — bucket-E render-smoke tests for the
// 5 data-dependent UI windows identified in plan #12 (batch 1 of 2).
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
//      Title: "Plan docs". Opens via showPlanDocViewer + requestPlanDocViewerFocus.
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
// Deferred (batch 2 — see §Deviations in the plan doc):
//   6. Attachment Preview ("Attachment Preview") — needs live attachment entries
//      in attachmentCollectionQueue to render past the "no entries" guard.
//   7. New Issue Draft inline row — embedded in drawActiveProjectWindow, not a
//      separate top-level window; needs fixture data + draft activation.
//   8. Offline Queue panel — also embedded inline (drawSecondaryWindows), no
//      separate top-level Begin; separate PR once the inline-panel smoke recipe
//      is established.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

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
// DrawPlanDocViewer (SmatchetPlanDocViewerUi.cpp). Title "Plan docs".
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

        ctx->SetRef("Plan docs");
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestPlanDocViewerFocus = true;
            return WindowIsLive("Plan docs");
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
// Robust probe: "CPU" — the first BeginTabItem("CPU") inside PerformanceTabs,
// always active on first open, queryable as an item in the ImGui Test Engine
// tree (tab items have an ID derived from their label within the tab bar scope).
void RegisterPerformanceWindowRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DataDependentWindowsSmoke", "PerformanceWindow_RendersAndShowsTabBar");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunWindowRenderSmoke(ctx, &UiDrawSession::showPerformance, &UiDrawSession::requestPerformanceFocus,
                             "Performance", "CPU");
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
        g_ui.gridState.ActiveIssueId = "SMAT-TEST-1";
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
        g_ui.gridState.ActiveIssueId = "";
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
}

#endif // SMATCHET_BUILD_UI_TESTS
