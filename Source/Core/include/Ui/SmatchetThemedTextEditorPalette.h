#pragma once

// Theme-aware TextEditor::Palette builders.
// Why this exists: AiChatTextEditorView and LuaConsolePlugin each embed a
// `TextEditor` widget whose color palette is set via SetPalette(). The
// third-party `TextEditor::GetDarkPalette()` / `GetLightPalette()` return
// `static const Palette&` constants — they're theme-INDEPENDENT. Before this
// helper landed, each call site set the palette ONCE at construction and the
// palette never refreshed when the user switched the Smatchet theme. To the
// user, that looked like "the chat editor / Lua editor still shows the
// previous theme's colors after switching back" — a visible residual.
// The fix here: derive the palette from `SmatchetTheme::GetSyntaxColors()`
// (which IS theme-tracked — every SmatchetTheme::ApplyStyle helper updates it)
// and re-apply on every Draw(). The widgets only need a `std::array<ImU32,N>`
// copy per frame which is O(slots) and a handful of cache lines — negligible
// against the per-frame ImGui draw cost.
// GetThemedLuaConsolePalette builds the full code-editor palette (background,
// line numbers, cursor, etc.) with syntax slots overridden from the active
// SmatchetTheme palette.

#include "TextEditor.h"

namespace SmatchetTheme {

// Build a `TextEditor::Palette` snapshot from the active Smatchet theme. The
// caller is expected to apply it via `editor.SetPalette(...)` every frame so
// theme switches propagate instantly. Cheap by construction — sizeof(Palette)
// is `21 * sizeof(ImU32)` = 84 bytes, two cache lines.
TextEditor::Palette GetThemedLuaConsolePalette();

} // namespace SmatchetTheme
