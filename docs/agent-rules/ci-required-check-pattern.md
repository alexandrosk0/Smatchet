# CI required-check pattern — always-report (avoid the path-filter deadlock)

Plan: [`docs/plans/shipped/gate-enforcement-hardening.md`](../plans/shipped/gate-enforcement-hardening.md) § Slice 0.

> The set of **required status checks** on `develop` is codified + applied by [`agents/scripts/core/setup-branch-protection.sh`](../../agents/scripts/core/setup-branch-protection.sh) (sourced from `project.config.json` § `branch_protection`). The separate decision to require **0 approving reviews** lives in [`docs/adr/0013-solo-no-required-review.md`](../adr/0013-solo-no-required-review.md).
>
> **Config↔live drift — editing the config is inert until the script re-runs.**
> `branch_protection.required_contexts` is the *intended* set; the *enforced* set is
> whatever `setup-branch-protection.sh` last PUT to the live ruleset (a full-replace
> REST call that lives in **no** workflow — nothing re-applies it on merge). Adding a
> context name to the config without re-running the script leaves the live ruleset
> behind, and a check the config *claims* is required is silently waivable. This is
> #1227: Coverage was added to the config but the apply script was never re-run, so
> the live ruleset enforced only 6 contexts and a red-Coverage PR merged clean. After
> editing `required_contexts`, **always** re-run the script and verify with
> `gh api repos/.../branches/develop/protection/required_status_checks --jq '.contexts'`.
> The [`test-required-context-parity.sh`](../../agents/scripts/core/test-required-context-parity.sh)
> gate guards config↔workflow parity (every named context resolves to an emitting job),
> **not** config↔live parity — that one is operational, owned by re-running the script.

## The deadlock

A **required** status check must report on **every** PR or the PR is wedged
("Expected — Waiting for status," unmergeable). GitHub does **not** synthesize a
success for a workflow that was skipped by a path filter. So if a required
check's workflow has a positive `on.pull_request.paths:` filter (e.g.
`perf-pr-fast.yml` filters to `Source/Core/**`), a docs-only PR never triggers
it → the required context never reports → the PR can never merge. The project ships
docs-only PRs constantly, so any required check MUST report unconditionally.

Note: `paths` / `paths-ignore` are **workflow-level** (`on.pull_request.*`), not
per-job — you cannot give two jobs in one workflow different path filters.

## Pattern A — no path filter + internal no-op (preferred for NEW checks)

The simplest always-reporting check: **no `on.paths` filter at all**, one job
whose name IS the required-check context, that computes the relevant changed
files itself and exits 0 when none apply.

```yaml
name: Pillar 2 scanner
on:
  pull_request: { branches: [develop] }
  push:        { branches: [develop] }
jobs:
  pillar2:
    name: Pillar 2 scanner          # ← the required-check context name
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - shell: bash
        run: |
          git fetch --no-tags --depth=1 origin develop
          files=$(git diff --name-only origin/develop...HEAD \
                  | grep -E '\.(cpp|h)$' | grep -E '^(Source/Core/|Source/Plugins/)' || true)
          [ -z "$files" ] && { echo "no first-party C++ changed — pass"; exit 0; }
          bash scripts/dev/<scanner>.sh $files
```

The check reports **every** time (green on no-relevant-change, green/red on the
scan otherwise). No companion job, no deadlock. Cost: the runner spins up even
for docs PRs, but the job is seconds. Use this for any new required gate.

## Pattern B — companion skip-workflow (for EXISTING path-filtered workflows)

When an existing required-candidate workflow already has a positive `paths:`
filter you don't want to restructure (e.g. `perf-pr-fast.yml`), add a **second
workflow file** that emits the **same check name** on the inverse
`paths-ignore`, so exactly one of the pair reports on any given PR:

```yaml
# perf-pr-fast-skip.yml
name: <same job name as the real check, e.g. "Perf PR-fast (windows-2022)">
on:
  pull_request:
    branches: [develop]
    paths-ignore: ['Source/Core/**', 'Source/Plugins/**', 'Source/Standalone/**']  # inverse of the real filter
jobs:
  skip:
    name: Perf PR-fast (windows-2022)   # MUST match the real workflow's job name exactly
    runs-on: ubuntu-latest
    steps: [{ run: 'echo "no perf-relevant paths changed — pass"' }]
```

The real workflow's `paths:` and the skip workflow's `paths-ignore:` are
mutually exclusive by construction, so the required context reports **exactly
once** per PR. The two job `name:`s must be byte-identical (that string is the
required-check context).

