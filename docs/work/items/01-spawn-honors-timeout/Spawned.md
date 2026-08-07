# Spawned — 01-spawn-honors-timeout

Ungated capture sheet for work this item spawns (deferrals, ideas, backlog entries, bugs) —
[work-items.md → Tracking](../../../agent-rules/work-items.md#tracking). Entries carry no canonical
ID; IDs are assigned when this sheet drains into the ledgers at close.

## Deferred

- **CLI-level `--spawn --timeout` wiring assertion** — the new doctests pin `ScenarioWaitMs` as a
  pure function only; nothing end-to-end asserts the `--spawn` driver actually passes `pa.timeoutMs`
  into the wait loop (re-inlining the expression or regressing to `ScenarioWaitMs(0, frames)` leaves
  them green). Add a launch-smoke assertion that `--spawn --timeout=<small>` fails fast, per the
  in-repo pattern (`scripts/dev/test-spawn-mcp-default-auth.sh`). Trigger: next time bucket-E or the
  spawn smoke lane is touched. Source: 5-review-1-claude-opus §3, 5-review-1-claude-sonnet §1.

## Ideas

*(none)*

## Backlog

*(none)*

## Bugs

- **`SMATCHET_SPAWN_TIMEOUT_MS` advertised as `--timeout`'s default but never consulted for the wait
  budget** — `cmd --help` (CliHelpAndAttach.cpp:154) and `docs/guides/cli.md:87,460` document the env
  var as the `--timeout` default, but `ParseArgs` (CliArgs.cpp) never reads it; the only reader uses
  it for the attach path's HTTP read timeout. Either default `out.timeoutMs` from
  `EnvIntOr("SMATCHET_SPAWN_TIMEOUT_MS", 0)` in `ParseArgs`, or delete the claim from `--help` and
  both `cli.md` rows. Source: 5-review-1-claude-opus §2.
