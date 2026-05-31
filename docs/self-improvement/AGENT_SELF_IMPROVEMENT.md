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

Applied entries are archived immediately to [`self-improvement/categories/applied.md`](self-improvement/categories/applied.md).

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
2. Orchestrator reads, dedupes, and **appends to the END of the matching category
   file** (`docs/self-improvement/categories/<category>.md`) — **not** this index
   file. Use the exact § Format block (date · agent · `[category]` · `P<0-3>` —
   title; then Details / Concrete next action / Status / Last-reviewed).
3. **Immediately sync the count index in the SAME commit** —
   `bash agents/scripts/core/test-backlog-counts.sh --fix` rewrites the § Index
   table from actual file counts (or hand-bump the one row). The pre-push gate
   `test-backlog-counts.sh` **rejects any drift**, so a skipped sync fails the
   push. (This is the #1 trip-wire when adding an entry — do it before you commit.)
4. When evidence accumulates (mentioned by ≥2 agents OR blocks the same
   workflow ≥3 times), apply: edit the relevant agent prompt(s) in `agents/`
   or AGENTS.md; flip Status to `applied`; move the entry to `applied.md` —
   **then re-run `--fix`** (two counts changed: the category −1, `applied` +1).

> **Common failures this prevents:** (a) appending to this index file instead of
> a category file; (b) forgetting the count sync → pre-push `test-backlog-counts`
> rejection; (c) on *archive*, bumping only the category count and not `applied`.
> `--fix` handles all counts at once — run it after any add / archive / remove.

## Triage cadence

Sweep when (a) opening any PR that touches `agents/`, (b) any single live
category file exceeds ~20 open items, or (c) a P0 entry has aged ≥7 days
without movement.

## Index

| Category | Live count | File |
|---|---|---|
| bug         | 14  | [self-improvement/categories/bug.md](self-improvement/categories/bug.md) |
| process     | 27  | [self-improvement/categories/process.md](self-improvement/categories/process.md) |
| tooling     | 40  | [self-improvement/categories/tooling.md](self-improvement/categories/tooling.md) |
| infra       | 16  | [self-improvement/categories/infra.md](self-improvement/categories/infra.md) |
| test        | 18  | [self-improvement/categories/test.md](self-improvement/categories/test.md) |
| security    | 14  | [self-improvement/categories/security.md](self-improvement/categories/security.md) |
| external    | 1   | [self-improvement/categories/external-blockers.md](self-improvement/categories/external-blockers.md) |
| applied (archive) | 144 | [self-improvement/categories/applied.md](self-improvement/categories/applied.md) |

> **Count maintenance**: each "Live count" is the number of `^- 20YY-MM-DD` entries in the linked file (`grep -c '^- 20' self-improvement/categories/<file>.md`). The applied-archive count is the same `grep -c '^- 20' self-improvement/categories/applied.md`. `agents/scripts/core/test-backlog-counts.sh` runs at the pre-push gate (`test-all.sh` discovery) and refuses if any row diverges from the actual file. Update the row in the same commit that adds / archives / removes an entry, or run `bash agents/scripts/core/test-backlog-counts.sh --fix` to rewrite the table from current file counts.
