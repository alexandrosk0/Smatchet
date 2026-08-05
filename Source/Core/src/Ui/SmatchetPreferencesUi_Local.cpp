// Appearance category of the Preferences window: theme & font, layout & density,
// display. The Updates / date-format / storage / local-database blocks that used
// to live here moved to General (SmatchetPreferencesUi_General.cpp). The
// ticket-change-monitor body stays in this TU but is drawn by the Tracker page,
// which owns the section it now belongs to.

#include "SmatchetPreferencesUi_detail.h"
#include "SmatchetUI.h"
#include "ConfigManager.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "VsyncControl.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// Theme & font section: the application font combo. The UI-language combo that
// used to sit beside it now lives in General > Language & region.
void DrawAppearanceTypographySection(UiDrawSession& d) {
    const char* fonts[] = {
        "Segoe UI",       "Proggy (Clean/Default)", "Consolas",     "Arial",  "Courier New", "Georgia",
        "Lucida Console", "Microsoft Sans Serif",   "Trebuchet MS", "Verdana"};
    int currentFontIdx = 0;
    for (int i = 0; i < 10; ++i) {
        if (d.cfg.SelectedFontName == fonts[i]) {
            currentFontIdx = i;
            break;
        }
    }
    if (ImGui::Combo("Application Font", &currentFontIdx, fonts, 10)) {
        d.cfg.SelectedFontName = fonts[currentFontIdx];
        MarkPrefsDirty(d);
        SmatchetRequestFontReload(d.cfg.SelectedFontName, static_cast<float>(d.cfg.FontSizePt));
    }
    ImGui::SetItemTooltip("Application-wide font; applies instantly.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.font.help",
                               "Select the typography for the entire application. Rebuilds and reloads the "
                               "font atlas instantly.");
}

// Grid-and-field-text block of Layout & density: overflow tooltips + wheel-swallow ticks.
void DrawAppearanceGridTextSection(UiDrawSession& d) {
    ImGui::TextUnformatted("Grid and field text");
    ImGui::Separator();
    if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Hover truncated cells to read the full text.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.overflow_tooltips.help",
                               "When a value is truncated to fit the cell, or spans multiple lines, hover "
                               "to read the full text in a tooltip.");
    int gridWheelSwallowTicks = d.cfg.GridEndWheelSwallowsBeforeHorizontal;
    if (ImGui::InputInt("Wheel ticks before horizontal scroll", &gridWheelSwallowTicks)) {
        if (gridWheelSwallowTicks < 0) {
            gridWheelSwallowTicks = 0;
        }
        if (gridWheelSwallowTicks > 32) {
            gridWheelSwallowTicks = 32;
        }
        d.cfg.GridEndWheelSwallowsBeforeHorizontal = gridWheelSwallowTicks;
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Wheel ticks swallowed at the grid edge.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.wheel_swallow.help",
                               "At top/bottom of the ticket grid, vertical wheel starts horizontal "
                               "scrolling after this many wheel ticks. 0 routes immediately.");
}

