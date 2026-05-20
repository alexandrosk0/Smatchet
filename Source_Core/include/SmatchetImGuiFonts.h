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

// Auxiliary fonts used by the Markdown preview pane to render inline marks
// (bold / italic / bold-italic / inline-code / code-block) without per-glyph
// font switching tricks. Each pointer is non-null after a successful font load —
// when a style variant TTF is missing on the host (rare) the pointer falls back
// to the regular font so callers can blindly PushFont without nullptr checks.
struct SmatchetPreviewFonts {
    ImFont* Regular = nullptr;
    ImFont* Bold = nullptr;
    ImFont* Italic = nullptr;
    ImFont* BoldItalic = nullptr;
    ImFont* Mono = nullptr;
};

// Reads the most recently built preview font set. Stable across frames; pointers
// are invalidated after every SmatchetApplyImGuiFont call (i.e. font hot-reload).
const SmatchetPreviewFonts& SmatchetGetPreviewFonts();

// True iff the Font Awesome 6 Solid TTF was successfully merged into the active
// atlas on the most-recent `SmatchetApplyImGuiFont` call. False when the TTF
// could not be located (missing from exe-dir + SMATCHET_ASSETS_SOURCE_DIR
// fallback) — UI consumers (AI chat hover row, pin strip, header buttons)
// check this flag and fall back to short text labels (`Copy`, `Pin`, `×`)
// instead of icon glyphs. Phase 4 of ai-chat-claude-desktop-parity.
bool SmatchetAreFaIconsLoaded();
