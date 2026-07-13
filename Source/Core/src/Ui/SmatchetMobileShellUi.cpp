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
#include "Ui/SmatchetTheme.h"
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

// Auto-mode width-hysteresis consts + resolveAutoEffectiveUiMode now live in SmatchetUiModeIds.h
// (pure + unit-tested); drawResolveUiMode below feeds them a DPI-independent logical width.

// Base band heights (device-independent px), scaled by the touch-density factor so a
// Comfortable shell gets larger hit targets than a Compact one.
constexpr float kAppBarBaseHeightPx = 40.0f;
constexpr float kBottomNavBaseHeightPx = 48.0f;
// P1.2 — height of the Grid-page touch view quick-switcher band (one tab row), scaled like the
// other bands. Only the Grid page reserves it; other pages get the full content region.
constexpr float kViewSwitcherBaseHeightPx = 40.0f;

} // namespace

// A6 (Chromebook) host UI-mode hint — declared in SmatchetUiModeIds.h. TU-local storage so it lives
// in the one place that reads it (drawResolveUiMode below). The host sets it once at startup; it is
// read once per frame. External-linkage functions (no -Wunused-function risk); the flag is only ever
// set true by the ChromeOS/ARC host, so phone/tablet/desktop behaviour is unchanged.
static bool s_hostPrefersDesktopUi = false;
void SmatchetSetHostPrefersDesktopUi(bool prefers) { s_hostPrefersDesktopUi = prefers; }
bool SmatchetHostPrefersDesktopUi() { return s_hostPrefersDesktopUi; }

// Resolves cfg.UiMode -> d.effectiveUiMode once per frame, delegating the decision to the pure,
// unit-tested resolveEffectiveUiMode (SmatchetUiModeIds.h): an explicit Desktop/Mobile pin wins; a
// host "prefers desktop" hint (A6 / ChromeOS) overrides Auto; otherwise the width hysteresis decides.
// The logical width is the raw io.DisplaySize.x (physical surface px on the Android EGL host) divided
// by the host density scale — without this divide a high-DPI phone's large pixel count (e.g. 1080 px
// at 2.6x) reads as a wide desktop and Auto never resolves Mobile. Desktop density is 1.0, so logical
// == physical there. d.effectiveUiMode is both the output AND the cross-frame hysteresis carry.
void SmatchetUI::drawResolveUiMode(UiDrawSession& d) {
    const float density = SmatchetTheme::HostDensityScale();
    const float logicalWidth = ::ImGui::GetIO().DisplaySize.x / (density > 0.0f ? density : 1.0f);
    d.effectiveUiMode =
        resolveEffectiveUiMode(d.cfg.UiMode, SmatchetHostPrefersDesktopUi(), logicalWidth, d.effectiveUiMode);
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
    // The base band heights are calibrated in desktop logical px. On a HiDPI host the style
    // (FramePadding) and font atlas are already scaled by the host density via ScaleAllSizes, so
    // the bands must carry the same density factor or they collapse shorter than the buttons they
    // host — a 40px bar under a 67px glyph clips its own content to an invisible sliver. Desktop
    // density is 1.0, so this is a no-op there (same recovery as the Auto-resolve logical-width divide).
    const float density = SmatchetTheme::HostDensityScale();
    const float densityFactor = density > 0.0f ? density : 1.0f;
    const float appBarH = kAppBarBaseHeightPx * scale * densityFactor;
    const float navH = kBottomNavBaseHeightPx * scale * densityFactor;

    const ::ImGuiViewport* vp = ::ImGui::GetMainViewport();
    ::ImGui::SetNextWindowPos(vp->WorkPos);
    ::ImGui::SetNextWindowSize(vp->WorkSize);
    ::ImGui::SetNextWindowViewport(vp->ID);

    // Deliberately NO NoBringToFrontOnFocus: that flag pins a window to ImGui's back layer,
    // the same layer as the host DockSpaceOverViewport window. The host's empty central dock
    // node fills that layer with the dark ImGuiCol_DockingEmptyBg, which then paints OVER the
    // shell and renders it invisible (blank dark screen). The shell must occlude the host
    // viewport, so it stays in the normal (front) layer. NoNavFocus is kept (keyboard-nav only).
    const ImGuiWindowFlags kShellFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

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
        // The Grid page carries a touch view quick-switcher band between the app bar and the
        // content dock (P1.2); other pages reserve no switcher height.
        const float switcherH = gridPage ? (kViewSwitcherBaseHeightPx * scale * densityFactor) : 0.0f;
        const float contentH = totalH - appBarH - navH - switcherH;

        if (::ImGui::BeginChild("##MobileAppBar", ::ImVec2(0.0f, appBarH), false, ImGuiWindowFlags_NoScrollbar)) {
            drawMobileTopAppBar(app, d);
        }
        ::ImGui::EndChild();

        if (gridPage && switcherH > 0.0f) {
            drawMobileViewQuickSwitcher(app, d, switcherH);
        }

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

    // Views confirm modals at shell level — must run every frame, not inside the drawer body:
    // a dirty view-switch latches the discard-confirm flag AND closes the drawer (#1117), so a
    // drawer-nested modal would never render and the switch would latch unresolvably. Submitted
    // after the drawer so the popup overlays on top; same-frame consume of the just-set flag.
    drawMobileViewsModals(app, d);

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
    ::ImGui::Text("Smatchet \xe2\x80\x94 %s", mobileNavPageLabel(mobilePageToString(d.mobilePage)).c_str());
}

