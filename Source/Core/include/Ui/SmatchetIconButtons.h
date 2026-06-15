#pragma once

#include "Ui/SmatchetIconButtonLabel.h"
#include "imgui.h"

// Icon-ONLY ImGui::Button: shows the Font Awesome glyph when the FA6 Solid font
// is merged into the atlas (SmatchetAreFaIconsLoaded()), else falls back to the
// `fallbackLabel` text button (graceful degradation when fa-solid-900.ttf is
// missing). The hover `tooltip` is shown in BOTH paths so an icon-only control is
// never a naked unlabelled glyph (Pillar 4 accessibility). `icon` is an ICON_FA_*
// glyph macro; pure label-selection logic lives in SmatchetIconButtonLabel.h.
// Returns true on click; `size` is forwarded to ImGui::Button (default auto-size).
bool SmatchetIconButton(const char* icon, const char* fallbackLabel, const char* tooltip,
                        const ImVec2& size = ImVec2(0.0f, 0.0f));

// Icon-LEADING ImGui::Button: "<icon> <label>" when the FA font is loaded, else
// the bare `label`. For commit-style actions (Save & Sync / Apply & Sync) that
// KEEP their visible text but gain a leading glyph. Optional hover `tooltip`
// (nullptr/"" to skip). Returns true on click; `size` forwarded to ImGui::Button.
bool SmatchetIconLeadingButton(const char* icon, const char* label, const char* tooltip,
                               const ImVec2& size = ImVec2(0.0f, 0.0f));
