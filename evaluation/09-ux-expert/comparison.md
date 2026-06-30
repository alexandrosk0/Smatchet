# Smatchet UX Evaluation — Comparison & Critic Report (Expert 09)

**Subject:** One senior UX / product-design expert, two passes over the same project.
**Pass A (without agents):** pure product-UI read — theme, locales, palette, shortcuts, golden screenshots. **6.0/10.**
**Pass B (with agents):** same UX expert, additionally factoring how UX quality is *governed* — Quality Pillars, visual-validation human gate, golden-image approval, bucket-C/E tests. **7.4/10.**
**Meta-analyst date:** 2026-06-30

---

## 1. Executive Summary

The two passes agree almost completely on what the *product* is — a fast, dense, keyboard-first Dear ImGui issue-tracker with above-ImGui-norm interaction craft, best-in-class computed contrast, and a hard screen-reader ceiling. What changes between them is not the artifact but the *epistemic frame*: Pass B can see how the project keeps its UX from rotting, and that view materially raises the grade.

The single biggest way the meta-layer changed the assessment: it revealed that **responsiveness and no-freeze are enforced merge invariants, not aspirations.** Pass A could observe ImGui's "frame-perfect responsiveness" as a toolkit property; only Pass B could see that a 6.94 ms/144 Hz frame budget and a "UI never blocks >100 ms without a cue" rule *auto-fail PRs*, owned by `perf-detective`, `spike-hunter`, and `code-review`, with any synchronous I/O reachable from an ImGui frame treated as a code-review CRITICAL. Pass B calls this "the strongest UX-governance story I've seen in a codebase" — a rare, genuine UX-governance win, because latency and freezes are the №1 felt-quality killers in a desktop app and here they are un-mergeable.

