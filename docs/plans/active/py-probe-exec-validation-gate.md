# Plan — py-probe-exec-validation-gate

**Status**: active
**Owner**: orchestrator
**Created**: 2026-08-04

## Context

On Windows, `%LOCALAPPDATA%\Microsoft\WindowsApps\python3.exe` is a Microsoft
Store *App Execution Alias* stub. It sits on `PATH`, so `command -v python3`
resolves it and returns 0 — but running it prints a "run without arguments to
install from the Microsoft Store" banner and exits non-zero. Every shell probe
that selects an interpreter by resolution alone therefore hands back a binary
that cannot execute anything.

PR #1936 fixed the reported instance (`tests/bats/issue_sweep.bats` and the
sibling bats suites, where the resolve-only probe defeated the skip guard so 4
of 6 cases failed instead of skipping). Two same-class sites were explicitly
left out of scope in that PR — [`scripts/dev/doctor.sh`](../../scripts/dev/doctor.sh)
and [`agents/scripts/core/merge-watcher-stuck-nudge.sh`](../../agents/scripts/core/merge-watcher-stuck-nudge.sh)
— and a sweep for the pattern found two more (`agent-eval-run.sh`,
`test-tooltip-wrapwidth.sh`) plus one in the plan-lock substrate itself
(`lock-table-cache.sh`).

The class is *silent-wrong*, not loud-broken: doctor reports an unparseable
python version on the one machine most in need of a truthful report; the
merge-watcher nudge degrades to silence on a machine that has a working
interpreter; the plan-lock cache helper fails, `ltc_covering_slug` returns
"undetermined", and the write-set guard fails open. None of them announce
anything. That is what earns a gate rather than another one-off fix.

## Approach

Two halves.

**1 — fix the remaining pickers.** Replace each resolve-only probe with a
resolve-**and-run** probe: `command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1`.
The canonical shape already exists in-tree at
[`assert-code-unchanged.sh:20`](../../agents/scripts/core/assert-code-unchanged.sh) —
the fixes converge on it. Repeating a ~6-line resolver across shell scripts does
not trip the `duplication` gate (AGENTS.md scopes that rule to first-party C++),
and no shared shell library exists that all five callers already source, so a
copied helper is the honest option here.

**2 — gate the class.** New rule 9 in
[`agents/scripts/core/test-shell-lint.sh`](../../agents/scripts/core/test-shell-lint.sh)
(`SHELL_LINT_PY_PROBE`) flagging a python-interpreter **picker** that resolves
without exec-validating.

The rule deliberately fires **only** on the picker shape — a probe choosing
among ≥ 2 python candidate names. That is the silent-wrong class: the stub wins
the selection over a working later candidate, so the script proceeds with an
interpreter that cannot run. A single-candidate `command -v python3 || exit 2`
hard-require guard is **not** flagged: it selects nothing, so it cannot
mis-select, and it fails loudly where the picker fails silently.

This narrowing is load-bearing rather than cosmetic. `test-shell-lint.sh` has no
WARN tier and no delta-gating — it is whole-tree and binary — so a new rule must
ship against a *clean* tree. A prototype of the broad rule (any resolve-only
python probe) hit 19 of 302 targets; clearing the ~15 single-candidate residual
would mean rewriting every downstream `python3 foo.py` call site to `"$PY" foo.py`,
an unrelated and much riskier diff. Narrowed to the picker shape the population
is 3, all genuine, all fixed here. Precedent for remediating pre-existing
violators inside the rule's own PR:
[`docs/plans/shell-script-self-review-lint.md`](../plans/shell-script-self-review-lint.md)
§ "Pre-existing violator remediation in same PR". The residual is recorded in
the backlog, not silently dropped.

Both rule shapes search for the exec-validation **generically** (any `<cmd> -c`
/ `--version` / `-V` within the window) rather than anchoring it to the probed
variable, because the canonical repo form validates a *different* variable than
it probes (`_p="$(command -v "$_c")"` … `"$_p" -c ""`). A stray unrelated `-c`
in the window only makes the rule quieter, never louder — the safe direction for
a blocking whole-tree rule.

## Files to modify

1. `scripts/dev/doctor.sh` — add `resolve_py()` (exec-validating) immediately
   after `REPO_ROOT=`, before first use; route the tier-resolution block and the
   `python >= 3.10` check through it. The version check must not go through
   `tool_version`, which folds stderr into stdout and would parse the stub's
   banner as "could not parse version" instead of an honest not-found.
2. `agents/scripts/core/merge-watcher-stuck-nudge.sh` — exec-validate the
   `python python3 py` candidate loop.
