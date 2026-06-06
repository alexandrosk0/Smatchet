// SmatchetUI_MainMenu — `SmatchetUI::drawMainMenuBar`, the 449-LOC top-menu
// composition extracted out of `SmatchetUI.cpp` per
// `docs/plans/shipped/large-files-and-phase-2.md` § A4. Only call site is
// `SmatchetUI::Draw` (root TU).
// The include + macro setup mirrors `SmatchetUI.cpp` byte-for-byte. We do NOT
// include `SmatchetUI_Internal.h` here because its `#define ImGui
// SmatchetLocalizedImGui` after `imgui.h` clashes with `imgui_internal.h`
// consumers. Order matters: `imgui_internal.h` must be pulled BEFORE the
// macro redefinition.

#include "SmatchetUI.h"
#include "AppController.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "NavigationHistory.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewVisibility.h"
#include "TicketGridModel.h"
#include "TrackerGridFieldDisplay.h"
#if defined(SMATCHET_WITH_WHISPER)
#include "DictationInsertionRouter.h"
#include "SmatchetLocalization.h"
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Helpers shared with `SmatchetUI.cpp` / `SmatchetUI_Layout.cpp`. Declared here
// inline (rather than via `SmatchetUI_Internal.h`) to keep the macro-order
// safety described in the file banner.
namespace smatchet {
namespace ui_detail {
void StartAppUpdateCheck(UiDrawSession& d, AppController& app, bool manual);
} // namespace ui_detail
} // namespace smatchet

// Per-frame state shared by the drawMainMenuBar section helpers. Captured once at the top of
// the menu-bar frame and passed by reference into each per-menu helper, so no helper re-fetches
// the snapshot or recomputes the derived selection flags. Mirrors the ActiveProjectDrawCtx
// pattern; declared at file scope (forward-declared in SmatchetUI.h).
struct MainMenuDrawCtx {
    AppController& app;
    UiDrawSession& d;
    const std::vector<CachedTicket>& tickets;
    const TrackerFieldCatalogIndex& catalogIndex;
    const std::vector<TicketGridColumn>& columns;
    bool hasSelection;
    bool hasTickets;
    bool trackerLocked;
};

void SmatchetUI::drawMainMenuBar(AppController& app, UiDrawSession& d) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    // NC 2.01 menu bar: white text on gray strip. Other themes inherit ImGuiCol_Text.
    const bool nortonMenuTint = (d.cfg.Theme == ThemeId::NortonCommander);
    if (nortonMenuTint) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
    auto ticketsSnap = app.GetActiveTicketsSnapshot();
    const std::vector<CachedTicket> emptyTickets;
    const std::vector<CachedTicket>& tickets = ticketsSnap ? *ticketsSnap : emptyTickets;

    // First-launch tracker gate: lock everything except Tools -> Preferences (which
    // hosts the Tracker tab) until the backend probe confirms a working connection.
    const bool trackerLocked = !d.cfg.BackendHasBeenReachable;
    if (trackerLocked) {
        d.showPreferences = true;
    }

    MainMenuDrawCtx ctx{app,
                        d,
                        tickets,
                        *gridFrameCtx_.catalogIndex,
                        gridFrameCtx_.columns,
                        d.gridState.RectSel.HasAnySelection(),
                        !tickets.empty(),
                        trackerLocked};

    if (trackerLocked)
        ImGui::BeginDisabled();
    drawMenuBarFileMenu(ctx);
    drawMenuBarEditMenu(ctx);
    drawMenuBarSelectionMenu(ctx);
    drawMenuBarViewMenu(ctx);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    drawMenuBarRunMenu(ctx);
#endif
    if (trackerLocked)
        ImGui::EndDisabled();
    // Tools menu stays enabled when locked — but only the Preferences entry inside.
    drawMenuBarToolsMenu(ctx);
    if (trackerLocked)
        ImGui::BeginDisabled();
    drawMenuBarHelpMenu(ctx);
    if (trackerLocked)
        ImGui::EndDisabled();
