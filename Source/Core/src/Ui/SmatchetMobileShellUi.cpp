// Mobile UI shell (dual-ui-mode-desktop-mobile plan, slice 3).
// Central architectural bet (see the plan): the host layer never learns about mobile
// mode. Every host keeps creating its empty viewport dockspace; the mobile shell is a
// single fullscreen NoDocking / NoTitleBar / NoSavedSettings window drawn on top, which
// fully occludes that dockspace. The shell is three fixed vertical bands — top app bar,
// flex page-content, bottom nav — plus an overlay drawer. Slice 3 ships the bare shell
// (app bar + bottom nav + empty page); page bodies reuse the desktop draw helpers from
// slice 4 onward. drawResolveUiMode + the Auto-mode width-hysteresis consts also live
// here so the whole mobile/Auto surface stays in one TU.
#include "SmatchetUI.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "GridPane.h"
#include "SmatchetUiSession.h"
#include "SmatchetUiModeIds.h"
#include "Ui/SmatchetGridPaneWindows.h"
#include "Ui/SmatchetFieldRender.h"
#include "Logger.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace
// (matches SmatchetUI.cpp). Positional-layout primitives use the ::ImGui:: real API.
#define ImGui SmatchetLocalizedImGui

#include <ghc/filesystem.hpp>

#include <string>
#include <vector>

namespace {

// Auto-mode width hysteresis (relocated from SmatchetUI.cpp in slice 3). Enter Mobile at
// or below the max-enter width; exit to Desktop at or above the min-exit width; hold the
// previous decision inside the dead band so a near-breakpoint resize never flaps.
constexpr float kMobileEnterMaxWidthPx = 720.0f;
constexpr float kMobileExitMinWidthPx = 860.0f;

// Base band heights (device-independent px), scaled by the touch-density factor so a
// Comfortable shell gets larger hit targets than a Compact one.
constexpr float kAppBarBaseHeightPx = 40.0f;
constexpr float kBottomNavBaseHeightPx = 48.0f;

// Human label for a bottom-nav page id. Slice 3 placeholders; localization keys land with
// the real page bodies (slice 4+).
const char* navPageLabel(const std::string& id) {
    if (id == "grid") {
        return "Tickets";
    }
    if (id == "views") {
        return "Views";
    }
    if (id == "log") {
        return "Log";
    }
    if (id == "settings") {
        return "Settings";
    }
    if (id == "ai") {
        return "AI";
    }
    return id.c_str();
}

} // namespace

// Resolves cfg.UiMode -> d.effectiveUiMode once per frame. Manual Desktop/Mobile pin
// directly; Auto applies width hysteresis on io.DisplaySize.x: enter Mobile at <= 720 px,
// exit to Desktop at >= 860 px, and hold the previous frame's decision inside the dead
// band so a window hovering near the breakpoint never flaps. d.effectiveUiMode is both
// the output AND the cross-frame carry — it is seeded to Desktop in UiDrawSession.
void SmatchetUI::drawResolveUiMode(UiDrawSession& d) {
    switch (d.cfg.UiMode) {
    case UiMode::Desktop:
        d.effectiveUiMode = EffectiveUiMode::Desktop;
        return;
    case UiMode::Mobile:
        d.effectiveUiMode = EffectiveUiMode::Mobile;
        return;
    case UiMode::Auto:
    default:
        break;
    }

    const float width = ::ImGui::GetIO().DisplaySize.x;
    if (width <= kMobileEnterMaxWidthPx) {
        d.effectiveUiMode = EffectiveUiMode::Mobile;
    } else if (width >= kMobileExitMinWidthPx) {
        d.effectiveUiMode = EffectiveUiMode::Desktop;
    }
    // else: dead band (720 < w < 860) — hold d.effectiveUiMode from the prior frame.
}

