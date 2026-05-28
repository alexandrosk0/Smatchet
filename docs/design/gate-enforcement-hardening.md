# Plan — Gate enforcement hardening (agent → GitHub promotion)

> **Slug**: `gate-enforcement-hardening`
>
> **Origin**: External reviewer feedback (2026-05-28) flagging the gap between Smatchet's documented quality stance ("Pillars 1-3 are enforceable merge gates") and the actual `develop` branch protection (4 required status checks: Test-delta + 3 MSVC builds; `required_reviews: null`; `enforce_admins: false`). Grilled via `grill-with-docs` skill; CONTEXT.md § UX Pillars updated inline with truthful § Enforcement matrix that forward-references this plan.

## Context

The reviewer correctly identified that several quality controls Smatchet documents as enforceable are actually advisory or orchestrator-side only:

- `coverage.yml` runs in advisory mode (`continue-on-error: true`, `--threshold 0`) with a flip date (2026-05-30) that arrived today.
- Bucket-E ImGui Test Engine isn't invoked from any GitHub workflow.
- `pillar2-scan.sh` exists but no workflow runs it as a required check.
- ASan/UBSan sanitizer build runs only during `debug-detective` investigations, not on PRs.
- `merge-gates.sh` (the orchestrator's CR/CI/user-comment poller) is **client-side**: a human clicking "Squash and merge" via the GitHub UI bypasses it entirely.
- `enforce_admins: false` lets the sole maintainer skip every required check, even the ones that ARE wired.

Grilled findings the reviewer missed: `enforce_admins: false` is the largest hole (sole maintainer bypasses everything); `merge-gates.sh` being client-side means none of CR/perf/CR-finding-count enforcement is server-enforced; the coverage.yml flip date is overdue execution, not a missing policy. CONTEXT.md was making a claim ("Pillars 1-3 are enforceable merge gates") that the GitHub config didn't back — that overclaim has been corrected to "agent-enforced" + an Enforcement matrix.

After this lands, the orchestrator-side `merge-gates.sh` contract is mirrored by GitHub-side required checks for Pillars 1-3, the sole maintainer can no longer bypass via the GitHub UI, and the gap between documented invariants and enforced reality is closed.

## Approach

**Agent → GitHub-side promotion** along a cheap-first sequence. Each promoted gate keeps its agent-side enforcement (the orchestrator + `merge-gates.sh` + specialist agents continue to do their job); the GitHub-side gate is the second line of defence that catches hand-merges + bypass attempts.

**Cheap-first ordering** chosen over risk-first because three of the five wins ship in < 1 day total. Risk-first would block on the Pillar 1 baselining work which is real engineering effort. Cheap-first lets the easy gates land while Pillar 1's prerequisite (baselining 8 remaining scenarios) proceeds in parallel.

**No gate is promoted before it is silent on `develop`.** The Pillar 2 scanner already passes repo-wide (verified: zero CRITICAL findings on 2026-05-28). The 3 `TODO(pillar2)` sites (`SmatchetAttachmentPreviewUi.cpp:62`, `SmatchetPlanDocViewerUi.cpp:104`, `LuaConsolePlugin.cpp:103`) are explicitly annotated and don't trip the scanner — they're decoupled work, not a gate-promotion blocker.

## Slices

### Slice 1 — Pillar 2 scanner workflow (cheap, no risk)

**Goal**: `pillar2-scan.sh` runs in CI; added to `develop` branch-protection required checks.

**Files**:
- `.github/workflows/pillar2-scan.yml` (new) — invokes `scripts/dev/pillar2-scan.sh` over the changed-files set against `develop`.
- `develop` branch protection — add `Pillar 2 scanner` to `required_status_checks`.

**Why cheap**: scanner is silent on develop today (`bash scripts/dev/pillar2-scan.sh` over `Source_Core/src/` + `Plugins/` returns zero CRITICAL findings). Adding it as required cannot break develop on day one. New PRs that introduce sync I/O to UI-thread paths block immediately — exactly the design intent.

**Est**: 30 min.

### Slice 2 — `enforce_admins: true` (biggest hole, smallest fix)

**Goal**: sole-maintainer GitHub-UI override of required checks eliminated.

**Files**:
- One `gh api -X PATCH repos/alexandrosk0/Smatchet/branches/develop/protection/enforce_admins` call to flip the flag.
- `docs/CONTEXT.md` § Enforcement matrix — note the flip date.

**Why fast**: single API call. No code change. Risk = rare emergency-merge friction; the orchestrator + ship-loop already do not rely on the admin bypass, so the friction lands on manual emergency-fix flows only. When such a flow surfaces, the recovery is documented: `gh api -X PATCH ... enforce_admins -F enabled=false`, merge, `... -F enabled=true`. Two API calls, surface bounded.

**Est**: 1 min API call + 5 min CONTEXT.md edit.

### Slice 3 — CR finding-count Action wrapper

**Goal**: CodeRabbit's "0 actionable findings" gate becomes a GitHub-required StatusContext.

**Files**:
- `.github/actions/cr-finding-gate/action.yml` (new) — composite action that calls the existing GraphQL query (`scripts/dev/merge-gates.graphql`) for the PR's headRefOid, counts actionable CR comments, posts a StatusContext via `gh api repos/.../statuses/<sha>`.
- `.github/workflows/cr-finding-gate.yml` (new) — runs the action on `pull_request` events.
- `develop` branch protection — add `CR findings (0 actionable)` to `required_status_checks`.

**Why custom action over flipping `.coderabbit.yaml: request_changes_workflow: true`**: CR's `chill` profile + flipping to `request_changes_workflow: true` would force every finding (including nits CR auto-resolves) to formally request changes — too aggressive for the project's posture. The custom action preserves the existing smart "0 actionable" semantics, lifted from client-side to server-side.

**Design constraint**: the action MUST honour the existing `tests-out-of-band` / `perf-out-of-band` label override pattern from `merge-gates.sh` for consistency.

**Est**: ~2 h (action.yml + workflow + branch-protection update + smoke-test against a no-op PR).

### Slice 4 — Nightly sanitizer build

**Goal**: ASan + UBSan + doctest suite runs nightly on `develop`; failure blocks the next morning's first PR until investigated.

**Files**:
- `.github/workflows/sanitizer-nightly.yml` (new) — cron `0 4 * * *` (UTC), checks out `develop`, builds with `ninja-test-msvc-asan` (or equivalent ASan preset; verify exact preset name during impl), runs `ctest`. Posts a StatusContext + opens a `bug` issue on failure.
- `develop` branch protection — does NOT require this check on per-PR; the gate is "last nightly was clean" surfaced as a `bug` issue, not a per-PR block. (Per-PR sanitizer build was explicitly rejected during grill — too slow for a solo project.)

**Why not required per-PR**: ASan + full ctest run = 10-20 min added per PR. UBSan-only is cheaper but still 5-10 min. Decision (grilled): per-PR cost too high; nightly catches regressions within 24 h with zero per-PR cost.

**Trade-off**: a UB regression can land + sit for up to ~24 h before the nightly catches it. Accepted; the agent-side enforcement (`debug-detective` runs sanitizer on every crash-suspect investigation) is the primary line; nightly is the safety net.

**Est**: ~3 h (workflow + UBSan flag tuning + first nightly run + verify failure-issue auto-open works).

### Slice 5 — Pillar 1 perf gate promotion (gated on baselining)

**Goal**: `Perf PR-fast (windows-2022)` promoted to required status check on `develop`.

**Blocker**: 8 of 15 perf scenarios lack baselines (backlog reference: `tooling-process-backlog-sweep` retrofitted some; 8 remain invisible to the gate today per the pillar-1-2-perf-review-system plan log). Promoting now would silently let perf regressions on the 8 un-baselined scenarios slip past.

**Sub-plan (separate work, this slice is gated on it)**:
- 5a — Identify the 8 un-baselined scenarios via `perf-baseline.sh --list-missing` (verify script supports this; add the flag if not).
- 5b — Run each scenario, capture baseline via `scripts/dev/perf-baseline.sh`.
- 5c — Verify all 15 scenarios are now in `docs/perf/baselines/<host>/*.json`.
- 5d — Flip `Perf PR-fast` to required.

**Why deferred to last**: real engineering work, not a config flip. Each baseline capture requires a clean run on a quiet machine; 8 scenarios = 8 captures + sanity-checking each result. Estimated at 4-6 h. Tracking as a sub-plan keeps the easy wins (slices 1-4) from being held hostage.

**Files (deferred to 5b implementation)**:
- 8 new files under `docs/perf/baselines/<host>/*.json`.
- `develop` branch protection — add `Perf PR-fast (windows-2022)` to required (slice 5d only).

**Est**: 4-6 h spread across 5a-5d.

## Ship order

Cheap-first. Slices 1-4 are independent — could ship in parallel in different PRs. Slice 5 is gated on its own 5a-5c work.

```text
Slice 1 (30 min)  →  Slice 2 (5 min)  →  Slice 3 (~2 h)  →  Slice 4 (~3 h)  →  Slice 5 (gated, 4-6 h)
```

Slices 1 + 2 ship in the same PR (both are dev-ops only, both are tiny).

## Summary

| Slice | Title | Mode after slice | Est. |
|---|---|---|---|
| 1 | Pillar 2 scanner workflow | Required check | 30 min |
| 2 | `enforce_admins: true` | No GitHub-UI bypass | 5 min |
| 3 | CR finding-count Action | Required check (mirrors merge-gates.sh) | 2 h |
| 4 | Nightly sanitizer build | Auto-opens `bug` issue on failure | 3 h |
| 5 | Pillar 1 perf gate promote | Required check (after 8 scenarios baselined) | 4-6 h |
| **Total** | | **5 slices** | **~10 h** |

## Existing utilities reused

- `scripts/dev/pillar2-scan.sh` — already exists, already silent on develop (slice 1).
- `scripts/dev/merge-gates.graphql` — single source of truth for the CR finding-count query (slice 3 lifts this into the GitHub Action).
- `scripts/dev/merge-gates.sh` — orchestrator-side enforcement stays as-is; slices 1/3/5 mirror its gates server-side without replacing them (defence in depth).
- `scripts/dev/perf-baseline.sh` — slice 5a-5c baseline capture; verify `--list-missing` exists or add it.
- `tests-out-of-band` / `perf-out-of-band` label override convention from `merge-gates.sh` — slice 3 honours the same labels for consistency.
- `develop` branch protection API surface — `gh api -X PATCH repos/.../branches/develop/protection` already used for required-status-check additions; reuse for slices 1-5.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: this plan's slice 5 IS Pillar 1's enforcement promotion. No new perf surface introduced by the plan itself.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: this plan's slice 1 IS Pillar 2's enforcement promotion. The 3 outstanding `TODO(pillar2)` sites are decoupled fixes (separate work).
- **Pillar 3 (never crash)**: this plan's slice 4 IS Pillar 3's enforcement promotion (nightly sanitizer). RAII rule + ASan + UBSan + ship-build graceful degradation continue as agent-side enforcement during the day.
- **Pillar 4 (accessibility)**: no change. Still aspirational per CONTEXT.md.

## Perf-review-system gates

N/A — this plan's diff is workflows + branch-protection API calls + docs. Zero `Source_Core/` C++ touches.

## Risks / non-goals

- **Risk (slice 2)**: `enforce_admins: true` will fire on a genuine emergency-fix flow. Mitigation: recovery is two `gh api` calls (flip off → merge → flip on); document in CONTEXT.md § Enforcement matrix.
- **Risk (slice 3)**: 3rd-party CR availability becomes a blocker. Mitigation: action falls through to PASS if CR's StatusContext is missing AND the `cr-installed` probe shows no `.coderabbit.yaml` — mirror `merge-gates.sh`'s existing graceful-degradation logic.
- **Risk (slice 4)**: nightly sanitizer failures pile up if not triaged. Mitigation: auto-issue-open on failure routes through `debug-detective` agent; existing self-improvement loop catches stale unfixed `bug` issues.
- **Risk (slice 5)**: 8 baseline captures may surface real perf regressions discovered only at baselining time. Mitigation: fix them (or document acceptance) before flipping the required check.
- **Non-goal**: coverage threshold enforcement (`coverage.yml` flip from advisory to required). Out of scope for this plan — overdue execution rather than missing policy, and the threshold value itself needs separate discussion. Track as a follow-up.
- **Non-goal**: `required_pull_request_reviews` (code-owner review). Sole maintainer; deferred until additional reviewers exist.
- **Non-goal**: bucket-E ImGui Test Engine in CI. Tracked separately under headless GL/Mesa story in backlog.

## Verification

- **Slice 1**: open a no-op PR; confirm `Pillar 2 scanner` check appears as required + reports green. Open a second PR adding a deliberate `cpr::Get(url).text` call from a `*Ui*.cpp` file; confirm the check fails. Revert the deliberate violation.
- **Slice 2**: post-flip, attempt `gh api -X PUT .../pulls/<n>/merge` on a PR with a failing required check while authenticated as the admin user; expect 405 / merge refusal. Document the override-then-restore recipe in CONTEXT.md § Enforcement matrix.
- **Slice 3**: open a no-op PR; confirm the `CR findings (0 actionable)` StatusContext fires SUCCESS. Test the failure path by posting a manual `coderabbitai[bot]`-impersonating inline comment via the API (or by waiting for a real CR finding on a follow-up PR).
- **Slice 4**: trigger the nightly workflow manually via `gh workflow run sanitizer-nightly.yml`; verify it runs to completion + correctly opens an issue on a deliberately-broken doctest. Revert the deliberate breakage.
- **Slice 5**: post-baselining, confirm `bash scripts/dev/perf-compare.py` against all 15 baselines reports a clean diff. Then add the required check.
- **Build gate**: N/A — no C++ touched.
- **Manual residue**: each slice has a one-time manual verification step (open a no-op PR to confirm the gate is wired). These are not automatable without a fixture-PR harness, which is out of scope. Tracked as `docs/backlog/agent-self-improvement/test.md` candidate if a future plan needs the harness.

## Out of scope

- Coverage threshold promotion (`coverage.yml: --threshold 0` → `--threshold 70` + `continue-on-error: false`). Overdue execution; separate follow-up.
- `required_pull_request_reviews` / CODEOWNERS-enforced review. Sole-maintainer project; revisit when additional reviewers exist.
- Bucket-E (ImGui Test Engine) in CI. Headless GL/Mesa story untracked here.
- TSan / MSan extensions to the nightly sanitizer. Start with ASan + UBSan; expand if real bugs surface.
- The 3 outstanding `TODO(pillar2)` sites. Decoupled fixes; not gate-promotion blockers.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
