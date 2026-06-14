# Workflow fleets — authoring multi-agent fan-outs (load on-demand)

Trigger: **authoring a multi-agent `Workflow` fan-out or background agent fleet** (survey swarms, audit fleets, salvage miners — anything that spawns several subagents over one task). Stub in [`AGENTS.md`](../../AGENTS.md) § Project rules § On-demand rule-docs points here.

Every rule below is a written-down diagnosis from the 2026-06-10/11 audit-fleet debacle (a 4.1M-token fleet died on a session limit with zero durable output, then three salvage waves each died of a different cause) plus the survey-fleet compaction incident (2 of 5 agents ballooned to ~130k tokens; one auto-compaction took 5.4 min and threw away 115k of paid context). Violating one of these is no longer "bad luck" — it is a rule violation.

## Failure inventory (what actually killed fleets)

| # | Failure mode | Observed | Rule that prevents it |
|---|---|---|---|
| 1 | Session/token limit kills the whole fleet at once — all-or-nothing, zero durable output | 2026-06-10 audit fleet, 4.1M tokens lost | § Checkpoint contract |
| 2 | TPM starvation: fan-out agents inherit a large-context model, N agents share one rate limit, everything crawls | 2026-06-11 salvage wave (inherited `fable-5[1m]`) | § Model pinning |
| 3 | Context overflow: an agent must Read an input file bigger than its window can survive | 2026-06-11 salvage wave (oversized transcript) | § Input-size pre-flight |
| 4 | Permission deny-all on out-of-workspace paths kills background agents mid-run | 2026-06-11 salvage wave (`~/.claude` session-dir reads) | § In-workspace staging |
| 5 | Runtime deletes the workflow run dir (journal + cached agent results) on kill — resume impossible | observed **twice** 2026-06-11 (audit fleet + wave-4: journal + 27 cached results destroyed) | § Checkpoint contract |
| 6 | Auto-compact inside a subagent: broad scope balloons context, compaction burns minutes + tokens, UI shows a stall | survey fleet, 2 of 5 agents ~130k | § Per-agent scoping |

## Two fan-out mechanisms — know which one you're launching

The rules below were written from **background-fleet** deaths, but this project has **two** distinct fan-out mechanisms with different failure modes. Know which you're using before applying a rule:

| | Background fleet (bash) | In-process `Workflow` tool |
|---|---|---|
| Spawns | bash agents writing `build/<slug>/results/*` | `agent()` calls inside `parallel()` / `pipeline()` |
| Durable output | result files on disk (the system of record) | each `agent()` return value + `resumeFromRunId` cache |
| Concurrency ceiling | shared-account TPM (§ Concurrency) | runtime cap `min(16, cores−2)` live slots |
| Kill cost | run-dir wiped, results survive on disk | run-dir wiped, **journal + cached results gone** unless agents also wrote to repo files |

**Cross-applies to both**: § Per-agent scoping, § Model pinning, § Input-size pre-flight. A `Workflow` `agent()` launched from a 1M-context session inherits that model and starves the shared TPM exactly like a bash miner — pin `model: 'sonnet'` on every fan-out agent.

**In-process-only rules** (the bash-fleet § Checkpoint contract / § In-workspace staging / § Salvage runbook do **not** map cleanly — a schema-returning `agent()` has no results file):

- **Cap fan-out WIDTH at the slot count.** Items in one `parallel()`/`pipeline()` call run `min(16, cores−2)` at a time; the rest queue. A 16-wide fan-out on a 12-slot machine starts 12 and queues 4 — the queued tail shows as a "stall" in `/workflows` though nothing failed (this was the 2026-06-13 arch-analysis friction). Size the fan-out array to the slot count: merge clusters (≤12 readers, not 16) or stage in explicit waves.
- **Prefer `pipeline()` over `parallel()` unless a stage truly needs all prior results.** `parallel()` is a barrier — the next phase waits on the slowest agent *plus the queued tail*. `pipeline()` streams each item through its stages independently, so an early map flows into the cross-cut stage while a later map is still running. Reach for the `parallel()` barrier only when a stage genuinely needs the whole prior set at once (dedup, early-exit-on-zero, "compare against the other findings").
- **Durable output is the return value + resume, not a results file.** A killed `Workflow` loses its journal + cached `agent()` results; recover with `resumeFromRunId` (same-session, best-effort). For a long/expensive in-process fan-out whose loss would hurt, have each agent *also* write its deliverable to a repo file (borrow the bash-fleet § Checkpoint contract) so a kill can't zero it.

