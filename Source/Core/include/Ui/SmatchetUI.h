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

/// Shared per-frame state for the section helpers that decompose
/// SmatchetUI::drawActiveProjectWindow. Defined at namespace scope in
/// SmatchetActiveProjectGridUi.cpp (the only TU that constructs it); forward-declared
/// here so the private section-helper member signatures can name it. Holds references
/// to orchestrator-owned stack state captured once at the top of the frame — never
/// re-fetched per helper.
struct ActiveProjectDrawCtx;

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
    void drawActiveProjectWindow(AppController& app, UiDrawSession& d);
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
    // Google-Sheets-style per-cell selection gesture (formerly the handleCellRectSel
    // lambda). Cell geometry passed as floats to avoid an imgui include in this header.
    void handleActiveProjectCellRectSel(ActiveProjectDrawCtx& ctx, int rowIdx, int colIdx, float cellOriginX,
                                        float cellWidth, float groupMinY, float groupMaxY, bool isIdCol,
                                        const std::string& issueId, bool activeIssueWasThisRow);
    void drawActiveProjectGridNewIssue(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridPost(ActiveProjectDrawCtx& ctx);
    void drawActiveProjectGridRectSelKeys(ActiveProjectDrawCtx& ctx);

    /// Hoisted singleton-window text buffers that were function-local `static`s inside
    /// drawActiveProjectWindow. Behaviour-identical for this single-instance window;
    /// promoting to members removes the statics so the section helpers stay reentrant-safe.
    struct ActiveProjectWindowState {
        char newViewNameBuf[128] = {}; // was `static char s_newViewName[128]`
        char lastFilterBuf[128] = {};  // was `static thread_local char lastFilter[128]`
    };
    ActiveProjectWindowState activeProjectState_;
    void drawAttachmentPreviewWindow(AppController& app, UiDrawSession& d);
    static void drawAuditWindow(AppController& app, UiDrawSession& d);
    static void drawLogWindow(UiDrawSession& d);
    static void drawBulkImportWindow(AppController& app, UiDrawSession& d);
    static void drawBulkExportWindow(AppController& app, UiDrawSession& d);
    static void prepareTopLevelWindow(UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                      bool requestFocus = false);
    static void repairTopLevelWindow(UiDrawSession& d, const char* layoutKey, float minW, float minH);
    void resetWindowLayoutToDefault(UiDrawSession& d);
};
