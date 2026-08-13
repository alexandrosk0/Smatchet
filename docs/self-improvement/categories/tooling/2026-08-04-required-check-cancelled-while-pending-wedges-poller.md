- 2026-08-04 · claude-code · [tooling] · P2 — A required check cancelled *while pending* wedges the gate-poller for its full budget with no actionable signal

  Observed on PR #1937 (Help > About dialog). The poller ran all 90 polls (~90 min)
  and returned `GATES_TIMEOUT` having merged nothing. Every poll printed the same
  three lines:

      CI: 21/21 pass (0 fail, 0 pending, 0 warn-downgraded, 1 req-missing)
      BLOCK: required-missing: CR finding gate (... never ran; e.g. a GITHUB_TOKEN
             bot push that did not re-trigger CI).
      BLOCK: GitHub mergeStateStatus=BLOCKED

  Root cause: `.github/workflows/cr-finding-gate.yml` sets `concurrency.group` per
  PR with `cancel-in-progress: false`. That flag stops a *newer* run from killing an
  *in-progress* one — but GitHub still keeps only ONE **pending** run per group and
  cancels the rest. Two pushes landed ~2 min apart (`ae082520`, then `289bb3ff`);
  the first run was in-progress, the second went pending and was cancelled. A run
  cancelled before it starts **creates no check-run at all**, so the required
  context `CR finding gate` was not red on the head — it was *absent*. Branch
  protection blocks on absent-required forever, and nothing re-triggers the
  workflow, because its triggers are `pull_request` (already consumed) plus CR
  review/comment events (CodeRabbit was rate-limited and never reviewed the head).

  Two distinct problems, both worth fixing:

  (1) **The poller cannot distinguish "not yet" from "never".** `required-missing`
      is treated identically to `pending` — wait and re-poll — but the two have
      opposite remedies. The head was otherwise 39 SUCCESS / 5 SKIPPED / 0 fail /
      0 pending from the first poll onward; there was nothing left to arrive. The
      diagnostic string already *guesses* the cause ("e.g. a GITHUB_TOKEN bot push
      that did not re-trigger CI") without checking it. Proposed: when a required
      context is missing AND every other check has reached a terminal state, query
      `gh run list --workflow <w> --json conclusion,headSha` for that context's
      workflow; if the newest run on the head is `cancelled`/`skipped`, emit an
      actionable BLOCK naming the run id and the one-line fix
      (`gh run rerun <id>`) and return immediately rather than burning the
      remaining budget. Cheap: one extra API call, only on the missing-required
      path, only once the rest of CI is terminal.

  (2) **`cancel-in-progress: false` does not mean what the workflow comment says.**
      The header comment reasons "let them all complete rather than cancel", which
      is true only for in-progress runs. The 18:29:50 burst in the same PR shows
      five `pull_request_review_comment` runs cancelled in one second — the pending
      queue collapsing exactly as documented by GitHub, contrary to the comment's
      stated intent. For a gate whose *absence* blocks merge, dropping pending runs
      is the dangerous direction. Options: drop the `concurrency` block entirely
      (the job is a few seconds and only posts a status, last-write-wins — which
      the comment already argues), or keep it and add a scheduled/`workflow_run`
      backstop that re-posts the context if the head lacks it.

  Note the near-miss: the *status context* `CR findings (0 actionable)` WAS green on
  the head ("cr-out-of-band label set — gate overridden"), so the override worked
  end-to-end. What blocked was the *check-run* `CR finding gate` — the job name.
  Same workflow, two surfaces, only one of them in the required list. Worth a line
  in `docs/agent-rules/ci-required-check-pattern.md`: a workflow that posts a status
  context under a different name than its job is two independently-failable gates.

  Scope correction (2026-08-05): the missing check-run was *a* blocker, not the
  last one. After a later push produced a green `CR finding gate`, the PR stayed
  `BLOCKED` — the residual cause was `required_conversation_resolution` with ten
  unresolved CodeRabbit threads, which the poller does not count. That is a
  separate defect, filed as the 2026-08-05 poller-bot-thread-filter entry
  (applied — the poller now names the cause; archived in `../applied.md`).
  Everything above about the cancelled-pending run still holds; it just was not
  the whole story, which is itself the lesson — the poller reported one BLOCK
  reason at a time and cleared it into another.

  Concrete next action: implement (1) in `agents/scripts/core/merge-gates.sh` with a
  `tests/bats/merge_gates.bats` case pinning the cancelled-run→actionable-BLOCK
  path; file (2) against the workflow separately, since it needs a decision on
  which of the two remedies to take.

  Status: open
  Last-reviewed: 2026-08-05
