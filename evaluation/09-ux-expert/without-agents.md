# Smatchet UX / Product-Design Evaluation (without agents.md)

## 1. Executive Summary & Overall UX Verdict

Smatchet is a desktop issue-tracker / productivity client built on Dear ImGui. Judged as a daily-driver product — not as a tool demo — it lands in an unusual place: the **interaction model is genuinely sophisticated** (a rebindable command system shared across palette/CLI/MCP/Lua, a real Views editor with explicit-commit semantics, an AI side panel with per-turn context controls and an outbound-consent gate), while the **presentation layer carries the structural debt of its immediate-mode toolkit** (debug-UI density, no native text-input affordances, and — most seriously — effectively zero assistive-technology support).

The team is clearly accessibility-aware in a way most ImGui apps are not: contrast ratios are computed and pinned in code comments, a dedicated High Contrast theme ships, font zoom spans 8–32pt, and `ImGuiConfigFlags_NavEnableKeyboard` is on. That is real, creditable work. But contrast and keyboard nav are only two WCAG pillars. The product has **no screen-reader surface at all** (no UIA/AT-SPI/aria/Narrator hooks anywhere in the UI tree), which is an architectural ceiling imposed by ImGui that no amount of palette polish overcomes.

**Overall verdict: a power-user instrument with above-average-for-ImGui craft, gated by toolkit-imposed accessibility and native-feel ceilings.** A keyboard-first engineer working a tracker all day will find it fast and learnable; a low-vision user relying on a screen reader cannot use it at all. **Overall UX maturity: 6.0 / 10.**

## 2. Scope & Method

I could not run the binary (no build in this environment), so all findings are reasoned from UI source under `Source/Core/src/Ui/**`, the theme palettes in `SmatchetTheme.cpp`, the localization table (`SmatchetLocalization.cpp`), the keyboard-shortcuts guide (`docs/guides/keyboard-shortcuts.md`), the bundled golden screenshots (`tests/golden/command-palette-fuzzy.png` proved especially load-bearing — it shows the real shell), and README/CLI docs. Where I assert a contrast ratio I rely on the values the code itself computes and pins.

