- 2026-08-13 · claude-code · [process] · P1 — the recorded review verdict named no commit, so one verdict outlived every push it never covered: six review-fix pushes on PR #2002 shipped with a stale "reviewed" claim standing

  Observed on the #2002 merge drive. The verdict line the `Intent section`
  check requires (`adversarial-code-review: N findings, <disposition>`) was
  recorded once, before the first push — correctly, for that diff. Then twelve
  CodeRabbit review rounds produced eleven fix commits across six pushes
  (`2fbcd345`..`a960dab1`), and the verdict line sat unchanged through all of
  them. Each of those pushes was exactly the thing the gate exists to make
  visible — a diff no recorded self-review covers — and the gate stayed green
  the whole time, because a verdict with no commit identity is a claim about
  "the branch, at some point", satisfied forever.

  Same failure class one layer down: the first batch of this session recorded
  its verdict AFTER the first push (post-push, pre-PR), and nothing could tell,
  because the claim carried no ordering evidence relative to any commit.

  Mechanism: the check verified the *presence* of a claim; staleness was not
  representable. Any assertion whose truth is per-commit but whose record is
  per-branch degrades to "was ever true once" — the same rot shape as the
  bucket-C golden mask (reported-once signals with no expiry) and the unearned
  `review-ack`.

  Shipped gate (this entry's PR):

  1. The verdict line carries `(head=<sha>)`, stamped by
     `agents/scripts/core/record-review-verdict.sh` (which validates the tail
     through the real checker, so placeholders are rejected at recording time).
  2. `check-pr-intent.sh` and the `Intent section` CI job reject a verdict with
     no `head=` or with a `head=` that does not prefix-match the PR head
     (`PR_HEAD_SHA` — CI re-runs on every synchronize with the fresh sha, which
     is what makes every push invalidate the prior verdict automatically). The
     regex pair stays byte-identical via `--check-workflow-sync`, which now
     also compares the `head_re` line.
  3. Pre-push hook stage (E) refuses a push whose non-protected `refs/heads/*`
     update records carry a tip with no `$GIT_DIR/review-verdict-<sha>` marker
     — the recorder stamps it, a commit made after the review lacks it. Judged
     per update record (review round 1 on the gate's own PR caught the
     HEAD-keyed first cut: a marked checkout could push an unmarked sibling
     ref, and a delete from an unmarked checkout was spuriously blocked).
     Deletes are exempt. Override `SMATCHET_SKIP_REVIEW_MARKER=1`; fail-open
     on infra.

  Residual limit, unchanged from the parent entry
  (pre-first-push-review-step-is-unenforced-and-was-skipped): all three layers
  verify a claim was recorded for the exact commit, never that the review ran.
  What the binding adds is that the claim can no longer be *accidentally*
  stale — going stale now requires re-stamping, an act, not an omission.

  Status: open — gate shipped with this entry's PR; leave open until it has
  survived one real multi-round review drive (the next PR with CR fix pushes),
  then archive.
  Last-reviewed: 2026-08-13