`fleet-preflight.sh` check 6 WARNs when a script's largest fan-out array tops the local slot count.

## Per-agent scoping — finish under ~80k tokens

A survey/reader agent that auto-compacts has already failed: compaction throws away most of what was paid for and looks like a stall in the UI. Scope every fan-out agent to finish **well under ~80k tokens**:

- **Split fat dimensions** into more, narrower agents (CI separate from packaging; one subsystem per reader) instead of fewer broad ones. A prompt like "examine all CMakeLists + scripts + workflows and answer 4 sub-questions" makes a reader enumerate the tree until it overflows.
- **Put explicit budget lines in the prompt**: "stay under ~40 tool calls", "Grep + targeted Read only, never read whole large files", "return as soon as the listed questions are answered".
- **Prefer `agentType: 'caveman:cavecrew-investigator'`** for locate-style sweeps — compressed output, refuses scope creep.
- **Chain two stages (locate → read)** when a dimension genuinely needs huge exploration — a cheap locator returns `file:line` targets, a second agent reads only those — rather than one mega-agent.

A broad-scope prompt with a windowed-read directive (a budget line, `offset/limit`, a `file:line` target, or the scope-refusing `cavecrew-investigator` agentType) is a **MUST**, not a nicety — `fleet-preflight.sh` check 7 WARNs a fan-out `agent()` that names a directory / glob scope but carries none. Pass agents **repo-relative** `file:line` targets, never bare basenames (a bare `Foo.cpp` reads as File-not-found + a wasted recovery turn that compounds compaction pressure — `fleet-preflight.sh` check 8, AGENTS.md § Semantic-search exceptions).

## Model pinning — never inherit a large-context model into a fan-out

Workflow `agent()` calls inherit the main-loop model by default. If the orchestrator session runs a 1M-context model, N inherited fan-out agents share one tokens-per-minute limit and the whole fleet crawls (failure 2). For fleets:

- **Pin `model: 'sonnet'`** (or smaller) on every survey/miner/locator agent. Reserve the big model for the single synthesis agent, if any.
- Never leave `model` unset in a fleet launched from a large-context session.

## Concurrency — cap against sibling sessions, not cores

The runtime caps concurrency at min(16, cores−2), but the real ceiling is the shared account TPM. **Cap background fleets at ≤5 concurrent agents** when any sibling session is live on this machine (check the SessionStart tree banner / `.claude/.active-sessions`); raise only when provably alone.

## Input-size pre-flight — every input ≤ ⅓ of the agent's context window

Before launching, measure every file an agent is told to Read. Anything over ~⅓ of the agent's context window (~65k tokens ≈ 260 KB of text for a 200K window) must be **pre-split or pre-distilled** — an oversized Read is an unrecoverable overflow crash mid-run (failure 3). Splitting is orchestrator work, not agent work: do it inline before the `Workflow` call.

## Work-list — pass it via `args`, never hard-code it in the body

A fan-out's per-item work-list (PR numbers, file paths, ticket ids) belongs in **`args`**, not a `const LIST = [...]` baked into the script. A baked list goes stale every run and forces a script edit per batch (the friction that motivated persisting [`historical-review-sweep`](workflow-orchestration.md)). Two rules make `args` reliable:

- **`args` always reaches the script as a string — `JSON.parse` is mandatory, not optional.** Even when you pass a real JSON array (`args: [1180, 1181]`), this harness serialises it: inside the script `typeof args === 'string'`, `Array.isArray(args)` is `false`, and `args` is the literal text `"[1180, 1181]"`. Proven by a 0-agent probe: `{argType:'string', isArray:false, rawValue:'[1180, 1181, 1182]', parsedIsArray:true}`. So the body must `JSON.parse` before any `.map`/`.length`/`.slice`; skipping it gives the classic "0-agent 14 ms no-op that looks like success".
- **Defensive parse + loud empty-guard in the script.** `parsed = typeof args === 'string' ? JSON.parse(args) : args`, then accept array · `{items:[…]}`, and **`throw`** on an empty resolved list with the correct invocation in the message — never `return` quietly, or a misrouted list reads as a clean success.

## In-workspace staging — everything a background agent touches lives in the repo