// Fullscreen mobile shell: occludes the host viewport dockspace with one borderless
// window holding the three fixed bands + the overlay drawer. Owns its own Begin/End and
// all style-var push/pop pairs.
void SmatchetUI::drawMobileShell(AppController& app, UiDrawSession& d) {
    // One-time seed of the active page from the persisted home page.
    if (!d.mobilePageSeeded) {
        d.mobilePage = mobilePageFromString(d.cfg.MobileHomePage);
        d.mobilePageSeeded = true;
    }

    // Desktop->Mobile edge (slice 5): detach io.IniFilename + load imgui_mobile.ini so the
    // mobile content-dock layout persists separately and the desktop imgui.ini is untouched.
    drawMobileEnsureIniAttached(d);

    const float scale = mobileTouchDensityScale(d.cfg.MobileTouchDensity);
    const float appBarH = kAppBarBaseHeightPx * scale;
    const float navH = kBottomNavBaseHeightPx * scale;

    const ::ImGuiViewport* vp = ::ImGui::GetMainViewport();
    ::ImGui::SetNextWindowPos(vp->WorkPos);
    ::ImGui::SetNextWindowSize(vp->WorkSize);
    ::ImGui::SetNextWindowViewport(vp->ID);

    const ImGuiWindowFlags kShellFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ::ImVec2(0.0f, 0.0f));
    const bool open = ::ImGui::Begin("##MobileShell", nullptr, kShellFlags);
    ::ImGui::PopStyleVar(3);

    // The Grid page hosts a local DockSpace (list / detail split); other pages stay a single
    // fill child. The dockspace id is captured here and its windows submitted AFTER the shell
    // window closes (docked windows are top-level — the imgui_demo dockspace-host pattern).
    const bool gridPage = (d.mobilePage == MobilePage::Grid);
    unsigned int gridDockId = 0;
    if (open) {
        const float totalH = ::ImGui::GetContentRegionAvail().y;
        const float contentH = totalH - appBarH - navH;

        if (::ImGui::BeginChild("##MobileAppBar", ::ImVec2(0.0f, appBarH), false, ImGuiWindowFlags_NoScrollbar)) {
            drawMobileTopAppBar(app, d);
        }
        ::ImGui::EndChild();

        const float pageH = contentH > 0.0f ? contentH : 0.0f;
        if (gridPage) {
            gridDockId = ::ImGui::GetID("MobileContentDock");
            ::ImGui::DockSpace(gridDockId, ::ImVec2(0.0f, pageH), ImGuiDockNodeFlags_None);
        } else {
            if (::ImGui::BeginChild("##MobilePage", ::ImVec2(0.0f, pageH), false)) {
                drawMobilePageContent(app, d);
            }
            ::ImGui::EndChild();
        }

        if (::ImGui::BeginChild("##MobileNav", ::ImVec2(0.0f, navH), false, ImGuiWindowFlags_NoScrollbar)) {
            drawMobileBottomNav(app, d);
        }
        ::ImGui::EndChild();
    }
    ::ImGui::End();

    // Grid dock windows are top-level (submitted after the shell window) so they dock into
    // MobileContentDock rather than nesting inside the shell.
    if (gridPage && gridDockId != 0) {
        drawMobileGridDockWindows(app, d, gridDockId);
    }

    // Drawer is an overlay drawn after (above) the shell window.
    drawMobileDrawer(app, d);

    // Persist mobile dock geometry: with io.IniFilename detached, ImGui raises
    // WantSaveIniSettings on a dirty layout instead of auto-writing imgui.ini — route it to
    // imgui_mobile.ini. Local sub-ms write, mirrors the desktop imgui.ini save path (Pillar 2).
    ::ImGuiIO& io = ::ImGui::GetIO();
    if (io.WantSaveIniSettings) {
        ::ImGui::SaveIniSettingsToDisk(ConfigManager::GetMobileImGuiSettingsPath().c_str());
        io.WantSaveIniSettings = false;
    }
}

// Top app bar: hamburger toggles the drawer; the active page name is the title.
void SmatchetUI::drawMobileTopAppBar(AppController& app, UiDrawSession& d) {
    (void)app;
    ::ImGui::AlignTextToFramePadding();
    if (::ImGui::Button("\xe2\x98\xb0", ::ImVec2(0.0f, 0.0f))) { // U+2630 trigram (hamburger)
        d.mobileDrawerOpen = !d.mobileDrawerOpen;
    }
    ::ImGui::SameLine();
    ::ImGui::Text("Smatchet \xe2\x80\x94 %s", navPageLabel(mobilePageToString(d.mobilePage)));
}

