#pragma once

#include "AnnotateAnalysisUi.h"
#include "Commands/CommandPaletteUi.h"
#include "ConfigManager.h"
#include "SmatchetToolbarUi.h"
#include "SmatchetPreferencesUi_detail.h"
#include "SmatchetThemeIds.h"
#include "TicketGridModel.h"
#include "Views.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AppController; // Forward declaration
struct UiDrawSession;
struct CachedTicket;
struct GridPane; // Source/Core/include/GridPane.h — per-pane dockable grid unit (ADR-0018)
struct TrackerConnectivityBannerForUi; // AppController.h — host-resolved once per frame

/// Shared per-frame state for the section helpers that decompose
/// SmatchetUI::drawActiveProjectWindow. Defined at namespace scope in
/// SmatchetActiveProjectGridUi.cpp (the only TU that constructs it); forward-declared
/// here so the private section-helper member signatures can name it. Holds references
/// to orchestrator-owned stack state captured once at the top of the frame — never
/// re-fetched per helper.
struct ActiveProjectDrawCtx;

/// Whether the current renderer backend can upload bitmap thumbnails, plus a
/// human-readable reason (shown when it can't). Computed once per frame in
/// SmatchetUI::drawAttachmentPreviewWindow and threaded through the section helpers.
struct AttachmentThumbnailSupport {
    bool CanRenderBitmapThumbnails = false;
    std::string Reason;
};
/// Shared per-frame state for the section helpers that decompose
/// SmatchetUI::drawMainMenuBar. Defined at file scope in SmatchetUI_MainMenu.cpp (the only
/// TU that constructs it); forward-declared here so the private per-menu helper signatures
/// can name it. Holds the active-tickets snapshot, catalog index, column set, and derived
/// selection flags captured once at the top of the menu-bar frame.
struct MainMenuDrawCtx;

/// Shared per-frame state for the section helpers that decompose
/// SmatchetUI::drawViewsDashboardWindow. Defined at namespace scope in
/// SmatchetViewsDashboardUi.cpp (the only TU that constructs it); forward-declared here so
/// the private section-helper member signatures can name it. Holds references to the active
/// view, store, and the action closures captured once at the top of the frame.
struct ViewsDashboardDrawCtx;

/// Per-frame cache for TrackerFieldCatalogIndex + TicketGridColumns, keyed by catalog revision and
/// active view id. Built once per frame in SmatchetUI::Draw before drawMainMenuBar and
/// drawActiveProjectWindow so neither rebuilds it independently.
struct GridFrameContext {
    std::uint64_t catalogRevision = 0;
    std::uint64_t viewsRevision = 0;
    std::string activeViewId;
    std::unique_ptr<TrackerFieldCatalogIndex> catalogIndex;
    std::vector<TicketGridColumn> columns;
};

/**
 * Blocking join for UiDrawSession std::async futures that capture `app` (field catalog fetch,
 * grid field commit, new-issue create, bulk-import rows). `~AppController` calls this first; safe
 * to call again if futures are already drained.
 */
void DrainUiDrawSessionFuturesBeforeAppTeardown(AppController& app);

/**
 * Free-function shim so command handlers (which hold no SmatchetUI*) can trigger a full dock
 * layout reset. Must be called on the UI thread. Equivalent to the menu Workspace > Reset
 * Workspace Layout path.
 */
void SmatchetUI_ResetLayoutToDefault(UiDrawSession& d);

class SmatchetUI {
  public:
    void Draw(AppController& app);

    // Forwarding shims for split-TU helpers in SmatchetPreferencesUi_*.cpp.
    const ViewsStore& GetViewsStore() const { return ViewState.GetStore(); }
    void DrawAnnotatePreferencesTabForwarded(const AppController& app) {
        annotateAnalysisUi_.DrawAnnotatePreferencesTab(app);
    }

    /// Ring-buffer LRU of recently toggled view command ids (capacity 5, oldest-first on read).
    class RecentViewLru {
      public:
        static const int kCapacity = 5;

        RecentViewLru() : size_(0) {}

