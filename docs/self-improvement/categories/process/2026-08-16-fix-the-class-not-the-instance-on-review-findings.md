- 2026-08-16 · orchestrator · [process] · P2 — a review finding was fixed at the flagged line instead of swept as a class, so ONE wrong statement cost THREE review rounds (PR #2023 rounds 4, 5, 6) — the class-sweep rule exists for exactly this and was not applied to review findings
  Details: PR #2023 changed bootstrap's reporting contract, which made the
    long-standing claim "Bootstrap runs always PASS" false. CodeRabbit flagged it
    three times, each at a wider scope, because each fix was applied only where
    the reviewer pointed: round 4 = the driver's file header; round 5 = the inline
    `# Bootstrap mode: ... No diff, always PASS.` comment 300 lines below it (plus
    the auto-bootstrap "soft PASS" comment and the exit-code table, swept only
    once round 5 forced a file-wide look); round 6 = the SAME claim in
    `tests/bats/bucket_lane_launch_smoke.bats`'s header, because round 5's sweep
    was scoped to the driver file rather than to every file in the diff. Each
    round costs a full CodeRabbit cycle — on an OSS repo that is a rate-limited,
    ~25-55 min wait plus a re-stamp of the verdict and a PR-body edit, so this
    single stale sentence consumed roughly an hour of wall-clock and three of the
    PR's seven review rounds. The repo ALREADY has this rule for a different
    trigger: `process-rules.md` § fabricated-quote class-sweep says that on a
    fabricated/incorrect quote you grep the class across the tree rather than
    fixing the cited line. Nothing said to apply the same move to a REVIEW
    FINDING, and the finding's own framing ("Line 322 says X") invites the
    narrow fix.
  Concrete next action: add a short rule to
    [`process-rules.md`](../../../agent-rules/process-rules.md) § Cadence and
    verification — *when a review finding reports a stale/incorrect STATEMENT
    (comment, doc line, header claim), fix the class, not the instance: grep the
    offending phrase across every file in the PR diff (`git diff --name-only
    <base>...HEAD`) before replying, and state in the reply that the sweep was
    diff-wide.* Cheap and mechanical; it generalises the existing
    fabricated-quote rule from "quotes" to "any statement a reviewer proves
    wrong". Optional follow-on if it recurs: a `pre-ship.sh` helper that takes a
    phrase and greps it across the diff's files, so the sweep is one command.
  Status: open
  Last-reviewed: 2026-08-16
