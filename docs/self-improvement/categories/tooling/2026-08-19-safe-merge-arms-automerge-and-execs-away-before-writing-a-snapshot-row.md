# The default merge path arms auto-merge and `exec`s away — no actor writes the ledger row

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-19
- **Observed on**: PR #2115 (permanent hole, past the janitor's 6 h repair window) and PR #2134 (hole closed by hand ~1 h later, by this entry's PR)
- **Status**: open

## What happened

`agents/scripts/core/safe-merge.sh` is the **sanctioned default non-admin merge path**
(`docs/agent-rules/ship-loops.md` § step 3, PR-1: "an armed or autonomous merge MUST go through
`safe-merge.sh` — never a bare `--auto`"). Its last statement is:

```bash
# agents/scripts/core/safe-merge.sh:555
exec gh pr merge "$pr" --squash --auto
```

`exec` replaces the shell process. There is no code path after the arm **by construction**, so the
script cannot observe its own merge and cannot append the ADR-0017 gate-verdict snapshot. Grepping
the script for `merge-snapshot-append` / `append_merge_snapshot` returns nothing; the repo-wide
referrer set is `git-janitor.sh`, `postmortem-owed.sh`, `safe-admin-merge.sh` (plus docs + bats).
The default path is absent from it.

The rule exists — it is just prose. `ship-loops.md` § step 3 names a **"Fourth writer — the session
that ARMED an auto-merge"**, which must run the helper with actor `orchestrator-automerge` and
`SNAPSHOT_MERGED_AT=<mergedAt>` on receiving the merged notification. Nothing enforces or performs
it, so coverage equals whatever the orchestrator remembers. The ledger shows both outcomes in the
same week: rows for #2114 / #2015 carry `"mergeActor":"orchestrator-automerge"` (remembered), and:

| PR | merged | ledger row | status |
|---|---|---|---|
| #2115 | 2026-08-18T14:15:44Z | **none** | permanent — past `SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS` (6 h), and the retro-compose prohibition forbids reconstructing it now |
| #2134 | 2026-08-19T12:08:35Z | appended by hand | closed ~1 h post-merge, only because the hole was noticed |

#2115's own title is `chore(ledger): merge-time gate snapshot for PR #2114` — the PR that closed one
hole opened the next one.

## Why it matters

ADR-0017 § Distributed-write contract states the mitigation as *"all three actors are named **and
wired** … behind one shared helper so they stay consistent."* For the path that carries most merges,
"wired" is not true: `safe-admin-merge.sh` (the narrow stale-BLOCKED carve-out) appends in code, and
`git-janitor.sh --post-merge` Step 5.5 backfills in code, but the **default** path does not. Ledger
coverage is therefore biased *away* from ordinary merges and *toward* the exceptional ones.

A hole costs losslessness, not blindness — `postmortem-owed.sh` falls back to the live
`statusCheckRollup`. But that fallback is exactly what ADR-0017 calls provably lossy: GitHub
overwrites rollup contexts by name on re-run and strips override labels post-merge. So the merges
whose gate truth is recoverable are the rare ones, and the merges whose truth silently degrades are
the routine ones.

There is also a **regress** the current design does not terminate: a `chore(ledger)` PR is itself a
merge that owes a row, so landing row N opens hole N+1. The only terminators are (a) the janitor's
6 h backfill actually running, or (b) batching the row into an unrelated develop-bound commit. This
entry's own PR is an instance — it lands #2134's row and will itself merge un-snapshotted.

## Concrete next action

Ranked, cheapest first.

1. **Make `safe-merge.sh` the fifth code writer.** Drop the `exec` (call `gh pr merge --squash
   --auto` normally), then poll `gh pr view "$pr" --json state,mergeCommit,mergedAt` on a short
   bounded budget and, on `MERGED`, call `append_merge_snapshot "$pr" <mergeCommit> <headSha>
   GATES_PASSED "<downgraded-csv>" "<override-csv>" orchestrator-automerge` with
   `SNAPSHOT_MERGED_AT` from the API. A short budget suffices because the script only arms **after**
   `GATES_PASSED` — every check is already terminal-green, so GitHub merges in seconds. The
   `redChecks` projection needs no new logic: `safe-merge.sh` already parses the poll's
   `GATE_SNAPSHOT cr_override=… downgraded=…` line at line 156 (`loadbearing_oob_labels`) for its
   obligation-stub path, and holds the label list in the same scope.
2. **Make the timeout branch mechanical, not remembered.** If the bounded wait expires (auto-merge
   still queued), print the ready-to-paste `merge-snapshot-append.sh` invocation with all 7 args
   pre-filled and `SNAPSHOT_MERGED_AT=` stubbed. Precedent in the same script: `file_obligation_stub`
   already converts a would-be-remembered obligation into a written artefact.
3. **Detect the residual hole inside the repair window.** Have the SessionStart nudge (or
   `postmortem-owed.sh`) flag any PR merged in the last 6 h with no ledger row, so a miss surfaces
   while `git-janitor --post-merge` can still legitimately backfill it — instead of hardening into a
   permanent hole like #2115.

**Enumerator + replay** (per AGENT_SELF_IMPROVEMENT.md): the gate is a new case in the existing
`tests/bats/safe_merge.bats` (205 lines, 14 `@test`s), asserting that after a PASS-gated arm exactly
one row lands in `MERGE_SNAPSHOT_LEDGER` (an existing env seam in `merge-snapshot-append.sh`) with
`.pr` = the PR, `.mergeActor` = `orchestrator-automerge`, `.gates` = `GATES_PASSED`.

Replayed against the script as it stands, that case **cannot pass**: `exec` at line 555 replaces the
process, so no append can run and the temp ledger stays empty — the #2115 / #2134 shape exactly.

Writing the case also needs one seam the fix must add. Today the arm block short-circuits on
`[ "${SAFE_MERGE_DRY_RUN:-}" = "true" ] || [ -n "${SAFE_MERGE_STUB_GATE:-}" ]`, so stubbing the gate
*forces* the `DRY-RUN: would run: gh pr merge …` exit — which is precisely what the existing
`arms auto-merge when the gate PASSES (exit 0)` bats case asserts on, and why it stops one line
short of the behaviour at issue. Separate the two conditions (keep the DRY-RUN exit; let a stubbed
gate proceed against a stub `gh` on `PATH` reporting `MERGED` + a merge oid) and the post-arm write
becomes testable. The 13 `--selftest` cases are unaffected — they exercise helpers
(`loadbearing_oob_labels`, `maybe_file_obligations`, `run_gate`, `default_flip_ready`) directly and
never enter the arm block at all.

Related: [`docs/adr/0017-merge-time-snapshot-ledger.md`](../../../adr/0017-merge-time-snapshot-ledger.md)
(the losslessness argument + the writer set), [`docs/agent-rules/ship-loops.md`](../../../agent-rules/ship-loops.md)
§ step 3 (the prose fourth-writer rule this entry proposes to turn into code).

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-19;n=15; action=check whether safe-merge.sh appends its own snapshot row, and re-measure the share of merged PRs with a ledger row; baseline=1 permanent hole (#2115) and 1 hand-closed hole (#2134) across 5 session merges on 2026-08-18/19; fired=never