        /// Record a view toggle command id (e.g. "view.toggle.log"). O(N), N <= 5.
        /// Silently ignores ids that do not start with "view.toggle." — only known
        /// view-toggle commands may enter the LRU, preventing future callers from
        /// accidentally dispatching arbitrary command names via the Recent Views submenu.
        void Touch(const std::string& commandId) {
            static const char kPrefix[] = "view.toggle.";
            if (commandId.compare(0U, sizeof(kPrefix) - 1U, kPrefix) != 0) {
                return;
            }
            // Remove existing occurrence to avoid duplicates.
            std::string* const end = entries_ + size_;
            std::string* const pos = std::find(entries_, end, commandId);
            if (pos != end) {
                for (std::string* p = pos; p < end - 1; ++p) {
                    *p = *(p + 1);
                }
                --size_;
            }
            // Evict oldest if at capacity.
            if (size_ == kCapacity) {
                for (int i = 0; i < kCapacity - 1; ++i) {
                    entries_[i] = entries_[i + 1];
                }
                --size_;
            }
            entries_[size_++] = commandId;
        }

        /// Returns up to kCapacity entries, oldest-first (most-recently used last).
        std::vector<std::string> Snapshot() const {
            std::vector<std::string> result;
            result.reserve(static_cast<size_t>(size_));
            for (int i = 0; i < size_; ++i) {
                result.push_back(entries_[i]);
            }
            return result;
        }

      private:
        std::string entries_[5];
        int size_;
    };

  private:
    Views ViewState;
    AnnotateAnalysisUi annotateAnalysisUi_;
    GridFrameContext gridFrameCtx_;
    smatchet::cmd::CommandPaletteUi commandPalette_;
    SmatchetToolbarUi toolbar_;
    // Tracks the palette currently applied to ImGui::GetStyle() so SmatchetUI::Draw can detect
    // a cfg.Theme change and re-apply once per dirty event (not every frame).
    ThemeId lastAppliedTheme_ = ThemeId::SmatchetDark;
    TrackerConfig::UiDensity lastAppliedDensity_ = TrackerConfig::UiDensity::Normal;
    RecentViewLru recentViews_;

    void drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d);
    void drawMainMenuBar(AppController& app, UiDrawSession& d);
    // Per-menu section helpers for drawMainMenuBar (function-size-compliance, monoliths
    // campaign). Each top-level helper owns its own BeginMenu/EndMenu pair identically to the
    // pre-decomposition body (BeginMenu returns bool; EndMenu is called only inside the
    // taken branch). The orchestrator retains the BeginMainMenuBar/EndMainMenuBar frame and
    // the trackerLocked BeginDisabled/EndDisabled groupings that bracket several menus.
    void selectAllGridRows(MainMenuDrawCtx& ctx);
    void drawMenuBarFileMenu(MainMenuDrawCtx& ctx);
    void drawMenuBarEditMenu(MainMenuDrawCtx& ctx);
    void drawMenuBarSelectionMenu(MainMenuDrawCtx& ctx);
    void drawMenuBarViewMenu(MainMenuDrawCtx& ctx);
    void drawMenuBarViewWindowToggles(MainMenuDrawCtx& ctx);
    void drawMenuBarAppearanceMenu(MainMenuDrawCtx& ctx);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    void drawMenuBarRunMenu(MainMenuDrawCtx& ctx);
#endif
    void drawMenuBarToolsMenu(MainMenuDrawCtx& ctx);
    void drawMenuBarHelpMenu(MainMenuDrawCtx& ctx);
    void drawMenuBarInlinePalette(MainMenuDrawCtx& ctx);
#if defined(SMATCHET_WITH_WHISPER)
    void drawMenuBarDictationIndicator(MainMenuDrawCtx& ctx);
#endif
#ifdef SMATCHET_EMBEDDED_IN_UNREAL
    void drawMenuBarUnrealCloseButton(MainMenuDrawCtx& ctx);
