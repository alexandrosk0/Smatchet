# Plan — promote `Fuzz smoke` deterministic build to a merge blocker
<!-- plan-date: 2026-06-16 -->

Status: `shipped` (#1323, merged 2026-06-16 — `Fuzz smoke` deterministic build added to the merge-gate block allow-list; stochastic run stays advisory) · Owner: orchestrator · Slice: testing-surface.md §6 follow-up (fuzz-gate hardening)
Created: 2026-06-16

## Goal

Make the **deterministic** half of the `Fuzz smoke (Linux libFuzzer)` CI job (configure →
build drivers → `ctest -runs=0`) a merge blocker on `develop`, WITHOUT gating the **stochastic**
time-boxed libFuzzer run. Closes the #1301 escape class — a libFuzzer driver that fails to
compile reds the check but was waved through because the whole check was non-required and not on
the meant-to-block allow-list.

## Context

`Fuzz smoke` is ONE CI job = ONE check name folding two failure modes:

- **Deterministic** (configure / build-drivers / `ctest -runs=0`) — a compile/link/smoke break is
  a real broken-develop signal. #1301 was exactly this (a stale `include/` path after a header
  move broke a driver build).
- **Stochastic** (time-boxed libFuzzer run) — a crash is corpus-luck discovery, not necessarily
  caused by the PR. Gating it would jam the merge-poller the same way the Mesa `Bucket-` lanes did
  (removed 2026-06-15).

Naively adding `Fuzz smoke` to `MERGE_GATES_BLOCK_ALLOWLIST_RE` would gate BOTH halves → poller
jam. So the two changes MUST ship together atomically:

1. `continue-on-error: ${{ github.event_name != 'schedule' }}` on the **time-boxed fuzz step
   only** — masks a stochastic PR crash (advisory) while configure/build/ctest carry NO
   `continue-on-error` (a real build break still reds the check). On the nightly `schedule`,
   `continue-on-error` evaluates false → a fuzz crash hard-fails → the "Open or update issue" step
   (`failure() && schedule`) still fires.
2. Add `Fuzz smoke` to `MERGE_GATES_BLOCK_ALLOWLIST_RE` so the (now deterministic-only) red blocks.

GitHub Actions semantics that drive the artifact/summary guards: `continue-on-error: true` masks
step failure so `failure()` returns FALSE in later steps, but `steps.fuzz.outcome` captures the
PRE-mask result (`'failure'`). So the crash-artifact upload + job-summary steps switch from
`if: failure()` to `if: ${{ failure() || steps.fuzz.outcome == 'failure' }}` to still preserve the
PR repro corpus on an advisory crash.

## Files to modify

- `.github/workflows/fuzz-smoke.yml` — add `id: fuzz` + `continue-on-error: ${{ github.event_name
  != 'schedule' }}` to the time-boxed fuzz step (with the rationale comment); switch the two
  failure-guarded steps (crash-artifact upload, job-summary) to `failure() || steps.fuzz.outcome ==
  'failure'`. "Open or update issue on nightly failure" UNCHANGED.
- `agents/scripts/core/merge-gates.sh` — add `Fuzz smoke` to the `MERGE_GATES_BLOCK_ALLOWLIST_RE`
  constant (single source of truth, spliced into the jq `$blocking`/`$failing` filters); extend the
  three prose enumerations (`$blocking` comment, `$failing` "deliberately tight" comment, the
  "Extend the regex" history block) with the `Fuzz smoke` member + the #1301 / continue-on-error
  rationale.
- `tests/bats/merge_gates.bats` — two new cases before the Bucket-E case: (1) non-required
  `Fuzz smoke (Linux libFuzzer)` conclusion FAILURE + green required `build` → asserts `status -eq
  1` && `"1 fail"`; (2) IN_PROGRESS (conclusion null) → asserts `status -eq 1` && `"1 pending"`.
- `AGENTS.md` § Merge gates — add `Perf PR-fast` / `Android security gate` / `Fuzz smoke` to the
  meant-to-block allow-list enumeration with the continue-on-error pairing note (#1301).
- `docs/self-improvement/postmortems.md` — #1308 owed RCA (override-legitimate) + #1301
  discretionary note (this gate is the preventing control).
- `docs/self-improvement/categories/tooling.md` — #1308 4th-recurrence note on
  `coverage-gate-platform-else-arm-exemption` residue (a).

## Verification

- `bash tests/bats/merge_gates.bats` — full suite green incl. the 2 new Fuzz-smoke cases.
- `shellcheck agents/scripts/core/merge-gates.sh` — clean.
- `bash agents/scripts/core/merge-gates.sh --selftest` if present — asserts the contract-card
  tokens (the allow-list constant lives in AGENTS.md § Merge gates).
- `actionlint .github/workflows/fuzz-smoke.yml` if available — the `continue-on-error` expression
  + the `steps.fuzz.outcome` guards parse.

## Perf gate

N/A — diff is CI workflow + merge-gate shell + bats + docs only. No `Source/Core/` C++ touched.

## Deviations

- The plan originally expected a stale `Bucket-` entry in `AGENTS.md` § Merge gates to remove. On
  `develop` it was ALREADY corrected (Bucket- noted dropped 2026-06-15). So the AGENTS.md edit was
  reduced to ADDING the new allow-list members, not a Bucket- removal.
