# `cr-finding-gate` posts an unbounded `pending` that no enforcement layer blocks on

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-19
- **Observed on**: PR #2127 (merged past it under an explicit user authorisation) — and #2130, #2122, #2121, #2119, #2118, #2117 before it, all merged with the same context `pending` and **no** override label
- **Status**: open

## What happened

`CR findings (0 actionable)` — the StatusContext `cr-finding-gate.yml` posts as its verdict — has
been stuck `pending` on every head in this repository for at least ~18 h (first observed merge
`2026-08-18T16:58Z`, still true now). Seven PRs merged past it. Seven more (#2132, #2131, #2129,
#2126, #2125, #2101, #2080) are sitting on it right now.

Nobody bypassed anything. GitHub reported each PR mergeable, because branch protection requires the
**job** (`CR finding gate`) and the job is green — while the **context** the job posts is `pending`.
The two names are different, and only one of them is guarded.

Full RCA: [`postmortems.md`](../../postmortems.md) § 2026-08-19 · PR #2127.

## The mechanics, source-read

1. CodeRabbit's OSS star-gate (<10 stars) makes every PR report
   `Review skipped: manual review required for this OSS repository`. `decide()` in
   `.github/actions/cr-finding-gate/action.yml` **refuses** to read that as a verdict
   (`maybe_nudge_review never-reviewed; return 1`) — correctly, guarding the #2028 fail-open.
2. But that refusal has **no terminal branch**. The poll exhausts `POLL_BUDGET_SECONDS=180`, exits
   the loop, and posts `pending "awaiting CodeRabbit review on current head"`. The condition it waits
   on is a repo-plan property, not a race — so the wait can never end.
3. The action runs `set +e` and **exits 0** on every terminal verdict (deliberate: it keeps the
   `if: always()` fallback poster alive through a step timeout). Job green, context pending.
4. `project.config.json` § `branch_protection.required_contexts` names `CR finding gate` and not
   `CR findings (0 actionable)` — confirmed by the auto-derived 22-name `requiredContexts` array in
   #2127's own `merge-snapshots.jsonl` row. GitHub says mergeable; `merge-gates.sh`, which blocks on
   **any** check, says `GATES_TIMEOUT`. Two layers, two answers.
5. `postmortem-owed.sh` cannot report it. Trigger 1 needs a *terminal* non-SUCCESS
   (`IN_PROGRESS/QUEUED/PENDING never count`; StatusContexts matched only on
   `IN("FAILURE","ERROR")`). The `required-never-terminal` column that **does** select
   `IN("PENDING","EXPECTED")` is AND-gated on the same `$reqNames` set point 4 shows this context is
   missing from. Trigger 2 needs an override label; none was used. `--list` after #2127's merge
   reported `no gate escapes owed a postmortem (last 20 merges clean)`.

The system-level shape: `merge-gates.sh` deliberately **bounds** its CR wait
(`MERGE_GATES_CR_GRACE_POLLS`, default 10, WARN-and-pass — "so a stuck integration never wedges the
ship-loop indefinitely") while the producer it waits on posts an **unbounded** `pending`. The
no-wedge hatch is defeated by the thing it hatches against.

## Relationship to the 2026-08-17 entry

[`2026-08-17-cr-finding-gate-accepts-a-verdict-line-without-a-review.md`](2026-08-17-cr-finding-gate-accepts-a-verdict-line-without-a-review.md)
covers the **review-assurance ladder** — whether a review actually happened, and how three rungs
each convert "not reviewed" into "reviewed, clean". This entry is the **gate mechanics** underneath
it: even once you correctly refuse to score a non-review as a pass (which `cr-finding-gate` already
does), the refusal has nowhere terminal to go, and the resulting `pending` is invisible to every
layer that could act on it. Same upstream cause (auto-review off), opposite failure direction — that
one fails *open*, this one fails *silent*. Fix them together; neither subsumes the other.

## Concrete next action

Ordered — (b) before (a) turns a soft repo-wide wedge into a hard one.

1. **(a) Add a terminal arm for the never-reviewed-by-policy input.** On
   `manual review required for this OSS repository`, stop polling and `post failure` naming the
   condition (e.g. `cr-auto-review-disabled (OSS <10 stars) — needs cr-out-of-band +
   cr-disposition:cr-auto-review-disabled`), mirroring the distinct `cr-quota-exhausted` verdict the
   #1962 postmortem proposed for the sibling input. A terminal `failure` is visible to
   `merge-gates.sh`, to branch protection, and to `postmortem-owed.sh`; an unbounded `pending` is
   visible to none of them. Enumerator:
   `grep -n 'manual review required\|POLL_BUDGET_SECONDS\|post pending' .github/actions/cr-finding-gate/action.yml`.
2. **Assert the invariant this escape violated**, in `tests/bats/cr_finding_gate.bats`: the action
   must never conclude its job green while the context it posted is `pending`. That single assertion
   catches every future variant of "refused, but had nowhere terminal to go".
3. **(b) Then close the name gap.** Add `CR findings (0 actionable)` to
   `branch_protection.required_contexts`. This makes GitHub and the poller guard the same name and
   switches on the detector's existing `required-never-terminal` column for free. Enumerator:
   `jq '.branch_protection.required_contexts' project.config.json`.
4. **Generalise it into a parity check**: every status context a first-party workflow posts as a
   *gate verdict* must appear in the required set. That is the check that would have caught point 4
   at authoring time instead of at merge #7. Enumerator:
   `grep -rn 'repos/.*/statuses/' .github/` cross-referenced against the required set.
5. **Name the human action as a human action.** Enabling CR auto-review (10 stars, or a paid plan)
   is the real remediation and is not a gate. Recording it here keeps 1–4 from being mistaken for a
   fix to the underlying blindness.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-19;n=15; action=re-check whether `CR findings (0 actionable)` still posts `pending` on merged heads, whether the terminal arm shipped, and whether the context joined required_contexts; baseline=7 consecutive merges past a pending context with 0 override labels, 2026-08-18/19; fired=never
