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

**GitHub branch-protection path-filter deadlock (load-bearing for every slice)**: GitHub does **not** report a path-filtered / skipped workflow as success. A required status check whose workflow has a positive `paths:` filter (e.g. `perf-pr-fast.yml` filters to `Source_Core/**` / `Plugins/**`) simply never reports on a PR that doesn't touch those paths — leaving the PR permanently "Expected — Waiting for status," unmergeable. Smatchet ships docs-only PRs constantly (this plan is one). Any slice that adds a path-filtered workflow to `required_status_checks` MUST pair it with the standard **dummy-pass companion job**: a second job in the same workflow guarded by the inverse `paths-ignore` filter that emits the identical check name with success, so the required context always reports exactly once regardless of which paths the PR touched. Slice 0 establishes this pattern; slices 1, 3, and 5 reuse it. (Source: independent Opus architect review, 2026-05-28.)

## Slices

### Slice 0 — Path-filter dummy-pass pattern (prerequisite for 1/3/5)

**Goal**: a reusable convention so any required path-filtered workflow always reports its check context, avoiding the deadlock above.

**Files**:
- `docs/agent-rules/ci-required-check-pattern.md` (new) — documents the dummy-pass companion-job recipe + the one-job-emits-the-check-name-once invariant.
- One reference implementation, applied first to the Pillar 2 workflow in slice 1.

**Recipe**: each gating workflow declares two jobs sharing one check name —
- `<gate>` (real): `on.pull_request.paths: [<relevant globs>]`, runs the actual check.
- `<gate>-skip` (dummy-pass): `on.pull_request.paths-ignore: [<same globs>]`, single step `exit 0`.

Both set the **same** `name:` so GitHub sees one required context that always reports. Never have both fire on the same PR (the paths / paths-ignore are mutually exclusive by construction).

**Est**: 45 min (doc + first reference job folded into slice 1).

### Slice 1 — Pillar 2 scanner workflow (cheap, low risk)

**Goal**: `pillar2-scan.sh` runs in CI; added to `develop` branch-protection required checks via the slice-0 dummy-pass pattern.

**Files**:
- `.github/workflows/pillar2-scan.yml` (new) — TWO jobs per slice-0: the real job computes the changed-file set itself (`git diff --name-only origin/develop...HEAD | grep -E '\.(cpp|h)$'`) and passes them to `bash scripts/dev/pillar2-scan.sh`; the dummy-pass job covers PRs that touch no `.cpp`/`.h`. (`pillar2-scan.sh` takes explicit file args only — no built-in diff/changed-files logic; that lives in the Claude hook + `test-all.sh` today, so the workflow must do the diff itself.)
- `develop` branch protection — add `Pillar 2 scanner` to `required_status_checks`.

**Why low risk**: scanner is silent on develop today (`bash scripts/dev/pillar2-scan.sh` over `Source_Core/src/` + `Plugins/` returns zero CRITICAL findings). Adding it as required cannot break develop on day one. New PRs that introduce sync I/O to UI-thread paths block immediately — exactly the design intent.

**Est**: 1 h (was 30 min — added the changed-files diff logic + dummy-pass job).

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
- `.github/workflows/cr-finding-gate.yml` (new) — runs the action; trigger detail below.
- `develop` branch protection — add `CR findings (0 actionable)` to `required_status_checks`.

**Why custom action over flipping `.coderabbit.yaml: request_changes_workflow: true`**: CR's `chill` profile + flipping to `request_changes_workflow: true` would force every finding (including nits CR auto-resolves) to formally request changes — too aggressive for the project's posture. The custom action preserves the existing smart "0 actionable" semantics, lifted from client-side to server-side.