#endif
    // drawMenuBarAppearanceMenu sub-sections (function-size-compliance). Each runs inside the
    // active Appearance BeginMenu scope; the theme/density/font helper opens and closes its own
    // nested BeginMenu/EndMenu pairs, matching the pre-decomposition order verbatim.
    void drawAppearanceThemeDensityFont(MainMenuDrawCtx& ctx);
    void drawAppearancePanelPosition(MainMenuDrawCtx& ctx);

    // Section helpers for SmatchetUI::Draw (function-size-compliance, monoliths campaign).
    // No positional-ImGui scope pair is split across a helper boundary; each helper either
    // opens and closes its own scope internally or runs entirely outside any scope.
    // Pre-existing perf-scope seams are reused verbatim, so no new perf markers appear.
    void drawInitConfigOnce(AppController& app, UiDrawSession& d);
    void drawApplyAppearanceSettings(UiDrawSession& d);
    void drawPerFrameTicksAndHandlers(AppController& app, UiDrawSession& d);
    void drawPreWindowOverlays(AppController& app, UiDrawSession& d);
    void drawViewStateAndConnectivity(AppController& app, UiDrawSession& d);
    void drawChromeAndModeToggles(AppController& app, UiDrawSession& d);
    void handleViewKeyboardShortcuts(UiDrawSession& d);
    void handlePanelVisibilityShortcuts(UiDrawSession& d);
    void handleViewRevealShortcuts(UiDrawSession& d);
    void drawSecondaryWindows(AppController& app, UiDrawSession& d);
    // Tail half of drawSecondaryWindows (toasts onward) — split for function-size compliance.
    // Each contained window owns its own Begin/End scope; no scope crosses the boundary.
    void drawSecondaryWindowsTail(AppController& app, UiDrawSession& d);
    void drawDockDebugOverlay(UiDrawSession& d);
    void drawEndOfFramePersistence(UiDrawSession& d);

    /// Hoisted Draw-body function-local `static`s. Behaviour-identical for this single
    /// SmatchetUI instance; promoting to members removes the statics so the section
    /// helpers stay reentrant-safe (no cross-window leakage). The Zen-mode key-chord
    /// detector and the dock-debug throttle counters lived as `static` locals in the
    /// pre-decomposition Draw body.
    struct DrawBodyState {
        // Zen Mode: Ctrl+M then Z chord (1 s timeout).
        bool zenChordPrefixArmed = false;
        float zenChordTimeoutSec = 0.0f;
        // Esc Esc to exit Zen Mode.
        int zenEscCount = 0;
        float zenEscTimer = 0.0f;
        // Dock-debug overlay LOG_DEBUG throttle (every 120 frames).
        int dockDebugLogFrame = 0;
    };
    DrawBodyState drawBodyState_;
#if defined(SMATCHET_WITH_AI)
    /// Right-anchored Smatchet Assistant side panel. Delegates to the free function in
    /// `SmatchetAiAssistantUi.cpp` after `drawAuditWindow` runs; early-returns inside
    /// the free function when `d.assistantPanelOpen` is false.
    void drawAiAssistantPanel(AppController& app, UiDrawSession& d);
#endif
    void drawPreferencesWindow(AppController& app, UiDrawSession& d);
    // Section helpers for drawPreferencesWindow (full-function-size-compliance Phase 5,
    // PR E11). The orchestrator owns the positional Begin/EndTabBar/End frame; each helper
    // runs inside that frame and never splits a positional-ImGui pair across the call
    // boundary. No SMATCHET_UI_PERF_SCOPE seams exist on this window, so the split is on
    // logical sections only (no new perf scopes added).
    void resetPreferencesWindowState(UiDrawSession& d);
    bool beginPreferencesWindow(UiDrawSession& d);
    void loadPreferencesBuffers(UiDrawSession& d);
    void drawPreferencesTrackerTab(UiDrawSession& d);
#if defined(SMATCHET_WITH_MCP)
    void drawPreferencesIntegrationsTab(AppController& app, UiDrawSession& d);
