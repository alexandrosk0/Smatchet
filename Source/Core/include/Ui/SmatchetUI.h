#pragma once

#include "AnnotateAnalysisUi.h"
#include "Commands/CommandPaletteUi.h"
#include "ConfigManager.h"
#include "SmatchetToolbarUi.h"
#include "SmatchetPreferencesUi_detail.h"
#include "SmatchetThemeIds.h"
#include "TicketGridModel.h"
#include "Ui/ImGuiHotkey.h"
#include "Ui/SmatchetHotkeyCapture.h"
#include "Ui/SmatchetUserInfoUi.h"
#include "Views.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AppController; // Forward declaration
struct UiDrawSession;
struct CachedTicket;
struct GridPane;                       // Source/Core/include/GridPane.h — per-pane dockable grid unit (ADR-0018)
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

/**
 * End-of-frame drain for a pending layout reset. SmatchetUI_ResetLayoutToDefault only LATCHES
 * (UiDrawSession::pendingLayoutReset) because it is invoked mid-frame; this applies the heavy
 * dock work (LoadIniSettingsFromMemory + force-redock arming) at the safe end-of-frame point.
 * No-op when the latch is clear. Must be called on the UI thread.
 */
void SmatchetUI_ApplyDeferredLayoutReset(UiDrawSession& d);

class SmatchetUI {
  public:
    void Draw(AppController& app);

    // Forwarding shims for split-TU helpers in SmatchetPreferencesUi_*.cpp.
    const ViewsStore& GetViewsStore() const { return ViewState.GetStore(); }
    void DrawAnnotatePreferencesTabForwarded(const AppController& app) {
        annotateAnalysisUi_.DrawAnnotatePreferencesTab(app);
    }

    /// Mark the parsed keybinding dispatch cache stale so the next frame rebuilds it
    /// from cfg.Keybindings (rebuildKeybindingCache). The Keyboard Shortcuts editor +
    /// the toolbar / command-palette quick-bind call this after mutating the binding
    /// table so a rebind takes effect immediately, without a restart.
    void MarkKeybindingsDirty() { keybindingCacheDirty_ = true; }

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
    SmatchetUserInfoUi userInfoUi_;
    GridFrameContext gridFrameCtx_;
    smatchet::cmd::CommandPaletteUi commandPalette_;
    SmatchetToolbarUi toolbar_;
    // Shared "Set shortcut..." quick-bind modal (toolbar button + command-palette row
    // context action). Owned here so it outlives the per-frame context menus that open
    // it; drawQuickBindPopup polls toolbar_/commandPalette_ for a fired request, opens
    // the modal, and persists + invalidates the dispatch cache on a commit/clear.
    smatchet::ui::QuickBindPopup quickBind_;
    // Tracks the palette currently applied to ImGui::GetStyle() so SmatchetUI::Draw can detect
    // a cfg.Theme change and re-apply once per dirty event (not every frame).
    ThemeId lastAppliedTheme_ = ThemeId::SmatchetDark;
    TrackerConfig::UiDensity lastAppliedDensity_ = TrackerConfig::UiDensity::Normal;
    // Touch-scaling dirty gate (dual-ui-mode slice 8): the effective mode + mobile
    // density last fed to SmatchetTheme::ApplyUiDensityScale + io.FontGlobalScale.
    // A flip into/out of Mobile (or a density change while in Mobile) rebuilds the
    // style at the new touch scale; desktop holds scale 1.0. Seeded Desktop/Comfortable
    // so the first frame in Desktop is a no-op and the first Mobile flip is detected.
    EffectiveUiMode lastAppliedEffectiveUiMode_ = EffectiveUiMode::Desktop;
    MobileTouchDensity lastAppliedMobileDensity_ = MobileTouchDensity::Comfortable;
    RecentViewLru recentViews_;

    // Parsed, ready-to-match form of cfg.Keybindings, built by rebuildKeybindingCache.
    // Disabled / unparseable bindings are dropped at build time so the per-frame
    // dispatch loop only walks valid entries. ArgsJson is kept as text and parsed at
    // dispatch (cold path: only when a hotkey actually fires).
    struct ParsedKeybinding {
        smatchet::ui::ImGuiBugHotkey hk;
        std::string commandId;
        std::string argsJson;
    };
    std::vector<ParsedKeybinding> keybindingCache_;
    bool keybindingCacheDirty_ = true;

