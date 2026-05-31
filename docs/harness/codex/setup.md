# Codex / OpenAI Agents adapter

No local files required. Codex reads:

- [`AGENTS.md`](../../../AGENTS.md) at the repo root — project rules + delegation table.
- [`agents/*.md`](../../../agents/) — individual agent definitions.

Per the [agents.md spec](https://agents.md/), this layout is the entire adapter.

## Verify after clone

```bash
bash agents/scripts/core/setup-harness.sh codex
```

The script prints a confirmation and counts the agent files. It writes nothing — `.codex/` is gitignored and never created.

## Wiring the token-tracking hook

If you want per-agent token telemetry under Codex, wire the `SubagentStop`-style hook documented at [`agents/_shared/token-tracking/README.md`](../../../agents/_shared/token-tracking/README.md) to invoke `agents/_shared/token-tracking/agent-token-log.py` after each subagent run. The hook is harness-agnostic.

## Removing — there's nothing to remove

Codex has no adapter directory. The agents.md spec covers everything.
