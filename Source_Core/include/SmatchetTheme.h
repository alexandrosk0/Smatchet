#pragma once

#include "SmatchetThemeIds.h"
#include "imgui.h"

#include <cstdint>

/** Per-theme palette for the C++ syntax tokenizer in CppSyntaxHighlight. */
struct SmatchetThemeSyntaxColors {
    float Keyword[4];
    float String[4];
    float Comment[4];
    float Number[4];
    float Preprocessor[4];
};

/** Per-theme palette for the AI chat panel (Phase 5 of ai-chat-claude-desktop-parity).
 *
 *  Six tokens consumed by Phase 6 (hover action row + user bubble + pin strip render
 *  rewrite). Phase 5 only populates them per-theme via `ApplyStyle`; consumers land
 *  in the next slice. Stored as `float[4]` (RGBA) to mirror `SmatchetThemeSyntaxColors`
 *  — same shape, same access pattern, same easy doctest pinning.
 *
 *  - `AiUserBubbleBg` — soft tint behind user messages so they're visually distinct
 *    from assistant turns on every theme. Alpha typically ~0.18 so the underlying
 *    text colour wins WCAG AA contrast against the window bg.
 *  - `AiUserRoleLabel` / `AiAssistantRoleLabel` — "You:" / "Assistant:" role-label
 *    colour. Picked per theme to contrast against bubble bg + window bg.
 *  - `AiActionRowIcon` / `AiActionRowIconHover` — Copy / Pin / Bookmark-close glyph
 *    colours (and text-label fallback when `SmatchetAreFaIconsLoaded() == false`).
 *    Idle = muted, hover = full-strength.
 *  - `AiPinStripBg` — background of the pinned-bookmarks strip above the history
 *    scroll child. Slightly more opaque than `AiUserBubbleBg` so the strip reads
 *    as a distinct surface, not a decorated message.
 */
struct SmatchetThemeAiColors {
    float AiUserBubbleBg[4];
    float AiUserRoleLabel[4];
    float AiAssistantRoleLabel[4];
    float AiActionRowIcon[4];
    float AiActionRowIconHover[4];
    float AiPinStripBg[4];
};

namespace SmatchetTheme {
/** Apply the named style palette to the current ImGui context. */
void ApplyStyle(ThemeId theme);

/** Active theme's C++ syntax-highlight palette. Updated by ApplyStyle. */
const SmatchetThemeSyntaxColors& GetSyntaxColors();

/** Active theme's AI chat-panel palette. Updated by ApplyStyle. Phase 5 of
 *  ai-chat-claude-desktop-parity — Phase 6 consumers (action row, bubble bg,
 *  pin strip) read through this accessor. */
const SmatchetThemeAiColors& GetActiveAiColors();

/** Monotonic counter bumped every time `ApplyStyle` completes. Consumers that
 *  cache theme-dependent state (`CodeColorView`'s tokenize cache, future
 *  per-theme glyph caches) snapshot the counter alongside their key; cache
 *  miss when their snapshot doesn't equal the live value. Atomic so the
 *  read-from-render-thread / write-from-ApplyStyle race is well-defined
 *  (ApplyStyle is always UI-thread, render is always UI-thread today, but
 *  the atomic future-proofs against any worker-thread theme audit). Slice 3
 *  of `docs/design/code-syntax-coloring-and-tooltips.md`. */
std::uint64_t GetThemeRevision();

/** Predefined colors for status and priorities. */
namespace Colors {
const ImVec4 StatusDone = ImVec4(0.25f, 0.60f, 0.30f, 1.0f);       // Green
const ImVec4 StatusInProgress = ImVec4(0.15f, 0.45f, 0.85f, 1.0f); // Blue
const ImVec4 StatusToDo = ImVec4(0.40f, 0.40f, 0.45f, 1.0f);       // Grey/Muted
const ImVec4 StatusBlocked = ImVec4(0.80f, 0.20f, 0.20f, 1.0f);    // Red

const ImVec4 PriorityHigh = ImVec4(0.90f, 0.30f, 0.30f, 1.0f);   // Red
const ImVec4 PriorityMedium = ImVec4(0.90f, 0.60f, 0.20f, 1.0f); // Orange
const ImVec4 PriorityLow = ImVec4(0.30f, 0.70f, 0.40f, 1.0f);    // Green
} // namespace Colors
} // namespace SmatchetTheme
