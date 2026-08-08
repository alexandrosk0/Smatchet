# 01-spawn-honors-timeout — closed

> Status: Closed (2026-08-07)

**Item:** trivial bug fix ([work-items.md → The loop](../../agent-rules/work-items.md#the-loop) —
skipped spec/design/plan and the pre-implementation gate). **Source:** GitHub issue #1943.
**Shipped in:** PR #1982 (commits `bc3b1cde` fix, `a134db72` review resolution). Pilot item of the
Whip-Process absorption (`docs/plans/absorb-whip-process.md` Phase 5) — first item through the
absorbed loop end-to-end.

## Shipped

`smatchet --spawn` silently ignored an explicit `--timeout`: the spawn driver
(`CliDispatch.cpp:476`) inlined only the frames-derived half of the wait-budget expression while the
in-process driver (`CliCommandRunner.cpp:122`) honored the flag — the timeout error's own hint
advertised a knob the path dropped. Fix extracts the full rule into `ScenarioWaitMs(timeoutMs,
frames)` (`CliArgCoercion.{h,cpp}`): explicit `--timeout` wins, else `(frames / 60 + 30) * 1000` ms.
Both drivers route through the helper, so the budget arithmetic cannot diverge again. Two doctest
cases pin the `--timeout` override (both directions) and anchor the frames-derived fallback as
characterization (default frames → 40 s, CI bucket-E derivation → 90 s, zero-frames floor → 30 s,
negative timeout falls back). Round-1 review also landed a fix: the bucket-E workflow comment
(`.github/workflows/build-and-test.yml`) stated the pre-fix behaviour ("`--timeout` is NOT the
knob", "filed separately") and was rewritten to the `ScenarioWaitMs` rule, keeping `--frames=5400`
and naming `--timeout` as the direct wall-clock dial.

## Key decisions & reasons

- **ctest on the shared helper instead of the issue's suggested bats spawn case** — deterministic
  where a wall-clock bats spawn is flaky, and the extraction removes the divergence risk the bats
  case would have guarded (both drivers call one function). The residual gap — nothing end-to-end
  pins the call-site wiring — is deferred with a trigger ([DEF-01](../DEFERRED.md)) rather than
  dropped, and the PR body's "cannot diverge" claim was softened to match what the tests prove.
- **Bucket-E keeps `--frames=5400`** — changing the CI lane's runtime knobs is out of scope for a
  fix PR; the rewritten comment tells the next maintainer `--timeout` is now the direct dial.
- **R3 bound as Deferred over Backlog** (reviewers split) — the ask has a natural trigger (next
  bucket-E / spawn-smoke touch), which is what distinguishes a deferral from undated backlog.
- **Panel degradation accepted** — 2/2 live legs (claude-opus, claude-sonnet); codex ×3, cursor ×2,
  opencode ×5 skipped, CLIs not on PATH. Verdict aggregate 0.8518, no hard veto, ack recorded
  against the branch. Close diffed against the binding dispositions: the one `fix` (R1) is in the
  branch diff; nothing bound as `fix` is unlanded.

## Spawned work

- Deferred: [DEF-01](../DEFERRED.md) — CLI-level `--spawn --timeout` wiring assertion (review R3,
  raised by both legs).
- Bugs: issue #1983 — `SMATCHET_SPAWN_TIMEOUT_MS` documented as `--timeout`'s default but never
  consulted (review R2).
- Ideas / Backlog: none.