As instructed, I **ignored the agentic-governance meta-layer entirely**: I did not open or factor `AGENTS.md` / `Source/Core/src/Ui/AGENTS.md`, the `agents/` directory, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`, `.coderabbit.yaml`, or `.cursor/`. This is a pure product-UX read.

## 3. Toolkit & Visual Design (Dear ImGui Implications)

**Honest pros.** ImGui buys Smatchet instant theming, frame-perfect responsiveness, a unified retained-immediate widget vocabulary, and trivial docking (`BeginViewportSideBar` is used for the omnibar, status bar, and panels). The team has invested to lift it above debug-UI defaults: `ApplyCommonStyle` sets consistent rounding (Window 6, Frame 4, Tab 4) and humane spacing (`WindowPadding 10×10`, `ItemSpacing 8×6`, `FramePadding 6×4`), and six full palettes ship — SmatchetDark, ModernDark, VS2022 Dark/Light, High Contrast, and a Norton Commander pastiche — each defining all ~50 `ImGuiCol_` slots plus matched syntax-highlight and AI-bubble sub-palettes. That is far more visual discipline than typical ImGui projects show.

**Toolkit-imposed cons (not fixable without leaving ImGui).**
- **Density and "debug-UI heritage."** The golden screenshot (`command-palette-fuzzy.png`) confirms the daily-driver concern: rows are tight, the grid runs ~13 columns edge-to-edge, and chrome (menu bar + toolbar + status bar) is functional but flat. It reads as a developer tool, not a polished SaaS client. The `Density` submenu (Compact/Normal/Comfortable) partially mitigates this, which is a thoughtful concession.
- **Text input.** ImGui's `InputText` lacks native niceties — no OS spell-check, no right-click "Look up", limited IME, no native undo stack, no drag-select-to-OS-clipboard semantics matching the platform. For a tracker where users write issue descriptions and AI prompts, this is a persistent low-grade friction.
- **Typography.** Fonts are resolved from **Windows system paths** (`C:\Windows\Fonts\segoeui.ttf`, `consola.ttf`, etc. in `SmatchetImGuiFonts.cpp`) rather than bundled — so the "Roboto" daily-driver feel is not guaranteed cross-platform, and `assets/fonts/` contains only a README. Font Awesome icons merge cleanly via a dedicated glyph range, and the omnibar correctly **falls back to text when an FA glyph is absent** (`OmnibarModeGlyph` comment) — good defensive design.
- **Native feel.** No OS-native menus, no platform file dialogs implied, no native window chrome. Acceptable for a cross-engine (also embeds in Unreal) tool; suboptimal as a standalone consumer app.

**Visual contrast craft is a standout.** The accent color was deliberately darkened from `(0.35,0.55,0.95)` to `(0.26,0.42,0.72)` with the reasoning pinned in-comment: white-on-fill moves from 2.90:1 (fail) to 4.67:1 (passes AA-normal) while keeping accent-on-WindowBg at 3.16:1 (clears the 3.0 UI-component floor). ModernDark needed its *own* shade `(0.29,0.42,0.62)` because its dimmer Text(0.92) landed the shared shade at 4.45:1. This is exactly the right way to reason about color, and it is rare.

## 4. Information Architecture & Navigation

**The Views system is the IA backbone and it is coherent.** A view bundles a Filter (JQL), a Fields set, a Columns order, and a Sort spec, edited in a tabbed two-pane editor (`SmatchetViewsDashboardUi.cpp`: `BeginTabItem("Filter"/"Fields"/"Columns"/"Sort")`) with a left sidebar list supporting search, inline rename, duplicate, and delete via a hover-revealed context menu. Columns and Sort rows are **drag-reorderable with a keyboard fallback** (`viewsKeyboardReorderRow`, `HandleRowReorder`, plus drag-drop auto-scroll). The microcopy "— drag to reorder, click direction to toggle" is a small but real discoverability win. This is a learnable, conventional IA borrowed sensibly from Jira/Linear.

**Three distinct "search bars" risk a navigation-model collision.** There is (a) the **Command Palette** (`Ctrl+Shift+P`, `ui.command_palette`) — a fuzzy command launcher with an inline menu-bar entry that pre-filters the modal; (b) the **Omnibar** (`SmatchetOmnibarUi.cpp`, docked `ImGuiDir_Up`) — a *per-pane* JQL/ticket-key/title-search bar that classifies input and previews the Enter action ("Ticket key — Enter opens this issue"); and (c) the per-column grid **Filter** box. The palette is excellent (the golden screenshot shows clear `id  description` rows, a red destructive-command tint on `scenario.run`, and a persistent footer "Enter to run · Esc to close · Up/Down to navigate"). But two top-of-window text bars with different verbs is a discoverability and mode-confusion hazard (Nielsen #6, recognition vs recall) — a new user will not immediately know which bar the command lives in.

**The grid** is the dense data surface. It supports per-pane views, multi-pane layouts with a min-1-pane invariant, column sort/filter, and connectivity chips. The "Load" buttons per row (visible in the screenshot) suggest lazy ticket hydration — sensible for large result sets but visually noisy as a repeated control.

## 5. Interaction Design

**The "unsaved layout changes" explicit-commit pattern is the right call — with one caveat.** Edits set `viewsDirty`, an inline amber `"  unsaved"` label appears (`TextColored(0.95,0.75,0.20)`), Discard is disabled until dirty, and switching views while dirty **latches a discard-confirm modal** rather than silently losing work (`viewsShowDiscardConfirm`). This respects Nielsen #5 (error prevention) and #3 (user control). It is the correct model for a destructive, hard-to-reconstruct artifact like a saved query. **Caveat:** explicit-commit adds a step for the common case of a quick filter tweak; the omnibar's *immediate* Enter-applies-query path is the escape hatch, but the dual model (commit-based in the editor, immediate in the omnibar) is itself a slight inconsistency users must learn.

**Keyboard-first design is a genuine strength.** Every shortcut is rebindable and maps to the same command id the palette/CLI/MCP/Lua dispatch — a single source of truth that means a rebind surfaces *everywhere* the command appears (menus show the combo on the right, toolbar tooltips, palette rows) "the next frame — no restart." Conflicts are **warned, not blocked**, with deliberate last-pressed-wins semantics so a user can move a combo in two steps. A read-only "System shortcuts" section honestly documents the few non-rebindable modal sequences (`Ctrl+M,Z` for Zen; `Esc Esc` to exit). This is mature shortcut UX.

**The AI side panel (`Ctrl+Shift+A`) is the most ambitious surface.** It streams (a live "Assistant (streaming...):" tail bypasses the per-message markdown cache), supports **pinned bookmarks** (a pin strip above history, capped at 4 visible rows so pinning 50 messages doesn't eat the panel), **per-turn model + reasoning-effort overrides** (`<default model>` / `<default effort>` sentinels — good explicit-default microcopy), and **five per-block context toggles** (Selection / Visible / Ticket / View / Audit) that persist immediately. Critically, a **first-send outbound-context consent modal** measures and shows exactly what will be transmitted before anything leaves the machine — a strong privacy-affordance and a trust win (Nielsen #1, visibility of system status). The accompanying line "This happens on every message. Toggle individual blocks with the context checkboxes" is good just-in-time guidance.

**Dictation** (Whisper) splices transcribed text into the input buffer and supports auto-send-on-punctuation. The cross-thread hand-off is carefully engineered; from a UX view the risk is **feedback**: a user dictating needs an unmistakable "listening / transcribing" indicator, and an auto-send trigger needs a visible countdown or undo, or it will surprise people. I could not confirm the listening overlay's prominence from source alone (`SmatchetWhisperOverlayUi.cpp` exists, which is encouraging).

## 6. Accessibility Audit (WCAG-framed)

This is the decisive axis, and Smatchet is split: strong on two pillars, absent on the most important one.

**Contrast (WCAG 1.4.3 / 1.4.11) — PASS, with evidence.** The theme code computes and pins ratios against both the 4.5:1 AA-normal-text floor and the 3.0:1 non-text UI-component floor, and chose hue-faithful accent shades that clear both per theme. The **High Contrast** theme (pure black bg, pure white text, cyan accent) targets AAA (>7:1) and even bumps AI-bubble alpha to 0.35 so the bubble reads as a surface on black. This is best-in-class for an ImGui app. (One residual risk: the **Norton Commander** theme uses cyan text on `#0000AA` blue panels and yellow-on-gray dialogs — its own comments admit "less ideally" legibility; it should be framed as a novelty, never the default, and arguably excluded from any "accessible themes" claim.)