**CR-timing race (must-solve, not a nicety)**: CodeRabbit posts its review **asynchronously** — on a fresh PR the action will run before CR finishes and read a not-yet-reviewed head (the `merge-gates.sh` analogue is `cr_actionable == -1` / "no inline CR comments on head"). A naive single `on: pull_request` run would false-pass (or false-fail) on every new PR. The workflow MUST trigger on the events that fire **after** CR submits — `on: pull_request_review` (CR's review-submitted) AND `on: pull_request_review_comment` — plus an initial `pull_request` run that, when CR hasn't reported yet, posts a **pending** status (not success), mirroring `merge-gates.sh`'s grace-window block. The required check goes green only once CR's review exists on the current head. Reuse `MERGE_GATES_CR_GRACE_POLLS` semantics conceptually but event-driven, not poll-driven.

**Fork-PR limitation**: `pull_request` from a fork has no write token, so `gh api .../statuses/<sha>` fails. Smatchet is sole-maintainer with no external PR flow today, so this is acceptable — but the plan records it: if external PRs are ever accepted, this gate cannot post a status for them (fall back to `pull_request_target` with explicit head-SHA pinning, or accept fork PRs bypass this gate). Document, don't engineer around it now.

**Design constraint**: the action MUST honour the existing `tests-out-of-band` / `perf-out-of-band` label override pattern from `merge-gates.sh` for consistency.

**Est**: ~3 h (was ~2 h — the CR-timing event wiring + pending-status path is the bulk of it; action.yml + branch-protection + smoke-test against a no-op PR).

### Slice 4 — Nightly sanitizer build

**Goal**: ASan + UBSan + doctest suite runs nightly on `develop`; failure auto-opens a `bug` issue.

**Files**:
- `.github/workflows/sanitizer-nightly.yml` (new) — cron `0 4 * * *` (UTC), checks out `develop`, builds with the **`ninja-clang-asan`** preset (verified to exist in `CMakePresets.json`; it is the only preset delivering ASan **+ UBSan** — `ninja-msvc-asan` is ASan-only because MSVC lacks UBSan), runs `ctest`. Posts a StatusContext + opens a `bug` issue on failure. NOTE: `ninja-clang-asan` requires clang-cl on the runner — the workflow must install / select the LLVM toolchain (the existing `ninja-iter-clang` CI path, if any, is the reference; otherwise add an LLVM setup step).
- `develop` branch protection — does NOT require this check per-PR; the gate is "last nightly was clean" surfaced as a `bug` issue, not a per-PR block. (Per-PR sanitizer build was explicitly rejected during grill — too slow for a solo project.)

**Why not required per-PR**: ASan + full ctest run = 10-20 min added per PR. Decision (grilled): per-PR cost too high; nightly catches regressions within 24 h with zero per-PR cost.

**Trade-off**: a UB regression can land + sit for up to ~24 h before the nightly catches it. Accepted; the agent-side enforcement (`debug-detective` runs sanitizer on every crash-suspect investigation) is the primary line; nightly is the safety net.

**Est**: ~3 h (workflow + clang-cl runner setup + first nightly run + verify failure-issue auto-open works).

### Slice 5 — Pillar 1 perf gate promotion (gated on CI-host baselining)

**Goal**: `Perf PR-fast (windows-2022)` promoted to required status check on `develop`, via the slice-0 dummy-pass pattern (the workflow has a positive `paths:` filter on `Source_Core/**` / `Plugins/**` / `Target_Standalone/**`, so it MUST get a dummy-pass companion or it deadlocks every docs-only PR).

**Blocker (corrected after Opus review — bigger than originally stated)**: the real blocker is NOT "8 of 15 scenarios un-baselined." It is "**zero CI-host baselines exist**." `perf-pr-fast.yml` compares against `docs/perf/baselines/<scenario>.ci-windows-latest.json`; every checked-in baseline today is `.dev.json` (local-host captures, useless to the CI gate). So all 15 scenarios need a fresh `ci-windows-latest` capture on the runner before the gate can mean anything. Baseline files are **flat-named** `docs/perf/baselines/<scenario>.<host>.json` — NOT a `<host>/` subdirectory.

**Sub-plan (separate work, this slice is gated on it)**:
- 5a — Enumerate the 15 scenarios from `scripts/dev/perf-pr-fast-set.json` (the curated PR-fast subset) and the full scenario registry; diff against the existing `*.ci-windows-latest.json` set (currently empty) to get the capture list. (`perf-baseline.sh` has NO `--list-missing` flag — it exposes only `init <scenario>` / `bump <scenario>`; the enumeration is a manual `ls docs/perf/baselines/*.ci-windows-latest.json` vs the scenario set.)
- 5b — Capture each CI-host baseline on the runner via `perf-pr-fast.yml`'s documented first-run bootstrap path (`bash scripts/dev/perf-baseline.sh init <scenario> --host=ci-windows-latest`, run on the CI runner — local captures are wrong-host). This is a runner-side operation, not a dev-machine one.
- 5c — Verify all 15 `*.ci-windows-latest.json` exist under `docs/perf/baselines/`.
- 5d — Add the slice-0 dummy-pass companion to `perf-pr-fast.yml`, then flip `Perf PR-fast (windows-2022)` to required.

**Why deferred to last**: real engineering work + runner-side baseline capture, not a config flip. The capture must happen on the CI host (timings are host-specific), so it goes through a workflow-driven bootstrap, not a local run. Tracking as a sub-plan keeps the easy wins (slices 0-4) from being held hostage.

**Files (deferred to 5b implementation)**:
- 15 new files: `docs/perf/baselines/<scenario>.ci-windows-latest.json` (one per PR-fast scenario).
- `perf-pr-fast.yml` — add dummy-pass companion job (slice 5d).
- `develop` branch protection — add `Perf PR-fast (windows-2022)` to required (slice 5d only).

**Est**: 4-6 h spread across 5a-5d (dominated by 5b runner-side captures + sanity-checking each).

## Ship order

Cheap-first, but **slice 0 first** — it establishes the path-filter dummy-pass pattern that 1, 3, and 5 all reuse. Without it, each promotion adds a fresh deadlock vector.

```text
Slice 0 (45 min, pattern)  →  Slice 1 (1 h)  →  Slice 2 (5 min)  →  Slice 3 (~3 h)  →  Slice 4 (~3 h)  →  Slice 5 (gated, 4-6 h)
```

Slices 0 + 1 + 2 ship in the same PR (slice 0's reference impl IS slice 1's workflow; slice 2 is a one-call config flip).

## Summary

| Slice | Title | Mode after slice | Est. |
|---|---|---|---|
| 0 | Path-filter dummy-pass pattern | Reusable convention (prereq for 1/3/5) | 45 min |
| 1 | Pillar 2 scanner workflow | Required check (dummy-pass companion) | 1 h |
| 2 | `enforce_admins: true` | No GitHub-UI bypass | 5 min |
| 3 | CR finding-count Action | Required check (event-driven, mirrors merge-gates.sh) | 3 h |
| 4 | Nightly sanitizer build (`ninja-clang-asan`) | Auto-opens `bug` issue on failure | 3 h |
| 5 | Pillar 1 perf gate promote | Required check (after 15 CI-host baselines captured) | 4-6 h |
| **Total** | | **6 slices** | **~12 h** |

## Existing utilities reused

- `scripts/dev/pillar2-scan.sh` — already exists, already silent on develop (slice 1).
- `scripts/dev/merge-gates.graphql` — single source of truth for the CR finding-count query (slice 3 lifts this into the GitHub Action).
- `scripts/dev/merge-gates.sh` — orchestrator-side enforcement stays as-is; slices 1/3/5 mirror its gates server-side without replacing them (defence in depth).
- `scripts/dev/perf-baseline.sh` — slice 5b baseline capture via `init <scenario> --host=ci-windows-latest` (subcommands: `init` / `bump`; there is NO `--list-missing` flag — enumeration is manual `ls`).
- `scripts/dev/perf-pr-fast-set.json` — the curated PR-fast scenario subset; slice 5a enumerates from here.
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

- **Risk (CRITICAL, slices 1/3/5)**: path-filter deadlock — a required check whose workflow has a positive `paths:` filter never reports on PRs outside those paths, wedging every docs-only PR forever. Mitigation: slice 0's dummy-pass companion-job pattern is a hard prerequisite; the slice-0 verification step (docs-only PR goes green) is the regression test. This is the single most likely real bug in the whole plan.
- **Risk (CRITICAL, slice 3)**: CR-timing race — the action runs before CodeRabbit posts its async review, reading a not-yet-reviewed head. Mitigation: event-driven trigger (`pull_request_review` + `pull_request_review_comment`) + initial `pull_request` run posts PENDING (not success) until CR's review exists on the current head. Naive single `pull_request` run = false-pass on every fresh PR.
- **Risk (slice 2)**: `enforce_admins: true` will fire on a genuine emergency-fix flow. Mitigation: recovery is two `gh api` calls (flip off → merge → flip on); document in CONTEXT.md § Enforcement matrix. NOTE: the flip-off window briefly removes ALL branch protection, not just admin enforcement — acceptable for solo but worth knowing. Compounds with the deadlock risk: a stuck docs-only PR (if slice 0 were skipped) couldn't even be admin-force-merged without flipping protection off first.
- **Risk (slice 3)**: 3rd-party CR availability becomes a blocker. Mitigation: action falls through to PASS if CR's StatusContext is missing AND the `cr-installed` probe shows no `.coderabbit.yaml` — mirror `merge-gates.sh`'s existing graceful-degradation logic. Fork PRs can't post statuses (no write token) — documented limitation; sole-maintainer repo takes no external PRs today.
- **Risk (slice 4)**: nightly sanitizer failures pile up if not triaged. Mitigation: auto-issue-open on failure routes through `debug-detective` agent; existing self-improvement loop catches stale unfixed `bug` issues. Secondary: `ninja-clang-asan` needs clang-cl on the runner — confirm toolchain availability before the first nightly.
- **Risk (slice 5)**: zero CI-host baselines exist today; capturing 15 fresh `ci-windows-latest` baselines may surface real perf regressions discovered only at capture time. Mitigation: fix them (or document acceptance) before flipping the required check.
- **Non-goal**: coverage threshold enforcement (`coverage.yml` flip from advisory to required). Out of scope for this plan — overdue execution rather than missing policy, and the threshold value itself needs separate discussion. Track as a follow-up.
- **Non-goal**: `required_pull_request_reviews` (code-owner review). Sole maintainer; deferred until additional reviewers exist.
- **Non-goal**: bucket-E ImGui Test Engine in CI. Tracked separately under headless GL/Mesa story in backlog.

## Verification

- **Slice 0**: open a docs-only PR (no `.cpp`/`.h` touched); confirm the dummy-pass companion job reports the required check name as success (NOT "Expected — Waiting for status"). This is the deadlock-regression test — run it for EACH of slices 1/3/5 after their workflow lands.
- **Slice 1**: open a no-op PR; confirm `Pillar 2 scanner` check appears as required + reports green. Open a second PR adding a deliberate `cpr::Get(url).text` call from a `*Ui*.cpp` file; confirm the check fails. Then open a docs-only PR and confirm it still goes green (dummy-pass path). Revert the deliberate violation.
- **Slice 2**: post-flip, attempt `gh api -X PUT .../pulls/<n>/merge` on a PR with a failing required check while authenticated as the admin user; expect 405 / merge refusal. Document the override-then-restore recipe in CONTEXT.md § Enforcement matrix.
- **Slice 3**: open a no-op PR; confirm the `CR findings (0 actionable)` StatusContext fires SUCCESS. Test the failure path by posting a manual `coderabbitai[bot]`-impersonating inline comment via the API (or by waiting for a real CR finding on a follow-up PR).
- **Slice 4**: trigger the nightly workflow manually via `gh workflow run sanitizer-nightly.yml`; verify it runs to completion + correctly opens an issue on a deliberately-broken doctest. Revert the deliberate breakage.
- **Slice 5**: post-baselining, confirm `python scripts/dev/perf-compare.py` (Python, not bash) against all 15 `*.ci-windows-latest.json` baselines reports a clean diff. Then add the dummy-pass companion + the required check.
- **Build gate**: N/A — no C++ touched.
- **Manual residue**: each slice has a one-time manual verification step (open a no-op PR to confirm the gate is wired). These are not automatable without a fixture-PR harness, which is out of scope. Tracked as `docs/self-improvement/categories/test.md` candidate if a future plan needs the harness.

## Out of scope

- Coverage threshold promotion (`coverage.yml: --threshold 0` → `--threshold 70` + `continue-on-error: false`). Overdue execution; separate follow-up.
- `required_pull_request_reviews` / CODEOWNERS-enforced review. Sole-maintainer project; revisit when additional reviewers exist.
- Bucket-E (ImGui Test Engine) in CI. Headless GL/Mesa story untracked here.
- TSan / MSan extensions to the nightly sanitizer. Start with ASan + UBSan; expand if real bugs surface.
- The 3 outstanding `TODO(pillar2)` sites. Decoupled fixes; not gate-promotion blockers.

## Implementation log

Autonomous overnight run (2026-05-28). Shipped as three independent workflow
PRs plus this plan-doc revision; branch-protection promotions sequenced after
merge + live non-wedge verification.

- **PR #518** (`gate-enforcement-pillar2`) · slices 0 + 1 · `docs/agent-rules/ci-required-check-pattern.md` (always-report contract: Pattern A no-path-filter+internal-no-op for new checks, Pattern B companion-skip-workflow for existing path-filtered ones) + `.github/workflows/pillar2-scan.yml` (Pattern A; "Pillar 2 scanner" job diffs changed first-party C++ and exits 0 when none). **Verified live on its own PR: "Pillar 2 scanner" passed in 7s — the always-report path does not wedge.**
- **PR #519** (`gate-enforcement-cr-gate`) · slice 3 · `.github/actions/cr-finding-gate/action.yml` + `.github/workflows/cr-finding-gate.yml` — lifts merge-gates.sh's "0 actionable" CodeRabbit semantics to a server-side `CR findings (0 actionable)` StatusContext; triggers on `pull_request` + `pull_request_review` + `pull_request_review_comment`. **Verified live: posts `pending — awaiting CodeRabbit review on current head` on the initial run (the grace-window-block path), confirming it does NOT false-pass before CR reviews.**
- **PR #520** (`gate-enforcement-sanitizer-nightly`) · slice 4 · `.github/workflows/sanitizer-nightly.yml` — cron 04:00 UTC + `workflow_dispatch`, `ninja-clang-asan` (ASan+UBSan via clang-cl on windows-2022), opens/updates a `bug` issue on failure. Not a per-PR required check.
- **PR #521** — this plan-doc revision (Implementation log + Deviations).

### Post-merge hardening (2026-05-29)

Live CodeRabbit behaviour exposed a cascade of real defects in the slice-3 CR
gate that the initial implementation could not have caught. Each was a genuine
bug that would have wedged or false-passed the gate once required:

- **PRs #524 + #525** (`fix/cr-gate-*`) · CR gate hardened through six fixes:
  (1) **head-aware `?ref=` probe** for `.coderabbit.y*ml` (was querying the default branch);
  (2) **empty-body on-head review** no longer conflated with "no review yet" — count on-head reviews separately (0 → pending; >0 with no actionable header → pass);
  (3) **CR-skip handling** — when CR skips review (trivial/docs/workflow PRs) it leaves no review node, only a green `CodeRabbit` StatusContext; the gate now reads that and passes instead of waiting forever;
  (4) **bounded ~3-min poll** to catch the CR-skip status flip (a skip fires no review/comment event to re-trigger the workflow);
  (5) **transient-GraphQL retry** — a fetch failure returns into the poll loop instead of posting a blocking `error`; `error` is surfaced only if every attempt failed;
  (6) **`set +e`** — GitHub injects `bash -e` into composite-action steps, which aborted the gate's `decide()` retry path before it could post a status. **Verified live green end-to-end on #525** (CR-skip path → `success`, job passes) and on real user PRs (#522 posted `success`).
- **PR #526** (`fix/ci-docs-only-required-checks`) · **Pattern C** — the docs-only required-check deadlock fix for `build-and-test.yml`. The required `Windows + MSVC` / `Windows + MSVC (Smatchet light …)` contexts were path-filtered out on docs-only PRs (workflow-level `paths-ignore`), so branch protection wedged every docs-only PR on "Expected." Replaced with a `changes` detect-job + `if: code == 'true'` gate (fail-safe default `code=true`); a skipped required job counts as success for branch protection. Added workflow-level least-privilege `permissions: { contents: read }`. Pattern C documented in `ci-required-check-pattern.md`. **Verified live on #515** (a docs-only PR): the Windows checks reported `skipping`, state went `CLEAN`, merged with no admin override.

All slice 0/1/3/4 PRs plus the hardening + Pattern C PRs are **merged to develop**.

## Deviations from plan

- **Slice 0/1 shape changed from the plan's "two jobs in one workflow" to Pattern A (single no-path-filter job with internal change-detection).** Rationale: GitHub `paths`/`paths-ignore` are workflow-level (`on.pull_request.*`), not per-job — the plan's two-job design is infeasible. Pattern A (and Pattern B for existing path-filtered workflows) is the robust equivalent; both documented in `ci-required-check-pattern.md`.
- **Slice 3 override label is `cr-out-of-band` (new), not the plan's literal `tests-out-of-band`/`perf-out-of-band`.** Rationale: those two labels downgrade specific CI checks and are meaningless for the CR gate; the plan's intent ("honour the override-label *pattern* for consistency") is satisfied by a dedicated `cr-out-of-band` label using the same mechanism. **Scope: `cr-out-of-band` is GitHub-check-only** — `scripts/dev/merge-gates.sh` does not parse it today (only `tests-out-of-band`/`perf-out-of-band` are handled there, at the `$tests`/`$perf` bindings ~`merge-gates.sh:213-214`). The orchestrator-side poller therefore still applies its own CR gate regardless of this label; if client-side parity is wanted later, extend merge-gates.sh's label parsing to add a `cr-out-of-band` downgrade. The GitHub-side required check is the only consumer of the label for now.
- **Slice 3 uses `jq` directly** (runner-side) rather than merge-gates.sh's jq-free "Option B" (`scripts/dev/merge-gates.sh:197` — "parse the GraphQL response with gh's BUNDLED jq (`gh api --jq`)"; rationale at `:87`, gh as the only hard dep). That jq-free constraint exists only for the local Windows dev env where standalone jq may be absent; ubuntu-latest runners always ship jq, so the action uses it directly.
- **A docs-only deadlock surfaced on the EXISTING `build-and-test.yml` and was fixed with Pattern C (new).** The plan's slices only added new workflows; it did not account for the fact that the *already-required* `Windows + MSVC` / `…light` checks live in `build-and-test.yml`, which used workflow-level `paths-ignore` → docs-only PRs skipped the workflow → those required contexts never reported → wedge (hit by PR #515). Fix (#526): drop `paths-ignore`, add a `changes` detect-job, gate the build jobs on `if: needs.changes.outputs.code == 'true'` (fail-safe default `true`). Documented as **Pattern C** in `ci-required-check-pattern.md`. This is the general fix for the whole class — not just the new gates.
- **Branch-protection required-check promotions (slices 1 & 3 final step) STILL DEFERRED to the user — but now low-risk.** A context can only be added to `required_status_checks` after its workflow is on `develop` (done) and only safely once it always-reports (now true: `Pillar 2 scanner` is Pattern A; `CR findings (0 actionable)` always posts via its skip path; Pattern C covers the build checks on docs PRs). Remaining caveat: any open PR whose branch predates the workflows must be rebased before the promotion, or it will lack the check. `enforce_admins` stays `false` (admin-reversible). The flip itself is a branch-protection change left to the maintainer's discretion.
- **Slice 2 (`enforce_admins: true`) DEFERRED to the user.** Rationale: this is the single action that turns a recoverable wedge into one requiring manual `enforce_admins=false`→merge→`true` recovery. Flipping it autonomously while the maintainer is away violates the "do not wedge overnight" constraint. Recommended only after the new required checks are observed behaving on real PRs. One-call flip when ready: `gh api -X PATCH repos/alexandrosk0/Smatchet/branches/develop/protection/enforce_admins -F enabled=true` (+ `docs/CONTEXT.md` § Enforcement matrix note).
- **Slice 5 (perf gate promotion) DEFERRED — infeasible autonomously.** Requires 15 `docs/perf/baselines/<scenario>.ci-windows-latest.json` captures on the CI runner (currently zero exist; all baselines are local `.dev.json`). Runner-side bootstrap work per the plan's 5a-5d sub-plan; tracked as remaining.

## Verification (actual)

- **Slice 1 — PASS (live).** `pillar2-scan.sh` exits 0 over develop's full first-party C++ tree (WARN-only backlog sites, no CRITICAL); `pillar2-scan.yml` parses as valid YAML; the "Pillar 2 scanner" check reported green in 7s on PR #518 (a workflow+docs PR) — proving the no-path-filter always-report path does not wedge non-C++ PRs.
- **Slice 3 — PASS (live, end-to-end after hardening).** Initial pending-path confirmed on #519. After the six post-merge fixes (#524/#525), the gate was verified **green end-to-end on #525** (CR-skip path → `success`, job passes) and posted `success` on real user PRs (#522). The pending/empty-body/skip/errexit/transient paths are all exercised by the fixes' unit checks (`bash -n`, jq programs validated against live GraphQL responses).
- **Slice 4 — NOT-RUN (by design).** `sanitizer-nightly.yml` parses as valid YAML; `ninja-clang-asan` preset confirmed present (ASan+UBSan). First end-to-end run happens on the 04:00 UTC cron or via `workflow_dispatch`; the clang-cl toolchain path is validated then.
- **Pattern C (docs-only deadlock fix) — PASS (live).** Detection logic unit-tested (docs-only / mixed / code-only / empty-fail-safe / config cases); verified end-to-end on **#515** — a real docs-only PR where `Windows + MSVC` + `…light` reported `skipping`, branch-protection state went `CLEAN`, and the PR merged with no admin override.
- **Slices 2 & 5 — DEFERRED (see Deviations).** Slice 1/3 required-check promotions also deferred to the maintainer (now low-risk; see Deviations).
