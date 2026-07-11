// SmatchetUI_MainMenu — `SmatchetUI::drawMainMenuBar`, the 449-LOC top-menu
// composition extracted out of `SmatchetUI.cpp` per
// `docs/plans/shipped/large-files-and-phase-2.md` § A4. Only call site is
// `SmatchetUI::Draw` (root TU).
// The include + macro setup mirrors `SmatchetUI.cpp` byte-for-byte. We do NOT
// include `SmatchetUI_Internal.h` here because its `#define ImGui
// SmatchetLocalizedImGui` after `imgui.h` clashes with `imgui_internal.h`
// consumers. Order matters: `imgui_internal.h` must be pulled BEFORE the
// macro redefinition.

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=dup gate flags this TU's #include list as a clone of SmatchetUI.cpp's — sibling UI TUs sharing a common include set, both in this diff; directive overlap not copy-pasted logic; owner=ui; revisit=when the dup auditor scopes cross-file clones to logic blocks)
// clang-format on
#include "SmatchetUI.h"
#include "AppController.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "KeybindingsConfig.h" // BoundHotkeyDisplay — live shortcut hints on menu items
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

// Live shortcut text for a menu item bound to a registry command. The keybinding registry is
// authoritative (KeybindingsConfig::Defaults() is seeded on every config load — ConfigManager
// load path), so the menu hint tracks user rebinds and clears instead of a stale literal. The
// `fallback` literal is used only when the command has no binding row at all (not possible
// post-seed, kept as a defensive default). A row whose hotkey was cleared shows no hint.
static std::string MenuShortcut(const MainMenuDrawCtx& ctx, const char* commandId, const char* fallback) {
    for (const Keybinding& b : ctx.d.cfg.Keybindings.Bindings) {
        if (b.CommandId == commandId) {
            return BoundHotkeyDisplay(ctx.d.cfg.Keybindings.Bindings, commandId);
        }
    }
    return std::string(fallback);
}

// Args-aware variant of MenuShortcut: one command id may carry several distinct-args
// bindings on distinct keys (e.g. view.toggle.views_dashboard for both "Open Project
// View" and "Views Dashboard"); BoundHotkeyDisplayForArgs picks the row matching
// argsJson by JSON semantic equality. Same fallback contract as MenuShortcut.
static std::string MenuShortcutArgs(const MainMenuDrawCtx& ctx, const char* commandId, const char* argsJson,
                                    const char* fallback) {
    for (const Keybinding& b : ctx.d.cfg.Keybindings.Bindings) {
        if (b.CommandId == commandId) {
            return BoundHotkeyDisplayForArgs(ctx.d.cfg.Keybindings.Bindings, commandId, argsJson);
        }
    }
    return std::string(fallback);
}

// Dispatch a zero-arg registry command from a menu click so the menu and the
// keybinding share one code path (zoom items). This runs on the UI thread, but the
// command's effect is still deferred: RunOnUiThreadAsCommandResult re-posts the body
// to the main-thread dispatcher, so the config mutation + Save land on the next drain
// rather than inline here. Source mirrors the recently-used-views menu dispatch below.
static void DispatchMenuCommand(MainMenuDrawCtx& ctx, const char* commandId) {
    smatchet::cmd::CommandContext cmdCtx;
    cmdCtx.ScenarioHost = &ctx.app;
    cmdCtx.Threading = &ctx.app;
    cmdCtx.Source = smatchet::cmd::CommandSource::Palette;
    const nlohmann::json emptyArgs = nlohmann::json::object();
    const smatchet::cmd::CommandResult r = ctx.app.Commands().Dispatch(commandId, emptyArgs, cmdCtx);
    if (!r.Ok) {
        LOG_DEBUG("SmatchetUI: menu command dispatch failed: %s", commandId);
    }
}

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
                        d.focusedPane().gridState.RectSel.HasAnySelection(),
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
    // Menu actions target the FOCUSED pane (ADR-0018 focused-pane semantics).
    // Shared with the grid-local Ctrl+A handler via GridSelectAllRows.
    GridSelectAllRows(ctx.d.focusedPane(), ctx.tickets);
}