// "Display" block — render/swapchain toggles. The vsync checkbox applies live
// via the smatchet::vsync hub (render loop picks it up next frame) and persists
// with the prefs Save like the sibling Appearance toggles.
void DrawAppearanceDisplaySection(UiDrawSession& d) {
    if (ImGui::Checkbox("Enable vsync", &d.cfg.VsyncEnabled)) {
        smatchet::vsync::SetEnabled(d.cfg.VsyncEnabled);
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Sync rendering with the monitor refresh rate.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.vsync.help",
                               "Synchronize rendering with the monitor refresh rate. Disabling uncaps the "
                               "frame rate (higher CPU/GPU usage).");
    // P2-M6: the confirm modal's "Don't ask again" needs a discoverable way back on.
    bool askBeforeLayoutReset = !d.cfg.SkipLayoutResetConfirm;
    if (ImGui::Checkbox("Ask before layout-resetting changes", &askBeforeLayoutReset)) {
        d.cfg.SkipLayoutResetConfirm = !askBeforeLayoutReset;
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Confirm before panel moves and Reset Layout replace your window arrangement.");
}

// Layout-mode block of Display: Desktop / Mobile / Auto selector. Auto resolves by
// viewport width per frame; Desktop/Mobile pin the mode. The combo index maps 1:1
// onto the UiMode enum order (Desktop=0/Mobile=1/Auto=2).
void DrawAppearanceUiModeSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Layout mode");
    ImGui::Separator();
    const char* modes[] = {SmatchetLocalization::T("prefs.uimode.desktop", "Desktop"),
                           SmatchetLocalization::T("prefs.uimode.mobile", "Mobile"),
                           SmatchetLocalization::T("prefs.uimode.auto", "Auto (by width)")};
    int currentMode = static_cast<int>(d.cfg.UiMode);
    if (ImGui::Combo("UI mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
        if (currentMode >= 0 && currentMode <= 2) {
            d.cfg.UiMode = static_cast<UiMode>(currentMode);
            MarkPrefsDirty(d);
        }
    }
    ImGui::SetItemTooltip("Desktop = docking workspace. Mobile = single-column shell with top bar + bottom nav. "
                          "Auto switches by window width.");
}

// Mobile-shell customization (Layout & density): touch density, bottom-nav page
// order + show/hide, and the home page. Only meaningful in Mobile/Auto, but shown
// always so the layout is configurable from a desktop session. Every mutation
// re-runs ConfigManager::SanitizeMobileNav (drop-unknown / dedup / >=1-visible /
// home-present guards) then MarkPrefsDirty for the debounced save.
void DrawAppearanceMobileSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Mobile layout");
    ImGui::Separator();

    // Touch density radio.
    int density = static_cast<int>(d.cfg.MobileTouchDensity);
    bool densityChanged = false;
    densityChanged |= ImGui::RadioButton("Compact", &density, static_cast<int>(MobileTouchDensity::Compact));
    ImGui::SameLine();
    densityChanged |= ImGui::RadioButton("Comfortable", &density, static_cast<int>(MobileTouchDensity::Comfortable));
    if (densityChanged) {
        d.cfg.MobileTouchDensity = static_cast<MobileTouchDensity>(density);
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Comfortable enlarges hit targets and fonts; Compact trades reach for density.");

    // Bottom-nav editor: visible pages first (ordered, with up/down + hide), then
    // the remaining hidden pages as unchecked rows to re-add.
    ImGui::Spacing();
    ImGui::TextDisabled("Bottom navigation");
    static const char* const kAllPageIds[] = {"grid", "views", "log", "settings", "ai"};
    std::vector<std::string>& nav = d.cfg.MobileNavPages;

    for (int i = 0; i < static_cast<int>(nav.size()); ++i) {
        ImGui::PushID(i);
        // Explicit hide action — a button, not a checkbox seeded to a constant. A visible
        // page only ever has one meaningful action here: drop it from the ordered nav list.
        // Re-adding a hidden page is handled by the separate unchecked-rows loop below.
        if (ImGui::SmallButton("Hide")) {
            nav.erase(nav.begin() + i);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
            std::swap(nav[static_cast<std::size_t>(i)], nav[static_cast<std::size_t>(i - 1)]);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < static_cast<int>(nav.size())) {
            std::swap(nav[static_cast<std::size_t>(i)], nav[static_cast<std::size_t>(i + 1)]);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(mobileNavPageLabel(nav[static_cast<std::size_t>(i)]).c_str());
        ImGui::PopID();
    }

    for (const char* id : kAllPageIds) {
        bool present = false;
        for (const std::string& cur : nav) {
            if (cur == id) {
                present = true;
                break;
            }
        }
        if (present) {
            continue;
        }
        ImGui::PushID(id);
        bool visible = false;
        if (ImGui::Checkbox("##vis", &visible)) {
            nav.push_back(id);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s (hidden)", mobileNavPageLabel(id).c_str());
        ImGui::PopID();
    }

    // Home page combo (from the visible pages).
    ImGui::Spacing();
    int homeIdx = 0;
    std::vector<std::string> homeLabels;
    for (int i = 0; i < static_cast<int>(nav.size()); ++i) {
        homeLabels.push_back(mobileNavPageLabel(nav[static_cast<std::size_t>(i)]));
        if (nav[static_cast<std::size_t>(i)] == d.cfg.MobileHomePage) {
            homeIdx = i;
        }
    }
    // ImGui::Combo wants a const char* array; build it after homeLabels is fully
    // populated so the std::string storage no longer reallocates (the pointers
    // stay valid for the Combo call).
    std::vector<const char*> homeLabelPtrs;
    homeLabelPtrs.reserve(homeLabels.size());
    for (const std::string& label : homeLabels) {
        homeLabelPtrs.push_back(label.c_str());
    }
    if (!homeLabelPtrs.empty() &&
        ImGui::Combo("Home page", &homeIdx, homeLabelPtrs.data(), static_cast<int>(homeLabelPtrs.size()))) {
        if (homeIdx >= 0 && homeIdx < static_cast<int>(nav.size())) {
            d.cfg.MobileHomePage = nav[static_cast<std::size_t>(homeIdx)];
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
    }
    ImGui::SetItemTooltip("The page shown first when the mobile shell opens.");
}

} // namespace

// Ticket-change monitor body (docs/plans/ticket-change-monitor.md). Enable toggle +
// poll-interval slider (greyed when the monitor is off). Both persist via MarkPrefsDirty,
// and the interval matches the [30, 3600] s load-clamp band. Lives here for history but the
// Tracker page owns the surrounding PrefsSection.
void DrawTrackerNotificationsSectionBody(UiDrawSession& d) {
    if (ImGui::Checkbox("Notify me when tracked issues change", &d.cfg.TicketChangeMonitorEnabled)) {
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Periodically poll the backend for changes to the open panes' tickets "
                          "and raise an in-app toast.");
    ImGui::BeginDisabled(!d.cfg.TicketChangeMonitorEnabled);
    int interval = d.cfg.TicketChangeMonitorIntervalSec;
    if (ImGui::SliderInt("Check interval (seconds)", &interval, 30, 3600, "%d s")) {
        d.cfg.TicketChangeMonitorIntervalSec = interval;
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("How often to poll for changes. Lower = faster notice, more requests.");
    ImGui::EndDisabled();
}

void DrawAppearancePreferencesTab(UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "appearance.theme_font", [&] {
        DrawAppearanceTypographySection(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "appearance.layout_density", [&] {
        DrawAppearanceGridTextSection(d);
        DrawAppearanceMobileSection(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "appearance.display", [&] {
        DrawAppearanceDisplaySection(d);
        DrawAppearanceUiModeSection(d);
    });
}
