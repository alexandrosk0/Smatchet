# All gates blocking — flip every CI test gate to merge-blocking + fix the reds

> **Slug**: `all-gates-blocking`
> **Status**: `active`
> **Created**: 2026-07-05 · **Owner**: orchestrator (merge-gates + CI workflows)
> <!-- index-summary: Retire the curated meant-to-block allow-list: the poller blocks on ANY red/pending check; every advisory lane unmasked + renamed; the two genuine reds (emulator boot-race, httplib zstd) fixed; required-contexts set extended for native auto-merge parity. -->

## Intent (originating ask)

User: "make all test gates blocking and fix them all" — in the context of the six-PR
merge session that surfaced advisory mobile lanes failing without blocking anything.

## Problem

Three tiers of gate softness existed:

1. **Poller scope** — `merge-gates.sh` blocked only on required checks + a curated
   *meant-to-block* allow-list (`Coverage|Sanitizer|Perf PR-fast|Android security
   gate|Fuzz smoke|Bucket launch-smoke|Intent section|Plan-lock gate`). The list grew
   **one gate-escape postmortem at a time** (#923 Coverage, #1237 pending-ASAN,
   #1301 fuzz compile-break, …) — every check not yet on it was a future escape.
2. **Workflow masks** — 30 `continue-on-error` masks (2 job-level ARM64/WoA legs +
   25 step-level "green-with-annotation" WoA steps + bucket-C job + texture-guard
   step + perf-full visible-cue stage) made real failures invisible even as reds.
3. **Advisory-by-omission** — the OSV CVE scan ran with no `--fail-on`; 9 checks
   carried an "advisory" name token that the poller's blocking filter explicitly
   exempted.

Two lanes were **genuinely failing** and had to be fixed before any flip
(a blocking flip on a red/flaky lane wedges develop):

- **Mobile — Android emulator smoke**: ~23 % of develop runs failed. Root cause
  (run 28730560601): the runner action quick-boots a cached `default_boot`
  snapshot; the restored state has `sys.boot_completed=1` **before** the `input`/
  `settings` services are registered, so the action's own post-boot
  `input keyevent 82` throws `ServiceNotFoundException("input")` and kills the job
  before the smoke script runs.
- **Mobile — Android NDK arm64**: intermittent `httplib.h:507 fatal: 'zstd.h' not
  found` — **already fixed upstream** (`3447bd08`/#1604 pins
  `HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF`); the observed rerun failure was a stale
  merge-ref artifact (reruns don't re-merge). Verified green on post-fix develop.

Verified before flipping: on real code pushes (e.g. the #1581 merge run) **every
advisory lane completed green with zero masked step failures** — ARM64 cross, both
WoA legs (a real windows-11-arm runner runs them), bucket-C, texture-guard, all
mobile lanes. Unmask + block is therefore safe; only the emulator flake needed code.

## Approach

1. **Fix the emulator boot-race** (`mobile-emulator-smoke.yml`): add
   `-no-snapshot-load` to the smoke leg → **cold boot** every run;
   `sys.boot_completed` then genuinely implies services-up (the AVD cache still
   saves the system-image download; only the quick-boot fast-path is forgone,
   ~60–90 s). Rename drops the `, advisory` token.
2. **Unmask + rename** (`build-and-test.yml`, `dep-cve-sbom.yml`, `perf-full.yml`):
   - Remove all job-level + step-level `continue-on-error` from: ARM64
     cross-compile, WoA native ctest leg, WoA installer smoke (25 step masks + 3
     job masks), texture-guard scenario step, bucket-C **job** level, perf-full
     visible-cue stage (with `always()` added to the three follow-up perf steps so
     a visible-cue red can't swallow a perf-regression issue).
   - Drop the `advisory` token from all 9 check names (POSIX core / Android NDK /
     Android APK / emulator smoke / ARM64 cross / WoA ×2 / C++ lint / CVE+SBOM).
   - Promote the OSV scan per its own recipe: `--fail-on HIGH` (allowlist already
     seeded: OSV-2022-126 md4c, no upstream fix).
   - **Four deliberate step-level masks survive** (documented design, not debt):
     fuzz-smoke's stochastic fuzz step (#1301 — deterministic build/ctest still
     red the check); bucket-C's per-scenario golden-diff step (goldens are
     per-developer GPU bootstraps — non-authoritative under llvmpipe; the
     lane-integrity sentinel + `Bucket launch-smoke` carry that lane's teeth);
     bucket-E's per-test ImGui run step (render-dependent failures under
     llvmpipe, a good run is ~71/74; broken/passed-nothing hard-fails via the
     unmasked lane-integrity step); cpp-lint's cppcheck step (compile-DB-free —
     unavoidable false positives; the lint-catch-all [error] tier blocks).
3. **Flip the poller to block-on-any-red** (`merge-gates.sh`):
   `MERGE_GATES_BLOCK_ALLOWLIST_RE="."` — every check name matches, so ANY
   non-required red **or pending** check blocks. The one exemption kept: a name
   containing `advisory` (now a sanctioned, name-visible convention; no lane uses
   it). Consumers (`safe-admin-merge.sh`, `postmortem-owed.sh`) source the constant
   and follow automatically. Out-of-band label escapes + `SKIP_MERGE_GATES`
   unchanged.
4. **Bats contract flips** (`tests/bats/merge_gates.bats` + fixture):
   `merge_gates_pass.json`'s `non-required-fail` → `non-required-fail (advisory)`
   (the fixture now exercises the escape); "Bucket-E FAILURE does NOT block" →
   **blocks**; advisory-IN_PROGRESS test retitled to pin the name-escape; new
   "arbitrary non-required check IN_PROGRESS blocks as pending" test.
5. **Required-contexts extension** (`project.config.json` § branch_protection +
   § ci, applied post-merge via `setup-branch-protection.sh`): +20 contexts — every
   **always-reporting** gate (jobs in workflows without workflow-level `paths:`
   filters; GitHub counts a `skipped` conclusion as satisfying, so `if:`-gated jobs
   are safe). This narrows the native-auto-merge race (arming `--auto` early merges
   on required-only). Path-filtered workflows (emulator smoke, dep-cve-sbom,
   fuzz-smoke, CodeQL) stay poller-gated — a required check that never reports
   wedges as "Expected — waiting".
6. **Docs**: AGENTS.md § Merge gates CI clause rewritten (block-on-any-red);
   `docs/agent-rules/merge-gates.md` item 1 + the `--auto` gap bullet updated.

## Out of scope

- WARN-tier **local lint** rules (`unused-symbol-under-config-guard`,
  `pr-numbered-temporal-comments`, `interface-doc`) — calibration gates by design,
  not CI test gates; graduating them is a separate call with its own precedent
  (ADR-0015 WARN-first).
- CI-native golden bootstrap for bucket-C's per-scenario diff (would let the last
  bucket-C mask go) — backlogged.
