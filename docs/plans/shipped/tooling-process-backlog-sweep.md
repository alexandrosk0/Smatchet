# Plan — Tooling + Process backlog sweep (P0-P2)

> **Slug**: `tooling-process-backlog-sweep` (matches this file's basename without `.md`).
>
> **Status**: `shipped` (2026-06-18) — all 10 slices complete. Slices 1-9 merged via #475-#487; **slice 7 (the bucket-E batch) closed out 2026-06-18 via the B8 campaign** (`docs/plans/shipped/b8-bucket-e-coverage.md`).
>
> Slice-7 disposition: **#24** (model-change strip) shipped **#1372** (B8 L2); **#31** (`BucketE::TooltipContentMatches` helper) shipped **#1364** (B8 L3); **#13** covered by the 10 shipped `tests/ui/ai_assistant_*` bucket-E TUs already on develop; **#23** moot (coderabbit-react-loop runtime deleted) and **#25** moot (Preferences Agentic tab removed). The two live `tooling.md` entries (#24/#31) are archived to `applied.md` in this PR.
>
> **Origin**: User request, 2026-05-27. Sweep all P0-P2 items from `docs/self-improvement/categories/process.md` and `tooling.md`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules for plan location, plan-doc safety, plan revision after implementation, plan stress-test, plan template, and plan-doc perf-gate section.

## Context

The agent self-improvement backlog has 14 P0-P2 process items and 23 P0-P2 tooling items (37 total). These encode real friction discovered across 40+ sessions — merge-gates bypasses, watcher bugs, missing test infrastructure, process gaps. Several items are already partially or fully fixed but never archived. After this lands, 37 backlog entries move to applied.md and the live P0-P2 count drops to zero across both categories.

## Approach

Group by subsystem and dependency. The merge-gates → merge-watcher chain ships first (P0 + P1 items). Independent slices (docs, CI, tests, git-janitor, perf) ship in parallel. Items already fixed get verified and archived in-slice rather than as a separate pass.

**Pre-implementation triage** found 4 items are already done or nearly done:
- Item 4 (GH_API_DOWN): exit-code mapping at `merge-watcher.py:328-335` is correct. Verify edge cases only.
- Item 5 (long-running polls): merge-watcher daemon exists. Close with note.
- Item 15 (triage budget per-HEAD): PR #418 merged 2026-05-23. Archive to applied.md.
- Item 20 (UnicodeEncodeError): `sys.stdout.reconfigure` applied. Only autostart wrapper env-var remains.

## Slices

### Slice 1 — Merge-gates: draft-PR CR bypass (P0) + ADR 0006 hole
**Items**: #1 (P0), #9 (P2)
**Est**: 3-4 h

The highest-priority fix. `.coderabbit.yaml` has `auto_review.drafts: false`, so draft PRs get a placeholder CR StatusContext SUCCESS that the poller treats as genuine. Three-prong fix:
1. Call `gh_pr_ready_idempotent` at poll start (not just at merge time) so CR can fire
2. Require non-empty CR review body (not just StatusContext) in the gates logic
3. Document the `@coderabbitai review` trigger for manual re-review

**Files**: `scripts/dev/merge-gates.sh`, `scripts/dev/merge-gates.graphql`, `docs/adr/0006-*.md`, `AGENTS.md`, `agents/core/git-janitor.md`, `tests/bats/merge_gates.bats`

### Slice 2 — Merge-gates: STALE recovery + explain BLOCKED
**Items**: #2 (P1 — verify completion), #10 (P2), #17 (P2)
**Est**: 3-4 h

STALE_WITH_FINDINGS/STALE_CLEAN/STALE_RESOLVED already implemented (7 occurrences in merge-gates.sh). Verify completeness of item 2, then add:
- Auto-post `@coderabbitai review` on persistent STALE (≥5 polls, same head SHA)
- Auto-restart poller when orchestrator pushes after COMMENTED (head SHA advances)
- Diagnostic line printing `mergeStateStatus` alongside the gate's own decision

**Files**: `scripts/dev/merge-gates.sh`, `scripts/dev/merge-gates.graphql`, `AGENTS.md`, `tests/bats/merge_gates.bats`

### Slice 3 — Merge-watcher: triage latch + residuals
**Items**: #3 (P1), #4 (P1 — verify), #5 (P1 — verify/close), #20 (P2 — residual)
**Est**: 4-5 h

Core fix: `TRIAGE_BUDGET_EXHAUSTED` at `merge-watcher.py:794` is a terminal sink. After auto-fix push + CR re-review confirms green, the watcher must re-evaluate gates from scratch. Also: resolve CR review threads after auto-fix via GraphQL `resolveReviewThread`. Items 4/5 are verification-only (exit-code mapping is correct, daemon exists). Item 20 residual: add `PYTHONIOENCODING=utf-8` to `merge-watcher-install-autostart.ps1`.

**Files**: `scripts/dev/merge-watcher.py`, `scripts/dev/merge-watcher-install-autostart.ps1`, `tests/bats/merge_watcher.bats`

### Slice 4 — Doc-only process rules
**Items**: #6, #7, #8, #11, #12, #35, #36 (all P2)
**Est**: 2-3 h

Seven pure documentation edits encoding process lessons:
- **#6**: Forward-reference grep rule for scope-reduction edits → `AGENTS.md`
- **#7**: Cross-cutting plumbing feasibility-check → `agents/core/architect.md`
- **#8**: Storage-substrate pre-flight → grill-with-docs skill
- **#11**: Plan-revision direct-push policy → `AGENTS.md` + `docs/agent-rules/process-rules.md`
- **#12**: "Extend CLI, never ask user manually" rule → `agents/core/perf-measure.md`, `agents/core/perf-detective.md`, `agents/core/spike-hunter.md`
- **#35**: Worktree-absolute vs main-repo-absolute path discipline → `agents/core/test-rig.md` + `AGENTS.md`
- **#36**: AI chat panel bucket-E coverage gap → tracking entry only (actual work in slice 7)

### Slice 5 — P4/git mode alignment + SessionStart hook
**Items**: #14, #18 (both P2)
**Est**: 2 h

Extend `scripts/clear-session-context.sh` to detect `SMATCHET_AGENT_VCS=p4`, run `p4 info`, emit a banner. Create `scripts/dev/p4-git-sync-check.sh` comparing pending git paths vs `p4 opened`. Add session-start self-check rule to AGENTS.md.

**Files**: `scripts/clear-session-context.sh`, `scripts/dev/p4-git-sync-check.sh` (new), `AGENTS.md`

### Slice 6 — CI/workflow small fixes
**Items**: #22, #32, #33, #34 (all P2)
**Est**: 1.5-2 h

- **#22**: Add Font Awesome `fa-solid-900.ttf` fetch to build/release workflow
- **#32**: Tighten `coverage-delta-gate.sh` to `*.test.cpp` only
- **#33**: Guard `shift 2` in `coverage.sh` under `set -euo pipefail`
- **#34**: Add `CMakePresets.json` to `.github/workflows/coverage.yml` cache key

### Slice 7 — Bucket-E test coverage batch  ✅ DONE (2026-06-18, via the B8 campaign)
**Items**: #13, #23, #24, #25, #31 (all P2)
**Est**: 8-10 h (largest slice)
**Disposition**: #24 (model-change strip) shipped #1372 (B8 L2); #31 (`BucketE::TooltipContentMatches` helper) shipped #1364 (B8 L3); #13 covered by the 10 shipped `tests/ui/ai_assistant_*` TUs; #23 moot (coderabbit-react-loop runtime deleted); #25 moot (Preferences Agentic tab removed). The two live `tooling.md` entries (#24/#31) are archived to `applied.md` in the same PR.

New ImGui Test Engine test files following `tests/ui/views_columns_reorder.test.cpp` pattern:
- **#31** first: shared `BucketE::TooltipContentMatches` helper
- **#13**: 5 AI chat scenarios (can be one TU with 5 variants)
- **#23**: coderabbit-react-loop UI probes
- **#24**: DeepSeek model-change strip
- **#25**: Preferences Agentic tab

### Slice 8 — Git-janitor / worktree fixes
**Items**: #16, #29, #30, #37 (all P2)
**Est**: 3-4 h

- **#16**: `scripts/dev/git-janitor.sh --post-merge <pr>` — fetch/prune, switch/ff develop, delete merged branch, dual-target build, report
- **#29**: Audit plan-lock release on squash-merge; add staleness sweep to git-janitor checklist
- **#30**: `test-all.sh` auto-detect worktree context, skip incompatible scripts with `SKIPPED (worktree)`
- **#37**: Worktree orphan pre-flight cross-check (`git worktree list` vs `.git/worktrees/`) + salvage-tag for detached-HEAD unique commits

### Slice 9 — Perf infrastructure
**Items**: #21, #26, #27 (all P2)
**Est**: 3-4 h

- **#21**: Triage 8 perf scenarios: confirm bucket-C-only exclusions, retrofit `OnFinish` rows in roundtrip scenarios
- **#26**: Fix `--spawn` stale-file footgun — `unlink(outPath)` before `scenario.run` in `ScenarioRunner.cpp`
- **#27**: `scripts/dev/test-skill-vs-agent-parity.sh` for cross-harness CI parity

### Slice 10 — Spawn stdout capture + archival
**Items**: #15 (P2 — archive), #28 (P2)
**Est**: 2 h

- **#15**: PR #418 merged 2026-05-23. Verify code on develop, archive entry to applied.md.
- **#28**: Extend `LaunchEphemeralInstance` in `Target_Standalone/CliCommandRunner.cpp` to capture child stdout/stderr to per-spawn temp file, emit path in spawn banner.

## Ship order

```text
Slice 1  ──→  Slice 2  ──→  Slice 3      (sequential: merge-gates → merge-watcher)
Slices 4-10                                (independent: ship in parallel or any order)
```

## Summary

| Slice | Title | Items | Est. Hours |
|-------|-------|-------|------------|
| 1 | Merge-gates: draft-PR CR bypass | 1, 9 | 3-4 |
| 2 | Merge-gates: STALE recovery | 2, 10, 17 | 3-4 |
| 3 | Merge-watcher fixes | 3, 4, 5, 20 | 4-5 |
| 4 | Doc-only process rules | 6, 7, 8, 11, 12, 35, 36 | 2-3 |
| 5 | P4/git mode alignment | 14, 18 | 2 |
| 6 | CI/workflow fixes | 22, 32, 33, 34 | 1.5-2 |
| 7 | Bucket-E tests | 13, 23, 24, 25, 31 | 8-10 |
| 8 | Git-janitor / worktree | 16, 29, 30, 37 | 3-4 |
| 9 | Perf infrastructure | 21, 26, 27 | 3-4 |
| 10 | Spawn capture + archival | 15, 28 | 2 |
| **Total** | | **37 items** | **~33-40 h** |

## UX Pillar callouts

- **Pillar 1 (perf)**: Slice 9 directly improves perf infrastructure (scenario row coverage, stale-file fix). No steady-state regression.
- **Pillar 2 (UI-thread)**: No new UI-thread blocking paths. Slice 10's spawn capture is process-level, not UI-thread.
- **Pillar 3 (never crash)**: Merge-gates/watcher fixes (slices 1-3) improve crash resilience of the ship pipeline. Bats coverage gates correctness.
- **Pillar 4 (accessibility)**: No impact.

## Perf-review-system gates

Slice 9 touches `Source_Core/src/Commands/Scenarios/*.cpp` — perf scenarios themselves. These files are exempt from the PR-fast gate (they ARE the gate). All other slices are scripts/docs/tests — no `Source_Core/` production paths touched.

## Risks / non-goals

- **Risk**: Slices 1-3 touch the merge-gates/watcher pipeline that governs all PR merges. Mitigation: bats test coverage + live validation PR (same pattern as PR #472).
- **Risk**: Slice 7 (bucket-E tests) is 8-10 h and may need splitting into sub-PRs. Mitigation: each test TU is independent; split by TU if needed.
- **Non-goal**: P3 items are explicitly excluded. They remain in-file under their existing priority.
- **Non-goal**: Bug and security category items are out of scope — separate plan.

## Verification

- **Per-slice**: each slice ships as one PR through the autonomous ship-loop
- **Merge-gates slices (1-3)**: bats tests + live dummy-PR gate-poll cycle
- **Doc slices (4-5)**: pure-docs-diff skip per `scripts/dev/is-pure-docs-diff.sh`
- **CI slices (6)**: CI workflow run on the PR
- **Bucket-E slice (7)**: `cmake --build --preset ninja-ui-test-msvc` + bash drivers
- **Shell script slices (8-9)**: run new scripts locally + bats where applicable
- **Archival (10)**: confirm moved entries appear in applied.md
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (slices touching Source_Core only)
- **Manual residue**: none — all verification is automated or self-evident (doc edits)

## Out of scope (flagged, not designed)

- **P3 items** (process: ~6, tooling: ~16 parked) — remain in backlog; sweep when adjacent feature lands
- **Bug category items** (11 open) — separate plan
- **Security category items** (8 open) — separate plan
- **Test category items** (16 open) — separate plan
- **Infra category items** (10 open) — separate plan

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
