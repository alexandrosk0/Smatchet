# Agent self-improvement — instructions

Suggestions emitted by delegated agents (canonical at `agents/`, mirrored to `.claude/agents/` for Claude Code auto-discovery) for improving the agent ecosystem itself — prompt tweaks, missing context, redundant steps, new-subagent candidates, tooling gaps.

The main thread appends new entries to the live data file (dedupe against existing). Periodically triage and apply real wins to agent prompts; close out items that landed by deleting them from the data file (git history preserves them).

## Format

```
- YYYY-MM-DD · <agent-name> · [shortcut|process|tooling|context|new-agent] — one-line description
  Details: (optional, single paragraph or short bullet list — context that explains why this would help)
  Status: open | applied | rejected (with reason)
```

Categories:

- **shortcut** — a step the agent finds itself doing manually that could be encoded in its prompt as a default
- **process** — workflow friction: redundant steps, wrong order, missing handoff between agents
- **tooling** — missing CLI / static-analyzer / vexp invocation that would speed things up
- **context** — context the agent had to discover during the task that should be pre-loaded in its prompt
- **new-agent** — subsystem / task pattern that recurs and would warrant its own subagent

## Workflow

1. Delegated agents end every report with a `## Self-improvement` section. Empty is the common case and explicitly fine — agents only flag real friction.
2. The orchestrator reads the section, dedupes against the live data file, appends new entries with date + source agent + category.
3. When an entry has gathered enough evidence (mentioned by ≥ 2 agents, or blocks the same workflow ≥ 3 times), apply it: edit the relevant agent prompt(s) in `agents/`, and close out the entry. Per-harness adapters (`.claude/agents/` etc.) are junctioned/symlinked by `scripts/setup-harness.sh` and pick up canonical edits immediately.

## Triage cadence

Sweep the data file when:

- Opening any PR that touches `agents/`
- The list exceeds ~20 open items

---

> Live entries: [`../backlog/AGENT_SELF_IMPROVEMENT.md`](../backlog/AGENT_SELF_IMPROVEMENT.md).
