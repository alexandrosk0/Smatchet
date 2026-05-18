# Agent self-improvement

Suggestions emitted by delegated agents for improving the agent ecosystem itself
— prompt tweaks, missing context, redundant steps, new-subagent candidates,
tooling gaps. Live entries split by category; this file is the index + spec.

## Format

```
- <YYYY-MM-DD> · <agent-name> · [<category>] · P<0-3> — <one-line title>
  Details: (single paragraph or short bullet list — context that explains why)
  Concrete next action: (what unblocks the entry)
  Status: open | deferred | observational
  Last-reviewed: <YYYY-MM-DD>   # default = creation date; bump on each sweep
```

Applied entries are archived immediately to [`agent-self-improvement/applied.md`](agent-self-improvement/applied.md).

## Categories

- **bug** — defect in shipped behaviour (production or test).
- **process** — workflow friction; orchestrator-packet discipline; shortcuts an
  agent finds itself doing manually that should be encoded in its prompt;
  context an agent had to discover that should be pre-loaded.
- **tooling** — missing CLI / lint / vexp invocation / harness automation gap.
- **infra** — build system / CI / scaffolding / new-subagent candidates.
- **test** — test coverage gap, fixture, bucket-E wiring.
- **security** — exploitable surface; secret leakage; sandbox escape; defense-in-depth.

`external-blockers.md` carries entries that can only be resolved outside this
repo (vexp tool, GitHub repo settings).

## Priority

- **P0** — data corruption · exploitable · merge-block.
- **P1** — load-bearing · silent failure · production regression.
- **P2** — test infra · process gap · cross-agent friction.
- **P3** — doc edit · cosmetic · single-agent prompt tweak.

Mandatory on every `open` entry.

## Workflow

1. Delegated agents end every report with `## Self-improvement`. Empty is fine.
2. Orchestrator reads, dedupes, appends to the matching category file (not here).
3. When evidence accumulates (mentioned by ≥2 agents OR blocks the same
   workflow ≥3 times), apply: edit the relevant agent prompt(s) in `agents/`
   or AGENTS.md; flip Status to `applied`; move the entry to `applied.md`.

## Triage cadence

Sweep when (a) opening any PR that touches `agents/`, (b) any single live
category file exceeds ~20 open items, or (c) a P0 entry has aged ≥7 days
without movement.

## Index

| Category | Live count | File |
|---|---|---|
| bug         | 6  | [agent-self-improvement/bug.md](agent-self-improvement/bug.md) |
| process     | 18 | [agent-self-improvement/process.md](agent-self-improvement/process.md) |
| tooling     | 25 | [agent-self-improvement/tooling.md](agent-self-improvement/tooling.md) |
| infra       | 9  | [agent-self-improvement/infra.md](agent-self-improvement/infra.md) |
| test        | 9  | [agent-self-improvement/test.md](agent-self-improvement/test.md) |
| security    | 6  | [agent-self-improvement/security.md](agent-self-improvement/security.md) |
| external    | 3  | [agent-self-improvement/external-blockers.md](agent-self-improvement/external-blockers.md) |
| applied (archive) | 74 | [agent-self-improvement/applied.md](agent-self-improvement/applied.md) |
