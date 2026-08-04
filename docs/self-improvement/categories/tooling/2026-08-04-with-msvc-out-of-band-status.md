- 2026-08-04 · orchestrator · [tooling] · P3 — `scripts/dev/with-msvc.ps1` signals its own "no usable MSVC toolchain" failure **in-band**, as exit code `78`, on the same channel it uses to propagate the wrapped command's exit code; a wrapped command that ever returns 78 would be misreported by `build.ps1` as a missing toolchain
  Details: Raised by CodeRabbit on PR #1933 (Major, `scripts/dev/with-msvc.ps1:27`) and accepted as a
    known residual rather than fixed there. The wrapper's tail is `& $exe @rest; exit $LASTEXITCODE`,
    so *every* code it emits other than its own four failure paths belongs to the child. PR #1933
    already moved those four from `2` to `78` precisely because `2` was a **live** collision — cmake,
    ninja and ctest all return 2 routinely, so an ordinary failed build printed "no usable MSVC
    toolchain (see the with-msvc line above)" and a winget install hint. `78` was picked to sit
    outside the range those tools use, which downgrades the collision from reachable to theoretical:
    the only command `build.ps1` ever wraps is `powershell -File build_and_run.ps1`, whose failure
    codes are cmake/ninja/ctest's 1/2/8 and PowerShell's 1. Nothing in the call graph returns 78
    today. But the ambiguity is structural, not numeric — any single-channel scheme has it, and a
    future wrapped command (or a future `build.ps1` that wraps something else) re-opens it.
  Concrete next action: give the wrapper an **out-of-band** status channel and stop overloading the
    exit code. Cheapest shape that fits the existing sandbox harness: have `with-msvc.ps1` write a
    sentinel file (path passed in via an env var, e.g. `SMATCHET_MSVC_STATUS_FILE`) on each of its
    four failure paths, and have `build.ps1` key its install hints on *that file's presence* rather
    than on `$LASTEXITCODE -eq 78`, while still propagating whatever code came back. Two call sites
    must move together: `build.ps1`'s else-branch currently reads only `$LASTEXITCODE`, and
    `scripts/dev/local/test-build-wrapper.ps1` stubs the wrapper **by exit code alone** — test 7's
    3-row table (78/msvc, 78/clang, wrapped-exit-2) would need its stub to write the sentinel too,
    and gains a fourth row: a wrapped command that returns 78 *without* the sentinel must propagate
    78 and print no hints. That fourth row is the assertion the current design cannot make, and is
    the reason to do the work at all. Est ~0.5d.
  Cross-ref: `scripts/dev/with-msvc.ps1` (`$ToolchainMissingExit`, the four `exit $ToolchainMissingExit`
    sites, and the `& $exe @rest; exit $LASTEXITCODE` tail that creates the sharing);
    `build.ps1` (the `-eq 78` branch); `scripts/dev/local/test-build-wrapper.ps1` (test 7);
    `docs/agent-rules/build.md` § Entry point (the documented contract that would change);
    `docs/plans/shipped/dev-onboarding-first-run-quickstart.md` § Deviations (the bullet that
    requires this entry); CodeRabbit thread on PR #1933 (comment `3714335162`).
  Status: open
  Last-reviewed: 2026-08-04