The second, subtler reveal is that **accessibility is actually *more* enforced than the docs claim.** Pillar 4 self-describes as "aspirational, no auto-fail gates today," yet Pass B found two merge-blocking accessibility tests: a WCAG-AA contrast doctest (`SmatchetThemeAccentContrast.test.cpp`, with regression guards proving the *old* accent fails 4.5:1) and a bucket-E keyboard-focus regression test (`help_marker_keyboard_focus.test.cpp`, gating Issue #1128, where ~38 hover-only tooltips were invisible to keyboard nav). Both directly contradict Pillar 4's "no gates today" framing. The accessibility story is therefore not "nothing enforced" but "enforcement is real yet incident-driven and uneven."

---

## 2. Score Delta: 6.0 → 7.4 (+1.4)

This is a notably large jump — larger than most paired UX re-reads, where the meta-layer typically adds polish-level credit. The +1.4 is concentrated in two places the dimension table makes explicit:

- A **brand-new dimension** appears in Pass B: *UX-quality governance* scores **8/10**. This is pure upside the without-pass structurally could not award — it didn't know the governance existed.
- **Accessibility rises 4.0 → 5.5.** Not because the product changed, but because Pass B discovered the contrast doctest and the keyboard-focus bucket-E test are *gated*, converting "good contrast in source comments" (Pass A's read) into "good contrast enforced at merge."

Information architecture (7.0 → 8) and interaction design (7.5 → 8) also tick up, because the same patterns Pass A praised on inspection (explicit-commit Views, AI consent gate, feedback states) turn out in Pass B to be *bucket-E-tested behavior* — `ai_assistant_preferences_save_discard.test.cpp`, `sync_stall_visible_cue.test.cpp`, `reset_layout_docking.test.cpp` — not just code that looks right.

The *why* is the core insight of this comparison: **the measurable slice of UX — latency, frame budget, freezes, contrast ratios, keyboard reachability, pixel diffs — is enforced at merge, and a pure-UI audit cannot see enforcement.** Pass A can confirm the accent is 4.67:1 *today*; only Pass B can confirm a regression below 4.5:1 *can't ship*. That distinction de-risks UX regression materially, and a UX leader rationally pays for de-risking. The +1.4 is mostly the price of that confidence, plus the discovered accessibility enforcement.

---

## 3. What Pass B Saw That Pass A Was Blind To

1. **Responsiveness/no-freeze as auto-failing invariants.** Pillars 1–2: the 6.94 ms steady-state budget, 10 ms p99 floor, `perf-compare.py` exiting non-zero on regression with per-host baselines; the Pillar-2 rule routing any synchronous HTTP/SQLite/p4/file-I/O/mutex call through `MainThreadDispatcher` on pain of a code-review CRITICAL, with `.coderabbit.yaml` path rules enforcing it. Pass A saw "frame-perfect responsiveness" as a free ImGui gift; Pass B saw it as a defended invariant.

2. **The gated contrast + keyboard-focus tests.** Pass A read the contrast arithmetic as *pinned in code comments* (creditable but inert); Pass B identified it as a *doctest that blocks merges* with regression guards, plus `ui-host.md` codifying "new palette tokens get a doctest CHECK." Likewise the #1128 keyboard-focus fix (glyph over `InvisibleButton` + `ImGuiButtonFlags_EnableNav`, tooltip on `IsItemHovered() || IsItemFocused()`) is a *tested* regression guard, not just observed behavior.

3. **The visual-validation human gate + golden-image approval discipline.** The ship-loop pauses the autonomous merge loop whenever a diff touches `SmatchetTheme.cpp`, ImGui style literals, dock-init, or `Locales/*.json` *and* no bucket-C/E test covers it — auto-launching the exe for a single human verdict. The golden contract forbids amending a golden to match a buggy state (born from the 2026-05-19 broken-theme-switch incident) and prefers "dual-capture-no-golden." Pass A had no way to know pixels were human-gated.

4. **That NO agent owns design/UX.** Pass B's §6 is the sharpest thing neither pass could have written from the UI alone: `ui-host` owns the theme *code* (compilation, dock migration, contrast doctest per token) — its charter is explicitly "the layer *below* the panels," not "whether the design is good." Per-panel content UX is scattered across grid-engine/orchestrator specialists with no unifying interaction-design authority; the human is the de-facto design owner via the visual gate. This reframes the whole governance story: it covers the measurable substrate and is silent on design judgment.

---

## 4. What Pass A Got Right That Survived Both Passes

The without-pass's core findings are not overturned by governance — they are *interaction-craft and accessibility gaps that no gate touches*:

- **The decisive screen-reader / AT-API FAIL.** Both passes land identically: ImGui renders textured triangles with no semantic tree, no UIA/AT-SPI/aria/Narrator hooks anywhere, so the product is "unusable for blind and many low-vision users" (Pass A) / "a non-starter for screen-reader users" (Pass B). Pass B adds governance color — the Pillar-4 debt entry candidly scopes an `AccessibilityNodeProvider` bridge as "a multi-week slice" and ships "documented non-support" first — but the verdict (WCAG 1.1.1 / 4.1.2 FAIL, architectural) is unchanged. Governance cannot fix a toolkit ceiling.

- **No onboarding / empty-states.** Pass A: no first-run/welcome/wizard, empty grid with no "you have no views yet" guidance. Pass B independently greps `onboard|firstRun|Welcome|tutorial|EmptyState` and surfaces "only a font file," calling onboarding "the single highest-leverage UX gap" and noting "nothing in the governance layer owns or gates onboarding quality." Both score discoverability/onboarding ~4.5–5.0.

- **Search-surface mode-confusion.** Pass A's three-competing-search-surfaces finding (Command Palette vs per-pane Omnibar vs per-column grid Filter — two top-of-window text bars with different verbs, a Nielsen #6 recognition-vs-recall hazard) survives; Pass B doesn't refute it and reframes the palette's invisibility-behind-a-shortcut as the same discoverability tax. Governance never addresses which bar a command lives in.

These three are exactly the *judgment* dimensions of UX — learnability, discoverability, assistive-tech reach — and both passes agree the governance machine has nothing to say about them.

---

## 5. Contradictions & Tensions

The central asymmetry, named by Pass B itself: **responsiveness is governed prospectively and systematically; accessibility and interaction-craft are governed reactively, locally, or not at all.** The project can auto-fail a PR that adds 0.5 ms to a frame, but cannot auto-fail a PR that ships a 2.9:1 button in an untested theme or a keyboard-unreachable control in a surface no one thought to pin. Contrast is pinned theme-by-theme only after the mobile P1.6 incident; keyboard nav is pinned widget-by-widget only after #1128; the four non-default themes (VS2022 Light, High Contrast, Norton) aren't proven AA across every text-on-surface pair, and the Pillar-4 debt note even records disabled-grey `0.50` at 4.15:1 — *failing AA-normal* — on the mobile accent path.

Does enforced perf make this "good UX"? Partly, and honestly so — Pass B refuses to over-claim, stating "UX" in the pillar names is really "performance + crash-safety + (reactive) contrast/keyboard," while palette ergonomics, empty-state guidance, onboarding, and discoverability are "governed by *nobody*." That is the right boundary to draw. But it surfaces the load-bearing critic's question: **did Pass B over-reward the project for governing the easy-to-measure slice?**

There is a real risk it did. The +1.4 leans heavily on the 8/10 governance dimension and the accessibility bump — both of which credit *enforcement of measurable things*. Milliseconds, contrast ratios, and pixel diffs are gateable precisely because they are cheap to compute; learnability and screen-reader reach are ungated precisely because they are expensive to judge. A governance system that hardens what's cheap and defers what's hard will *always* look impressive to a measurement-oriented audit, even if the user's hardest problems (a blind user can't open it; a new user is stranded in a dense docked workspace) are untouched. Pass B's own §5.3 concedes a theme-wide AA sweep is "a few hours of the same arithmetic already in `SmatchetThemeAccentContrast.test.cpp`" — meaning even the *measurable* a11y slice is only partly gated, and the project gets governance credit for an incomplete sweep. The tension is genuine: enforced perf is a real UX win, but it is also the slice that flatters the scoreboard.

A secondary tension: Pass B credits the explicit-commit and feedback patterns more highly *because they're bucket-E-tested*. But a test proving "the save/discard dirty model behaves" certifies *correctness of the implemented pattern*, not *that the pattern is the right UX*. Governance can confirm the thing works as built; it cannot confirm the thing should have been built that way. Pass B mostly respects this line, but the IA/interaction bumps (7.0→8, 7.5→8) ride partly on "it's tested," which is a correctness signal masquerading slightly as a usability signal.

---

## 6. Critic's Verdict

**Which pass is more useful for a UX leader?** Pass B, decisively — but with a caveat. A UX leader inheriting this product needs to know not just *what the UI is today* but *what will silently regress tomorrow*, and only Pass B answers that. Knowing that perf and no-freeze are un-mergeable, that theme/locale changes are human-gated, and — critically — that *no agent owns design or the a11y backlog* is exactly the org-design intelligence a UX leader acts on. Pass A, read alone, would let a leader believe the contrast work is "just comments" and miss that it's enforced, while also missing that onboarding has no owner. Pass B is the strategic document.

But Pass A is not redundant; it is the *honest baseline*. Its scorecard is the pure-product truth that governance can't inflate: 4.0 accessibility, 4.5 onboarding, a hard screen-reader ceiling. A UX leader should read Pass A to calibrate skepticism *before* reading Pass B, precisely so the governance glow doesn't obscure that the product is unusable for blind users and unfriendly to newcomers.

**Is "UX as enforced invariant" genuine maturity or a measurement-bias trap?** It is genuine *for the felt-quality dimensions perf protects* — making latency un-mergeable is real, rare, high-value UX engineering, and Pass B is right not to discount it. But the framing is a measurement-bias trap *if read as a claim about UX maturity overall*. A product can have the best responsiveness governance in existence and still strand a blind user and a first-time user — both of which this product does. Maturity in *governing the measurable* is not maturity in *UX*; it is maturity in *the slice of UX that submits to a number*. The honest reading is Pass B's own: best-in-class for the measurable slice, silent on the judgment slice.

**Critique of the reports themselves.** Pass A is rigorous and self-aware about its scope limit, but it slightly *under-credits* the contrast work by reading it as inert comments — a defensible call given its instructions, but it means its 4.0 accessibility is too harsh on the enforced sub-axes. Pass B is the stronger report: it actively *contradicts the project's own docs* (the "no gates today" claim) with file-level evidence, and its §5.3/§6 asymmetry analysis is the sharpest content in either pass. Its weakness is the one named above — the +1.4 partly rewards governance that covers the cheap slice, and the IA/interaction bumps lean on "it's tested" as if that were a usability proof. Pass B catches this risk in prose but doesn't fully discount its own score for it. Neither report is wrong; Pass B is more useful and Pass A is more skeptical, and a leader needs both lenses.

---

## 7. Synthesis & Blended Verdict

The combined UX bottom line: **Smatchet is a fast, dense, keyboard-respecting power-tool whose *measurable* UX quality — responsiveness, no-freeze, contrast, keyboard reachability, pixel fidelity — is enforced at merge to a degree almost no product achieves, and whose *judgment* UX quality — onboarding, empty-states, search-surface clarity, and above all assistive-technology reach — is ungoverned, unowned, and in the screen-reader case structurally impossible without leaving ImGui.** The governance lens earns the project real, defensible credit for de-risking felt quality; it does not earn credit it sometimes implicitly takes for "UX" writ large, because the hardest user problems sit entirely outside what any gate measures.

Pass A (6.0) is the honest product floor; Pass B (7.4) is the governance-aware ceiling. The +1.4 is justified in part (responsiveness-as-invariant and discovered a11y enforcement are real) and inflated in part (it over-rewards the measurable slice while the screen-reader FAIL and onboarding void persist). Discounting the governance dimension modestly for measurement bias, and holding firm on the two unfixed P0s both passes agree on, the blended figure lands just below Pass B:

**Blended UX maturity: 7.0 / 10.**

A genuinely well-engineered power instrument with rare, real responsiveness governance and honestly-documented structural blind spots — held back, in any user-centered accounting, by an accessibility ceiling and an onboarding void that no amount of enforced milliseconds can close.
