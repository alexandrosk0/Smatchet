# Re-running a body-reading gate replays a frozen event payload — and the rollup dedup turns that into a fresh red

- **Category**: process
- **Priority**: P2
- **Date**: 2026-09-07
- **Observed on**: PR #2180 (`claude/agent-layer-a1-seam`) — two `gh run rerun` invocations against run `34139417600`, the second landing a terminal `FAILURE` at `17:03:05Z` where the head had previously carried only a stale `CANCELLED`
- **Status**: open

## What happened

`Intent section` was red on #2180's head. The reflex remedy — the one `merge-gates.md` documents —
is `gh run rerun <id>`. It was issued twice. Neither run could ever have passed, and the second one
made the gate strictly worse than before it ran.

The re-trigger that actually worked was a **PR body edit**, which produced run `34147012343`,
`SUCCESS` at `17:17:52Z`.

## The mechanics, source-read

1. `.github/workflows/doc-validation.yml:536` passes the body into the check as
   `PR_BODY: ${{ github.event.pull_request.body }}` — the body **as it stood in the event payload
   that started the run**, not the body as it stands now. `PR_HEAD_SHA` at line 537 comes from the
   same frozen payload.
2. `gh run rerun` re-executes a run against its **original** payload. So a rerun of a `synchronize`
   run re-reads the body from the moment of that push. If the body was wrong then — a missing
   `## Intent`, or a `head=` that no longer matches after a later push — it is still wrong on every
   rerun, forever. The condition being waited on is immutable, so the retry can never converge.
3. The workflow already knows this and says so, at `doc-validation.yml:73-79`: `edited` is in the
   trigger list precisely so "a PR that adds or fixes its `## Intent` section via a body edit
   re-runs the `Intent section` job and self-heals the stale-red — no wasted empty-commit push".
   The comment documents the cure. Nothing anywhere documents that the *reflex* is a poison.
4. The rerun is not merely futile — it is **actively regressive**. `merge-gates.sh:30-32`:
   > Rollup dedup: required CheckRuns with the same `.name` are deduped to the entry with the latest
   > `.startedAt` so stale FAILUREs from rerun jobs don't falsely block.
   Latest-`startedAt`-wins is the right rule for its stated purpose, but it cuts both ways: a rerun
   always carries the newest `startedAt`, so a rerun that fails **displaces whatever was there
   before**. On #2180 the pre-rerun state was a `CANCELLED` entry that blocked nothing; the post-rerun
   state was a terminal `FAILURE` that blocked everything. The remedy manufactured the block it was
   invoked to clear.

## The class, and how small it is

Exactly **one** PR-gating workflow in this repo reads a frozen payload field as its subject:

```
grep -rln 'github.event.pull_request.body\|github.event.pull_request.title' .github/workflows/
  .github/workflows/doc-validation.yml     # pull_request: [opened, synchronize, reopened, edited]
  .github/workflows/lock-cleanup.yml       # pull_request: [closed] — not a gate
```

That is the whole population today. The bound is what makes a gate cheap; the fact that the
population is one is also why the trap has never been written down.

## Why the existing docs point the wrong way

[`merge-gates.md`](../../../agent-rules/merge-gates.md) is where an agent looks when a check is red,
and it prescribes `gh run rerun` twice without scoping:

- line 97, halt-code 8 (*Cancelled-while-pending*): "Rerun the named run(s) (`gh run rerun <id>` from
  the BLOCK output), then re-poll".
- line 245, in the recovery recipe: `gh run rerun <run-id>  # the run whose job is CANCELLED, not the
  newer one`, followed by "No push, no force, no PR-body re-pin — none of those touch the stale
  context."

Both are correct **for the concurrency-collapse case they were written for**, where the run never
executed and the payload is irrelevant. Neither says "unless the workflow's subject is the event
payload". The last quoted sentence is the exact inversion of the truth for `doc-validation` — there,
the PR-body re-pin is the *only* thing that touches it.

## Concrete next action

1. **Scope the rerun remedy where it is prescribed.** Add a one-line carve-out at
   `merge-gates.md:97` and `:245`: a rerun cannot fix a check whose input is the event payload
   (`github.event.pull_request.*`); for those, edit the PR body to fire the `edited` trigger.
   Enumerator: the `grep -rln` above — keep the carve-out keyed on that command, not on a hardcoded
   workflow name, so it stays true when the population grows.
2. **Make the workflow say it at the point of temptation.** The `doc-validation.yml:73-79` comment
   explains why `edited` exists; extend it with the inverse — that `gh run rerun` replays the stale
   body and can never pass. An agent reading the failing workflow should not have to find the
   remedy doc to learn the remedy is wrong here.
3. **Assert the property rather than the instance**, in `tests/bats/workflow_job_mask.bats` (the
   existing workflow-YAML-shape suite): every `pull_request`-triggered workflow that references
   `github.event.pull_request.body` or `.title` MUST list `edited` in its `types:`. Without `edited`
   such a gate has *no* re-trigger at all short of a new commit — the failure mode one config edit
   away from today's, and the one a name-based carve-out would not catch.
4. **Record the dedup's second edge.** The `merge-gates.sh:30-32` comment justifies latest-wins in
   one direction only ("stale FAILUREs … don't falsely block"). Note the other: a rerun's fresh
   FAILURE displaces an older SUCCESS or a harmless CANCELLED. The rule is still right; a reader
   deciding whether to rerun needs to know it is not free.

Triggered-follow-up: when=pr-count:base=develop;since=2026-09-07;n=20; action=re-check whether the rerun carve-out landed in merge-gates.md, whether any new workflow reads `github.event.pull_request.body` without an `edited` trigger, and whether another rerun-induced FAILURE displaced a green; baseline=1 rerun-manufactured FAILURE on PR #2180 and 1 payload-reading gate repo-wide, 2026-09-07; fired=never
