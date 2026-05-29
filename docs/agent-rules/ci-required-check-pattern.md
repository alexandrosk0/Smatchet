# CI required-check pattern — always-report (avoid the path-filter deadlock)

Plan: [`docs/plans/active/gate-enforcement-hardening.md`](../design/gate-enforcement-hardening.md) § Slice 0.

## The deadlock

A **required** status check must report on **every** PR or the PR is wedged
("Expected — Waiting for status," unmergeable). GitHub does **not** synthesize a
success for a workflow that was skipped by a path filter. So if a required
check's workflow has a positive `on.pull_request.paths:` filter (e.g.
`perf-pr-fast.yml` filters to `Source_Core/**`), a docs-only PR never triggers
it → the required context never reports → the PR can never merge. Smatchet ships
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
                  | grep -E '\.(cpp|h)$' | grep -E '^(Source_Core/|Plugins/)' || true)
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
    paths-ignore: ['Source_Core/**', 'Plugins/**', 'Target_Standalone/**']  # inverse of the real filter
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

## Invariant

For any context in `required_status_checks`, a job emitting that context name
must **report** on **every** PR regardless of changed paths — either by running
(real verdict) or by being `if:`-skipped (success), never by being
path-filtered out of existence (perpetual "Expected"). Pattern A guarantees it
with one always-on job; Pattern B with a real+skip pair; Pattern C with a
detect-changes gate. Verify by opening a docs-only PR and confirming the check
reports (not "Expected").
