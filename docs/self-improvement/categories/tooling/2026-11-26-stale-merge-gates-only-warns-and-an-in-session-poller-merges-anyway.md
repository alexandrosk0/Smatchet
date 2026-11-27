# A stale `merge-gates.sh` only warns, so an in-session poller merges on out-of-date gate logic

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-11-26
- **Observed on**: PR #2490, #2494, #2496, #2515 (merged 2026-11-25 and 2026-11-26), in the 2026-11-26 entry in [`postmortems.md`](../../postmortems.md). Related, archived as applied: `2026-08-06-gate-tooling-run-from-stale-session-branch`.
- **Status**: open

## What happened

A coordinator session polled and merged from its own long-lived worktree, whose `agent-layer` checkout was still at `75e1728`. `develop` moved the pin to `4334c2b` in #2492, adding the plan CI-proof gate to `merge-gates.sh`. Four later merges were judged without that gate and returned `GATES_PASSED`.

## Why

The freshness check exists and reports `stale` for that checkout, but `MERGE_GATES_FRESHNESS` defaults to `warn`, so the poll still passes. The session's poller script printed only the `Poll` summary line when the gates passed, which dropped the warning. Nothing makes a caller that is about to merge ask for `block`.

## Proposed fix

Refuse `GATES_PASSED` on a stale checkout by default (`block`), keeping `warn` as the explicit opt-out for read-only status polls. If a global default flip is too broad, have `merge-gates.sh` emit the staleness verdict on the `GATE_SNAPSHOT` / `GATE_HEAD` output and have every merge actor, including a hand-written poller, require it to be `fresh`. Pin it with a bats case that sets the fingerprint test seam to differing blobs and no `MERGE_GATES_FRESHNESS`, and asserts no `GATES_PASSED`.

**Enumerator + replay**: `script_freshness_verdict` over the gate file set. On the coordinator's checkout it yields `stale`; on a worktree at `origin/develop` `97dd196cd` it yields `fresh`.
