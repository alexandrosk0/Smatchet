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
  Update (a) SHIPPED 2026-08-17 — but NOT as proposed above, because the proposal
    was wrong. "Treat the name as blocking if ANY of its contexts is FAILURE/…"
    would have regressed the case the dedup exists for: `merge-gates.graphql:59-62`
    records that a job rerun leaves BOTH the old FAILURE and the new SUCCESS on the
    head, so an any-blocks rule wedges every PR ever fixed by a rerun. It would also
    have over-blocked PR #2091, where two elder runs were cancelled by concurrency,
    the newest succeeded, and GitHub merged on the first attempt.
    The two cases are indistinguishable in the data the poller fetched, which is the
    real defect: `startedAt` cannot tell "same job, rerun" from "different run,
    cancelled by concurrency". Three observations pin the actual rule — GitHub reads
    the newest WORKFLOW RUN for a name, the poller read the newest `startedAt`, and
    those diverge only when a newer run is cancelled before an older run finishes.
    Fix: query `checkSuite { workflowRun { databaseId } }` and sort by
    `[run id, startedAt]`. A rerun reuses its run id, so it ties on the first key and
    still resolves by `startedAt` (rerun-to-green preserved); different runs order by
    run id (matches GitHub). Non-Actions check runs (CodeRabbit, Bugbot) have no
    `workflowRun`, tie at 0, and behave exactly as before.
    Four `tests/bats/merge_gates.bats` cases pin it: rerun-same-run, the #2071
    elder-cancelled shape, the #2091 newest-success shape, and the no-workflowRun
    fallback. All 213 merge_gates cases plus 359 across the seven sibling suites that
    source merge-gates pass unchanged — existing fixtures carry no `checkSuite`, so
    they tie at 0 and keep their old ordering.
    NOT verified: the GraphQL field path could not be validated against the live
    schema (this session serves only pinned PR-review operations; docs.github.com is
    egress-blocked). If the path is wrong the query errors, which returns GH_API_DOWN
    — a terminal notifying state that blocks rather than merges, so it fails safe and
    loudly. Watch the first real poller run.
    (b) and the `merge-gates.md` half of (c) remain open — with (a) shipped the gate
    now blocks in step with GitHub, so the confusing 405 should not reach an operator
    in the poller path, but a merge attempted outside the poller can still hit it.
  Status: partially applied (2026-08-17 — (a) shipped with tests; (b) warning and
    the (c) merge-gates.md recovery note remain)
  Last-reviewed: 2026-08-17
