# Golden-image approval contract

Stub in [`AGENTS.md`](../../AGENTS.md) § Project rules § Golden-image approval contract points here.

Any agent that writes or regenerates a checked-in reference artefact a regression gate diffs against — `tests/golden/*.png`, JSON snapshots, deterministic byte streams — MUST treat the bootstrap as a UI-tuning-equivalent change. The visual-validation exception applies.

## Theme rule — goldens use the DEFAULT ImGui style (user-mandated 2026-06-07)

Every golden image / screenshot capture runs under the **stock ImGui style**
(`ImGui::StyleColorsDark()`), never `SmatchetTheme::ApplyStyle`. Rationale: the
custom theme is retuned frequently (the trivial-visual envelope exists exactly
because palette changes are cheap and common) — a golden pinned to it is
invalidated by every retune, turning theme work into golden-churn. Stock style
is stable upstream, so goldens only change when the UNDER-TEST surface changes.
Mechanically: capture paths opt out of the theme at bootstrap via the
test-driver env knob (`SMATCHET_TEST_DEFAULT_IMGUI_THEME=1`; introduced with
multi-grid Slice 2 — the knob's call site is beside `ApplyStyle`). Theme
correctness itself is covered by the dual-capture-no-golden pattern below, not
by goldens. Existing goldens captured under the custom theme are recaptured
under default style on their next touch (the bucket-C lane was dead-but-green
until 2026-06-07 — postmortems.md — so pre-existing goldens were unverified
anyway).

## Recipe

1. **Build** the change that produces the artefact.
2. **Hand the artefact path + the launched-app handle to the user** showing the captured state. For PNGs, `SendUserFile` the file with a one-line caption naming the scenario + what the state is supposed to represent (e.g. "dark theme post-switch, no residual colors").
3. **Wait for an explicit "looks right" / "approve golden" verdict** before any `git add tests/golden/<file>` + commit.
4. **On rejection**: `git checkout -- tests/golden/<file>` and iterate the underlying fix BEFORE re-bootstrapping. **Never amend the golden to match a buggy state** — that's the exact failure mode this rule exists to prevent.

## Motivating incident

2026-05-19 — `tests/golden/theme-switch-roundtrip.png` was bootstrapped while the theme-switch-residual-color bug was active. The PNG captured the broken post-round-trip state; the diff gate would have certified the bug as "expected behaviour" forever. Same trap waited on `dock-gap-sentinel` and `command-palette-fuzzy` goldens. Caught only because the user opened the PNG by hand and said "this is the result of the bug".

## Preferred shape — dual-capture-no-golden

When both states are produced at runtime within the same test (see `scripts/dev/test-theme-roundtrip.sh`), there is no checked-in artefact to enshrine. The failure mode is structurally impossible. Prefer this pattern when feasible.

## Agents that own this

- `test-author` — primary executor; sub-bullet under § Pattern C — Screenshot scan in [`agents/core/test-author.md`](../../agents/core/test-author.md).
- Any other agent that ships a regression-gate artefact (e.g. `debug-detective` shipping a pink-clear sentinel image, `perf-detective` shipping a deterministic perf-snapshot fixture).