#if !defined(SMATCHET_WITH_LUA_AUTOMATION)
    {
        static bool s_loggedLuaMenuAbsent = false;
        if (!s_loggedLuaMenuAbsent) {
            s_loggedLuaMenuAbsent = true;
            LOG_WARN("SmatchetUI: Lua automation disabled in this binary (no Scripts & Actions window).");
        }
    }
#endif

    // Inline Command Palette input — VS Code Quick Input.
    // Typing pre-fills + opens the existing palette modal; Enter does the same.
    // cppcheck-suppress duplicateCondition
    if (trackerLocked) {
        ImGui::BeginDisabled();
    }
    drawMenuBarInlinePalette(ctx);
    if (trackerLocked)
        ImGui::EndDisabled();

#if defined(SMATCHET_WITH_WHISPER)
    drawMenuBarDictationIndicator(ctx);
#endif
#ifdef SMATCHET_EMBEDDED_IN_UNREAL
    drawMenuBarUnrealCloseButton(ctx);
#endif
    if (nortonMenuTint) {
        ImGui::PopStyleColor();
    }
    ImGui::EndMainMenuBar();
}

#if defined(SMATCHET_WITH_WHISPER)
// Phase E — push-to-talk REC indicator in the menu bar, just before
// the (Unreal) Close button. Polls the lock-free recording flag on
// the router; cheap (one atomic load per frame). Per Pillar 2 the
// indicator must appear < 100 ms after hotkey press; the worker
// flips the atomic immediately on onPress, and the next UI frame
// picks it up.
// REC indicator stays red while audio is being captured; switches to
// an amber "Transcribing" indicator once the user releases the hotkey
// and the worker is running. Closes the visual gap between mic stop
// and text insertion for the multi-second local-model path.
void SmatchetUI::drawMenuBarDictationIndicator(MainMenuDrawCtx& ctx) {
    (void)ctx;
    const bool isRec = g_dictationRouter.IsRecording();
    const bool isTx = !isRec && g_dictationRouter.IsTranscribing();
    if (isRec || isTx) {
        const char* label =
            isRec ? SmatchetLocalization::T("whisper.statusBar.recording", "\xE2\x97\x8F REC")
                  : SmatchetLocalization::T("whisper.statusBar.transcribing", "\xE2\x97\x90 Transcribing...");
        const float labelW = ImGui::CalcTextSize(label).x;
        constexpr float kRightMarginRec = 12.0f;
#ifdef SMATCHET_EMBEDDED_IN_UNREAL
        constexpr float kReservedForCloseButton = 80.0f;
#else
        constexpr float kReservedForCloseButton = 0.0f;
#endif
        const float xPosRec = (std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - labelW -
                                                                     kRightMarginRec - kReservedForCloseButton);
        ImGui::SetCursorPosX(xPosRec);
        const ImVec4 col = isRec ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)   // red
                                 : ImVec4(0.95f, 0.78f, 0.20f, 1.0f); // amber
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            const char* tip = isRec ? SmatchetLocalization::T("whisper.statusBar.recordingTooltip",
                                                              "Recording for dictation — release hotkey to "
                                                              "transcribe; press Esc to cancel")
                                    : SmatchetLocalization::T("whisper.statusBar.transcribingTooltip",
                                                              "Transcribing captured audio — text will appear in the "
                                                              "last-focused input field when done");
            ImGui::SetTooltip("%s", tip);
        }
    }
}
#endif