**Keyboard operability (WCAG 2.1.1) — PARTIAL PASS.** `ImGuiConfigFlags_NavEnableKeyboard` is enabled and `NavHighlight` is themed, so widgets are reachable. The command palette is fully keyboard-driven. Views reorder has a keyboard path. **But** ImGui's keyboard nav is non-standard relative to OS expectations (Tab order, arrow semantics, and focus rings differ from native), and complex surfaces (multi-pane grid, drag-drop) lean on mouse interaction as the primary path. There is no documented "keyboard map" of focus traversal for the grid.

**Font scaling (WCAG 1.4.4) — PASS.** `FontSizePt` spans 8–32pt (default 16; `SmatchetDefaults.h`), exposed via menu zoom controls and applied two ways: a cheap per-frame `FontGlobalScale = FontSizePt/16` for instant zoom, plus a real atlas reload for crisp glyphs. The status bar even shows the live `"%dpt"`. This comfortably exceeds the 200%-resize requirement.

**Screen readers (WCAG 1.1.1, 4.1.2) — FAIL (architectural).** A grep across the entire UI tree finds **no UIA, AT-SPI, aria, Narrator, or any accessibility-API integration whatsoever.** ImGui renders everything as textured triangles with no semantic tree, so to a screen reader Smatchet is an opaque rectangle: no roles, no names, no states, no announcements. There is no text alternative for the Font Awesome icon-only controls beyond hover tooltips (which a screen reader cannot reach). **This makes the product unusable for blind and many low-vision users and is the single largest UX/accessibility liability.** It is honest to call it toolkit-imposed — but it is still a hard ceiling, and any accessibility claim must be scoped to "low-vision via contrast + zoom," never "screen-reader accessible."

**Net WCAG posture:** strong on 1.4.3 / 1.4.4 / 1.4.11, partial on 2.1.1, failing on 1.1.1 / 4.1.2.

## 7. Localization & Microcopy

**Localization is solid for a v1.** English/French ship via a compiled `kEntries[]` table (~512 entries) keyed by stable ids, with French strings as proper `u8"…"` UTF-8 including accents — and the font glyph atlas explicitly loads the **Latin-1 Supplement range** plus Greek/Cyrillic/Vietnamese, so accented French renders. The fallback chain is sound: a runtime `Locales/<lang>.json` **override file** can patch any string by key or by English source, with a guard that **rejects an override file whose declared `locale` doesn't match the active language** (logged as a warning) — preventing the classic "wrong-language overrides leak in" bug. Missing keys fall back to the English source, never to a raw key (`TranslateEntryLocked`). The `LabelFromSource`/`###`-id splitting correctly preserves ImGui's hidden-id suffixes while translating the visible portion. The font and theme apply "instantly."

**Microcopy is mostly clear and occasionally excellent**, e.g. the omnibar Enter-preview hints, the consent line, and the offline-queue dead-letter tooltip ("This queued create was missing a project after the project-key migration. Restore and pick a project to retry."). **Weaknesses:** the status bar leans on terse single words ("online" / "offline" / "auth error" / "unavailable" / "unknown") with no inline remedy; some labels still expose jargon ("JQL", "reasoning_effort" surfaced verbatim in a tooltip) that will read as developer-speak to a non-technical PM. French coverage is partial by construction — only app-owned chrome is translated; tracker data and many dynamic strings remain English.

