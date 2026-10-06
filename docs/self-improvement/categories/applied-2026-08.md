# Agent self-improvement — applied (archive partition 2026-08)

> Rotated slice of [`applied.md`](applied.md) (see its header for format /
> categories / workflow). Entries whose original surface date falls in
> 2026-08, sorted latest first. Append-only via
> `agents/scripts/core/rotate-applied-md.sh`; do not file new work here.
>
> **Deleted-runtime banner (2026-05-21)** — entries below that reference the
> agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`,
> `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`,
> `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`,
> `CiFailureClassifier`, the `dispatch_source` enum, the sentinel-file protocol,
> `agent/<proposalId>` worktrees, the `coderabbit-react-loop` design,
> `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code
> removed 2026-05-21 (v1 PR1 of `../../plans/shipped/github-tracker-backend.md`,
> merge sha `b1d241bc`). Preserved as historical record of what was tried.

<!-- Latest first. Appended by rotate-applied-md.sh only. -->

# `cr-finding-gate`'s auto-nudge is bot-authored, and CodeRabbit ignores bot-authored commands

- **Category**: process
- **Priority**: P2
- **Date**: 2026-08-30
- **Observed on**: PR #2176 (both heads: the initial review head and `b335714be` after the CR-Major fix)
- **Status**: applied — 2026-09-09 (OSS manual-trigger playbook: labeled trigger + terminal failure + merge-gates discount + scripts/dev/trigger-coderabbit-review.sh)

## What happened

On a sub-10-star repo CodeRabbit posts `Review skipped: manual review required for this OSS
repository` on every head. `cr-finding-gate`'s `maybe_nudge_review never-reviewed` rung
(`action.yml:511`) is supposed to break that state by commenting `@coderabbitai review` — but the
comment posts via the workflow's `GITHUB_TOKEN`, i.e. authored by `github-actions[bot]`, and
**CodeRabbit does not act on bot-authored commands**. The nudge lands, CR stays silent, the
context stays `pending "awaiting CodeRabbit review on current head"` forever.

Observed twice on #2176: after each push the gate's own nudge produced nothing; the pending
resolved ONLY once a **human-authored** `@coderabbitai review` PR comment was posted out-of-band
(sanctioned human-nudge carve-out at `merge-gates.sh:149-150`). After the human comment CR
reviewed the head, the context went terminal, and the merge proceeded on real review evidence —
so `merge-gates.sh`'s block-on-any-red held correctly; the dead rung just converts "auto-heal"
into "silent wedge until a human notices".

## Relationship to existing entries

[`2026-08-19-cr-finding-gate-posts-an-unbounded-pending-no-layer-blocks-on.md`](applied.md)
documents that the never-reviewed refusal has no terminal arm (fails silent). This entry adds the
*reason the wait can never self-heal*: the one automated actor that could end it speaks with a
voice CodeRabbit is deaf to. Fix both together — a terminal arm makes the wedge visible; a
human-credentialed nudge (or dropping the dead rung) makes the auto-heal real.

## Concrete next action

1. Make the nudge human-credentialed: post it with the orchestrator's user token (`ORCH_USER`
   path already exists in `safe-merge.sh`) from the *caller* side rather than the workflow side —
   or delete the `never-reviewed` nudge rung outright and document that resolution requires a
   human comment, so the gate does not pretend to an ability it lacks.
   Enumerator: `grep -n 'maybe_nudge_review\|GITHUB_TOKEN' .github/actions/cr-finding-gate/action.yml`.
2. Assert in `tests/bats/cr_finding_gate.bats` that whichever path remains is honest: either the
   nudge is posted with a non-bot credential, or the never-reviewed branch posts a terminal
   verdict naming the human action required.

## Follow-up observation — 2026-09-12 (PR #2184)

Re-checked on PR #2184. The finding **reproduced, and the dead rung is still in place**, but with one
correction to how "applied" should be read.