Background agents hit permission walls on out-of-workspace paths (failure 4). Stage **all** fleet inputs and outputs under the repo workspace in a gitignored scratch dir — convention: `build/<fleet-slug>/` (e.g. `build/salvage/`). Never point an agent at `~/.claude/**` session dirs, `%TEMP%`, or any absolute path outside the worktree.

## Checkpoint contract — repo files, not run-dir state

The workflow runtime owns its run dir and **deletes it on kill** — journal and cached `agent()` results included (failure 5, observed twice). Resume-from-journal is best-effort, not durable. Therefore:

- **Every fleet agent writes its deliverable to a repo file as it finishes** (`build/<fleet-slug>/results/<agent>.md`), in-prompt, as part of its task — not only as the `agent()` return value.
- The workflow script also appends a one-line progress marker per completed agent (same dir), so a dead fleet's progress is reconstructible from the filesystem alone.
- For long fleets, commit intermediate results (`wip(<fleet-slug>): checkpoint N/M`) so even a worktree wipe can't lose them.

Run-dir artefacts (journal, cached results) are an optimization for live resume, never the system of record.

## Stall watchdog — distinguish crawl from frozen before killing

A "stalled" fleet has two very different causes with opposite remedies. Read the signals before acting:

| Signal | Diagnosis | Action |
|---|---|---|
| Result files / transcript mtimes advancing, slowly | **Crawl** = TPM starvation | Reduce concurrency and/or pin a smaller model; do NOT kill — work is landing |
| Newest transcript mtime static ≥ ~10 min, no new results | **Frozen** = permission prompt or agent death | Kill + enter the salvage runbook; waiting longer buys nothing |

Poll: count of `build/<fleet-slug>/results/*` + newest agent-transcript mtime. Automated by [`workflow-watchdog.sh`](../../agents/scripts/core/workflow-watchdog.sh) (`<fleet-slug> [--nudge]`) — read-only; classifies crawl vs frozen per this table and emits a one-line verdict (or a SessionStart nudge, with `--nudge`, only when frozen). Two polls are needed to tell crawl from frozen (the first sets the baseline).

## Salvage runbook — when a fleet dies with transcripts intact

Transcripts under the session dir usually survive even when the run dir is destroyed. Recovery at ~⅓ the original cost:

1. **Distill first, scriptably**: strip tool-result bodies from each `agent-*.jsonl` transcript, keep final text + assistant findings → slim evidence files in `build/salvage/*.slim.md`. This is a script, not an agent — near-free.
2. **Pre-flight the slim files** (§ Input-size pre-flight: each ≤ ⅓ window; split if not).
3. **Run miner agents over the slim files** — one miner per dead agent's evidence, pinned model, tight prompt ("re-extract the deliverable from this evidence; do not re-explore the repo").
4. Miners obey the § Checkpoint contract from minute one — a salvage wave that dies must itself be salvageable.

## Pre-launch checklist

Before any `Workflow` fleet launch, confirm:

- [ ] Each agent scoped to finish < ~80k tokens; budget lines in every prompt (§ Scoping)
- [ ] `model` pinned on every fan-out agent (§ Model pinning)
- [ ] Concurrency ≤ 5 with live siblings (§ Concurrency)
- [ ] Every input file ≤ ⅓ context window (§ Input-size pre-flight)
- [ ] All inputs/outputs staged under `build/<fleet-slug>/` (§ In-workspace staging)
- [ ] Each agent prompt includes the write-your-result-to-a-repo-file step (§ Checkpoint contract)
- [ ] **In-process `Workflow` tool**: fan-out width ≤ local slots `min(16,cores−2)`; `pipeline()` not `parallel()` unless a stage needs all prior results (§ Two fan-out mechanisms)
- [ ] Every broad-scope agent prompt carries a windowed-read directive; source files are passed as repo-relative `file:line`, never bare basenames (§ Per-agent scoping)

Mechanical validation of this checklist runs via [`fleet-preflight.sh`](../../agents/scripts/core/fleet-preflight.sh) (`<workflow-script> [fleet-dir] [--strict]`) — static-analyses the Workflow script + staged inputs for the checks above (incl. the in-process fan-out-width vs slot-count check, the broad-scope read-discipline check, and the bare-basename path-hygiene check); advisory (WARN lines, exit 0) by default, `--strict` turns any WARN into a non-zero exit to gate a launch.
