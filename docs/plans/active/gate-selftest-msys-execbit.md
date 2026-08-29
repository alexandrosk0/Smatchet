# Plan — gate-selftest MSYS exec-bit heuristic fix

> **Slug**: `gate-selftest-msys-execbit` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

`agents/scripts/core/test-gate-selftests.sh --selftest` fails on Windows Git
Bash with 11 `raw self-exec was NOT flagged` FAILs while `--check` passes,
making the local `scripts/dev/test-docs.sh` mirror report a false red (18/19)
on every docs slice. CI (Ubuntu) is unaffected.

Root cause: `run_selftest` plants non-executable fixture scripts in a mktemp
dir (untracked, not even a git repo), so the mode probe falls through to the
fs-bit arm (`[ -x "$f" ]`). On MSYS/NTFS that probe is a shebang HEURISTIC —
Git Bash reports ANY file starting with `#!` as executable (verified:
`printf '#!/usr/bin/env bash\n' > f; [ -x f ]` → true) and `chmod -x` cannot
clear it. So `_nonexec` is 0 and the mode-100644 raw-self-exec rule never
fires on the fixtures.

After this lands: `--selftest` and `--check` both pass on Windows Git Bash AND
Linux, with `# selftest: asserts-failure` semantics intact.

## Approach

Add an env knob `SMATCHET_GATE_SELFTEST_FORCE_NONEXEC` consumed only by the
untracked-file fallback arm of the mode probe (`1` = treat as 100644, `0` =
treat as 100755, unset = fs bit as today). `run_selftest` sets it to `1` as a
dynamically-scoped `local` (visible in the `run_check` calls it makes,
auto-unset on return), and flips it to `0` around the one fixture that
exercises the executable-script exemption. Real-tree `--check` behaviour is
unchanged (knob unset; tracked files keep using the git-index mode).

Chosen over auto-detecting MSYS because NTFS has no real exec bit to fall back
to — any "detect and stat harder" path still ends at a synthesized bit; the
knob makes the fixtures' intended mode explicit and deterministic on every
platform. On Linux the forced values match what the fs bit already reports, so
Linux behaviour is byte-identical.

## Files to modify

1. [agents/scripts/core/test-gate-selftests.sh](../../../agents/scripts/core/test-gate-selftests.sh) — knob in the mode-probe fallback arm; `local` pin in `run_selftest`; comment update.
2. [tests/bats/gate_selftests.bats](../../../tests/bats/gate_selftests.bats) — regression test pinning both knob directions against a contradicting fs bit (runs on Linux CI, guards the mechanism Windows relies on).

## Existing utilities reused

- `run_check` / `run_selftest` / `_selfexec_hits` in `test-gate-selftests.sh` — untouched except the probe arm; all fixtures and assertions stay as-is.
- `write_exposer` in `gate_selftests.bats` — reused for the new regression fixture.

## Extraction sizing

N/A — nothing extracted or split.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — agent-infra shell script, no product code.
- **Pillar 2 (UI never freezes)**: no impact — no product code.
- **Pillar 3 (never crash)**: no impact — no product code.
- **Pillar 4 (accessibility)**: no impact — no product code.

## Perf-review-system gates

N/A — diff touches no `Source/Core/` (shell + bats + docs only).

## Risks / non-goals

- **Risk**: knob set in a user's environment during a real-tree `--check` would force untracked scan-dir files down one path. Accepted — tracked files (the whole real tree) keep the git-index mode; knob is documented as selftest-internal.
- **Non-goal**: fixing MSYS `[ -x ]` semantics generally, or auditing other gates for the same heuristic (separate sweep if it recurs).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ touched.
- **Bucket E (ImGui Test Engine)**: N/A — no UI touched.
- **Bash-driver scenario / screenshot / sanitizer**: `bash agents/scripts/core/test-gate-selftests.sh --selftest` and `--check` green on Windows Git Bash; `bats tests/bats/gate_selftests.bats` green (incl. new knob test); Linux parity via WSL if available, else the CI bats job is the Linux leg.
- **Build gate**: N/A — no C++ touched (shell/bats/docs-only diff).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green — the very suite whose false red (18/19) this fixes; expect full green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model before finalising; record the outcome in § Verification (actual).
- **Manual residue**: none — all steps scripted.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — nothing deferred; no stale refs to sweep.

- Sweep of other gates for MSYS `[ -x ]` reliance — only this gate's selftest is known-red; no-action unless it recurs.

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
