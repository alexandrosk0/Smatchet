# Smatchet project rules

The canonical Smatchet project rules, agent delegation table, semantic-codebase-search policy, self-improvement loop, and harness-adapter section all live in **[`AGENTS.md`](../AGENTS.md)** at the repo root.

This file exists only so Claude Code (which auto-loads `.claude/CLAUDE.md`) picks up AGENTS.md content. The `@`-import below is Claude Code's file-include syntax — other agentic harnesses read `AGENTS.md` directly per the [agents.md spec](https://agents.md/).

@../AGENTS.md
