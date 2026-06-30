# Smatchet UX Expert Evaluation — WITH agents.md

**Evaluator lens:** Senior UX / product-design expert (interaction design, usability, accessibility). Nielsen heuristics + WCAG 2.x.
**Scope flag:** PASS — product UX **plus** the agentic-governance meta-layer (`AGENTS.md`, per-subsystem `AGENTS.md`, the Quality Pillars, ship-loops visual-validation, golden-image approval, bucket-C/E tests, `.coderabbit.yaml`, `docs/self-improvement/**`).
**Date:** 2026-06-30

---

## 1. Executive Summary + Verdict

Smatchet is a Dear ImGui desktop issue-tracker with the production polish of a tool built by people who feel their own latency. The interaction model is keyboard-first and command-driven (Ctrl+Shift+P palette over a registry, an omnibar, a streaming AI side panel, dictation), the visual layer ships six hand-tuned themes including a dedicated High-Contrast palette, and — the headline finding — **UX responsiveness is not a wish but an enforced merge invariant**: a 6.94 ms/144 Hz frame budget and a "UI never blocks >100 ms without a cue" rule that auto-fail PRs. That is a genuinely unusual and admirable posture: most products treat perf and no-freeze as aspirations; here they are gates with owning agents (`perf-detective`, `spike-hunter`, `code-review`).

The governance layer's self-description undersells what is actually shipped. Pillar 4 (Accessibility) is flagged "aspirational, no auto-fail gates today" — yet the codebase contains a **WCAG-AA contrast doctest that blocks merges** (`SmatchetThemeAccentContrast.test.cpp`, with regression guards proving the old accent fails 4.5:1) and a **bucket-E keyboard-accessibility regression test** (`help_marker_keyboard_focus.test.cpp`, gating Issue #1128). So accessibility is *partly* enforced in exactly the two areas (contrast, keyboard-reachability) the pillar names — the "no gate" framing is stale. The real gap is not "nothing is enforced" but "enforcement is incident-driven and uneven": contrast is pinned theme-by-theme only after a bug; keyboard-nav is pinned widget-by-widget only after a regression; screen-reader support is explicitly deferred (ImGui has no a11y tree); and OS-level large-text scaling is unwired on mobile.

**Verdict: 7.4/10 — a fast, dense, keyboard-respecting power-tool with best-in-class *responsiveness* governance and better-than-advertised accessibility enforcement, held back by ImGui's structural a11y ceiling, incident-driven (not systematic) WCAG coverage, and the discoverability tax that every immediate-mode GUI pays.** The novel governance lens reveals a real strength (responsiveness as invariant is a true UX win) and a real, honestly-documented blind spot (no agent *owns* design/UX; accessibility hardens reactively).

---

## 2. Scope & Method

I read the product UI sources under `Source/Core/src/Ui/` (theme, command palette, AI assistant, omnibar, layout, help markers, localization) and the governance corpus: root + per-subsystem `AGENTS.md`, `docs/agent-rules/quality-pillars.md`, `ship-loops.md`, `golden-image-approval.md`, the bucket-C golden set + bucket-E UI tests under `tests/ui/` and `tests/golden/`, `.coderabbit.yaml`, and the `docs/self-improvement/**` backlog (notably the Pillar-4 debt entry). Heuristic frame: Nielsen's 10 usability heuristics for the interaction critique; WCAG 2.1 AA (1.4.3 contrast, 1.4.4 resize text, 2.1.1 keyboard, 2.4.7 focus visible, 4.1.2 name/role/value) for the accessibility audit. I separate (a) genuine product strengths, (b) immutable Dear ImGui toolkit limits, and (c) gaps the governance layer itself reveals.

---

## 3. Product UX — Toolkit / IA / Interaction / Visual

### 3.1 Dear ImGui as a daily-driver toolkit