    void drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d);
    // Consume the toolbar / command-palette "Set shortcut..." request latch and draw the
    // shared quick-bind modal once per frame (desktop path only — the toolbar + palette
    // are desktop chrome). On a commit/clear, persists cfg + marks the dispatch cache dirty.
    void drawQuickBindPopup(AppController& app, UiDrawSession& d);
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
    // Resolves cfg.UiMode -> d.effectiveUiMode once per frame at the top of Draw.
    // Manual Desktop/Mobile pin directly; Auto applies width hysteresis on
    // io.DisplaySize.x (enter Mobile <= 720 px, exit Desktop >= 860 px, hold the
    // previous frame's decision in the dead band). No fork yet in slice 2 — only
    // the resolved field + a temp overlay; the Draw() mobile fork lands in slice 3.
    void drawResolveUiMode(UiDrawSession& d);
    // ---- Mobile shell (dual-ui-mode-desktop-mobile plan, slice 3+) ----
    // When drawResolveUiMode yields EffectiveUiMode::Mobile, Draw() forks to
    // drawMobileShell instead of the desktop chrome + docked windows. The shell is a
    // single fullscreen NoDocking/NoTitleBar/NoSavedSettings window that fully occludes
    // the host's empty viewport dockspace; its three fixed bands are the top app bar, the
    // flex page-content region, and the bottom nav, with an overlay drawer. The host layer
    // never learns about mobile mode — see the plan's "central architectural bet".
    void drawMobileShell(AppController& app, UiDrawSession& d);
    void drawMobileTopAppBar(AppController& app, UiDrawSession& d);
    void drawMobilePageContent(AppController& app, UiDrawSession& d);
    void drawMobileBottomNav(AppController& app, UiDrawSession& d);
    void drawMobileDrawer(AppController& app, UiDrawSession& d);
    // Slice 5 — content dockspace + persistence. The Grid page hosts a local
    // MobileContentDock split (list over issue-detail); other pages stay single-fill.
    // The dock-windows helper submits the two dockable grid windows after the shell
    // window ends, the seed helper builds the one-shot vertical split, and the detail
    // helper renders the read-only field list for the active ticket. The two ini
    // helpers toggle io.IniFilename so mobile dock geometry persists to a separate
    // imgui_mobile.ini while the desktop imgui.ini stays untouched during mobile.
    void drawMobileGridDockWindows(AppController& app, UiDrawSession& d, unsigned int gridDockId);
    // Shared focused-pane setup for the mobile grid (slice 5/10): the desktop
    // drawGridPaneWindows loop is skipped in mobile mode, so both the single-fill
    // Grid page and the dockspace Tickets window reproduce its per-frame focus setup.
    // ensure*FocusedMobileGridPane loads panes + resolves the focused pane (fallback
    // to front), returns null only when there are no panes; drawEmbeddedFocusedGrid
    // marks focus, points AppController's focused-context delegators at it, syncs the
    // active view, and draws the embedded grid body against it.
    GridPane* ensureFocusedMobileGridPane(UiDrawSession& d);
    void drawEmbeddedFocusedGrid(AppController& app, UiDrawSession& d, GridPane& focused);
    void seedMobileGridDock(unsigned int gridDockId);
    void drawMobileGridDetail(AppController& app, UiDrawSession& d, GridPane* focused);
    void drawMobileEnsureIniAttached(UiDrawSession& d);
    void drawMobileRestoreDesktopIni(UiDrawSession& d);
    // Slice 6 — the mobile drawer's saved-views section. Defined in
    // SmatchetViewsDashboardUi.cpp so it can rebuild the file-local
    // ViewsDashboardDrawCtx + activate closures and reuse drawViewsSidebar
    // verbatim; selecting a view closes the drawer.
    void drawMobileDrawerViews(AppController& app, UiDrawSession& d);
    // P1.2 — touch view quick-switcher band drawn in the Grid page's chrome (between the app
    // bar and the content dock). A horizontal-scroll strip of saved-view tabs (active one
    // highlighted) + a trailing New-view button, routing through the same dirty-aware
    // viewsRequestActivate / viewsCreateNewView the drawer uses. Defined in
    // SmatchetViewsDashboardUi.cpp where the ViewState store + activate helpers are local.
    // bandHeight is the switcher-band height reserved by drawMobileShell.
    void drawMobileViewQuickSwitcher(AppController& app, UiDrawSession& d, float bandHeight);
    // Builds the per-frame ViewsDashboardDrawCtx shared by the two mobile Views paths
    // (drawer sidebar + the confirm modals), so neither rebuilds the closure set. Defined
    // in SmatchetViewsDashboardUi.cpp where ViewsDashboardDrawCtx is fully visible; the
    // reference members bind to app/d/the ViewState store, all of which outlive the frame.
    // sidebarWidth is caller-supplied: the drawer path passes its live content-region
    // width, the modal-only path passes 0 (it may run outside a live window, so
    // GetContentRegionAvail must not be queried there; the modals ignore the width).
    ViewsDashboardDrawCtx buildMobileViewsCtx(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                                              float sidebarWidth);
    // Renders the Views discard/delete-confirm modals at mobile-shell level every frame.
    // Must live OUTSIDE the drawer-open branch: the drawer's drawViewsSidebar latches the
    // confirm flags and closes the drawer on a dirty switch (#1117), so the consuming
    // modals would never render if they were nested in the drawer. Mirrors the desktop
    // drawViewsModals call site in drawViewsDashboardWindow.
    void drawMobileViewsModals(AppController& app, UiDrawSession& d);
    // Mode-independent floating overlays (toasts + app-update modal). Extracted from
    // drawSecondaryWindowsTail so both the desktop and mobile Draw paths render them.
    void drawGlobalOverlays(AppController& app, UiDrawSession& d);
    void drawApplyAppearanceSettings(UiDrawSession& d);
    void drawPerFrameTicksAndHandlers(AppController& app, UiDrawSession& d);
    void drawPreWindowOverlays(AppController& app, UiDrawSession& d);
    void drawViewStateAndConnectivity(AppController& app, UiDrawSession& d);
    void drawChromeAndModeToggles(AppController& app, UiDrawSession& d);
    // Rebindable keyboard-shortcut dispatch. Replaces the former hardcoded
    // handleViewKeyboardShortcuts / handlePanelVisibilityShortcuts /
    // handleViewRevealShortcuts: each frame dispatchKeybindings matches the parsed
    // cfg.Keybindings table against the current ImGui input and routes a match through
    // the command registry (the ui.command_palette pseudo-binding is handled inline so
    // it can self-gate on BackendHasBeenReachable, matching the pre-migration palette
    // toggle). The parse cache is rebuilt lazily when keybindingCacheDirty_ is set —
    // once after config load (drawInitConfigOnce) and on any future editor save.
    void dispatchKeybindings(AppController& app, UiDrawSession& d);
    void rebuildKeybindingCache(UiDrawSession& d);
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
    // embedded=true (dual-ui slice 4): mobile page-body fill — skip the panel's own
    // window chrome (Begin/End + the assistantPanelOpen gate + dock/focus mechanics) and
    // draw the body directly into the caller's region. Default false = the desktop dock
    // window, byte-identical to the pre-slice-4 path.
    void drawAiAssistantPanel(AppController& app, UiDrawSession& d, bool embedded = false);