3. `scripts/dev/agent-eval-run.sh` — replace the `if/elif` resolve-only chain
   with an exec-validating loop.
4. `scripts/dev/test-tooltip-wrapwidth.sh` — its comment already names the Store
   stub but only *reorders* candidates; exec-validate instead.
5. `agents/scripts/core/lock-table-cache.sh` — exec-validate `_ltc_pybin()`;
   additionally strip CR in `_ltc_norm_path` (see § Risks).
6. `agents/scripts/core/test-shell-lint.sh` — rule 9 `check_py_probe()`, header
   rule list, `eight-rule` → `nine-rule`, wire into the per-target loop.
7. `tests/bats/shell_lint.bats` — rule-9 fires / does-not-fire cases; update the
   two tests whose names hardcode the rule count.
8. `tests/fixtures/shell_lint/py-probe-picker-{bad,good}.sh` — new fixtures.
9. `docs/self-improvement/categories/tooling/2026-08-04-py-probe-single-candidate-residual.md`
   — backlog entry for the deliberate residual + the `_lock-json.py` CRLF root
   cause.

## Existing utilities reused

- `non_comment()` and `emit()` from `test-shell-lint.sh` — rule 9 adds no new
  infrastructure.
- The exec-validating probe shape from `assert-code-unchanged.sh:20`.
- Here-strings (`<<<`) throughout rule 9, never `printf | grep -q`: `grep -q`
  exits on the first match and SIGPIPEs the producer, which under this script's
  own `set -o pipefail` reads as "no match". msys bash ignores SIGPIPE, so that
  trap is CI-only — it is exactly what the script's own rules 6 and 8 guard.

## Extraction sizing

N/A — no C++ touched.

## Perf-review gates

N/A — no `Source/Core/` change. Dev tooling only.

## UX Pillars

- **Pillar 1 (performance)** — no impact; nothing ships in the app binary.
- **Pillar 2 (UI never freezes)** — no impact.
- **Pillar 3 (never crash)** — no impact on the product; the plan-lock fix
  removes a fail-open in agent tooling.
- **Pillar 4 (accessibility)** — no impact.
- **Pillar 5 (DRY)** — the repeated shell resolver is out of that gate's scope
  (C++ only) and has no shared-library home; noted rather than hidden.

## Risks / non-goals

- **Residual single-candidate guards (~15).** A `command -v python3 || exit 2`
  hard-require still breaks on a stub-only box — loudly, at the guard, which is
  the acceptable failure mode. Durable fix is a shared `resolve_py` helper all
  scripts source; backlogged, not attempted here.
- **`_lock-json.py` CRLF.** It writes TSV rows via `sys.stdout.write("\n")`
  through a Windows text-mode stdout, so every row arrives CRLF-terminated and
  the trailing CR lands on the last field — the path. `_ltc_norm_path`'s
  exact-match then fails for every locked path: a held lock silently covers
  nothing. This was *masked* until now, because the pre-fix `_ltc_pybin()`
  picked the Store stub, `_lock-json.py` never ran, `ltc_covering_slug` returned
  rc 2 (undetermined) and the guard failed open. Fixing `_ltc_pybin()` unmasked
  it. Mitigated here at the shell normalization chokepoint (which already owns
  path normalization); the python-side `reconfigure(newline="\n")` root fix is
  backlogged — the module carries `from __future__ import print_function`, so it
  needs a guarded py2-tolerant form and does not belong in this diff.
- **No WARN tier.** Rule 9 is blocking from day one because the lint has no
  advisory tier to graduate from. The narrow shape is what makes that safe.
- **Not a general "python must exist" check.** The rule says nothing about
  scripts that require python; only about how they *choose* it.

## Verification

- `bash agents/scripts/core/test-shell-lint.sh` — whole-tree, must pass with
  rule 9 live.
- `bash agents/scripts/core/test-shell-lint-bats.sh` — delta driver.
- `bats tests/bats/shell_lint.bats` — rule-9 fires / does-not-fire.
- `bash scripts/dev/test-docs.sh` — plan/doc link integrity.
- Manual: `bash scripts/dev/doctor.sh` on the Windows box reports a real python
  version rather than "could not parse".

## Out of scope

- Rewriting single-candidate guards (backlogged).
- The `_lock-json.py` newline root fix (backlogged).
- Any non-python interpreter probe (`node`, `jq`, …) — same class in principle,
  no observed failure, no Store-alias equivalent.

## Implementation log

_(filled post-ship)_

## Deviations

_(filled post-ship)_

## Archive

On completion move to `docs/plans/shipped/<slug>.md` and update any inbound
references.
