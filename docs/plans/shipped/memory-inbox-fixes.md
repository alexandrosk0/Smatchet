# Plan — Memory-inbox fixes: postmortem-owed false-positive + workflow-fleet rules

> **Slug**: `memory-inbox-fixes` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

The auto-memory inbox holds two items (2026-06-11) that each name a concrete defect in the agent system:

1. **`owed-postmortem-defensive-label-falsepos`** — `agents/scripts/core/postmortem-owed.sh` emits an `override: <label>` owe whenever an override label is present on a merged PR, **regardless of whether any check was red**. Investigated PRs #1124 (`tests-out-of-band`, all checks green at merge) and #1110 (`cr-out-of-band`, snapshot `redChecks:[]`) bypassed nothing, yet nudge every SessionStart. Label-presence ≠ red bypass; the wolf-crying nudge trains agents to ignore a gate-don't-trust signal (alarm fatigue).
2. **`workflow-agents-scope-tight-avoid-compact`** — broad survey dimensions made 2 of 5 workflow subagents balloon to ~130k tokens and auto-compact (one compaction 5.4 min, 130k→15k): pure token waste that looks like a stall in the UI. Same failure class as the 2026-06-10/11 audit debacle: a 4.1M-token fleet died on a session limit with zero durable output, then three salvage waves died of (a) TPM starvation from an inherited 1M-context model, (b) 200K-context overflow on oversized input files, (c) a permission deny-all on out-of-workspace reads — and the final wave's run dir (journal + 27 cached results) was deleted by the runtime on kill. None of these rules are written down anywhere.

After this lands: the postmortem nudge fires only on a true red-bypass, and a `docs/agent-rules/workflow-fleets.md` rule-doc makes each diagnosed fleet failure mode a violation of a written rule.

## Approach

Two independent slices, one PR each (one PR per logical feature).

**Slice A (tooling)** — teach `postmortem-owed.sh` to distinguish "label + red bypassed" (true escape → owe) from "label, nothing red" (defensive label → soft advisory). Snapshot path: derive `redChecks` and `overrideLabels` separately; owe only when `redChecks` is non-empty; label-only rows downgrade to a soft `defensive label (no red bypassed)` line shown in `--list` and **silent in `--nudge`**. Live-fallback path (no snapshot row): same rule keyed on the live rollup. Companion process rule in `docs/agent-rules/merge-gates.md`: never pre-apply `*-out-of-band` labels — apply only in response to a confirmed red check. New bats suite covers both paths plus the existing de-noise/dedupe behaviours (the script has zero test coverage today).

**Slice B (docs)** — new `docs/agent-rules/workflow-fleets.md` encoding the fleet-authoring rules learned from the memory item + the five diagnosed debacle failure modes: tight per-agent scoping with explicit budget lines, model pinning for fan-outs, concurrency caps sized against sibling sessions, input-size pre-flight, in-workspace staging of everything a background agent touches, a repo-file checkpoint contract (runtime-owned run dirs are deleted on kill — observed twice), a stall-watchdog reading pattern, and a transcript-salvage runbook. Wired into the `AGENTS.md` on-demand rule-docs table (one row; AGENTS.md at 97/150 lines). Watchdog + pre-flight *scripts* are deferred to `categories/tooling.md` backlog entries — docs first, automation when the next fleet runs.

## Files to modify

1. [agents/scripts/core/postmortem-owed.sh](../../../agents/scripts/core/postmortem-owed.sh) — split snapshot trigger derivation into red/labels parts; owe vs soft-note logic in both snapshot and live-fallback paths; `POSTMORTEM_LEDGER` env override for testability; header-comment trigger table updated.
2. `tests/bats/postmortem_owed.bats` (new) — fixture snapshot ledger + postmortems ledger, stubbed `gh` (PR TSV rows + `pr view` files) and `git` (revert log), ~10 cases.
3. [docs/agent-rules/merge-gates.md](../../agent-rules/merge-gates.md) — § Override-label hygiene (never pre-apply; defensive labels soft-noted, not owed).
4. `docs/agent-rules/workflow-fleets.md` (new) — fleet-authoring rules (slice B).
5. [AGENTS.md](../../../AGENTS.md) — one row in § Project rules on-demand rule-docs table: fleet/workflow authoring → `workflow-fleets.md`.
6. [docs/self-improvement/categories/tooling.md](../../self-improvement/categories/tooling.md) — two P2 entries: `workflow-watchdog.sh`, fleet pre-flight check.

## Existing utilities reused

- `snapshot_parts` (renamed from `snapshot_trigger`) / `has_snapshot` jq plumbing — `agents/scripts/core/postmortem-owed.sh:109-130` — extended + renamed for the TSV field-shift fix (emits red-checks/labels parts); ledger stays schema=1.
- `core_scoped_only_trigger` + `pr_touches_core_cpp` de-noise — `postmortem-owed.sh:69-83` — unchanged, still applied to owes.
- bats stub-`gh`/fixture conventions — `tests/bats/followup_due_nudge.bats:35-43` — same FAKEBIN-on-PATH pattern.
- `scripts/dev/project-config.sh` `PC_OVERRIDE_LABELS` emission — config-sourced label set, unchanged.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — agentic shell + docs only, no product code.
- **Pillar 2 (UI-thread)**: no impact — no product code.
- **Pillar 3 (never crash)**: no impact — no product code.
- **Pillar 4 (accessibility)**: no impact — no product code.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — pure agentic-shell (bash + bats) and docs diff; no `Source/Core/` files touched, so all five gates (PR-fast, Pillar-2 scanner, dispatcher drain, bucket-E, marker inventory) do not fire.