## Pattern C — detect-changes job + conditional skip (for EXISTING multi-job workflows)

When a single workflow already carries the required job **and** several
expensive non-required jobs (e.g. `build-and-test.yml`: `Windows + MSVC` +
`Windows + MSVC (Smatchet light …)` plus bucket-C/E + sanitizer), converting it
to Pattern A by no-op-guarding every step is noisy. Instead:

1. Remove the workflow-level `paths-ignore:` so the workflow **always triggers**
   (the required contexts are always created).
2. Add a fast `changes` job that diffs the PR and outputs `code` = "did any
   non-docs path change?" — **defaulting to `true` on any detection
   uncertainty** (empty/failed diff) so a real code PR can never be wrongly
   skipped.
3. Gate each build job with `needs: changes` + `if: needs.changes.outputs.code
   == 'true'`. Dependent jobs (`needs: <build job>`) cascade-skip automatically.

On a docs-only PR the build jobs are **skipped**, and a skipped required job is
treated as **success** by branch protection (unlike a path-skipped *workflow*,
which never reports → wedge). The failure mode is bounded: detection trouble
defaults to running the build, never to skipping a code PR's required check.

This is the form used by `.github/workflows/build-and-test.yml`.

### Multiple skip dimensions (e.g. android-only diffs)

The detect-changes job can output **more than one** skip signal. Beyond `code`
(docs-only → skip the build jobs), `build-and-test.yml` + `perf-pr-fast.yml`
also emit `android_only` — `true` when **every** changed file is an
Android-specific path (`Source/Mobile/**`, `cmake/toolchains/**`) or an
uncompiled docs-class path. The desktop build jobs gate on
`code == 'true' && android_only != 'true'`, so an android-only PR **skips** the
desktop standalone builds (`Windows + MSVC`, `…light`, `Perf PR-fast`) while the
`mobile-android-ndk` job — which gates on `code` alone — still runs. Same
safety as the docs-only case: the skipped jobs are **required** checks, and a
skipped required check counts as **success** for branch protection. Fail-safe
is **FALSE** (run the full desktop suite) on any uncertainty or any non-android,
non-docs path. The two workflows compute `android_only` independently with the
same tolerated set, so they agree on what counts as android-only.

## Pattern D — masked step inside a blocking check (step-level `continue-on-error`, not job-level)

Under block-on-any-red the CHECK always gates (any non-advisory-named red
blocks the poller), so Pattern D is now about scoping WHAT reds the check: a
genuinely-noisy step (golden diff on non-authoritative goldens, stochastic
fuzzing, Mesa render-dependent per-test results, compile-DB-free cppcheck)
keeps a **step-level** `continue-on-error` while an unmasked hard-fail step
catches the broken-lane class — that unmasked step's failure DOES red the check
and DOES block the merge. Job-level `continue-on-error` is banned for gate
lanes: it masks **every** step, so a broken harness / infra failure / "the lane
ran nothing" green-washes the workflow run. Keep artifact upload keyed on the
step `outcome` / `failure()` so the evidence survives a masked failure. The
current sanctioned masked steps: fuzz-smoke's stochastic fuzz run, bucket-C's
per-scenario golden diff, bucket-E's per-test ImGui run, cpp-lint's cppcheck
report (each documented at the step).

Canonical snippet (this is exactly the shape `sanitizer-ubsan-pr` +
`bucket-c-screenshot-diff` already use):

```yaml
jobs:
  advisory-thing:
    name: Advisory thing (not required)
    runs-on: windows-2022
    # NO job-level `continue-on-error: true` — that would mask the lane-integrity
    # step below too. Keep the mask at the step that is genuinely advisory.
    steps:
      - uses: actions/checkout@v4

      - name: Run the advisory thing
        id: run                       # ← step id so later steps read its outcome
        continue-on-error: true       # ← STEP-level: a per-scenario regression is advisory
        run: bash scripts/dev/<advisory>.sh   # writes a lane-status sentinel

      # Hard-fail teeth OUTSIDE the mask: "passed nothing / harness died" is a
      # broken lane, not an advisory regression. A missing sentinel also fails closed.
      - name: Lane-integrity — did not pass-nothing
        run: |
          set -euo pipefail
          test -f "$SENTINEL" || { echo "::error::harness wrote no sentinel — dead lane"; exit 1; }
          # ... assert NOT (Passed==0 && Failed>0) ...

      - name: Upload evidence
        if: always()                  # ← survives a masked failure (or: ${{ steps.run.outcome == 'failure' }})
        uses: actions/upload-artifact@v4
        with: { name: advisory-evidence-${{ github.run_id }}, path: out/, if-no-files-found: ignore }
```

