# CR merge-gate: two silent BLOCKED-with-everything-green states + their auto-remedies

- **Date**: 2026-07-13
- **Author**: orchestrator
- **Category**: tooling
- **Priority**: P2

## What

During a large merge push (~15 PRs), multiple PRs sat `BLOCKED` / `UNSTABLE` for 90+ minutes
with **every CI check green and CodeRabbit itself reporting clean** — no red, no findings. The
merge-watcher / babysit loop waited indefinitely because it only merges on `CLEAN`. Two distinct
CR-gate mechanics were the cause; both are non-obvious and cost a live diagnosis to find. Both have
a cheap, deterministic remedy the merge-watcher (and the orchestrator's merge step) should apply
automatically instead of stalling.

## Friction A — the `CR findings (0 actionable)` aggregator gate gets stuck `pending`

`.github/workflows/cr-finding-gate.yml` posts the `CR findings (0 actionable)` StatusContext. It
runs on `pull_request` (fires BEFORE CodeRabbit reviews → posts `pending`) and re-runs on
`pull_request_review` / `pull_request_review_comment` / CR-bot `issue_comment` to post `success`.
**But when CodeRabbit finds 0 actionable findings it flips its own `CodeRabbit` status to green with
NO review node** — so none of the re-trigger events fire, and the aggregator stays `pending`
forever. GitHub then shows the PR `BLOCKED` (a required context never reached success) even though
CodeRabbit is done and clean. Re-running the `pull_request` workflow run just re-posts `pending`
(same stuck condition). A human/agent comment does NOT help — the workflow's `issue_comment` handler
only acts on **CodeRabbit-bot-authored** comments (a non-bot comment run "skips").

- **Remedy that works:** post `@coderabbitai review` on the PR. CodeRabbit re-reviews and this time
  posts a **review node**, which re-fires `cr-finding-gate` → it posts `success` → PR goes `CLEAN`.
  Verified on 4 PRs (#1799/#1801/#1803/#1804) — each cleared within ~90 s of the nudge. **No
  `cr-out-of-band` override or admin-merge was needed** (and must not be used here — CR genuinely
  reviewed clean; only the aggregator was mechanically stuck).

## Friction B — an ADDRESSED CodeRabbit thread still blocks via "require conversation resolution"

After fixing a CodeRabbit inline finding by **pushing a code change**, the review **thread stays
`isResolved:false`** (CodeRabbit does not auto-resolve it, and its re-review may report "0 findings"
without touching the old thread). Branch protection's *require-conversation-resolution* then holds
the PR `BLOCKED` with all checks green, `mergeable:true`, `mergeable_state:blocked`, `strict:false`
— an easy misdiagnosis as "out of date" or "needs review." Seen on #1810 (dangling-pointer fix) and
#1821 (state-leak fix).

- **Remedy that works:** after the fix lands, resolve the (now-addressed/outdated) thread via the
  GraphQL `resolveReviewThread` mutation. PR flips to `CLEAN` immediately.

## Concrete next action (implement)

Teach the **merge-watcher** (`agents/scripts/core/merge-watcher.py` / `merge-gates.sh`) — and the
orchestrator's inline merge step — to auto-remedy both before concluding a PR is un-mergeable:

1. If `mergeStateStatus != CLEAN` but **all CI is green and `CodeRabbit` status is success**:
   - resolve any review thread that is `isResolved:false` AND (`isOutdated:true` OR its finding's
     file/line no longer exists in the head diff) — the addressed-thread case (Friction B);
   - if the `CR findings (0 actionable)` context is still `pending` after that (Friction A), post
     `@coderabbitai review` **once** (idempotency-guarded) and re-poll.
   Only then, if it is still not green, fall through to the existing halt/label logic.
2. **Root-cause fix for Friction A (preferred, removes the nudge dance):** make `cr-finding-gate.yml`
   also trigger on the `CodeRabbit` StatusContext reaching `success` — e.g. a `status:` /
   `check_run: [completed]` event handler that re-evaluates + posts `CR findings (0 actionable)`
   `success` when CodeRabbit is done with no review node. That closes the "0-findings-via-status,
   no-review-node" hole so the gate self-resolves.
3. Add a bats guard for the merge-watcher remedy path (mock a stuck-pending aggregator + an
   outdated-unresolved thread; assert the watcher resolves + nudges rather than stalling).

Until (2) ships, the manual sequence is: **resolve the addressed thread → `@coderabbitai review`
→ merge when CLEAN** (never `cr-out-of-band`/admin-merge while CR is genuinely reviewing clean).

## Status

open (documented; the merge-watcher auto-remedy + the cr-finding-gate status-event trigger are the
implementable fixes)
