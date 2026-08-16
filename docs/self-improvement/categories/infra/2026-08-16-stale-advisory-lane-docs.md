- 2026-08-16 · orchestrator · [infra] · P2 — four docs still describe the pre-all-gates-blocking world, so a reader planning CI-gating work inherits a blocker that no longer exists
  Details: Surfaced while grounding `docs/plans/active/autonomous-debug-live-evidence.md`
    against the tree; unrelated to that plan's subject, so filed here rather than
    bundled into it (`docs/agent-rules/ship-loops.md` § "Unrelated work never
    shares a PR").
    The all-gates-blocking flip set `MERGE_GATES_BLOCK_ALLOWLIST_RE="."`
    (`agents/scripts/core/merge-gates.d/00-common.sh:42`), so every check-run
    blocks unless its NAME contains `advisory`; `tests/bats/merge_gates.bats:469`
    now asserts "non-required Bucket-E FAILURE blocks". Neither bucket-C nor
    bucket-E carries a job-level `continue-on-error` any more — only the two
    documented step-level masks (the per-scenario golden diff at
    `.github/workflows/build-and-test.yml:1041`, the bucket-E per-test run at
    `:1279`). Four places still say otherwise:
    (1) `docs/plans/active/testing-surface-roadmap.md:275-282` — Slice B's stated
    blocker is false in **all three** clauses: the Mesa exe boots, the "blanket
    `continue-on-error`" is gone, and `Bucket-` is no longer dropped from the
    allow-list. The real residual work is much narrower — fix or skip the ~3/74
    render-dependent bucket-E tests, and establish CI-native goldens so the
    bucket-C mask can go.
    (2) `docs/guides/testing-surface.md` and (3)
    `docs/self-improvement/categories/infra.md:14` — both still describe the two
    Mesa lanes as "fully advisory to the merge-gate poller".
    (4) Docs citing `infra.md` `bucket-mesa-exe-boot` point at the wrong file: the
    entry moved to `docs/self-improvement/categories/applied.md:1565`, and its
    premise was **falsified** on 2026-06-18 (the exe boots in ~2 s under llvmpipe;
    the ~26 s exit was the since-fixed `--spawn` exit-code bug).
    Compounding: (1) is the one a future agent is most likely to act on, because
    it reads as a live blocker on a plan slice that is actually unblocked.
  Concrete next action: rewrite `testing-surface-roadmap.md:275-282` to the two
    real residuals above; correct the "fully advisory" sentences in
    `docs/guides/testing-surface.md` and `infra.md:14`; re-point
    `bucket-mesa-exe-boot` references at `applied.md`. Cheap, docs-only, one PR.
    Consider whether a gate can catch this class at all — the drift is between
    prose and a shell constant, which nothing currently compares; a grep-based
    check that any doc asserting "advisory" about a named lane matches the lane's
    actual check name would have caught all four.
    Related: infra/2026-08-16-tsan-lane-advisory-label-drift covers the inverse
    shape (a lane the docs call advisory that the poller actually blocks on).
  Status: open
  Last-reviewed: 2026-08-16