Job-level `continue-on-error: true` is correct ONLY when the **entire** job is
advisory with no broken-lane class worth catching — e.g. the runner-gated /
brand-new-arch legs (`windows-msvc-arm64`, `windows-arm64-native`,
`windows-arm64-installer`) and the static-analysis debt job, which are wholly
non-blocking by design. The out-of-band-label valve (`continue-on-error: ${{
steps.<oob>.outputs.oob == '1' }}` on a required check's run step) is a third,
distinct shape — a *required* check whose mask is conditional on a named escape
label, NOT an advisory job.

**Audit (2026-06, PR-15):** every `continue-on-error` in `build-and-test.yml`
was checked against this. The job-level uses are the wholly-advisory
runner-gated ARM64 legs + the static-analysis debt job (correct — no broken-lane
class to protect); `bucket-c`/`bucket-e` pair job-level advisory with an unmasked
lane-integrity hard-fail step (the hybrid above — correct); the
ASan/UBSan jobs use the conditional out-of-band-label valve (correct). No job was
found using job-level masking where a step-level mask + lane-integrity guard was
the right shape, so no gating semantics were changed.

## Invariant — a path-gated job must run on its OWN PR before merge

A check gated to run only on certain paths (e.g. `sanitizer-ubsan-pr` /
`sanitizer-asan` gate on `changes.outputs.source_core_cpp == 'true'` — only when
`Source/Core/**` or `Source/Plugins/**` C++ changed) must actually **execute**
(not merely SKIP-as-success) on the PR that changes that surface, **before** that
PR merges. The failure class (PR-15 / ubsan-merged-without-executing-validation):
a Core change rides in on a PR whose detect-changes step mis-classified the diff,
the sanitizer job SKIPs, the skip counts as success for branch protection, and the
UB validation never ran on the code it was supposed to cover — merged green
without ever executing.

Convention:
- The detect-changes gate (`source_core_cpp`) is **fail-safe = TRUE on
  uncertainty** (empty/failed diff → run the sanitizer), so the only way to wrongly
  skip is a *non-empty* diff that genuinely matched no Core/Plugins C++ path —
  which is the intended skip. Keep it that way; never make it fail-safe-false.
- On a PR you know touches `Source/Core/**`, **confirm the sanitizer check
  reports a real verdict (green/red), not "Skipped"**, before merging. A
  SKIPPED sanitizer on a Core PR is the smell — treat it as the gate not having
  run, investigate the detect-changes classification, don't merge past it.

Cheap assertion (optional, if a Core-PR self-check is wanted): the
detect-changes job already echoes `source_core_cpp=<bool>` to its log and
`$GITHUB_OUTPUT`. A reviewer/agent can read it from the run
(`gh run view <id> --json jobs` → the `Detect code changes` job log) and assert
it is `true` whenever the PR diff includes a `Source/Core/**` or
`Source/Plugins/**` `.cpp/.h/.hpp` file — i.e. the sanitizer **was** scheduled
to execute, not skipped. This stays a convention (read-and-confirm) rather than a
forced heavy gate; the structural protection is the fail-safe-TRUE default above.

## Invariant

For any context in `required_status_checks`, a job emitting that context name
must **report** on **every** PR regardless of changed paths — either by running
(real verdict) or by being `if:`-skipped (success), never by being
path-filtered out of existence (perpetual "Expected"). Pattern A guarantees it
with one always-on job; Pattern B with a real+skip pair; Pattern C with a
detect-changes gate; Pattern D keeps an advisory job's broken-lane teeth via a
step-level mask. Verify by opening a docs-only PR and confirming the check
reports (not "Expected").

## Two surfaces per workflow: job name vs status context

A workflow that posts a StatusContext under a different name than its job (the
CR finding gate posts `CR findings (0 actionable)` from a job named
`CR finding gate`) is **two independently-failable gates**: branch protection
can require either surface, and only the required one blocks. Observed on
PR #1937: the status context was green (label override worked end-to-end) while
the required *check-run* was absent — its pending workflow run had been
cancelled by GitHub's one-pending-per-concurrency-group collapse, which
creates no check-run at all. When requiring such a workflow, know WHICH
surface is in the required list, and remember that a cancelled-while-pending
run leaves that surface absent-forever (the poller's `required-missing-
cancelled` exit names the run and the `gh run rerun <id>` fix).