- **The rung still posts as `github-actions[bot]`.** `.github/actions/cr-finding-gate/action.yml:158-165`
  documents it in its own comment: *"the nudge posts via the workflow token, i.e. as
  `github-actions[bot]`"*. The 2026-09-09 remedy shipped
  `scripts/dev/trigger-coderabbit-review.sh` — a **human-run playbook**, which is a real improvement
  to the recovery path but does not repair the automated rung. Action 1 of this entry ("make the
  nudge human-credentialed, or delete the rung outright") is therefore still open, and the entry's
  `Status: applied` overstates coverage.
- **Bot nudge 0 for 3 on one PR.** #2184 fired auto-nudges at 2026-09-06T19:03:11Z,
  2026-09-07T00:02:27Z and 2026-09-07T18:59:32Z. The first two were ignored for ~20 h and ~15 h; the
  third was likewise never acted on. All three pre-date the 2026-09-09 remedy, so they are evidence
  about the rung, not about the playbook.
- **Human nudge acted on in 7 seconds, twice.** A human-authored `@coderabbitai review` at
  2026-09-07T15:37:15Z drew a CodeRabbit reply at 15:37:22Z (7 s). A second at 20:08:15Z drew a reply
  at 20:08:20Z (5 s) and the completed review that produced 2 actionable findings. The asymmetry is
  now 0/3 bot vs 2/2 human on this PR alone, or **0/5 bot vs 4/4 human** counting #2176.
- **New adjacent fact worth folding in:** the first human nudge was refused with *"Review rate
  limited"*, and CodeRabbit's summary comment states the real constraint — *"Your plan provides up to
  1 included review per hour; 0 remain after this review"*. So on this repo the recovery path is
  bounded by an OSS quota as well as by authorship; a human-credentialed nudge fixes the authorship
  half only, and the terminal-arm work in
  [`2026-08-19-cr-finding-gate-posts-an-unbounded-pending-no-layer-blocks-on.md`](applied.md)
  is what makes the quota wait *visible* rather than silent.

Suggested status correction: `applied` → `partially applied` (human playbook shipped; automated rung
unchanged).

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-30;n=15; action=re-check whether the never-reviewed nudge still posts as github-actions[bot] and whether any PR resolved the pending without a human comment; baseline=2 bot-nudges ignored, 2 human nudges acted on, PR #2176 2026-08-29/30; fired=2026-09-12
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — the never-reviewed nudge rung is no longer reached (only stale-clean has call sites) and the OSS state posts a terminal failure naming the human step. Hypothesis, not mechanism: bot-authored nudges drew no review in the observed cases while human ones did (one bot nudge on #2036 did draw a reply) — see the observation-vs-mechanism rule in AGENT_SELF_IMPROVEMENT.md § Workflow.
  Status: applied (2026-10-04)

- 2026-08-20 · orchestrator · [process] · P2 — UI feature sourced from a runtime-populated cache shipped non-functional in its headline flow (#2148 → #2149): trace the cache's producers (incl. empty/error paths), prefer interaction-time data capture, and never merge past an outstanding visual-validation pause on a bare "merge"
  Details: The #2148 `reads as` echo named accountIds only via `GetAvailableUsers()` + session search results; the catalog's user list is legitimately empty (warning-only fetch failure, cache-restored catalog), so the headline flow (select user → see name) rendered nothing. Green everywhere: pure tests stocked the catalog by construction, bucket-C/E goldens don't cover the query editor. The autocomplete row the user clicked carried both id and name the whole time — capture at the interaction point needed no cache and no network (what #2149 does). The visual-validation exception (AGENTS.md pause 5) was flagged open on every status update yet a bare "merge" executed without restating it.
  Concrete next action: (1) one sentence in `docs/agent-rules/ship-loops.md` § Visual-validation exception: a pending visual verdict survives a bare "merge" — restate the unverified behavior and require an explicit merge-anyway. (2) review-checklist line (code-review agent prompt or `docs/agent-rules/cpp-rules.md` § Quality): UI output read from an app-owned cache ⇒ PR body enumerates the cache's producers + the empty-state behavior; "renders nothing" is a finding unless intended. (3) promote the postmortem's eval case (postmortems.md 2026-08-20) into the subagent-eval suite.
  Resolution: applied 2026-10-04 (backlog-sweep-2026-10) — (1) was already in docs/agent-rules/ship-loops.md § Visual-validation exception ('An outstanding visual verdict survives a bare merge'); (2) review-checklist line 'Cache-sourced UI output' added to agents/core/code-review.md; (3) eval case tests/agent-eval/code-review/cr-reads-as-empty-user-catalog.json (schema-valid; scoring it needs a live harness run, not done here).
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

# `cr-finding-gate` posts an unbounded `pending` that no enforcement layer blocks on

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-19
- **Observed on**: PR #2127 (merged past it under an explicit user authorisation) — and #2130, #2122, #2121, #2119, #2118, #2117 before it, all merged with the same context `pending` and **no** override label
- **Status**: applied — 2026-09-09 (OSS manual-trigger playbook: labeled trigger + terminal failure + merge-gates discount + scripts/dev/trigger-coderabbit-review.sh)

## What happened

`CR findings (0 actionable)` — the StatusContext `cr-finding-gate.yml` posts as its verdict — has
been stuck `pending` on every head in this repository for at least ~18 h (first observed merge
`2026-08-18T16:58Z`, still true now). Seven PRs merged past it. Seven more (#2132, #2131, #2129,
#2126, #2125, #2101, #2080) are sitting on it right now.

Nobody bypassed anything. GitHub reported each PR mergeable, because branch protection requires the
**job** (`CR finding gate`) and the job is green — while the **context** the job posts is `pending`.
The two names are different, and only one of them is guarded.

Full RCA: [`postmortems.md`](../postmortems.md) § 2026-08-19 · PR #2127.

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

[`2026-08-17-cr-finding-gate-accepts-a-verdict-line-without-a-review.md`](process/2026-08-17-cr-finding-gate-accepts-a-verdict-line-without-a-review.md)
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

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-19;n=15; action=re-check whether `CR findings (0 actionable)` still posts `pending` on merged heads, whether the terminal arm shipped, and whether the context joined required_contexts; baseline=7 consecutive merges past a pending context with 0 override labels, 2026-08-18/19; fired=2026-08-30

## Re-check 2026-08-30 (fired via PR #2176 ship-loop)

- **Terminal arm: NOT shipped.** `action.yml:510-511` still handles `manual review required` by
  nudging + `return 1` — no `post failure`; `:635` still posts the unbounded
  `pending "awaiting CodeRabbit review on current head"`.
- **Name gap: still open.** `project.config.json` `.branch_protection.required_contexts` carries
  the job name `CR finding gate`, not the posted context `CR findings (0 actionable)`.
- **But no merge escaped past it this time**: on #2176 `merge-gates.sh` block-on-any-red held the
  merge until the pending context resolved (poll 41/90, 22/22 green) — the baseline's
  "7 consecutive merges past a pending context" class did not recur. The remaining risk is the
  silent wedge, not a silent escape; the wedge's root cause is the bot-authored nudge —
  see [`2026-08-30-cr-gate-auto-nudge-is-bot-authored-so-coderabbit-ignores-it.md`](applied.md).
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — the OSS arm posts a terminal failure naming the human action (action.yml, bats-covered), and block-on-any-red holds merges on a pending context. Residual NOT done here: adding the CR findings context to branch protection's required contexts is an admin design call (merge-gates.md keeps CR PR-advisory by design).
  Status: applied (2026-10-04)

# The default merge path arms auto-merge and `exec`s away — no actor writes the ledger row

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-19
- **Observed on**: PR #2115 (permanent hole, past the janitor's 6 h repair window), PR #2134 (hole closed by
  hand ~1 h later, by this entry's PR #2137), and PR #2137 itself (the predicted regress, observed: it armed
  through `safe-merge.sh`, merged at `e272d630`, and left no row — closed by hand in the follow-up PR)
- **Status**: open

## What happened

`agents/scripts/core/safe-merge.sh` is the **sanctioned default non-admin merge path**
(`docs/agent-rules/ship-loops.md` § step 3, PR-1: "an armed or autonomous merge MUST go through
`safe-merge.sh` — never a bare `--auto`"). Its last statement is:

```bash
# agents/scripts/core/safe-merge.sh:555
exec gh pr merge "$pr" --squash --auto
```

`exec` replaces the shell process. There is no code path after the arm **by construction**, so the
script cannot observe its own merge and cannot append the ADR-0017 gate-verdict snapshot. Grepping
the script for `merge-snapshot-append` / `append_merge_snapshot` returns nothing; the repo-wide
referrer set is `git-janitor.sh`, `postmortem-owed.sh`, `safe-admin-merge.sh` (plus docs + bats).
The default path is absent from it.

The rule exists — it is just prose. `ship-loops.md` § step 3 names a **"Fourth writer — the session
that ARMED an auto-merge"**, which must run the helper with actor `orchestrator-automerge` and
`SNAPSHOT_MERGED_AT=<mergedAt>` on receiving the merged notification. Nothing enforces or performs
it, so coverage equals whatever the orchestrator remembers. The ledger shows both outcomes in the
same week: rows for #2114 / #2015 carry `"mergeActor":"orchestrator-automerge"` (remembered), and:

| PR | merged | ledger row | status |
|---|---|---|---|
| #2115 | 2026-08-18T14:15:44Z | **none** | permanent — past `SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS` (6 h), and the retro-compose prohibition forbids reconstructing it now |
| #2134 | 2026-08-19T12:08:35Z | appended by hand | closed ~1 h post-merge, only because the hole was noticed |

#2115's own title is `chore(ledger): merge-time gate snapshot for PR #2114` — the PR that closed one
hole opened the next one.

## Why it matters

ADR-0017 § Distributed-write contract states the mitigation as *"all three actors are named **and
wired** … behind one shared helper so they stay consistent."* For the path that carries most merges,
"wired" is not true: `safe-admin-merge.sh` (the narrow stale-BLOCKED carve-out) appends in code, and
`git-janitor.sh --post-merge` Step 5.5 backfills in code, but the **default** path does not. Ledger
coverage is therefore biased *away* from ordinary merges and *toward* the exceptional ones.

A hole costs losslessness, not blindness — `postmortem-owed.sh` falls back to the live
`statusCheckRollup`. But that fallback is exactly what ADR-0017 calls provably lossy: GitHub
overwrites rollup contexts by name on re-run and strips override labels post-merge. So the merges
whose gate truth is recoverable are the rare ones, and the merges whose truth silently degrades are
the routine ones.

There is also a **regress** the current design does not terminate: a `chore(ledger)` PR is itself a
merge that owes a row, so landing row N opens hole N+1. The only terminators are (a) the janitor's
6 h backfill actually running, or (b) batching the row into an unrelated develop-bound commit. This
entry's own PR #2137 is an instance, and the prediction held: it landed #2134's row, armed through
`safe-merge.sh`, merged at `e272d630` on a 22/22 `GATES_PASSED` poll, and wrote nothing. Its row is in
the follow-up PR carrying this edit — which is itself instance four. The chain does not converge by
hand-appending; only action 1 below ends it.

## Concrete next action

Ranked, cheapest first.

1. **Make `safe-merge.sh` the fifth code writer.** Drop the `exec` (call `gh pr merge --squash
   --auto` normally), then poll `gh pr view "$pr" --json state,mergeCommit,mergedAt` on a short
   bounded budget and, on `MERGED`, call `append_merge_snapshot "$pr" <mergeCommit> <headSha>
   GATES_PASSED "<downgraded-csv>" "<override-csv>" orchestrator-automerge` with
   `SNAPSHOT_MERGED_AT` from the API. A short budget suffices because the script only arms **after**
   `GATES_PASSED` — every check is already terminal-green, so GitHub merges in seconds. The
   `redChecks` projection needs no new logic: `safe-merge.sh` already parses the poll's
   `GATE_SNAPSHOT cr_override=… downgraded=…` line at line 156 (`loadbearing_oob_labels`) for its
   obligation-stub path, and holds the label list in the same scope.
2. **Make the timeout branch mechanical, not remembered.** If the bounded wait expires (auto-merge
   still queued), print the ready-to-paste `merge-snapshot-append.sh` invocation with all 7 args
   pre-filled and `SNAPSHOT_MERGED_AT=` stubbed. Precedent in the same script: `file_obligation_stub`
   already converts a would-be-remembered obligation into a written artefact.
3. **Detect the residual hole inside the repair window.** Have the SessionStart nudge (or
   `postmortem-owed.sh`) flag any PR merged in the last 6 h with no ledger row, so a miss surfaces
   while `git-janitor --post-merge` can still legitimately backfill it — instead of hardening into a
   permanent hole like #2115.

**Enumerator + replay** (per AGENT_SELF_IMPROVEMENT.md): the gate is a new case in the existing
`tests/bats/safe_merge.bats` (205 lines, 14 `@test`s), asserting that after a PASS-gated arm exactly
one row lands in `MERGE_SNAPSHOT_LEDGER` (an existing env seam in `merge-snapshot-append.sh`) with
`.pr` = the PR, `.mergeActor` = `orchestrator-automerge`, `.gates` = `GATES_PASSED`.

Replayed against the script as it stands, that case **cannot pass**: `exec` at line 555 replaces the
process, so no append can run and the temp ledger stays empty — the #2115 / #2134 shape exactly.

Writing the case also needs one seam the fix must add. Today the arm block short-circuits on
`[ "${SAFE_MERGE_DRY_RUN:-}" = "true" ] || [ -n "${SAFE_MERGE_STUB_GATE:-}" ]`, so stubbing the gate
*forces* the `DRY-RUN: would run: gh pr merge …` exit — which is precisely what the existing
`arms auto-merge when the gate PASSES (exit 0)` bats case asserts on, and why it stops one line
short of the behaviour at issue. Separate the two conditions (keep the DRY-RUN exit; let a stubbed
gate proceed against a stub `gh` on `PATH` reporting `MERGED` + a merge oid) and the post-arm write
becomes testable. The 13 `--selftest` cases are unaffected — they exercise helpers
(`loadbearing_oob_labels`, `maybe_file_obligations`, `run_gate`, `default_flip_ready`) directly and
never enter the arm block at all.

Related: [`docs/adr/0017-merge-time-snapshot-ledger.md`](../../adr/0017-merge-time-snapshot-ledger.md)
(the losslessness argument + the writer set), [`docs/agent-rules/ship-loops.md`](../../../agent-layer/docs/agent-rules/ship-loops.md)
§ step 3 (the prose fourth-writer rule this entry proposes to turn into code).

## Follow-up measurement — 2026-09-12 (PR #2184)

Re-measured. **The entry's prediction held completely, and the hole is no longer occasional — it is
total.**

- **`safe-merge.sh` still `exec`s away.** `exec gh pr merge "$pr" --squash --auto` is still the last
  statement (line 555, unchanged), and `grep -n 'append_merge_snapshot' agents/scripts/core/safe-merge.sh`
  still returns nothing. Action 1 is untouched.
- **Ledger coverage since this entry: 0%.** The newest row in
  `docs/self-improvement/merge-snapshots.jsonl` is **#2137, 2026-08-19T13:13:10Z** — this entry's own
  PR, hand-appended. Since that timestamp **58 PRs have merged into `develop` and 0 carry a row**
  (152 rows total, none newer). The baseline was 1 permanent + 1 hand-closed hole across 5 session
  merges; the measured reality is 58 for 58.
- **Fresh instance, observed live.** PR #2184 merged 2026-09-12T12:30:00Z as `0685b5f0bbd6`, armed
  through `safe-merge.sh` on a clean `GATES_PASSED` poll (CI 22/22, `GATE_SNAPSHOT cr_override=0
  downgraded=`). No row was written. That snapshot line is exactly the data the ledger wants and it
  was discarded at `exec`.
- **The hand-append terminator is now decisively disproven.** This entry predicted that landing row N
  opens hole N+1 and that only action 1 converges. Over 58 merges nobody hand-appended even once, so
  the practice did not merely regress — it stopped entirely. Any future backfill is also blocked:
  all 58 are far past `SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS` (6 h), and the retro-compose
  prohibition forbids reconstructing them. **The ledger has a permanent 58-PR hole**, an order of
  magnitude past the 28-PR hole that
  [`2026-08-18-merge-snapshot-ledger-28-pr-hole.md`](applied.md)
  was raised for.
- **Consequence now visible in practice:** the #2160 gate-escape postmortem filed 2026-09-12 could
  not recover *why* `plan-lock-out-of-band` was applied, precisely because the ledger row that would
  have captured the override does not exist and GitHub had stripped the label 43 s after the merge.
  That is ADR-0017's losslessness argument failing in the exact way it predicted.

Suggested priority bump: **P1 → P0**. The permanent-loss rate is 100% of merges and each day adds
irrecoverable rows.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-19;n=15; action=check whether safe-merge.sh appends its own snapshot row, and re-measure the share of merged PRs with a ledger row; baseline=1 permanent hole (#2115) and 1 hand-closed hole (#2134) across 5 session merges on 2026-08-18/19; fired=2026-09-12
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — safe-merge.sh no longer execs away: after arming it polls the PR (bounded by SAFE_MERGE_SNAPSHOT_WAIT_SECONDS), appends the ledger row (actor orchestrator-automerge, redChecks/overrideLabels, crState) on merge, else prints the paste-ready append line; DRY_RUN split from STUB_GATE; bats asserts exactly one row.
  Status: applied (2026-10-04)

# `merge-gates.sh` passes its ~25 KB jq filter on the `gh` command line, which exceeds the Windows 32 KB `CreateProcess` cap — the poller reports `GH_API_DOWN` on a healthy API

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-19
- **Found during**: shipping [PR #2124](https://github.com/alexandrosk0/Smatchet/pull/2124) (README screenshot) from a Windows session

## Symptom

Every invocation of the gate poller died immediately, three attempts in a row:

```
gh: Argument list too long
```

`poll_merge_gates` mapped that to its API-outage classification and returned **3**
(`GH_API_DOWN`) — a verdict that says *GitHub is unreachable*. GitHub was fine; plain
`gh api`, `gh pr view`, and `gh run list` all worked in the same shell seconds later.

Nothing about the failure points at its cause. The operator's reasonable next move —
retry, then wait for the "outage" to clear — never succeeds, because there is no outage.

## Cause

[`merge-gates.sh:646`](../../../agent-layer/agents/scripts/core/merge-gates.sh) invokes:

```bash
gh api graphql -f owner=… -f repo=… -F pr=… -f query="$query_body" --jq "$GATE_FILTER"
```

Both large payloads travel **as command-line arguments**:

- `$GATE_FILTER` — the gate-decision jq program from
  [`merge-gates.d/10-gate-filter.sh`](../../../agent-layer/agents/scripts/core/merge-gates.d/10-gate-filter.sh),
  **25,185 bytes** today and growing with every gate refinement;
- `$query_body` — the GraphQL document from
  [`merge-gates.graphql`](../../../agent-layer/agents/scripts/core/merge-gates.graphql).

Windows caps a `CreateProcess` command line at 32,767 characters. The two together clear
it, so the process never launches. On Linux/macOS the equivalent limit (`ARG_MAX`, ~2 MB)
is far away, which is why CI and the maintainer's non-Windows paths never saw this — the
gate poller is effectively **unrunnable locally on Windows**, on a repo whose primary
development host is Windows 11.

The classification is a second, separable defect: a launch failure of the `gh` binary is
attributed to the *remote* API. `Argument list too long` is an `E2BIG` from the local OS
and can never mean the API is down.

## Workaround used

A `gh` shell-function shim that intercepts `--jq`, writes the filter to a temp file, and
pipes the response through local `jq -r -f "$tmp"`. The GraphQL body alone fits under the
cap, so only the filter needs relocating. With the shim the poller ran normally and
reached `GATES_PASSED`.

## Proposed fix

1. **Stop passing the filter as an argument** (~20 min). Fetch the raw GraphQL response
   with `gh api graphql` (no `--jq`) and pipe it through `jq -r -f <file>`, reading the
   filter from a temp file written by the script. The filter is already assembled in a
   variable, so this is a call-site change, not a restructure. `jq -f` has no
   command-line-length exposure at all.
2. **Also move the GraphQL body off the command line** — `gh api graphql -F query=@file`
   accepts a file reference, removing the second contributor and leaving headroom as both
   payloads keep growing.
3. **Do not classify a local exec failure as an API outage.** Match `Argument list too
   long` / `E2BIG` on the `gh` invocation and return the usage/dependency code (2) with
   the real reason, rather than folding it into `GH_API_DOWN` (3). An operator who is told
   "GitHub API is down" has no path to the actual fix.
4. **Regression guard**: a bats case in `tests/bats/merge_gates.bats` asserting the
   assembled filter is never interpolated into an argv position — e.g. that the `gh api
   graphql` call site carries no `--jq`.

## Why it matters

The gate poller is the enforcement point for AGENTS.md § Merge gates — the thing that
stands between an agent and a merge past a red check. On Windows it does not run at all,
and it fails in the one way that discourages investigation: an outage verdict on a
healthy API, which invites either waiting or reaching for the admin-merge carve-out. A
gate that is locally unrunnable on the primary dev platform is a gate that gets routed
around. See also [`2026-08-18-cr-out-of-band-label-inert-until-gate-rerun.md`](applied.md)
for the other half of the same session's un-wedging cost.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — merge-gates.sh writes the gate filter to a temp file and pipes the raw gh api graphql response through standalone jq -r -f whenever jq is on PATH (gh-failure vs jq-failure classification kept via PIPESTATUS; C2 + GH_ARGV_TOO_LONG arms unchanged); the jq-less gh --jq fallback stays under the argv budget bats case.
  Status: applied (2026-10-04)

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
   [`merge-gates.sh:1414`](../../../agent-layer/agents/scripts/core/merge-gates.sh) says so outright:
   *"(ci_fail / ci_pend) … are NOT touched"*. But `GATES_PASSED` requires `ci_pend -eq 0`
   (lines 1502 and 1557), and `CR findings (0 actionable)` is an ordinary StatusContext that
   lands in `ci_pend` (`fields[7]`, line 736) like any other check.
2. **The CR finding gate action** ([`.github/actions/cr-finding-gate/action.yml:249-254`](../../../.github/actions/cr-finding-gate/action.yml))
   *does* honour the label — it fetches labels live and posts `success` with
   `cr-out-of-band label set — gate overridden`. That branch is the thing that clears
   `ci_pend`.

The gap: **nothing re-triggers the workflow when the label is applied.**
[`cr-finding-gate.yml`](../../../.github/workflows/cr-finding-gate.yml) triggers on
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

## Resolution — 2026-09-09

**Status: applied.** All three proposed fixes shipped together with the OSS
manual-trigger playbook:

1. `cr-finding-gate.yml` now triggers on `labeled` / `unlabeled`.
2. `merge-gates.d/10-gate-filter.sh` discounts `CR findings*` from `ci_pend` /
   `ci_fail` when `cr-out-of-band` + `cr-disposition` are present.
3. `scripts/dev/trigger-coderabbit-review.sh` + `merge-gates.md` § CodeRabbit OSS
   manual-trigger name the human-trigger / waive moves (bot nudges retired for
   the never-reviewed arm; terminal failure instead of unbounded pending).
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — cr-finding-gate.yml re-runs on labeled/unlabeled, 10-gate-filter.sh discounts the CR context under cr-out-of-band + disposition, and the playbook is in merge-gates.md (entry's own 2026-09-09 resolution).
  Status: applied (2026-10-04)

# Merge-snapshot ledger has a 28-PR hole: no row written between #2067 and #2111

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-18
- **Found during**: appending the mandatory merge-time snapshot after
  [PR #2111](https://github.com/alexandrosk0/Smatchet/pull/2111) merged

## Symptom

`docs/self-improvement/merge-snapshots.jsonl` holds 147 rows. The last row before this
one is `#2067`; the next is `#2111`. Every PR merged in between has **no row**:

```
$ gh pr list --state merged --limit 60 --json number \
    -q '[.[] | select(.number > 2067 and .number < 2111)] | length'
28
```

All 28 (#2068 … #2108) merged between 2026-08-16T21:33Z and 2026-08-18T12:10Z, all with
`mergedBy = alexandrosk0`. Two full days of ship-loop activity left no gate-verdict
capture at all.

## Cause

Not yet diagnosed — this entry records the gap, not the fix. What is already known:

[`ship-loops.md:44`](../../../agent-layer/docs/agent-rules/ship-loops.md) names four writers, and
`ship-loops.md:57` adds two that are code rather than rule (merge-pipeline-02):
`safe-admin-merge.sh` appends its own row, and `git-janitor.sh --post-merge` Step 5.5
backfills a row for a merge **no** actor recorded (verdict `BACKFILLED`, actor
`git-janitor`). That backfill is the designed net for exactly this case — a human/UI
merge — so the hole means either the in-session actor never reached its append **and**
`--post-merge` never ran, or it ran outside its age cap.

The cap is what makes this unrecoverable: Step 5.5 is bounded by
`SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS` (default 6 h, undatable fails closed to skip),
and `ship-loops.md:55` prohibits retro-composing older rows outright — *"do not
retro-compose rows hours later from a possibly re-run rollup, a stale line is worse than
a hole."* Every one of the 28 is now past the cap. The data is gone, by design; only the
recurrence is fixable.

## Why it matters

The ledger exists because the merge-decision instant is the **only** lossless capture —
GitHub overwrites `statusCheckRollup` contexts on re-run and strips override labels
post-merge ([ADR-0017](../../adr/0017-merge-time-snapshot-ledger.md)). A hole is not a
cosmetic gap in a log: it is the permanent loss of the evidence that a given merge was
gated, over precisely the window in which 10 `*-out-of-band` labels were left behind on
merged PRs (surfaced by the same closeout's `issue-sweep.sh` dry-run: #2105, #2100, #2096,
#2088, #2075, #2074, #2071, #2070, #2069 `cr-out-of-band`; #2097
`plan-lock-out-of-band`). Those are the merges whose override rationale most needed
capturing, and they are the merges with no row.

It is filed **P1** rather than P2 because the failure is silent and the loss is
irreversible: nothing warned that 28 consecutive merges skipped a step documented as
*mandatory*, and by the time the gap is visible in the file, the age cap has already
closed the only sanctioned repair. `postmortem-owed.sh` keeps a live `statusCheckRollup`
fallback for un-snapshotted merges, but that fallback reads current state — the exact
thing the ledger exists because it cannot trust.

## Proposed fix

Two independent gaps, either of which alone would have prevented this:

1. **Detect the hole while it is still repairable.** A SessionStart or closeout check
   comparing merged-PR numbers against ledger rows over the last N hours, nagging while
   the merges are inside the Step 5.5 age cap. The nag has to fire on the *gap*, not on
   any single session's behaviour — no session that skipped its own append is going to
   notice it skipped.
2. **Close the actor-taxonomy gap this closeout hit.** The valid `mergeActor` tokens are
   `orchestrator`, `git-janitor`, `merge-watcher`, `safe-admin-merge`,
   `orchestrator-automerge`. None describes what happened here: an in-session orchestrator
   polled the gates to `GATES_PASSED` and the **user** then merged through the GitHub UI.
   The row was written with `orchestrator` (accurate as the *writer* — that is the sense
   the `git-janitor`/`BACKFILLED` case already uses, where the actor did not merge either),
   but a reader cannot distinguish "the orchestrator merged" from "the orchestrator
   verified and a human merged". A distinct token would make that legible instead of
   leaving it to the reader.

Both are small; the value is that (1) turns a permanent hole into a recoverable one.

## Note on scope

The 28 missing rows were deliberately **not** backfilled — that is the retro-compose
prohibition above, and a fabricated row is worse than the hole it fills.

## Recurrence (2026-10-04)

The hole reopened right after the #2212 row and is now larger than the one recorded above. On `develop` at `d8b4371e`, `merge-snapshots.jsonl` ends at #2212 (merged 2026-09-12). The 49 develop merges after it (#2213 … #2294) have no row, apart from #2290 and #2294. This note's PR lands those two rows; the in-session orchestrator wrote each one right after its merge call. The other 47 are permanent holes: all are past the janitor's 6 h repair window, and the retro-compose prohibition covers them.

Among the 47:
- #2213, the PR that landed #2212's row. That is the regress `categories/tooling/2026-08-19-safe-merge-arms-automerge-and-execs-away-before-writing-a-snapshot-row.md` predicts.
- #2262, #2272, #2273, #2277, #2280 and #2289, which one in-session orchestrator merged over REST without running the append.
- #2286, which native auto-merge took past a red bucket-E lane, and #2280 and #2213, which merged under `plan-lock-out-of-band`. Their postmortems in `postmortems.md` had to rebuild each merge instant from the PR timeline because no row existed.

Neither proposed fix has landed. No check reports the gap, and the default merge paths still do not write the row in code. Nothing flagged 47 consecutive skipped writes; this recurrence was found by hand while drafting the #2286 postmortem.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — (1) agents/scripts/core/merge-snapshot-holes.sh (--list/--nudge/--selftest, REST, bounded pages, SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS window; 'window NOT scanned' on fetch failure) wired into SessionStart for Claude Code and Codex; (2) the 'user' mergeActor token (orchestrator verified, human merged) is canonical in merge-snapshot-append.sh's header and ship-loops.md. Root cause closed by the safe-merge snapshot fix.
  Status: applied (2026-10-04)

# A single-source Font Awesome fetch red-walls five required CI jobs on an upstream 404, and `--retry-all-errors` burns 5 minutes retrying a permanent failure

- **Category**: infra
- **Priority**: P1
- **Date**: 2026-08-18
- **Found during**: un-wedging [PR #2070](https://github.com/alexandrosk0/Smatchet/pull/2070) — its `Windows + MSVC` attempt 1 failure

## Symptom

`Windows + MSVC` — a **required** check — went red on PR #2070 with a failure that had
nothing to do with the diff. Step 11, *Fetch Font Awesome 6 Solid TTF (pinned +
checksum-verified)*:

```
curl: (22) The requested URL returned error: 404
```

repeated **eleven times**, then `##[error]Process completed with exit code 22`. A bare
re-run at 22:37Z cleared the same step and the job went green at 23:05:21Z — the URL was
transiently serving 404. The diff was never at fault.

The red counted against the PR for hours. Under block-on-any-red it also blocks every other
open PR that inherits the check, and this fetch runs in **five** jobs
([`build-and-test.yml`](../../../.github/workflows/build-and-test.yml) lines 338, 436,
519, 711, 831), so one upstream blip reds the board.

## Cause

[`.github/actions/fetch-fontawesome/action.yml`](../../../.github/actions/fetch-fontawesome/action.yml)
fetches from exactly one origin with no mirror and no cache:

```bash
curl -fsSL --retry 10 --retry-delay 30 --retry-max-time 600 --retry-all-errors \
     "https://github.com/FortAwesome/Font-Awesome/raw/${FA_TAG}/webfonts/fa-solid-900.ttf" \
     -o assets/fonts/fa-solid-900.ttf
```

Two compounding problems:

1. **No fallback source.** `github.com/.../raw/` availability is a hard build dependency of
   five required jobs. The pin itself is right (immutable tag + sha256, per the action's own
   comment) — the pin is what makes a mirror *safe*, since any source that hashes to
   `af19d135…` is byte-identical by construction.
2. **`--retry-all-errors` treats 404 as retryable.** It was added for a real reason (the
   comment records a ~2h 429 rate-limit on this URL on 2026-07-09), but 429/5xx are
   transient and 404 is not — `--retry 10 --retry-delay 30` spent ~5 minutes of runner time
   re-asking a question already answered, then failed anyway. The eleven identical lines in
   the log are that loop.

## Proposed fix

1. **Add a mirror, try in order** (~1h). jsDelivr (`cdn.jsdelivr.net/gh/FortAwesome/Font-Awesome@${FA_TAG}/webfonts/fa-solid-900.ttf`)
   serves the same tag from different infrastructure. Loop over an ordered source list,
   accept the first that passes `sha256sum -c -`, fail only when all are exhausted. The
   checksum is the trust anchor, so adding sources adds availability without adding trust
   surface.
2. **Cache the font by `FA_SHA256`.** `actions/cache` keyed on the hash makes the steady
   state a cache hit and takes upstream off the hot path entirely for the other four jobs
   once one has fetched it.
3. **Stop retrying 404.** Drop `--retry-all-errors` and keep the default retryable set
   (transient + 429), or gate it behind an explicit status check. Preserves the 2026-07-09
   fix while failing fast on a permanent error — and a fast failure is what makes the
   mirror attempt cheap.

## Why it matters

A required gate that reds on third-party CDN weather is a gate whose reds stop being read.
The correct reflex here was "re-run it" — indistinguishable, at a glance, from the reflex
that lets a real red through, and the repo has already paid for that confusion once
([`postmortems.md`](../postmortems.md), #1957 class). The fix is cheap and the pin
already did the hard part: with a checksum this strict, a second source costs nothing in
supply-chain risk and removes a single point of failure from five required jobs.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — the font is committed, so the fetch-fontawesome action now only verifies assets/fonts/fa-solid-900.ttf against the pinned FA_SHA256 (scripts/dev/verify-fontawesome.sh, fails closed with a named ::error); no network, no single source; all six call sites unchanged.
  Status: applied (2026-10-04)

- 2026-08-17 · orchestrator · [infra] · P2 — the merge-gate poller collapses duplicate check-runs by name; GitHub does not, so a concurrency-cancelled twin reports GATES_PASSED and then 405s the merge
  Details: Hit on PR #2071 (head `90504efe0875`), a docs-only diff where every check
    was green or skipped. Two workflow runs had published a check-run under the SAME
    name `Perf PR-fast (windows-2022)`: run 31981596731 produced `skipped` at
    00:43:19 (correct — the perf lane skips on a docs-only diff), and run
    31981601014 had a `cancelled` one at 00:29:03, killed by its concurrency group
    before it could resolve.
    The poller collapses that pair. `agents/scripts/core/merge-gates.d/10-gate-filter.sh:82`
    keys the rollup contexts by `["CheckRun", name]` then
    `group_by(._k) | map(sort_by(.startedAt // "") | .[-1])`, so the 00:43:19
    `skipped` wins and the 00:29:03 `cancelled` is discarded before any conclusion
    is examined. GitHub's required-status-check evaluation applies no such collapse:
    `PUT /repos/alexandrosk0/Smatchet/pulls/2071/merge` returned
    **`405 Required status check "Perf PR-fast (windows-2022)" is cancelled`**.
    Note the intent is already aligned — `:102` lists `CANCELLED` among the
    conclusions that block. ONLY the latest-per-name dedup diverges, and it diverges
    in the dangerous direction: the poller says green, the merge is impossible, and
    there is no red check anywhere for an operator or an autonomous loop to point at.
    Recovery (verified): `rerun_workflow_run` on the run that owns the stale
    check-run — here 31981601014. No push, no force, no PR-body re-pin. The 405 text
    transitions `is cancelled` -> `is expected` (the stale check is invalidated and
    GitHub now awaits a fresh one), then the re-run's job 95255190961 reported
    `skipped` at 01:03:26 and the merge succeeded as `ae6892c0`.
    Cost this time: two rejected merge calls and ~35 min wall-clock on a docs-only
    PR. The exposure is not rare — concurrency-cancelled twins are produced by the
    repo's ordinary flow, every time a PR-body edit or a quick second push supersedes
    an in-flight run. The `Intent section` body-repin dance manufactures exactly this
    shape, so any PR that needs a verdict-line update can inherit it.
  Concrete next action: (a) **Align the collapse with GitHub** — in `10-gate-filter.sh:82`,
    do not let a newer same-named context mask an older one whose conclusion is in the
    blocking set; treat the name as blocking if ANY of its contexts is
    FAILURE/TIMED_OUT/CANCELLED/ACTION_REQUIRED/STARTUP_FAILURE. That trades a false
    "green" for a false "wait", which is the correct direction — a false wait is
    visible and self-clearing, a false green wedges the loop with nothing to point at.
    (b) Cheaper interim, and worth doing regardless: emit a WARNING naming the
    divergence when one check NAME carries >1 context with differing conclusions, so
    the reason for the coming 405 is on screen before the merge is attempted.
    (c) Document the recovery in `docs/agent-rules/merge-gates.md` — the rerun-the-owning-run
    fix is cheap but completely non-obvious from the 405 text, and nothing in the repo
    currently describes this failure shape.
    Add a `tests/bats/merge_gates.bats` case pinning it: two contexts, same name,
    elder CANCELLED + newer SKIPPED, asserting the gate does NOT report passed.
    Prefer (c)+(b) immediately (docs + one log line), (a) as the real fix.
  Update (a) SHIPPED 2026-08-17 — but NOT as proposed above, because the proposal
    was wrong. "Treat the name as blocking if ANY of its contexts is FAILURE/…"
    would have regressed the case the dedup exists for: `merge-gates.graphql:59-62`
    records that a job rerun leaves BOTH the old FAILURE and the new SUCCESS on the
    head, so an any-blocks rule wedges every PR ever fixed by a rerun. It would also
    have over-blocked PR #2091, where two elder runs were cancelled by concurrency,
    the newest succeeded, and GitHub merged on the first attempt.
    The two cases are indistinguishable in the data the poller fetched, which is the
    real defect: `startedAt` cannot tell "same job, rerun" from "different run,
    cancelled by concurrency". Three observations pin the actual rule — GitHub reads
    the newest WORKFLOW RUN for a name, the poller read the newest `startedAt`, and
    those diverge only when a newer run is cancelled before an older run finishes.
    Fix: query `checkSuite { createdAt }` and sort by `[suite createdAt, startedAt]`.
    A rerun stays in one check suite, so it ties on the first key and still resolves
    by `startedAt` (rerun-to-green preserved); different runs order by suite age,
    which tracks the newest run (matches GitHub). Contexts with no `checkSuite` tie
    at "" and behave exactly as before.
    The first attempt used `workflowRun.databaseId` and was WRONG — caught by
    CodeRabbit on PR #2107 before merge. GitHub types `databaseId` as GraphQL `Int`,
    i.e. signed 32-bit (max 2147483647), and live run ids are ~1.5e10 — about 15x
    past that ceiling. The fixture in the very test pinning this bug carried
    31981601014. The server cannot serialise it, so the query errors, the poll
    retries, and the gate returns GH_API_DOWN: the fix for a false-green would have
    become a hard block on every merge. A DateTime carries the same ordering with no
    integer, and is strictly more robust — if a rerun ever DID mint a fresh suite,
    the newer SUCCESS still wins, so rerun-to-green holds under either reading.
    Four `tests/bats/merge_gates.bats` cases pin it: rerun-same-suite, the #2071
    elder-cancelled shape, the #2091 newest-success shape, and the no-checkSuite
    fallback. All 213 merge_gates cases plus 359 across the seven sibling suites that
    source merge-gates pass unchanged — existing fixtures carry no `checkSuite`, so
    they tie at "" and keep their old ordering.
    Still NOT verified: the field path could not be executed against the live schema
    (this session serves only pinned PR-review operations; docs.github.com is
    egress-blocked), and no CI lane runs the real query. If it is wrong the query
    errors, which returns GH_API_DOWN — a terminal notifying state that blocks rather
    than merges, so it fails safe and loudly. Watch the first real poller run. Note
    this residual risk is exactly what bit the databaseId attempt, and what an
    external reviewer caught that local tests could not: every bats fixture is
    synthetic, so the suite happily passed 213 cases against a query the GitHub
    server would have rejected.
  Update (b)+(c) SHIPPED 2026-08-18. (b) the gate now emits a WARN naming any check
    whose duplicate collapse discarded a BLOCKING context from a DIFFERENT check
    suite — two new projection fields (35 dupMaskedNames / 36 dupMaskedCount, the
    names-then-numeric-count tail shape the stale-override pair already used; the
    field-count assertion and its fail-closed canary moved 35 -> 37). Scoped
    CROSS-SUITE deliberately: the ledger wording ("any name with >1 context and
    differing conclusions") would have fired on every rerun-to-green, since a rerun
    leaves old-FAILURE + new-SUCCESS in one suite — a warning on the normal healthy
    path is noise, not signal. Winner-blocking cases are excluded too: the gate
    already blocks there, so a warning adds nothing.
    (c) `docs/agent-rules/merge-gates.md` § Duplicate check-name divergence documents
    both keys and the recovery, including the 405 message progression
    (`is cancelled` -> `is expected` -> mergeable) and the two things NOT to do:
    re-run the newer run, or re-run a body-dependent job like `Intent section`
    (a re-run replays the original event payload, so it re-reads the stale body).
    Source comments in merge-gates.graphql / 10-gate-filter.sh now cite that section
    rather than this entry, so archiving this file cannot strand them.
    Verification: 215 merge_gates cases (2 new for the WARN: fires cross-suite,
    silent on same-suite rerun) + 359 across the seven sibling suites, 0 failures;
    shellcheck introduces no new codes; markdown-link / plan-ref / backlog-count
    gates pass.
  Status: applied (2026-08-18 — (a) suite-aware dedup key, (b) divergence WARN,
    (c) merge-gates.md recovery section; all with test coverage)
  Last-reviewed: 2026-08-18

# pre-push (B) is refspec-blind: it refuses `refs/locks/*` deletes from a merged branch

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-17
- **Found during**: releasing the plan-lock after [PR #2097](https://github.com/alexandrosk0/Smatchet/pull/2097) merged

## Symptom

The documented release path, run from the worktree of the branch whose PR had just
merged:

```bash
bash agents/scripts/core/lock-release.sh github-issue-body-empty-line
```

refused with the merged-PR banner:

```
pre-push: REFUSING push.
  branch:    claude/github-issue-body-empty-line-9aa2f2
  PR #2097 state: MERGED
Pushing to a MERGED PR branch silently lands commits the PR will never pick up.
```

Nothing was being pushed to the PR branch. `lock-release.sh:75` pushes a **delete**:
`git push "$remote" ":$ref"` where `$ref` is `refs/locks/<slug>`. Cleared with
`SMATCHET_ALLOW_MERGED_PR_PUSH=1` — an override the hook's own header labels *"rare,
usually wrong"*, on the one path the ship-loop is supposed to take every time.

## Cause

Stage (B) of [`scripts/git-hooks/pre-push`](../../../scripts/git-hooks/pre-push)
never looks at what is being pushed. It keys entirely on the checkout:

- `:170` — `branch=$(git rev-parse --abbrev-ref HEAD)`
- `:366` — `gh pr view "$branch" --json state`; `exit 0` only when empty or `OPEN`
- `:373-396` — otherwise print the banner and `exit 1`

The `push_updates` snapshot taken at `:62` — which carries `<local_ref> <local_sha>
<remote_ref> <remote_sha>` for every update — is read by stage (A) and stage (E) but
not by (B). Both of those stages already recognise a delete and skip it: (A) at `:72`
(`[ "$local_sha" = "$zero_sha" ] && continue   # a branch DELETE — not a content push`),
(E) by exempting deletes per its header at `:36`. (B) is the only stage that judges the
push without reading it, so *every* refspec inherits the merged-PR refusal — lock
deletes, tag pushes, any sibling ref — on the sole basis of which branch happens to be
checked out.

The guard's own justification does not extend to these: the banner's premise is
"commits the PR will never pick up", and a `refs/locks/*` delete carries no commits and
touches no branch ref.

## Proposed fix

Give (B) the same delete/ref awareness (A) and (E) already have: iterate `push_updates`
and only refuse when at least one update targets `refs/heads/*` with a non-zero
`local_sha`. That is a handful of lines and it preserves the guard's entire purpose (the
orphaned-commit case) while removing the false refusal for lock deletes and other refs.

One trap in the test harness, worth naming because it decides the default: the existing
bucket-A harness
[`agents/scripts/core/test-pre-push-merged-pr-guard.sh`](../../../agent-layer/agents/scripts/core/test-pre-push-merged-pr-guard.sh)
(9 cases) runs the hook with **empty stdin** — its own comments note (A) "is stdin-driven
and inert on the empty stdin here". A naive stdin-keyed (B) would therefore see zero
updates and allow, flipping the harness's MERGED/CLOSED refusal cases (6, 7) green for
the wrong reason. So: keep empty/unparseable stdin on the **refuse** side (fail-closed,
matching today's behaviour), teach `run_hook` to pipe ref-update records, and add two
cases — a `refs/locks/<slug>` delete from a MERGED-PR checkout is allowed, a
`refs/heads/<branch>` content push from the same checkout still refuses.

## Why it matters

The stale-lock class this compounds is filed separately
([`2026-08-17-pr-body-rewrite-drops-lock-slug-marker.md`](tooling/2026-08-17-pr-body-rewrite-drops-lock-slug-marker.md)),
and the two chain: the automated release silently no-ops, and then the documented manual
recovery is blocked by a hook that tells the operator they are almost certainly doing
something wrong. The cost is not the extra env var — it is that reaching for
`SMATCHET_ALLOW_MERGED_PR_PUSH=1` on a routine, correct operation is exactly how an
override stops meaning anything.

## Recurrence

- **2026-08-18** — same refusal releasing the `fix-four-open-issues` lock after
  [PR #2111](https://github.com/alexandrosk0/Smatchet/pull/2111) merged; cleared the same
  way (`SMATCHET_ALLOW_MERGED_PR_PUSH=1 bash agents/scripts/core/lock-release.sh
  fix-four-open-issues` → `refs/locks/fix-four-open-issues deleted`). Second occurrence in
  two days, both on the routine post-merge release path — the override is now the *normal*
  way to run `lock-release.sh`, which is the failure mode this entry predicted. Priority
  unchanged at P2 (loud refusal with a documented escape, not a silent failure); the
  frequency is the argument for scheduling the § Proposed fix rather than for a bump.

## Status

Applied (2026-08-19 — [`docs/plans/shipped/pre-push-refspec-scope.md`](../../plans/shipped/pre-push-refspec-scope.md), [#2131](https://github.com/alexandrosk0/Smatchet/pull/2131) squash-merged `56f5be77`).
Stage (B) now iterates the `push_updates` snapshot before the `gh pr view` lookup and
`exit 0`s unless at least one update targets `refs/heads/*` with a non-zero `local_sha`,
mirroring (A)'s delete-skip idiom and reusing its `zero_sha` constant. Empty or
unparseable stdin is treated as a branch push (fail-closed), exactly as this entry's
§ Proposed fix prescribed — that is what keeps the harness's MERGED/CLOSED cases
honest rather than accidentally green.

`agents/scripts/core/test-pre-push-merged-pr-guard.sh` gained an optional stdin-records
parameter (defaulting to an ordinary content push of the branch under test, so all nine
pre-existing cases keep asserting what they asserted) and four new cases: a
`refs/locks/<slug>` delete from a MERGED-PR checkout is allowed **and prints no banner**
(10a/10b), a `refs/heads/<branch>` content push from the same checkout still refuses
(11a/11b), empty stdin still refuses (12), and a post-merge `refs/heads/<branch>` delete
is allowed (13). Confirmed the new cases fail against the pre-fix hook (10a/10b/13 red,
11/12 green) so they are a real regression guard, not a tautology. 18/18 green after;
`test-pre-push-stage-neutralisers.sh --check` passes (no new escape variable was added)
and all 20 `tests/bats/pre_push_guard.bats` cases stay green.

`lock-release.sh` now runs clean from a just-merged checkout, so
`SMATCHET_ALLOW_MERGED_PR_PUSH=1` goes back to meaning what its own header says it
means.

# `StubAiClient: cancel mid-stream` asserts a 100 ms wall-clock budget and flakes on CI

- **Category**: test
- **Priority**: P2
- **Date**: 2026-08-17
- **Observed on**: PR #2090, `Windows + MSVC` job 95255794038 (head `ea80edbaac09`)
- **Status**: open

## What happened

`Windows + MSVC` failed — a **required** check — on a PR whose only C++ change was comment
deletions proven token-identical to the merge base. The build was clean (776 TUs, `Compilation
failures 0`); the failure was in step 10, `Run ctest (MSVC)`, and it cascaded: every later step
(including `Build SmatchetStandalone`) was skipped, so the job also reported
`No files were found with the provided path: build/ninja-iter-msvc/Smatchet.exe`, which reads like
a link failure and is not one.

The suite ran **2904 test cases / 38380 assertions with exactly one failure**:

```
tests\Core\StubAiClientCancel.test.cpp(33):
TEST CASE:  StubAiClient: cancel mid-stream stops onDelta within 100 ms

tests\Core\StubAiClientCancel.test.cpp(98): ERROR: CHECK_FALSE( stub.CancelBudgetExceeded ) is NOT correct!
  values: CHECK_FALSE( true )
```

The assertion is a **wall-clock latency budget**, not a logic check: the cancel did land, it just
took longer than its budget on a contended runner. The file is explicitly timing-conditional around
that line — `CHECK(postCancelMs < 200)` / `CHECK(totalMs < 400)`, with a `3200` ms variant under a
slower-config guard — so the budget already needed platform-specific tuning once.

## Why it is a flake, not a regression

Three independent lines of evidence, all on the same commit:

1. **`Coverage (windows-2022 + OpenCppCoverage)` passed on the identical head** — it builds and runs
   the same doctest suite on the same OS. Same commit, same tests, one pass and one fail is the
   definition of non-deterministic.
2. The failing file, `tests/Core/StubAiClientCancel.test.cpp`, is **not in the PR diff at all**, and
   every C++ file that *is* in the diff was verified token-identical to the merge base via
   `dup_audit.normalized_stream()` — comment-only edits cannot change runtime behaviour.
3. `Mobile — POSIX core compile gate` (compiles all `CORE_SOURCES`, runs the builtin-command
   dispatch harness and the command-registry monkey) passed on the same head.

Note the run took 34.4 s for a suite that is normally much faster, and the job had a 0.13 % sccache
hit rate — i.e. this runner was doing a full cold compile of 776 TUs concurrently with the test run.
A 100 ms budget is not robust under that load.

**Confirmed 2026-08-17 02:24Z.** The next push to the same branch re-ran `Windows + MSVC` on head
`624a579`, whose C++ content is token-identical to `ea80edb`, and `Run ctest (MSVC)` passed cleanly
in 33 s. One failure and one pass on the same code is direct evidence rather than the circumstantial
argument above, and it rules out a deterministic regression. No re-run was requested to get it — the
push re-triggered the job on its own.

## Why it matters

`Windows + MSVC` is a branch-protection **required** check, so this flake blocks merges at random
and lands on whoever pushed. It also mis-presents itself: because ctest failure skips the exe build,
the surface symptom is a missing `Smatchet.exe`, which sends the reader looking for a link error.
The repo already has a quarantine mechanism for exactly this class —
`tests/Core/FlakySelfTest.test.cpp` carries a `[quarantined:test-rig]` tag — so the pattern exists
and is unused here.

## Concrete next action

Make the assertion robust rather than deleting it — the behaviour under test (cancel actually stops
`onDelta`) is worth keeping; only the *timing* half is unstable.

1. Split the assertion: keep a hard `CHECK` that `onDelta` stops after cancel (a correctness claim,
   deterministic), and downgrade the 100 ms budget to a `WARN`, or raise it to a value that reflects
   a loaded CI runner (the observed suite runtime suggests seconds, not 100 ms, of scheduling jitter
   is possible).
2. If the budget is meant as a perf guard rather than a unit assertion, move it to the perf-gate
   harness (`scripts/dev/perf-*`), which is built to tolerate machine variance, and tag the unit
   test `[quarantined:test-rig]` in the interim.

Enumerator for the sweep: `grep -rn "Budget\|_ms\b\|std::chrono" tests/Core/*.test.cpp` names every
unit test carrying a wall-clock assertion; `StubAiClientCancel.test.cpp:98` is the row that
motivated this entry, and any sibling with the same shape has the same exposure.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-17;n=20; action=re-check whether StubAiClientCancel or another wall-clock unit assertion has failed a required check again; baseline=1 observed flake and 1 observed pass on token-identical code, 2026-08-17 (PR #2090); fired=2026-10-06
  Resolution: applied 2026-10-06 (backlog-sweep-2026-10, PR #2296) — StubAiClientCancel.test.cpp's three wall-clock checks are advisory (WARN_LT / WARN_FALSE) while its behaviour checks stay hard (plus two new ones: every emitted delta reached onDelta; no final delta after cancel), and the file joined the Linux subset. 20/20 passes under full CPU saturation and with the test pinned to one busy CPU. Not swept: the sibling wall-clock CHECKs in BulkImportAbandonNonBlocking, CallstackParser, CancelToken, SubprocessCapture and UserInfoActivityCancelUaf tests (files under other sessions' locks at the time); the enumerator above still lists them.
  Status: applied (2026-10-06)

- 2026-08-16 · orchestrator · [infra] · P2 — four docs still describe the pre-all-gates-blocking world, so a reader planning CI-gating work inherits a blocker that no longer exists
  Details: the all-gates-blocking flip set `MERGE_GATES_BLOCK_ALLOWLIST_RE="."` (`agents/scripts/core/merge-gates.d/00-common.sh:42`) — every check-run blocks unless its NAME contains `advisory` — and neither bucket-C nor bucket-E carries a job-level `continue-on-error` any more (only the two documented step-level masks). Four places still said otherwise: (1) `testing-surface-roadmap.md:275-282` — Slice B's stated blocker false in all three clauses (Mesa exe boots; blanket `continue-on-error` gone; the allow-list itself retired); (2) `docs/guides/testing-surface.md` + (3) `infra.md:14` — "fully advisory to the merge-gate poller"; (4) refs to `infra.md` `bucket-mesa-exe-boot` pointing at the wrong file (entry moved here; premise falsified 2026-06-18 — exe boots ~2 s under llvmpipe, the ~26 s exit was the since-fixed `--spawn` exit-code bug).
  Resolution: applied 2026-08-29 (branch claude/testing-surface-activation-904b38) — (1) roadmap Slice-B deviation rewritten to "unblocked and mostly superseded" naming the two real residuals (fix/skip ~3/74 render-dependent bucket-E tests; CI-native goldens so the bucket-C golden-diff mask can go) + progress header refreshed; (2) `testing-surface.md` §1/§2/§3/§4/§5/§5.1/§6 corrected to lane-blocks/step-mask reality (also refreshed the bucket-E count 26→58 TUs and marked the Slice-C required-context P0 item done); (3) `infra.md` entry got an "Update (c) SUPERSEDED 2026-08-29" line (the obsolete "re-add `Bucket-|`" graduation step retired); (4) live-doc `bucket-mesa-exe-boot` refs now point at `applied.md` (postmortems left as historical record). The suggested prose-vs-shell-constant drift gate was NOT built (open idea, not re-filed — revisit if the class recurs).
  Status: applied
  Last-reviewed: 2026-08-29

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
    [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md) § Cadence and
    verification — *when a review finding reports a stale/incorrect STATEMENT
    (comment, doc line, header claim), fix the class, not the instance: grep the
    offending phrase across every file in the PR diff (`git diff --name-only
    <base>...HEAD`) before replying, and state in the reply that the sweep was
    diff-wide.* Cheap and mechanical; it generalises the existing
    fabricated-quote rule from "quotes" to "any statement a reviewer proves
    wrong". Optional follow-on if it recurs: a `pre-ship.sh` helper that takes a
    phrase and greps it across the diff's files, so the sweep is one command.
  Status: applied (flipped at archival)
  Last-reviewed: 2026-08-16

- 2026-08-16 · orchestrator · [tooling] · P1 — the `CR findings` gate treats CodeRabbit's `Review skipped: manual review required for this OSS repository` status as "CR reviewed and found nothing", so on this repo it goes GREEN on an entirely unreviewed head — and that is the DEFAULT state of every new PR, not an edge case
  Details: [`cr-finding-gate/action.yml`](../../../.github/actions/cr-finding-gate/action.yml)
    disambiguates a head with no CR review node via CR's own `CodeRabbit`
    StatusContext. It already special-cases ONE not-a-review description —
    `grep -qiE 'rate.?limit|limit reached'` — and correctly resolves that to
    PENDING plus a full-review nudge. Everything else falls through to
    `SUCCESS) post success "CodeRabbit completed with no review on head
    (skipped/clean)"; exit 0`. The in-file comment states the intent: *SUCCESS ->
    CR is done and skipped the review (trivial / workflow / docs change)*.
    But `Review skipped: manual review required for this OSS repository` does
    NOT mean that. It means the opposite: CR has **not** looked and is waiting to
    be asked. CodeRabbit requires a manual `@coderabbitai review` on repositories
    with fewer than 10 stars, so this status is posted on **every** PR here at
    creation time. The gate is therefore green-by-default on unreviewed code, and
    only turns honest if a real review later lands.
    Observed live on PR #2028: CR posted the skip status at 03:46:17, the gate
    posted `success` at 03:46:30, and the PR then sat for **11.5 hours** with
    `mergeable_state: clean`, all 36 CI checks green, and the CR gate green —
    with zero review having occurred. The only thing that stopped an unreviewed
    merge was the orchestrator manually applying the repo learning ("a skipped /
    rate-limited stamp is NOT review evidence"). A `smatchet-merge-watcher`
    registration, a `governance.auto_merge: on` grant, or any operator trusting
    the checks would have merged it. #2023 and #2025 showed the same green.
    This is the exact fail-open the rate-limit branch was added to close
    (its comment: *"the branch below would translate that into 'completed with no
    review on head (skipped/clean)' and pass an entirely unreviewed commit"*) —
    the same sentence describes this case verbatim, only with a different
    description string. The scoping decision ("an unrecognised description must
    keep its existing pass behaviour instead of hanging every PR") was a
    deliberate fail-open for UNKNOWN markers; `manual review required` is no
    longer unknown.
  Concrete next action: extend the not-a-review description match from
    `rate.?limit|limit reached` to also cover `manual review required` /
    `review skipped` (keeping the deliberate fail-open for genuinely unrecognised
    descriptions), so the head resolves to PENDING and `maybe_nudge_full_review`
    fires — which is already the right recovery and is proven to work (a manual
    `@coderabbitai review` on #2028 produced a clean review in ~3 min). Guard
    against the sibling risk the existing comment names: a docs-only PR whose
    files are all path-excluded must still pass, and that case is already handled
    up front by the `selfImpOnly` head-accurate file-list check, so widening this
    match does not re-wedge it. Add a `merge_gates`/`cr_finding_gate` bats case
    per description string (rate-limited, manual-review-required, genuinely
    unknown) so the vocabulary cannot silently regress — the sibling entry
    2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop records the same
    class of brittleness in the auto-nudge's own regexes. Est ~0.5d.
  Status: applied — the CI action now classifies `manual review required` as a
    not-a-review marker (PENDING, not a pass) and self-heals with a new
    `never-reviewed` nudge that posts a plain `@coderabbitai review`; the
    pre-existing nudge could not cover this state because it REQUIRED a prior
    clean pass to key on. Matched on `manual review required`, never a bare
    `review skipped`, so CR's terminal path-filter skip still passes. Correction
    to this entry's original framing: the CLIENT-side poller was never
    vulnerable — `merge-gates.d/10-gate-filter.sh` already excludes this string
    (plus `available on request` and the rate-limit texts) from
    `crReviewSkipped`, hardened on PR #2017. The gap was that the server-side
    gate never received the same treatment, despite its own header describing
    itself as lifting the client-side verdict server-side.
    Merge-history evidence added 2026-08-16 (PR #2038, after the fix landed): a
    sweep of all 1,416 merged PRs above #500 — each merged head SHA's status
    contexts plus CR's reviews/comments on that same SHA — shows this class had
    already merged **4 PRs with CR never having looked at all**: #2014, #2024,
    #2027, #2031 (2026-08-15 → 08-16), none carrying an override label, and
    #2024 is not docs (`fix(sync): stop multi-pane full syncs from deleting each
    other's cached tickets`, 19 code files). #2030 carries the same green status
    but did get a CR walkthrough on its head, so it is an instance of the status
    and not of the never-reviewed outcome. The #2028 near-miss in the original
    framing was the case that got caught; these four are the ones that did not.
  Last-reviewed: 2026-08-16

- 2026-08-16 · orchestrator · [infra] · P2 — a `send_later` check-in that fires while the remote container is suspended is silently lost, so an autonomous ship-loop can park a finished PR indefinitely with no alarm and no retry
  Details: The autonomous backlog loop drives each PR to merge via self-scheduled
    `send_later` check-ins. On PR #2028 the 04:23Z check-in — whose whole job was
    to post the `@coderabbitai review` trigger once the rolling-hour quota
    reopened — never ran: the trigger record shows `last_fired_at
    2026-08-16T04:24:03Z` with `ended_reason: run_once_fired`, so the scheduler
    considered it delivered, but the session was suspended and no work happened.
    The PR then sat **11.5 hours** at head `8c1f1646` with all CI green and no
    review requested. Nothing surfaced it: the fire-and-forget check-in is
    one-shot, so a lost firing is indistinguishable from a firing that ran and
    found nothing actionable (the loop deliberately re-arms *silently* in that
    case, which is correct behaviour and exactly what makes the failure
    invisible).
    Compounding: the CR gate was green the whole time for an unrelated reason
    (sibling entry 2026-08-16-cr-gate-greens-on-manual-review-required-skip), so
    every surface signal said "ready to merge". The two failures point the same
    way — toward an unreviewed merge — which is what makes the pair worth a gate
    rather than a note.
  Concrete next action: make loss detectable rather than trying to make delivery
    reliable (the scheduler is not ours to fix). Cheapest shape: have the check-in
    prompt stamp a heartbeat — e.g. append `<pr> <head> <iso8601>` to a
    session-local file on every firing — and have the SessionStart nudge compare
    the newest heartbeat against any OPEN PR authored by this session whose head
    is older than ~2h, raising `WARN: PR #<n> has had no check-in for <N>h` so a
    resumed session immediately re-arms instead of assuming the loop is alive.
    A cheaper stopgap that needs no new state: on SessionStart, list this
    account's open PRs on `claude/*` branches and re-poll each one's gates —
    a resumed session should never assume an in-flight PR is being watched.
    Related: infra/2026-08-05-merge-watcher-liveness-unmonitored covers the same
    "the watcher itself is unwatched" shape for the merge-watcher process.
  Status: applied — `agents/scripts/core/unwatched-pr-nudge.sh` (SessionStart,
    wired into the claude-code + codex hook templates) reports any OPEN
    non-draft PR on a `claude/`/`agent/` branch quiet longer than
    SMATCHET_UNWATCHED_PR_STALE_SECONDS (default 2h), and says to re-poll the
    gates before assuming anything still drives it. Took the STOPGAP shape from
    this entry, not the heartbeat: a heartbeat file would be written by the very
    process that dies, so a suspended container and a fresh checkout both look
    identical to "never armed" — asking GitHub what is open needs no cooperation
    from the thing that failed. Degrades silent with no gh / no auth / no
    network, and skips drafts (parked on purpose) so it does not train readers
    to ignore it. 14 bats cases + a fixture-driven --selftest, all
    negative-tested.
  Last-reviewed: 2026-08-16

# The auto-register hook silently grants auto-merge authorization to every PR the agent opens

- **Date**: 2026-08-16
- **Author**: orchestrator
- **Category**: process
- **Priority**: P1

## What

`gh pr create` auto-registers the new PR with `smatchet-merge-watcher`, and mere
registration is documented as *authorization to auto-merge*. The two facts compose
into a consent bypass: **every** agent-opened PR is auto-merge-authorized at creation
time, before the user has been asked anything.

The two halves:

1. **`docs/harness/claude-code/hooks/autoregister-pr.sh`** — a `PostToolUse(Bash)`
   hook wired at `docs/harness/claude-code/settings.json.tmpl:145` and `:161`, copied
   to `.claude/hooks/` by `setup-harness.sh claude-code`. On any Bash command
   containing `gh pr create` it greps `pull/<N>` out of the payload and runs
   `python agents/scripts/core/merge-watcher-cli.py register <N>`. Unconditional —
   no authorization check, no user prompt, no opt-in flag. Its header states the
   intent plainly: closing the "shipped a PR but forgot to register it" gap.

2. **`docs/agent-rules/merge-gates.md:84`** — "Auto-`gh pr ready` + merge apply only
   when the user has explicitly authorised this PR for merge (post-ship option 3
   'Register with watcher', in-session 'merge when green', **OR any PR registered
   with `smatchet-merge-watcher`**)."

That third clause is what converts the hook's bookkeeping into consent. Result: the
post-ship 4-option `AskUserQuestion` (AGENTS.md § Autonomous ship-loop) is decorative
with respect to auto-merge — picking option 1, 2, or 4 cannot withhold it, because
the hook already registered the PR minutes earlier.

**Observed live, 2026-08-16, PR #2027.** The user answered the post-ship question with
"Wait and report gates" and stated "No merge without your go-ahead." `merge-watch list`
nonetheless showed `{"pr": 2027, "registered_at": 1786850965, ...}`. Nothing merged
without consent only because the entry was parked at `stuck_reason: "REVIEW_REQUIRED"`,
`stuck_streak: 11` — an incidental branch-protection state, not the consent boundary.
Once every check went terminal-green and `mergeStateStatus` flipped to `CLEAN`, the
next daemon cycle had no remaining reason to hold. The orchestrator ran
`merge-watch unregister 2027` by hand to restore the user's stated intent.

**Reproduced by the PR that files this entry.** Opening the backlog PR fired the hook
again — `merge-watch list` showed `{"pr": 2031, "registered_at": 1786895461, ...}`
seconds after `gh pr create`, with no authorization asked for or given, and the user
had authorized no merge. Unregistered by hand a second time. Two for two: the bypass
is the default path, not an edge case.

## Why it matters

This inverts the autonomy model. [`AI_POLICY.md`](../../../AI_POLICY.md) makes agent
autonomy a **granted and revocable mode**, and AGENTS.md § Merge gates states
"**Auto-merge applies only when explicitly authorised**". Here authorization is
granted by a side effect of the agent's own tool call — the agent effectively
self-authorizes, and the user's answer to the one question that exists to gate it has
no mechanical effect.

It also contradicts the watcher's own shipped design decision,
[`smatchet-merge-watcher.md:102`](../../plans/shipped/smatchet-merge-watcher.md)
("Explicit owner transfer on register … Clean ownership boundary; no race between
orchestrator + watcher + user"). Ownership transfer was specified as an explicit user
act; the hook made it automatic and silent while the docs kept describing it as
explicit.

The failure is quiet by construction. Registration emits only a `systemMessage`
inside a hook result, so nothing in the post-ship summary tells the user that a
background daemon now holds merge rights on their PR. The user learns only if they
run `merge-watch list` — or after the merge lands.

Same family as the 2026-06-19 "Intent gate bypassed via non-poller merge" entry (since
applied — see [`applied.md`](applied.md)): enforcement that lives on the merge-actor
side is defeated by a path that never consults it. There the bypass was the merge
*mechanism*; here it is the merge *authorization*.

## Concrete next action

Split registration from authorization — keep the hook, drop its power. The hook's
observability value is real (gate-polling and the stuck-nudge caught wedged PRs
#2024 and #2027); the defect is that registration silently implies merge rights.

1. **Registry gains `authorized` (bool, default `false`).** Hook-driven registration
   writes `false`. Explicit user authorization — post-ship option 3, in-session
   "merge when green" — writes `true`.
2. **`merge-watcher.py` merges only on `authorized: true`.** An unauthorized entry
   still polls gates, still nudges on stuck/stale, still escalates — it just never
   calls the merge REST endpoint or `gh pr merge --auto`. Preserves everything the
   hook was written for.
3. **Add `merge-watch authorize <pr>` / `deauthorize <pr>`** verbs; make `register`
   take `--authorized` for the explicit-consent path. Have `authorize` print the
   ownership-transfer notice that `smatchet-merge-watcher.md:102` already specifies.
4. **Fix `docs/agent-rules/merge-gates.md:84`** — replace "OR any PR registered with
   `smatchet-merge-watcher`" with "OR a PR whose registry entry has
   `authorized: true`". As written today the clause is the actual bug in doc form.
5. **bats coverage** (sibling of `test-merge-watcher-bats.sh` /
   `test-merge-watcher-integration-bats.sh`): a hook-registered entry with **all gates
   PASS** must not be merged; `authorize` must flip it to mergeable; `register` +
   `--authorized` must be equivalent to `register` then `authorize`.
6. **Surface it in the post-ship summary.** If the PR is registered but unauthorized,
   say so in the ship report — a background daemon holding a PR should never be
   invisible state.

Est ~0.5d. Rejected alternative: delete `autoregister-pr.sh`. That trades one real
gap (forgotten registration → no gate-polling, no stuck-nudge) for another, when the
coupling between registration and authorization is the only thing actually broken.

## Status

applied 2026-08-16 — all 6 steps shipped on `fix/watcher-merge-authorization`.
Registry entries carry `authorized` (default `false`); `merge-watcher.py` gates
**both** write verbs on it — the merge in `handle_pass` and, beyond this entry's
literal text, the earlier C4 draft→ready flip in `poll_one` (a ready-flip is a
write to the user's PR, so leaving it ungated would reproduce the bypass one verb
earlier). An unauthorized non-draft PR still polls gates, nudges and escalates;
only a genuinely-draft one parks at the new `DRAFT_UNAUTHORIZED` state.

## Cross-ref

- `docs/harness/claude-code/hooks/autoregister-pr.sh`
- `docs/harness/claude-code/settings.json.tmpl:145`, `:161`
- `docs/agent-rules/merge-gates.md:84`
- `agents/scripts/core/merge-watcher-cli.py`, `agents/scripts/core/merge-watcher.py`
- `docs/plans/shipped/smatchet-merge-watcher.md:102` (explicit owner transfer)
- AGENTS.md § Merge gates (auto-merge authorization), § Autonomous ship-loop default
  (post-ship 4-option question)
- `AI_POLICY.md` § Two loop modes (autonomy as granted / revocable)
- PR #2027 (observed instance), PR #2024 (second registered entry, same session),
  PR #2031 (this entry's own PR — reproduced the bypass on creation)
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — merge-watcher.py gates both write verbs on the per-PR authorized flag, authorize/deauthorize exist in merge-watcher-cli.py, and merge-gates.md documents the consent rule.
  Status: applied (2026-10-04)

# Three `bare-json-parse-untrusted` exemptions describe input they do not actually receive

- **Category**: security
- **Priority**: P1
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../audits/DEVIATION_AUDIT_2026-08-16.md)
- **Status**: open

## What happened

`bare-json-parse-untrusted` exists because a bare `nlohmann::json::parse` builds a DOM whose
**recursive `~json` teardown** overflows the stack on a deeply-nested payload — an uncatchable
crash no `try/catch` intercepts (#1271 / #1287 / #1290), and the 3-arg non-throwing form does not
help because it still builds the full DOM. The rule is repo-wide default-deny; six first-party
sites escape it with a deviation asserting their bytes are program-internal.

Re-evaluating those six against the code that actually feeds them, three assertions do not hold.

**1 · `Source/Core/src/Config/KeybindingsConfig.cpp:153` and `:175`** —
`reason=keybinding args are app-serialised local config bytes loaded via the bounded config
reader, not external ingress`. The bounded config reader bounds the *outer* keybindings document.
`ArgsJson` is a `std::string` **field inside** it (`Config/KeybindingsConfig.h:27`, "command args
as JSON text; parsed at dispatch"), so to the outer `ParseBounded` a hostile payload is a single
depth-1 string node — the outer bound gives the inner parse no protection whatsoever. The tree
already disagrees with the reason in two places: the same `ArgsJson` bytes go through
`smatchet::json_safe::ParseBounded` at `Source/Core/src/Ui/SmatchetToolbarUi.cpp:132` and
`Source/Core/src/Ui/SmatchetImGuiHost.cpp:1060`. One field, three parse sites, and only this one
exempts itself.

That is not a coincidence, and it is the sharpest part of this entry. **`docs/audits/CPP_CODE_AUDIT.md`
finding #12 is this exact class** — *"[Low][Security/DoS] Bare `json::parse` on toolbar/keybinding
`ArgsJson` (2 sites) … Config-sourced `ArgsJson` (from `smatchet_config.json`) bare-parsed; deep DOM
→ uncatchable teardown crash. Fix: `ParseBounded` with empty-object fallback."* It is marked
**✅ Fixed** in that audit's summary table (line 16). The two sites it named
(`SmatchetToolbarUi.cpp`, `SmatchetUI.cpp`) *were* fixed, with precisely that fix. The class then
recurred in `KeybindingsConfig.cpp`, which the curated two-site list did not watch — and instead of
the prescribed one-line fix, that site received a deviation asserting it was safe.

This is the documented failure mode of a curated allow-list, quoted in `cpp-rules.md` as the reason
`bare-json-parse-untrusted` went repo-wide default-deny in the first place: *"the list lagged the
code every time the class recurred (#1573 / #1592 / #1598)."* Default-deny did its job and caught
the recurrence; the deviation then waived it on a premise the audit had already rejected.

**2 · `Source/Core/src/Tracker/PlaneProjectScope.cpp:13`** (and the same reason at
`PlaneIssueMappingPure.cpp:223`, `PlaneIssueSearch.cpp:98`) —
`reason=the Plane structured-query blob is app-serialised program-internal bytes, not
tracker-response ingress`. Traced the argument: `SetProjectInQuery(currentJql, …)` is called from
`Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp:293`, where `currentJql` is
`std::string(d.viewJqlEditor.buf)` (`:245`) — the **query text the user types into the view
editor**, which is then persisted with the saved view. That is user-authored input, not
app-serialised bytes.

**3 · `Source/Core/src/Tracker/TrackerFixtureBackendBase.cpp:25`** (`rule=unbounded-file-slurp`) —
`reason=developer-authored local fixture file, small by construction`. Three lines above it, the
header this file implements says the opposite in writing
(`Source/Core/include/Tracker/TrackerFixtureBackendBase.h:77-79`): *"The fixture path is
env-var-selectable per backend (`SMATCHET_TEST_*_BACKEND_FIXTURE`), so the parse caps
depth/nodes/bytes rather than trusting wherever it points."* The *parse* is indeed
`ParseBoundedOrDiscarded` — but the exemption is on the **slurp**, `buf << in.rdbuf()`, which
reads the whole file into memory before any cap applies.

## Why it matters

None of these is remotely reachable, and the realistic worst case is a local self-inflicted crash
or memory spike, so this is not an urgent exploit. What matters is the audit trail: a deviation is
the project's record of *why* a blocking safety rule was waived, and the next auditor re-evaluating
these in 2026-12 will read a provenance claim that is simply not what the code does. A wrong reason
is worse than no reason, because it terminates the inquiry.

Note also that all four Plane/IssueDraft markers in this class are among the 15 that `clang-format`
rewrites (fixed 2026-08-16 via `CommentPragmas`), and `GitHubFixtureBackend.cpp:26` — the wrapped
twin of item 3 — is failing open today.

## Concrete next action

Per site, cheapest first:

1. **KeybindingsConfig `:153` / `:175`** — delete both deviations and route through
   `smatchet::json_safe::ParseBounded`, exactly as `SmatchetToolbarUi.cpp:132` already does with
   the same field. The call sites already handle a failed parse (`is_discarded()` →
   `json::object()`), so `ParseBoundedOrDiscarded` is a drop-in. This removes the class rather than
   re-arguing it.
2. **Plane query blob (3 markers)** — same treatment; `ParseBoundedOrDiscarded` matches the
   existing `is_discarded()` handling line for line. If the exemption is kept instead, the reason
   must say what is true: *user-authored view-query text, bounded risk accepted because the blast
   radius is the author's own session*.
3. **`TrackerFixtureBackendBase.cpp:25`** — keep the exemption (a size cap on a test-fixture read
   is not worth the code), but rewrite the reason to match the header it contradicts:
   *env-var-selected local path, developer-controlled by deployment; parse is bounded, slurp is
   not*. Then fix the wrapped twin at `GitHubFixtureBackend.cpp:26`, which suppresses nothing today
   — `--scan-slurps` reports its slurp at `:28`.

Enumerator for the verification sweep: the six `rule=bare-json-parse-untrusted` and two
`rule=unbounded-file-slurp` markers are the complete first-party set —
`bash agents/scripts/project/test-lint-rules.sh --scan-bare-json` and `--scan-slurps` enumerate
exactly the sites they guard, and `--scan-bare-json` is empty tree-wide today. Replaying the
motivating case against that enumerator: drop the marker line at `KeybindingsConfig.cpp:153`
without touching the parse and `scan_bare_json_parse_file` immediately reports the parse — proving
the marker is load-bearing and the rule really does watch that line, so the deviation is the only
thing standing between this parse and the gate.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — all three sites now parse through ParseBoundedOrDiscarded / LoadFixtureJson (read capped before a bounded parse) with no deviation; no rule=bare-json-parse-untrusted marker remains in Source/ and --scan-bare-json is empty.
  Status: applied (2026-10-04)

# test-gate-selftests raw-self-exec negatives are vacuous on Windows (MSYS `#!` → `-x` true)

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-16
- **Found during**: shipping `github-issue-body-empty-line` (a UI display fix that touches no shell gate)

## Symptom

`bash scripts/dev/test-docs.sh` reports `test-gate-selftests` FAILED on Windows, on a
tree whose copy of `agents/scripts/core/test-gate-selftests.sh` is byte-identical to
`origin/develop`. The gate's own `--selftest` mode prints six paired failures:

```
test-gate-selftests selftest: FAIL — raw self-exec behind env -- was NOT flagged
test-gate-selftests selftest: FAIL — env -- exposer rejected for the wrong reason:
    test-gate-selftests: PASS — all 1 --selftest-exposing scripts assert a failure case.
```

(same shape for `env -i`, tab-separated, spaced-redirection, `&>`-redirection,
braced-expansion and `$( )`-capture fixtures). `--check` mode alone passes, so the
failure is invisible unless the suite runs `--selftest`.

## Cause

The raw-self-exec rule deliberately applies only to **non-executable** scripts (on a
mode-100755 file the raw form execs fine, so the 126-Permission-denied premise does not
hold). It reads the mode from the git index, falling back to the filesystem bit for
**untracked** files — and the `--selftest` synth fixtures are untracked, so they take
that fallback:

```sh
*) if [ -x "$f" ]; then _nonexec=0; else _nonexec=1; fi ;;
```

MSYS / Git-Bash has no real exec bit and reports any file whose first bytes are `#!` as
executable. Every synth fixture is written starting `#!/usr/bin/env bash`, so `[ -x ]` is
true, `_nonexec=0`, and the rule is skipped — the negatives can never fire. Reproduced
directly in this shell: a two-line `#!`-headed file reports `-x` true; the same file
without the shebang reports false. The `chmod -x "$synth"` the fixture sequence performs
does not help, since the heuristic ignores the mode.

Production impact is limited to the gate's own negatives: **tracked** files take their
mode from the git index, so the `--check` path that polices real first-party scripts is
correct on Windows. The damage is that a Windows agent sees `test-docs.sh` red on every
branch, which trains the reflex this repo least wants — treating a red gate as background
noise.

## Proposed fix

Make the untracked fallback independent of the MSYS heuristic. Cheapest correct option:
have the `--selftest` harness classify its own fixtures explicitly (e.g. `git -C "$tmp"
init` + `git add` the fixture so the index-mode branch is exercised, which is also closer
to what the rule actually polices), or gate the fs-bit fallback on
`git config core.fileMode` being true and treat untracked files as non-executable
otherwise. Whichever is chosen, add a negative that fails on a platform where `[ -x ]` is
unreliable, so the vacuity cannot come back.

## Why it matters

This is a `--selftest`-asserts-a-failure-case gate — its entire job is proving its
negatives are reachable — and on one supported platform six of them are unreachable for
the same class of reason the rule itself was written to catch (a negative satisfied by
something other than the behaviour it names).
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — test-gate-selftests.sh pins SMATCHET_GATE_SELFTEST_FORCE_NONEXEC (flipped to 0 for the one 100755 fixture), so the negative fixtures no longer depend on msys exec-bit answers; tests/bats/gate_selftests.bats covers both override directions.
  Status: applied (2026-10-04)

- 2026-08-16 · orchestrator · [tooling] · P2 — `test-gate-selftests.sh --selftest` fails all 11 negative fixtures on Windows/msys because its untracked-file mode fallback is `[ -x "$f" ]`, which msys answers TRUE for every temp file — so `scripts/dev/pre-ship.sh` cannot go green on a Windows dev box and its red becomes background noise
  Details: the raw-self-exec rule in
    [`test-gate-selftests.sh`](../../../agent-layer/agents/scripts/core/test-gate-selftests.sh)
    only applies to mode-100644 scripts (the "126 Permission denied" premise does
    not hold on a `+x` file). It reads the mode from the git index and falls back
    to the filesystem bit for untracked files — and the synthetic selftest
    fixtures are exactly that: untracked temp files. On NTFS under msys there is
    no meaningful exec bit; a freshly `printf`-written temp file reports
    `-rwxr-xr-x` and `[ -x ]` is TRUE. Every fixture therefore resolves to
    `_nonexec=0`, the detection chain short-circuits before the awk lexer runs,
    and each negative reports `raw self-exec … was NOT flagged`, `rc=1`.
    Verified platform, not regression: the script is byte-identical to
    `origin/develop` (`git show origin/develop:… | diff -q` → identical),
    `origin/develop` is green in CI (Linux, where the fs bit is real), and the
    detection pieces work standalone here — GNU Awk 5.3.2 matches
    `SELF_EXEC_RE` against a fixture by hand. Only the mode gate misfires.
    Damage is scoped to the SELFTEST, not the gate: real first-party scripts are
    tracked, so they take the `git ls-files --stage` path and classify correctly
    on every platform. The cost is that `pre-ship.sh` — the documented "run all
    gates locally before every push" entry point — is permanently red on Windows,
    so a genuine finding sitting next to it gets skipped. Hit while shipping
    `branch-protection-config-completeness`, where it sat alongside a real
    (separate, local-only) `test-agent-contract` drift that was easy to miss
    behind the standing red.
  Concrete next action: make the fixture's mode explicit instead of inferring it
    from the filesystem. Cheapest fix is at the fixture site — `chmod a-x` each
    synthetic fixture right after it is written, so the fallback is never
    consulted; one line per fixture, production path untouched. Belt-and-braces
    alternative: have the selftest export a mode override the classifier honours
    (e.g. `_GATE_SELFTEST_FORCE_NONEXEC=1`), which also documents that the
    fixtures are deliberately mode-100644. Either way add a bats case asserting an
    untracked, non-executable fixture IS flagged, so the platform divergence
    cannot regress silently. Est ~1h.
  Resolution: duplicate of the gate-selftests-msys-exec-bit entry, same fix — verified-in-tree 2026-10-04 (backlog-sweep-2026-10): the SMATCHET_GATE_SELFTEST_FORCE_NONEXEC pin replaces the [ -x ] untracked-mode fallback, with bats coverage.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [tooling] · P1 — `historical-review-worklist-misses-merge-commit-prs`: the historical-review work-list is built by scraping `(#N)` off develop squash subjects, so a PR that landed as a **true merge commit** is invisible to it — Batch 20 claimed a contiguous #1–#1940 frontier while silently omitting 7 merged PRs
  Details: The sweep's resume recipe
  ([`historical-review-findings.md`](../historical-review-findings.md) § Sweep status)
  discovers each batch's work-list from `gh pr list`, but in a `gh`-less environment
  (every remote session — see the Batch 13 header) the documented fallback is to
  scrape the develop log for squash subjects ending in `(#N)`. That scrape is only
  correct under the repo's stated squash-merge invariant, and the invariant does not
  actually hold: PRs merged with a **merge commit** carry the subject
  `Merge pull request #N from <branch>`, which has no trailing `(#N)`, and their
  constituent commits carry no PR reference at all. Such a PR is not "skipped" with a
  warning — it never enters the work-list, so it cannot appear in the batch's
  reviewed / clean / superseded counts, and the batch reports a complete frontier.
  <br><br>
  Measured on the #1878–#1940 range while re-running Batch 20 at full agent grade:
  GitHub's merged list for `base=develop` returns **60** PRs; the `(#N)` scrape
  returns **53**. The 7 missing are **#1883, #1919, #1920, #1921, #1923, #1927,
  #1932** — every one a merge commit, and (not coincidentally) every one part of the
  release-publishing pipeline (`.github/workflows/release.yml` driving
  `scripts/publish/release-github.sh`), which is signing/publishing code that has
  therefore never been survivor-reviewed. Batch 20's header nonetheless states "The
  frontier is now #1–#1940 contiguous", and the § Sweep status coverage claim inherits
  that error.
  <br><br>
  This is a **recurrence, not a first occurrence**, which is what raises it above a
  one-off correction. The Batch 13 header already records the same class — "4 PRs with
  edited/non-standard squash subjects the `(#N)`-suffix scrape misses: #1439/#1577/#1593/#1597"
  — caught that time only because the run cross-validated against GitHub's merged list
  by hand. Batch 20 did not repeat the cross-validation and the misses went unrecorded.
  The knowledge exists in the ledger as a batch-local anecdote; nothing in the tooling
  enforces it, so whether a batch is honest about its own coverage depends on whether
  that particular run happened to remember. Note also the second-order defect: blame
  attributes lines to a merge commit's **constituents**, never to the merge commit
  itself, so even a work-list that correctly contained #1883 would extract
  `FULLY SUPERSEDED` (an empty, falsely-clean review surface) if it passed the merge
  sha to the extractor. The #1593 "per-constituent special" in Batch 16 is the existing
  precedent for the right handling; it too is recorded only as prose.
  Concrete next action: make coverage a computed property of the sweep instead of a
  claim in its header. (1) Add a `--worklist <lo> <hi>` mode to
  [`historical-review-survivors.sh`](../../../agent-layer/agents/scripts/core/historical-review-survivors.sh)
  (or a sibling `historical-review-worklist.sh`) that emits `{pr, sha}` units for a PR
  range and **fails loudly** when the two enumerators disagree. The gate's enumerator
  is the GitHub merged set — `gh pr list --state merged --base develop --json number,mergeCommit`,
  or in a `gh`-less session the `list_pull_requests(base=develop, state=closed)` MCP
  call filtered on non-null `merged_at`; the candidate set is the `(#N)` develop-log
  scrape. Replaying the motivating bug against that enumerator: for `lo=1878 hi=1940`
  the GitHub set contains rows `1883, 1919, 1920, 1921, 1923, 1927, 1932` that the
  scrape set does not, so the gate trips on exactly the 7 PRs Batch 20 lost — and on
  the Batch 13 set (#1439/#1577/#1593/#1597) for its own range. (2) When a work-list
  entry's resolved sha is a merge commit (`git rev-list --parents -n1 <sha>` reports
  2+ parents), expand it to one unit per constituent (`<first-parent>..<sha> --no-merges`)
  rather than emitting the merge sha, promoting the #1593 special into the default
  path. (3) Have the sweep return its own coverage triple (requested / reviewed /
  unreachable) so a batch header quotes a computed number rather than asserting one,
  and correct Batch 20's contiguity claim in the ledger. Est ~2–3 h for (1)+(2),
  ~1 h for (3). Cross-ref: Batch 13 header (the #1439/#1577/#1593/#1597 precedent);
  Batch 16 § the #1593 per-constituent special; Batch 20 header (the incorrect
  contiguity claim); the #1987 review-ref bug, the other case where the extractor
  degraded silently rather than failing loudly.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — all three parts shipped (historical-review-worklist.sh with --json coverage; Batch 20 marked SUPERSEDED with the REDO section in historical-review-findings.md); archival had only waited on a lock that is gone from origin.
  Status: applied (2026-10-04; was: partially applied (2026-08-16 — shipped: parts (1) + (2). New gate)
    [`historical-review-worklist.sh`](../../../agent-layer/agents/scripts/core/historical-review-worklist.sh)
    builds the work-list from the GitHub merged set cross-validated against the
    develop-log scrape, **refuses to emit a scrape-only list** when no authority is
    available (`gh` absent and no `--merged-list`), reports any PR the scrape missed,
    fails loudly on a merged PR with no resolvable commit, and expands merge shas to
    per-constituent units. Validated by replaying the motivating bug against the real
    range: `--range 1878 1940` reports `authoritative 60 / scrape 53 / MISSED 1883
    1919 1920 1921 1923 1927 1932 / coverage 60/60 -> 64 units`, independently
    reconstructing the hand-built Batch 20-REDO work-list. `--selftest` carries 3 e2e
    fixtures, two of them asserts-failure (no-authority must exit 2; an unresolvable
    merged PR must exit 2). Wired into § Sweep status resume instructions and used to
    build Batch 22. Part (3) shipped 2026-08-29: `--json` emits ONE OBJECT
    carrying the computed coverage triple alongside the units, the sweep
    workflow accepts that object as `args` and returns the triple verbatim
    (`result.coverage`), and the selftest asserts the object against the
    fixture-A ground truth (2 authoritative / 1 scraped / missed [101] /
    2 covered → 3 units) plus units-parity with the bare-array output.
    Validated on the real Batch 23 range: `--json --range 2042 2159` returns
    `{authoritative: 76, scraped: 76, missed: [], covered: 76, units: 76}`,
    matching the shipped batch header. All three parts applied — archive to
    `applied.md` deferred only because that file sits under the
    `testing-surface-activation` plan-lock held by a concurrent session.)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [process] · P2 — a new WARN-first gate shipped without measuring its false-positive ratio on the whole tree first; the rule as written was 8/12 false, which would have trained readers to ignore it
  Details: PR #2028 added a code-span repo-path check to
    [`test-markdown-links.sh`](../../../agent-layer/agents/scripts/core/test-markdown-links.sh).
    It was authored, unit-tested, bats-tested and negative-tested against the
    delta scope — all green — and only a discretionary whole-tree `--all` run
    revealed the real signal quality: **12 warnings, of which 8 were false**. A
    backlog entry's proposal block names files it exists to CREATE ("Concrete
    next action: add a `scripts/dev/install-security-tools.sh`"), and those paths
    are the single most common repo-path code span in the backlog. The rule was
    narrowed (a structural, label-delimited proposal-block exemption) before
    landing, ending at 3 warnings — all genuine, all fixed.
    Residual, found by this very entry dogfooding the rule: the exemption is
    keyed on the entry's own proposal block, so QUOTING someone else's proposal
    inside a `Details:` block still warns (the sentence above does exactly that —
    the install-security-tools path it quotes is illustrative and is not on
    `develop`). That is the documented escape working as intended — the rule says
    *fix it, or note in prose that it is not on develop yet*, and this is the
    note — but it is a real residual worth watching: if quoted-proposal warnings
    become common, the exemption should widen from "inside a proposal block" to
    "any path in a sentence that names it as something to create".
    Why this matters beyond the one rule: a WARN-first gate's entire value is
    that a human reads the warning. A majority-false rule is worse than no rule,
    because it teaches the reader to skip the whole category — the same failure
    the `applied.md` exemption in that PR was independently added to avoid (~100
    archive rows that would have drowned the live signal). Delta-scoped tests
    cannot catch this: they only ever see the handful of lines a change touches,
    which is precisely the sample where a new rule looks clean.
    The near-miss was caught by discretion, not by process. Nothing in the
    ship-loop asks for the measurement, so the next WARN gate is one distracted
    author away from landing at 8/12.
  Concrete next action: add a line to
    [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md) § Cadence and
    verification — *a new or widened WARN-first rule must be run whole-tree
    (`--all` / equivalent) before push, and the PR body must state the warning
    count and the true/false split; a rule whose warnings are majority-false gets
    narrowed or scoped before it lands, never after.* Pairs naturally with the
    existing "gate, don't trust" principle: a gate nobody reads is not a gate.
    Cheap and mechanical — the measurement is one command, and stating it in the
    body makes the reviewer a second check on the calibration.
  Resolution: applied 2026-10-04 (backlog-sweep-2026-10) — the rule landed in docs/agent-rules/cpp-rules.md § Tiered enforcement ('Landing a new or widened WARN-first rule — measure it whole-tree first'), next to the gate definitions, instead of process-rules.md (that file was in another session's plan-lock write set). The quoted-proposal residual stays observational.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [tooling] · P2 — the CR finding gate re-triggers on `issue_comment` / `pull_request_review*`, and those runs execute the **default branch's** action code, not the PR's; so a fix to the gate cannot be trusted on its own PR — develop's old logic keeps overwriting the head's status and the last writer wins
  Details: Found while merging #2036, which closes a fail-open in
    [`cr-finding-gate/action.yml`](../../../.github/actions/cr-finding-gate/action.yml)
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
    [`cr-finding-gate.yml`](../../../.github/workflows/cr-finding-gate.yml)),
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
    [`merge-gates.md`](../../../agent-layer/docs/agent-rules/merge-gates.md) § CodeRabbit —
    *a change to a gate's own action/workflow is NOT proven by its check on
    its own PR; comment- and review-triggered runs execute the default
    branch's code, so verify against the `pull_request` run's log (or a
    scratch PR opened after the merge), never the final status cell.*
    (2) Make it visible instead of inferred: have the action print which ref
    it is running from (`GITHUB_EVENT_NAME` + the workflow ref) into the job
    summary and into the status description on a disagreement, so a mixed-code
    sequence is legible from the PR page rather than from three API calls.
    Neither needs new infrastructure; (2) is the one with teeth.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — cr-finding-gate.yml gained an always-run 'Record which gate code this run executes' step (event, ref, sha, workflow_sha, PR head to the step summary; NOTE when the checked-out ref is not refs/pull/*), plus the merge-gates.md line that a comment-triggered run does not prove a change to the gate's own code; structural bats.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [process] · P2 — a backlog entry asserted a third-party system's *policy* ("CodeRabbit ignores bot-authored triggers") on the strength of one non-response; a positive observation 85 minutes later contradicted it, on the same PR, before the entry had even merged
  Details: The sequence is worth keeping because nothing in it looks careless
    at the time. A nudge posted at `17:17:12Z` drew no CR response. Twelve
    minutes of nothing, plus a sibling entry recording a similar silence under
    a different bot identity, plus a plausible mechanism ready to hand (loop
    prevention — bots must not trigger bots), and the inference wrote itself. I
    filed it as a P2 with a settled-sounding title and three remediation
    options, one of which cost a repo secret. At `18:42:19Z` CR replied
    "`@github-actions`[bot] Reviewing `#2036` …" to a nudge from exactly that
    identity. The mechanism was never real.
    The defect is not the wrong guess — it is the *grade of claim*. Two
    genuinely different things got written in the same voice:
    - `17:17:12Z` produced no response — an **observation**, cheap and
      permanent; and
    - CR ignores bot-authored comments — a **mechanism**, which absence of a
      response cannot establish, because every competing explanation (quota,
      coalescing, a dropped webhook, an outage) produces the identical
      non-event.
    A non-response is compatible with every hypothesis, so it discriminates
    between none of them. Only a *positive* observation — a system doing
    something under condition A that it does not under condition B — licenses a
    mechanism. Silence licenses "unexplained".
    The cost is real even when caught. A wrong mechanism in the backlog is a
    wrong mechanism a later session will implement: the entry's cheapest-first
    remediation was to buy a PAT and store it as a repo secret, which would
    have added a rotating credential to the repo to solve a problem that did
    not exist. It also seeded a false sentence into a sibling entry's action
    item, which was queued for a rule-doc. Fabricated mechanisms propagate the
    same way fabricated quotes do, and the existing class-sweep rule in
    [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md) already covers
    the sweep once one is found — what is missing is the guard that stops it
    being written in the first place.
  Concrete next action: add one line to
    [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md) § Self-improvement
    entries — *an entry may state what was observed at any time; it may state
    WHY a third-party system behaved that way only from a positive observation.
    An inference drawn from a non-response is labelled `Hypothesis:` in the
    entry and MUST NOT appear in the entry's title, priority rationale, or any
    remediation that spends money, adds a credential, or lands in a rule-doc.*
    Mechanical to apply, and it would have downgraded this exact entry to
    "unexplained silence, experiment pending" — which is what it always was.
    Pairs with the class-sweep rule already there: that one cleans up after a
    false claim, this one keeps it out of the title.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — the observation-vs-mechanism rule is in AGENT_SELF_IMPROVEMENT.md § Workflow step 2; the class sweep reworded the unqualified 'CodeRabbit ignores bot-authored triggers' claim in cr-finding-gate/action.yml, merge-gates.md, merge-gates.sh and scripts/dev/trigger-coderabbit-review.sh as an observed asymmetry.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [tooling] · P2 — two silent ways an `@coderabbitai review` trigger does nothing: posted under a BOT identity it is dropped with no ack, and posted after a RATE-LIMITED pass on the same head it is a no-op because the head is already "seen" — both are indistinguishable from ordinary throttling, and each cost a full check-in cycle on PR #2023
  Details: **(a) Identity.** In the remote environment `$GITHUB_TOKEN` posts as
    `claude[bot]`, while `mcp__github__add_issue_comment` posts as the user.
    Evidence from #2023: all 8 triggers CR ever acted on were authored by
    `alexandrosk0`; the one posted via `curl` + `$GITHUB_TOKEN` sat for 31
    minutes with no ack, no rate-limit notice, and no review. There is no
    negative signal — the comment posts 201 and is simply never read — so the
    natural reading is "still throttled", and the wait is unbounded.
    **Correction (same day, from #2036):** this originally read "CodeRabbit
    ignores bot-authored comments (loop prevention)". That mechanism is wrong
    as stated — CR demonstrably acted on a `github-actions[bot]` trigger,
    acking it by name and reviewing the head. What survives is the narrower
    observation above: a `claude[bot]`-authored trigger drew nothing for 31
    min. Treat that as an unexplained result for that one app identity, not a
    policy, and read
    [`2026-08-16-cr-gate-nudge-403-and-the-withdrawn-bot-identity-claim.md`](tooling/2026-08-16-cr-gate-nudge-403-and-the-withdrawn-bot-identity-claim.md)
    for the full timeline and the experiment that would settle it. Note this does NOT retract the
    `curl -X PATCH` advice in the sibling entry
    [`2026-08-16-verdict-head-hex-hand-copied-into-pr-body.md`](applied.md):
    PR-*body* edits are identity-neutral and the token is the cheap path there.
    The split is the rule — bot token for body edits, user identity for anything
    CR must READ.
    **(b) Rate-limited head is consumed.** CR is incremental and "does not
    re-review already reviewed commits". A pass that reaches the head and THEN
    hits the limit still marks it seen, so the follow-up `@coderabbitai review`
    returns "Reviews are available now" and produces no review node — the
    `CR findings` gate stays correctly pending forever. The escape is
    `@coderabbitai full review`, which is exactly what the repo's own
    `cr-finding-gate` auto-nudge posts for this state; it worked first try
    (review node on head, `Merge Risk … up to 6b545`, 0 actionable).
    **(c) Gate-coverage gap found while diagnosing (b).** The auto-nudge did not
    fire here. Its clean-pass guard greps `no actionable (comments|findings)`,
    but CR's targeted verification reply said only "No findings", so `clean_ts`
    never advanced past `busy_ts` and the nudge stayed suppressed in precisely
    its target scenario. The rate-limit notice wording match has the same
    brittleness: it looks for `next review available`, while the follow-up said
    "Reviews are available now".
  Concrete next action: (1) add the already-settled rule to the CR rate-limit
    playbook in [`merge-gates.md`](../../../agent-layer/docs/agent-rules/merge-gates.md) §
    CodeRabbit rate-limit playbook — *after any rate-limited pass on the
    current head, escalate to `full review` rather than repeating `review`,
    since a plain re-trigger is a no-op on an already-seen commit*. The
    identity half of this action item (*"never post under the bot token"*) is
    **on hold** pending the experiment in the sibling entry above — do not
    write it into a rule-doc until a positive observation supports it. Add the
    diagnostic tell: **a trigger that
    draws no CR response AND no limit notice within ~15 min is an authorship or
    already-seen problem, not throttling.** (2) Widen the `cr-finding-gate`
    clean-pass regex to also accept a bare `no findings` (and the busy regex to
    accept `reviews are available`), with a bats case per wording — the guard
    is only as good as its vocabulary, and it silently under-fires when CR
    rephrases. Est ~0.5d total; (2) is the part with a gate behind it.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — merge-gates.md rate-limit playbook rule 3 (after a rate-limited pass post @coderabbitai full review; no reply and no limit notice in ~15 min = authorship / already-seen, not throttling); the identity half cites the OSS human-trigger invariant; the clean-pass regex accepts 'no actionable (comments|findings)|no findings' with a bats case per wording; 'Reviews are available' pinned as NOT busy.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [tooling] · P2 — the `(head=<12-hex>)` the Intent gate checks is transcribed BY HAND into the PR body while `record-review-verdict.sh` already prints the exact line; one mistyped hex red-flagged the gate on PR #2023, and every re-push needs the same manual re-transcription
  Details: `record-review-verdict.sh` writes the local marker AND echoes the
    canonical line `adversarial-code-review: <verdict> (head=<12-hex>)`. The PR
    body must carry that same line for the Intent-section check
    (`check-pr-intent.sh` matches `(head=<12-hex>)` against the PR head). Nothing
    connects the two: the orchestrator reads the script's output and retypes it
    into the body. On PR #2023 I predicted the hex before the commit existed and
    wrote `498f7ad2b3f4` when the real head was `498f7ad2c1f3` — the gate caught
    it correctly, but it cost a red check, a diagnosis detour, and a body edit.
    The same PR then needed the line re-transcribed on SIX subsequent pushes
    (rounds 1-6), each an opportunity for the same typo. Related cost: every one
    of those body edits re-sent the ENTIRE body through the PR-update tool
    (~8 KB), when a `curl -X PATCH` with `$GITHUB_TOKEN` (present in the remote
    environment) does it far more cheaply — worth codifying alongside.
  Concrete next action: teach `record-review-verdict.sh` an opt-in
    `--sync-pr <n>` (or a sibling `sync-verdict-to-pr.sh`) that, after writing
    the local marker, PATCHes the PR body: replace the existing
    `^adversarial-code-review: .*$` line with the freshly generated one, or
    append it when absent. The script already owns the canonical string and the
    head SHA, so the transcription step — and its typo class — disappears. Guard
    it: no-op with a clear message when `$GITHUB_TOKEN` is unset or `--sync-pr`
    is omitted (local-only runs must keep working), and never invent a PR
    number. Bats-testable against a stub API endpoint. Est ~0.5d.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — record-review-verdict.sh --sync-pr <n> upserts the head-bound verdict line into the PR body through agents/scripts/core/lib/pr-body-edit.sh (refuses to drop lock-slug:/holds-lock: lines); ship-loops.md item 5 and the PR template name it.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [tooling] · P2 — a `lock-slug:` marker left inside the HTML comment the PR template ships it in never releases the lock: `lock-cleanup.yml` anchors its regex to `$`, finds no match, emits a `::notice::` and exits 0, so the job goes **green** with the delete step **skipped** — the failure is invisible on the PR (an HTML comment renders as nothing) and invisible in the checks list (green), and only surfaces days later as a stale `refs/locks/<slug>` in the staleness sweep
  Details: **(a) The mechanism.**
    [`lock-cleanup.yml:58-62`](../../../.github/workflows/lock-cleanup.yml)
    matches `^[[:space:]]*lock-slug:[[:space:]]*[a-z0-9][a-z0-9-]{0,63}[[:space:]]*$`
    — the trailing `$` after the slug means the template's
    `<!-- lock-slug: your-slug-here -->` form cannot match, because ` -->`
    follows the slug. On no match, `:64-68` prints
    `::notice::No 'lock-slug: <slug>' line found in PR body; no release.` and
    `exit 0`. The `Delete refs/locks/<slug> if present` step is gated on
    `steps.parse.outputs.slug != ''` (`:80`), so it reports **skipped**, and the
    workflow run is **green**. The regex is pinned verbatim as `EXPECTED_PAT` in
    [`tests/bats/lock_cleanup.bats:19`](../../../tests/bats/lock_cleanup.bats),
    so the anchor is deliberate and tested — the defect is not the regex, it is
    that the *only* signal for the miss is a `notice` annotation nobody reads.
    **(b) The trap is the template itself.**
    [`.github/pull_request_template.md:50-51`](../../../.github/pull_request_template.md)
    ships both markers pre-commented, and `:46` instructs "add the trigger line
    below somewhere in the PR body (**uncomment** + edit)". Leaving the comment
    delimiters in place is therefore the *default* state of every PR body, and
    the one keystroke that arms the release is the one nothing checks. The rule
    itself is unambiguous —
    [`ship-loops.md:162`](../../../agent-layer/docs/agent-rules/ship-loops.md) requires the `open
    PR` step to write "*the exact line `lock-cleanup.yml` matches*", i.e. the
    bare form — so this is an operator error, not a doc conflict;
    it is worth a guard precisely because the correct and incorrect forms are
    visually identical in the rendered PR body.
    **(c) Observed, 2026-08-16.** Two of three PRs merged this session
    (`agent-debug-build-flag`, `windows-cdb-tooling-doc`) carried the commented
    form. Both cleanup runs were green with the delete step skipped; both locks
    survived the merge and had to be deleted by hand via
    `gh api -X DELETE repos/<owner>/<repo>/git/refs/locks/<slug>` — the same call
    the workflow would have made. `gh run view --log | grep '::notice'` does not
    surface the annotation (it returns only the `##[group] Run` echo); the notice
    is reachable only via
    `gh api repos/<owner>/<repo>/check-runs/<jobid>/annotations`. The existing
    downstream catch,
    [`lock-staleness-sweep.sh:177`](../../../agent-layer/agents/scripts/core/lock-staleness-sweep.sh),
    already names this exact failure in its remediation text ("the PR was missing
    a `lock-slug: ${slug}` line in its body") — so the class is known and the
    sweep is the only thing catching it, days late.
  Concrete next action: (1) **Detect the commented form and fail loudly** — in
    the parse step, when the body contains `lock-slug:` but the anchored regex
    matched nothing, `::error::` (not `::notice::`) and exit non-zero with the
    text *"a `lock-slug:` marker is present but commented out or malformed;
    uncomment it to a bare line"*. This is the only variant that distinguishes
    "no lock on this PR" (legitimate and common — pure-docs slices) from "a lock
    that was meant to release and did not", and it is a two-line change plus a
    bats case alongside the existing `EXPECTED_PAT` assertions. Deliberately
    scope it to the *commented-marker* case; a blanket fail-on-no-slug would red
    every lockless PR. (2) **Remove the trap at the source** — either drop the
    comment delimiters from the template line (leaving a placeholder slug that
    fails the grammar check loudly) or move the marker out of an HTML comment
    entirely, so the armed and unarmed states differ visibly in the rendered
    body. Keep the informational `holds-lock:` line commented; it is
    intentionally never matched. (3) Have the ship-loop's `open PR` step
    self-verify: after creating the PR, re-read the body and assert the bare
    line matches the same anchored regex, so the miss is caught at PR-open time
    rather than at merge time. Est ~0.5d total; (1) is the cheapest and has a
    gate behind it.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — lock-cleanup.yml warns on a commented-out real slug (the template placeholder stays silent); a warning, not an error, because the new branch-keyed release (lock-release-on-close.sh) frees a lock the PR's own branch claimed anyway; template prose now says a commented line is not armed. bats covers all three shapes.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-16 · orchestrator · [infra] · P2 — the TSan lane is documented as advisory in four places but its check name carries no `advisory` token, so the merge poller blocks on it
  Details: Surfaced while grounding `docs/plans/shipped/autonomous-debug-live-evidence.md`;
    unrelated to that plan's subject, filed here per `docs/agent-rules/ship-loops.md`
    § "Unrelated work never shares a PR".
    `.github/workflows/tsan-linux-nightly.yml:51` publishes the check name
    **`TSan Linux subset (Clang)`**. The poller's only exemption is a
    case-insensitive `contains("advisory")` on the check NAME
    (`agents/scripts/core/merge-gates.d/10-gate-filter.sh:66-70`, `:91-95`) under
    `MERGE_GATES_BLOCK_ALLOWLIST_RE="."`. That name has no such token, so a red
    TSan run **blocks the merge gate** — while four places assert the opposite:
    `tsan-linux-nightly.yml:13-14` ("ADVISORY / non-required"),
    `docs/plans/active/tsan-imgui-linked-target.md:79` and `:92`, and
    `docs/plans/active/build-quality-velocity-hardening.md:244`.
    The exposure is bounded but real: the workflow's `pull_request` trigger is
    paths-scoped (`CMakeLists.txt`, `CMakePresets.json`, `cmake/Sanitizers.cmake`,
    `tests/CMakeLists.txt`, `Source/Core/src/Sync/**`, `.../Persistence/**`,
    `.../Config/**`, `GridLiveContext.cpp`), so it only lands on PR heads touching
    those paths — but when it does, "advisory" is documentation, not mechanism.
    This is the inverse of the sibling entry
    infra/2026-08-16-stale-advisory-lane-docs: there the docs under-state what
    blocks; here they over-state what doesn't.
    Note the decision is genuinely open, not merely a doc fix — a required-ish
    TSan lane may well be *desirable*. What is not defensible is the current state,
    where the behaviour and the four documents disagree.
  Concrete next action: pick one and make the other side match. Either
    (a) rename the check to `TSan Linux subset (Clang, advisory)` so the mechanism
    matches the four docs, or (b) keep it blocking and correct all four docs plus
    `AGENTS.md` § Merge gates / `docs/agent-rules/merge-gates.md`'s advisory-token
    user list. Prefer (a) unless the lane's green-rate on the scoped paths is
    already high enough to gate on — check the last ~20 scoped runs before
    deciding. Also worth extending `tests/bats/merge_gates.bats` with a
    name-vs-intent case so a future lane cannot claim advisory in prose while
    blocking in fact.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — option (b): tsan-linux-nightly.yml and both plan docs now say the lane is not a required context but blocks under block-on-any-red when it runs on a PR; merge_gates.bats pins a red 'TSan Linux subset (Clang)' as blocking. codeql.yml's twin drift is fixed too (findings advisory, the check is not).
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

# `dup_audit.py` flags shared include prologues, so every god-file split buys 4-5 exemptions

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../audits/DEVIATION_AUDIT_2026-08-16.md) § S6
- **Status**: applied (2026-10-03)

## What happened

Of the 131 single-line `duplication` markers in `Source/`, **73 (55%)** sit above an `#include` /
`using` / `namespace` prologue, or say as much in their `reason=`. They are not exempting
copy-pasted logic; they are exempting the fact that four sibling TUs carved out of one god-file
necessarily open with the same include block.

Root cause is in the tokenizer, not the code: `agents/scripts/core/dup_audit.py` has no
include-block handling anywhere. `normalize_token()` maps identifiers to `ID` and passes
punctuation through, so `#include "AppControllerImpl.h"` tokenizes exactly like executable code. A
shared prologue of ~70+ tokens therefore clears `MIN_CLONE_TOKENS = 70` and reports as a
cross-file copy-paste clone.

The cost compounds: `god-file-splits` is an endorsed refactor, and each split adds one exemption
per sibling TU. The seven largest families in the tree today —
`CliCommandRunner` (5), `AppController_LuaBindings` (4), `ConfigManager` (4), `MarkdownConvert` (3),
`ActiveProjectGrid` (3), the `Scenarios/` TUs, the `Builtin/` command TUs — are all this one shape,
and each carries a `revisit=when a shared <X> TU prologue header is introduced` that nobody is
committed to landing.

`Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:12` already names the real fix in its revisit:
`when the dup auditor scopes cross-file clones to logic blocks`. It has not fired.

## Why it matters

Every one of those 73 exemptions is a line of prose a reviewer must read and a future auditor must
re-evaluate, standing in for a tokenizer decision. It also inverts the gate's signal: a reviewer
who sees `SMATCHET_DEVIATION(rule=duplication)` at the top of a TU learns nothing, because half of
them mean "this file has includes".

## Concrete next action

Teach `dup_audit.py` to drop contiguous preprocessor runs before shingling: in `_tokens_with_lines`
(or a filter immediately after it), skip tokens whose source line's first non-space character is
`#`, and skip a leading `using`-declaration run at file scope. Enumerator for the verification
sweep: the 73 markers are exactly the `rule=duplication` markers in
`git ls-files 'Source/**' | grep -E '\.(cpp|h|hpp)$'` whose next non-blank line starts with
`#include`, `using`, or `namespace`. Replaying the motivating case: remove the marker at
`Source/Core/src/Config/ConfigManager_Load.cpp:18` and re-run `dup_audit.py --diff origin/develop`
— it must stay green, where today it FAILs on the `ConfigManager` prologue clone. Then retire the
73 in one sweep and drop the seven dead `when a shared <X> prologue header is introduced` triggers
with them.

Guard against over-correction: keep flagging a clone that merely *starts* in a prologue and
continues into real logic — skip the preprocessor tokens, do not skip the span that contains them.

Triggered-follow-up: when=pr-count:base=develop;since=2026-08-16;n=20; action=re-measure the include-prologue share of duplication exemptions; baseline=73 of 131 (55%) on 2026-08-16; fired=2026-10-03

## Resolution

Applied 2026-10-03. `agents/scripts/core/dup_audit.py` now drops preprocessor directives before
shingling, except function-like macros (their bodies are code), plus the `using` / namespace-alias
run that opens a file. Only the directive tokens go, so a clone that continues past a prologue into
logic is still reported, starting at its first line of logic (selftest + bats pin both halves).

Measured on the tree that day:
- Cross-file clones: 646 → 441.
- `duplication` markers: 174 → 113. 61 exempted only a prologue and were removed: 54 made dead by
  the change, 7 already dead. The prologue marker in `Source/Core/src/Commands/ViewCommands.cpp` is
  dead too but stays for now: that file was never clang-formatted, so touching it makes
  `pre-ship.sh` reformat all of it, which pushes two functions over the size cap.
- 5 markers covered a clone that continues into logic and were fixed in place:
  - 2 were unwrapped onto one line;
  - 1 lost the `// clang-format off/on` guards that stopped it being the nearest line above;
  - 2 moved below the include block, directly above the code they cover (a fixture declaration
    shell and a scenario class skeleton).
- The exemptions were compared clone by clone before and after the marker edits: the same clones,
  and none lost its exemption.
- 5 clones became newly visible, because an `#if` line used to split them. They are real copy-paste
  and are grandfathered like any existing clone.

The replay in the next action above was inaccurate. Deleting a marker leaves `--diff` green with or
without the fix: removing a comment leaves the token stream unchanged, so the clone stays
grandfathered. The new `--dead-markers` mode answers the question instead: it lists every marker
that exempts no clone the detector reports today. 10 remain: the `ViewCommands.cpp` one above and 9
that are not prologue markers, left for their own revisit dates.

# 47 `SMATCHET_DEVIATION` markers are wrapped across lines and invisible to every gate

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../audits/DEVIATION_AUDIT_2026-08-16.md) § S2
- **Status**: applied 2026-10-03

## What happened

The bash gate matches `DEV_RE='SMATCHET_DEVIATION\(([^)]*)\)'`, which requires the closing paren on
the **same line**. 47 live first-party markers open on one line and close on a later one, so
`DEV_RE` never matches them: the line is treated as ordinary prose, and **both the suppression and
the expiry are lost**. The Python auditors (`dup_audit`, `function_size_audit`,
`appcontroller_fan_in_audit`, `include_cycle_audit`) are per-line too — their "nearest non-blank
line above the target" is the marker's trailing prose, which carries no token, so a wrapped marker
survives only via `dup_audit._suppressed`'s "anywhere within the clone span" fallback, which
[`cpp-rules.md`](../../../agent-layer/docs/agent-rules/cpp-rules.md) itself warns is accidental and intermittent.

Where they are: `Source/Core/include/Tracker/{GitHub,Jira,Linear,Plane}Client.h` (21),
`Source/Core/src/Tracker/*` (8), the three AI provider clients (5), `Source/Standalone/Cli*` (4),
9 others.

Two sibling fixture backends make it legible — same rule, same reason, same code:

```
Source/Core/src/Tracker/TrackerFixtureBackendBase.cpp:25   marker on ONE line  -> suppressed, clean
Source/Core/src/Tracker/GitHubFixtureBackend.cpp:26        marker WRAPPED      -> NOT suppressed
```

Running the project's own `scan_file_slurp_file` over the tree today emits
`unbounded-file-slurp  Source/Core/src/Tracker/GitHubFixtureBackend.cpp:28`.

## Why it matters

`unbounded-file-slurp` is WARN-first, so nothing blocks today. A whole-tree sweep with every bash
scanner confirms `bare-json-parse-untrusted` and `catch-all-swallow` are currently clean — i.e. no
*blocking* rule is defeated right now. That is luck. The same wrap on a `bare-json-parse-untrusted`
or `no-detach` escape fails the merge gate for reasons unrelated to the author's change, and a wrap
on any marker removes it from `deviation-overdue` permanently and silently.

This is the tail of [`2026-08-05-clang-format-reflows-deviation-comments.md`](applied.md):
that entry's option 2 (`CommentPragmas`) shipped 2026-08-16 and stops *new* wrapping, but it does
not un-wrap the 47 already in the tree.

**Update 2026-10-03 — step 1 is done.** The 2026-11-30 / 12-01 and 2026-12-31 deviation batches
unwrapped every remaining wrapped marker: `git grep -n "SMATCHET_DEVIATION(" -- 'Source/**'` now finds
none without `revisit=` on the same line. Each one was resolved rather than just re-flowed:
- 5 that exempted no clone were deleted (`JiraClient.h`, `LinearClient.h` ×2, `AppController.h`,
  `LinearIssueMutation.cpp`);
- the `Tracker/*Client.h` override-signature ones became one-line `revisit=never`;
- the rest got one-line, staggered dates backed by debt entries.

The wrap had also hidden 24 markers dated 2026-12-31 from `deviation-overdue`, so the 2027-01-01 cliff
was larger than the gate could see.

**Update 2026-10-03 — step 2 is done.** `deviation-malformed` is an absolute, whole-tree rule next to
`deviation-overdue`. It fails a `SMATCHET_DEVIATION(` that does not close on its own line, that has a
missing or blank `rule=` / `reason=` / `owner=`, or that has no `revisit=` at all. A blank `revisit=`
stays `deviation-overdue`'s to report. A prose mention of the token in a C++ comment fails too, because
`DEV_RE` would read it as a marker. Run against develop before the batch, it reports 30 lines: the 28
wrapped markers, the wrapped `GitHubFixtureBackend.cpp` slurp marker, and
`ITrackerFieldCatalog.h:42` (no `reason=`).

## Concrete next action

Two steps, in order — the second is unsafe before the first.

1. **Sweep**: re-word each of the 47 markers so the whole `SMATCHET_DEVIATION(...)` fits one line
   directly above its target, moving overflow prose to lines *above* the marker (the shape
   `cpp-rules.md` § "One line, directly above" prescribes). 47 judgement calls about `reason=`
   prose, not a mechanical edit — do it per-subsystem, `Tracker/*Client.h` first (21 of the 47, all
   the same "interface-mandated override-signature symmetry" text).
2. **Gate it**: add the well-formedness rule that option 3 of the 2026-08-05 entry proposed —
   fail on a `SMATCHET_DEVIATION(` with no balanced `)` on the same line, and on a marker missing
   `reason=` / `owner=` / `revisit=`. Enumerator: every line matching `SMATCHET_DEVIATION(` in
   `git ls-files 'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**'` filtered to
   `.cpp/.h/.hpp` — the same file set `compute_wide_violations` already walks. Replaying the
   motivating case against that enumerator: `Source/Core/src/Tracker/GitHubFixtureBackend.cpp:26`
   appears in it, has no balanced `)` on the line, and would trip the gate — as would
   `Source/Core/include/AppController.h:989`, which has no `owner=` or `revisit=`. Run step 2 only
   after step 1, or CI red-walls on all 47 at once.

Triggered-follow-up: when=date:2026-09-15; action=re-run the wrapped-marker count and confirm the sweep landed before the 2026-10-01 overdue cliff; baseline=47 wrapped markers on 2026-08-16; fired=2026-10-03 (0 wrapped markers left after the 2026-12-31 batch)

# 25 deviations expire on the same day and block every merge when they do

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-16
- **Observed on**: the full deviation re-evaluation, [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../audits/DEVIATION_AUDIT_2026-08-16.md) § S5
- **Status**: applied 2026-10-04

## What happened

`deviation-overdue` is an **absolute, whole-tree** rule: `compute_wide_violations()` scans every
first-party C++ file (not the diff), and any hit sets `rc=1` at
`agents/scripts/project/test-lint-rules.sh:705`. One overdue marker anywhere fails the gate for
every open PR until it is re-dated or removed.

The live `revisit=` dates are not spread out — they were stamped in bulk by sweeps. Stubbing
`today_ymd()` and re-running the real `compute_wide_violations`:

| date | markers overdue | gate |
|---|---|---|
| 2026-08-16 (today) | 0 | green |
| **2026-10-01** | **25** | **RED — all merges blocked** |
| 2026-10-02 | 27 | RED |
| 2026-12-02 | 34 | RED |
| **2027-01-01** | **94** | **RED** |

**Update 2026-08-16** — retiring the 20 markers that suppress no live clone (audit § Retire) cuts
the near cliff from **25 to 15** and the 2027-01-01 cohort from **94 to 77**. The class is reduced,
not closed: 15 markers still land on one day, and the remaining 77 are still a single date.

The 25 that land on 2026-10-01 all carry `revisit=2026-09-30` with `owner=security-audit` or
`owner=cpp-audit` — a single sweep's default date, not 25 exemptions that genuinely come due the
same Tuesday. The 2027-01-01 spike is the same story with `revisit=2026-12-31`.

## Why it matters

The failure lands on whoever happens to push that morning, not on the owner of any of the 25
markers, and it lands on all of them at once. The gate is correct — the audit loop is *supposed* to
force a re-evaluation — but a same-day cohort converts "re-evaluate one exemption" into "re-evaluate
25 or bypass the gate", and bypassing is the outcome that actually happens under deadline. An
`--admin` merge past it is exactly what `postmortem-owed.sh` flags, so the cliff manufactures the
incident it then reports.

## Concrete next action

1. **Before 2026-09-30**, re-evaluate the 25-marker cohort (they are listed in the audit's retarget
   table) and give each an outcome: retire it, or re-date it to a *staggered* date, or convert it to
   `revisit=never` where the exemption is genuinely standing. Most are the include-prologue class
   (tooling entry "`dup_audit.py` flags shared include prologues", archived in [`applied.md`](applied.md))
   and should be retired by fixing the auditor, not re-dated. The auditor fix landed 2026-10-03.
2. **Stop the class regenerating**: a sweep that stamps N markers must not give them all one date.
   Add a check next to the deviation well-formedness gate (`deviation-malformed`, landed 2026-10-03;
   history in [`2026-08-16-wrapped-deviation-markers-invisible-to-gate.md`](applied.md))
   that WARNs when more than ~8 first-party markers share a single `revisit=` date. Enumerator: the
   `revisit=` values `compute_wide_violations` already parses, bucketed by date. Replaying the
   motivating case against it: today's tree has buckets of 54 (2026-12-31) and 25 (2026-09-30), both
   of which would WARN.
3. `AGENTS.md` § Tiered enforcement should say plainly that `deviation-overdue` is whole-tree and
   merge-blocking, not diff-scoped. Today a reader has to infer that from the script.

Triggered-follow-up: when=date:2026-09-20; action=confirm the 25-marker 2026-09-30 cohort has been re-evaluated before it fires; baseline=25 markers dated 2026-09-30 as of 2026-08-16; fired=never

**Applied 2026-10-04.**
1. The 2026-09-30 / 10-01, 11-30 / 12-01 and 2026-12-31 cohorts were each re-evaluated in their own PR (#2272, #2275, #2280). Markers were retired where they exempted nothing, folded where the clone could be folded, set to `revisit=never` where standing, and otherwise staggered with a debt entry. Unwrapping the wrapped markers showed the 2026-12-31 cohort had been larger than the gate could see.
2. The `deviation-cohort` WARN in `test-lint-rules.sh --diff` fires when a diff adds a marker on a date more than 8 markers already share (`SMATCHET_DEVIATION_COHORT_MAX`). The `--scan-revisit-cohorts` sweep lists every crowded date. The last such date, 2027-03-31 with 21 markers, was spread by group so that no date holds more than 8.
3. `cpp-rules.md` states that `deviation-overdue` is a strict, whole-tree, merge-blocking rule, and the AGENTS.md contract-card row lists it as absolute.

# `cancel-in-progress: false` still collapses the PENDING queue — cr-finding-gate needs a decision

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-14
- **Split from**: `required-check-cancelled-while-pending-wedges-poller`
  (2026-08-04, archived — its problem (2), filed separately per its own scoping
  because this needs a workflow-design decision, not a poller change)

## Problem

`.github/workflows/cr-finding-gate.yml` sets `concurrency.group` per PR with
`cancel-in-progress: false`. The header comment reasons "let them all complete
rather than cancel" — true only for **in-progress** runs. GitHub keeps only ONE
**pending** run per group and cancels the rest; on #1937 five
`pull_request_review_comment` runs were cancelled in one second, and a
cancelled-while-pending run creates **no check-run at all**, leaving the
required `CR finding gate` context absent-forever (nothing re-triggers it).

The poller side is fixed (merge-gates.sh exit 8 names the cancelled run and the
`gh run rerun <id>` remedy), but the workflow still manufactures the wedge.

## Decision needed (either resolves it)

1. **Drop the `concurrency` block entirely.** The job only posts a status and
   the comment already argues last-write-wins; every run evaluates the current
   head state via GraphQL, so verdicts converge regardless of completion order.
   Cost: bursts run more concurrent jobs, each polling up to
   POLL_BUDGET_SECONDS (180 s) — runner minutes, not correctness.
2. **Keep it and add a backstop** (scheduled or `workflow_run`) that re-posts
   the context when the head lacks it. More moving parts; keeps burst cost low.

Option 1 is simpler and removes the class; take it unless runner-minute cost
is shown to matter.

## Status

Applied (2026-08-14, same day — option 1 taken per this entry's own
recommendation: the `concurrency` block is removed from
`.github/workflows/cr-finding-gate.yml`, with the replacement comment
documenting why running every event is correct for this gate (each run
evaluates current head state and posts last-write-wins, so parallel runs
converge; burst cost is bounded by POLL_BUDGET_SECONDS per run). The
no-concurrency shape is pinned by `tests/bats/cr_finding_gate.bats`
("workflow has NO concurrency block", YAML-key match) so the collapse class
cannot be silently reintroduced.

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

  Status: applied — survival condition met on the #2006 merge drive
  (2026-08-13): three CodeRabbit fix-push rounds, and on every push the
  `Intent section` run that raced the body update FAILED on the stale
  `head=` (three observed rejections) until `record-review-verdict.sh`
  re-stamped the verdict for the new head — the binding fired exactly as
  designed, and the PR merged with the verdict bound to the merged head
  `18198525dd09`.
  Last-reviewed: 2026-08-14

- 2026-08-12 · claude-code · [process] · P2 — a CodeRabbit review that COMPLETED can still leave the head with no review evidence the `CR findings` gate accepts, in three observed shapes: a rate-limit-stale `success` status, a comment-only clean pass that posts no review object, and a clean-with-nitpicks review object whose body omits the actionable-count header; all are indistinguishable from "never reviewed" until something re-triggers the reviewer

  Details: the gate rule (shipped after #1996, tightened by the ledger learning
  of 2026-08-11) is correct: `state: success` + description `Review rate
  limited` is NOT review evidence, and the gate must see an actual review on
  the CURRENT head. What this entry records is how often a genuinely-completed
  review still fails to produce that evidence, measured across the #1999 merge
  drive (2026-08-12, ~7 review rounds):

  1. **Rate-limit-stale status.** The auto-review attempt posts `success /
     "Review rate limited"` and never updates, even after a later
     comment-triggered review of the same head completes clean. Observed on
     head e33b5ca0: the 08:11 incremental pass replied "Review complete — no
     actionable findings" as an ISSUE COMMENT, posted no review object, and
     left the 07:13 rate-limit status in place; the gate re-polled at 08:22
     and correctly reported "awaiting CodeRabbit review on current head".
     Correct gate, wedged PR.
  2. **Comment-only clean pass.** `@coderabbitai review` on an
     incrementally-clean head can complete without submitting a GitHub review
     object at all (its reply carries the verdict as prose). Nothing for the
     gate's GraphQL query to find; same wedge from a different door.

  Both resolved the same way both times: `@coderabbitai full review`, which
  always submits a review object and refreshes the commit status ("It must
  create current-head review evidence" — CodeRabbit's own ack of the request).
  Cost when it recurs: one full extra review cycle plus however much of the
  adaptive rate-limit window the retry burns (25-55 min per wait, four waits
  during the #1999 drive).

  3. **Header-less clean-with-nitpicks review** (added 2026-08-13, observed on
     the #2002 merge drive, round 12). A full review CAN submit a review object
     on the current head and still wedge the gate: a clean pass that carries
     only nitpicks omits the `Actionable comments posted: N` header line from
     the review body. `cr-finding-gate`'s parser greps for exactly that header,
     treats a header-less body as "not parseable → retry", exhausts its retry
     window, and resolves to PENDING — permanently, since the review it is
     waiting for already happened. Unlike shapes 1–2, `@coderabbitai full
     review` does NOT resolve this one: the fresh review is clean again, omits
     the header again, and re-wedges. This is a gate bug, not a reviewer
     quirk — the fix is in the gate: an on-head review object whose body has
     nitpicks/summary content but no actionable-count header IS evidence of 0
     actionable findings and must resolve to success. On #2002 the status sat
     pending through a valid round-12 clean review and the merge proceeded on
     directly-verified review evidence instead of the status (rationale on the
     PR).

  Also worth recording for the next long merge drive: pushing to a PR while
  CodeRabbit is mid-review ABORTS the review ("head commit changed during the
  review"), and the automatic retry burns the next rate-limit slot — the
  costly half of the #1999 churn was self-inflicted by exactly that. Batch
  fixes; push once; request once.

  Concrete next action: teach the `CR finding gate` workflow's poller the
  distinction it already half-knows. When it observes (a) a CodeRabbit status
  whose description is terminal-but-evidence-free (`Review rate limited`, or
  `Review completed` with no review object on the head) AND (b) a completed
  clean pass advertised only in comments, it should POST the
  `@coderabbitai full review` nudge itself — once per head, budget-capped —
  instead of parking on `pending` until a human or a timer intervenes. The
  merge-gates.sh side already has the auto-post shape
  (MERGE_GATES_STALE_REREVIEW_POLLS); the CI gate lacks it. For shape 3 the
  nudge is useless (see above) — the parser itself must accept an on-head
  review object with no actionable-count header as 0 actionable. Est ~0.5d
  including bats coverage for the once-per-head cap.

  Status: applied (flipped at archival)
  Last-reviewed: 2026-08-13

- 2026-08-11 · claude-code · [process] · P3 — the shared staleness helper exists but only three of the core gates call it; `issue-sweep.sh` still runs whatever logic its checkout happens to hold, with nothing saying so

  Details: carried forward from
  [`2026-08-06-gate-tooling-run-from-stale-session-branch`](applied.md) (applied
  2026-08-11), whose thesis — make staleness self-announcing instead of silent — is
  shipped. [`agents/scripts/core/lib/script-freshness.sh`](../../../agent-layer/agents/scripts/core/lib/script-freshness.sh)
  now provides `script_freshness_verdict` + `warn_if_script_stale`, and three callers
  use it: `merge-gates.sh` (off/warn/block, default warn), `pre-ship.sh` (advisory,
  printed immediately before its `Safe to push` line), and `postmortem-owed.sh`
  (qualifying its `no gate escapes owed` clean result).

  What is left is breadth, and it is deliberately P3 rather than P1 because the
  highest-stakes surfaces are already covered:
  - **`issue-sweep.sh`** — genuinely unwired. Lower stakes than the three above (it is
    triage assistance, not a merge or push gate), but it is the last core script whose
    verdict a stale checkout can silently change.
  - **The lint gates** — covered *indirectly* and probably sufficiently:
    `pre-ship.sh`'s declared set already fingerprints
    `agents/scripts/project/test-lint-rules.sh` plus `lint-rules.d/*.sh`, so a stale
    rule module is reported at the push gate, which is where it would do harm. A
    direct call inside `test-lint-rules.sh` would additionally cover invoking it
    standalone — worth it only if that turns out to be a common path.

  Concrete next action: add the two-line `warn_if_script_stale` call to
  `issue-sweep.sh` over its own declared set (itself + the helper), following the
  `postmortem-owed.sh` shape. Then decide, from actual usage, whether
  `test-lint-rules.sh` needs its own direct call or whether the `pre-ship.sh` coverage
  is enough — do not add it reflexively; every call site pays a bounded `git fetch`
  (~0.8s measured), which is immaterial for a push gate and less obviously so for
  something invoked in a loop.

  **Applied 2026-08-11.** `issue-sweep.sh` is wired: it resolves its own directory
  before the `cd` (so the lib is locatable when invoked outside a checkout), sources
  the helper, and calls `warn_if_script_stale` LAST — after both the Issue verdicts
  and the out-of-band-label strip, since a stale checkout can change either. Missing
  lib degrades to silence rather than an error. Three tests in
  [`issue_sweep.bats`](../../../agent-layer/tests/bats/issue_sweep.bats) pin the wiring by
  running a copy of the script beside a stub lib, so they assert the call actually
  fires with the real declared set rather than grepping the source; removing the call
  fails two of them.

  **`test-lint-rules.sh` deliberately NOT wired**, and the reason is stronger than
  "not worth it". Its two real invocation paths are `pre-ship.sh` — which already
  fingerprints it plus `lint-rules.d/*.sh` in its own declared set, at the push gate
  where staleness would do harm — and CI, where the warning would be actively WRONG:
  CI checks out the PR head on purpose, so a freshness-vs-develop note would fire on
  every PR that touches a lint rule and train readers to ignore it. Standalone
  invocation is not a common path. Re-open only if that changes.

  Status: applied
  Last-reviewed: 2026-08-11

- 2026-08-11 · claude-code · [tooling] · P2 — the documented archival command re-parents a per-entry file one directory UP, silently breaking every relative link in it; the docs gate catches it only after the fact, and only if someone runs it

  Details: [`AGENT_SELF_IMPROVEMENT.md`](../../../agent-layer/docs/self-improvement/AGENT_SELF_IMPROVEMENT.md) § workflow
  step 4 prescribes archiving a per-entry file with

  ```
  cat docs/self-improvement/categories/<cat>/<file>.md >> docs/self-improvement/categories/applied.md
  git rm docs/self-improvement/categories/<cat>/<file>.md
  ```

  The entry was authored at `categories/<cat>/` depth; `applied.md` lives one level up
  at `categories/`. Every relative link in the body is therefore off by one directory
  the instant it is appended. Hit live archiving
  `2026-08-06-gate-tooling-run-from-stale-session-branch`: **7 dangling links** in one
  entry — `../../../../agents/...` (correct from `categories/process/`) resolved to
  `../agents/...` from `categories/`, `../../../agent-rules/...` to `agent-rules/...`,
  and a sibling-category link `../tooling/<slug>.md` to `docs/self-improvement/tooling/`.

  Two properties make this worse than a one-off:

  1. **It fails silently at the moment of the mistake.** `cat` cannot fail here. The
     only signal is `test-markdown-links.sh` reporting on `applied.md` later — and a
     PR that archives an entry may not otherwise touch a file that trips the docs gate
     locally, so the author's first notice can be CI.
  2. **It degrades the archive specifically.** `applied.md` is the durable record read
     months later; the whole value of an archived entry is that its cited paths still
     resolve. This mechanically guarantees the opposite for exactly the entries that
     carried the most cross-references.

  **The same command breaks links in the mirror direction too, and that half bites
  harder.** The `git rm` deletes a path other documents cite. Caught live in CI on this
  very branch (PR #1996, `Agentic self-tests (bats)`): archiving
  `2026-08-06-gate-tooling-run-from-stale-session-branch` left **2 inbound dangling
  links** — from [`postmortems.md`](../postmortems.md) and from the sibling entry
  [`2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](applied.md),
  both of which had correctly linked a file that existed when they were written.

  Inbound is the worse half for two reasons. The outbound breakage is confined to the
  archived entry and is repairable by re-depthing, mechanically, inside one file. The
  inbound breakage is scattered across files the archiver never opened, has no
  mechanical fix — `applied.md` is a 3000-line append-only ledger with no per-entry
  anchors, so there is no equivalent link to rewrite *to*, only prose to restate — and
  it is invisible to any check scoped to the diff, because the referring files are
  unmodified. Only a repo-wide `--all` sweep sees it. That is exactly why this surfaced
  as a red required check rather than as a local pre-ship failure.

  The 550+ legacy entries already in `applied.md` were largely moved from monolith
  `categories/<cat>.md` files, which sit at the SAME depth as `applied.md` — so the
  bug is new-ish, arriving with the one-file-per-entry convention, and will recur on
  every per-entry archival from here.

  Concrete next action: replace the raw `cat` in the § workflow step with a small
  `archive-backlog-entry.sh` that appends AND re-depths — mechanically, `../../../../`
  → `../../../` and `../../../` → `../../` for links leaving `docs/`, and
  `../<sibling-cat>/` → `<sibling-cat>/` — then `git rm`s the source and re-runs
  `test-markdown-links.sh` as a self-check. A script is the right shape rather than a
  documented sed: the transform depends on the source entry's depth, which is exactly
  the detail a human copying a command from a doc will not re-derive. The same script
  must also handle the inbound half **before** the `git rm`: grep the repo for the
  entry's slug and rewrite each referring link to prose naming the entry plus a link to
  `applied.md`, refusing to delete while any inbound reference remains. Until it exists,
  the doc should at minimum say "re-run `test-markdown-links.sh --all` after archiving" —
  that one line would have caught both halves, and the `--all` is load-bearing: default
  mode is diff-scoped and sees neither the re-parented body nor the orphaned referrers.

  **Applied 2026-08-11.** [`archive-backlog-entry.sh`](../../../agent-layer/agents/scripts/core/archive-backlog-entry.sh)
  replaces the raw recipe, and § workflow step 4 now calls it and says explicitly
  not to hand-roll the `cat`. Two things came out different from the plan above:

  - **The outbound transform is one rule, not a table.** The entry enumerated the
    rewrites by hand (`../../../../` → `../../../`, `../<cat>/` → `<cat>/`). Those
    are all instances of: a link `X` written at `categories/<cat>/` resolves to
    `categories/<cat>/X`, so from `applied.md`'s home the equivalent is
    `normalize("<cat>/" + X)`. Implementing the rule rather than the table also
    covers same-dir links and stays correct if the convention ever nests deeper.
  - **Prose mentions are advisory, not blocking.** The entry proposed refusing to
    delete while any inbound reference remains. That is right for markdown links
    (which dangle) but wrong for code spans and bare-path prose, which the link
    gate does not follow — blocking on those would make some entries unarchivable
    for a cosmetic reason. Links are repointed at `applied.md` at each referrer's
    own depth; prose mentions are reported as WARN for a human to restate.

  Nineteen tests in [`archive_backlog_entry.bats`](../../../agent-layer/tests/bats/archive_backlog_entry.bats)
  drive a throwaway repo laid out like the real docs tree, and assert on the
  RESULTING LINK TARGETS rather than exit status — reverting the fix fails 8 of
  them. One test exists only because the first real run hit it: `git rm` refuses a
  file with local modifications, which is the *normal* case here (you flip Status
  to `applied` and archive in the same session), and it refused AFTER the append
  had landed — leaving the tree half-archived, where a retry would double-append.
  The removal is now forced (the working-tree body is what was preserved) and the
  pre-flight happens before the first write.

  Status: applied
  Last-reviewed: 2026-08-11

- 2026-08-11 · claude-code · [tooling] · P2 — `required-absent` judges every historical merge against TODAY's required-context set, so promoting a context retroactively flags PRs that merged before it existed and can hard-fail `--blocking` at SessionStart

  Details: `postmortem-owed.sh` reads the required set once
  (`REQ_CTX_JSON`, from `project.config.json`) and applies it uniformly to every PR
  in the scan window. A context that became required *after* a PR merged is absent
  from that PR's rollup **by design** — nothing was wrong with the merge — but the
  cross-check cannot tell that apart from a genuine escape and reports
  `required-absent`.

  Consequence: the first sweep after any required-context promotion or rename flags
  up to `SCAN_N` historical PRs at once. With `POSTMORTEM_BLOCKING_GRACE=0` that
  hard-fails `--blocking`, which runs at SessionStart — so a routine branch-protection
  change can wedge every new session until someone notices the flags are phantom.

  Found by an explicit self-review of the diff in PR #1996 and independently
  confirmed by CodeRabbit on the same PR. Both landed on the same two candidate
  fixes:

  - **Record the required set at merge time.** Most correct, most work: the sweep
    would need a per-PR snapshot of what was required when it merged, which nothing
    currently persists.
  - **Apply each context only from its effective date.** Cheaper and self-contained:
    derive a per-context "earliest observed present" from the scan window itself and
    skip any PR merged before it. Needs the row stream buffered — the loop is
    currently single-pass over a process substitution — so it is a real restructure
    of `postmortem-owed.sh:573-719`, not a patch.

  Deliberately **not** fixed in #1996. It fails LOUD and rarely (only on a
  required-set change), which is the opposite of the silent false-green class that
  PR exists to close; picking between the two designs is a judgement call rather
  than a defect fix, and doing it badly would put noise into the one gate that is
  supposed to be trustworthy. The sibling defect on the same detector — a POST-merge
  re-run reading as `required-never-terminal` — WAS fixed there, because it fires on
  ordinary PRs and the fix is local (ignore runs that started after `mergedAt`).

  Escape hatch until fixed: `POSTMORTEM_ABSENT_GRACE_SECONDS` does not help (the
  merges are old, so the grace has long elapsed). Either raise
  `POSTMORTEM_BLOCKING_GRACE` or narrow `SCAN_N` past the promotion date for one
  sweep.

  **Applied 2026-08-11**, via the second (cheaper) option. The row stream is now
  buffered into `ROWS` before judging — the loop was single-pass over a process
  substitution, which is why this needed a restructure rather than a patch — and a
  first pass derives `REQ_FIRST_SEEN[ctx]`, the earliest merge in the window on
  which each required context was observed PRESENT. A PR that merged before its
  first observation is not judged for that context.

  Two cases the original write-up did not anticipate, both found by running the
  existing suite against the change rather than by re-reading the plan:

  - **A context observed on NO PR in the window is undatable**, and the two
    explanations point opposite ways: a promotion so recent nothing has run it yet
    (benign), or a context that never reports at all (the #1941 shape, and
    serious). They are genuinely indistinguishable from the window alone. Flagging
    per-PR would rebuild the exact wedge this fixes, so it is reported ONCE as a
    WARN naming the context and the ambiguity. Signal preserved, wedge removed —
    and critically it does not hard-fail `--blocking`.
  - **An EMPTY rollup must stay exempt from the dating entirely.** The first
    implementation suppressed it, which broke the #1941 regression test — correctly.
    "Nothing reported at all" cannot be explained by a late promotion: had the
    context merely been added since, the OTHER required contexts would still be in
    the rollup. Empty rollups (#1941, #1972-#1974) now bypass the effective-date
    gate and always flag.

  Two existing tests changed shape rather than intent: their single-PR fixtures had
  the missing context observed nowhere, so under the new rule they were undatable.
  Both gained an earlier corroborating PR carrying the context — which is what a
  real window looks like, since a required context reports on many PRs — and still
  assert the escape is caught. Five new cases cover the boundary in both directions,
  the whole-window promotion wedge, the once-only undatable report, and the
  empty-rollup exemption; reverting the gate fails two of them.

  The other candidate — persisting the required set at merge time — remains the
  more correct fix and is still unbuilt. This one is derivable from data already
  fetched, which is why it went first.

  Status: applied
  Last-reviewed: 2026-08-11

- 2026-08-11 · claude-code · [process] · P2 — the `[pre-first-push gate]`'s self-review step is the only one with no backstop, so skipping it is invisible; skipped on PR #1996 and it cost ~8 CodeRabbit cycles, both bots' rate limits, and four locally-knowable defects reaching CI

  Details: [`ship-loops.md`](../../../agent-layer/docs/agent-rules/ship-loops.md) § `[pre-first-push gate]`
  makes a local self-review mandatory before the first push, "never deferring
  locally-knowable findings to CI/CR", and the
  [`adversarial-code-review`](../../../agent-layer/agents/_shared/skills/adversarial-code-review/SKILL.md)
  skill says to use it "proactively before opening a PR". On PR #1996 the gate's
  other steps were either run or genuinely n/a (no strict-zone C++ touched, so
  the dual-target `/WX` build, `ctest`, and the leaf-`AGENTS.md` self-review did
  not apply). **The review step was simply not run.** It happened 14 commits
  later, only because the user asked "have you code reviewed the changes?".

  **Measured cost on that one PR**, all of it the churn `reduce-coderabbit-review-spend`
  Slice 1 exists to prevent:

  - ~8 completed CodeRabbit review cycles across the PR's 16 commits.
  - CodeRabbit's adaptive per-developer limit hit repeatedly (27–49 min waits,
    three `@coderabbitai review` requests answered only by the plan/rate-limit
    note). The rate-limiting that dominated the session was substantially
    self-inflicted by this PR's own churn.
  - Cursor Bugbot's usage cap hit on at least three separate heads.
  - **Four locally-knowable defects reached CI/CR** that the review found the
    moment it finally ran: the develop-side half-set glob enumeration, the
    silently-dead grace cutoff, and two docs this PR itself falsified. None
    needed CI, a reviewer, or a running gate to find — only reading the diff.
    (A fifth of the same class, the CWD-relative glob expansion, was caught by
    CodeRabbit as a *trivial* nitpick and escalated on inspection. A pre-push
    review would plausibly have found it too, but the credit is CodeRabbit's,
    not the self-review's — and this entry got that attribution wrong on its
    first draft, caught only by re-checking before merge.)

  **The structural point, which outlives this PR.** The automatic backstop
  (`scripts/git-hooks/pre-push` step D) mirrors the *mechanical* required checks
  — lint rules, doc anchors, markdown links, portable purity, clang-format,
  shell-lint. It cannot mirror a judgement step. So of the four gate items, three
  either self-enforce or are visibly n/a, and the fourth **leaves no trace either
  way**: a PR whose review was skipped and a PR whose review was clean look
  identical from outside.

  That is precisely the `required-check-that-never-reports-is-invisible` shape
  the same PR was fixing in CI — a check that produces no signal reads as a pass.
  Worth noting the gate was skipped *by the session working on that entry*, which
  suggests the failure is structural rather than a lapse of attention.

  Candidate fixes, cheapest first:

  - **Record the verdict.** Require the PR body's test plan to carry a line for
    the pre-first-push review (e.g. `adversarial-code-review: N findings, all
    fixed` or an explicit `n/a — trivial diff`). Makes absence visible without
    enforcing anything; the `Intent section` workflow already parses the body, so
    there is a home for the assertion.
  - **Cite it like gate evidence.** PR #1996 added the rule that gate-tool output
    must record the tree + commit it ran from. The same discipline applied here
    would make "the review ran, against this diff" checkable rather than assumed.
  - **Do not** try to enforce it in the pre-push hook. The step is a judgement
    call; a hook can confirm a claim was made, never that the review was real.
    An enforcement that can only check the claim would manufacture exactly the
    kind of green this backlog keeps filing entries about.

- 2026-08-11 · claude-code · [tooling] · P1 — `test-gate-selftests --check` proves a `# selftest: asserts-failure` marker EXISTS, not that the negative under it can ever fail; `test-plan-index.sh`'s negative had been satisfied by a `Permission denied` for its whole life, and four more vacuous negatives were written in the two sessions that touched this area

  Details: [`test-gate-selftests.sh`](../../../agent-layer/agents/scripts/core/test-gate-selftests.sh)
  enforces that every `--selftest`-exposing gate script carries a negative
  assertion, marked `# selftest: asserts-failure`. That is the right gate to
  have — it closed a real gap. But what it can check is the presence of a marker
  and some negative-looking code near it. It cannot check the property that
  matters: **that the negative actually fails when the behaviour it names is
  removed.**

  **The live instance.** [`test-plan-index.sh`](../../../agent-layer/agents/scripts/core/test-plan-index.sh)
  case (3) fed a non-existent archive dir and required a non-zero exit. Two
  independent reasons it could not fail:

  1. It invoked `"$_SCRIPT_PATH" --check` — executing the script directly. The
     file is mode **100644** in git (`git ls-files -s` confirms), so that exec
     fails with **126 Permission denied** on every machine, including CI. The
     assertion was satisfied by the permission error, never reaching the
     archive-dir guard at all.
  2. Even via `bash`, it accepted ANY non-zero status. Deleting the guard leaves
     the script exiting non-zero on an unhandled `os.listdir` traceback — so the
     assertion stayed green with the behaviour it names entirely removed.

  Verified both ways: with the archive-dir guard deleted, the selftest reported
  PASS before the fix and FAIL after it.

  **This is a class, not an instance, and the evidence is uncomfortable.** In the
  same two sessions, *four more* vacuous negatives were written — by the session
  fixing this very family of bugs:

  - `archive-backlog-entry.sh`'s first negative assertions (three of them) each
    required only a non-zero exit; all three still passed with their own guard
    deleted, because the script dies downstream for an unrelated reason.
  - The `plan-date` marker selftest wrapped its assertions in `( set -e … ) || {…}`.
    `set -e` is **suppressed inside a subshell that is the left operand of `||`**
    — the shell disables it for any command in a `&&`/`||` list — so every step
    ran regardless of failure and only the last command's status was reported. It
    passed with the fix disabled.

  Five vacuous negatives, none of which a reviewer or a marker-checking gate
  caught. Every one was found the same way: **delete the code under the assertion
  and re-run.** That is the only check that distinguishes a test from a comment.

  Two recurring mechanical causes worth naming, because both are invisible on
  inspection:

  - **Accepting a bare non-zero status.** Any sufficiently broken program exits
    non-zero. A negative must assert the *reason* — the refusal message, the
    specific exit code — or it is satisfied by crashes, missing interpreters,
    permission errors and typos in the test itself.
  - **`set -e` inside a `&&`/`||` operand.** Already documented in
    [`script-freshness.sh`](../../../agent-layer/agents/scripts/core/lib/script-freshness.sh)
    for the callee side; the *test* side has the same trap and no note anywhere.

  Concrete next action, cheapest first:

  1. **Make `test-gate-selftests --check` reject self-exec.** A `--selftest` that
     re-invokes its own script must do it via `bash "$path"`, never `"$path"`,
     since every gate script in this repo is mode 100644. This is a grep, it is
     exact, and it would have caught the live instance. Check the other 77
     scripts for the same shape while adding it.
  2. **Require negatives to assert a reason.** Flag an `asserts-failure` block
     whose only assertion is a bare status test (`if ! cmd; then`/`|| fail=1`)
     with no message/exit-code comparison anywhere in the block. Necessarily
     heuristic, so WARN rather than block, and cite this entry in the message.
  3. **Do NOT try to prove reachability mechanically.** Confirming a negative can
     fail means mutating the subject and re-running — that is mutation testing,
     and building it here would cost far more than it returns. The durable fix is
     the authoring rule (delete the code, re-run, watch it go red), which belongs
     in the review checklist rather than in a gate. A gate that pretended to
     verify reachability would itself be the false green this entry is about.

  Outcome (2026-08-12): action 1 shipped — `test-gate-selftests --check` now
  blocks raw self-exec of `$_SCRIPT_PATH`/`$SCRIPT_PATH`/`$0` (comment lines
  excluded; `bash|sh|.`-prefixed forms pass), with selftest coverage for both
  the flagged and legitimate shapes; the sweep of the other scripts found no
  further live instances. Action 2 was attempted and measured out: a
  bare-status-negative heuristic over the 79 exposers produced ~24 false
  positives (sampled files all assert reasons via shapes no enumeration
  catches — captured output matched later, python `assertIn`, refusal tokens
  grepped far from the status test). A WARN wrong ~30% of the time is the
  wolf-cry this backlog already documents, so it was removed rather than
  shipped — which is action 3's own reasoning applied one rung down. Action 3
  remains "do not build" by design; the durable rule (delete the code under
  the negative, watch it go red) lives in the review checklist and was applied
  to every assertion in this batch.

  Status: applied
  Last-reviewed: 2026-08-12

- 2026-08-11 · claude-code · [tooling] · P2 — a required context that stops reporting on EVERY PR is now only a WARN, so `postmortem-owed.sh --blocking` exits 0 on it; the effective-date fix traded this away to stop a SessionStart wedge, and only a merge-time snapshot buys it back

  Details: the effective-date fix for
  [`required-absent-judges-history-by-todays-required-set`](applied.md) (applied
  2026-08-11) dates each required context from the earliest merge in the scan window
  where it was observed PRESENT, and skips PRs that merged before that. A context
  observed on **no** PR in the window cannot be dated at all, and the two
  explanations point opposite ways:

  - it was promoted so recently that nothing has run it yet — benign; or
  - it never reports at all — the #1941 shape, and one of the most serious escapes
    this detector exists to catch.

  From the window alone these are **indistinguishable**: both produce exactly "the
  name is in `required_contexts` and appears in zero rollups". So the undatable case
  is reported once as a `warns` line naming the ambiguity, and `warns` never affects
  the exit code.

  **The cost, stated plainly.** Before the fix, a required workflow that silently
  stopped reporting would flag every PR in the window and hard-fail `--blocking`.
  After it, that same outage produces one advisory line and a green `--blocking`.
  That is a real reduction in detection strength on the detector's headline case,
  and it was accepted only because the alternative re-creates the wedge the fix
  exists to remove: with `POSTMORTEM_BLOCKING_GRACE=0`, flagging per-PR means a
  routine branch-protection change hard-fails `--blocking` at SessionStart for up to
  `SCAN_N` PRs at once, wedging every new session on phantom escapes.

  Two mitigations already limit the blast radius, which is why this is P2 and not P1:

  - An **empty rollup** is exempt from the dating entirely and still flags. The
    reported escapes (#1941, #1972-#1974) are all empty-rollup merges, so the
    historical cases remain caught. The gap is narrower than "absence is unchecked":
    it is specifically *one* context missing from otherwise-populated rollups, across
    the whole window.
  - The WARN is emitted on every sweep, so the signal is never lost — only
    downgraded from blocking to advisory.

  Found by the pre-first-push adversarial review of the fix itself, which correctly
  called it out as "deliberate and documented, but the headline case of the detector
  added in the immediately preceding commit". Recording it rather than leaving the
  reasoning only in a code comment, because a deliberate trade-off that lives only in
  a comment is indistinguishable from an oversight six months later.

  Concrete next action: the fix is the OTHER candidate from the original entry —
  **persist the required set at merge time**. The merge-snapshot ledger
  (`merge-snapshots.jsonl`) already writes a per-merge record and is the natural
  home: add the branch-protection required-context list to the snapshot the watcher
  captures. Then "was this context required when this PR merged?" is a lookup rather
  than an inference, the effective-date heuristic and the undatable case both
  disappear, and a context that stops reporting can be flagged per-PR again without
  any risk of the promotion wedge. Note this only helps merges made AFTER the
  snapshot gains the field, so the window-derived dating has to stay as the fallback
  for older merges — the two coexist rather than one replacing the other.

  Update (2026-08-13): shipped as proposed. `merge-snapshot-append.sh` now
  persists the branch-protection required set at the decision instant
  (`requiredContexts`, schema 2; self-derived from `project.config.json`
  § branch_protection — the file `setup-branch-protection.sh` applies, so it is
  the set in force — with a `SNAPSHOT_REQUIRED_CONTEXTS` test seam; a capture-
  time config miss records `[]`, which readers treat as "no merge-time set").
  `postmortem-owed.sh` prefers the snapshot's merge-time set per merge: "was
  this context required when this PR merged?" is a lookup, so the undatable
  ambiguity disappears for snapshotted merges — a never-reporting required
  context flags per-PR and hard-fails `--blocking` again, with zero
  promotion-wedge risk (a context absent from the merge-time set is simply not
  judged). The effective-date heuristic and the once-per-window WARN remain the
  fallback for schema-1 history, exactly as the entry predicted the two would
  coexist. The creation-lag grace still defers fresh merges. Six bats cases
  (headline flag / not-required-then / schema-1 fallback / recorded-[] fallback
  / grace deferral / --blocking exit 1) plus writer selftest coverage.

  Status: applied
  Last-reviewed: 2026-08-13

- 2026-08-07 · claude-code · [tooling] · P1 — `postmortem-owed.sh`'s first dedup probe matches `PR #N` **anywhere** in the ledger, so a prose mention of a PR inside an unrelated entry permanently suppresses that PR's own gate-escape nudge

  Observed after PR #1962 merged with `cr-out-of-band` +
  `cr-disposition:cr-rate-limited` — a real override escape (CodeRabbit's
  account quota was exhausted, so the diff was never reviewed). The escape owes
  a postmortem by
  [`AGENTS.md` § Self-improvement loop](../../../AGENTS.md), but
  `bash agents/scripts/core/postmortem-owed.sh --list` reports
  `no gate escapes owed a postmortem (last 20 merges clean)`. Four consecutive
  invocations agree, so this is a deterministic miss, not a flake.

  Mechanism. `has_entry()`
  ([`postmortem-owed.sh:240-251`](../../../agent-layer/agents/scripts/core/postmortem-owed.sh))
  runs two probes and the trigger-1 loop skips the PR when either fires:

  ```bash
  grep -qE "PR #$1([^0-9]|$)|commit $1([^0-9A-Fa-f]|$)" "$LEDGER" && return 0
  grep -qE "^#+ .*PR #[0-9].*[,[:space:]/]#$1([^0-9]|$)" "$LEDGER"
  ```

  The **second** probe is correctly scoped to a heading line (`^#+ …`) — its
  comment even states the intent: *"scoped to `^#+ …` so a #N mention in prose
  body can't false-suppress a real owe"*. The **first** probe is not scoped at
  all. It scans the whole file, so any sentence anywhere that happens to write
  `PR #1962` satisfies it. Measured against `origin/develop`'s ledger:

  ```
  grep -cE "PR #1962([^0-9]|$)"        → 2     # prose, inside an unrelated entry
  grep -cE "^#+ .*PR #1962([^0-9]|$)"  → 0     # no entry is actually filed
  ```

  Both hits are body prose in the 2026-08-05 `#1937` postmortem
  ([`postmortems.md:2171,2184`](../postmortems.md)) — written by the very
  work that produced #1962, citing it as the determinism fix its instance
  ratchet rests on. Citing a PR is the normal way these entries are written, so
  the failure mode is not exotic: **any PR named in an existing entry's prose is
  silently exempted from ever being nudged again.** The suppression is
  permanent and silent — the detector's output is indistinguishable from a
  genuinely clean window, which is the same "mask discards the verdict" shape as
  [`2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md`](tooling/2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md).

  Blast radius: this is the *detector for gate escapes*. A hole here doesn't
  leak one defect, it suppresses the mechanism that converts escapes into new
  gates. Entries accumulate cross-references over time, so the exempted set only
  grows.

  Proposed gate — **dedup on a structured field, never on free prose.**

  1. **Scope probe 1 the same way probe 2 already is.** Require the `PR #N`
     match to land on an entry heading (`^#+ .*PR #N([^0-9]|$)`), matching the
     documented entry shape `## <date> · PR #N[, #M …] · <trigger>`. This is a
     one-line change and immediately un-suppresses #1962. The `commit <sha>`
     alternation should be split out and kept whole-file — `has_sha_entry`
     already documents bare-sha matching as deliberate for triggers 3+4.
  2. **Add a bats regression case.** `tests/bats/` should assert that a ledger
     containing only a *prose* `PR #N` mention still reports #N as owed, and
     that a real `## … PR #N …` heading dedupes it. This is the property both
     probes are trying to express and neither one tests.
  3. **Consider a machine-readable key.** Longer-term, have each entry carry an
     explicit `### Escaped PRs: #A, #B` field and dedup on that alone, so
     heading prose style can drift without re-opening the hole. Optional — (1)
     plus (2) closes the class.

  Item 1 is the fix; item 2 is what keeps it fixed. Do not apply from here —
  suggestion-only per the skill's finder/applier split.

  **Applied 2026-08-11** (items 1 + 2; item 3 deliberately not taken — (1)+(2)
  close the class, as the entry itself says). Both probes are now heading-scoped,
  and the symmetry is the fix.

  Two refinements the write-up did not have:

  - **The `commit <sha>` alternation was removed, not split out.** The entry
    proposed keeping it whole-file. In fact `has_entry` is only ever called with
    a PR NUMBER (both call sites pass one; the sha paths use `has_sha_entry`), so
    that branch was matching the literal string `commit <pr-number>` — which
    occurs **0 times** in the ledger, measured. It guarded a caller shape that
    does not exist. `has_entry` now documents that it takes a PR number and that
    sha dedup belongs to `has_sha_entry`.
  - **Scoping was checked against the real ledger before landing**, since the
    opposite error — re-nudging a PR that IS postmortemed — would be noisier than
    the bug. 12 PR numbers were deduped by prose alone; all 12 are plain
    citations ("shipped in PR #1953", "concurrently by PR #1078"), none an entry
    of its own. Every PR-keyed heading uses the documented
    `## <date> · PR #N[, #M …] · <trigger>` shape, so heading-scoping loses no
    real entry. The one heading carrying a bare `#N` without the `PR ` prefix is
    an Issue reference, never in scope here. #1962, the reported instance, has
    since been given a real entry and still dedupes correctly.

  Four bats cases pin the property both probes were expressing and neither
  tested: prose does not dedupe, a documented heading does, prose cannot satisfy
  the combined-heading probe either, and a substring PR number does not
  cross-suppress. Restoring the unscoped probe fails the first.

  Secondary observation, low confidence, recorded rather than actioned: one
  earlier `--list` invocation in the same session emitted six owed escapes
  (#1979, #1974, #1971, #1964, #1968, #1954) while six others reported clean.
  Re-running four times after the fact was stable-clean, and the underlying
  `gh pr list` query returned 20 rows on three consecutive checks, so the
  transient was not a `gh` failure. The trigger-1 loop ends in
  `2>/dev/null || true`, which would turn any upstream failure into a silent
  "clean" — worth a `set -o pipefail` + explicit row-count assertion if it
  recurs, but it is not reproducible today and is **not** the cause of the
  #1962 miss (that one is fully explained above).

  Status: applied
  Last-reviewed: 2026-08-11

- 2026-08-07 · claude-code · [process] · P1 — when review finds one fabricated verbatim quote, re-verify **every** quote in the changed doc; fixing only the flagged one leaves the class alive

  Twice in one session I wrote a fenced code block that quoted code existing nowhere in the
  tree. The first was caught as a Critical, and I fixed *that block*. The replacement text I
  wrote in the same edit contained a second fabricated block, caught as a Critical by the next
  review pass. Correcting the instance did nothing about the habit that produced it.

  The mechanism is specific and worth naming: a verbatim block reconstructed from memory of
  reading the file — rather than pasted from a fresh read — is plausible by construction. It
  uses the right identifiers in the right shape, so it survives every check except resolving it
  against the file. Reviewer attention and my own re-reading both slide over it.

  Rule: a finding of the form "this quote does not exist" is a **class** finding. Its fix is not
  the corrected quote — it is a sweep of every fenced block and every `file:line` citation in
  every file the diff touches, each one re-resolved by an actual read at the cited line. Cheap:
  a handful of `sed -n '<a>,<b>p'` calls. The cost of skipping it is a second Critical on the
  fix commit, which is what happened here.

  Then the sweep has to go one step further, because a third review pass on this same diff found
  a Critical the rule as stated above would have **missed**: a claim that five call sites "can
  still mint an orphan root node on a dead id" when all five already guard against exactly that.
  Every citation in that paragraph was correct; the *characterization* of what the cited code
  does was wrong — and the claim carried no line number at all, which is what let it through.
  So the sweep covers **every claim about what cited code does**, verified by reading the
  enclosing function rather than the cited line. A citation-shaped assertion with no citation is
  the highest-risk case, not the lowest.

  Corollary for authoring, not just for repair: quotes go into a doc by paste from a read
  performed for that purpose. If the block was typed rather than pasted, treat it as unverified
  until resolved, however confident it looks.

  Belongs in [`docs/agent-rules/process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md)
  § Cadence and verification, alongside the existing stale-`Edit` recovery rule — same shape
  (a stale mental model of a file standing in for the file).

- 2026-08-07 · claude-code · [process] · P2 — a background-task completion notification's "exit code 0" is the **pipeline's** exit, not the gate's; always read the in-file `*_EXIT=` value

  Hit twice this session. A gate run in the background as

  ```bash
  bash agents/scripts/project/test-lint-rules.sh --diff origin/develop 2>&1 | tee out.txt
  ```

  completes with a notification reading `exit code 0` **regardless of the gate's verdict**,
  because the reported status is the pipeline's, and the pipeline ends in `tee`. The same
  trap bites the interactive form: `echo "$?"` after a pipe reports the **last** element's
  exit — use `${PIPESTATUS[0]}`.

  The concrete near-miss: two wrong bucket-E invocations
  (`--target SmatchetUiTests` → `ninja: error: unknown target`, and
  `bash scripts/dev/test-ui.sh` → `No such file or directory`) both surfaced as exit 0 and
  were nearly recorded as passing runs. They were only caught by reading the output file,
  which contained the errors in plain text.

  Two fixes, both cheap:

  1. **Convention** — every gate wrapper this repo runs in the background must end with an
     explicit `echo "<NAME>_EXIT=${PIPESTATUS[0]}"` appended to the same output file, and
     the reader must grep for that token rather than trusting the notification. This is an
     ad-hoc habit today, not a repo convention — agents invent a token per run (`LINT_EXIT=`,
     `DOCS_EXIT=`, `FMT=`, `BUILD_*_DONE`) and no gate wrapper in `scripts/` emits a verdict
     token into its own output. (`scripts/dev/local/test-build-wrapper.sh:186` sets
     `SMATCHET_TEST_WITHMSVC_EXIT=` — an env var handed to a subprocess, not a verdict written
     where a reader will look for it.) Naming the convention and writing it down is the whole
     proposal.
  2. **Doc** — add the rule to [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md)
     § Cadence and verification, next to the existing note that `tail -N` can truncate a
     gate's verdict off the head of its output.

  Generalised: **a notification is a liveness signal, not a verdict.** Any claim that a
  gate passed must cite a line from the gate's own output.

- 2026-08-07 · claude-code · [process] · P2 — `code-review` told "review the staged diff" silently skips working-tree hunks on `MM` paths; it must report the split before reviewing

  Concrete miss: the worktree A review of [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966)
  was scoped to `git diff --cached`, but `Source/Core/src/Ui/SmatchetWindowExpand.cpp` was
  `MM` — the most substantive hunk under discussion (the `PushStyleColor` array/loop refactor
  the requester explicitly asked to have its push/pop balance verified) existed **only in the
  working tree**. The agent caught it, but only because it happened to run `git status`; a
  reviewer that goes straight to `git diff --cached` reviews code the requester is not
  looking at, and reports green on a file whose real content it never read.

  The requester's mental model of "what I'm about to commit" is wrong precisely on `MM`
  files — that is what `MM` means.

  Fix, in [`agents/core/code-review.md`](../../../agent-layer/agents/core/code-review.md): make step 1
  run `git status --short` and, for **any** `MM` path in scope, state the staged/unstaged
  split up front and ask which one is under review (default: review the **working tree**,
  since that is what will be built and tested). Cheap — one command — and it converts a
  silent scope hole into an explicit question.

- 2026-08-07 · claude-code · [process] · P2 — a doc-correction sweep must grep the *distinguishing phrase*, not the subsystem name, or it silently misses files

  Correcting a stale claim across the docs tree ("the duplication gate is WARN-first" → "it is
  blocking") I swept for the subsystem tokens — `duplication`, `dup_audit` — and declared the
  drift bounded to two files. A reviewer then found a third, [`docs/CONTEXT.md`](../../CONTEXT.md),
  which states the claim without naming the gate at all: it says only *"DRY is WARN-first today
  per ADR-0015"*. The subsystem grep could not have found it.

  The rule: sweep on the phrase that makes the claim **wrong** (`WARN-first`), not on the thing
  the claim is **about**. The wrong phrase is what needs to change, so it is the complete
  enumerator by construction; the subsystem name is only a proxy, and any doc that refers to
  the subsystem obliquely escapes it.

  Cost is real but bounded: `WARN-first` matches roughly **100 lines across 40 markdown files**
  on this worktree, most legitimately describing *other* gates still in calibration. Triage is a
  scan, not a rewrite, and it is the price of the sweep being complete rather than plausible.
  (Approximate deliberately: this entry contains the phrase several times, so an exact count
  self-invalidates on its own next revision — a hazard for any doc that counts a token it uses.)

  Two scoping notes learned by running it: frozen docs (`docs/plans/shipped/**`, `evaluation/**`)
  legitimately record what was true when written and should be **excluded by default** rather
  than "fixed" — a shipped plan describing the gate as WARN-first at the time is accurate
  history. And a stale-claim sweep is a *whole-file* scan, so a hit inside a fenced code block
  or a quoted historical excerpt is a false positive to skip, not a line to edit.

  Belongs as a line in [`docs/agent-rules/process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md)
  § Cadence and verification, next to the existing "use `test-markdown-links.sh` as the enumerator, not grep"
  note — same failure shape: a hand-rolled proxy standing in for a complete enumerator.

- 2026-08-07 · claude-code · [process] · P2 — a backlog entry proposing a gate must name the concrete symbol the gate enumerates, and be checked against the bug that motivated it

  I proposed a gate to catch dock-node-id constants that name no real slot, and specified it as
  "enumerate `SmatchetDockNodeIds::kEntries`". That table lives in an anonymous namespace in the
  `.cpp` (so the qualified name is not even addressable) and maps layout keys to only three
  slots; `kSecondarySideBar` — the exact constant the gate exists to catch — never appears in
  it. The gate as written would have stayed green through its own motivating bug.

  The proposal read as concrete because it named a real symbol. Naming a symbol is necessary but
  not sufficient; the symbol has to be the one that actually enumerates the population.

  Two checks, both mechanical, both cheap enough to be unconditional:

  1. **Name the enumerator explicitly** — the file and the declaration the gate iterates, not a
     prose description of the population ("every dock id"). A prose population cannot be wrong,
     which is precisely why it hides this failure.
  2. **Replay the motivating bug against it** — walk the proposed enumerator by hand and confirm
     the known-bad case appears in it. If the entry cannot point at the row the gate would have
     tripped on, the gate is not specified yet.

  Generalises past gates: the same check applies to any proposed automation described by the
  population it covers. "Assert every X" is only meaningful once X resolves to an enumerable
  declaration, and only correct once the known counterexample is shown to be inside it.

  Concrete instance and the corrected proposal:
  [`../debt/2026-08-07-dock-node-id-slot-liveness-followups.md`](debt/2026-08-07-dock-node-id-slot-liveness-followups.md).

- 2026-08-07 · claude-code · [process] · P2 — add a `code-review` checklist line: an existence check on an ImGui / dock / handle id must also assert the **containment** relationship, because ids are recycled *and persisted*

  Caught as a High in the [PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984)
  review. The first cut of `EnsureDockSlotAlive` was:

  ```cpp
  return ImGui::DockBuilderGetNode(slot) != nullptr ? slot : 0;
  ```

  which looks obviously correct and is obviously wrong. `DockBuilderGetNode` is a flat
  `DockContextFindNodeByID` map lookup (`imgui.cpp:20701-20705`). The orphan root nodes the
  guard exists to reject are written to `imgui.ini` under `[Docking][Data]` and **reloaded
  next launch** — so for every user who already ran the buggy build, the lookup succeeds for
  exactly the ids that must fail. The guard would have been a no-op on the whole installed
  base while passing every fresh-profile test.

  The fix was one clause: also require `node->ParentNode != nullptr`, since every constant in
  `SmatchetDockNodeIds.h` names a node the default layout cuts as a *child* of the dockspace
  root, so a null parent means the id resolved to a detached node.

  Generalised checklist line, for the `code-review` subsystem-invariants section under `Ui/`:

  > An existence check on an ImGui id, dock node, or opaque handle is not a validity check.
  > Ids are hashes — recycled across sessions and, for dock nodes, **persisted to `imgui.ini`**.
  > A lookup that succeeds proves an object exists, not that it is the object you meant.
  > Require the structural relationship too (parent / root / owning container), and name in a
  > comment which relationship the constant is supposed to satisfy.

  Broader than docking: the same shape applies to `ImGuiID` window lookups (`FindWindowByName`
  finds a stale window from a previous layout) and to any `id -> object` map that outlives a
  session.

- 2026-08-07 · claude-code · [process] · P2 — when a claim reads "N sites do X, M do not-X" off a single grep, the two populations are usually **nested, not disjoint**; subtract before writing the numbers down

  Caught as a High on the [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966)
  plan-doc addendum, and traced back into the already-pushed
  [PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984) entry it summarised
  ([`categories/test/2026-08-07-booted-app-or-skip-fails-open.md`](test/2026-08-07-booted-app-or-skip-fails-open.md)).
  That entry stated the bucket-E guard "fails **open** at 56 call sites" and, four paragraphs
  later, that "**6** sites already fail **closed**". Both numbers came from the same
  `grep -c "AppController\* app = SmatchetActiveUiTestAppController();"` match set — the six
  fail-closed sites are *inside* the 56, because 56 counts the **assignment** shape, which every
  site shares regardless of what it does on the next line. The correct fail-open count is 50.
  The entry contradicted itself in its own text (6 + 56 ≠ 56) and neither I nor two earlier
  review passes read the two paragraphs against each other.

  This is a distinct failure mode from the fabricated-quote class
  ([`2026-08-07-fabricated-quote-is-a-class-not-an-instance.md`](applied.md)).
  There the citation was invented; here every grep was real, its output was pasted correctly,
  and the arithmetic was never done. A measured number with a wrong population reads exactly
  like a measured number with the right one — there is no surface tell, which is why it survived
  further than the fabricated quotes did.

  Two mechanical checks, both cheap:

  1. **Assert disjointness explicitly.** When one grep shape underlies both counts, the second
     population is a *filter* of the first. Either re-grep the complement (`grep -L`, or grep the
     shape and subtract the exception files' own counts) or state the relationship in the text —
     "50 of its 56" rather than "56 … and separately 6".
  2. **Read the paragraphs against each other before shipping.** The contradiction here was
     internal to one file and visible without leaving it. A doc that quotes two counts of the
     same population owes a sentence saying how they relate.

  Same shape, different unit, in the addendum that summarised it: a sentence whose subject was
  "commit A **and** commit B" carried counts derived from B alone. Check: when a claim names
  multiple commits, run the union — `git diff --name-only <first>~1 <last> -- <path>` — rather
  than reading one commit's `--stat`.

  Belongs in [`docs/agent-rules/process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md)
  § Cadence and verification, next to the other verify-the-claim-not-the-tool rules.

- 2026-08-07 · claude-code · [tooling] · P2 — `dup_audit.py` suppression is a **per-line** test, so a multi-line `SMATCHET_DEVIATION` comment whose last line is prose silently fails to suppress; neither `cpp-rules.md` nor the gate's own output says so

  Mechanics, from [`dup_audit.py:353-381`](../../../agent-layer/agents/scripts/core/dup_audit.py):
  `_has_dup_deviation(line)` requires the `SMATCHET_DEVIATION` token **on that one
  line**, with `duplication` among the comma-separated `rule=` ids. `_suppressed`
  then checks (a) the nearest **non-blank line immediately above** the clone start
  and (b) **any line within** `[start_line, end_line]`. `run_diff` wraps it as
  `any(_suppressed(...) for ... in c.locations)`, so a marker on **either**
  occurrence exempts the pair.

  Cost when this bites: a deviation written in the natural way —

  ```cpp
  // SMATCHET_DEVIATION(rule=duplication; reason=the MCP and Lua-console window-layout
  // helpers are long-standing structural twins; unifying would couple independent
  // subsystems; owner=ui-host; revisit=2026-12-31)
  ```

  — does not suppress, because the nearest line above the clone is line 3, which
  carries no token. The gate reports a bare `[dup] FAIL a.cpp:111 <-> b.cpp:74`
  with no hint that a marker was present-but-ineffective, so the reader's first
  instinct is that the exemption text is wrong rather than its *shape*. Cost one
  round of guessing before reading the script.

  Two fixes, independent:

  1. **Doc** — add to [`cpp-rules.md`](../../../agent-layer/docs/agent-rules/cpp-rules.md)
     § `SMATCHET_DEVIATION` grammar: *the whole `SMATCHET_DEVIATION(...)` must fit on
     a single line for the `duplication` rule; put explanatory prose on separate
     comment lines **above** it.* (`.clang-format` `ColumnLimit: 120` is the real
     constraint on how much reason text fits.)
  2. **Gate** — when a clone pair FAILs, scan a small window (say 5 lines) above the
     clone start for the literal string `SMATCHET_DEVIATION` and, if found without a
     matching single-line marker, emit
     `hint: a SMATCHET_DEVIATION comment is nearby but spans multiple lines — the marker must be on one line`.
     Turns a silent shape error into a self-explaining one.

  Related: the same file's `--diff` mode graduated `duplication` from WARN to
  **BLOCKING** on 2026-06-21, which `AGENTS.md` and `agents/core/code-review.md`
  still describe as WARN-first calibration — see the sibling entry.

- 2026-08-07 · claude-code · [tooling] · P2 — repo paths written as inline code spans in backlog entries are never checked, so a backlog entry can cite a file that does not exist

  [`agents/scripts/core/test-markdown-links.sh`](../../../agent-layer/agents/scripts/core/test-markdown-links.sh)
  resolves markdown **links** — its `LINK_RE` matches only the bracketed-label-then-parenthesised-
  href form. A repo path written as a bare code span
  — `` `scripts/dev/test-ui-window-expand.sh` `` — is invisible to it. In this session I wrote a
  [tooling] entry whose entire proposal was anchored on such a path, and the file does not exist
  on `develop` (it lives only on a feature branch). The docs gate went green. A `code-review`
  pass caught it as a Critical; nothing mechanical would have.

  This matters more in `docs/self-improvement/categories/**` than elsewhere: a backlog entry is
  read months later by someone who will act on it, and its whole value is that the cited
  file:line is real. A stale entry there wastes the reader's time in exactly the way the backlog
  exists to avoid.

  Proposed: extend the markdown-link checker (or add a sibling) with a **WARN-first** rule scoped
  to `docs/self-improvement/categories/**` — for each inline code span that looks like a repo path
  (leading `scripts/`, `Source/`, `docs/`, `agents/`, `tests/`, `tools/` plus a file extension),
  assert it resolves — **at `HEAD` first, falling back to `origin/develop`**. Checking only
  `origin/develop` would false-warn on every path added by the same PR that adds the entry, which
  is the common case; checking only `HEAD` would miss the failure this entry exists for. WARN-first
  because a *deliberate* reference to a path on some other unmerged branch is legitimate; that
  entry should then carry the "not on `develop` yet" caveat in prose, which is exactly the review
  the warning prompts.

  Same delta-gate shape as the other doc gates: only newly-added or modified lines, so the whole
  existing backlog does not have to be clean on day one.

  The blindness cuts both ways, and this entry tripped the other edge while being written: prose
  quoting the *shape* of a markdown link inside a code span is read by the checker as a real link
  and reported as a dangling one. So the same fix — teach the tokenizer about inline code spans —
  removes a false negative (paths in spans never checked) and a false positive (link-shaped spans
  checked as if they were links). Until then, a doc that needs to discuss link syntax has to
  describe it in words, which is why the sentence above does.

- 2026-08-07 · claude-code · [tooling] · P2 — extract the ephemeral-`SMATCHET_USER_DATA` hardening into a shared helper; roughly three-quarters of the bucket-E runners still inherit the developer's real profile

  There are **30** `scripts/dev/test-ui-*.sh` runners. Only **7** point `SMATCHET_USER_DATA`
  at a `mktemp -d` (`data-dependent-windows`, `duration-inline-edit`, `funcsize-grid-render`,
  `grid-pane-windows`, `jira-deterministic-backend`, `linear-deterministic-backend`,
  `omnibar-search-apply`), and only **3** of those also seed the config JSON
  (`duration-inline-edit`, `grid-pane-windows`, `omnibar-search-apply`). So ~23 runners need
  the ephemeral home and ~27 need the seed.

  The exemplar to copy is [`scripts/dev/test-ui-grid-pane-windows.sh`](../../../scripts/dev/test-ui-grid-pane-windows.sh)
  lines 38-47, which does both and already carries the rationale in a comment. The seed is:

  ```json
  {"read_only_mode": false, "whisper_setup_completed": true, "backend_has_been_reachable": true}
  ```

  Two independent reasons, both applying to every runner:

  - **Correctness.** A *fresh* profile shows the `##WhisperSetupBanner`, which floats over
    window headers and swallows clicks. A case that clicks anything in the top strip fails
    for a reason unrelated to the code under test — and, worse, *passes* on a developer
    machine whose real profile already dismissed the banner. That is a machine-dependent
    test, which is the failure mode bucket-E exists to avoid.
  - **Safety.** Without the override the run reads and writes the developer's real
    `%LOCALAPPDATA%/Smatchet/`, including `imgui.ini`. A docking test that ends mid-layout
    leaves the developer's actual window arrangement mangled.

  Proposed: extract the seed + `mktemp -d` + trap-cleanup into a sourced helper
  (`scripts/dev/lib/ui-test-home.sh`) and have every runner source it, so the hardening
  cannot drift back out one script at a time — which is what the 7-of-30 split shows has
  already happened.

  A `test-orphan-bats`-style gate would keep it honest: assert every `scripts/dev/test-ui-*.sh`
  either sources the helper or carries an explicit opt-out comment.

  Separately, `test-ui-window-expand.sh` on the [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966)
  branch (not on `develop`, so not linkable from here) takes a **different** approach to the
  same problem — it exports `LOCALAPPDATA` / `APPDATA` / `XDG_CONFIG_HOME` to a `mktemp -d`
  rather than setting `SMATCHET_USER_DATA`, and seeds nothing. Worth reconciling into the one
  helper when that branch merges rather than letting two idioms coexist.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — the two isolation idioms (platform-dir shadowing and SMATCHET_USER_DATA) are one helper, scripts/dev/lib/ui-test-driver.sh, used by 29 of 30 drivers (one reasoned opt-out), enforced by the ratchet in tests/bats/ui_driver_filters.bats; helper bats in tests/bats/ui_test_driver_lib.bats.
  Status: applied (2026-10-04)

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
    [`tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](applied.md).
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
    (3) **Rule text.** Add to [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md)
    § Concurrent interactive sessions: *"Run gate tooling from a worktree freshly based on
    `origin/develop`, never from a long-lived session branch. A BLOCK observed from a stale checkout
    is not evidence about `develop`; reproduce from a fresh worktree before filing anything against a
    gate."* This is the rule that would have stopped the bad entry with zero code.
    (4) **Evidence rule for the ledger.** A postmortem or backlog entry whose central evidence is
    gate-tool output must record the tree + commit the tool ran from. Fold into the
    [`gate-escape-postmortem`](../../../agent-layer/agents/_shared/skills/gate-escape-postmortem/SKILL.md)
    skill's evidence checklist — the discipline generalises past this bug.
    Est ~0.5d ((1)+(2) helper + 2 bats cases; (3)+(4) doc edits).
  Cross-ref: `agents/scripts/core/merge-gates.sh` (:1201-1202 the downgrade absent from the stale
    copy); `4685997d` / PR #1468 (2026-06-20, the commit the session branch predates); PR #1953
    (phantom block) vs PR #1961 (same class, passes from a fresh worktree);
    [`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md) § Concurrent interactive sessions
    (`nsc <slug>` one-worktree-per-session rule this extends from correctness to *freshness*).

  **Update 2026-08-11 — (1), (3), (4) shipped; (2) is the remaining scope.**

  - **(1) Version self-check in the poller — DONE, by fixing its default rather than
    building it.** The machinery already existed (`MERGE_GATES_FRESHNESS`, added for
    #1428: fingerprints `merge-gates.sh` + both `merge-gates.d/` modules against
    `origin/develop` and warns or fails closed). It was **defaulted `off`**, and a repo
    sweep found `merge-watcher.py` to be its only setter (`block`) — so the caveat
    reached the one caller that is never stale, and never reached the human-invoked
    poll this entry is about. The default is now **`warn`**: a BLOCK can no longer be
    read without the "differs from origin/develop" line when the checkout is behind.
    `warn` never sets `self_stale`, so no caller's verdict changes and offline /
    detached / no-remote stay usable, exactly as this entry required. The bats suite
    now sets `MERGE_GATES_FRESHNESS=off` explicitly (its ~170 cases would otherwise each
    attempt a real `git fetch` and warn about the branch under development), and the
    old "freshness OFF by default" test is replaced by one pinning the `warn` default
    plus an explicit-off canary.
  - **(2) Same check in the shared helper — DONE.** Extracted to
    [`agents/scripts/core/lib/script-freshness.sh`](../../../agent-layer/agents/scripts/core/lib/script-freshness.sh):
    `script_freshness_verdict` (the bounded fetch, the multi-file combined
    fingerprint, the fail-closed blanking) plus the advisory `warn_if_script_stale`
    wrapper. `merge-gates.sh` now delegates detection to it while keeping its own
    message prose — the split is deliberate: prose duplication is cheap and lets each
    gate cite its own history, whereas a second hand-rolled fetch is where hangs and
    silent stale-compares come from. Behaviour-preserving, proven by the poller's
    171-case suite passing unchanged across the extraction.
    Wired into **`pre-ship.sh`**, the false-**greens** case this entry called the worse
    direction: the caveat prints immediately before the `Safe to push` line (a startup
    banner would have scrolled away behind minutes of gate output, and the verdict is
    what it qualifies), over a declared set covering the entry point, the delta-lint
    gate, its rule modules, the review-ack lib and the detector itself. Also wired into
    **`postmortem-owed.sh`**, qualifying specifically its `no gate escapes owed` clean
    result — a false green there is exactly the reading that let #1941 pass as clean.
    `issue-sweep.sh` and the lint gates are still unwired; the helper is in place, so
    each is now a two-line call rather than a re-implementation.
    Three design points worth keeping: `unverifiable` is a verdict DISTINCT from
    `stale` (a failed fetch must not be reported as drift, nor drift hidden when
    offline); no fetch caching (measured 0.79s against the real remote, immaterial
    beside what these gates already do, and caching would mean comparing against a
    possibly-stale ref — the exact bug being fixed); and each caller's declared set
    includes the detector itself, closing the one blind spot that could hide all the
    others.
    **Extraction bug worth recording**, caught by `pre-ship.sh --selftest` on first
    wiring: the inline original used `local var="$(cmd)"`, where `local`'s own success
    MASKS the command substitution's exit status. Plain assignments in a sourced lib
    lose that mask, so under the `set -euo pipefail` its callers run, a missing file
    killed the calling gate outright instead of degrading to `unverifiable`. Every
    substitution now carries an explicit `|| var=""`, and
    [`tests/bats/script_freshness.bats`](../../../agent-layer/tests/bats/script_freshness.bats)
    pins it with four `set -e`/`set -u` survival cases (16 cases total).
  - **(3) Rule text — DONE.** `process-rules.md` § Concurrent interactive sessions
    carries "Run gate tooling from a tree freshly based on `origin/develop`, never from
    a long-lived session branch", stating that a BLOCK from a stale checkout is not
    evidence about `develop`, that the WARN is a stop-and-re-run signal, and that the
    other core gates have no such guard yet.
  - **(4) Evidence rule for the ledger — DONE.** The
    [`gate-escape-postmortem`](../../../agent-layer/agents/_shared/skills/gate-escape-postmortem/SKILL.md)
    skill's step 1 now requires recording the tree + commit any cited gate-tool output
    ran from, and to reproduce from an `origin/develop`-based worktree before the
    finding enters the ledger.

  All four proposals are shipped, so this entry is closed. The one leftover — wiring
  the remaining callers (`issue-sweep.sh`; the lint gates, already covered indirectly
  because `pre-ship.sh` fingerprints `test-lint-rules.sh` + `lint-rules.d/`) — is
  breadth over a helper that now exists, not this entry's thesis, and is carried
  forward as its own P3: `2026-08-11-script-freshness-remaining-callers`.

  Status: applied (2026-08-11 — (1)/(2)/(3)/(4) all shipped)
  Last-reviewed: 2026-08-11

- 2026-08-06 · orchestrator · [infra] · P1 — `required-check-that-never-reports-is-invisible`: the merge-gate poller and the gate-escape detector both key on RED checks; a required context that reports NOTHING is indistinguishable from a slow one at the merge box, and merges past silently.
  Details: observed live on #1964 and #1970. `Agentic self-tests (bats)` is a branch-protection REQUIRED context (`project.config.json` § branch_protection.required_contexts). On #1964 its only run was created at 15:48, sat `queued` from 16:21 for ~2h20m, and was pinned to `6244d02b` — which then MERGED as part of that very PR, so the run could never have reported on the merged code. #1964 merged with ten contexts in a non-green terminal state, six of them still `queued`. Recorded losslessly in `merge-snapshots.jsonl` (PR #1969, `gates: GATES_INCOMPLETE`). On #1970 it was worse: after the stale run was cancelled, pushes of `000abdd9` and `216f1fbd` created NO run at all, and a close/reopen (the only lever — the workflow has no `workflow_dispatch`) still left `get_check_runs` at **total_count 0**. The PR merged with zero check runs in existence. Run creation was NOT broken repo-wide at the time (#1969 got a full set at 18:13), so this is per-PR/per-branch, not a global outage — which is exactly why "it's just the outage, it'll report eventually" was the wrong read for hours. `merge-gates.sh` GATE_FILTER and `postmortem-owed.sh` both enumerate *red* contexts; neither asserts that every required context has produced a terminal conclusion at all, so absence reads as "nothing to block on".
  Concrete next action: add an ABSENT-REQUIRED-CONTEXT assertion to the merge-gate poller — enumerate `project.config.json` § branch_protection.required_contexts, intersect with the contexts present in the PR's `statusCheckRollup`, and BLOCK on any required name with no check-run/status at all (or one still `queued`/`in_progress` past a staleness cutoff), reported distinctly from a red check (e.g. `GATES_ABSENT` vs the existing red-check block) so an operator can tell "never ran" from "ran and failed". Mirror the same assertion in `postmortem-owed.sh` so an absent-required merge owes a postmortem the way a red-check merge does — today it owes nothing. Cheap sub-case worth doing first: flag a check-run whose `head_sha` is not the PR's current head (the #1964 phantom — a queued run pinned to an already-merged sha), which is detectable with no timing heuristic at all. Est ~3-4 h. Cross-ref: `merge-snapshots.jsonl` row for #1964 (the lossless capture); PR #1969; PR #1970 § comment "The `Agentic self-tests (bats)` lane has never run on this PR".
  Recurrences (all 2026-08-06, same day the entry was filed):
  - #1964 — merged with 10 contexts non-green, 6 still `queued`. Ledger row: `GATES_INCOMPLETE`.
  - #1970 — merged with `get_check_runs` at **total_count 0**; a stale run pinned to an already-merged sha was cancelled, then two pushes AND a close/reopen produced no check suite. Ledger row: `GATES_ABSENT`.
  - #1972, #1973, #1974 — each merged with exactly ONE check run (Bugbot) and **zero workflow runs**. #1974 carried real shell logic plus 9 new tests, none of which executed in CI. Queried directly: `list_workflow_runs` for `claude/verifier-slice2-preship` returned `total_count: 0` across ALL workflows.
  - #1941 — independently reached by another session and written up as a full postmortem: an `--admin` merge past 22 required contexts that never ran, after which `postmortem-owed.sh --list` reported "no gate escapes owed (last 20 merges clean)". See [`postmortems.md`](../postmortems.md) 2026-08-06 · PR #1941, and the `process` entry `2026-08-06-admin-merge-past-absent-checks-undetected` landing in PR #1975 (not linked as a path — it is not on develop yet).

  **Cross-link — this is the same hole as `2026-07-10 · PR #1698` and `2026-08-05 · PR #1937`, reached from a third direction.** Those two describe a check green on the PR head and red on develop; this entry describes a required context that never reports at all; #1941 describes an `--admin` merge past absent checks. All three are the same missing assertion — *no consumer verifies that every required context actually produced a terminal conclusion* — and both prior entries proposed a develop-tip required-green assertion that never landed. That is now **three prior proposals plus six observed occurrences**, which is the argument for building it rather than filing it a fourth time.

  **Converge on ONE gate, not two.** PR #1975 proposes the strongest form: extend `postmortem-owed.sh` with a fifth signal that, for each merge commit on develop, resolves the merged PR's head sha and flags `actions/runs?head_sha=<sha>` returning `total_count == 0`, or a head rollup carrying fewer contexts than the branch-protection required set. That subsumes this entry's proposed cheap sub-case (flag a check-run whose `head_sha` is not the PR's current head) and needs no cooperation from the merge actor, which the label-keyed signals do. Build #1975's version; keep this entry's merge-gate-side assertion (block, don't merely detect) as the second half, since detection after the fact does not stop the merge.

  **A likely mechanical cause for the zero-run PRs, from #1975's finding.** GitHub will not build a head whose `mergeStateStatus` is `DIRTY` / `mergeable` is `CONFLICTING`. #1972-#1974 all touched files with concurrent churn (`docs/plans/INDEX.md`, `merge-snapshots.jsonl`), so a conflicted head is the leading explanation for "no runs created" as distinct from the repo-wide queue jam — two different failures that present identically at the merge box. Unverified retroactively (the PRs are merged); worth confirming when the next zero-run PR appears.

  **Confirmed against the next zero-run PR — and DIRTY does NOT explain it.** #1976 (this entry's own PR, seventh occurrence) sat at `total_count: 1` (Bugbot only, zero workflow runs) for over an hour, while its `mergeable_state` read **`blocked`**, not `dirty` — blocked precisely *because* the required contexts were absent. So a conflicted head is at most a contributing cause, not the mechanism. **The mechanism, measured: run creation LAGS by tens of minutes under backlog — it does not drop.** Tracking #1976's head continuously produced the number this entry was missing: a push at 21:20Z showed `total_count: 0` at 21:21Z and again at 21:25Z, and its full 15-run set was created at **21:46:58Z — a ~27-minute creation lag.** Two earlier readings that looked like permanent absence were simply taken inside that window. Those 15 runs subsequently all completed **success**.

  **But BOTH failure modes are real, and the very next push proved it.** The successor head `92553244` (pushed 22:10Z) had **zero** runs at 23:16Z — 66 minutes, more than double the measured lag — while the queue was fully drained and healthy (1 queued, 3 in-progress, 12 successes repo-wide, newest run created 23:15:57Z, other branches getting suites within seconds). With a healthy queue as the control, that is not lag: for this head, runs were genuinely **never created**. The push was not lost — CodeRabbit posted against `92553244` minutes after it landed — so GitHub received the ref update and Actions alone did not act on it.

  **That pair of observations is the actual requirement, and it is harder than either failure alone.** Consecutive pushes on the *same branch, same day* produced one head that lagged ~27 min and then succeeded, and one that never got a suite at all. The two are indistinguishable at every instant before the lagging one recovers. So no timeout constant can be *correct* — it can only trade false alarms against missed escapes. The gate must therefore **block on absent regardless of cause** (never merge a head whose rollup carries fewer contexts than the required set, with no time-based escape hatch), and treat the timeout purely as the threshold for *notifying a human*, not for deciding the merge is safe.

  **This retracts two confident explanations recorded above, and both retractions matter.** An "Actions outage window; GitHub never retroactively creates a suite" claim was written on the 20:23Z/20:33Z recovery, then disproved by a re-push showing zero. A follow-on "creation is intermittently failing; a fresh head is not a reliable remedy" was written on that, then disproved by the 21:46:58Z creation. The lesson is methodological and belongs in the gate design: **`total_count == 0` at an instant does not distinguish "will never be created" from "not created yet",** and there is no API field that does. Any check keyed on a point-in-time zero — including #1975's proposed `actions/runs?head_sha=` signal — needs a lag tolerance, and (per the next paragraph) that tolerance can only ever be a heuristic for *when to shout*, never a proof that the checks are absent for good.

  **Leading explanation for the merged zero-run PRs, and it is partly on the merge actor, not on GitHub.** #1972-#1974 were each merged within a few minutes of PR creation — well inside a lag window this large. So "zero runs at merge" was plausibly "runs not created yet", and the operative defect is **merging before the required contexts have had time to appear.** That is a materially more actionable finding than an Actions fault, because it is entirely within our control: the merge-gate half must refuse to merge while the head's rollup carries *fewer contexts than the required set*, treating absent-so-far exactly as it treats red — and must never interpret an empty rollup as "nothing to wait for". Unverified retroactively for those three PRs (they are merged and their creation timestamps cannot be recovered); verified prospectively on #1976.

  **Auto-merge is the one consumer that behaves correctly here, and it is worth saying why.** Auto-merge armed on a head with an empty rollup simply waits, and once the lagged contexts are created and pass it fires normally — observed on #1976. The hazard is not that it breaks; it is that *waiting on absent contexts* and *waiting on pending contexts* are indistinguishable in every UI GitHub offers, so an operator watching a genuinely-stuck PR has no signal and an operator watching a merely-lagged one has no reassurance. That asymmetry is what the merge-gate-side half must fix: report **absent** distinctly from **pending**, block on both, and let elapsed-time-since-push decide when absent has stopped being lag and started being a fault.

  **Update 2026-08-11 — the detector half landed; the entry narrows to one sub-case.** Both halves of "converge on ONE gate" are now in tree, from different directions:
  - *Merge-gate side (block)* — already shipped before this sweep and re-verified: `merge-gates.d/10-gate-filter.sh` computes `$reqAbsent` (config `required_contexts` minus the head rollup's context names), `merge-gates.sh` BLOCKs on `req_absent > 0`, fails closed on a parse miss (`req_absent < 0`), and `req_absent -eq 0` is a conjunct of `GATES_PASSED`. It is reported distinctly from a red check (`required-missing:` vs the red-check block) and has **no time-based escape hatch**, which is the property this entry argued for. A required context that is PRESENT-but-`QUEUED` is correctly not "absent".
  - *Detector side (after the fact)* — NEW: `postmortem-owed.sh` gained a required-ABSENT cross-check (field 5 `present_names` vs `$reqNames`), running on **both** the snapshot and live paths because a merge-instant snapshot records the red set, not rollup membership. This is the fifth signal PR #1975 proposed, and it closes the "`--admin` merge past absent checks owes nothing" hole for #1941 / #1972-#1974. Guarded by `POSTMORTEM_ABSENT_GRACE_SECONDS` (default 3600, ~2x the measured 27-min creation lag) so a merge still inside the lag window is deferred to the next sweep rather than false-flagged — per this entry's own methodological lesson, that grace is a *notification* threshold only and deliberately does NOT exist on the blocking side. Suite: `tests/bats/postmortem_owed.bats` § required-absent (8 cases, incl. the #1941 zero-rollup shape and the absent-vs-pending distinction).

  **The last sub-case is closed too, but NOT as this entry proposed it — the `head_sha`-mismatch probe does not fit the mechanism.** This entry named "flag a check-run whose `head_sha` is not the PR's *current* head" as the cheap #1964 sub-case, detectable "with no timing heuristic at all". Checked against the actual query: the rollup is fetched via `commits(last: 1) { commit { statusCheckRollup … } }`, i.e. it is scoped to the PR's current head **by construction** — GitHub aggregates check runs whose `head_sha` IS that commit, so a foreign-sha run cannot appear in it and there is nothing for the probe to find. Re-reading this entry's own #1964 evidence confirms it: the run was pinned to `6244d02b`, which *was* the head at the time and then merged as part of that PR. It was never a mismatched sha; it was a run that **never reached a terminal conclusion**.
  So the property that actually distinguishes the #1964 rollup from a healthy one is **non-terminality**, and that is what shipped: `postmortem-owed.sh` now also emits `required-never-terminal: <names>` for a required context PRESENT in a merged PR's rollup whose latest run is still `QUEUED`/`IN_PROGRESS` (or a `StatusContext` still `PENDING`). This is genuinely invisible to every other signal — the absence check sees it as present, and the red-check curation deliberately requires a terminal verdict, so it is neither absent nor red. It reuses the same `$absent_judgeable` grace gate and so needs no timing heuristic of its own: a check merely mid-flight at merge goes terminal within minutes, so one still non-terminal an hour later never finished at all. That reads a fact rather than guessing. Same latest-run-per-context dedupe as the red curation, so a queued *re-run* beside an older SUCCESS counts (the context's current state never reported). 6 cases in `tests/bats/postmortem_owed.bats` § required-never-terminal, including #1964's exact shape and the absent-vs-never-terminal partition.

  **Nothing is left of this entry.** Both halves of the merge-box assertion (block pre-merge, detect post-merge) and both masks the escape wears (absent, present-but-never-terminal) are covered.

  Status: applied (2026-08-11 — absent-required both halves + never-terminal; the proposed head_sha probe was investigated and does not apply, see above)
  Last-reviewed: 2026-08-11

- 2026-08-06 · claude-code · [process] · P1 — an `--admin` merge past checks that never ran leaves no trace any detector reads, and `merge-gates.sh` mislabels a CI-cannot-run head as "required-missing"

  **Evidence.** PR #1941 (preferences IA re-segmentation + global search) squash-merged
  2026-08-06T19:25:12Z as `c7fb2236` on `develop` via `gh pr merge --squash --admin`, with all
  22 branch-protection-required contexts absent from the head rollup. The head `4617a034` was
  never built at all: `gh api repos/<o>/<r>/actions/runs?head_sha=4617a034…` returned
  `total_count: 0`. `postmortem-owed.sh --list` afterwards reported "no gate escapes owed
  (last 20 merges clean)".

  Two independent holes produced this:

  1. **No defined behaviour when CI is structurally unavailable.** Actions was jammed
     repo-wide — 75 runs stuck `queued` since 18:13 UTC, no new run created repo-wide after
     19:16 UTC, and a `gh pr close && gh pr reopen` (to re-fire the `pull_request` event)
     produced 0 runs. There was no path to a green head. The ship-loop's only defined move
     is to keep polling, so the operator's choices collapse to "wait indefinitely" or
     "override" — and the override is exactly what the gates exist to prevent. The escape is
     the *absence of a third option*, not the person who took the second.

  2. **The escape class is invisible to the detector.** `postmortem-owed.sh` keys on a
     non-SUCCESS check at merge, an override label, a `Revert` commit, or an overdue
     deviation. An `--admin` merge past *absent* checks emits none of those four signals:
     there is no red check (there is no check), no label, no revert. This is the same
     detection hole already recorded in the ledger twice — `2026-07-10 · PR #1698` and
     `2026-08-05 · PR #1937` — both of which proposed a develop-tip required-green assertion
     that never landed. Third recurrence.

  A third, lower-severity contributor worth fixing in the same area: **`merge-gates.sh`
  reports a conflicted head as N `required-missing` checks.** When `mergeStateStatus` is
  `DIRTY` / `mergeable` is `CONFLICTING`, GitHub declines to build the head at all, so the
  poller sees 22 absent required contexts and prints the generic "never ran; e.g. a
  GITHUB_TOKEN bot push that did not re-trigger CI" hint. That cost a full 90-poll timeout
  before the actual cause (a conflict in `docs/plans/INDEX.md`) was found by hand. The
  actionable cause was available on poll 1 from a field the poller already fetches.

  Proposed fixes:

  1. **Detect the merge after the fact.** Extend `postmortem-owed.sh` with a fifth signal:
     for each merge commit on `develop` in the scanned window, resolve the merged PR's head
     sha and flag it when `actions/runs?head_sha=<sha>` yields `total_count == 0`, or when
     the head rollup carries fewer contexts than the branch-protection required set. This
     catches admin merges, zero-rollup merges, and the "CI never triggered" class in one
     check, and it needs no cooperation from whoever performed the merge — which is the
     property the label-keyed signals lack.
  2. **Name the real cause in the poller.** In `merge-gates.sh`, branch on
     `mergeStateStatus == DIRTY` / `mergeable == CONFLICTING` before reporting
     `required-missing`, and emit a distinct blocked reason ("head is conflicted — CI will
     not build it; merge origin/develop first"). Same for `BLOCKED` with a zero-length
     rollup. Cheap: both fields are already in the existing GraphQL response.
  3. **Give "CI is unavailable" a defined move.** Today the ship-loop has none. Minimum
     viable: when the poller observes zero runs created repo-wide inside the poll window
     (an Actions outage, not a PR problem), it should stop polling and escalate with that
     diagnosis rather than time out at 90 polls with a per-check message — per
     `AI_POLICY.md` § Escalate, don't assume, an unvalidatable state is an escalation, and
     an outage is unvalidatable by construction.

  Concrete next action: fix (1) — it is self-contained inside `postmortem-owed.sh`, closes
  a hole that has now recurred three times, and is testable in `tests/bats/`. (2) is a small
  follow-up in the same PR if the diff stays small. (3) needs a design call on what the
  ship-loop does with an escalation and should not be bundled.

  Related, distinct — do not merge these: the two prior ledger entries (2026-07-10 · #1698,
  2026-08-05 · #1937) describe the same *detector* hole reached from a different direction
  (a check green on the PR head and red on develop). A single develop-tip required-green
  assertion would close all three, and that is the argument for finally building it.

  Compensating verification actually performed on the merged head, for the record: dual-target
  build (`SmatchetStandalone` + `SmatchetCore_DX12`) EXIT=0 and
  `test-lint-rules.sh --diff origin/develop` EXIT=0 (advisory WARNs only). That is not CI and
  does not substitute for it — the next `develop` post-merge run, once Actions drains, is the
  backstop to watch.

  **Update 2026-08-11 — fixes (1) and (2) shipped; (3) is all that remains.**

  - **(1) Detect the merge after the fact — DONE.** `postmortem-owed.sh` gained the
    fifth signal as a required-context-ABSENT cross-check: every name in
    `branch_protection.required_contexts` must appear in the merged PR's rollup
    (field 5, the `|||`-joined present-context list the script already parsed for the
    expected-present allow-list), else the merge owes a postmortem. It runs on the
    snapshot path as well as the live one — a snapshot records the merge-instant RED
    set, never rollup *membership*, so absence is only ever observable from the live
    rollup and an instrumented merge would otherwise be exempt from the one signal
    snapshots cannot carry. It needs no cooperation from the merge actor, which is
    the property this entry wanted. Implemented via rollup membership rather than the
    proposed `actions/runs?head_sha=<sha>` probe: same escape class, no extra API call
    per scanned merge, and it also catches a head that *was* built but whose suite is
    missing contexts. Guarded by `POSTMORTEM_ABSENT_GRACE_SECONDS` (default 3600) —
    the infra entry `required-check-that-never-reports-is-invisible` measured a ~27-min
    check-suite creation lag, so a just-merged PR is deferred to the next sweep instead
    of false-flagged. Suite: `tests/bats/postmortem_owed.bats` § required-absent,
    including this PR's exact shape (zero rollup, no red, no label → owes).
  - **(2) Name the real cause in the poller — DONE.** `merge-gates.sh` now branches on
    `mergeStateStatus == DIRTY` before emitting the generic hint and reports "head is
    CONFLICTED … merge origin/develop first; this is NOT a CI fault and polling will
    not clear it". As predicted the field was already in the poller's GraphQL response,
    so the diagnosis is available on poll 1 rather than after a 90-poll timeout. The
    non-DIRTY branch keeps the original "never ran" hint and gained the creation-lag
    caveat. Both are still BLOCKs — only the diagnosis differs. Cases in
    `tests/bats/merge_gates.bats` § required-missing cause attribution, incl. a negative
    canary that a DIRTY head with nothing absent emits no conflict line.
    *Adjacent false-pass found while testing, and fixed:* the `mergeStateStatus`
    guard blocked on `BLOCKED|BEHIND` only, so an all-green **DIRTY** head — one whose
    required contexts DID report before the conflict appeared — reached `GATES_PASSED`
    and failed later at the REST merge. Initially left alone on the reasoning that this
    entry asked for diagnosis rather than a new block; that was too narrow, since an
    unmergeable head passing the gate is the same false-pass class the entry is about.
    `DIRTY` now joins the blocking set. Safe because `DIRTY` is a *computed* verdict —
    GitHub reports `UNKNOWN` while mergeability is still being determined — so it
    cannot fire on a pending computation, and `MERGE_GATES_IGNORE_MERGESTATE` still
    covers a positively-confirmed stale `DIRTY` (pinned by its own test).
  - **(3) Give "CI is unavailable" a defined move — DONE (2026-08-13).** Design call
    made: escalation is a deterministic poller verdict, and the move on it is
    stop-and-surface, never override. `merge-gates.sh` gained an Actions-outage
    detector: after `MERGE_GATES_OUTAGE_POLLS` (default 15) consecutive
    unexplained required-absent polls it probes `actions/runs?created=>=<poll
    start>` repo-wide; zero runs created → stop polling, print `ESCALATE:
    ACTIONS UNAVAILABLE` with the diagnosis, return the new exit code 7 —
    instead of burning the remaining window to the generic per-check timeout.
    The PROBE (not the threshold) separates an outage from the ~27 min
    check-suite creation lag: backlogged-but-alive Actions still creates runs
    repo-wide, so the count stays >0 and polling continues. Conflict-explained
    absence (mergeStateStatus=DIRTY) never counts toward the streak, a failed
    probe keeps polling (escalation is never taken on unverified evidence), and
    0 disables. The ship-loop's defined move for exit 7 is documented at
    `docs/agent-rules/ship-loops.md` § CI unavailable: escalate per
    `AI_POLICY.md`, re-run when Actions drains or re-fire CI, never `--admin`
    past absent checks — which fix (1)'s detector now flags regardless. Suite:
    `tests/bats/merge_gates.bats` § Actions-outage escalation (8 cases:
    escalate / alive / probe-fail / threshold / streak accumulation / DIRTY
    exemption / disabled / knob validation).

  Status: applied — all three fixes shipped ((1)+(2) 2026-08-11, (3) 2026-08-13)
  Last-reviewed: 2026-08-13

# The required CR-findings gate has no pass path when CodeRabbit never reviews (throttled, draft-skipped, or path-excluded)

- 2026-08-06 · orchestrator · [tooling] · P2 — `merge-gates.sh` misclassifies **every** CodeRabbit skip as the too-many-files size-skip: the `$crskip` disjunct `contains("skip review by coderabbit.ai")` matches the HTML marker CR emits for *any* skip reason, so a **path-filter** skip is routed into the size-skip hard-block arm. Currently **masked** for the `docs/self-improvement/**` class by the 2026-06-20 auto-exemption, so it only bites other skip classes — hence P2, not P1
  Details: The `$crskip` computation at `agents/scripts/core/merge-gates.sh:552-554` is:
      `any(contains("skip review by coderabbit.ai") or (test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files"))))`
    CR's skip comment carries the literal marker `<!-- This is an auto-generated comment: skip review
    by coderabbit.ai -->` for **every** skip reason (path filters, docs-only, trivial diff,
    too-many-files). The first disjunct therefore fires unconditionally on any skip, `$crskip=true`,
    and the `case NONE` size-skip arm (:957-961) short-circuits to `cr_pass=false` +
    `cr_size_skip_block=true` with "CodeRabbit skipped review — too many files (exceeds CR file
    limit); split the PR" — advice that is unactionable when the diff is one file. The arm is
    deliberately a hard short-circuit (it closed the hole that let a 638-file reorg merge with zero CR
    review), so it also pre-empts the PASS arm below it, `$crreviewskipped` (:564-569) — which would
    not have fired anyway: it keys on the `CodeRabbit` StatusContext **description** matching "review
    skipped", and on the observed PRs that description read `Review completed`. CR's status text and
    its comment text disagree about the same run, so the intended fallback is keyed on a field that
    does not carry the fact.
  Scope of the damage today — smaller than it first appeared, verified 2026-08-06: commit `4685997d`
    (2026-06-20, "feat(merge-gates): auto-exempt pure self-improvement doc PRs from CR + Bugbot
    review", #1468) added a belt-and-suspenders downgrade at :1201-1202 — when
    `self_imp_only` (tuple field 27, diff entirely under `docs/self-improvement/**`) is true, a CR
    block is downgraded to `WARN: self-improvement doc PR — CR gate auto-skipped`. So on current
    `develop` the misclassification is **printed but not load-bearing** for self-improvement doc PRs;
    confirmed on PR #1961, where the bogus size-skip BLOCK appeared every poll and the run still
    reached `GATES_PASSED` at poll 12. The bug remains live for any *other* skip class — a different
    `.coderabbit.yaml` path filter, a docs-only or trivial-diff skip on a non-self-improvement path —
    where nothing downgrades it and the only exits are the `cr-out-of-band` label or an out-of-band
    merge. That residual case is the reason to still fix it.
  Correction to the first draft of this entry: it was written from a poller run on PR #1953 that
    appeared to block indefinitely, and claimed the sanctioned merge path was structurally unusable
    for self-improvement PRs. That was wrong on two counts and the ledger entry built on it has been
    withdrawn. (1) That poller was invoked from the long-lived integration tree
    (`C:/Dev/Smatchet` @ `ff0ee7a6`), whose `merge-gates.sh` predates `4685997d` and has no downgrade
    — the block was an artefact of a stale checkout, not of current `develop`; filed separately as
    `2026-08-06 · [process] · P1 — gate tooling run from a long-lived session branch`, since
    archived as applied in [`applied.md`](applied.md).
    (2) That run did **not** end in `GATES_TIMEOUT` at 60/60 as first reported — both poll logs end
    `PR_MERGED` at poll 49, i.e. the poller observed the merge and exited normally. No gate was
    escaped on #1953: CI was 22/22, `CR findings (0 actionable)` was SUCCESS with description
    `self-improvement-only diff (1 file(s) under docs/self-improvement/**) — CR review exempt`, there
    were no unresolved threads and no override label, and a sibling session merging an open green PR
    on the shared login is documented-expected
    ([`process-rules.md`](../../../agent-layer/docs/agent-rules/process-rules.md) § Git/p4 discipline).
  Concrete next action: two changes in `agents/scripts/core/merge-gates.sh`, plus test pins.
    (1) **Narrow `$crskip`** — drop the bare `contains("skip review by coderabbit.ai")` disjunct (a
    generic skip marker, not a size marker) and keep only the size-specific test, i.e.
    `any(test("##[[:space:]]*Review skipped"; "i") and (ascii_downcase | contains("too many files")))`.
    A genuine too-many-files skip still blocks — it carries that exact phrase; a path-filter /
    docs-only / trivial-diff skip then falls through to the terminal-pass arms as intended. This is
    the whole fix; the rest is defence in depth.
    (2) **Let the repo's own required gate win** — when the `CR findings (0 actionable)` StatusContext
    is SUCCESS on the current head, pass the CR bucket without re-deriving a verdict. That context is
    required and already fail-closed (it returns non-terminal rather than guess — see
    `docs/self-improvement/postmortems.md`, 2026-08-06 · PR #1948), so a poller re-derivation that
    disagrees with it can only produce a false block, never catch a real escape. Order it ahead of the
    `case NONE` chain. Note this also makes the :1201 self-improvement downgrade redundant for the
    common case rather than load-bearing, which is the healthier arrangement — today a masking
    downgrade is the only thing standing between the misclassification and a false block.
    (3) **Pin in `tests/bats/merge_gates.bats`**: (a) path-filter skip comment on a NON-self-improvement
    path + `CR findings` SUCCESS → PASS (the currently-unmasked case); (b) `## Review skipped` +
    "too many files" → still BLOCK (the #638-reorg contract preserved); (c) skip comment +
    `CR findings` SUCCESS → PASS via the new precedence rule, with `self_imp_only=false` so the test
    cannot pass merely via the :1201 downgrade.
    Est ~0.5d (jq edit + precedence arm + 3 bats cases).
  Cross-ref: `agents/scripts/core/merge-gates.sh` (:552-554 `$crskip`, :564-569 `$crreviewskipped`,
    :937-980 the `case NONE` arms, :1201-1202 the self-improvement downgrade that masks it,
    :1164-1167 the size-skip override message); `4685997d` / PR #1468 (the masking exemption);
    PR #1953 + PR #1961 (observations); `.coderabbit.yaml` (`!docs/self-improvement/**` path filter);
    `.github/actions/cr-finding-gate/action.yml` (the required gate whose SUCCESS the poller
    re-derives). Related: [`process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md`](applied.md)
    — the mirror-image hole (gate cannot reach a verdict) in the same CR path.

- 2026-08-06 · orchestrator · [process] · P2 — `postmortem-owed.sh`'s `cr-out-of-band` de-noise drops a **load-bearing** override whenever the diff has no `Source/Core/src/*.cpp`: it assumes the label only ever waives an *advisory CR verdict*, but the label is also the only exit from a **wedged required CR gate**, and that class ships invisible to the nudge
  Details: PR #1948 (the font-asset worktree fallback) merged 2026-08-05 carrying `cr-out-of-band`.
    The label was strictly load-bearing: `CR findings (0 actionable)` — a **required** StatusContext —
    was stuck PENDING because CR's last on-head review was body-less, so
    `.github/actions/cr-finding-gate/action.yml` `decide()` could not terminate (root cause filed as
    [`process/2026-08-05-cr-finding-gate-empty-body-review-wedge.md`](applied.md)).
    Every other check was green; re-running the workflow re-posted PENDING. The label's early-exit was
    the only way to clear `mergeStateStatus=BLOCKED`.
    `bash agents/scripts/core/postmortem-owed.sh --list` nonetheless reports "no gate escapes owed":
    `core_scoped_only_trigger()` (:156-163) returns 0 — drop — for the trigger string
    `override: cr-out-of-band`, and the guard that would keep it, `pr_touches_core_cpp()` (:167-170),
    is false because #1948 changed only `CMakeLists.txt`, `cmake/SmatchetFontAssets.cmake`,
    `Source/Standalone/CMakeLists.txt`, a bats file, a wrapper script and a README.
    The de-noise rationale (:149-155) is sound for its intended class — "cr-out-of-band only waives the
    (advisory) CodeRabbit review, so on a non-Core diff the escape is a false positive". It does not
    hold for the wedge class: there the label dismisses a **required, non-terminal gate**, and the diff
    scope is irrelevant to whether that mattered. Note the two holes point opposite ways in the same
    CR path — this one hides an override that WAS load-bearing; the sibling tooling entry
    [`tooling/2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](applied.md)
    manufactures override use where none is needed.
  Concrete next action: make the drop conditional on the CR gate having actually *ruled*, not on diff
    scope. In `postmortem-owed.sh`, before `core_scoped_only_trigger()` drops a `cr-out-of-band`
    trigger, consult `gate_conclusion "$pr" 'CR findings'` (the helper already exists, :187-202):
      - context SUCCESS on its own → the label dismissed nothing → drop (today's behaviour, now
        justified by evidence rather than by diff scope);
      - context PENDING / non-SUCCESS / absent at merge → the label was load-bearing → **keep**, owes a
        postmortem, regardless of Core-cpp scope.
    This reuses the same "moot vs load-bearing" test the script already applies to
    `tests-out-of-band` / `perf-out-of-band` / `coverage-out-of-band` / `intent-out-of-band` in
    `override_is_moot()` (:212-232) — `cr-out-of-band` is the one override currently exempted from that
    test (:227-230 routes it to the scope heuristic instead). Folding it in removes the special case.
    Caveat to encode: the live rollup is re-run-lossy and override labels are stripped post-merge, so
    prefer the snapshot path where available and fall back to the live query with the existing
    documented lossiness note. Add a `--selftest` case for each of the two arms.
    Est ~0.5d (one helper call + branch + 2 selftest cases + comment rewrite at :149-155).
  Cross-ref: `agents/scripts/core/postmortem-owed.sh` (:149-163 `core_scoped_only_trigger`,
    :165-170 `pr_touches_core_cpp`, :187-202 `gate_conclusion`, :204-232 `override_is_moot`);
    PR #1948 (`2602340e`, the escape this hid); `docs/self-improvement/postmortems.md`
    (2026-08-06 · PR #1948 entry).

## [P1] CR finding gate: poll loop outlives its own job timeout, wedging the PR

**Category**: tooling
**Date**: 2026-08-06
**Observed on**: PR #1954

### What happened

`.github/actions/cr-finding-gate/action.yml` polled CodeRabbit with
`ATTEMPTS=12` + `sleep 15`. That bounds only the *sleeping* (165 s); the loop's
real cost is 165 s **plus 12 GraphQL round-trips**, which is unbounded from the
step's point of view. The job carried `timeout-minutes: 5`, shared with setup +
checkout.

On a congested runner the job was killed mid-`sleep`, **before either terminal
`post` ran**. Consequences, all silent:

- the required check-run `CR finding gate` never reached a terminal state;
- the StatusContext `CR findings (0 actionable)` kept whatever value it already
  had (stale);
- nothing re-triggers the workflow — its triggers are CR review/comment events,
  and CR had already spoken.

Net: the PR wedged with **no self-healing path**. Only a manual re-run cleared
it. The gate's entire contract is "always leave a status behind", and the one
failure mode that breaks that contract had no guard.

### Why the existing gates missed it

The two numbers that must be ordered (`POLL_BUDGET`/`ATTEMPTS` in the composite
action, `timeout-minutes` in the workflow) live in **different files**, with no
assertion tying them together. Nothing failed; the job simply died. A timeout
kill is not a red test — it looks like infrastructure noise.

### Preventing gate

`tests/bats/cr_finding_gate.bats` (wrapper
`agents/scripts/project/test-cr-finding-gate-bats.sh`) pins both invariants:

1. **Timeout ordering** — `POLL_BUDGET_SECONDS` < Evaluate step
   `timeout-minutes` < job `timeout-minutes`, so the poll window always closes
   with time left to post and the step always dies before the job.
2. **Fallback poster exists** — an `if: always()` step posts PENDING to the
   *same* StatusContext when `steps.eval.outcome` is neither `success` nor
   `skipped`, converting "wedged forever" into "pending, re-runnable".

Both carry negative selftest fixtures (inverted ordering; fallback removed), so
the checks are proven to fire rather than passing vacuously.

### Generalisable lesson

**A poll loop that bounds its sleeps has not bounded its runtime.** Bound the
wall clock (`SECONDS`-based deadline), not the attempt count — then the exit
time is an invariant of the step rather than a consequence of API latency.

**Step-scoped vs job-scoped timeouts are not interchangeable.** A step timeout
cancels one step and lets `if: always()` cleanup run; a job timeout takes the
cleanup down with it. Any workflow whose contract is "always post something"
needs its risky work step-scoped.

- 2026-08-06 · claude-code · [debt] · P2 — `ScenarioRunner::Tick` runs **twice per rendered frame**, so `--warmupFrames=N` silently means `N/2` rendered frames and any scenario that draws in `OnFrame` submits its content twice into one ImGui frame

  Two call sites, both live in the standalone ephemeral loop:
  - [`SmatchetUI.cpp:642`](../../../Source/Core/src/Ui/SmatchetUI.cpp) — end of
    `drawPerFrameTicksAndHandlers`, itself the tail of `SmatchetUI::Draw`.
  - [`StandaloneAppBootstrap.cpp:685`](../../../Source/Standalone/StandaloneAppBootstrap.cpp)
    — in `RunRenderLoop`, after `SmatchetDrawFrameWithSeh`.

  `git log -S` dates both sites and the bootstrap `SmatchetDrawFrameWithSeh` call to
  `27063e22` (2026-05-29), so the duplication is not recent.

  Two observable consequences:

  1. **Frame budgets are halved.** `IsDone(frameIndex)` is compared against a
     frame counter incremented twice per rendered frame, so `--warmupFrames=16`
     gives 8 actual rendered warm-up frames. Every scenario's warm-up constant is
     off by 2× from what its author intended, and any timing-sensitive scenario is
     tuned against the wrong number.
  2. **Content is drawn twice.** Scenarios that issue ImGui calls from `OnFrame`
     — e.g. [`CodeSyntaxColoringScenario`](../../../Source/Core/src/Commands/Scenarios/CodeSyntaxColoringScenario.cpp)
     — submit their whole window twice into a single ImGui frame. The
     `code-syntax-coloring` capture visibly renders every code sample twice (and the
     Syncing toast twice, the second dimmed). Scenarios whose `OnFrame` only sets
     idempotent session flags (the `user-info-*` family) show no duplication, which
     is why this stayed invisible.

  Not fixed inline with PR #1962 deliberately: removing either call site changes
  frame semantics for **every** scenario (warm-up counts double overnight) and
  invalidates every bucket-C golden, so it needs its own slice with golden
  regeneration — which is approval-gated by
  [`golden-image-approval.md`](../../../agent-layer/docs/agent-rules/golden-image-approval.md).

  Suggested shape: keep the in-`Draw` tick (it matches production ordering and
  runs inside the `NewFrame`/`Render` bracket), delete the bootstrap one, then
  halve every scenario's default warm-up constant in the same commit so the
  *rendered*-frame count is unchanged and only the goldens that were genuinely
  double-drawn move.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — the bootstrap tick is gone (StandaloneAppBootstrap.cpp carries a 'Deliberately no scenario-runner tick' comment); SmatchetUI.cpp is the only desktop tick site, so --warmupFrames=N means N rendered frames.
  Status: applied (2026-10-04)

# Bucket-C goldens silently depend on whether the gitignored icon font resolves

- **Category**: test
- **Priority**: P2
- **Date**: 2026-08-06
- **Source**: PR #1952 (bucket-C determinism) — cost a full misdiagnosis round

## What

`assets/fonts/fa-solid-900.ttf` is **gitignored**. Toolbars render icon glyphs
when it resolves and a text fallback ("Refresh View", "Filter", …) when it does
not — a whole-row pixel difference, far past the L_inf 4 tolerance.

Two things make that environment-dependent rather than constant:

- CI has no font (nothing fetches it), so every checked-in golden is a
  *text-fallback* capture.
- PR #1948's `smatchet_resolve_font_asset()` falls back to the **main worktree**,
  so a linked worktree under `.claude/worktrees/<id>/` finds the dev's local copy
  and captures *icon* glyphs.
- The build's link step copies the font next to the exe whenever it resolves, so
  "I moved it aside" does not survive the next `cmake --build`.

Net effect: `bash scripts/dev/test-screenshot-diff.sh` fails on a dev machine
that has the font, passes in CI, with a diff that looks like a real UI
regression. During #1952 this masked a genuine, unrelated capture bug for a
whole debugging round (moving the TTF aside took the suite 9/6 → 11/4).

## Fix options

1. **Pin the font into the capture path** — have the screenshot driver force the
   text fallback (an env knob the font resolver honours, e.g.
   `SMATCHET_DISABLE_ICON_FONT=1`, exported by `test-screenshot-diff.sh`). Makes
   local and CI captures identical by construction; preferred.
2. **Vendor the font** — un-ignore it and check it in, so every environment
   including CI renders icons. Larger blast radius (licence + repo size + every
   existing golden regenerates), but removes the whole class.
3. **Assert, don't guess** — at minimum, make the driver detect a resolved font
   and print a loud banner naming it as a likely diff source. Cheap; strictly a
   diagnostic, not a fix.

Option 1 plus the option-3 banner is the recommended pair.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — resolved by option 2: assets/fonts/fa-solid-900.ttf is committed, so CI, the main tree and worktrees render identical icons.
  Status: applied (2026-10-04)

# bucket-E drivers run a stale exe silently

- **Category**: test
- **Priority**: P2
- **Date**: 2026-08-06
- **Source**: window-expand-button (bucket-E red chased for two build cycles)

## Friction

`scripts/dev/test-ui-window-expand.sh` (and every sibling bucket-E driver) runs
`build/ninja-ui-test-msvc/Smatchet.exe` unconditionally. If the source edit under test
landed after the last build, the driver reports a *product* verdict for a binary that does
not contain the change — and the failure text is indistinguishable from a real one.

Two debug cycles on this feature were spent disproving a hypothesis that had, in fact,
already been fixed in the working tree but not in the exe. Instrumentation added in the same
window produced zero log lines, which read as "the code path never runs" rather than "the
code was never compiled".

`docs/agent-rules/debug-techniques.md` § exe-staleness documents the manual check. A manual
check that must be remembered at exactly the moment the agent is deepest in a wrong
hypothesis is the weakest possible placement.

## Proposed fix

Add a staleness guard to the shared bucket-E driver preamble: compare the exe's mtime
against the newest mtime under `Source/` + `tests/ui/`, and hard-FAIL (exit 2, same class as
"binary missing") with the offending file named. Cheap — one `find -newer`. Belongs next to
the existing exit-2 "binary missing" check so every driver inherits it.

## Status

Open.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — scripts/dev/is-exe-fresh.sh takes repeatable --src dirs (Source/ + tests/ui/) and names the newest offending file; the shared driver helper exits 2 on a stale exe (opt-out SMATCHET_ALLOW_STALE_EXE=1, skipped under CI where the exe is a downloaded artifact) and every exe-launching driver calls it.
  Status: applied (2026-10-04)

# CR finding gate wedges on an empty-body CodeRabbit review

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-05
- **Observed on**: PR #1948 (`fix/fa-ttf-worktree-fallback`)
- **Status**: applied (2026-08-14 — landed structurally stronger than proposed,
  on the #1996 re-occurrence: instead of special-casing an empty body inside the
  `n_reviews > 0` path, `decide()` in
  `.github/actions/cr-finding-gate/action.yml` excludes blank-bodied review
  nodes from `n_reviews` entirely, so this entry's shape (an empty CR
  acknowledgement review as the only on-head node) takes the existing
  `n_reviews == 0` StatusContext branch — `SUCCESS` ⇒ pass, exactly the
  disambiguation proposed here. The paired rate-limit-description guard keeps a
  `SUCCESS` whose description says "Review rate limited" from counting as review
  evidence (CR repo learning, 2026-08-11), so the fix cannot fail open on an
  unreviewed head. Decision-table cases pinned in
  `tests/bats/cr_finding_gate.bats`: "a genuinely completed SUCCESS with no
  review node still passes", "a rate-limited SUCCESS is NOT review evidence",
  plus the blank-reply block.)

## What happened

CodeRabbit posted a `COMMENTED` review node on the PR head
(`e56ac352`) with an **empty body** — its acknowledgement after the one inline
finding was addressed and the thread resolved. No new push followed, so CR never
posted another review.

`.github/actions/cr-finding-gate/action.yml` then took its fail-closed branch:

- `n_reviews > 0` (a review node exists on this head), so the
  `n_reviews == 0` disambiguation via CR's own `CodeRabbit` StatusContext —
  which *was* `SUCCESS` — is never reached;
- the review body carries no `Actionable comments posted: N` header, so
  `n` is empty and `decide()` returns non-terminal;
- the 12×15 s window exhausts and the action posts
  `pending — awaiting CodeRabbit review on current head`.

`CR findings (0 actionable)` is a **required** StatusContext, so the PR sat at
`mergeStateStatus=BLOCKED` with every other check green. Re-running the workflow
re-ran the identical logic and re-posted PENDING — the state is not
self-healing; only a new push or a fresh CR review can clear it, and neither is
guaranteed to arrive.

Unwedged by applying `cr-out-of-band` (user-authorised, "ignore cr") and
re-running the gate, which took the label-override early-exit. That is an
override label standing in for a gate that could not reach a verdict — the shape
we normally treat as a gate escape.

## Why it matters

The fail-closed branch is correct in spirit (a header-less review is not proof
of "0 actionable"), but it has no terminal state for the case where the
header-less review is CR's *final* word on the head. Every such PR needs a human
override, which erodes the label's meaning as a deliberate exception.

## Proposed fix

In the `n_reviews > 0` / empty-`n` path, disambiguate the same way the
`n_reviews == 0` path already does, but only for a **body-less** review:

- if the latest on-head CR review body is empty/whitespace **and** CR's own
  `CodeRabbit` StatusContext on that head is `SUCCESS`, treat it as
  "CR settled with nothing actionable" → `post success`;
- keep the current non-terminal retry for a **non-empty** body that merely lacks
  the header (that really is an unsettled/unexpected state).

An empty body cannot hide a finding count, so this does not reopen the #524
fail-open (which was a *preamble line above* the header, i.e. a non-empty body).

Add a case to whichever harness covers the action's decision table so the
empty-body + StatusContext-SUCCESS combination is pinned.

# Develop-tip health assertion — second occurrence, promote it

- **Category**: infra
- **Priority**: P1
- **Date**: 2026-08-05
- **Observed on**: PR #1957 (installer smokes red on develop for 3 merges)
- **Status**: applied (2026-08-14 — the widening this entry asked for is live in
  `agents/scripts/core/develop-tip-required-green.sh` (file name kept for the
  SessionStart-hook wiring): the pure detector now sweeps the develop tip's
  ACTUAL check runs — required and non-required alike, block-on-any-red parity
  — labeling each not-green check `required|non-required|unknown` so the nudge
  distinguishes "blocks every PR" from "broken post-merge backstop". The
  advisory-name exemption applies ONLY to non-required checks (true merge-gate
  parity: a required red blocks every PR whatever its name says), and an
  unreadable required set degrades the label to `unknown` instead of silencing
  the sweep. CANCELLED gets its own unknown-not-green wording (the exact way
  #1957 hid), with a later green re-run of the same check superseding an
  earlier cancel; the tip's check-run pages are globally time-sorted before
  last-wins so a >100-run tip cannot resurface a stale row. Selftest cases pin
  the #1957 shapes: red non-required backstop reported,
  cancelled-run-as-unknown, advisory silence for non-required only
  (advisory-named REQUIRED red still reported), empty-required-set →
  kind=unknown, superseded-cancel silence, plus the original required-red /
  all-green / absent(PR-only) / in-progress cases.)

## What happened

`Windows x64 installer smoke` and `Windows-on-ARM ARM64 installer smoke` went RED
on develop at #1957 and stayed red while #1959 and #1960 merged on top. Nothing in
the pipeline asserts the develop tip is green before the next merge, and #1957's own
develop run was **cancelled** by a superseding push, so the failure never even
announced itself on the PR.

This is the *identical class* the 2026-07-10 / #1698 postmortem already named and
proposed a gate for — "required check goes red on develop and nobody notices until it
blocks the next PR". That proposal was filed as a follow-up and never landed. This is
its second occurrence, with a worse variant: the red checks here are **non-required**
post-merge backstops, so even the block-on-any-red inheritance that surfaced #1698
did not fire.

## Proposed action

Land the assertion the #1698 entry proposed, widened to non-required checks:

- A `develop-tip-required-green.sh` (or an extension of
  `agents/scripts/core/postmortem-owed.sh`'s sweep) that queries the develop tip's
  check conclusions — **required and non-required alike**, matching the merge-gate
  allow-list philosophy in `AGENTS.md` § Merge gates — and raises a loud, attributable
  SessionStart nudge naming the PR whose merge introduced each red.
- Treat a **cancelled** post-merge run on develop as unknown-not-green, since that is
  precisely how #1957 hid.

## Why it matters

Post-merge backstop jobs are the *only* coverage for code PR checks structurally cannot
run (here: a ~20-30 min LTO publish build). If nothing reads their result, they are
decorative — the break sat on develop across three merges and was found by an ad-hoc
adversarial review, not by the system.

- 2026-08-05 · claude-code · [tooling] · P1 — `test-plan-index.sh` derives shipped-plan index dates from `git log --follow`, which squash-merge rewrites — so a plan archived and merged across a midnight boundary reddens `develop` the instant it lands, with no pre-merge state that could have passed

  Observed on PR #1937 (Help > About dialog). Merged `2026-08-05T11:33:25Z` as
  `fce0951c` with the required `Doc anchors + agent contract` terminal-green. The
  develop tip went RED on that same check immediately after:
  `test-plan-index: DRIFT — shipped-plan index out of sync (182 plans in archive)`.
  Full RCA in [`postmortems.md`](../postmortems.md) (2026-08-05 entry).

  Mechanism. `agents/scripts/core/test-plan-index.sh:122-143` resolves each row's
  date with `git log --follow --format=%ad --date=short -- <path>` — the file's
  *first-commit* date. `--follow` is what normally makes this stable across the
  `plans/active/` → `plans/shipped/` move. A squash-merge collapses the branch into
  one commit and the per-file pre-merge history is unreachable from `develop`, so
  `--follow` finds exactly one commit and returns the **squash date**:

      $ git log --follow --format='%ad %h %s' --date=short \
          -- docs/plans/shipped/about-dialog-help-menu.md
      2026-08-05 fce0951c feat(about): About Smatchet dialog under Help, ... (#1937)

  Three conditions, all common: the PR archives a plan *and* commits its index row;
  the repo squash-merges; branch work and merge fall on different calendar days.
  Every plan-shipping PR that spans a midnight hits this.

  Why it is P1 rather than P2: the check is **required**, and a red required check
  on the develop tip is inherited by every open PR's own head under block-on-any-red.
  One late-evening merge blocks the whole queue until someone notices, and nothing
  announces it — `postmortem-owed.sh` keys on merge-instant signals (non-SUCCESS
  checks, override labels, `Revert`, overdue deviations) and this class emits none,
  so it reports "no gate escapes owed" for the PR that caused it.

  Proposed fix — **stop deriving the value from mutable git metadata.** Have
  `--fix` write the resolved date into the plan file as an explicit
  `<!-- plan-date: YYYY-MM-DD -->` marker when a plan is archived, and have the
  generator prefer that marker, falling back to `git log --follow` only for legacy
  plans without one. Content survives squash, shallow clone and staged rename
  identically. This is not a fourth special case — it **retires the two already in
  the script**, both of which exist to paper over the same history lookup: the
  shallow-clone guard (`:45`, `:105`) and the staged-rename sibling-tier fallback
  (`:135-143`, whose comment already cites the #1061 / #1092 archive date-drift
  "twice"). Squash-merge is the third way the same lookup moves under the generator;
  the recurring shape is the defect.

  Concrete next action: add the marker read/write to `test-plan-index.sh`, plus two
  `--selftest` cases — (1) a `shipped/` plan whose only commit is the current HEAD
  still resolves a stable date; (2) a marker date disagreeing with its index row
  FAILs. Migrate existing rows by running `--fix` once to stamp markers from the
  currently-committed dates, so no archived plan's date changes on adoption.

  Paired with: the develop-tip required-green assertion proposed in the
  `2026-07-10 · PR #1698` postmortem and never landed. This is its second instance —
  a gate that can only go red *after* the merge needs a detector that looks after
  the merge.

  **Applied 2026-08-11 (mechanism); back-catalogue migration still PENDING.**
  `test-plan-index.sh` now reads a `<!-- plan-date: YYYY-MM-DD -->` marker in
  preference to git, and `--fix` stamps one into any plan **the current change
  touches** that lacks it. `--check` never writes, so the required gate stays
  read-only. Stamping is diff-scoped deliberately: an unscoped `--fix` turned
  the CI autosync into a bulk migration — with all 188 archived plans unstamped,
  the first PR to trigger it had 188 bot-authored marker commits pushed onto its
  branch (192 files, past CodeRabbit's 100-file review limit, required CR gate
  wedged; observed live on PR #1999). That mass stamp was **reverted** and the
  scope added; migrating the back catalogue is now a deliberate one-shot behind
  `SMATCHET_PLAN_STAMP_ALL=1`, to land as its own reviewed PR.

  An earlier draft of this block claimed the 188-plan migration had landed and
  that shallow-clone `--check` was "demonstrated end to end". **As of this
  writing it has not**: 1 of 188 shipped plans carries a marker, git is still
  consulted for the other 187, and shallow-clone `--check` stays git-dependent
  until the migration PR lands. What WAS verified: the migration built from
  `INDEX.md`'s own committed rows leaves `INDEX.md` byte-identical, and with it
  applied, `--check` flips from DRIFT to pass on a shallow clone — demonstrated
  on a branch, then reverted with the rest of the mass stamp.

  **One claim in the proposal is wrong and should not be carried forward.** The
  entry says the marker "retires the two special cases already in the script"
  (the shallow-clone guard and the staged-rename sibling fallback). It does not.
  Both are still needed at STAMP time: a new plan is stamped from git exactly
  once, and that one read must be correct — on a shallow clone it would not be,
  and for a freshly `git mv`'d plan the sibling fallback is what resolves it at
  all. What the marker changes is the *exposure*: from every run on every clone
  forever, down to a single deterministic moment on the author's full-history
  checkout. That is a large win, but it is a narrowing, not a retirement, and
  deleting those guards on the strength of this entry would reintroduce the drift
  at the one moment it still matters.

  Two `--selftest` cases were added as asked, in a throwaway repo whose plan has
  exactly ONE commit dated today — the post-squash shape, which cannot be staged
  inside this repo: (a) the row carries the marker date, not the commit date;
  (b) a marker disagreeing with its committed row FAILs `--check`. Disabling
  marker precedence fails (a).

  While adding them, the file's PRE-EXISTING negative assertion turned out to be
  vacuous — it self-exec'd a mode-100644 script, so `126 Permission denied`
  satisfied it — and the first version of the new cases was vacuous too, via
  `set -e` inside a `||` operand. Both fixed here; the class is filed as
  [`asserts-failure-marker-does-not-prove-the-negative-is-reachable`](applied.md).

  Not addressed: the paired develop-tip required-green assertion. Still open, and
  still the right second layer — this fix removes the cause, not the class of
  "green on the PR head, red on develop".

  Status: applied
  Last-reviewed: 2026-08-11

- 2026-08-05 · claude-code · [tooling] · P2 — The gate-poller filters bot review threads out of its user-comment gate, but `required_conversation_resolution` counts them — so the poller can report all-clear on a PR GitHub will never merge

  Observed on PR #1937 (Help > About dialog). After the missing `CR finding gate`
  check-run was resolved (see
  `2026-08-04-required-check-cancelled-while-pending-wedges-poller.md`), the head
  was 43 SUCCESS / 5 SKIPPED / 0 fail, `cr-out-of-band` was set with a
  `cr-disposition:` marker, and the poller printed `User: 0`. GitHub still
  reported `mergeStateStatus=BLOCKED` and refused the merge.

  Cause: branch protection on `develop` sets
  `required_conversation_resolution: {"enabled": true}`, and GitHub counts **every**
  unresolved review thread — including bot-authored ones. Ten unresolved CodeRabbit
  threads were open on the PR. The poller's gate #3 deliberately excludes them:
  `agents/scripts/core/merge-gates.d/10-gate-filter.sh:210-212` selects only threads
  with `.author.__typename != "Bot"` and a login other than `ORCH_USER`. Gate #2 (the
  CodeRabbit gate) passes on `APPROVED` / `COMMENTED + 0 actionable` and is separately
  waivable via `cr-out-of-band` — but that label waives the **poller's** gate. GitHub
  branch protection has never heard of it. So both poller gates read green while the
  thing actually holding the merge was a count neither of them measures.

  Why it matters beyond this PR: the divergence is silent and it fails in the
  expensive direction. The poller's own output is what an operator (or the
  merge-watcher) reads to decide whether to keep waiting, and it says the PR is
  ready. The only signal to the contrary is the opaque `mergeStateStatus=BLOCKED`
  line, which names no cause. On #1937 this cost the full ~90 min budget and a
  manual GraphQL sweep to discover the ten threads and resolve them one by one.
  The `cr-out-of-band` label makes it *worse*, not better: waiving the CR gate is
  precisely the situation in which unresolved CR threads are expected to remain,
  so the label reliably steers into the wedge.

  Proposed fix: project the **unfiltered** unresolved-non-outdated thread count as a
  new field alongside the existing user count, and on a `mergeStateStatus=BLOCKED`
  poll where every other gate passes, emit an actionable BLOCK naming it — e.g.

      BLOCK: mergeStateStatus=BLOCKED with all gates green; 10 unresolved review
             thread(s) (0 user, 10 bot) and branch protection requires conversation
             resolution. Resolve them or the merge will never unblock.

  The thread nodes are already fetched by the same GraphQL query, so this is a jq
  projection change, not an extra API call. Gate the message on the repo actually
  having `required_conversation_resolution` enabled (one `gh api
  repos/{o}/{r}/branches/{base}/protection` read, cached per run) so it does not
  fire spuriously on repos without it.

  Deliberately **not** proposed: making the poller resolve bot threads itself. That
  is a merge-blocking judgement call — auto-resolving CR threads would silently
  discard findings, which is exactly the failure mode `cr-out-of-band` already has a
  `cr-disposition:` attestation to prevent.

  Concrete next action: add the unfiltered-thread-count field to
  `agents/scripts/core/merge-gates.d/10-gate-filter.sh` and the BLOCK branch to
  `agents/scripts/core/merge-gates.sh`, with a `tests/bats/merge_gates.bats` case
  pinning the "all gates green + BLOCKED + N bot threads" path to the actionable
  message. Also worth a line in `docs/agent-rules/merge-gates.md` § CodeRabbit gate:
  `cr-out-of-band` waives the poller's gate only — it does not waive branch
  protection's conversation-resolution requirement.

  Update (2026-08-13): shipped as proposed. `10-gate-filter.sh` projects the
  unfiltered unresolved-non-outdated thread counts (fields 31 total / 32
  user-authored; the projection grew 31 -> 33 fields with the fail-closed
  count assertion updated in lockstep), and `merge-gates.sh` names the cause on
  the poll it appears: when `mergeStateStatus=BLOCKED` with every other gate
  green and threads open, it emits the actionable BLOCK with the (user, bot)
  split, gated on the base branch actually requiring conversation resolution
  (one branch-protection read cached per run; env seam
  `MERGE_GATES_CONV_RES_REQUIRED` mirrors pr-blocked-why.sh's; a probe miss
  reports "may require" rather than staying silent). A user-authored open
  thread never takes this path — it reds the user-comment gate, which already
  names itself; the line is reserved for the invisible bot-only case, and it
  points at `pr-blocked-why.sh` for the per-thread classification. merge-gates.md
  § Per-PR overrides now states the general rule: out-of-band labels waive the
  poller's gates only, never branch protection. Six bats cases pin the path
  (named / conv-res-disabled silent / unknown hedged / zero threads silent /
  other-gate-red silent / user-thread reds the user gate instead).

  Status: applied
  Last-reviewed: 2026-08-13

# Bucket-E drivers boot against the developer's real `%LOCALAPPDATA%\Smatchet\imgui.ini`

- **Category**: test
- **Priority**: P1
- **Date**: 2026-08-05
- **Status**: open (one driver fixed; the rest unaudited)

## What happened

`scripts/dev/test-ui-window-expand.sh` failed `TabBarToggleClickExpandsThenMinimizes`
deterministically on one machine and passed on another. The geometry assertion
(`RectFull.Max.x == BarRect.Max.x`) passed; only `ItemClick` on the docked toggle
could not land:

```
Failed to move window 'Views - Jira###SmatchetViewsDashboard'! While trying to make
space to click at (1245.50,102.50) over window 'WindowOverViewport_.../DockSpace_...'.
Error 'MouseMove: Unable to Hover 0xED75AA4E ... Hovered id was 0x00000000 in ''.
```

Root cause: the spawned app resolves its user-data dir via
`ConfigManager::GetPlatformSharedUserDataDirectory()` → `%LOCALAPPDATA%\Smatchet`,
so it loads whatever `imgui.ini` the developer's own interactive session last
wrote. Windows the test never opens were left floating over the dock tab bar,
making the docked control unhoverable. Pointing `LOCALAPPDATA` at an empty
throwaway dir took the same binary from 6/7 to **7/7**.

A first fix that assumed z-order (park overlapping floaters, then click) was
written, built, and re-run — it changed nothing, because the offender was itself
docked. Worth recording: the *hypothesis* was wrong, and only swapping the
environment proved which layer owned the failure.

## Why it matters

Any bucket-E (and bucket-C screenshot) lane that boots the real exe inherits
per-developer, per-boot dock state. That is a silent correctness hole in the test
bucket that is supposed to keep visual features out of the human-verification
pause: a green local run is not evidence the layout under test was the shipped
default, and a red one is not evidence of a product defect.

## Proposed fix

1. Audit every driver that spawns the exe (`scripts/dev/test-ui-*.sh`,
   `test-screenshot-diff.sh`) for the same exposure.
2. Factor the isolation into one shared helper rather than the per-driver
   `mktemp -d` + `export LOCALAPPDATA/APPDATA/XDG_CONFIG_HOME` + `trap` block now
   in `test-ui-window-expand.sh`.
3. Consider a first-party knob instead of environment shadowing —
   `ConfigManager::GetPlatformSharedOverrideRef()` already exists as a
   programmatic hook and would not depend on the platform's env-var names.

## Evidence

- Same binary, same host: real `LOCALAPPDATA` → `Passed: 6 Failed: 1`;
  throwaway dir → `Passed: 7 Failed: 0`.
- Resolution order read from `Source/Core/src/Config/ConfigManager_PathUtils.cpp`
  (`GetPlatformSharedUserDataDirectory`, `GetImGuiSettingsPath`).
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — scripts/dev/lib/ui-test-driver.sh gives every driver a throwaway SMATCHET_USER_DATA profile (seeded or unseeded per driver, pin + keep-on-exit honoured) with the update check off; 29 of 30 drivers use it (20 newly isolated) and a ratchet in tests/bats/ui_driver_filters.bats keeps it that way. test-ui-automation-reload-hooks-race.sh opts out with a reason (its race needs the profile's Lua-consent state) until that test seeds its own consent. The drivers cannot run here — the next Windows bucket-E run is the behavioural check.
  Status: applied (2026-10-04)

# "No new .ps1" rule is documented but ungated

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-05
- **Observed on**: PR #1960 (kill-PowerShell plan closeout)

## What happened

`docs/harness/SETUP.md` § Windows-only shims now states the invariant that the five
remaining `.ps1` files are the complete set and that "Adding a **new** `.ps1` anywhere
else is a regression". Nothing enforces it:

```
grep -rn "ps1" agents/scripts/project/test-lint-rules.sh scripts/dev/test-docs.sh
```

returns nothing. The whole point of the 7-slice kill-PowerShell plan was to stop the
toolchain re-growing; a prose-only invariant will not.

## Proposed action

Add a delta-gated `lint-rules.d/` rule (`no-new-ps1`) asserting that the tracked `.ps1`
set equals the five files named in the SETUP.md table, with the usual
`SMATCHET_DEVIATION(rule=no-new-ps1; ...)` escape for a genuinely Windows-only addition.
Cheap: `git ls-files '*.ps1'` diffed against the documented list. Pair it with a check
that each kept shim still carries its `# Last remaining PowerShell file` marker line and
stays ASCII-only / no BOM / LF, which SETUP.md also states and also does not enforce.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — scripts/dev/test-no-new-ps1.sh (auto-enrolled by test-all.sh): tracked .ps1 basenames must equal docs/harness/SETUP.md's Windows-only shims table, each with the 'Last remaining PowerShell file' marker, ASCII-only, no BOM, no CR; escape SMATCHET_DEVIATION(rule=no-new-ps1; …); --selftest + no_new_ps1.bats; rule id documented next to the table.
  Status: applied (2026-10-04)

# `clang-format` reflows a long `SMATCHET_DEVIATION` comment and silently breaks its parser

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-05
- **Status**: applied — option 2 on 2026-08-16, option 3 on 2026-10-03

## What happened

`.clang-format` sets `ColumnLimit: 120`. A `SMATCHET_DEVIATION(rule=…; reason=…; owner=…;
revisit=…)` comment with a descriptive `reason=` exceeds that, so `clang-format -i` wraps it
onto a second `//` line. Every deviation consumer (`dup_audit.py`, `test-lint-rules.sh`, the
`deviation-overdue` gate) matches the directive on a **single line**, so the wrapped form is
not a syntax error — it simply stops being a deviation, and the rule it was escaping fires
again with no explanation of why the comment above it exists.

Hit while adding the four duplication exemptions for the window-expand feature: the reason
strings had to be shortened to fit rather than written for the reader.

## Why it matters

Two gates disagree about the same line — the formatter, which every pre-push hook runs, and
the lint gates, which block the merge. The failure is silent in the direction that matters
(escape lost, not escape wrongly granted), and the fix pressure lands on comment prose
instead of on the tooling.

## Proposed fix

Pick one:

1. Teach the deviation parsers to join a `//` continuation line before matching, so wrapping
   is harmless. Cheapest, keeps `ColumnLimit` untouched.
2. Add `CommentPragmas: '^ SMATCHET_DEVIATION'` to `.clang-format` so the formatter leaves
   these comments alone. One line, but the long comment then visibly overruns the column
   limit.
3. Add a gate that fails on a wrapped `SMATCHET_DEVIATION(` with no closing `)` on the same
   line — turns the silent loss into a loud one without changing either tool's behaviour.

Option 2 plus option 3 is the smallest combination that is both correct and self-policing.

## Update — 2026-08-16 (deviation re-evaluation)

Measured rather than predicted. 73 live markers exceed `ColumnLimit`; 58 survive only because
someone hand-wrapped them in `// clang-format off` / `// clang-format on`. The remaining **15 are
rewritten by `clang-format` today** — 8 `duplication`, 4 `bare-json-parse-untrusted`, 3
`app-controller-fan-in`. Proof end-to-end on `Source/Core/src/Tracker/PlaneProjectScope.cpp` using
the real `scan_bare_json_parse_file`: in-tree → clean, after `clang-format` →
`bare-json-parse-untrusted`, gate FAILS. `scripts/dev/pre-ship.sh:429` runs `clang-format -i` on
every changed first-party TU before the gate, and no CI job checks formatting, so the drift is
invisible until someone touches one of those 12 files.

**Option 2 applied**: `.clang-format` now carries `CommentPragmas: '^ *SMATCHET_DEVIATION'`.
Verified across all 110 marker-holding TUs — marker lines clang-format would rewrite goes 15 → 0,
with no other formatting change attributable to the pragma.

**Option 3 deliberately deferred**, and the reason matters: 47 markers in the tree are *already*
wrapped and already invisible to every gate (see
[`2026-08-16-wrapped-deviation-markers-invisible-to-gate.md`](applied.md)).
A wrapped-marker gate added today red-walls CI on all 47 at once. Sequence is: un-wrap the 47, then
add the gate. This entry stays open until option 3 lands.

Separately, the same audit found and fixed a second parser defect the original entry did not
anticipate: `DEV_RE`'s `[^)]*` body capture truncates at the first `)`, so a `reason=` containing a
parenthetical hid `revisit=` from `deviation-overdue` on 40 markers while still granting the
suppression — see [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../audits/DEVIATION_AUDIT_2026-08-16.md) § S1.

**Option 3 applied 2026-10-03**: once the 2026-12-31 deviation batch had unwrapped every marker, the
absolute `deviation-malformed` rule (`dev_marker_malformed` in `lint-rules.d/00-common.sh`, emitted
by `scan_file_rules`, enforced whole-tree by `compute_wide_violations`) landed. It fails any
`SMATCHET_DEVIATION(` that does not close on its own line or lacks `rule=` / `reason=` / `owner=` /
`revisit=`. Covered by `tests/fixtures/lint_rules/deviation-malformed.cpp` and four `lint_rules.bats`
cases, each field check pinned by a mutation that fails them.

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

  Status: applied (2026-08-14 — fix (1) landed in
  `agents/scripts/core/merge-gates.sh`: in the unexplained required-missing
  branch, once every other check is terminal-green, the poller probes the
  head's workflow runs; ≥1 CANCELLED run with zero queued/in-progress is the
  "never, not late" evidence — it emits `BLOCK: required-missing-cancelled`
  naming each run id with its `gh run rerun <id>` one-liner and exits with the
  new distinct code 8 instead of burning the remaining budget. Probe failure,
  any active run, or no runs at all (the pure creation-lag shape) keep
  polling, and the outage exit-7 path is regression-pinned to still fire —
  six cases in `tests/bats/merge_gates.bats` (§ cancelled-while-pending).
  The two-surface observation is now a section in
  `docs/agent-rules/ci-required-check-pattern.md` ("Two surfaces per
  workflow"). Fix (2) is filed separately as
  `2026-08-14-cr-finding-gate-pending-queue-collapse.md` per this entry's own
  scoping — it needs a workflow-design decision, with option 1 (drop the
  concurrency block) recommended there.)
  Last-reviewed: 2026-08-14

- 2026-08-04 · orchestrator (py-probe-exec-validation-gate) · [tooling] · P2 — resolve-only python probes: single-candidate hard-require guards (~15 scripts) stay unflagged by shell-lint rule 9, and `_lock-json.py` emits CRLF-terminated lock-table rows that silently void every plan-lock on Windows
  Details: on Windows `python3` on `PATH` is the Microsoft Store *App Execution Alias* stub — `command -v python3` resolves it and returns 0, but running it prints an install banner and exits non-zero. PR #1936 fixed the bats suites; this follow-up fixed the four remaining **pickers** (`doctor.sh`, `merge-watcher-stuck-nudge.sh`, `agent-eval-run.sh`, `test-tooltip-wrapwidth.sh`, plus `lock-table-cache.sh` `_ltc_pybin()`) and added rule 9 (`SHELL_LINT_PY_PROBE`). Two residuals were left deliberately. **(1)** The single-candidate shape (`command -v python3 || exit 2` … then a bare `python3 tools/foo.py`) is not flagged on purpose: with one candidate nothing can be mis-selected, so it fails loudly at the first invocation instead of silently preferring a stub over a working interpreter. Clearing it is not a probe edit — every downstream call site must become `"$PY" foo.py` across ~15 scripts — and `test-shell-lint.sh` has no WARN tier and no delta-gating, so a rule covering the shape would have to ship with all ~15 already converted. **(2)** `agents/scripts/core/_lock-json.py` writes its TSV rows with `sys.stdout.write("\n")` through a Windows **text-mode** stdout, so every row arrives CRLF-terminated and the trailing CR lands on the last field — the path. `_ltc_norm_path`'s exact-match in `lock-table-cache.sh` then fails for *every* locked path: a held plan-lock silently covers nothing. This was masked until now — the pre-fix `_ltc_pybin()` picked the stub, `_lock-json.py` never ran, `ltc_covering_slug` returned rc 2 ("undetermined") and the write-set guard took its documented fail-open path. Fixing the probe is what surfaced it: an inert mandatory concurrency guard started running and immediately mismatched. Mitigated at the shell normalization chokepoint (`_ltc_norm_path` strips CR before its existing backslash normalization); not fixed at the source because the module carries `from __future__ import print_function`, so `sys.stdout.reconfigure(newline="\n")` (py3.7+) needs a guarded py2-tolerant form that does not belong in a probe-fix diff. Both residuals are the same class the gate was added for — *silent-wrong on one platform* — and (2) is worse than the originally reported bug.
  Concrete next action: (a) add `agents/scripts/core/lib/resolve-py.sh` with one exec-validating `resolve_py()`, convert the ~15 single-candidate guards to source it, then widen rule 9 to flag any python probe not routed through it (one shape to enforce instead of a regex guessing at intent); (b) fix `_lock-json.py` newline handling at the source (reconfigure the stream or write bytes), keep the shell-side strip as defence in depth, and add a test asserting no `\r` survives in a lock-table row on Windows.
  Status: applied (flipped at archival)
  Last-reviewed: 2026-08-04

- 2026-08-04 · orchestrator · [tooling] · P3 — `scripts/dev/with-msvc.ps1` signals its own "no usable MSVC toolchain" failure **in-band**, as exit code `78`, on the same channel it uses to propagate the wrapped command's exit code; a wrapped command that ever returns 78 would be misreported by `build.ps1` as a missing toolchain
  Details: Raised by CodeRabbit on PR #1933 (Major, `scripts/dev/with-msvc.ps1:27`) and accepted as a
    known residual rather than fixed there. The wrapper's tail is `& $exe @rest; exit $LASTEXITCODE`,
    so *every* code it emits other than its own four failure paths belongs to the child. PR #1933
    already moved those four from `2` to `78` precisely because `2` was a **live** collision — cmake,
    ninja and ctest all return 2 routinely, so an ordinary failed build printed "no usable MSVC
    toolchain (see the with-msvc line above)" and a winget install hint. `78` was picked to sit
    outside the range those tools use, which downgrades the collision from reachable to theoretical:
    the only command `build.ps1` ever wraps is `powershell -File build_and_run.ps1`, whose failure
    codes are cmake/ninja/ctest's 1/2/8 and PowerShell's 1. Nothing in the call graph returns 78
    today. But the ambiguity is structural, not numeric — any single-channel scheme has it, and a
    future wrapped command (or a future `build.ps1` that wraps something else) re-opens it.
  Concrete next action: give the wrapper an **out-of-band** status channel and stop overloading the
    exit code. Cheapest shape that fits the existing sandbox harness: have `with-msvc.ps1` write a
    sentinel file (path passed in via an env var, e.g. `SMATCHET_MSVC_STATUS_FILE`) on each of its
    four failure paths, and have `build.ps1` key its install hints on *that file's presence* rather
    than on `$LASTEXITCODE -eq 78`, while still propagating whatever code came back. Two call sites
    must move together: `build.ps1`'s else-branch currently reads only `$LASTEXITCODE`, and
    `scripts/dev/local/test-build-wrapper.ps1` stubs the wrapper **by exit code alone** — test 7's
    3-row table (78/msvc, 78/clang, wrapped-exit-2) would need its stub to write the sentinel too,
    and gains a fourth row: a wrapped command that returns 78 *without* the sentinel must propagate
    78 and print no hints. That fourth row is the assertion the current design cannot make, and is
    the reason to do the work at all. Est ~0.5d.
  Cross-ref: `scripts/dev/with-msvc.ps1` (`$ToolchainMissingExit`, the four `exit $ToolchainMissingExit`
    sites, and the `& $exe @rest; exit $LASTEXITCODE` tail that creates the sharing);
    `build.ps1` (the `-eq 78` branch); `scripts/dev/local/test-build-wrapper.ps1` (test 7);
    `docs/agent-rules/build.md` § Entry point (the documented contract that would change);
    `docs/plans/shipped/dev-onboarding-first-run-quickstart.md` § Deviations (the bullet that
    requires this entry); CodeRabbit thread on PR #1933 (comment `3714335162`).
  Status: applied (flipped at archival — implemented against the bash ports that superseded the
    PS files after PR #1956 deleted them: `scripts/dev/with-msvc-env.sh` writes a
    `toolchain-missing` sentinel to `$SMATCHET_MSVC_STATUS_FILE` via a `fail_env` helper on every
    own-failure exit, `build.sh` keys its install hints on the sentinel's presence while
    propagating the exit code, and `scripts/dev/local/test-build-wrapper.sh` test 7 gained the
    fourth row — wrapped exit 78 *without* the sentinel propagates with no hints)
  Last-reviewed: 2026-08-15

- 2026-08-04 · orchestrator · [tooling] · P2 — six gate scripts under `agents/scripts/core/` still carry the **resolve-only python probe** that PR #1936 (`f423605a`, 2026-08-04) fixed everywhere else: `command -v python3` matches the Windows Store App Execution Alias, which resolves on PATH but exits non-zero on run, so the "no python" guard never fires and the script dies mid-run instead of skipping cleanly
  Details: Hit live while validating a PR body — `bash agents/scripts/core/check-pr-intent.sh <body-file>`
    printed "Python was not found; run without arguments to install from the Microsoft Store" and exited
    **49**, not the documented `2` (no python3). The presence probe at `check-pr-intent.sh:20` had already
    passed, so the fail-closed infra-error path was bypassed and the caller got an undocumented exit code
    from the stub. Worked around by symlinking a real interpreter onto PATH ahead of the alias.
    #1936 swept `tests/bats/**` plus `issue-sweep.sh` / `migrate-bugs-to-issues.sh` to the probe-EXECUTE
    form (`"$c" -c ""`); the gate scripts were out of that PR's scope. Remaining sites:
    `check-pr-intent.sh:20`, `sort-applied-md.sh:26`, `test-autonomous-debug-loop.sh:22` (all `python3` —
    these are the ones that hit the alias today, since `python3` has no real entry on a stock Windows
    dev box), plus `test-agent-contract.sh:77`, `test-lint-hook-split.sh:411`, `test-skill-load-log.sh:16`
    (`python` — currently resolve to a real interpreter here, but identical shape and identical failure on
    a machine where only the alias exists). Impact is local-dev-experience only: CI (Ubuntu) has a working
    `python3`, so no gate is wrong on the ship-line — same blast radius as the check-5 emoji false negative
    shipped alongside this entry (PR #1938).
  Concrete next action: apply #1936's canonical probe-EXECUTE form to the six sites — iterate the
    `python3 python py` candidate list and select on `"$c" -c "" >/dev/null 2>&1`, not on `command -v`.
    Mechanical; est ~0.5h including a `tests/bats` assertion that a stub named `python3` which exits
    non-zero is rejected rather than selected. Consider hoisting the resolver into `agents/_shared/` so
    the next script cannot reintroduce the resolve-only shape, and a `test-shell-lint.sh` rule matching
    `command -v python` used as an interpreter-selection probe.
  Cross-ref: PR #1936 (`f423605a`, the exec-validate sweep + the canonical form); PR #1938 (where this
    surfaced); `agents/scripts/core/check-pr-intent.sh` :20; `agents/scripts/core/sort-applied-md.sh` :26;
    `agents/scripts/core/test-autonomous-debug-loop.sh` :22; `agents/scripts/core/test-agent-contract.sh` :77;
    `agents/scripts/core/test-lint-hook-split.sh` :411; `agents/scripts/core/test-skill-load-log.sh` :16.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — all six named scripts source agents/scripts/core/lib/resolve-py.sh (which executes the interpreter), covered by tests/bats/resolve_py.bats and the shell-lint probe-shape rule.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-04 · orchestrator (visual-validation exception from the about-dialog-help-menu ship) · [test] · P3 — the About modal's two manual verification items (open/populate/copy round-trip, and the Help-menu enablement matrix) have no automated coverage; needs a bucket-E scenario
  Details: the About feature landed with items 10 and 11 of its § Verification (actual) signed off by a human eye-test under the `AGENTS.md` visual-validation exception, because the diff touches `Source/Core/src/Ui/SmatchetAboutUi.cpp` + `SmatchetUI_MainMenu.cpp` and there is no bucket-C/E coverage for either surface. The pure half IS covered — `tests/Core/AboutInfo.test.cpp` asserts `GatherAboutInfo` / `BuildAboutReportText` / `ParseDepManifest`, and `tests/bats/about_buildinfo.bats` asserts the CMake codegen — so what is uncovered is strictly the ImGui layer: that `app.about.open` actually opens the modal, that its six sections render non-empty rows, and (the higher-value half) that the Help-menu reachability restructure holds. That restructure is a real regression risk: `drawMenuBarHelpMenu` was deliberately moved OUTSIDE the `trackerLocked` `BeginDisabled` bracket in `SmatchetUI_MainMenu.cpp` so About stays clickable in exactly the backend-down state where a user needs to copy build info, with the two pre-existing Help items re-disabling themselves per-item. Anyone who later "tidies" that bracket back would silently re-break About in the one state that matters, and nothing in CI would say so.
  Concrete next action: add a bucket-E (ImGui Test Engine) scenario that (a) dispatches `app.about.open` and asserts the "About Smatchet" popup is the top-most modal with a non-empty version row and a non-empty deps row, (b) presses Escape and asserts the popup closes and the cached snapshot is released (`g_ui.aboutInfo == nullptr`), and (c) drives the enablement matrix — with `cfg.BackendHasBeenReachable` false, assert the Help menu itself is enabled, the About item is enabled, and the two tracker-dependent items report disabled. Register it in the bucket-E driver plus a `scripts/dev/test-ui-about.sh` wrapper (the fixture-gated bucket-E wrappers are not in the main required lane, so enrollment needs the same treatment as the omnibar suite). Cross-ref: `docs/plans/shipped/about-dialog-help-menu.md` § Verification (actual) items 10-11; `Source/Core/src/Ui/SmatchetAboutUi.cpp`; `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp` (`drawMenuBarHelpMenu` placement); `Source/Core/src/Commands/AppViewCommands.cpp` (`app.about.open`).
  Resolution: duplicate — folded 2026-10-04 (backlog-sweep-2026-10) into test/2026-08-18-about-modal-no-bucket-c-or-e-coverage.md, which now carries both asks (open/populate/copy round-trip and the Help-menu enablement matrix).
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

- 2026-08-04 · orchestrator · [test] · P2 — `scripts/dev/test-ui-agent-proposal-store-sqlite.sh` is an **orphaned bucket-E driver**: no test TU named `AgentProposalStore` exists anywhere in `tests/`, so the driver can only ever fail (`ui_test.run matched 0 tests for filter 'AgentProposalStore'`) — and it goes unnoticed because `test-all.sh` **SKIPs** it on any tree without a built `Smatchet.exe`, which is the common local state
  Details: Surfaced running the full `scripts/dev/test-all.sh` twice for slice 2 of
    `dev-onboarding-first-run-quickstart` — once on a worktree with a built `ninja-ui-test-msvc`
    exe (the driver ran and FAILed) and once on a worktree with no build (the driver SKIPped and
    the suite looked cleaner). That skew is the real lesson: the same commit produces a different
    failure count depending on whether a build directory happens to exist, so a genuinely broken
    driver reads as "environment noise" rather than breakage. `grep -rn AgentProposalStore tests/`
    returns nothing — the only hits are inside the driver itself (its header comment at :3 crediting
    "slice 9" and its `FILTER="${UI_TEST_FILTER:-AgentProposalStore}"` default at :12). The driver's
    zero-match fail-closed behaviour is itself correct and was added deliberately by #1192
    (the 6-HIGH fail-open gate cluster fix) — it is doing its job; what is missing is the TU it was
    written to drive, which either never landed or was removed without removing its driver.
  Concrete next action: decide the disposition and act on it in one PR. Either (a) restore the
    missing `tests/ui/agent_proposal_store.test.cpp` + its registry entry in
    `tests/ui/ui_tests_registry.cpp` if the SQLite-backed proposal-store lifecycle is still meant to
    be covered, or (b) delete the driver. Then add the *class* gate so this cannot recur silently: a
    check that every `scripts/dev/test-ui-*.sh` driver's `UI_TEST_FILTER` default resolves to at
    least one registered test name — cheap as a bats case that greps each driver's default filter and
    asserts a matching `SmatchetRegister*Tests` / `IM_REGISTER_TEST` name exists in `tests/ui/`. That
    runs with no build and no exe, which is exactly the gap that let this sit. Est ~0.5d.
  Cross-ref: `scripts/dev/test-ui-agent-proposal-store-sqlite.sh` (the orphan);
    `tests/ui/ui_tests_registry.cpp` (where the registration would live);
    PR #1192 (added the zero-match fail-closed behaviour that makes the orphan visible);
    `scripts/dev/test-all.sh` (the SKIP-when-no-exe path that hides it).
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — the agent-proposal-store driver is deleted (no AgentProposal/ProposalStore code remains); the omnibar driver is now test-ui-grid-search-apply.sh (FILTER=GridSearch, 3 live cases); tests/bats/ui_driver_filters.bats asserts every driver's default filter names a string literal in tests/ui and goes red on a reintroduced orphan.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

# Empty-body CodeRabbit reply reviews defeat the CR gate's StatusContext fallback

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-03
- **Observed on**: PR #1928 (`feat/ui-thread-sync-reads`), head `0b077b5e`
- **Status**: applied (2026-08-14 — both proposed fixes are live in
  `.github/actions/cr-finding-gate/action.yml` `decide()`, landed on the #1996
  re-occurrence of this class. Fix 1 verbatim: blank-bodied review nodes are
  filtered out of BOTH the `n_reviews` count and the latest-body selection, so a
  reply-only head falls back to the `CodeRabbit` StatusContext exactly as
  proposed — pinned by `tests/bats/cr_finding_gate.bats` ("a blank-bodied thread
  reply is NOT a review (the #1996 wedge)", "whitespace-only body", "findings
  survive a LATER blank reply", "a clean review followed by a blank reply still
  passes"). Fix 2 landed NARROWER than proposed, deliberately: only the
  observed clean-with-nitpicks header-less shape passes (two-sided
  discrimination pinned by the shape-3 bats block); an outside-diff-only
  header-less body stays fail-closed because outside-diff comments ARE findings
  — current CR format posts them alongside an `Actionable comments posted: N`
  header, so the auto-pass this entry proposed would fail open on them. That
  residual resolves to PENDING plus the gate's full-review auto-nudge rather
  than a wedge.)

## Friction

`CR findings (0 actionable)` — a **required** commit StatusContext — sat PENDING
for hours on a PR whose every other check was terminal-green, with no operator
action able to clear it.

`decide()` in [`.github/actions/cr-finding-gate/action.yml`](../../../.github/actions/cr-finding-gate/action.yml)
branches on `n_reviews`, the count of CodeRabbit review nodes whose
`commit.oid == headRefOid`:

- `n_reviews == 0` → fall back to CodeRabbit's own `CodeRabbit` StatusContext;
  `SUCCESS` ⇒ pass.
- `n_reviews > 0` → grep the latest on-head review body for
  `Actionable comments posted: N`. A **missing header is fail-closed**
  (`return 1`, non-terminal, poll retries).

The wedge: replying to a CodeRabbit inline thread with
`addPullRequestReviewThreadReply` creates a **review node with an empty body**,
and CodeRabbit's auto-acknowledgement of that reply creates **another**. On
#1928 five thread replies produced three `coderabbitai[bot]` reviews on head
`0b077b5e` with `bodylen=0`.

That drove `n_reviews` from 0 to 3 — pushing the gate out of the branch where
the green `CodeRabbit` StatusContext would have passed it, and into the
header-grep branch, where three empty bodies can only ever fail closed. **The
act of responding to the review is what broke the gate.**

Worse, it is not self-healing on the same head. When CodeRabbit later posted a
genuine 6415-char review, every finding was an *"Outside diff range comment"* —
a body shape that carries **no** `Actionable comments posted:` header at all. So
the header grep still returned nothing and the gate still failed closed. Once a
head reaches this state the only exit is a **new head**.

Neither existing entry covers this: the
[adaptive-ratelimit](applied.md)
one is about CR never *arriving*; the
[stuck-blockers](applied-2026-07.md) one is about
findings that *are* parseable. This is CR having arrived and the gate being
structurally unable to read it.

## Cost

~3 h of a session spent diagnosing and attempting recovery on an
otherwise-mergeable PR, ending in a no-op push purely to reset the head. Two
false starts along the way: a GraphQL review-body dump that returned empty
(needed the REST `repos/.../pulls/N/reviews` projection to reveal `bodylen=0`),
and a CR re-trigger whose gate run was then cancelled by concurrency with no
re-run.

## Proposed fix

Two independent changes, either of which unwedges this class:

1. **Ignore empty-body reviews in the `n_reviews` count.** A zero-length body
   carries no verdict, so it should not be evidence that CR reviewed this head.
   Filter `bodylen == 0` out before the branch, which restores the
   StatusContext fallback for the reply-only case.
2. **Treat a non-empty body with no actionable header as `0 actionable`, not as
   a retry.** The "Outside diff range comment" shape is a legitimate CR output
   with genuinely zero actionable in-diff comments. Fail-closed is right for a
   *truncated/unknown* body, but a body that parses as a complete CR review with
   no header is a pass, not an indefinite retry.

## Operator guidance until fixed

- **Do not reply to CodeRabbit threads via `addPullRequestReviewThreadReply`**
  while `CR findings (0 actionable)` is a required check. Address findings in
  the commit message on the fixing commit instead.
- If a head is already wedged, do not reach for `cr-out-of-band` on a code PR —
  push a new head. The label exists for a rate-limited CR, and using it here
  would wave un-reviewed code through.

## Status

Applied — see the Status header line for the disposition.

- 2026-08-03 · orchestrator · [tooling] · P1 — a shipped plan's § Deviations / § Implementation log can assert a delivery that never landed and **no gate reads it**: `msvc-build-onboarding-hardening.md:85` claims "`build_standalone.ps1` (plan file 1) already had the MSVC bootstrap from slice 1" — `git log -S vcvars` on that file is **empty across all history**; the promised vcvars/vswhere env import was never written, in any revision
  Details: Surfaced while validating the `dev-onboarding-first-run-quickstart` plan, whose § Context
    premise ("MSVC bootstrap already exists, just needs a root entry point") was inherited from that
    line. Chain: PR #493 planned "locate `vcvars64.bat` through `vswhere.exe` … import via
    `cmd /c \"...vcvars64.bat && set\"`" for `build_standalone.ps1`. PR #495 (`da36b45f`, 2026-05-28)
    shipped the *other* items and closed the row with the § Deviations line above. The blameless root
    cause is a **name conflation**: the file did contain a `Use-Msys2Ucrt64Environment` call (an **MSYS2**
    UCRT64 env bootstrap) which #495 replaced with the retirement `throw` — that pre-existing env-setup
    call was read as "the bootstrap", so the row was closed as already-done rather than dropped. The
    file's only `vswhere` use is `Get-VsWherePath` (:71-83), which locates **MSBuild.exe**, not vcvars.
    Nothing contradicted the claim: § Verification (actual) lists `test-build-wrapper.ps1` 3/3 green, but
    all three cases test the msys2-retirement throw, the `Exe :`/`Time:` print, and the stale-sibling
    table — **none exercises an MSVC env bootstrap**, so a passing verification block is fully consistent
    with the capability being absent. And `postmortem-owed.sh --list` returns "no gate escapes owed": the
    nudge reads merge signals (non-SUCCESS checks, override labels, `Revert`, overdue deviations), so an
    *untrue prose claim* in a doc is structurally invisible to it. The claim then sat load-bearing for
    ~2 months and seeded a false premise into a downstream plan.
  Concrete next action: add gate rule **`plan-claim-anchor`** —
    `agents/scripts/core/test-plan-claim-anchors.sh`, joining the existing plan-doc gate family
    (`test-plan-index.sh` / `test-plan-ref-integrity.sh` / `test-markdown-links.sh`) in the
    "Doc anchors + agent contract" doc-validation job. Rule: inside a plan's **§ Deviations** or
    **§ Implementation log** sections only, a line matching the pre-existing-delivery claim set
    (`already had|has|have|exists|existed|implemented|landed|shipped`, `was already`) MUST carry a
    verifiable citation — a markdown link or backticked ref with a `:<line>` suffix, or a `#<PR>` /
    commit-sha reference. Delta-gated vs `origin/develop` and baseline-grandfathered like every other
    rule (measured 2026-08-03, claim pattern above + anchor pattern `:<line>` / `#<2+ digits>` /
    7-40-char hex sha, scanning `## Deviations` / `## Implementation log` sections only:
    **38 such claims across 30 files, 25 unanchored** — all grandfathered; only NEW claims must
    anchor. The `--selftest` re-derives this baseline rather than hardcoding it). Escape:
    `SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=…; owner=…; revisit=…)` for claims about state
    outside the repo (e.g. `solo-merge-review-gate.md:91` cites GitHub branch-protection API state,
    which has no `file:line`). This does not prove a claim true — it forces the author to point at the
    code, and **there is no line to point at for a vcvars import that does not exist**, which is exactly
    where #495 would have stopped. Est ~0.5d (bash gate + `--selftest` + AGENTS.md contract-card row).
    Explicitly NOT proposed: extending `postmortem-owed.sh` — this class carries no merge signal, so
    detection belongs at doc-gate time, not at merge-nudge time.
  Cross-ref: `docs/plans/shipped/msvc-build-onboarding-hardening.md` (:82 impl-log, :85 the false
    § Deviations claim, § Verification (actual) 3/3 non-covering tests); `scripts/dev/local/build_standalone.ps1`
    (:71-83 `Get-VsWherePath` → MSBuild only, zero vcvars); PR #493 (`a9058b96`, plan) / **PR #495**
    (`da36b45f`, the escaping PR); `scripts/dev/with-msvc.ps1` :39-139 (where a real vcvars import DOES
    live — the capability exists in the tree, just not in the file the plan named);
    `docs/plans/shipped/dev-onboarding-first-run-quickstart.md` (downstream plan that inherited the false
    premise); `docs/self-improvement/postmortems.md` (ledger entry).
  Status: applied (this entry shipped as test-plan-claim-anchors.sh; Status flipped at archival)
  Last-reviewed: 2026-08-03

- 2026-08-03 · orchestrator · [test] · P1 — **every** bucket-E `--spawn` driver hung to its full timeout on a debug/asserts build, and the surfaced error named the wrong cause: the child process passed its tests, then tripped `"You need to call ImGui::DestroyContext() BEFORE ImGuiTestEngine_DestroyContext()"` during teardown and died before printing the result envelope, so the parent reported `{"code":"timeout","hint":"Try --timeout=<larger-ms> or --frames=<smaller-n>"}` — a hint that is actively misleading (no timeout value can fix a teardown assert, and `CliDispatch.cpp:476` ignores `pa.timeoutMs` on the `--spawn` path anyway)
  Details: Surfaced writing the bucket-E TU for slice 2 of `dev-onboarding-first-run-quickstart`.
    Isolated as pre-existing by running an **untouched** sibling driver
    (`scripts/dev/test-ui-annotate-prefs-persist.sh`), which hung identically. Mechanism: the test
    engine is created per `ui_test.run` and destroyed while the app's ImGui context is still alive
    (the app outlives any single `ui_test.run`), and ImGui's settings-save path runs against the
    engine's context during that window. Fixed in this slice by setting `io.ConfigSavedSettings = false`
    in `Source/Core/src/Commands/Scenarios/UiTestScenario.cpp` — the scenario has no use for persisted
    ini state, and disabling it removes the teardown write entirely. Two failure-visibility problems
    remain and are the real lesson: (1) a child that dies after passing is indistinguishable at the
    parent from a child that never finished; (2) the timeout hint asserts a remedy the code path does
    not implement. Note: bucket-E **CI-lane** flake remediation (llvmpipe/Mesa collapse to
    `Passed:0 Failed:73`) is separately in flight in the `bucket-e-gate-escape-pm` /
    `bucket-e-residual-fix` worktrees — this entry is the **local driver/teardown** class, not that one.
  Concrete next action: two small changes in `Source/Core/src/Commands/CliDispatch.cpp` around the
    `--spawn` wait. (a) On child exit **without** a parsed envelope, distinguish the cases: report
    `code:"child-died"` with the child's exit code and the tail of its log instead of `code:"timeout"`,
    and only report `timeout` when the child is still alive at the deadline. (b) Either honour
    `pa.timeoutMs` in the `scenarioWaitMs = (frames / 60 + 30) * 1000` computation at :476, or drop
    `--timeout` from the hint string — a hint naming a flag the path ignores costs every future
    investigator the same detour. Est ~0.5d. Optional follow-on: a bats case that plants a child
    which exits non-zero after printing a PASS line and asserts the parent reports `child-died`.
  Cross-ref: `Source/Core/src/Commands/Scenarios/UiTestScenario.cpp` (the `io.ConfigSavedSettings = false`
    fix); `Source/Core/src/Commands/CliDispatch.cpp:476` (the `scenarioWaitMs` computation that drops
    `pa.timeoutMs`); `scripts/dev/test-ui-tracker-first-run-setup.sh` + `scripts/dev/test-ui-annotate-prefs-persist.sh`
    (drivers that both hung pre-fix); `docs/plans/shipped/dev-onboarding-first-run-quickstart.md`
    § Deviations (the out-of-plan infra-fix entry).
  Status: applied (flipped at archival — the spawn code had moved to `Source/Standalone/` in the
    CliCommandRunner god-file split by implementation time. Part (b) was already resolved upstream:
    `ScenarioWaitMs` (CliArgCoercion.cpp) honours `pa.timeoutMs`, so the `--timeout` hint is
    accurate. Part (a) implemented: `LaunchEphemeralInstance` (CliSpawn.cpp) now hands back a
    move-only `SpawnedChild` tracker (Windows keeps `pi.hProcess` instead of closing it at launch;
    POSIX keeps the fork pid), `PollSpawnedChild` WNOHANG-probes liveness, and
    `AwaitSpawnResultFile` (CliDispatch.cpp) interleaves the result-file wait with that probe — a
    child observed dead without an envelope (after a 250 ms flush-grace re-check) reports
    `code:"child-died"` with the child's exit code + a sanitized 2 KiB log tail + the log path,
    exit `kExitHandler`; `code:"timeout"` is only reported when the child was never observed dead.
    `ExitCodeForErrorCode` maps `child-died` explicitly, pinned in CliExitCodes.test.cpp. The
    optional bats follow-on (plant a child that dies after a PASS line) is not automatable without
    a real crashing spawn child — deferred with the CliExitCodes contract test as the deterministic
    stand-in.)
  Last-reviewed: 2026-08-15
