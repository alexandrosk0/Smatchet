# `daemon_loop` bats tests don't stub `maybe_self_resync`, so they run real git/network and flake in the required selftests lane

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [test] · P1 — `test-merge-watcher-bats.sh` test 30 fails ~1-in-5 runs because `daemon_loop` calls `maybe_self_resync(0)` at startup and the test never stubs it, so a "unit" test exercises real `git fetch` + drift detection

## Friction

`tests/bats/merge_watcher.bats:679` ("daemon_loop per-PR backstop: a transient
exception in one PR is logged + the loop continues") drives `mw.daemon_loop(0)`
with `process_registered_pr`, `read_registry`, `write_pid_file`,
`clear_pid_file`, and `time.sleep` all monkeypatched — but **not**
`maybe_self_resync`. `daemon_loop` (verified `agents/scripts/core/merge-watcher.py:3171`)
unconditionally calls `maybe_self_resync(0)` *before* the poll loop as a startup
gate-freshness check, and that function runs a real bounded `git fetch` + drift
detection against the live checkout (and "may re-exec on POSIX"). So the test's
outcome depends on network latency and the working tree's drift state at run
time — it passed 5/6 local runs and failed the 6th on exactly this test, and it
reddened the required `Agentic self-tests (bats)` lane on an unrelated docs-only
PR (#1718). The sibling `daemon_loop` tests at :709 and :739 have the same latent
gap.

The failure surfaces as a wrong `seen:`/missing-WARN assertion, which reads like a
logic regression but is pure test-isolation leakage — a false red that costs a
diagnosis round and blocks merge on a flake.

## Proposal

Stub `mw.maybe_self_resync = lambda *_a, **_k: {}` (a no-op returning an empty
dict, matching its contract of `.get('resync_action')` / `.get('resync_needs_*')`)
in the four `daemon_loop` tests (:679, :709, :739, :771) alongside the existing
`write_pid_file`/`time.sleep` stubs, so `daemon_loop` never touches git/network in
a unit test. Optionally add a module-level guard so `daemon_loop`'s startup resync
is skippable via an env knob the tests already set. Est ~15 min. Deterministic
after — the assertions are otherwise fully specified by the faked registry.

**Update (2026-07-10): fixed in this PR (#1718).** Added the
`mw.maybe_self_resync` no-op stub to all four `daemon_loop` tests; the suite went
8/8 green locally (was ~1-in-5 red on test 30). Archive to `applied.md` on the
next self-improvement sweep.

## Format

- Details: see § Friction. Verified: `merge-watcher.py:3171` `daemon_loop` calls
  `maybe_self_resync(0)` unconditionally; the test at `merge_watcher.bats:679`
  stubs five symbols but not `maybe_self_resync`; observed 1/6 local failure on
  test 30 and the CI red on #1718 head `9101c9c7`.
- Concrete next action: see § Proposal.
- Status: open
- Last-reviewed: 2026-07-10
