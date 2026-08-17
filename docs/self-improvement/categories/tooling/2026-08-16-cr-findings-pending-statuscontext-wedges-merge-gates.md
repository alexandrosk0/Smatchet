- 2026-08-16 · orchestrator · [tooling] · P2 — a PENDING `CR findings (0 actionable)` StatusContext blocks `merge-gates.sh` forever through the **CI** bucket, and no label reaches it: `cr-out-of-band` / `cr-disposition:*` downgrade the CodeRabbit *review* gate, while the pending count is taken over `$blocking` with no `$downgraded` filter — so a PR whose CR gate has already self-downgraded to WARN still reports `1 pending` on every poll until a human merges it out from under the poller
  Details: **(a) The mechanism.** Under block-on-any-red
    (`MERGE_GATES_BLOCK_ALLOWLIST_RE="."`) `$blocking` is *required contexts PLUS
    every non-required non-`advisory` check*
    ([`10-gate-filter.sh:93-97`](../../../../agents/scripts/core/merge-gates.d/10-gate-filter.sh)).
    The pending count at `:280-282` then counts any `$blocking` StatusContext in
    `PENDING`/`EXPECTED`. `CR findings (0 actionable)` is **not** one of the 22
    required contexts on `develop`, but it is a non-required non-advisory
    StatusContext, so block-on-any-red pulls it into `$blocking` anyway.
    Critically, the failing filter at `:279` subtracts `$downgraded`; the pending
    filter at `:280` does **not**. `$downgraded` (`:137-141`) only ever holds
    *failing* CheckRuns for `Test-delta gate` / `Perf PR-fast` / `Intent section`
    / `Plan-lock gate`. There is no path from any label to a pending
    StatusContext. The CR bucket's own rate-limit and pure-docs auto-downgrades
    are real and did fire — they just downgrade gate **#2**, and this block lives
    in gate **#1**.
    **(b) Why it never resolves.** Per
    [`2026-07-13-cr-merge-gate-stuck-blockers.md`](2026-07-13-cr-merge-gate-stuck-blockers.md)
    § Friction A the aggregator posts `pending` on `pull_request` and flips to
    `success` only on a CR **review node**. When CR cannot produce one — an
    exhausted rate limit on an already-seen head
    ([`2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop.md`](2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop.md)
    § (b)), or the OSS `Review skipped: manual review required for this OSS
    repository` threshold — the pending is permanent by construction. Those
    entries call it "correctly pending forever" from CR's side; what is new here
    is that `merge-gates.sh` converts it into an **unescapable CI block**, so an
    operator who has already dispositioned CR by hand (adversarial review,
    `cr-disposition:` line, label applied) has no remaining move except to merge
    outside the gate.
    **(c) Observed, 2026-08-16, PRs #2071 / #2077 / #2081.** Every poll on all
    three read `CI: 21/22 pass (0 fail, 1 pending, …)` with the CR bucket
    already at WARN (`rate-limit pure-docs-auto-downgrade`, Bugbot
    `no-wedge pass`, `0 open`, `User: 0`). Two independent pollers held that
    exact line to their last cycle and terminated on `PR_MERGED`, not on
    `GATES_PASSED`. A separate 30-cycle probe showed `CRfindings=pending` with
    `CodeRabbit="Review rate limited"` on #2071 and #2077 throughout. The
    watcher's cycle counts on these PRs (94 and 21) are the same block.
    **(d) A push is what unblocks it.** The rollup is head-bound, so on a fresh
    commit the context is simply *absent* — and an absent non-required context is
    counted by nothing (only `$reqAbsent` tracks absence, and only for the 22
    required names), so the gate passes. It flips to `1 pending` once
    `cr-finding-gate` posts on the new head. That transition was visible on
    #2081 across the `cb890979a541` → `ece22ffefc62` push. Net effect: the gate
    is *more* permissive on an unreviewed head than on one where the aggregator
    has spoken. Inferred from the observed absent→pending flip, not read out of
    the workflow — confirm directly before acting on it.
  Concrete next action: (1) **The narrow fix** — exclude the CR aggregator from
    the CI pending count when the CR bucket has already been downgraded to WARN:
    apply at `:280` the same `$downgraded`-style subtraction `:279` already
    applies to failures, keyed on `CR findings (0 actionable)` plus an active CR
    downgrade (`cr-out-of-band`, the pure-docs classifier, or the rate-limit
    auto-downgrade). Double-counting CR across both buckets is the actual defect:
    gate #2 exists to adjudicate CodeRabbit, and gate #1 should not re-litigate
    it under a different name. Bats case: a PENDING `CR findings (0 actionable)`
    plus an active CR downgrade must yield `GATES_PASSED`, and must still block
    when no CR downgrade is in force. (2) **Decide the general question** —
    should *any* non-required PENDING StatusContext block indefinitely with no
    label escape? The block-on-any-red flip targeted non-required **reds**
    (#923-class escapes); sweeping non-required pendings in was a side effect of
    reusing `$blocking` for both counts. Either add a generic per-context waiver
    (`gate-pending-out-of-band:<context>`) or bound the wait, so the class cannot
    recur under a different context name. (3) Record the operator workaround in
    [`merge-gates.md`](../../../agent-rules/merge-gates.md) § CodeRabbit
    rate-limit playbook until (1) lands: when the only non-green is a
    permanently-pending CR aggregator and CR has been dispositioned by hand,
    that is a *gate defect*, not a real block — record the disposition and merge
    rather than polling to the cap. Est ~0.5d for (1)+(3); (2) is a design call
    that wants a human. (1) is independently shippable.
  Status: open
  Last-reviewed: 2026-08-16
