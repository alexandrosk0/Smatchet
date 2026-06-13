# Plan — cell-edit-burst p99 verify-on-real-HW (then conditionally optimize) (#973)

> **Slug**: `cell-edit-burst-p99-verify` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

GitHub Issue **#973** (P2, Pillar-1). The `cell-edit-burst` perf scenario's frame p99 (`SmatchetUI::Draw` p99Ms) measures **~10.9 ms on the ci-windows-latest runner** — above the Pillar-1 p99 floor of 10.0 ms (100 Hz). Median-of-3 armed run = 10.879 ms (run 27105397982, PR #970) — persistent, not scheduler noise. Every other scenario (`idle`, `priority-grid-scroll`, `ai-chat-history-render`) sits at 0.45–0.71 ms; only this one is hot.

Crucially this **may not be a product bug**: ci-windows-latest is a 2-core shared VM with Mesa software GL — nothing like user hardware; and cell-edit-burst is a deliberate edit-storm stress, arguably not "steady-state UI work." A CI-only ceiling (`regression-policy.json` `perScenario.cell-edit-burst.p99_abs_ceiling_ms = 15.0`, PR #970) already prevents this from red-walling CI, but **does not excuse a real >10 ms p99 on user hardware**. Intended outcome: a definitive real-HW datapoint that either closes this as a CI-software-GL artifact or scopes a concrete optimization.

## Approach

**Phase 1 — measure on real hardware (the blocking unknown; cannot be done in CI or this agent environment — needs a real GPU box).** Run the `cell-edit-burst` scenario via `scripts/dev/perf-run.sh` on a real-GPU machine (native GL, not Mesa), median-of-3, and read `SmatchetUI::Draw` p99Ms. Compare against the 6.94 ms steady-state target and the 10.0 ms p99 floor. This is a **measurement protocol** the user (or a real-HW runner) executes — the plan documents exactly which command + how to read the result.

**Phase 2 — branch on the datapoint:**
- **If real-HW p99 ≤ ~7 ms** → the CI number is a 2-core-Mesa-software-GL artifact. Action: document the finding, keep the CI-only 15.0 ms ceiling, and **close #973** as not-a-product-bug (with the real-HW evidence attached). No code change.
- **If real-HW p99 > 10 ms** → real Pillar-1 violation. Action: profile `SmatchetUI::Draw` under cell-edit-burst (`SMATCHET_UI_PERF_SCOPE` markers around the grid cell-edit commit path / field-edit pipeline), find the hot path (likely the per-edit grid re-layout or field-edit-pipeline revalidation firing every burst frame), and optimize (coalesce edits, cache layout, defer revalidation). That fix is a separate implementation plan scoped from the profile.

Trade-off named: this is deliberately a **verify-first** plan — committing to an optimization before the real-HW datapoint risks optimizing a software-GL artifact that doesn't exist on user hardware. The cheap measurement gates the expensive profiling.

## Files to modify

Phase 1 (measurement) — no product files; possibly a short note in `docs/perf/` recording the real-HW result.

Phase 2 (only if real-HW confirms) — scoped from the profile, expected candidates:
1. `Source/Core/src/Ui/` grid cell-edit / `SmatchetUI::Draw` hot path (exact file from the profile).
2. The grid field-edit pipeline (`SmatchetGridFieldEditPipeline` / `TicketGridModel`) if per-edit revalidation is the cost.
*(Named precisely only after Phase-1 confirms a real regression + Phase-2 profiling — do not pre-commit.)*

## Existing utilities reused

- `scripts/dev/perf-run.sh` + `scripts/dev/perf-compare.py` + `perf-median.py` — the measurement harness.
- `docs/perf/regression-policy.json` — the per-scenario ceiling already set.
- `SMATCHET_UI_PERF_SCOPE` markers (via the `perf-instrument` skill) — for Phase-2 profiling.
- `perf-detective` / `perf-measure` agents — own the Phase-2 hypothesis→profile→validate loop if it triggers.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms; p99 floor 10.0 ms)**: the entire subject. Phase 1 determines whether a real violation exists.
- **Pillar 2 / 3 / 4**: N/A — no blocking I/O, lifetime, or a11y change (Phase 2 fix, if any, is pure perf).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

Phase 1: **N/A** (measurement only, no Source/Core diff). Phase 2 (if triggered): gate 1 (PR-fast CI) names `cell-edit-burst` as the directly-exercised scenario; gate 5 (marker inventory) regen if `SMATCHET_UI_PERF_SCOPE` markers are added; baseline-bump + `perf-out-of-band` only if an intentional tradeoff. Declared per the Phase-2 PR.

## Risks / non-goals

- **Risk**: no real-HW box is available to the agent — Phase 1 is **user-gated** (or a self-hosted real-GPU runner). This plan cannot self-complete; it produces the protocol + the decision tree. Flagged as manual residue with a concrete automation path (a self-hosted real-GPU perf runner) below.
- **Non-goal**: removing the CI-only 15.0 ms ceiling — it stays regardless (CI software-GL is legitimately slower); this plan is about the *real-HW* truth.
- **Non-goal**: optimizing other scenarios — only cell-edit-burst is hot.

## Verification

- **Bucket A / E**: N/A for Phase 1. Phase-2 fix (if any) verified by the `cell-edit-burst` PR-fast scenario dropping below the floor on the real-HW/baseline comparison.
- **Real-HW measurement (manual residue — see below)**: `perf-run.sh cell-edit-burst` median-of-3 on a native-GL machine; record p99Ms.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: grill with the user — do they have a real-HW box / self-hosted runner to get the datapoint, or should this stay parked behind the CI ceiling?
- **Manual residue**: the real-HW measurement is the one manual step. **Deferred-automation action plan**: stand up a self-hosted real-GPU GitHub runner that runs the perf suite on native GL (tracked as a tooling follow-up) — then `cell-edit-burst` p99 is measured on representative hardware every PR and this verify-first dance disappears. Not a flat "out of scope": the concrete automation is the self-hosted-real-GPU-runner item.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs (esp. the tooling.md `p99-gate-warmup-frame-exclusion` item, which is the sibling CI-p99-fidelity concern).

- The self-hosted real-GPU perf runner (the durable fix for "can't measure real HW in CI") — tooling follow-up.
- Phase-2 optimization detail — a separate implementation plan, scoped only if Phase 1 confirms a real regression.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