// Page content (slice 4): single-panel fill. Each page draws one desktop helper with
// embedded=true, which suppresses that helper's dock-window chrome (Begin/End/focus/
// open-gate/persist) so its body fills the mobile page child. The desktop draw paths are
// untouched (embedded defaults to false at every desktop call-site).
void SmatchetUI::drawMobilePageContent(AppController& app, UiDrawSession& d) {
    switch (d.mobilePage) {
    case MobilePage::Grid: {
        // The desktop drawGridPaneWindows loop is skipped in mobile mode, so reproduce
        // its essential per-frame focused-pane setup before the embedded body draw:
        // ensure panes are loaded, mark the focused pane, point AppController's
        // focused-context delegators at it, kick its active-view sync, and route the
        // shared grid helpers (header/cells/new-issue) to it via activePaneForDraw.
        SmatchetGridPaneWindows::EnsurePanesLoaded(d);
        GridPane* focused = FindGridPaneById(d.gridPanes, d.focusedPaneId);
        if (focused == nullptr && !d.gridPanes.empty()) {
            focused = &d.gridPanes.front();
            d.focusedPaneId = focused->id;
        }
        if (focused != nullptr) {
            for (GridPane& p : d.gridPanes) {
                p.focused = (p.id == focused->id);
            }
            app.SetFocusedPane(focused->id);
            syncFocusedPaneWithActiveView(app, d, *focused, false);
            const TrackerConnectivityBannerForUi trackerBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
            d.activePaneForDraw = focused;
            drawActiveProjectWindow(app, d, *focused, trackerBanner, /*embedded=*/true);
            d.activePaneForDraw = nullptr;
        }
        break;
    }
    case MobilePage::Views:
        drawViewsDashboardWindow(app, d, /*embedded=*/true);
        break;
    case MobilePage::Log:
        drawLogWindow(d, /*embedded=*/true);
        break;
    case MobilePage::Settings:
        drawPreferencesWindow(app, d, /*embedded=*/true);
        break;
    case MobilePage::Ai:
#if defined(SMATCHET_WITH_AI)
        drawAiAssistantPanel(app, d, /*embedded=*/true);
#else
        (void)app;
        ::ImGui::TextDisabled("AI assistant is not built in this configuration.");
#endif
        break;
    }
}