// File menu. Owns its own BeginMenu/EndMenu pair (EndMenu only when BeginMenu returns true).
void SmatchetUI::drawMenuBarFileMenu(MainMenuDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Project View...",
                            MenuShortcutArgs(ctx, "view.toggle.views_dashboard",
                                             "{\"action\":\"show\",\"via\":\"open_project_view\"}", "Ctrl+O")
                                .c_str())) {
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
            GridPane& pane = d.focusedPane();
            CopyGridRectAsTsv(ctx.tickets, pane.filteredIndices, ctx.columns, ctx.catalogIndex, pane.gridState.RectSel);
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
        if (ImGui::MenuItem("Clear Selection", MenuShortcut(ctx, "grid.clear_selection", "Ctrl+Shift+G").c_str(), false,
                            ctx.hasSelection)) {
            d.focusedPane().gridState.RectSel.ClearAll();
        }
        if (ImGui::MenuItem("Copy Selection", "Ctrl+Shift+C", false, ctx.hasSelection && !ctx.columns.empty())) {
            GridPane& pane = d.focusedPane();
            CopyGridRectAsTsv(ctx.tickets, pane.filteredIndices, ctx.columns, ctx.catalogIndex, pane.gridState.RectSel);
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
    if (ImGui::MenuItem("Full Screen", MenuShortcut(ctx, "app.fullscreen.toggle", "F11").c_str(), d.cfg.FullScreen)) {
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
    // Zoom clicks route through the command registry (ui.zoom.*) so the menu and the
    // Ctrl+= / Ctrl+- / Ctrl+0 keybindings share one clamp+Save path. Enable-gates keep
    // the pre-registry min/max bounds (the command clamps too).
    if (ImGui::MenuItem("Zoom In", MenuShortcut(ctx, "ui.zoom.in", "Ctrl+=").c_str(), false,
                        d.cfg.FontSizePt < SmatchetDefaults::kFontSizeMaxPt)) {
        DispatchMenuCommand(ctx, "ui.zoom.in");
    }
    if (ImGui::MenuItem("Zoom Out", MenuShortcut(ctx, "ui.zoom.out", "Ctrl+-").c_str(), false,
                        d.cfg.FontSizePt > SmatchetDefaults::kFontSizeMinPt)) {
        DispatchMenuCommand(ctx, "ui.zoom.out");
    }
    if (ImGui::MenuItem("Reset Zoom", MenuShortcut(ctx, "ui.zoom.reset", "Ctrl+0").c_str(), false,
                        d.cfg.FontSizePt != SmatchetDefaults::kFontSizeDefaultPt)) {
        DispatchMenuCommand(ctx, "ui.zoom.reset");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Primary Side Bar", MenuShortcut(ctx, "view.sidebar.primary", "Ctrl+B").c_str(),
                        d.cfg.ShowPrimarySideBar)) {
        SetViewVisible(d.cfg, ViewSlot::PrimarySideBar, !d.cfg.ShowPrimarySideBar);
        recentViews_.Touch("view.toggle.primary-side-bar");
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Secondary Side Bar", MenuShortcut(ctx, "view.sidebar.secondary", "Ctrl+Alt+B").c_str(),
                        d.cfg.ShowSecondarySideBar)) {
        SetViewVisible(d.cfg, ViewSlot::SecondarySideBar, !d.cfg.ShowSecondarySideBar);
        recentViews_.Touch("view.toggle.secondary-side-bar");
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Status Bar", nullptr, d.cfg.ShowStatusBar)) {
        d.cfg.ShowStatusBar = !d.cfg.ShowStatusBar;
        recentViews_.Touch("view.toggle.status-bar");
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::MenuItem("Panel", MenuShortcut(ctx, "view.panel.bottom", "Ctrl+J").c_str(), d.cfg.ShowPanel)) {
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
    if (ImGui::MenuItem("Command Palette...", MenuShortcut(ctx, "ui.command_palette", "Ctrl+Shift+P").c_str())) {
        commandPalette_.Open();
    }
    if (ImGui::MenuItem("Open View...", MenuShortcut(ctx, "ui.open_view", "Ctrl+Shift+V").c_str())) {
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
                    cmdCtx.ScenarioHost = &app;
                    cmdCtx.Threading = &app;
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
    if (ImGui::MenuItem(
            "Views Dashboard",
            MenuShortcutArgs(ctx, "view.toggle.views_dashboard", "{\"action\":\"show\"}", "Ctrl+Shift+E").c_str(),
            d.showViewsDashboard)) {
        // Menu click always reveals + focuses the window. Closing happens via the
        // window's X button (the p_open arg to ImGui::Begin flips d.showViewsDashboard
        // back to false). See AGENTS.md for the always-reveal contract. Click stays
        // inline (not Dispatch) to preserve the recentViews_.Touch the command lacks.
        d.showViewsDashboard = true;
        d.requestViewsDashboardFocus = true;
        recentViews_.Touch("view.toggle.views-dashboard");
    }
    if (ImGui::MenuItem("Annotate", MenuShortcut(ctx, "view.toggle.source_annotate", "Ctrl+Shift+N").c_str(),
                        d.showAnnotateAnalysis)) {
        d.showAnnotateAnalysis = !d.showAnnotateAnalysis;
        recentViews_.Touch("view.toggle.source-annotate");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Log", MenuShortcut(ctx, "view.toggle.log", "Ctrl+Shift+U").c_str(), d.showLogWindow)) {
        d.showLogWindow = true;
        d.requestLogFocus = true;
        recentViews_.Touch("view.toggle.log");
    }
    if (ImGui::MenuItem("Notifications", MenuShortcut(ctx, "notifications", "").c_str(),
                        d.showNotificationCenterWindow)) {
        d.showNotificationCenterWindow = true;
        d.requestNotificationCenterFocus = true;
        recentViews_.Touch("notifications");
    }
    if (ImGui::MenuItem("Backend Audit", MenuShortcut(ctx, "view.toggle.backend_audit", "Ctrl+Shift+M").c_str(),
                        d.showAuditTrail)) {
        d.showAuditTrail = true;
        d.requestAuditTrailFocus = true;
        recentViews_.Touch("view.toggle.backend-audit");
    }
    if (ImGui::MenuItem("Performance", MenuShortcut(ctx, "view.toggle.performance", "Ctrl+Shift+F").c_str(),
                        d.showPerformance)) {
        d.showPerformance = true;
        d.requestPerformanceFocus = true;
        recentViews_.Touch("view.toggle.performance");
    }
    if (ImGui::MenuItem("Plan docs", MenuShortcut(ctx, "view.toggle.plan_doc_viewer", "Ctrl+Shift+D").c_str(),
                        d.showPlanDocViewer)) {
        d.showPlanDocViewer = true;
        d.requestPlanDocViewerFocus = true;
        recentViews_.Touch("view.toggle.plan_doc_viewer");
    }
    if (ImGui::MenuItem("Bulk Import", MenuShortcut(ctx, "view.toggle.bulk_import", "Ctrl+Shift+I").c_str(),
                        d.showBulkImport)) {
        d.showBulkImport = true;
        d.requestBulkImportFocus = true;
        recentViews_.Touch("view.toggle.bulk-import");
    }
    if (ImGui::MenuItem("Bulk Export", MenuShortcut(ctx, "view.toggle.bulk_export", "Ctrl+Shift+X").c_str(),
                        d.showBulkExport)) {
        d.showBulkExport = true;
        d.requestBulkExportFocus = true;
        recentViews_.Touch("view.toggle.bulk-export");
    }
    if (ImGui::MenuItem("Preferences", MenuShortcut(ctx, "view.toggle.preferences", "Ctrl+,").c_str(),
                        d.showPreferences)) {
        d.showPreferences = true;
        d.requestPreferencesFocus = true;
        recentViews_.Touch("view.toggle.preferences");
    }
#if defined(SMATCHET_WITH_MCP)
    if (ImGui::MenuItem("MCP Server", MenuShortcut(ctx, "view.toggle.mcp_server", "Ctrl+Shift+K").c_str(),
                        d.showMcpServerWindow)) {
        d.showMcpServerWindow = true;
        d.requestMcpServerFocus = true;
        recentViews_.Touch("view.toggle.mcp-server");
    }
#endif
#if defined(SMATCHET_WITH_AI)
    if (ImGui::MenuItem("Assistant", MenuShortcut(ctx, "view.assistant", "Ctrl+Shift+A").c_str(),
                        d.assistantPanelOpen)) {
        d.assistantPanelOpen = true;
        d.requestAssistantFocus = true;
        recentViews_.Touch("view.toggle.assistant");
    }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    if (ImGui::MenuItem("Scripts & Actions", MenuShortcut(ctx, "view.toggle.scripts", "Ctrl+Shift+L").c_str(),
                        d.showLuaAutomationWindow)) {
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
        if (ImGui::MenuItem("Preferences...", MenuShortcut(ctx, "view.toggle.preferences", "Ctrl+,").c_str())) {
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
        if (ImGui::MenuItem("Report a Bug...",
                            MenuShortcut(ctx, "app.bug_report.open", d.cfg.BugReportHotkey.c_str()).c_str())) {
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
