- 2026-06-19 · orchestrator · [process] · P2 — block-allowlisted `Intent section` gate is bypassed by every non-poller merge path (bare `gh pr merge --auto`, direct REST); promote the 2026-06-11 "use the poller" advice into an enforced non-admin merge wrapper
  Details: Three PRs (#1414, #1415, #1406) merged to develop in one 07:55–07:58Z batch with the
    `Intent section` doc-validation check non-green and no `intent-out-of-band` override label —
    see postmortems.md 2026-06-19 PR #1406, #1414, #1415. `Intent section` is on the merge-gates.sh
    `MERGE_GATES_BLOCK_ALLOWLIST_RE` *meant-to-block* allow-list (added 2026-06-18, ADR-0022) but is
    deliberately NOT a develop branch-protection required context, so it is enforced ONLY by the
    merge-gates poller / watcher. Bare native `gh pr merge --auto` (#1414, #1406) waits only on the
    branch-protection required contexts (Test-delta, Windows+MSVC ×2, Shell lint, Doc anchors,
    Perf PR-fast, Coverage, Sanitizer ×2 — Intent is not among them) and merges past a red Intent the
    instant those green; a direct REST `PUT …/merge` (#1415) polls nothing at all. This is the exact
    sharp edge already documented for CodeRabbit in the 2026-06-11 process.md entry ("raw `--auto` only
    waits on the required status checks") — its remedy was advisory ("DEFAULT to the merge-gates poller
    path"), and the 3× recurrence in one batch shows advisory guidance does not hold under autonomous
    ship-loops. (#1406 is a stale-red member — its `## Intent` is filled now; the failed run predated the
    fill — but it merged via the same `--auto` bypass and the detector nags it under the same trigger.)
  Concrete next action: add `agents/scripts/core/safe-merge.sh` — a non-admin sibling of
    `safe-admin-merge.sh` that (1) runs `merge-gates.sh` for the PR (which already blocks on the full
    block-allowlist incl. `Intent section` / CodeRabbit), (2) arms `gh pr merge --squash --auto` ONLY on a
    PASS, and (3) refuses on any red block-allowlist gate lacking its `*-out-of-band` override label. Make
    it the single sanctioned agent merge entry-point; update `docs/agent-rules/ship-loops.md` +
    `docs/agent-rules/merge-gates.md` to forbid bare `gh pr merge --auto` and direct REST `PUT …/merge` in
    the ship-loop. Back it with a bats test (sibling of `test-safe-admin-merge-bats.sh`) asserting refusal
    when a block-allowlist gate is red without its override label, and PASS-then-arm when green/overridden.
    Est ~0.5–1d. ADR-0022 keeps Intent off branch-protection required-contexts on purpose (merge-queue
    deadlock reversibility), so enforcement must stay on the merge-actor side — do NOT "fix" this by
    promoting Intent to a required context.
  Triggered-follow-up: supersedes the advisory remedy in the 2026-06-11 process.md entry "authorized
    auto-merge armed via raw `gh pr merge --auto`" — when this wrapper lands, update that entry's status to
    note the advisory was promoted to an enforced gate.
  Cross-ref: postmortems.md 2026-06-19 PR #1406, #1414, #1415; merge commits 96e79412 (#1414),
    7bb77daa (#1406), f6bb3972 (#1415); merge-gates.sh `MERGE_GATES_BLOCK_ALLOWLIST_RE`;
    docs/agent-rules/merge-gates.md:84; docs/adr/0022-intent-gate-promotion.md; safe-admin-merge.sh.
  Status: open
  Last-reviewed: 2026-06-19
