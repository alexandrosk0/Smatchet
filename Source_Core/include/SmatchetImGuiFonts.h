#pragma once

#include "imgui.h"

// Rebuilds the UI font with wide Unicode ranges (bullets ● •, dingbats, arrows, math, Greek, …).
// On Windows, prefers Segoe UI (+ Symbol merge) so glyphs exist; elsewhere uses the default font + ranges.
// Call once after ImGui::CreateContext(), before the first font atlas build / backend init.
void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io);