// Resolve the focused grid pane for the mobile paths (slice 5/10). The desktop
// drawGridPaneWindows loop is skipped in mobile mode, so ensure panes are loaded and
// pick the focused pane (fallback to the front pane, persisting its id). Returns null
// only when there are no panes at all.
GridPane* SmatchetUI::ensureFocusedMobileGridPane(UiDrawSession& d) {
    SmatchetGridPaneWindows::EnsurePanesLoaded(d);
    GridPane* focused = FindGridPaneById(d.gridPanes, d.focusedPaneId);
    if (focused == nullptr && !d.gridPanes.empty()) {
        focused = &d.gridPanes.front();
        d.focusedPaneId = focused->id;
    }
    return focused;
}

// Mark the resolved pane focused, point AppController's focused-context delegators at it,
// kick its active-view sync, and draw the embedded grid body against it (the shared grid
// helpers — header/cells/new-issue — route through activePaneForDraw). Mirrors the desktop
// drawGridPaneWindows per-frame setup; shared by the single-fill Grid page + the dockspace
// Tickets window so the setup lives in exactly one place.
void SmatchetUI::drawEmbeddedFocusedGrid(AppController& app, UiDrawSession& d, GridPane& focused) {
    for (GridPane& p : d.gridPanes) {
        p.focused = (p.id == focused.id);
    }
    app.SetFocusedPane(focused.id);
    syncFocusedPaneWithActiveView(app, d, focused, false);
    const TrackerConnectivityBannerForUi trackerBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
    d.activePaneForDraw = &focused;
    drawActiveProjectWindow(app, d, focused, trackerBanner, /*embedded=*/true);
    d.activePaneForDraw = nullptr;
}