// Bottom nav: one equal-width button per configured page; the active page is highlighted.
void SmatchetUI::drawMobileBottomNav(AppController& app, UiDrawSession& d) {
    (void)app;
    const std::vector<std::string>& pages = d.cfg.MobileNavPages;
    const int count = static_cast<int>(pages.size());
    if (count <= 0) {
        return;
    }
    const float spacing = ::ImGui::GetStyle().ItemSpacing.x;
    const float avail = ::ImGui::GetContentRegionAvail().x;
    const float btnW = (avail - spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
    const float btnH = ::ImGui::GetContentRegionAvail().y;

    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            ::ImGui::SameLine();
        }
        const MobilePage page = mobilePageFromString(pages[static_cast<std::size_t>(i)]);
        const bool active = (page == d.mobilePage);
        if (active) {
            ::ImGui::PushStyleColor(ImGuiCol_Button, ::ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        ::ImGui::PushID(i);
        if (::ImGui::Button(navPageLabel(pages[static_cast<std::size_t>(i)]), ::ImVec2(btnW, btnH))) {
            d.mobilePage = page;
            d.mobileDrawerOpen = false;
        }
        ::ImGui::PopID();
        if (active) {
            ::ImGui::PopStyleColor();
        }
    }
}

// Overlay drawer (slide-in left panel). Slice 3 minimal: a dimmed full-screen catcher
// that closes on outside-click + a left panel listing the nav pages. Real drawer content
// (account, theme, mode toggle) lands later.
void SmatchetUI::drawMobileDrawer(AppController& app, UiDrawSession& d) {
    (void)app;
    if (!d.mobileDrawerOpen) {
        return;
    }
    const ::ImGuiViewport* vp = ::ImGui::GetMainViewport();
    const float drawerW = vp->WorkSize.x * 0.72f;

    // Full-screen dim catcher behind the panel.
    ::ImGui::SetNextWindowPos(vp->WorkPos);
    ::ImGui::SetNextWindowSize(vp->WorkSize);
    ::ImGui::SetNextWindowViewport(vp->ID);
    ::ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags kScrimFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (::ImGui::Begin("##MobileDrawerScrim", nullptr, kScrimFlags)) {
        if (::ImGui::IsWindowHovered() && ::ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            d.mobileDrawerOpen = false;
        }
    }
    ::ImGui::End();
    ::ImGui::PopStyleVar(2);

    // Left panel.
    ::ImGui::SetNextWindowPos(vp->WorkPos);
    ::ImGui::SetNextWindowSize(::ImVec2(drawerW, vp->WorkSize.y));
    ::ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;
    if (::ImGui::Begin("##MobileDrawerPanel", nullptr, kPanelFlags)) {
        ::ImGui::TextDisabled("Menu");
        ::ImGui::Separator();
        for (const std::string& id : d.cfg.MobileNavPages) {
            if (::ImGui::Selectable(navPageLabel(id))) {
                d.mobilePage = mobilePageFromString(id);
                d.mobileDrawerOpen = false;
            }
        }
    }
    ::ImGui::End();
}

// Grid page dock windows (slice 5): the ticket list (reuses the embedded desktop grid body)
// and the read-only issue-detail pane, docked top/bottom inside MobileContentDock. Submitted
// as top-level windows after the shell closes. The focused-pane setup mirrors the desktop
// drawGridPaneWindows loop (skipped in mobile) so the embedded grid renders against the
// focused pane's live context.
void SmatchetUI::drawMobileGridDockWindows(AppController& app, UiDrawSession& d, unsigned int gridDockId) {
    if (d.mobileDockNeedsSeed) {
        seedMobileGridDock(gridDockId);
        d.mobileDockNeedsSeed = false;
    }

    SmatchetGridPaneWindows::EnsurePanesLoaded(d);
    GridPane* focused = FindGridPaneById(d.gridPanes, d.focusedPaneId);
    if (focused == nullptr && !d.gridPanes.empty()) {
        focused = &d.gridPanes.front();
        d.focusedPaneId = focused->id;
    }

    const ImGuiWindowFlags kDockWinFlags = ImGuiWindowFlags_NoCollapse;
    if (::ImGui::Begin("Tickets###MobileGridList", nullptr, kDockWinFlags)) {
        if (focused != nullptr) {
            for (GridPane& p : d.gridPanes) {
                p.focused = (p.id == focused->id);
            }
            app.SetFocusedPane(focused->id);
            syncFocusedPaneWithActiveView(app, d, *focused, false);
            const TrackerConnectivityBannerForUi trackerBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
            d.activePaneForDraw = focused;
            drawActiveProjectWindow(app, d, *focused, trackerBanner, /*embedded=*/true);
            d.activePaneForDraw = nullptr;
        } else {
            ::ImGui::TextDisabled("No grid pane.");
        }
    }
    ::ImGui::End();

    if (::ImGui::Begin("Details###MobileGridDetail", nullptr, kDockWinFlags)) {
        drawMobileGridDetail(app, d, focused);
    }
    ::ImGui::End();
}

// One-shot DockBuilder seed: split MobileContentDock vertically, list on top (~60%), the
// issue-detail pane on the bottom (~40%). Runs only when no imgui_mobile.ini existed on mobile
// entry; afterwards the saved layout (incl. a user-dragged split ratio) wins.
void SmatchetUI::seedMobileGridDock(unsigned int gridDockId) {
    const ::ImGuiViewport* vp = ::ImGui::GetMainViewport();
    ::ImGui::DockBuilderRemoveNode(gridDockId);
    ::ImGui::DockBuilderAddNode(gridDockId, ImGuiDockNodeFlags_DockSpace);
    ::ImGui::DockBuilderSetNodeSize(gridDockId, vp->WorkSize);
    ImGuiID bottomNode = 0;
    ImGuiID topNode = 0;
    ::ImGui::DockBuilderSplitNode(gridDockId, ImGuiDir_Down, 0.40f, &bottomNode, &topNode);
    ::ImGui::DockBuilderDockWindow("Tickets###MobileGridList", topNode);
    ::ImGui::DockBuilderDockWindow("Details###MobileGridDetail", bottomNode);
    ::ImGui::DockBuilderFinish(gridDockId);
}

// Read-only issue-detail pane: the active ticket's id + each visible column's label/value,
// reusing the same date-display + clipped-text renderers as the grid cells. No editing here
// (cells stay editable in the list); this is a compact master/detail preview for small screens.
void SmatchetUI::drawMobileGridDetail(AppController& app, UiDrawSession& d, GridPane* focused) {
    if (focused == nullptr) {
        ::ImGui::TextDisabled("No grid pane.");
        return;
    }
    GridPane& pane = *focused;
    const std::string& activeId = pane.gridState.ActiveIssueId;
    if (activeId.empty()) {
        ::ImGui::TextDisabled("Select a ticket to see its details.");
        return;
    }
    const auto ticketsSnap = pane.ticketsSnapshot;
    if (!ticketsSnap) {
        ::ImGui::TextDisabled("No tickets loaded.");
        return;
    }
    const std::vector<CachedTicket>& tickets = *ticketsSnap;
    const CachedTicket* active = nullptr;
    for (const CachedTicket& t : tickets) {
        if (t.id == activeId) {
            active = &t;
            break;
        }
    }
    if (active == nullptr) {
        ::ImGui::TextDisabled("Selected ticket is not in the current view.");
        return;
    }

    ViewDefinition* activeView = resolvePaneView(d, pane);
    const TrackerFieldCatalogIndex& catalogIndex = *gridFrameCtx_.catalogIndex;
    std::shared_ptr<const ViewDefinition> paneOwnResolvedView =
        pane.focused ? nullptr : app.GetPaneResolvedView(pane.id);
    const std::vector<TicketGridColumn>& columns =
        resolvePaneColumns(pane, catalogIndex, activeView, paneOwnResolvedView.get());

    ::ImGui::TextUnformatted(active->id.c_str());
    ::ImGui::Separator();
    for (const TicketGridColumn& column : columns) {
        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
            continue;
        }
        const std::string value = active->GetFieldValue(column.FieldId);
        const TrackerField* fieldMeta = catalogIndex.Find(column.FieldId);
        const std::string display = DisplayValueForTrackerDateField(
            column.FieldId, fieldMeta, value, d.cfg.DateFormatOption, d.cfg.DateCompactRelativeThresholdDays);
        const bool isDescriptionField =
            !column.FieldId.empty() && (column.FieldId.find("description") != std::string::npos ||
                                        column.FieldId.find("Description") != std::string::npos);
        const std::string* tip = (column.IsDateLike || isDescriptionField) ? &value : nullptr;
        ::ImGui::TextDisabled("%s", column.Label.c_str());
        RenderClippedFieldText(display, ::ImGui::GetContentRegionAvail().x, d.cfg.EnableFieldOverflowTooltips, false,
                               tip, isDescriptionField, &column.FieldId);
        ::ImGui::Spacing();
    }
}