- The WoA lanes stay push-only post-merge backstops (deliberate: an unschedulable
  ARM runner must never wedge a PR); "blocking" for them = an unmasked red on
  develop's checks.

## Perf-gate

No `Source/Core/` (or any product C++) change — workflows, poller shell, bats
fixtures, config, docs only. No perf surface.

## Verification

- [ ] `merge_gates.bats` full suite green after the contract flips (incl. the new
      arbitrary-pending test + flipped Bucket-E test).
- [ ] `osv-scan.py --fail-on HIGH` green locally against the generated SBOM
      (allowlist absorbs OSV-2022-126).
- [ ] All edited workflow YAML parses (python yaml.safe_load).
- [ ] `test-lint-rules.sh --diff origin/develop` + doc-validation green.
- [ ] PR's own CI: the renamed checks report under their new names and pass; the
      poller (new logic, from this branch) gates the merge.
- [ ] Post-merge: `setup-branch-protection.sh` applied (live protection = config);
      first mobile-touching develop push boots the emulator cold and passes.
- [ ] Post-merge watch: one full develop push cycle with zero new reds.

## Implementation log

_(appended as shipped)_

## Deviations

- `ci.required_checks` (the config mirror consumed by `project-config.sh`) updated
  in lockstep with `branch_protection.required_contexts` — the two lists were
  identical before and stay identical.
- The old dep-cve-sbom promotion note prescribed adding it to branch_protection;
  deviated (kept poller-only) because the workflow is path-filtered and a
  required-but-never-reporting check wedges unrelated PRs — recorded in the
  workflow header.
- **Pre-ship code-review round (2026-07-05) reshaped the required-contexts set**
  (4 HIGH findings, all applied):
  - `Intent section` + `Plan-lock gate` NOT added to required_contexts after
    all — ADR-0022 and plan-lock-enforcement Q7 explicitly rejected the
    branch-protection route (the `*-out-of-band` label hatches cannot reach
    GitHub protection, and `plan-lock-gate.yml` has no `labeled` re-trigger).
    They block via the poller's block-on-any-red, where the hatches work.
  - The four Mesa lanes (bucket-C / bucket-E ×2 / texture-guard) NOT added
    either — their red mode is infra/dead-harness with a flake history; as
    required contexts a recurrence would wedge with admin-merge as the only
    escape (no `bucket-out-of-band` exists). Poller-blocking preserves triage
    room. `Bucket launch-smoke` (stable, deliberately graduated) stays required.
  - `High-integrity baseline/narrowing` NOT added — push-only jobs skip on
    every PR (vacuously satisfied; implying protection that doesn't exist).
  - `C++ lint` finished its deferred promotion (job-level mask dropped so the
    lint-catch-all [error] tier has real teeth); the cppcheck step stays
    report-only (fourth sanctioned survivor). bucket-E + texture-guard job-level
    masks dropped (their step-level design carries the nuance).
  - `MERGE_GATES_MAX_POLLS` default 60 → 90: the pending-hold now spans every
    lane incl. bucket-E's 45-min cap.
  - `pr-blocked-why.sh` fallback literal updated from the stale curated list to
    "." (a silent source-failure no longer reverts the diagnostic to curated-era
    semantics).
