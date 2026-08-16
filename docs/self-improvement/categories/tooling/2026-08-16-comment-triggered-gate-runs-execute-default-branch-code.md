- 2026-08-16 · orchestrator · [tooling] · P2 — the CR finding gate re-triggers on `issue_comment` / `pull_request_review*`, and those runs execute the **default branch's** action code, not the PR's; so a fix to the gate cannot be trusted on its own PR — develop's old logic keeps overwriting the head's status and the last writer wins
  Details: Found while merging #2036, which closes a fail-open in
    [`cr-finding-gate/action.yml`](../../../../.github/actions/cr-finding-gate/action.yml)
    (a `Review skipped: manual review required` status was being read as a
    clean pass). On head `702b5b57` the gate posted three statuses within 70
    seconds and they disagreed:
    - `19:11:14Z` run, event `pull_request`, `head_branch` = the PR branch →
      PENDING, "awaiting CodeRabbit review on current head". Correct: this ran
      the fixed action from the PR.
    - `19:11:19Z` and `19:11:45Z` runs, event `issue_comment`, `head_branch`
      **develop**, `head_sha` **dcf4cd3f** → SUCCESS, "CodeRabbit completed
      with no review on head (skipped/clean)" at `19:11:35Z` and `19:12:23Z`.
      Those ran develop's action — the code the PR exists to replace.
    GitHub resolves a workflow triggered by a non-PR event against the default
    branch, so `issue_comment` / `pull_request_review*` runs check out and
    execute develop, whatever the PR changed. The gate depends on exactly those
    events to un-stick itself when CR finishes (documented at the top of
    [`cr-finding-gate.yml`](../../../../.github/workflows/cr-finding-gate.yml)),
    so the mixed-code path is not incidental — it is the common path. Statuses
    have no precedence, only recency, so the newest writer wins and the stale
    logic decides the visible state.
    Why this is worth a rule and not just a note: the failure is
    self-concealing in the one place it matters most. Dogfooding a gate fix on
    its own PR is the repo's normal proof, and it silently proves the wrong
    thing here — the first `pull_request` run shows the new behaviour, then a
    comment lands and develop's code overwrites it with the old behaviour. A
    reader checking the cell after the fact sees green and concludes the fix
    did not work, or worse, that the old behaviour was correct. It also means
    the gate's protection against the class it was hardened for is only as new
    as develop for as long as the fix is in flight.
    Not a merge blocker for #2036: merging IS the fix, since after the squash
    every trigger path runs the new code. What the merge cannot fix is the
    reading — the green cell on that PR must not be cited as review evidence,
    and CR's own posted verdict has to be read instead.
  Concrete next action: two cheap, independent pieces.
    (1) Note it where it is read: a line in
    [`merge-gates.md`](../../../agent-rules/merge-gates.md) § CodeRabbit —
    *a change to a gate's own action/workflow is NOT proven by its check on
    its own PR; comment- and review-triggered runs execute the default
    branch's code, so verify against the `pull_request` run's log (or a
    scratch PR opened after the merge), never the final status cell.*
    (2) Make it visible instead of inferred: have the action print which ref
    it is running from (`GITHUB_EVENT_NAME` + the workflow ref) into the job
    summary and into the status description on a disagreement, so a mixed-code
    sequence is legible from the PR page rather than from three API calls.
    Neither needs new infrastructure; (2) is the one with teeth.
  Status: open
  Last-reviewed: 2026-08-16
