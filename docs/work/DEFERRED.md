# Deferred

`DEF-NN` — scope cuts and review leftovers that fall out **during work items**. Already approved
(the item's process vetted the cut); **trigger-gated** — each entry says *when* to take it on.
Lifecycle and citation rules: [work-items.md → Tracking](../agent-rules/work-items.md#tracking).

Entry shape: `- **DEF-NN — <title>** — <one-liner>; deferred from NN-slug; **trigger:** <when to
take this on>`. This ledger holds only **open** and **`Promoted → NN-slug`** entries — resolved ones
are deleted.

- **DEF-01 — CLI-level `--spawn --timeout` wiring assertion** — the `ScenarioWaitMs` doctests pin
  the budget arithmetic only; nothing end-to-end asserts the `--spawn` driver passes `pa.timeoutMs`
  into the wait loop (regressing a call site to `ScenarioWaitMs(0, frames)` stays green). Add a
  launch-smoke assertion that `--spawn --timeout=<small>` fails fast, per the in-repo pattern
  (`scripts/dev/test-spawn-mcp-default-auth.sh`); deferred from
  [01-spawn-honors-timeout](closed/01-spawn-honors-timeout.md); **trigger:** next time bucket-E or
  the spawn smoke lane is touched.