#ifdef SMATCHET_EMBEDDED_IN_UNREAL
// Embedded-overlay Close button, right-aligned in the menu bar. Hides the Smatchet
// overlay (same effect as Ctrl+Shift+J). Only present in the Unreal-embedded build.
void SmatchetUI::drawMenuBarUnrealCloseButton(MainMenuDrawCtx& ctx) {
    AppController& app = ctx.app;
    const char* closeLabel = "Close";
    const float btnW = ImGui::CalcTextSize(closeLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    constexpr float kRightMargin = 10.0f;
    const float xPos = (std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - btnW - kRightMargin);
    ImGui::SetCursorPosX(xPos);
    if (ImGui::SmallButton(closeLabel)) {
        app.CloseEmbeddedUi();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hide Smatchet overlay (same as Ctrl+Shift+J)");
    }
}
#endif

// Rect-select "Select All" over the active filtered/unfiltered row set. Was the
// `selectAllRows` lambda inside the menu-bar body.
void SmatchetUI::selectAllGridRows(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    const std::vector<CachedTicket>& tickets = ctx.tickets;
    auto& sel = d.gridState.RectSel;
    sel.ClearAll();
    const size_t rowCount = !d.filteredIndices.empty() ? d.filteredIndices.size() : tickets.size();
    for (size_t row = 0; row < rowCount; ++row) {
        sel.Rows.insert(static_cast<int>(row));
    }
    if (rowCount > 0) {
        sel.PrimaryRow = 0;
        sel.SortSignature =
            ComputeGridSortSignature(d.cachedSortFingerprint, d.cachedSortTicketsRevision, tickets.size());
        const size_t firstTicketIndex = !d.filteredIndices.empty() ? d.filteredIndices.front() : 0;
        if (firstTicketIndex < tickets.size()) {
            d.gridState.ActiveIssueId = tickets[firstTicketIndex].id;
        }
    }
}

// File menu. Owns its own BeginMenu/EndMenu pair (EndMenu only when BeginMenu returns true).
void SmatchetUI::drawMenuBarFileMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Project View...", "Ctrl+O")) {
            d.showViewsDashboard = true;
            d.requestViewsDashboardFocus = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import Issues...")) {
            d.showBulkImport = true;
        }
        if (ImGui::MenuItem("Export Issues...")) {
            d.showBulkExport = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Read-only Mode", nullptr, d.cfg.ReadOnlyMode, true)) {
            d.cfg.ReadOnlyMode = !d.cfg.ReadOnlyMode;
            ConfigManager::Save(d.cfg);
        }
        ImGui::EndMenu();
    }
}

// Edit menu with the Copy item. Owns its own begin-menu and end-menu calls.
void SmatchetUI::drawMenuBarEditMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, ctx.hasSelection && !ctx.columns.empty())) {
            CopyGridRectAsTsv(ctx.tickets, d.filteredIndices, ctx.columns, ctx.catalogIndex, d.gridState.RectSel);
        }
        ImGui::EndMenu();
    }
}

// Selection menu. Owns its own BeginMenu/EndMenu pair.
void SmatchetUI::drawMenuBarSelectionMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Selection")) {
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, ctx.hasTickets)) {
            selectAllGridRows(ctx);
        }
        if (ImGui::MenuItem("Clear Selection", "Ctrl+Shift+A", false, ctx.hasSelection)) {
            d.gridState.RectSel.ClearAll();
        }
        if (ImGui::MenuItem("Copy Selection", "Ctrl+Shift+C", false, ctx.hasSelection && !ctx.columns.empty())) {
            CopyGridRectAsTsv(ctx.tickets, d.filteredIndices, ctx.columns, ctx.catalogIndex, d.gridState.RectSel);
        }
        ImGui::EndMenu();
    }
}

