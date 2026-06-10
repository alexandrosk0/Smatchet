#pragma once

#include "SmatchetThemeIds.h"
#include "imgui.h"

#include <cstdint>

/** Per-theme palette for the C++ syntax tokenizer in CppSyntaxHighlight + the
 *  polymorphic `CodeColorView` (slice 1 of code-syntax-coloring-and-tooltips). */
struct SmatchetThemeSyntaxColors {
    float Keyword[4];
    float String[4];
    float Comment[4];
    float Number[4];
    float Preprocessor[4];
    /** Slice 6 of code-syntax-coloring-and-tooltips — identifier token color
     *  (function names, type names, ordinary identifiers, KnownIdentifier +
     *  PreprocIdentifier from LanguageDefinition::mIdentifiers /
     *  mPreprocIdentifiers). Previously identifiers fell through to
     *  ImGuiCol_Text (same as plain text); the user reported "no coloring"
     *  on call-stack / annotate views which are mostly identifiers. Pick a
     *  per-theme tint that's visibly distinct from default text but doesn't
     *  dominate (identifiers are the most common token). */
    float Identifier[4];
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

/** Scale every size / spacing / rounding metric of the current style by `densityScale`
 *  (wraps ImGui::GetStyle().ScaleAllSizes). Mobile host-injection seam (#13): a high-DPI
 *  touch host calls this AFTER ApplyStyle — which rebuilds the style from scratch and would
 *  otherwise wipe any earlier scaling — to enlarge controls to finger size; the host also
 *  sets io.DisplayFramebufferScale per frame for crisp rasterization. No-op when
 *  `densityScale` <= 0 or NaN (caller error — current style is left intact). Desktop never
 *  calls it (scale stays 1.0), so the seam is inert and fully desktop-compilable. */
void ApplyUiDensityScale(float densityScale);

/** Re-assert the host density scale to `newScale` WITHOUT compounding. ApplyUiDensityScale
 *  multiplies the *current* style, so calling it a second time after a runtime DPI change would
 *  stack the old and new scales. This instead applies only the relative factor (newScale / old)
 *  to the live style and updates the stored host scale so later ApplyStyle rebuilds re-assert the
 *  new value. Mobile seam: the Android host calls it on APP_CMD_CONFIG_CHANGED when the display
 *  density moves (item 15 / #1071-adjacent). No-op on `newScale` <= 0 / NaN or when unchanged;
 *  desktop never calls it (scale stays 1.0). */
void ReassertHostDensityScale(float newScale);

/** The host-injected UI density scale last set via ApplyUiDensityScale / ReassertHostDensityScale
 *  (1.0 on desktop where the seam is inert; e.g. ~2.6 on a 420-dpi phone). The Auto UI-mode
 *  breakpoint divides the raw `io.DisplaySize.x` (physical surface pixels on the Android EGL host)
 *  by this to recover a DPI-independent logical width — without it a high-DPI phone's large pixel
 *  count reads as a wide desktop and Auto never resolves Mobile. UI-thread-only read. */
float HostDensityScale();

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
 *  of `code-syntax-coloring-and-tooltips`. */
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