#endif
    void onPreferencesSaveAndSync(AppController& app, UiDrawSession& d);

    /// Hoisted singleton-window lazy-load flags that were a function-local `static`
    /// inside drawPreferencesWindow. Behaviour-identical for this single-instance
    /// window; promoting to a member removes the static so the section helpers stay
    /// reentrant-safe and the close-reset path can clear it without a free-function
    /// reach-around.
    struct PreferencesWindowState {
        SmatchetPreferencesUiTemplateFlags templateFlags;
    };
    PreferencesWindowState preferencesState_;
    void drawViewsDashboardWindow(AppController& app, UiDrawSession& d);
    // Section helpers for drawViewsDashboardWindow (function-size decomposition). Each owns its
    // own positional-ImGui Begin/End pairs in full — no pair is split across the orchestrator/
    // helper boundary. The per-tab helpers each own their BeginTabItem/EndTabItem (EndTabItem
    // runs only when BeginTabItem returned true, preserved verbatim). ViewsDashboardDrawCtx
    // carries the active view, store, and the action closures captured once at frame top.
    void drawViewsSidebar(ViewsDashboardDrawCtx& ctx);
    void drawViewsEditorHeader(ViewsDashboardDrawCtx& ctx);
    void drawViewsFilterTab(ViewsDashboardDrawCtx& ctx);
    void drawViewsFieldsTab(ViewsDashboardDrawCtx& ctx);
    void drawViewsColumnsTab(ViewsDashboardDrawCtx& ctx);
    void drawViewsSortTab(ViewsDashboardDrawCtx& ctx);
    // Sort-tab sub-sections split out of drawViewsSortTab under the function-size cap: the
    // drag-reorderable per-key rows, and the "+ Add sort key" popup picker.
    void drawViewsSortRows(ViewsDashboardDrawCtx& ctx, ViewDefinition* mutableActive);
    void drawViewsAddSortKeyPopup(ViewsDashboardDrawCtx& ctx, ViewDefinition* mutableActive);
    void drawViewsModals(ViewsDashboardDrawCtx& ctx);
    // Action + chrome helpers split out of drawViewsDashboardWindow under the function-size cap. The
    // five "views*" methods hold the former action-closure bodies (the lambdas bound into
    // ViewsDashboardDrawCtx now just forward to these); the banner + shortcut helpers are chrome.
    void viewsApplyAndSync(AppController& app, UiDrawSession& d, const ViewDefinition* activeView);
    void viewsDiscardChanges(UiDrawSession& d);
    void viewsActivateView(AppController& app, UiDrawSession& d, const std::string& id);
    void viewsRequestActivate(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                              const std::string& id);
    void viewsCreateNewView(UiDrawSession& d, const ViewDefinition* activeView);
    void drawViewsConnectivityBanner(AppController& app, UiDrawSession& d);
    void handleViewsDashboardShortcuts(AppController& app, UiDrawSession& d, const ViewDefinition* activeView);
    // Pane-window host (multi-grid-tabs Slice 2): bootstraps d.gridPanes from
    // smatchet_panes.json, draws one dockable window per pane via the re-entrant
    // drawActiveProjectWindow below, tracks focus, applies pane add/close after the
    // loop (min 1 pane survives), and debounce-saves the panes file. Defined in
    // SmatchetGridPaneWindows.cpp.
    void drawGridPaneWindows(AppController& app, UiDrawSession& d);
    // Per-frame steady-state sync for the focused pane: follows the active view
    // (viewId/title), and on a focus SWITCH activates the pane's view + lets the
    // sync path swap the backend (Slice-2: one live context, focused pane drives it).
    void syncFocusedPaneWithActiveView(AppController& app, UiDrawSession& d, GridPane& pane, bool focusSwitched);
    // Re-entrant per-pane grid window (Slice 2): renders ONE GridPane, using the
    // pane's own snapshot + sort/filter caches. Called once per visible pane per frame.
    // The connectivity banner is resolved ONCE per frame by the host and passed in.
    void drawActiveProjectWindow(AppController& app, UiDrawSession& d, GridPane& pane,
                                 const TrackerConnectivityBannerForUi& trackerBanner);
    // Pane-view resolution helpers (Slice 2). resolvePaneView falls back to the active
    // view when the pane's view is absent from the loaded bucket — self-repairing
    // pane.viewId ONLY when the pane belongs to the focused backend's bucket (a
    // cross-backend pane's viewId is valid in its own bucket and must survive —
    // review HIGH-1); resolvePaneColumns reuses the shared GridFrameContext for the
    // active view and a per-pane revision-keyed cache otherwise.
    ViewDefinition* resolvePaneView(UiDrawSession& d, GridPane& pane);
    const std::vector<TicketGridColumn>& resolvePaneColumns(GridPane& pane, const TrackerFieldCatalogIndex& catalogIndex,
                                                            const ViewDefinition* paneView);
    // Section helpers for drawActiveProjectWindow (monoliths Slice 1b). Each owns one of
    // the pre-existing SMATCHET_UI_PERF_SCOPE seams VERBATIM. Positional-ImGui Begin/End
    // pairs that span the table body stay in the orchestrator; these helpers run inside
    // the active BeginTable scope and never split a pair across the call boundary.
    void drawActiveProjectHeader(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectUnsavedStrip(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectSaveAsNewModal(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridSetup(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridSort(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridRows(ActiveProjectDrawCtx& ctx);
    // Renders one grid cell (key or data column) including its rect-select hit-box +
    // overlay. Split out of drawActiveProjectGridRows to keep both under the function-
    // size cap. `clippedRow` is the current sort-order row index; `colIndex` the logical
    // column index. ImVec2 cannot appear here (no imgui in this header), so cell geometry
    // is exchanged through the float-typed rect-select helper below.
    void drawActiveProjectGridCell(ActiveProjectDrawCtx& ctx, const CachedTicket& ticket,
                                   const TicketGridColumn& column, int clippedRow, int colIndex,
                                   bool idKeySelectableSelected, bool activeIssueWasThisRow, float rowHeight);
    // Renders a non-Id (data) grid cell: write-state badge, saving vs editable value render, and the
    // trailing rect-select hit-box. Split out of drawActiveProjectGridCell under the function-size cap.
    // Cell geometry passed as floats (no imgui include in this header).
    void drawActiveProjectGridValueCell(ActiveProjectDrawCtx& ctx, const CachedTicket& ticket,
                                        const TicketGridColumn& column, int clippedRow, int colIndex, float cellOriginX,
                                        float cellWidth);
    // Google-Sheets-style per-cell selection gesture (formerly the handleCellRectSel
    // lambda). Cell geometry passed as floats to avoid an imgui include in this header.
    void handleActiveProjectCellRectSel(ActiveProjectDrawCtx& ctx, int rowIdx, int colIdx, float cellOriginX,
                                        float cellWidth, float groupMinY, float groupMaxY, bool isIdCol,
                                        const std::string& issueId, bool activeIssueWasThisRow);
    void drawActiveProjectGridNewIssue(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridPost(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridRectSelKeys(ActiveProjectDrawCtx& ctx);
    // View-switch + grid-context-change bookkeeping (resets unsaved-edit flags, recomputes
    // gridSortEnvironmentChanged). Split out of drawActiveProjectWindow under the function-size cap.
    void applyActiveProjectViewChange(ActiveProjectDrawCtx& ctx);
    // The TicketGrid BeginTable block: setup / sort / rows / new-issue / post section helpers plus the
    // post-layout inside-table click detection. Split out of drawActiveProjectWindow under the cap.
    void drawActiveProjectTable(ActiveProjectDrawCtx& ctx);

    /// Hoisted singleton-window text buffers that were function-local `static`s inside
    /// drawActiveProjectWindow. Behaviour-identical for this single-instance window;
    /// promoting to members removes the statics so the section helpers stay reentrant-safe.
    struct ActiveProjectWindowState {
        char newViewNameBuf[128] = {}; // was `static char s_newViewName[128]`
        // lastFilterBuf moved into GridPane (Slice 2) — the filter is per-pane state.
    };
    ActiveProjectWindowState activeProjectState_;
    void drawAttachmentPreviewWindow(AppController& app, UiDrawSession& d);
    /// Per-frame references shared across the attachment-preview section helpers.
    /// Snapshots (thumbnailSupport) are captured once in drawAttachmentPreviewWindow.
    struct AttachmentPreviewDrawCtx {
        AppController& app;
        UiDrawSession& d;
        const AttachmentThumbnailSupport& thumbnailSupport;
    };
    void drawAttachmentListPane(AttachmentPreviewDrawCtx& ctx);
    void drawAttachmentDetailsPane(AttachmentPreviewDrawCtx& ctx);
    static void drawAuditWindow(AppController& app, UiDrawSession& d);
    static void drawLogWindow(UiDrawSession& d);
    static void drawBulkImportWindow(AppController& app, UiDrawSession& d);
    static void drawBulkExportWindow(AppController& app, UiDrawSession& d);
    static void prepareTopLevelWindow(UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                      bool requestFocus = false);
    static void repairTopLevelWindow(UiDrawSession& d, const char* layoutKey, float minW, float minH);
    void resetWindowLayoutToDefault(UiDrawSession& d);
};