// Theme / Density / Font nested submenus inside Appearance. Each opens and closes its own
// BeginMenu/EndMenu pair; runs inside the active Appearance scope (no scope of its own).
void SmatchetUI::drawAppearanceThemeDensityFont(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Theme")) {
        struct ThemeEntry {
            ThemeId id;
            const char* label;
        };
        constexpr ThemeEntry kEntries[] = {
            {ThemeId::ImGuiDefaultDark, "ImGui Default Dark (bright)"},
            {ThemeId::SmatchetDark, "Smatchet Dark"},
            {ThemeId::ModernDark, "Modern Dark"},
            {ThemeId::Vs2022Dark, "VS 2022 Dark"},
            {ThemeId::Vs2022Light, "VS 2022 Light"},
            {ThemeId::HighContrast, "High Contrast"},
            {ThemeId::NortonCommander, "Norton Commander"},
        };
        for (const ThemeEntry& e : kEntries) {
            if (ImGui::MenuItem(e.label, nullptr, d.cfg.Theme == e.id)) {
                d.cfg.Theme = e.id;
                ConfigManager::Save(d.cfg);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Density")) {
        struct DensityEntry {
            TrackerConfig::UiDensity id;
            const char* label;
        };
        const DensityEntry kDensities[] = {
            {TrackerConfig::UiDensity::Compact, "Compact"},
            {TrackerConfig::UiDensity::Normal, "Normal"},
            {TrackerConfig::UiDensity::Comfortable, "Comfortable"},
        };
        for (const DensityEntry& e : kDensities) {
            if (ImGui::MenuItem(e.label, nullptr, d.cfg.Density == e.id)) {
                d.cfg.Density = e.id;
                ConfigManager::Save(d.cfg);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Font")) {
        const char* kFonts[] = {
            "Segoe UI", "Consolas", "Calibri", "Arial", "Cascadia Code", "JetBrains Mono",
        };
        const int kFontCount = static_cast<int>(sizeof(kFonts) / sizeof(kFonts[0]));
        for (int fi = 0; fi < kFontCount; ++fi) {
            const bool selected = (d.cfg.SelectedFontName == kFonts[fi]);
            if (ImGui::MenuItem(kFonts[fi], nullptr, selected)) {
                d.cfg.SelectedFontName = kFonts[fi];
                ConfigManager::Save(d.cfg);
                SmatchetRequestFontReload(d.cfg.SelectedFontName, static_cast<float>(d.cfg.FontSizePt));
            }
        }
        ImGui::EndMenu();
    }
}

// Panel Position nested submenu inside Appearance. Owns its own BeginMenu/EndMenu pair; runs
// inside the active Appearance scope.
void SmatchetUI::drawAppearancePanelPosition(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Panel Position")) {
        if (ImGui::MenuItem("Bottom", nullptr, d.cfg.PanelDockSide == TrackerConfig::PanelPosition::Bottom)) {
            if (d.cfg.PanelDockSide != TrackerConfig::PanelPosition::Bottom) {
                d.cfg.PanelDockSide = TrackerConfig::PanelPosition::Bottom;
                ConfigManager::Save(d.cfg);
                // Dock rebuild is complex and fragile; reset layout instead.
                // The panel will be repositioned after layout reset on next launch.
                resetWindowLayoutToDefault(d);
            }
        }
        if (ImGui::MenuItem("Right", nullptr, d.cfg.PanelDockSide == TrackerConfig::PanelPosition::Right)) {
            if (d.cfg.PanelDockSide != TrackerConfig::PanelPosition::Right) {
                d.cfg.PanelDockSide = TrackerConfig::PanelPosition::Right;
                ConfigManager::Save(d.cfg);
                resetWindowLayoutToDefault(d);
            }
        }
        ImGui::EndMenu();
    }
}

// View > Appearance submenu (themes, density, font, panel position, zoom, panel toggles).
// Owns its own BeginMenu/EndMenu pair; nested theme/density/font/panel submenus own theirs.
void SmatchetUI::drawMenuBarAppearanceMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (!ImGui::BeginMenu("Appearance")) {
        return;
    }
#ifndef SMATCHET_EMBEDDED_IN_UNREAL
    if (ImGui::MenuItem("Full Screen", "F11", d.cfg.FullScreen)) {
        d.requestFullScreenToggle = true;
    }
#endif
    if (ImGui::MenuItem("Zen Mode", "Ctrl+M, Z", d.cfg.ZenMode)) {
        d.cfg.ZenMode = !d.cfg.ZenMode;
    }
    ImGui::Separator();
    drawAppearanceThemeDensityFont(ctx);
    ImGui::Separator();
    drawAppearancePanelPosition(ctx);
    {
        const char* swapLabel =
            d.cfg.PrimarySideBarOnRight ? "Move Primary Side Bar Left" : "Move Primary Side Bar Right";
        if (ImGui::MenuItem(swapLabel)) {
            d.cfg.PrimarySideBarOnRight = !d.cfg.PrimarySideBarOnRight;
            ConfigManager::Save(d.cfg);
            resetWindowLayoutToDefault(d);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Zoom In", "Ctrl+=", false, d.cfg.FontSizePt < 32)) {
        d.cfg.FontSizePt = (d.cfg.FontSizePt < 32) ? (d.cfg.FontSizePt + 1) : 32;
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Zoom Out", "Ctrl+-", false, d.cfg.FontSizePt > 8)) {
        d.cfg.FontSizePt = (d.cfg.FontSizePt > 8) ? (d.cfg.FontSizePt - 1) : 8;
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Reset Zoom", "Ctrl+0", false, d.cfg.FontSizePt != 16)) {
        d.cfg.FontSizePt = 16;
        ConfigManager::Save(d.cfg);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Primary Side Bar", "Ctrl+B", d.cfg.ShowPrimarySideBar)) {
        SetViewVisible(d.cfg, ViewSlot::PrimarySideBar, !d.cfg.ShowPrimarySideBar);
        recentViews_.Touch("view.toggle.primary-side-bar");
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Secondary Side Bar", "Ctrl+Alt+B", d.cfg.ShowSecondarySideBar)) {
        SetViewVisible(d.cfg, ViewSlot::SecondarySideBar, !d.cfg.ShowSecondarySideBar);
        recentViews_.Touch("view.toggle.secondary-side-bar");
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Status Bar", nullptr, d.cfg.ShowStatusBar)) {
        d.cfg.ShowStatusBar = !d.cfg.ShowStatusBar;
        recentViews_.Touch("view.toggle.status-bar");
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Panel", "Ctrl+J", d.cfg.ShowPanel)) {
        SetViewVisible(d.cfg, ViewSlot::BottomPanel, !d.cfg.ShowPanel);
        recentViews_.Touch("view.toggle.panel");
        ConfigManager::Save(d.cfg);
    }
    ImGui::EndMenu();
}

// View menu — palette, toolbar, the Appearance submenu, window toggles, Recently Used
// Views, and Reset Layout. Owns its own begin-menu and end-menu calls; delegates the
// Appearance submenu and the window-toggle block to sub-helpers.
void SmatchetUI::drawMenuBarViewMenu(MainMenuDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    if (!ImGui::BeginMenu("View")) {
        return;
    }
    if (ImGui::MenuItem("Command Palette...", "Ctrl+Shift+P")) {
        commandPalette_.Open();
    }
    if (ImGui::MenuItem("Open View...", "Ctrl+Shift+V")) {
        commandPalette_.Open();
        commandPalette_.SetFilterText("view.toggle.");
    }
    if (ImGui::MenuItem("Show Toolbar", nullptr, d.cfg.Toolbar.Visible)) {
        d.cfg.Toolbar.Visible = !d.cfg.Toolbar.Visible;
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Customize Toolbar...")) {
        toolbar_.OpenEditor();
    }
    ImGui::Separator();
    drawMenuBarAppearanceMenu(ctx);
#if defined(SMATCHET_ENABLE_EDITOR_LAYOUT)
    if (ImGui::BeginMenu("Editor Layout")) {
        if (ImGui::MenuItem("Single")) { /* TODO: DockBuilderSplitNode single */
        }
        if (ImGui::MenuItem("Two Columns")) { /* TODO */
        }
        if (ImGui::MenuItem("Three Columns")) { /* TODO */
        }
        if (ImGui::MenuItem("Two Rows")) { /* TODO */
        }
        if (ImGui::MenuItem("Grid (2x2)")) { /* TODO */
        }
        ImGui::EndMenu();
    }
#endif
    drawMenuBarViewWindowToggles(ctx);
    ImGui::Separator();
    // Recently Used Views submenu: lists the last 5 toggled view ids, oldest first.
    if (ImGui::BeginMenu("Recently Used Views")) {
        const std::vector<std::string> recent = recentViews_.Snapshot();
        if (recent.empty()) {
            ImGui::BeginDisabled();
            ImGui::MenuItem("(none yet)");
            ImGui::EndDisabled();
        } else {
            for (int ri = static_cast<int>(recent.size()) - 1; ri >= 0; --ri) {
                const std::string& cmdId = recent[static_cast<size_t>(ri)];
                if (ImGui::MenuItem(cmdId.c_str())) {
                    smatchet::cmd::CommandContext cmdCtx;
                    cmdCtx.App = &app;
                    cmdCtx.Source = smatchet::cmd::CommandSource::Palette;
                    const nlohmann::json emptyArgs = nlohmann::json::object();
                    smatchet::cmd::CommandResult r = app.Commands().Dispatch(cmdId, emptyArgs, cmdCtx);
                    if (!r.Ok) {
                        LOG_DEBUG("SmatchetUI: recently used view command not found: %s", cmdId.c_str());
                    }
                }
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Layout")) {
        resetWindowLayoutToDefault(d);
    }
    ImGui::EndMenu();
}

// Window-visibility toggle MenuItems inside the View menu (Views Dashboard through the
// optional Scripts & Actions entry). Split out of drawMenuBarViewMenu to keep both under the
// branch cap; runs inside the active View BeginMenu scope (no Begin/End pair of its own).
void SmatchetUI::drawMenuBarViewWindowToggles(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    ImGui::Separator();
    if (ImGui::MenuItem("Views Dashboard", "Ctrl+Shift+E", d.showViewsDashboard)) {
        // Menu click always reveals + focuses the window. Closing happens via the
        // window's X button (the p_open arg to ImGui::Begin flips d.showViewsDashboard
        // back to false). See AGENTS.md for the always-reveal contract.
        d.showViewsDashboard = true;
        d.requestViewsDashboardFocus = true;
        recentViews_.Touch("view.toggle.views-dashboard");
    }
    if (ImGui::MenuItem("Annotate", "Ctrl+Shift+B", d.showAnnotateAnalysis)) {
        d.showAnnotateAnalysis = !d.showAnnotateAnalysis;
        recentViews_.Touch("view.toggle.source-annotate");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Log", "Ctrl+Shift+U", d.showLogWindow)) {
        d.showLogWindow = true;
        d.requestLogFocus = true;
        recentViews_.Touch("view.toggle.log");
    }
    if (ImGui::MenuItem("Backend Audit", "Ctrl+Shift+M", d.showAuditTrail)) {
        d.showAuditTrail = true;
        d.requestAuditTrailFocus = true;
        recentViews_.Touch("view.toggle.backend-audit");
    }
    if (ImGui::MenuItem("Performance", "Ctrl+Shift+F", d.showPerformance)) {
        d.showPerformance = true;
        d.requestPerformanceFocus = true;
        recentViews_.Touch("view.toggle.performance");
    }
    if (ImGui::MenuItem("Plan docs", "Ctrl+Shift+D", d.showPlanDocViewer)) {
        d.showPlanDocViewer = true;
        d.requestPlanDocViewerFocus = true;
        recentViews_.Touch("view.toggle.plan_doc_viewer");
    }
    if (ImGui::MenuItem("Bulk Import", "Ctrl+Shift+I", d.showBulkImport)) {
        d.showBulkImport = true;
        d.requestBulkImportFocus = true;
        recentViews_.Touch("view.toggle.bulk-import");
    }
    if (ImGui::MenuItem("Bulk Export", "Ctrl+Shift+X", d.showBulkExport)) {
        d.showBulkExport = true;
        d.requestBulkExportFocus = true;
        recentViews_.Touch("view.toggle.bulk-export");
    }
    if (ImGui::MenuItem("Preferences", "Ctrl+,", d.showPreferences)) {
        d.showPreferences = true;
        d.requestPreferencesFocus = true;
        recentViews_.Touch("view.toggle.preferences");
    }
#if defined(SMATCHET_WITH_MCP)
    if (ImGui::MenuItem("MCP Server", "Ctrl+Shift+K", d.showMcpServerWindow)) {
        d.showMcpServerWindow = true;
        d.requestMcpServerFocus = true;
        recentViews_.Touch("view.toggle.mcp-server");
    }
#endif
#if defined(SMATCHET_WITH_AI)
    if (ImGui::MenuItem("Assistant", "Ctrl+Shift+A", d.assistantPanelOpen)) {
        d.assistantPanelOpen = true;
        d.requestAssistantFocus = true;
        recentViews_.Touch("view.toggle.assistant");
    }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    if (ImGui::MenuItem("Scripts & Actions", "Ctrl+Shift+L", d.showLuaAutomationWindow)) {
        d.showLuaAutomationWindow = true;
        d.requestLuaAutomationFocus = true;
        d.requestScriptingEditorTabFocus = true;
        recentViews_.Touch("view.toggle.scripts-and-actions");
    }
#endif
}

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
// Run menu for the Lua automation entry. Owns its own begin-menu and end-menu calls.
void SmatchetUI::drawMenuBarRunMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Run")) {
        if (ImGui::MenuItem("Scripts & Actions...")) {
            d.showLuaAutomationWindow = true;
            d.requestLuaAutomationFocus = true;
            d.requestScriptingEditorTabFocus = true;
        }
        ImGui::EndMenu();
    }
}
#endif

// Tools menu — stays enabled when the tracker gate is locked, but the MCP entry inside is
// disabled. Owns its own BeginMenu/EndMenu pair (and the inner BeginDisabled/EndDisabled).
void SmatchetUI::drawMenuBarToolsMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Preferences...", "Ctrl+,")) {
            d.showPreferences = true;
            d.requestPreferencesFocus = true;
        }
#if defined(SMATCHET_WITH_MCP)
        if (ctx.trackerLocked)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem("MCP Server...")) {
            d.showMcpServerWindow = true;
            d.requestMcpServerFocus = true;
        }
        if (ctx.trackerLocked)
            ImGui::EndDisabled();
