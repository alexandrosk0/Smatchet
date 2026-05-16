# `agents/` — canonical agent definitions

This directory holds the **single source of truth** for every Smatchet subagent. Files here follow the [agents.md spec](https://agents.md/) so any harness — Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic — can read them with no conversion.

Layout:

- `agents/<name>.md` — one file per delegated agent (architect, build-doctor, code-review, …).
- `agents/_shared/skills/` — skill definitions that more than one harness can wire (currently `grill-with-docs`, `scratchpad-recall`).
- `agents/_shared/token-tracking/` — `SubagentStop`-style hook + statusline renderer + slash-skill definition that any harness can wire to log per-agent token usage. See [`_shared/token-tracking/README.md`](_shared/token-tracking/README.md) for the wiring contract.

## Edit here, never in `.claude/`

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`) are **gitignored**. They're regenerated locally from this canonical tree by `bash scripts/setup-harness.sh <name>`.

Adapters are **links** (junctions / symlinks / hardlinks) into this `agents/` tree wherever the harness allows — so an edit to `agents/architect.md` is visible to Claude Code immediately, no sync step required.

See [`docs/harness/SETUP.md`](../docs/harness/SETUP.md) for per-harness setup instructions.

## Project rules

Repo-wide rules + the agent delegation table live in [`AGENTS.md`](../AGENTS.md) at the repo root.
