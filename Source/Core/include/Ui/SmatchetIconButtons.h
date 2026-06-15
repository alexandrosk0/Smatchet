#pragma once

#include "Ui/SmatchetIconButtonLabel.h"
#include "imgui.h"

// Renders an icon-ONLY ImGui::Button when the Font Awesome 6 Solid font is
// merged into the active atlas (SmatchetAreFaIconsLoaded()), with a mandatory
// hover tooltip; otherwise falls back to the original `fallbackLabel` text
// button (graceful degradation when fa-solid-900.ttf is unavailable). Returns
// true on click. `size` is forwarded to ImGui::Button (default = auto-size).
//
// Pillar 4 (accessibility): the tooltip is shown on hover in BOTH the icon and
// text paths, so an icon-only control is never a naked, unlabelled glyph.
//
// `icon` is a Font Awesome glyph macro (e.g. ICON_FA_ARROWS_ROTATE); the pure
// label-selection logic lives in SmatchetIconButtonLabel.h.
bool SmatchetIconButton(const char* icon, const char* fallbackLabel, const char* tooltip,
                        const ImVec2& size = ImVec2(0.0f, 0.0f));

// Renders an icon-LEADING ImGui::Button — "<icon> <label>" when the FA font is
// loaded, else the bare `label` text. For commit-style actions (Save & Sync /
// Apply & Sync) that KEEP their visible text but gain a leading glyph (dropping
// the label on a write-and-sync action would hurt clarity). Optional hover
// `tooltip` (nullptr/"" to skip). Returns true on click; `size` forwarded to
// ImGui::Button.
bool SmatchetIconLeadingButton(const char* icon, const char* label, const char* tooltip,
                               const ImVec2& size = ImVec2(0.0f, 0.0f));
