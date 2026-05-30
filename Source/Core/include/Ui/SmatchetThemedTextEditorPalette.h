#pragma once

// Theme-aware TextEditor::Palette builders.
//
// Why this exists: AiChatTextEditorView and LuaConsolePlugin each embed a
// `TextEditor` widget whose color palette is set via SetPalette(). The
// third-party `TextEditor::GetDarkPalette()` / `GetLightPalette()` return
// `static const Palette&` constants — they're theme-INDEPENDENT. Before this
// helper landed, each call site set the palette ONCE at construction and the
// palette never refreshed when the user switched the Smatchet theme. To the
// user, that looked like "the chat editor / Lua editor still shows the
// previous theme's colors after switching back" — a visible residual.
//
// The fix here: derive the palette from `SmatchetTheme::GetSyntaxColors()`
// (which IS theme-tracked — every SmatchetTheme::ApplyStyle helper updates it)
// and re-apply on every Draw(). The widgets only need a `std::array<ImU32,N>`
// copy per frame which is O(slots) and a handful of cache lines — negligible
// against the per-frame ImGui draw cost.
//
// The two builders below produce palettes for the two distinct embed sites:
//   * GetThemedLuaConsolePalette — full code-editor palette (background, line
//     numbers, cursor, etc.) with syntax slots overridden from the active
//     SmatchetTheme palette.
//   * GetThemedAiChatPalette — derived from the Lua-console palette but with
//     three chat-specific overrides preserved (transparent background so the
//     parent BeginChild bleed-through stays correct, off-white body text for
//     prose readability, amber for user-role markers vs. theme-accent for
//     assistant). These overrides are deliberately theme-INDEPENDENT — the
//     chat panel is laid out by Smatchet, not the user's preferred IDE
//     palette, so we keep its branding consistent across themes.

#include "TextEditor.h"

namespace SmatchetTheme {

// Build a `TextEditor::Palette` snapshot from the active Smatchet theme. The
// caller is expected to apply it via `editor.SetPalette(...)` every frame so
// theme switches propagate instantly. Cheap by construction — sizeof(Palette)
// is `21 * sizeof(ImU32)` = 84 bytes, two cache lines.
TextEditor::Palette GetThemedLuaConsolePalette();

// Same as GetThemedLuaConsolePalette but with the AI-chat-specific overlay
// slots preserved (transparent bg, bright body text, amber user-role overlay).
TextEditor::Palette GetThemedAiChatPalette();

} // namespace SmatchetTheme
