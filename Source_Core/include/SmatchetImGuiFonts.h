#pragma once

#include "imgui.h"

#include <string>

// Rebuilds the UI font with wide Unicode ranges (bullets ● •, dingbats, arrows, math, Greek, emoji BMP+SMP, …).
// On Windows, prefers Segoe UI + Symbol + Emoji merge (needs IMGUI_USE_WCHAR32 from CMake for SMP glyphs).
// Call once after ImGui::CreateContext(), before the first font atlas build / backend init.
void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io);

// Request a dynamic font change on the next frame boundary.
void SmatchetRequestFontReload(const std::string& fontName, float fontSizePixels);

// Check if a font reload is pending and reload the font if so. Returns true if a reload happened.
bool SmatchetCheckAndApplyFontReload();






