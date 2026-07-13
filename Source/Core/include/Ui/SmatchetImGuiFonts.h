#pragma once

#include "imgui.h"

#include <string>

// Rebuilds the UI font with wide Unicode ranges (bullets ● •, dingbats, arrows, math, Greek, emoji BMP+SMP, …).
// On Windows, prefers Segoe UI + Symbol + Emoji merge (needs IMGUI_USE_WCHAR32 from CMake for SMP glyphs).
// Call once after ImGui::CreateContext(), before the first font atlas build / backend init.
void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io);

// Host-injected font bytes (#12, mobile host-injection seam). A host that cannot expose a
// system-font *path* to Core (Android — Core TUs include no <android/...> headers) reads a
// bundled TTF itself and injects the raw bytes here. Call ONCE before the first
// SmatchetApplyImGuiFont* call: the bytes are copied into a Core-owned buffer and loaded via
// AddFontFromMemoryTTF on every platform, taking precedence over the per-OS font-file path.
// `data`==null / `dataSize`<=0 / `sizePixels`<=0 clears the injection (resume default path).
// Desktop never calls this (it has the system-font path); the seam stays desktop-compilable.
void SmatchetSetInjectedFontBytes(const void* data, int dataSize, float sizePixels);

// Request a dynamic font change on the next frame boundary.
void SmatchetRequestFontReload(const std::string& fontName, float fontSizePixels);

// True when `fontName` resolves to a font file present on this machine. Lets the Font
// picker mark entries that would silently fall back to the built-in default (a missing
// system TTF is common on trimmed installs / non-Windows hosts). Names the resolver does
// not know return false.
bool SmatchetIsFontAvailable(const std::string& fontName);

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
    // Bug-report censor: renders every text codepoint as a U+2588 block (icons
    // survive via merged FA). Falls back to Regular if the body font lacks U+2588.
    ImFont* Redaction = nullptr;
};

// Reads the most recently built preview font set. Stable across frames; pointers
// are invalidated after every SmatchetApplyImGuiFont call (i.e. font hot-reload).
const SmatchetPreviewFonts& SmatchetGetPreviewFonts();

// Bug-report censor (font-redaction): for
// the single screenshot-capture frame, repoint every preview font + io.FontDefault
// at the Redaction font so the captured frame renders all text as blocks (sharp,
// layout-preserving). Push BEFORE ImGui::NewFrame; Pop after the capture. No-op
// when no Redaction font is available. Not re-entrant (one capture at a time).
void SmatchetPushRedactionFonts();
void SmatchetPopRedactionFonts();

// True iff the Font Awesome 6 Solid TTF was successfully merged into the active
// atlas on the most-recent `SmatchetApplyImGuiFont` call. False when the TTF
// could not be located (missing from exe-dir + SMATCHET_ASSETS_SOURCE_DIR
// fallback) — UI consumers (AI chat hover row, pin strip, header buttons)
// check this flag and fall back to short text labels (`Copy`, `Pin`, `×`)
// instead of icon glyphs. Phase 4 of ai-chat-claude-desktop-parity.
bool SmatchetAreFaIconsLoaded();
