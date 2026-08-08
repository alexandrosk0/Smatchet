- 2026-08-06 · orchestrator · [process] · P1 — gate tooling invoked from a long-lived session branch runs a **months-old** copy of the gate logic and manufactures phantom blocks: `merge-gates.sh` run out of the integration tree (branch `claude/peaceful-faraday-6jm1w5` @ `ff0ee7a6`) predated the CR auto-exemption on `develop`, so it hard-blocked a PR that current `develop` passes — and the block was misdiagnosed as a product-gate defect, nearly costing a spurious ledger entry
  Details: While auditing PR #1953 I ran
    `bash agents/scripts/core/merge-gates.sh 1953` from `C:/Dev/Smatchet` — the shared integration
    tree, sitting on session branch `claude/peaceful-faraday-6jm1w5` at `ff0ee7a6` (2026-08-05). That
    copy hard-blocked on `CodeRabbit: NONE+size-skip`. The same PR class run from a fresh
    `develop`-based worktree prints
    `WARN: self-improvement doc PR — CR gate auto-skipped` and reaches `GATES_PASSED` (verified on
    PR #1961, poll 12/90). The difference is commit `4685997d` — "feat(merge-gates): auto-exempt pure
    self-improvement doc PRs from CR + Bugbot review" (#1468), merged **2026-06-20**, adding the
    downgrade at :1201-1202. `grep -c "self-improvement doc PR — CR gate auto-skipped"` in the
    integration tree returns **0**. The session branch was ~7 weeks behind on this file.
    The failure mode is not "the script was wrong" — it is that **nothing in the output distinguishes
    a stale-script block from a real one**. Every line the stale run emitted was a plausible, correctly
    formatted BLOCK. I took it as evidence about `develop`'s gate behaviour, wrote a postmortem
    asserting the sanctioned merge path was structurally unusable for self-improvement PRs, and only
    caught it because a later run from a fresh worktree printed a WARN line the first run never had.
    The withdrawn entry and the corrected finding are in
    [`tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](../tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md).
    Two properties make this recur rather than be a one-off:
      1. **Session branches are long-lived by design.** `claude/<id>/*` branches persist across many
         days; nothing pulls `agents/scripts/**` forward on them. The longer a session lives, the more
         the gate logic it runs diverges from the logic that actually guards `develop`.
      2. **The tree that tempts this is the shared one.** The SessionStart banner already warns that
         `C:/Dev/Smatchet` is shared and that HEAD changes collide — so a session is *discouraged from
         updating it*, which is exactly what keeps the scripts stale. The safe-for-siblings move and
         the fresh-tooling move point in opposite directions.
    Blast radius beyond this incident: every script under `agents/scripts/core/` has the same exposure
    — `postmortem-owed.sh`, `issue-sweep.sh`, `pre-ship.sh` and the lint gates all encode rules that
    change on `develop`. A stale `pre-ship.sh` is the worse direction: it can pass a diff that current
    `develop` gates would fail, i.e. it produces false **greens**, not just false reds.
  Concrete next action — make staleness self-announcing rather than silent:
    (1) **Version self-check in the poller.** At startup `merge-gates.sh` compares the merge-base of
    `HEAD` against `origin/develop` for its own path: if `git log --oneline HEAD..origin/develop --
    agents/scripts/core/merge-gates.sh` is non-empty, print
    `WARN: merge-gates.sh is N commit(s) behind origin/develop — re-run from a fresh worktree before
    trusting a BLOCK` and echo the newest such commit's subject. Never fail on it (offline / detached
    / no-remote must stay usable) — the point is that the operator can no longer read a BLOCK without
    seeing the caveat. Cheap: one `git log` against an already-fetched ref, no network if
    `origin/develop` is current, silently skipped when it is not.
    (2) **Same check in the shared helper, not per-script.** Put it in a `warn_if_script_stale
    <path>` helper (sourced by `merge-gates.sh`, `postmortem-owed.sh`, `pre-ship.sh`) so the other
    gate scripts inherit it — `pre-ship.sh` especially, where staleness yields false greens.
    (3) **Rule text.** Add to [`process-rules.md`](../../../agent-rules/process-rules.md)
    § Concurrent interactive sessions: *"Run gate tooling from a worktree freshly based on
    `origin/develop`, never from a long-lived session branch. A BLOCK observed from a stale checkout
    is not evidence about `develop`; reproduce from a fresh worktree before filing anything against a
    gate."* This is the rule that would have stopped the bad entry with zero code.
    (4) **Evidence rule for the ledger.** A postmortem or backlog entry whose central evidence is
    gate-tool output must record the tree + commit the tool ran from. Fold into the
    [`gate-escape-postmortem`](../../../../agents/_shared/skills/gate-escape-postmortem/SKILL.md)
    skill's evidence checklist — the discipline generalises past this bug.
    Est ~0.5d ((1)+(2) helper + 2 bats cases; (3)+(4) doc edits).
  Cross-ref: `agents/scripts/core/merge-gates.sh` (:1201-1202 the downgrade absent from the stale
    copy); `4685997d` / PR #1468 (2026-06-20, the commit the session branch predates); PR #1953
    (phantom block) vs PR #1961 (same class, passes from a fresh worktree);
    [`process-rules.md`](../../../agent-rules/process-rules.md) § Concurrent interactive sessions
    (`nsc <slug>` one-worktree-per-session rule this extends from correctness to *freshness*).
