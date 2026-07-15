# Workflow multi-agent orchestration

> When a deterministic multi-agent **Workflow** is sanctioned over a plain `Agent`
> delegation, which agents are fan-out-safe, the three hard boundaries, and the
> cost guardrails. Cross-linked from [`delegation.md`](delegation.md) § Parallel
> dispatch (NOT from `AGENTS.md` — it is at its grandfathered line cap; § Delegation
> already routes here through delegation.md).
>
> Plan: [`docs/plans/shipped/adopt-workflow-orchestration.md`](../plans/shipped/adopt-workflow-orchestration.md).
> **Claude-Code-specific** — the `Workflow` tool is a Claude Code capability; other
> harnesses (Codex / Cursor / Aider) fall back to manual § Parallel dispatch.

## What a Workflow is

A `Workflow` is a deterministic JS script that fans subagents out via `agent()` /
`parallel()` / `pipeline()` and collects structured results — control flow (loops,
conditionals, fan-out, barriers) is in the script, not model-driven. It invokes the
repo's own prompts as subagents via `agentType=<name>` (e.g. `agentType: 'code-review'`),
so no new agent code is needed. Saved scripts live in tracked `agents/_shared/workflows/*.js`
and are linked into the gitignored `.claude/workflows/` by `setup-harness.sh`; the
Workflow tool resolves them by name (`Workflow({name: 'pre-merge-review'})`).

## Decision rule — Workflow vs plain Agent vs inline

| Situation | Use |
|---|---|
| One bounded task for one specialist | a single `Agent` call |
| 2–4 independent contracts, one batch, no orchestration logic | **§ Parallel dispatch** (multiple `Agent` calls in one block) — simpler than a Workflow |
| Deterministic **fan-out → barrier → synthesize**, or **pipeline** over a work-list, or **loop-until-dry / loop-until-count**, or **adversarial-verify** (N skeptics per finding) | a **Workflow** |
| The control flow needs cross-item dedup, early-exit on zero, or a judge stage over all results | a **Workflow** (barrier or judge stage) |
| A single-fact lookup you can do yourself | inline — no agent at all |

Reach for a Workflow when the *structure* of the work (how it fans out, what
verifies, what synthesizes) is worth encoding deterministically. For an ad-hoc
2-agent batch, plain § Parallel dispatch is lighter and preferred.

## Fan-out-safe roster — the 7 read-only agents

Default wide fan-out is restricted to agents declaring `read-only: true` — they
touch no files, so any number run concurrently with **zero write-set collision**,
no worktree, no lock:

`architect` · `code-review` · `coderabbit-triage` · `perf-detective` ·
`perf-measure` · `security-review` · `spike-hunter`

Any **write** fan-out (parallel editors / subsystem specialists mutating disjoint
zones) is NOT default-safe — see Boundary 1.

## The two boundaries (hard rules)

1. **Write fan-out needs isolation + a lock check.** Two agents editing the same
   shared clone in parallel clobber each other. A workflow that mutates files MUST
   give each writer its own git worktree (`isolation: 'worktree'` per `agent()`)
   AND pass `bash agents/scripts/core/locks-show.sh` (plan-lock collision check,
   [`delegation.md`](delegation.md)) before dispatch. Default to read-only fan-out;
   reach for write fan-out only with disjoint zones + worktree + lock pass.
2. **The gated ship-loop tail stays orchestrator-owned.** Workflows are scoped to
   **diagnose / review / analyze** phases only. `commit → push → open PR →
   gate-check → squash-merge → git-janitor` is sequential, determinism-critical,
   and **never** wrapped in a Workflow (`ship-loops.md`). A workflow produces
   findings; the orchestrator acts on them through the gated tail.

## Cost guardrails

The token budget is a **shared pool** across the main loop and all workflows
(`budget.spent()` is global). Opus-dominated fan-out is the expensive case.

- **Opus concurrency cap 4–6.** Workflows that recruit Opus subagents keep
  effective concurrency ≤ 6 (the runtime already caps at `min(16, cores-2)`; stay
  well under for Opus). Wide sweeps (10+ agents) must be **sonnet/haiku read-only**.
- **Escalate before unbounded loops.** Any `loop-until-dry`, `loop-until-count`, or
  multi-sample perf workflow that could spend a large, hard-to-predict amount gets a
  cost-estimate `AskUserQuestion` **first** (ties to [`AI_POLICY.md`](../../AI_POLICY.md)
  § Escalate — cost-unbounded → escalate). A `budget.total`-guarded loop is the
  encoded form: `while (budget.total && budget.remaining() > <floor>) { … }`.
- **Cost is observable.** `.claude/.agent-tokens.jsonl` + `agent-tokens-report.py`
  (SubagentStop hook) already record per-workflow spend — no new tracking needed;
  check it after a wide run.

## Saved workflows

Canonical home is tracked `agents/_shared/workflows/*.js` (NOT `.claude/workflows/`,
which is gitignored — `.gitignore:63`). `setup-harness.sh` links each into
`.claude/workflows/` so the Workflow tool can resolve it by name. To add one: drop
the `.js` under `agents/_shared/workflows/`, re-run `bash agents/scripts/core/setup-harness.sh claude-code`.

**Portable vs project-scoped.** `agents/_shared/workflows/` is purity-gated
(`test-portable-purity`) — a script there must embed **no** project literals
(paths, subsystem names, repo-specific commands). A workflow that *must* name
project internals lives in **`agents/project/workflows/`** instead (excluded
from the purity scan); `setup-harness.sh` links that dir into the same
`.claude/workflows/`, so both resolve identically by name.

| Workflow | Home | Shape | Use |
|---|---|---|---|
| `pre-merge-review` | `_shared` | parallel-barrier `code-review` + `security-review` → judge `agent()` (schema-forced) → one ranked deduped verdict + advisory verifier scores | pre-merge review of a PR / local branch diff. `args = {pr: N}` / `{base: 'origin/develop'}` / no-arg = local diff vs `origin/develop` |
| `subsystem-invariant-audit` | `_shared` | discover leaf-AGENTS.md zones at runtime (delegated to a discovery agent — the JS runtime has no shell) → one read-only `code-review` agent per zone vs ONLY that leaf rules → aggregate into one ranked drift report | drift audit of each subsystem against its own leaf-AGENTS.md invariants. `args = {}` (all zones) / `{zones: [...]}` (named dirs). Portable: zero project literal in the `.js` |
| `historical-review-sweep` | `project` | fan-out: one `code-review` agent per merged PR over its survivor digest (`historical-review-survivors.sh`) → severity-tagged findings, aggregated | historical code-review of a merged-PR batch. `args = [<PR numbers>]` (real JSON array, required). Pairs with the `historical-code-review` skill + ledger |

The `.js` is the single source of truth; this doc references + excerpts it, never
re-copies the body (no drift). Canonical excerpt of the judge-after-barrier shape:

```js
// agents/_shared/workflows/pre-merge-review.js (excerpt)
const reviews = await parallel([
  () => agent(reviewPrompt(target), { agentType: 'code-review',     schema: FINDINGS }),
  () => agent(reviewPrompt(target), { agentType: 'security-review', schema: FINDINGS }),
])                                              // barrier: both read-only → zero collision
const verdict = await agent(judgePrompt(reviews.filter(Boolean)),
                            { schema: VERDICT })  // plain agent(): default model, schema-forced
return verdict                                   // ranked, deduped — orchestrator acts via the gated tail
```