## 8. Discoverability, Onboarding & Feedback States

**Feedback states are a relative strength.** The golden screenshot shows live status chips ("TRACKER OK", "MCP LIVE: 58756") and a non-blocking **toast** ("Syncing / Refreshing issues from Tracker…") — good Nielsen #1 visibility. The status bar carries a backend chip, a connectivity word, a `"%d queued"` offline-ops counter, an in-flight edit dot, and the font/theme/fps readout. Tracker connectivity drives a banner with Error/Warning levels and read-only-mode enforcement, with grid edits explicitly disabled-with-explanation when the tracker is unreachable ("Grid edits and quick comment actions stay disabled until Tracker is reachable") — a clear, honest degraded state.

**Onboarding is the weak spot.** There is **no first-run/welcome/setup flow** in the UI source (grep finds no "Get started", "Welcome", "first run", or wizard). A new user lands in an empty grid and must discover the View dropdown, the palette, and connectivity setup unaided. **Empty states are largely absent**: the code handles "no views in the bucket" by falling back to the active view internally, but I found no user-facing "You have no views yet — create one" guidance. Discoverability of the deep feature set (multi-pane, dictation, Lua actions, MCP) rests almost entirely on the command palette and menus; there is no in-app tour, no contextual coachmarks, and the README has a Localization section but no "Quick Start." The `SmatchetHelpMarker` `(?)` pattern is used well in Preferences, but it's pull, not push.

## 9. Scorecard

| Dimension | Score /10 | Rationale |
|---|---|---|
| Visual design | 6.5 | Disciplined palettes + computed contrast lift it above ImGui norms; still dense, flat, debug-tool-feeling; fonts not bundled. |
| Information architecture | 7.0 | Views system is coherent and learnable; three competing search bars muddy the model. |
| Interaction design | 7.5 | Explicit-commit Views, single-source rebindable commands, ambitious AI panel with consent gate — genuinely strong. |
| Accessibility | 4.0 | Excellent contrast + zoom + High Contrast theme, keyboard nav on; but **zero screen-reader support** caps it hard. |
| Localization | 7.5 | Clean en/fr table, robust override + locale-guard + English fallback, accented glyphs covered. |
| Discoverability / onboarding | 4.5 | Palette + menus + tooltips are good for power users; no onboarding, no empty-state guidance, no tour. |
| Consistency | 6.5 | Strong command/shortcut consistency; dual commit-vs-immediate apply model and triple search bars are inconsistencies. |
| **Overall UX maturity** | **6.0** | A polished-for-ImGui power instrument with a hard accessibility ceiling and a thin first-run experience. |

## 10. Prioritized UX Recommendations

**P0 — Accessibility honesty + the achievable wins.**
1. **Never claim "accessible" unscoped.** Document the real posture: "low-vision support via High Contrast theme + 8–32pt zoom + AA contrast; screen-reader support is not available." This is an integrity issue, not just polish.
2. **Investigate a minimal AT bridge.** Full UIA over ImGui is hard, but a parallel off-screen semantic mirror for the most critical flows (grid rows, palette, primary actions) — or shipping the existing CLI/MCP as the documented accessible path — would move the product from "unusable" to "operable by alternative means" for blind users.
3. **Give every icon-only control a real text label option** (not just a hover tooltip), and ensure Density/zoom defaults don't clip text.

**P1 — Onboarding & empty states (cheap, high-impact).**
4. Add a **first-run welcome / connect-your-tracker step** and a one-time "here are the 3 things to know" coachmark (palette, Views, AI panel).
5. Add **empty-state copy** to the grid ("No issues match this view — adjust the filter or create one") and the Views list ("No views yet — New View to start").

**P1 — Resolve the search-bar collision.**
6. Visually and verbally differentiate the **Command Palette** (commands) from the **Omnibar** (queries/tickets) — distinct placeholders, distinct iconography, and a one-line hint on first use of each. Consider whether both need to live at the top of the window.

**P2 — Microcopy & feedback.**
7. Make terse status words actionable: "offline" → hover/click reveals "Tracker unreachable — Retry / Check settings."
8. De-jargon user-facing tooltips (surface "reasoning effort" plainly; gloss "JQL" on first encounter).
9. Ensure **dictation** has an unmistakable listening indicator and an undo/countdown for auto-send-on-punctuation.

**P2 — Visual polish.**
10. Bundle the intended UI font cross-platform so the daily-driver typography is guaranteed; soften grid density defaults; consider de-emphasizing per-row "Load" buttons (auto-hydrate on scroll, or a single batch control). Demote the Norton Commander theme to an explicit novelty, excluded from any accessibility framing.
