- 2026-08-11 · claude-code · [process] · P3 — the shared staleness helper exists but only three of the core gates call it; `issue-sweep.sh` still runs whatever logic its checkout happens to hold, with nothing saying so

  Details: carried forward from
  [`2026-08-06-gate-tooling-run-from-stale-session-branch`](../applied.md) (applied
  2026-08-11), whose thesis — make staleness self-announcing instead of silent — is
  shipped. [`agents/scripts/core/lib/script-freshness.sh`](../../../../agents/scripts/core/lib/script-freshness.sh)
  now provides `script_freshness_verdict` + `warn_if_script_stale`, and three callers
  use it: `merge-gates.sh` (off/warn/block, default warn), `pre-ship.sh` (advisory,
  printed immediately before its `Safe to push` line), and `postmortem-owed.sh`
  (qualifying its `no gate escapes owed` clean result).

  What is left is breadth, and it is deliberately P3 rather than P1 because the
  highest-stakes surfaces are already covered:
  - **`issue-sweep.sh`** — genuinely unwired. Lower stakes than the three above (it is
    triage assistance, not a merge or push gate), but it is the last core script whose
    verdict a stale checkout can silently change.
  - **The lint gates** — covered *indirectly* and probably sufficiently:
    `pre-ship.sh`'s declared set already fingerprints
    `agents/scripts/project/test-lint-rules.sh` plus `lint-rules.d/*.sh`, so a stale
    rule module is reported at the push gate, which is where it would do harm. A
    direct call inside `test-lint-rules.sh` would additionally cover invoking it
    standalone — worth it only if that turns out to be a common path.

  Concrete next action: add the two-line `warn_if_script_stale` call to
  `issue-sweep.sh` over its own declared set (itself + the helper), following the
  `postmortem-owed.sh` shape. Then decide, from actual usage, whether
  `test-lint-rules.sh` needs its own direct call or whether the `pre-ship.sh` coverage
  is enough — do not add it reflexively; every call site pays a bounded `git fetch`
  (~0.8s measured), which is immaterial for a push gate and less obviously so for
  something invoked in a loop.

  Status: open
  Last-reviewed: 2026-08-11
