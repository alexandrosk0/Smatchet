- 2026-08-04 · orchestrator · [tooling] · P2 — six gate scripts under `agents/scripts/core/` still carry the **resolve-only python probe** that PR #1936 (`f423605a`, 2026-08-04) fixed everywhere else: `command -v python3` matches the Windows Store App Execution Alias, which resolves on PATH but exits non-zero on run, so the "no python" guard never fires and the script dies mid-run instead of skipping cleanly
  Details: Hit live while validating a PR body — `bash agents/scripts/core/check-pr-intent.sh <body-file>`
    printed "Python was not found; run without arguments to install from the Microsoft Store" and exited
    **49**, not the documented `2` (no python3). The presence probe at `check-pr-intent.sh:20` had already
    passed, so the fail-closed infra-error path was bypassed and the caller got an undocumented exit code
    from the stub. Worked around by symlinking a real interpreter onto PATH ahead of the alias.
    #1936 swept `tests/bats/**` plus `issue-sweep.sh` / `migrate-bugs-to-issues.sh` to the probe-EXECUTE
    form (`"$c" -c ""`); the gate scripts were out of that PR's scope. Remaining sites:
    `check-pr-intent.sh:20`, `sort-applied-md.sh:26`, `test-autonomous-debug-loop.sh:22` (all `python3` —
    these are the ones that hit the alias today, since `python3` has no real entry on a stock Windows
    dev box), plus `test-agent-contract.sh:77`, `test-lint-hook-split.sh:411`, `test-skill-load-log.sh:16`
    (`python` — currently resolve to a real interpreter here, but identical shape and identical failure on
    a machine where only the alias exists). Impact is local-dev-experience only: CI (Ubuntu) has a working
    `python3`, so no gate is wrong on the ship-line — same blast radius as the check-5 emoji false negative
    shipped alongside this entry (PR #1938).
  Concrete next action: apply #1936's canonical probe-EXECUTE form to the six sites — iterate the
    `python3 python py` candidate list and select on `"$c" -c "" >/dev/null 2>&1`, not on `command -v`.
    Mechanical; est ~0.5h including a `tests/bats` assertion that a stub named `python3` which exits
    non-zero is rejected rather than selected. Consider hoisting the resolver into `agents/_shared/` so
    the next script cannot reintroduce the resolve-only shape, and a `test-shell-lint.sh` rule matching
    `command -v python` used as an interpreter-selection probe.
  Cross-ref: PR #1936 (`f423605a`, the exec-validate sweep + the canonical form); PR #1938 (where this
    surfaced); `agents/scripts/core/check-pr-intent.sh` :20; `agents/scripts/core/sort-applied-md.sh` :26;
    `agents/scripts/core/test-autonomous-debug-loop.sh` :22; `agents/scripts/core/test-agent-contract.sh` :77;
    `agents/scripts/core/test-lint-hook-split.sh` :411; `agents/scripts/core/test-skill-load-log.sh` :16.
  Status: open
  Last-reviewed: 2026-08-04