// Desktop->Mobile ini edge. Captures the host's desktop imgui.ini pointer, detaches it (so
// ImGui stops auto-saving desktop dock nodes), drops the in-memory desktop layout, and loads
// imgui_mobile.ini if present. Absent file -> arm the DockBuilder seed. Idempotent: re-runs are
// no-ops while mobileDockSeeded stays true.
void SmatchetUI::drawMobileEnsureIniAttached(UiDrawSession& d) {
    if (d.mobileDockSeeded) {
        return;
    }
    ::ImGuiIO& io = ::ImGui::GetIO();
    d.savedDesktopIniFilename = io.IniFilename;
    io.IniFilename = nullptr;
    ::ImGui::ClearIniSettings();
    const std::string mobileIni = ConfigManager::GetMobileImGuiSettingsPath();
    bool loaded = false;
    {
        std::error_code ec;
        if (ghc::filesystem::exists(mobileIni, ec) && !ec) {
            ::ImGui::LoadIniSettingsFromDisk(mobileIni.c_str());
            loaded = true;
        }
    }
    d.mobileDockNeedsSeed = !loaded;
    d.mobileDockSeeded = true;
    io.WantSaveIniSettings = false;
    LOG_DEBUG("Mobile shell: attached imgui_mobile.ini (loaded=%d, willSeed=%d)", loaded ? 1 : 0, loaded ? 0 : 1);
}

// Mobile->Desktop ini edge (called from the desktop Draw path). Flushes mobile geometry one
// last time, drops the mobile layout, re-attaches the captured desktop imgui.ini pointer, and
// reloads it so desktop windows come back exactly as saved (the byte-identical round-trip:
// nothing in the mobile session ever writes imgui.ini). Resets the seed latches per the plan.
void SmatchetUI::drawMobileRestoreDesktopIni(UiDrawSession& d) {
    if (!d.mobileDockSeeded) {
        return;
    }
    ::ImGui::SaveIniSettingsToDisk(ConfigManager::GetMobileImGuiSettingsPath().c_str());
    ::ImGui::ClearIniSettings();
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.IniFilename = d.savedDesktopIniFilename;
    d.savedDesktopIniFilename = nullptr;
    if (io.IniFilename != nullptr) {
        ::ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    }
    io.WantSaveIniSettings = false;
    d.mobileDockSeeded = false;
    d.mobileDockNeedsSeed = false;
    d.mobilePageSeeded = false;
    LOG_DEBUG("Mobile shell: restored desktop imgui.ini");
}

#undef ImGui
