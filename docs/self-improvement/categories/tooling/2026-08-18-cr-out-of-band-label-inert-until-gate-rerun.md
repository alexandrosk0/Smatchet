# `cr-out-of-band` downgrades the CR gate but cannot clear the CR gate's own pending status — the label is inert until someone re-runs the workflow by hand

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-18
- **Found during**: un-wedging [PR #2070](https://github.com/alexandrosk0/Smatchet/pull/2070) (`branch-protection-enforce-admins`), stuck 116 watcher cycles

## Symptom

PR #2070 sat wedged behind an exhausted CodeRabbit review quota. The operator took the
sanctioned waiver path — applied `cr-out-of-band` + `cr-disposition:cr-rate-limited`
with a justifying comment. `merge-gates.sh` acknowledged the waiver:

```
WARN: cr-out-of-band + cr-disposition label downgraded CR block (…) to WARN
```

and still refused to emit `GATES_PASSED`:

```
Poll 1/1 — CI: 21/22 pass (0 fail, 1 pending, 0 warn-downgraded, 0 req-missing) | …
```

The one pending item **was the CR gate's own StatusContext**, `CR findings (0 actionable)`,
description `awaiting CodeRabbit review on current head`. The waiver that exists precisely
to unblock this state could not unblock it.

## Cause

Two mechanisms that both key on `cr-out-of-band` are not wired to each other:

1. **`merge-gates.sh`** downgrades the *CodeRabbit gate* (gate 2) to WARN. It deliberately
   does **not** touch the CI counters — the comment at
   [`merge-gates.sh:1414`](../../../../agents/scripts/core/merge-gates.sh) says so outright:
   *"(ci_fail / ci_pend) … are NOT touched"*. But `GATES_PASSED` requires `ci_pend -eq 0`
   (lines 1502 and 1557), and `CR findings (0 actionable)` is an ordinary StatusContext that
   lands in `ci_pend` (`fields[7]`, line 736) like any other check.
2. **The CR finding gate action** ([`.github/actions/cr-finding-gate/action.yml:249-254`](../../../../.github/actions/cr-finding-gate/action.yml))
   *does* honour the label — it fetches labels live and posts `success` with
   `cr-out-of-band label set — gate overridden`. That branch is the thing that clears
   `ci_pend`.

The gap: **nothing re-triggers the workflow when the label is applied.**
[`cr-finding-gate.yml`](../../../../.github/workflows/cr-finding-gate.yml) triggers on
`pull_request` (default types — no `labeled`), `pull_request_review`,
`pull_request_review_comment`, and `issue_comment`, and the `issue_comment` arm is
additionally gated on a **CodeRabbit-authored** comment. Labelling the PR fires nothing;
the operator's own disposition comment fires nothing.

So the override sat live in the API and inert in CI. The unwedge required knowing to run
`gh run rerun 32035340446` against a stale run of that workflow, purely so the action would
re-read the labels. After the re-run the status flipped in one poll and the merge went
through (`22/22 pass, 0 pending` → `GATES_PASSED` → merged `30d09b2dbcbd`).

## Proposed fix

1. **Add `labeled` / `unlabeled` to the workflow's `pull_request` types** (~15 min). A
   labelled event re-runs the action, which already reads labels live and already has the
   override branch. `unlabeled` matters too — pulling the waiver should re-evaluate, not
   leave a forged `success` behind.
2. **Make `merge-gates.sh` self-consistent.** When the CR gate is downgraded by
   `cr-out-of-band` + disposition, discount the CR gate's own StatusContext from `ci_pend`
   (it is the same signal counted twice — once as gate 2, once as a check). Leaving it in
   means the documented waiver can never produce `GATES_PASSED` on its own, which is the
   defect above stated at the poller instead of the workflow. Guard with a bats case in
   `tests/bats/merge_gates.bats` asserting `cr-out-of-band` + disposition + a pending
   `CR findings (0 actionable)` reaches `GATES_PASSED`.
3. **Say the next step in the block message.** If (2) is rejected as too clever, the
   `BLOCK:` line should name the manual step (`re-run the "CR finding gate" workflow so the
   override is re-evaluated`) rather than leaving the operator to derive it from two files.

## Why it matters

This is a waiver that reports as applied and does not apply. The operator sees
`WARN: … downgraded CR block … to WARN` — a success message — and a poll that still blocks,
with no line connecting the two. The only escape a reader is likely to find from there is
the admin-merge carve-out, i.e. the documented safe path pushes people onto the unsafe one.
`cr-out-of-band` is the repo's designated pressure valve for exactly the CR-quota wedge that
cost #2070 116 watcher cycles; a pressure valve that needs an undocumented `gh run rerun` to
open is not a pressure valve.

## Recurrence — 2026-08-19, [PR #2124](https://github.com/alexandrosk0/Smatchet/pull/2124)

Same defect, different trigger, one day later. Not a quota exhaustion this time: CodeRabbit
posted `Review skipped: manual review required for this OSS repository` (the repo is under
CR's 10-star auto-review threshold, so CR reviews **nothing** unsolicited). The gate's own
auto-nudge posted `@coderabbitai review`; CR never answered. `CR findings (0 actionable)`
sat `pending / awaiting CodeRabbit review on current head` for **2h10m** with 0 reviews on
the PR, and the 90-poll gate run ended `GATES_TIMEOUT`.

Applying `cr-out-of-band` + `cr-disposition:oss-threshold-no-auto-review` again changed
nothing until `gh run rerun 32190691612` was issued by hand, exactly as documented above.
After the re-run the status flipped to `cr-out-of-band label set — gate overridden` and the
next poll reached `GATES_PASSED`.

Two things this recurrence adds to the fix list:

- **Fix 1 (`labeled` / `unlabeled` trigger) is the load-bearing one.** Both incidents were
  un-wedged by a manual re-run whose only purpose was making the action re-read labels.
- **The sub-10-star state is permanent, not incidental.** Unlike a rate limit, it never
  clears on its own — every PR on this repo reaches `pending` and stays there unless a human
  asks CR for a review or waives the gate. #2117, #2119, and #2122 all merged carrying
  `CR findings (0 actionable) = pending`, i.e. the gate is routinely bypassed rather than
  satisfied. Worth deciding explicitly whether the nudge should be retried on a schedule, or
  whether the CR gate should have a documented terminal disposition for repos CR will not
  auto-review — the status quo is a required-looking check that nobody can turn green.

## Recurrence — 2026-08-19, [PR #2131](https://github.com/alexandrosk0/Smatchet/pull/2131)

Third occurrence, second in 24 hours, same trigger as #2124 (the permanent sub-10-star
`Review skipped: manual review required for this OSS repository` state). PR open
05:54:16Z → merged 11:20:38Z, **5h26m**, with **0 reviews** on it the whole time;
`CR findings (0 actionable)` never left `pending / awaiting CodeRabbit review on current head`.
Every other gate was clean throughout — CI 30 pass / 9 skipping / 0 red, Bugbot pass with
0 findings in 2m5s, 0 review threads, 0 non-bot comments. Gate 2 alone held the merge.

What this occurrence adds is a **negative** result the first two did not isolate. A plain
re-run of the wedged workflow, with **no label applied**, is not sufficient:
`gh run rerun 32221202456` completed (`96049595922`, `96042985248` — both pass) and the
status stayed `pending`. Only `cr-out-of-band` + `cr-disposition:oss-threshold-no-auto-review`
*followed by* a re-run flipped it to `cr-out-of-band label set — gate overridden`.

So the manual step is not "re-run the workflow" — it is the ordered pair *(apply label,
then re-run)*, and the ordering is silent: labelling fires no event, and a re-run without the
label reports success while changing nothing. That is two ways to do the documented recovery
and get no signal that you did it wrong. It also sharpens fix 1 — a `labeled` trigger removes
the ordering hazard entirely, because the label *is* the event.

Sequence across the three incidents, for whoever picks up the fix:

| PR | Trigger | Wedged for | Cleared by |
|---|---|---|---|
| [#2070](https://github.com/alexandrosk0/Smatchet/pull/2070) | CR review quota exhausted | 116 watcher cycles | label + `gh run rerun 32035340446` |
| [#2124](https://github.com/alexandrosk0/Smatchet/pull/2124) | sub-10-star, nudge unanswered | 2h10m, `GATES_TIMEOUT` | label + `gh run rerun 32190691612` |
| [#2131](https://github.com/alexandrosk0/Smatchet/pull/2131) | sub-10-star, nudge unanswered | 5h26m | label + `gh run rerun 32221202456` (bare re-run first: no effect) |