#endif
        ImGui::EndMenu();
    }
}

// Help menu with the Check-for-Updates and Report-a-Bug items. Owns its own begin-menu
// and end-menu calls.
void SmatchetUI::drawMenuBarHelpMenu(MainMenuDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Check for Updates...", nullptr, false, !d.appUpdateCheckInFlight)) {
            smatchet::ui_detail::StartAppUpdateCheck(d, app, true);
        }
        if (ImGui::MenuItem("Report a Bug...", d.cfg.BugReportHotkey.c_str())) {
            d.showBugReport = true;
            d.bugReportOpenLatch = true;
            // Opener owns the screenshot toggle — seed from config (the modal no longer does).
            d.bugReportInclScreenshot = d.cfg.BugReportScreenshotDefault;
        }
        ImGui::EndMenu();
    }
}

// Inline command-palette InputText anchored to the right of the menu bar. Owns its own
// PushStyleVar/PopStyleVar pair.
void SmatchetUI::drawMenuBarInlinePalette(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    constexpr float kInlineMaxWidthPx = 640.0f;
    constexpr float kInlineMinWidthPx = 200.0f;
    constexpr float kRightReservedPx = 140.0f;
    const float menuRightEdge = ImGui::GetCursorPosX();
    const float rightLimit = ImGui::GetWindowContentRegionMax().x - kRightReservedPx;
    const float availW = (std::max)(0.0f, rightLimit - menuRightEdge);

    if (availW >= kInlineMinWidthPx) {
        const float inputW = (std::min)(kInlineMaxWidthPx, availW * 0.55f);
        const float xPad = (availW - inputW) * 0.5f;
        ImGui::SetCursorPosX(menuRightEdge + xPad);
        ImGui::SetNextItemWidth(inputW);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
        const bool committed =
            ImGui::InputTextWithHint("##cmd-palette-input", "Search commands (Ctrl+Shift+P)", d.paletteInlineBuf,
                                     IM_ARRAYSIZE(d.paletteInlineBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar();
        const bool edited = ImGui::IsItemEdited();
        const bool activated = ImGui::IsItemActivated();
        if ((activated || edited || committed) && d.paletteInlineBuf[0] != '\0') {
            if (!commandPalette_.IsOpen()) {
                commandPalette_.Open();
            }
            commandPalette_.SetFilterText(d.paletteInlineBuf);
        }
        if (!commandPalette_.IsOpen() && !ImGui::IsItemActive() && d.paletteInlineBuf[0] != '\0') {
            d.paletteInlineBuf[0] = '\0';
        }
    }
}
