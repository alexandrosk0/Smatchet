- 2026-06-20 · orchestrator · [tooling] · P2 — the `intent-out-of-band` override hatch from ADR-0022 (#1391) shipped only HALF-wired: the doc-validation gate + `poll_merge_gates` referenced the label by exact name, but (a) the GitHub label was never created in the repo, and (b) `safe-admin-merge.sh` never honored it — so a data/docs PR that tripped the `Intent section` block-allowlist check had NO working override on either merge path
  Details: Discovered while landing PR #1435 (pure-data merge-watcher ledger flush). The PR's body
    omitted `## Intent` → `Intent section` doc-validation check went RED; that check is on
    `MERGE_GATES_BLOCK_ALLOWLIST_RE`, so it gated both the poller and `safe-admin-merge`. Two gaps surfaced:
    (1) `gh pr edit --add-label intent-out-of-band` failed with "label not found" — the label documented in
    doc-validation.yml's error message ("Override with the 'intent-out-of-band' label.") and recognised by
    `merge-gates.sh` ($intent downgrade, lines ~482/540) was never `gh label create`d. Created this session
    (#BFD4F2, matching the other *-out-of-band labels).
    (2) `safe-admin-merge.sh` honored `tests-out-of-band` / `perf-out-of-band` in its `evaluate_rollup` jq
    filter but NOT `intent-out-of-band`, so it REFUSED #1435 ("RED/PENDING: Intent section") even with the
    label applied — diverging from `poll_merge_gates`, which correctly downgraded it. The two gate paths are
    supposed to mirror each other (the allow-list is single-sourced); the label-downgrade set was not.
  Concrete next action: BOTH FIXED. (a) label created in the repo (one-time). (b) SHIPPED in this PR
    (feat/safe-admin-intent-oob): added `$intentOob` binding + `Intent section` downgrade in both the
    rowBlockers and absentReq branches of `evaluate_rollup`, a header-doc line, and selftest CASE4b
    (15/15 pass). RESIDUAL: ADR-0022's rollout checklist should have included "create the label" +
    "wire every gate path that reads the allow-list" — consider a parity test asserting the *-out-of-band
    label set in `merge-gates.sh` $downgraded == the set honored in `safe-admin-merge.sh` evaluate_rollup,
    so a future allow-list addition can't half-wire again. Est ~0.5d for the parity test.
  Cross-ref: #1391 / docs/adr/0022-intent-gate-promotion.md; .github/workflows/doc-validation.yml
    (Intent section gate); agents/scripts/core/merge-gates.sh (MERGE_GATES_BLOCK_ALLOWLIST_RE + $intent
    downgrade); agents/scripts/core/safe-admin-merge.sh (evaluate_rollup $intentOob, CASE4b); PR #1435
    (the data PR that surfaced it);
    docs/self-improvement/categories/tooling/2026-06-19-merge-watcher-runs-stale-gate-logic.md (sibling
    Intent-section escape — distinct: that was stale daemon logic, this is incomplete label/tool wiring).
  Status: open
  Last-reviewed: 2026-06-20