ImGui is the foundational UX decision and it cuts both ways. **For:** retained snappiness (no DOM, no layout thrash), trivial density tuning, dockable multi-pane layouts that power users love, and a render loop so cheap the team can afford a 6.94 ms budget. **Against:** ImGui is an *immediate-mode debug-UI toolkit* repurposed as an application shell. It carries no native accessibility tree, no OS text-scaling integration, no platform widgets (native file dialogs, menus, IME edge cases), and its hover-driven affordances assume a mouse. The team is clearly aware: `SmatchetTheme.cpp` re-derives WCAG luminance by hand, `ApplyCommonStyle` sets sane rounding/padding (`FramePadding 6,4`, `ItemSpacing 8,6`) for a denser-than-default but not cramped feel, and `ImGuiConfigFlags_NavEnableKeyboard` is set at host init (`SmatchetImGuiHost.cpp:536`). This is ImGui driven about as far toward "native-feeling app" as the toolkit allows — but the native-feel ceiling is real and it lands hardest on accessibility (§4).

### 3.2 Information architecture & navigation

The IA is **command-centric**, which is the right call for a power-user tool. `Ctrl+Shift+P` opens a fuzzy command palette (`CommandPaletteUi.cpp`) backed by a command *registry* — every action is a named command, recents float to the top on an empty query, and fuzzy scoring ranks matches. This is the single best IA decision in the product: it makes the entire feature surface reachable and discoverable from one keystroke (Nielsen "flexibility & efficiency"; also the named keyboard entry-point in Pillar 4's locked scope). The omnibar (`SmatchetOmnibarUi.cpp`, `omnibar_search_apply.test.cpp`) and a dockable grid round out the navigation model. The two-pane Views editor + docking gives spatial flexibility, and a **Reset Layout** path (`reset_layout_docking.test.cpp`) is a critical escape hatch — docking UIs strand users in broken layouts without one. Risk: docking's flexibility is also a footgun (windows can be dragged shut or off-screen); the reset command is the mitigation and it is tested.

### 3.3 Interaction design

Strong, deliberate interaction patterns:

- **Explicit-commit discipline.** Preferences and layout mutations use a deferred/dirty model with explicit Save/Discard rather than silent live-apply — visible in `SmatchetUI_Layout.cpp` (`pendingLayoutReset`, final synchronous Save "if a Preferences mutation is still dirty") and the `ai_assistant_preferences_save_discard.test.cpp` bucket-E test. This respects Nielsen "user control & freedom" (reversible actions, no surprise persistence).
- **Streaming AI side panel** (`Ctrl+Shift+A`, `SmatchetAiAssistantUi.cpp`): token-streamed assistant turns with a per-frame-mutating streaming tail, pin-to-context toggles, a five-checkbox context-block row (`DrawContextBlockCheckboxes`), and — importantly — a **first-send outbound-context consent popup** before any context leaves the machine. That consent gate is a privacy-UX best practice many AI features skip.
- **Dictation** wired through a router (`DictationInsertionRouter`) that registers/unregisters active input buffers (the palette explicitly registers `filterBuf_` on open) — a coherent cross-cutting input modality, not a bolt-on.
- **Feedback states** are systematic: a toast manager (`SmatchetToast.cpp`), notification center, offline-queue UI with per-row status badges, and sync-stall cues. The `sync_stall_visible_cue.test.cpp` and `attachment_thumbnail_loading_cue.test.cpp` bucket-E tests prove the Pillar-2 "visible cue within 100 ms" contract is *tested behavior*, not just doc aspiration.

### 3.4 Visual design

`SmatchetTheme.cpp` ships six palettes (SmatchetDark default, ModernDark, VS2022 Dark/Light, High Contrast, Norton Commander) sharing one geometry layer (`ApplyCommonStyle`) so chrome stays consistent and only colors diverge — a clean separation that keeps theme work cheap and safe. The palettes are not vibes; they carry inline WCAG arithmetic. The default accent was deliberately darkened from `(0.35,0.55,0.95)` to `(0.26,0.42,0.72)` to clear *both* 4.5:1 white-on-fill (AA-normal) and 3.0:1 accent-on-dark (UI-component floor), and ModernDark needed its *own* darker shade `(0.29,0.42,0.62)` because its dimmer text landed the shared shade at 4.45:1 (<4.5). That is real, per-theme contrast engineering. Typography is single-font-atlas with user-controlled point size (§4.2). The one visual-design weakness is inherent to ImGui: limited typographic hierarchy (no rich font-weight/family mixing per run without atlas gymnastics), so emphasis leans on color, which constrains the design vocabulary.

---

## 4. Accessibility Audit (WCAG-framed)

This is where the "with-agents" lens most changes the score, because the codebase is **more accessible than the governance docs claim**, yet still structurally capped by ImGui.

**1.4.3 Contrast (Minimum) — PARTIAL PASS, and *gated*.** The default and ModernDark themes have machine-checked AA contrast: `SmatchetThemeAccentContrast.test.cpp` reimplements the WCAG sRGB→linear luminance transfer and contrast-ratio formula, asserts white-on-accent ≥4.5 and accent-on-bg ≥3.0, *and* includes regression guards that the old accents fail. `ui-host.md` codifies the rule: "New palette tokens get a doctest CHECK (WCAG AA contrast where the token sits on a known bg)." So contrast *is* an enforced gate for the tokens that have been pinned — directly contradicting Pillar 4's "no auto-fail gates today." **Gap:** coverage is token-by-token and theme-by-theme, added reactively after the mobile P1.6 incident; the four non-default themes (VS2022 Light, High Contrast, Norton) are not all proven AA across every text-on-surface pair, and body/disabled-text combos aren't systematically swept. The Pillar-4 debt note even records disabled-grey `0.50` at 4.15:1 (failing AA-normal) on the mobile accent path.

**1.4.4 Resize Text — PASS.** Font size is user-controlled 8–32 pt (default 16; `SmatchetDefaults.h`), applied per-frame via `ImGuiIO::FontGlobalScale` with no atlas rebuild (`SmatchetUI.cpp:401-405`), exposed as zoom commands with clamping (`BuildtinCommands_Ui.cpp AdjustFontSize`) and persisted in config. Effective 0.5×–2× range comfortably exceeds WCAG's 200% requirement. This is a clean, governance-named win (Pillar 4 "user-controlled FontGlobalScale, persisted").

**2.1.1 Keyboard — PARTIAL PASS, and *partly gated*.** `NavEnableKeyboard` is on; the command palette makes every registered command keyboard-reachable. The standout is `help_marker_keyboard_focus.test.cpp`: a bucket-E regression test for Issue #1128, where ~38 help tooltips were hover-only `TextUnformatted` (invisible to keyboard nav); the fix renders the glyph over an `InvisibleButton` with `ImGuiButtonFlags_EnableNav` and fires the tooltip on `IsItemHovered() || IsItemFocused()`. This is exactly the systematic keyboard-a11y work the pillar describes — and it is *tested*. **Gap:** like contrast, it's per-widget and incident-driven. There is no global "every actionable widget is keyboard-reachable" sweep or gate; the next hover-only affordance ships the same bug until someone files the next #1128.

**2.4.7 Focus Visible — PARTIAL.** `ImGuiCol_NavHighlight` carries the accent in every theme (and pure cyan in High Contrast), so a focus ring exists. ImGui's nav focus indicator is functional but visually subtle on dense surfaces; no governance check asserts focus-ring contrast.

**4.1.2 Name, Role, Value (screen readers) — FAIL, honestly deferred.** This is the hard ceiling. Dear ImGui renders to a GPU surface with no `AccessibilityNodeInfo`/UIA tree, so screen readers see one opaque element. The Pillar-4 debt entry is candid: TalkBack/NVDA support needs an `AccessibilityNodeProvider` virtual-view bridge — "a multi-week slice" — and ships "documented non-support" first. This is the correct engineering call (don't fake it) but it means Smatchet is effectively unusable for blind users today, and no roadmap commits a date.

**Net:** Smatchet clears the *visual* accessibility bar (contrast, text resize, keyboard reachability) better than its own docs advertise, and *cannot* clear the *assistive-tech* bar without leaving ImGui or building a bridge. The honest framing: AA-ish for sighted keyboard users, a non-starter for screen-reader users.

---

## 5. UX-as-Governed-Quality — The Novel Lens

This is the most interesting thing about Smatchet from a UX-leadership standpoint: **it treats interaction quality as a class of invariant normally reserved for correctness.**

### 5.1 Responsiveness and no-freeze as enforced invariants — a genuine UX win

Pillars 1–2 encode what UX teams usually only aspire to. The 6.94 ms steady-state frame budget (144 Hz) and 10 ms p99 floor are regression-gated by `perf-detective` and `spike-hunter` with per-host baselines and a delta gate (`perf-compare.py` exits non-zero on regression). Pillar 2 makes any synchronous HTTP/SQLite/p4/file-I/O/mutex call reachable from an ImGui frame a **code-review CRITICAL**, requires a worker-thread hand-off via `MainThreadDispatcher`, and mandates a visible cue within 100 ms for unavoidable blocks — with `.coderabbit.yaml` path rules and the `Ui/AGENTS.md` checklist enforcing it at review time, plus bucket-E cue tests (`sync_stall_visible_cue`) proving it at runtime. From a UX perspective this is exactly right: *latency and freezes are the №1 felt-quality killers in a desktop app*, and Smatchet has made them un-mergeable. This is the strongest UX-governance story I've seen in a codebase and it is not over-claimed — the tooling exists and runs.

### 5.2 Visual-validation human gate + golden-image approval — good discipline

The ship-loop's **Visual-validation exception** pauses the otherwise-autonomous merge loop whenever a diff touches `SmatchetTheme.cpp/.h`, ImGui style literals, dock-init paths, or `Locales/*.json` *and* no bucket-C/E test covers the change — auto-launching the built exe and asking the human a single verdict question before commit. The **golden-image approval contract** forbids amending a golden to match a buggy state (born from the 2026-05-19 incident where a broken theme-switch PNG would have certified the bug forever) and prefers a "dual-capture-no-golden" pattern that makes that failure structurally impossible. Goldens render under stock ImGui style, not the custom theme, so theme retunes don't churn them. This is mature visual-regression discipline: it correctly recognizes that *pixels are a UX surface a diff can't fully judge*, keeps a human in the loop for exactly the un-automatable cases, and has a postmortem-driven guardrail against the classic "enshrine the bug" trap.

### 5.3 The accessibility-deferral pattern — the real, but nuanced, blind spot

The honest critique: **responsiveness is enforced *systematically and prospectively*; accessibility is enforced *reactively and locally*.** Pillars 1–3 (and DRY) have owning agents and block merges by construction. Pillar 4 has **"none today"** as its owning agent and hardens only "once the supporting infra lands." In practice that means a11y enforcement exists (the contrast doctest, the keyboard-focus bucket-E test) but only as *point fixes filed after a bug*, never as a standing gate that catches the *next* contrast regression in an untested theme or the *next* hover-only widget. The asymmetry is telling: the project can auto-fail a PR that adds 0.5 ms to a frame, but cannot auto-fail a PR that ships a 2.9:1 button or a keyboard-unreachable control in a surface no one thought to pin. The Pillar-4 doc rationalizes this ("there is no automated check for keyboard-reachability or palette-wide WCAG today") — which is true but also a choice about where to invest. A contrast-sweep gate over *all* theme token-on-surface pairs is a few hours of the same arithmetic already in `SmatchetThemeAccentContrast.test.cpp`; the fact that it's still per-token is the blind spot in miniature.

**Is "UX as enforced invariant" genuine or does it over-index on perf?** Both. The perf/no-freeze invariants are genuine and high-value — they protect the felt quality every user experiences every frame. But "UX" in the pillar names is really "performance + crash-safety + (reactive) contrast/keyboard." The interaction-design and information-architecture quality — palette ergonomics, empty-state guidance, onboarding, discoverability — is governed by *nobody*: there's no heuristic-evaluation gate, no usability-test loop, no design owner. The governance over-indexes on the *measurable* dimensions of UX (milliseconds, contrast ratios, pixel diffs) and under-serves the *judgment* dimensions (is this flow learnable? is this affordance discoverable?). That's understandable — you gate what you can measure — but it's the honest boundary of the "UX as invariant" claim.

---

## 6. Does Anyone Own UX / Design?

**No design owner; one *code* owner of the visual substrate.** The `ui-host` agent (`agents/project/ui-host.md`) owns `SmatchetTheme.cpp`, the dockspace scaffold, font atlas, and host bootstrap — but its charter is explicitly *implementation*: "ImGui host / theme / docking / bootstrap specialist — the layer *below* the panels," model sonnet/effort low, with hard invariants about dual-target compilation, dock-migration ordering, and adding a doctest CHECK per palette token. It owns *that the theme compiles, migrates, and pins contrast* — not *whether the design is good*. Per-panel content UX is scattered across subsystem specialists (grid-engine, orchestrator) with no unifying interaction-design authority. The **human** is the de-facto design owner via the visual-validation pause and golden approval — which is appropriate (a human *should* judge pixels) but means design quality scales with one person's attention, not with an enforced standard. There is no agent whose job is "run a heuristic eval," "audit the new flow for discoverability," or "own the a11y backlog to closure." The closest thing to a UX-quality owner is the *aggregate* of perf agents + code-review's Pillar-2 sniff + the human visual gate — competent at the measurable slice, silent on the rest.

---

## 7. Localization & Microcopy

Localization is **real, in-binary, and surprisingly thorough**: `SmatchetLocalization.cpp` ships a static English↔French table of ~586 entries (`{key, English, French}`) covering common verbs, toasts, menus, whisper/dictation, offline-queue badges — with full French sentences for error states, not just button labels. The fallback chain is sound (`TranslateEntryLocked`): runtime JSON override (`Locales/<lang>.json`) → key match → English-string match → French (when `fr-FR`) → caller fallback → English → empty — so a missing French entry degrades to English rather than showing a raw key, and missing keys are tracked (`MissingKeysRef`). A `SmatchetLocalizedImGui` wrapper localizes most widget labels, with the command palette deliberately opting out (its labels are "its own translation surface"). The visual-validation exception explicitly fires on `Locales/*.json` changes — so localization is treated as a UX surface a human verifies, good. **Gaps:** only en/fr today (no RTL, no pluralization/gender rules beyond what hand-written entries encode), and French string length isn't governance-checked against layout (French runs ~20% longer; in a fixed-density ImGui grid, truncation/overflow is a real risk that no bucket-C golden currently sweeps in fr-FR).

---

## 8. Discoverability / Onboarding / Feedback

**Feedback: excellent** (§3.3) — toasts, notification center, offline badges, loading cues, all bucket-E-tested. This is the strongest of the three.

**Discoverability: mixed.** The command palette is a powerful discovery surface *if you know to press Ctrl+Shift+P* — a chicken-and-egg problem every command-driven tool has. `SmatchetHelpMarker` (`(?)` glyphs, ~13 call sites, now keyboard-reachable) provides contextual help, and the menu bar (`SmatchetUI_MainMenu.cpp`) gives a browsable fallback. But ImGui's hover-tooltip idiom means much guidance is invisible until you hover the right pixel — a discoverability tax, partly mitigated by #1128's focus-reachability fix.

**Onboarding: the weakest area.** I found no first-run/welcome/tutorial/empty-state guidance system (grep for `onboard|firstRun|Welcome|tutorial|EmptyState` surfaced only a font file). A new user lands in a dense docked workspace with no guided path to "connect a tracker → see issues → make an edit." For a tool whose power is gated behind a keyboard shortcut and a docking model, the absence of any onboarding is the single highest-leverage UX gap. Nothing in the governance layer owns or gates onboarding quality.

---

## 9. Scorecard

| Dimension | Score | Rationale |
|---|---:|---|
| Visual design | 7.5/10 | Six coherent themes, shared geometry, real per-theme contrast engineering; capped by ImGui's thin typographic hierarchy. |
| Information architecture | 8/10 | Command-registry + palette + omnibar + dockable panes is the right power-user IA; reset-layout escape hatch is tested. |
| Interaction design | 8/10 | Explicit-commit discipline, streaming AI w/ consent gate, dictation, systematic feedback states — all bucket-E-tested. |
| Accessibility | 5.5/10 | Contrast + text-resize + keyboard-reachability gated (better than docs claim); screen readers a structural FAIL; coverage reactive/uneven. |
| Localization | 7/10 | ~586-entry en/fr table, sound fallback chain, human-verified on change; only 2 langs, no RTL/length-overflow gate. |
| Discoverability / onboarding | 5/10 | Strong feedback + contextual help, but no onboarding/empty-state system; discovery gated behind a shortcut. |
| UX-quality governance | 8/10 | Responsiveness/no-freeze as enforced invariants + visual-validation human gate + golden discipline = best-in-class for the *measurable* slice; a11y enforced only reactively, no design owner. |
| **Overall** | **7.4/10** | Fast, dense, keyboard-respecting power-tool; exemplary responsiveness governance; honest, structurally-capped accessibility; missing onboarding. |

---

## 10. Prioritized UX Recommendations

1. **Ship an onboarding / empty-state layer (highest leverage, low cost).** A first-run guided path (connect tracker → see grid → make an edit) and per-pane empty states that name the relevant command + its shortcut. This directly attacks the worst gap and amplifies the existing command-palette IA. No toolkit limit blocks it.

2. **Promote contrast from per-token to a *theme-wide AA sweep gate*.** The luminance/contrast math already exists in `SmatchetThemeAccentContrast.test.cpp`; generalize it to iterate every (text/icon)-on-(surface) pair across **all six** themes (incl. VS2022 Light, High Contrast, Norton) and fail any pair below its WCAG floor. This converts Pillar 4's contrast clause from reactive to enforced — closing the asymmetry with Pillars 1–3 cheaply.

3. **Name an owning agent (or human role) for Pillar 4 and drive the backlog to closure.** "none today" is the tell. Even a low-effort `a11y-auditor` agent that (a) runs the contrast sweep, (b) greps new `Smatchet*Ui*.cpp` for hover-only affordances lacking a nav target (the #1128 pattern), and (c) keeps the mobile/Pillar-4 debt entry moving, would make accessibility a *standing* concern rather than an incident response.

4. **Make the command palette discoverable without the shortcut.** Surface a persistent, visible affordance (a search/command pill in the toolbar or status bar) that opens it on click, plus a one-time hint. The palette is the product's best feature and currently invisible to anyone who doesn't read the docs.

5. **Add an fr-FR layout-overflow golden sweep.** Capture key dense surfaces (grid header, preferences, toolbar) under `fr-FR` as bucket-C goldens to catch French-length truncation that the en-only goldens miss — extending the existing visual-validation discipline to the localization risk it doesn't yet cover.

6. **Scope the screen-reader bridge as a real, dated slice — or document the non-support prominently.** The `AccessibilityNodeProvider`/UIA virtual-tree is multi-week, but leaving it open-ended means it never ships. Either commit a milestone or state the non-support in user-facing docs so blind users aren't stranded silently.

7. **Add a focus-ring contrast assertion.** Extend the contrast gate to `ImGuiCol_NavHighlight`-on-surface so keyboard focus (WCAG 2.4.7) is provably visible on every theme, closing a quiet gap in the otherwise-solid keyboard story.