## Risks / non-goals

- **Risk**: live-fallback downgrades hide a real escape on an un-instrumented merge whose checks were re-run green post-merge (the rollup is lossy). Accepted — the snapshot ledger is written by every merge actor since ADR-0017, so fallback-only merges are pre-ledger legacy; the memory item's investigation chose exactly this trade-off.
- **Risk**: `categories/tooling.md` append may conflict with another live session's staged edit to the same file. Accepted — appends are trivially rebased.
- **Non-goal**: implementing `workflow-watchdog.sh` / fleet pre-flight scripts (backlogged, slice B entries).
- **Non-goal**: re-running the audit salvage workflow (separate decision — cost question for the user; slim evidence + script survive in `build/salvage/` + the session workflows dir).
- **Non-goal**: owed-postmortem RCAs for #1124/#1110 — the memory item's investigation concluded no escape occurred; the fix here makes the detector agree.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no product code.
- **Build gate**: N/A — pure docs + agentic shell; `is-pure-docs-diff` cadence rule, no cmake build.
- **bats**: `bats tests/bats/postmortem_owed.bats` green (new suite, ~10 cases: snapshot label-only soft + nudge-silent, snapshot red±label owes, clean-snapshot silent, live-fallback both ways, ledger dedupe, core-scoped drop, revert trigger).
- **Lint gates**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` green (shell-lint covers the .sh edit; agent-size covers AGENTS.md row).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-tested against the domain model before finalising; storage-substrate pre-flight done — the snapshot ledger is append-only JSONL at `docs/self-improvement/merge-snapshots.jsonl` (schema=1 keys `pr`/`mergeCommit`/`redChecks`/`overrideLabels`, asserted at `postmortem-owed.sh:95-96`), not a DB; no schema bump needed since owe/soft is derived, not stored.
- **Manual residue**: none — both slices fully machine-verified (bats + doc suite + lint gates).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them. (Checked: `workflow-watchdog` / `fleet-preflight` appear nowhere yet — the backlog entries this plan adds are their first mention.)

- **Workflow-runtime changes** (journal persistence outside the run dir, checkpoint API) — harness-owned, not repo-owned; the rule-doc works around it. No-action.
- **`postmortem-owed.sh --selftest`** — the advisory script has no selftest contract today; the bats suite is the regression net. Follow-up only if the gate-selftest pattern is extended to advisory scripts.
- **Salvage wave-5 re-run** — user decision post-ship (token cost); evidence preserved.

## Implementation log

- `c4896898` · wip(plan): memory-inbox-fixes — plan-doc committed up front.
- `b69a7c2d` · slice A (PR #1139): `postmortem-owed.sh` owe-vs-soft split (snapshot + live-fallback paths), `POSTMORTEM_LEDGER` override, new 15-case `tests/bats/postmortem_owed.bats`, `merge-gates.md` § Override-label hygiene.
- `6f573d68` · slice B (PR #1141): new `docs/agent-rules/workflow-fleets.md`, AGENTS.md on-demand rule-docs row, 2 P2 `categories/tooling.md` entries (`workflow-watchdog.sh`, fleet pre-flight).

## Deviations from plan

- bats suite grew to 15 cases vs planned ~10 — added mixed-PR, nudge-vs-list parity, and snapshot-authoritative cases while writing.
- **Unplanned fix**: the suite exposed a latent pre-existing TSV field-shift bug — tab is IFS *whitespace* to bash `read`, so an empty `labels` field shifted `redChecks` left and silently dropped real escapes on label-less red-check rows. Fixed at both read sites via `tr '\t' '\037'` + `IFS=$'\037'` (ASCII unit separator is non-whitespace → positional split survives empty fields).
- Slice B branch work in the session worktree needed git plumbing (`branch` + `symbolic-ref` + `read-tree --reset -u`) — `guard-shared-tree.sh` false-denies `-C <worktree>`-targeted `checkout` (known issue, `historical-review-findings.md` #913).

## Verification (actual)

- `bats tests/bats/postmortem_owed.bats` — **15/15 pass** (slice A).
- `agents/scripts/project/test-lint-rules.sh --diff origin/develop` — all PASS on both slices (shell-lint on the .sh edit; `agent-too-long` on the AGENTS.md row).
- `scripts/dev/test-docs.sh` — **11/11 PASS** on both slices.
- Live re-run of the fixed script: #1124/#1110 false positives gone; two **genuine** red-check owes surfaced (#1137 — Bucket-E/Sanitizer; #1130 — Coverage) and left to the existing nudge flow.
- Build: not run — pure docs + agentic-shell diff (`is-pure-docs-diff` cadence rule), as planned.

