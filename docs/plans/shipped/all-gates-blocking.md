# All gates blocking — flip every CI test gate to merge-blocking + fix the reds

> **Slug**: `all-gates-blocking`
> **Status**: `shipped` (2026-07-05 — flip live on develop; texture-guard held advisory per § Deviations)
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

- [x] `merge_gates.bats` 164/164 green after the contract flips (incl. the new
      arbitrary-pending test + flipped Bucket-E test); safe_admin_merge 22/22,
      postmortem_owed 31/31, safe_merge 14/14, pr_status_watch 13/13;
      safe-admin-merge --selftest 18/18.
- [x] `osv-scan.py --fail-on HIGH` green locally against the generated SBOM
      (allowlist absorbs OSV-2022-126).
- [x] All edited workflow YAML parses (python yaml.safe_load).
- [x] doc-validation green (test-docs 14/0); pre-ship gates clean at every push.
- [x] PR #1619's own CI: renamed checks reported under the new names and passed —
      including **Mobile — Android emulator smoke (7m26s PASS)** validating both
      lane fixes (cold boot + stale-APK uninstall) end-to-end.
- [x] Post-merge: `setup-branch-protection.sh` applied — live protection = the
      22-context config, verified via the protection API.
- [x] Post-merge watch: develop push green. The first two pushes surfaced (and
      fixed) the WoA whisper/ggml break and a stale-APK/emulator second bug; the
      third surfaced the texture-guard llvmpipe hang → that ONE lane held advisory
      (below). Every other lane green on develop; the flip is live and enforced.

## Implementation log

- **2026-07-05 — shipped as PR #1619** (squash `05f1f2f4`). Three CR findings
  triaged across two rounds (poll-timeout cap derived from the poll budget;
  stale ANDROID_BUILD bullet); the emulator lane failed once on the PR head and
  exposed a SECOND latent bug the quick-boot snapshot had been hiding — the
  cached AVD userdata carried a previous runner's install signed with a
  different debug keystore (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`); fixed with
  uninstall-before-install in the smoke script. Branch protection applied
  post-merge (22 contexts live).
- **2026-07-05 — first develop push surfaced the WoA lanes' real state.** Both
  Windows-on-ARM legs failed at configure: whisper.cpp's ggml hard-errors
  "MSVC is not supported for ARM, use clang" on a native ARM64 host
  (`CMAKE_SYSTEM_PROCESSOR=ARM64`). The step-level masks had green-washed this
  since the lanes' creation — step `continue-on-error` rewrites the step's API
  *conclusion* to success, which is exactly why the pre-flip "zero failed step
  conclusions" verification read healthy (a false negative this plan's Problem
  section overstated as "genuinely green"; the CROSS leg truly was — CMake on an
  x64 host reports AMD64 so ggml's ARM branch never fires). Fix:
  `SMATCHET_WITH_WHISPER=OFF` on `ninja-publish-msvc-arm64` (a native-MSVC ARM64
  Whisper build was never possible; preset description records the ggml
  citation + re-enable conditions) + a `workflow_dispatch` trigger on
  build-and-test.yml so the push-only WoA legs are testable from a topic branch
  instead of merge-and-watch.
- **2026-07-05 — texture-guard held ADVISORY (the one lane the flip could not
  graduate).** After WoA went green, the develop push red on
  `Mobile texture-guard smoke` — the `--spawn` child HANGS under llvmpipe
  (`rc=124` at the inner `timeout`, all 3 retry attempts). Reliability check:
  ~3/13 green pre-flip, RED on #1619 + #1620 heads + the develop push — a real
  product/harness deadlock in the forced-fault render path under software GL, not
  a retryable flake. **Corrective**: reverted this lane to advisory (the
  poller's `advisory`-name escape + step mask) — its one live user — with an
  inner `timeout 300` so a hang ends deterministically, and backlogged the fix +
  re-graduation criteria
  (`docs/self-improvement/categories/infra/2026-07-05-texture-guard-llvmpipe-spawn-hang.md`).
  **Honest scope correction**: the plan's "every advisory lane verified genuinely
  green before unmasking" (Problem/Approach) held for bucket-C/E/Jira/launch-smoke
  (zero real failures over 13 pushes; they carry retry-on-collapse + lane-integrity
  teeth) but was WRONG for texture-guard — the run I sampled was one of its lucky
  greens. So "all gates blocking" shipped as "all gates blocking except one
  documented, backlogged, genuinely-flaky render lane," not a silent green-wash.

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
