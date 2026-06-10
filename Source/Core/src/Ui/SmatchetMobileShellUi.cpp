// Mobile UI shell (dual-ui-mode-desktop-mobile plan, slice 3).
//
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
#include "SmatchetUiSession.h"
#include "SmatchetUiModeIds.h"
#include "Logger.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace
// (matches SmatchetUI.cpp). Positional-layout primitives use the ::ImGui:: real API.
#define ImGui SmatchetLocalizedImGui

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

    const float scale = mobileTouchDensityScale(d.cfg.MobileTouchDensity);
    const float appBarH = kAppBarBaseHeightPx * scale;
    const float navH = kBottomNavBaseHeightPx * scale;

    const ::ImGuiViewport* vp = ::ImGui::GetMainViewport();
    ::ImGui::SetNextWindowPos(vp->WorkPos);
    ::ImGui::SetNextWindowSize(vp->WorkSize);
    ::ImGui::SetNextWindowViewport(vp->ID);

    const ImGuiWindowFlags kShellFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ::ImVec2(0.0f, 0.0f));
    const bool open = ::ImGui::Begin("##MobileShell", nullptr, kShellFlags);
    ::ImGui::PopStyleVar(3);

    if (open) {
        const float totalH = ::ImGui::GetContentRegionAvail().y;
        const float contentH = totalH - appBarH - navH;

        if (::ImGui::BeginChild("##MobileAppBar", ::ImVec2(0.0f, appBarH), false,
                                ImGuiWindowFlags_NoScrollbar)) {
            drawMobileTopAppBar(app, d);
        }
        ::ImGui::EndChild();

        if (::ImGui::BeginChild("##MobilePage", ::ImVec2(0.0f, contentH > 0.0f ? contentH : 0.0f),
                                false)) {
            drawMobilePageContent(app, d);
        }
        ::ImGui::EndChild();

        if (::ImGui::BeginChild("##MobileNav", ::ImVec2(0.0f, navH), false,
                                ImGuiWindowFlags_NoScrollbar)) {
            drawMobileBottomNav(app, d);
        }
        ::ImGui::EndChild();
    }
    ::ImGui::End();

    // Drawer is an overlay drawn after (above) the shell window.
    drawMobileDrawer(app, d);
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

// Page content. Slice 3 placeholder — real page bodies (reusing desktop draw helpers via
// an `embedded` param) land slice 4+.
void SmatchetUI::drawMobilePageContent(AppController& app, UiDrawSession& d) {
    (void)app;
    ::ImGui::TextDisabled("%s page \xe2\x80\x94 coming soon", navPageLabel(mobilePageToString(d.mobilePage)));
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
    const ImGuiWindowFlags kScrimFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
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
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNavFocus;
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

#undef ImGui
