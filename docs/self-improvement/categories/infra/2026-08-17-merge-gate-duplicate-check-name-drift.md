- 2026-08-17 · orchestrator · [infra] · P2 — the merge-gate poller collapses duplicate check-runs by name; GitHub does not, so a concurrency-cancelled twin reports GATES_PASSED and then 405s the merge
  Details: Hit on PR #2071 (head `90504efe0875`), a docs-only diff where every check
    was green or skipped. Two workflow runs had published a check-run under the SAME
    name `Perf PR-fast (windows-2022)`: run 31981596731 produced `skipped` at
    00:43:19 (correct — the perf lane skips on a docs-only diff), and run
    31981601014 had a `cancelled` one at 00:29:03, killed by its concurrency group
    before it could resolve.
    The poller collapses that pair. `agents/scripts/core/merge-gates.d/10-gate-filter.sh:82`
    keys the rollup contexts by `["CheckRun", name]` then
    `group_by(._k) | map(sort_by(.startedAt // "") | .[-1])`, so the 00:43:19
    `skipped` wins and the 00:29:03 `cancelled` is discarded before any conclusion
    is examined. GitHub's required-status-check evaluation applies no such collapse:
    `PUT /repos/alexandrosk0/Smatchet/pulls/2071/merge` returned
    **`405 Required status check "Perf PR-fast (windows-2022)" is cancelled`**.
    Note the intent is already aligned — `:102` lists `CANCELLED` among the
    conclusions that block. ONLY the latest-per-name dedup diverges, and it diverges
    in the dangerous direction: the poller says green, the merge is impossible, and
    there is no red check anywhere for an operator or an autonomous loop to point at.
    Recovery (verified): `rerun_workflow_run` on the run that owns the stale
    check-run — here 31981601014. No push, no force, no PR-body re-pin. The 405 text
    transitions `is cancelled` -> `is expected` (the stale check is invalidated and
    GitHub now awaits a fresh one), then the re-run's job 95255190961 reported
    `skipped` at 01:03:26 and the merge succeeded as `ae6892c0`.
    Cost this time: two rejected merge calls and ~35 min wall-clock on a docs-only
    PR. The exposure is not rare — concurrency-cancelled twins are produced by the
    repo's ordinary flow, every time a PR-body edit or a quick second push supersedes
    an in-flight run. The `Intent section` body-repin dance manufactures exactly this
    shape, so any PR that needs a verdict-line update can inherit it.
  Concrete next action: (a) **Align the collapse with GitHub** — in `10-gate-filter.sh:82`,
    do not let a newer same-named context mask an older one whose conclusion is in the
    blocking set; treat the name as blocking if ANY of its contexts is
    FAILURE/TIMED_OUT/CANCELLED/ACTION_REQUIRED/STARTUP_FAILURE. That trades a false
    "green" for a false "wait", which is the correct direction — a false wait is
    visible and self-clearing, a false green wedges the loop with nothing to point at.
    (b) Cheaper interim, and worth doing regardless: emit a WARNING naming the
    divergence when one check NAME carries >1 context with differing conclusions, so
    the reason for the coming 405 is on screen before the merge is attempted.
    (c) Document the recovery in `docs/agent-rules/merge-gates.md` — the rerun-the-owning-run
    fix is cheap but completely non-obvious from the 405 text, and nothing in the repo
    currently describes this failure shape.
    Add a `tests/bats/merge_gates.bats` case pinning it: two contexts, same name,
    elder CANCELLED + newer SKIPPED, asserting the gate does NOT report passed.
    Prefer (c)+(b) immediately (docs + one log line), (a) as the real fix.
  Status: open
  Last-reviewed: 2026-08-17