#endif
    // embedded=true (dual-ui slice 4): mobile Settings page-body fill — skip the show-gate +
    // beginPreferencesWindow/End chrome, draw the tab bar + Save&Sync directly. Default false
    // = desktop dock window, byte-identical to the pre-slice-4 path.
    void drawPreferencesWindow(AppController& app, UiDrawSession& d, bool embedded = false);
    // Section helpers for drawPreferencesWindow (full-function-size-compliance Phase 5,
    // PR E11). The orchestrator owns the positional Begin/EndTabBar/End frame; each helper
    // runs inside that frame and never splits a positional-ImGui pair across the call
    // boundary. No SMATCHET_UI_PERF_SCOPE seams exist on this window, so the split is on
    // logical sections only (no new perf scopes added).
    void resetPreferencesWindowState(UiDrawSession& d);
    bool beginPreferencesWindow(UiDrawSession& d);
    void loadPreferencesBuffers(UiDrawSession& d);
    void drawPreferencesTrackerTab(UiDrawSession& d);
    void drawPreferencesUserInfoTab(UiDrawSession& d);
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
    // embedded=true (dual-ui slice 4): mobile Views page-body fill — skip the show-gate +
    // window chrome (prepareTopLevelWindow/Begin/End/focus), draw the sidebar|editor body
    // directly. Default false = desktop dock window, byte-identical to the pre-slice-4 path.
    void drawViewsDashboardWindow(AppController& app, UiDrawSession& d, bool embedded = false);
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
    /// kickSync=false: adopt the view identity (Activate + buffers + cfg JQL/fields + Save +
    /// nav history) WITHOUT the SyncWithCurrentView network re-fetch — used by the pane
    /// focus-switch path when the pane's own GridLiveContext already synced (Slice 3).
    void viewsActivateView(AppController& app, UiDrawSession& d, const std::string& id, bool kickSync = true);
    /// "Add to view query" from the User Info window: replace the source pane's
    /// view query with a single-key lookup (bound into d.onUserInfoAddToQuery).
    void userInfoAddToQuery(AppController& app, UiDrawSession& d, const std::string& sourcePaneId,
                            const std::string& issueKey);
    /// Global Chrome-omnibox-style search bar (jql-omnibox plan, Stream B). Reserved as a
    /// top viewport side-bar (ImGui::BeginViewportSideBar) and drawn between the chrome and
    /// the docked windows in Draw; drives the FOCUSED grid pane's view. Owns its own
    /// JqlEditorState instance (d.omniJqlEditor) so its in-flight autocomplete request-ids
    /// never collide with the dashboard editor's.
    void drawOmnibar(AppController& app, UiDrawSession& d);
    /// Outcome of applyQueryToPaneView — each caller maps the variants to its own toast copy.
    enum class ApplyQueryResult { Ok, ViewUnavailable, UpdateFailed };
    /// Replaces the view owned by `target` with `query` and re-runs it, adopting that view's
    /// identity first if it isn't the active one. Mirrors the applied query into d.cfg.JqlQuery
    /// and the dashboard JQL editor buffer. Shared core of userInfoAddToQuery + the omnibar.
    ApplyQueryResult applyQueryToPaneView(AppController& app, UiDrawSession& d, GridPane& target,
                                          const std::string& query);
    /// Routes a committed omnibar Enter against `target` per OmnibarInputClassifier (slice 2c,
    /// sync v1): a bare ticket key jumps the pane (SetActiveIssue if the row is already loaded,
    /// else opens the browse URL); a structured query drives applyQueryToPaneView; plain words
    /// fill the pane's grid filter box. No network on the UI thread — the loaded-row check is a
    /// snapshot membership test (an async existence-fetch is a deferred follow-up).
    void applyOmnibarEnter(AppController& app, UiDrawSession& d, GridPane& target, const std::string& raw);
    void viewsRequestActivate(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                              const std::string& id);
    void viewsCreateNewView(UiDrawSession& d, const ViewDefinition* activeView);
    /// Consumes the one-frame deferred view-create latch (UiDrawSession::viewsPendingCreate —
    /// Pillar 3 crash fix: a mid-frame Views::Create reallocates store.Views and dangles every
    /// ViewDefinition* resolved earlier in the frame). Called at the top of Draw, BEFORE any
    /// view pointer is resolved for the frame. Defined in SmatchetViewsDashboardUi.cpp.
    void applyPendingViewCreate(UiDrawSession& d);
    /// Delete twin of applyPendingViewCreate — Views::DeleteActive erases from the same
    /// vector (same invalidation class). Defined in SmatchetViewsDashboardUi.cpp.
    void applyPendingViewDelete(AppController& app, UiDrawSession& d);
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
    // embedded=true (dual-ui slice 4): mobile Tickets page-body fill — skip the pane window
    // chrome (prepareTopLevelWindow/Begin/End/focus + the pane-strip "+" button), draw the
    // grid body directly into the caller's region. Default false = desktop dock window,
    // byte-identical to the pre-slice-4 path. Single-panel mobile passes the focused pane.
    void drawActiveProjectWindow(AppController& app, UiDrawSession& d, GridPane& pane,
                                 const TrackerConnectivityBannerForUi& trackerBanner, bool embedded = false);
    // Pane-view resolution helpers (Slice 2). resolvePaneView falls back to the active
    // view when the pane's view is absent from the loaded bucket — self-repairing
    // pane.viewId ONLY when the pane belongs to the focused backend's bucket (a
    // cross-backend pane's viewId is valid in its own bucket and must survive —
    // review HIGH-1); resolvePaneColumns reuses the shared GridFrameContext for the
    // active view and a per-pane revision-keyed cache otherwise.
    ViewDefinition* resolvePaneView(UiDrawSession& d, GridPane& pane);
    // resolvePaneColumns: builds the pane's column SET. paneOwnResolvedView (Slice 4) is the
    // pane context's OWN view resolved from its backend bucket — used only on the cold-start
    // fallback path so a cross-backend pane with no session capture still renders ITS OWN
    // columns instead of the focused view's (null for the focused pane / when unresolved).
    const std::vector<TicketGridColumn>& resolvePaneColumns(GridPane& pane,
                                                            const TrackerFieldCatalogIndex& catalogIndex,
                                                            const ViewDefinition* paneView,
                                                            const ViewDefinition* paneOwnResolvedView);
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
        /// DR22 — the "Save as new..." button lives inside the ##UnsavedLayoutStrip child
        /// window, so OpenPopup issued there resolves to a different id than the parent-scope
        /// BeginPopupModal and the modal never appeared. This flag defers OpenPopup to the
        /// same scope as BeginPopupModal.
        bool openSaveAsNewModal = false;
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
    // embedded=true (dual-ui slice 4): mobile Log page-body fill — skip the prepareTopLevel/
    // Begin/End chrome, draw the toolbar + scroll region directly. Default false = desktop
    // dock window, byte-identical to the pre-slice-4 path.
    static void drawLogWindow(UiDrawSession& d, bool embedded = false);
    static void drawBulkImportWindow(AppController& app, UiDrawSession& d);
    static void drawBulkExportWindow(AppController& app, UiDrawSession& d);
    static void prepareTopLevelWindow(UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                      bool requestFocus = false);
    static void repairTopLevelWindow(UiDrawSession& d, const char* layoutKey, float minW, float minH);
    void resetWindowLayoutToDefault(UiDrawSession& d);
};
