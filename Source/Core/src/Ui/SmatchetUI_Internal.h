#pragma once

// Private implementation header for SmatchetUI — shared by:
//   SmatchetUI.cpp, SmatchetUI_MainMenu.cpp, SmatchetUI_Layout.cpp.
// Do NOT include this from any other translation unit; it pulls in the
// `#define ImGui SmatchetLocalizedImGui` alias which would silently rewrite
// every ImGui:: call in the including file.
// Cross-TU helpers that were previously file-scope statics are exposed inside
// `namespace smatchet { namespace ui_detail { ... } }` with external linkage
// so the linker resolves them across the new TUs without ODR collisions.

#include "SmatchetUI.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
// imgui_internal.h MUST be included before the `#define ImGui` alias below — the
// alias would otherwise rewrite the header's own `namespace ImGui { ... }`.
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

class AppController;
struct TrackerConfig;

namespace smatchet {
namespace ui_detail {

// Triggers a one-shot async app-update check via std::async. Called from the
// startup branch in SmatchetUI::Draw and from the Help > "Check for Updates..."
// menu item in SmatchetUI::drawMainMenuBar.
void StartAppUpdateCheck(UiDrawSession& d, AppController& app, bool manual);

// Mirrors the d.show* window-visibility flags into d.cfg.Show*Window and
// persists via ConfigManager::Save when anything changed. Called from
// SmatchetUI::Draw (end of frame) and the reset-layout helpers (Layout TU).
void PersistWindowOpenPreferences(UiDrawSession& d);

} // namespace ui_detail
} // namespace smatchet