// Page content (slice 4): single-panel fill. Each page draws one desktop helper with
// embedded=true, which suppresses that helper's dock-window chrome (Begin/End/focus/
// open-gate/persist) so its body fills the mobile page child. The desktop draw paths are
// untouched (embedded defaults to false at every desktop call-site).
void SmatchetUI::drawMobilePageContent(AppController& app, UiDrawSession& d) {
    switch (d.mobilePage) {
    case MobilePage::Grid: {
        // Single-panel fill: resolve the focused pane (shared with the dockspace path)
        // then draw the embedded grid body against it.
        GridPane* focused = ensureFocusedMobileGridPane(d);
        if (focused != nullptr) {
            drawEmbeddedFocusedGrid(app, d, *focused);
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
        ::ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("mobile.ai.not_built", "AI assistant is not built in this configuration."));
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
        if (::ImGui::Button(mobileNavPageLabel(pages[static_cast<std::size_t>(i)]).c_str(), ::ImVec2(btnW, btnH))) {
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
        ::ImGui::TextDisabled("%s", SmatchetLocalization::T("mobile.drawer.pages", "Pages"));
        ::ImGui::Separator();
        // Full 5-page universe (not just the visible MobileNavPages subset) so a
        // page hidden from the bottom nav stays reachable from the drawer.
        static const char* const kAllPageIds[] = {"grid", "views", "log", "settings", "ai"};
        for (const char* id : kAllPageIds) {
            const bool selected = (mobilePageFromString(id) == d.mobilePage);
            if (::ImGui::Selectable(mobileNavPageLabel(id).c_str(), selected)) {
                d.mobilePage = mobilePageFromString(id);
                d.mobileDrawerOpen = false;
            }
        }
        ::ImGui::Spacing();
        ::ImGui::TextDisabled("%s", SmatchetLocalization::T("mobile.page.views", "Views"));
        ::ImGui::Separator();
        // Slice 6 — reuse the desktop Views sidebar (search / activate / rename /
        // duplicate / delete) inside the drawer; picking a view closes the drawer.
        drawMobileDrawerViews(app, d);
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

    GridPane* focused = ensureFocusedMobileGridPane(d);

    const ImGuiWindowFlags kDockWinFlags = ImGuiWindowFlags_NoCollapse;
    // The aliased wrapper Begin translates the visible "Tickets"/"Details" title while the
    // "###" suffix keeps the dock ID stable, so seedMobileGridDock's raw DockBuilder strings
    // keep matching in every language.
    if (ImGui::Begin("Issues###MobileGridList", nullptr, kDockWinFlags)) {
        if (focused != nullptr) {
            drawEmbeddedFocusedGrid(app, d, *focused);
        } else {
            ::ImGui::TextDisabled("%s", SmatchetLocalization::T("mobile.grid.no_pane", "No grid pane."));
        }
    }
    ::ImGui::End();

    if (ImGui::Begin("Details###MobileGridDetail", nullptr, kDockWinFlags)) {
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
    ::ImGui::DockBuilderDockWindow("Issues###MobileGridList", topNode);
    ::ImGui::DockBuilderDockWindow("Details###MobileGridDetail", bottomNode);
    ::ImGui::DockBuilderFinish(gridDockId);
}

// Read-only issue-detail pane: the active ticket's id + each visible column's label/value,
// reusing the same date-display + clipped-text renderers as the grid cells. No editing here
// (cells stay editable in the list); this is a compact master/detail preview for small screens.
void SmatchetUI::drawMobileGridDetail(AppController& app, UiDrawSession& d, GridPane* focused) {
    if (focused == nullptr) {
        ::ImGui::TextDisabled("%s", SmatchetLocalization::T("mobile.grid.no_pane", "No grid pane."));
        return;
    }
    GridPane& pane = *focused;
    const std::string& activeId = pane.gridState.ActiveIssueId;
    if (activeId.empty()) {
        ::ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("mobile.grid.select_ticket", "Select a ticket to see its details."));
        return;
    }
    const auto ticketsSnap = pane.ticketsSnapshot;
    if (!ticketsSnap) {
        ::ImGui::TextDisabled("%s", SmatchetLocalization::T("mobile.grid.no_tickets", "No tickets loaded."));
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
        ::ImGui::TextDisabled("%s", SmatchetLocalization::T("mobile.grid.ticket_not_in_view",
                                                            "Selected ticket is not in the current view."));
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
