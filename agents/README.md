# `agents/` — canonical agent definitions

This directory holds the **single source of truth** for every Smatchet subagent. Files here follow the [agents.md spec](https://agents.md/) so any harness — Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic — can read them with no conversion.

Layout (portable / project split — see [`docs/PORTABILITY.md`](../docs/PORTABILITY.md)):

- `agents/core/<name>.md` — **portable** generic engineering roles (architect, build-doctor, code-review, …), reusable in another project; project-specific values come from [`project.config.json`](../project.config.json).
- `agents/project/<name>.md` — **project-specific** subsystem-bound agents (tracker-backend, grid-engine, lua-binder, mcp-toolsmith, offline-sync, unreal-bridge, p4-annotate, command-system).
- `agents/_shared/skills/` — skill definitions that more than one harness can wire (auto-linked by `agents/scripts/core/setup-harness.sh`'s skills glob; see the directory for the current set).
- `agents/_shared/workflows/` — **portable** saved Claude-Code `Workflow` scripts (deterministic multi-agent fan-out), auto-linked into the gitignored `.claude/workflows/` by `setup-harness.sh`'s workflows glob; resolved by name via `Workflow({name})`. See [`docs/agent-rules/workflow-orchestration.md`](../docs/agent-rules/workflow-orchestration.md) for when a Workflow is sanctioned + the fan-out-safe roster.
- `agents/project/workflows/` — **project-specific** saved `Workflow` scripts that embed Smatchet literals (paths, subsystem names) and so can't live in the purity-gated `_shared/workflows/`; linked into the same `.claude/workflows/` by the same `setup-harness.sh` loop, resolved identically by name. Current: `historical-review-sweep`.

Harnesses discover agents flatly at `.claude/agents/*.md`; `agents/scripts/core/setup-harness.sh` materialises that as flat per-agent links into the `core/` + `project/` subdirs. **After pulling this split, re-run `bash agents/scripts/core/setup-harness.sh claude-code` to regenerate the flat links.**
- `agents/_shared/token-tracking/` — `SubagentStop`-style hook + statusline renderer + slash-skill definition that any harness can wire to log per-agent token usage. See [`_shared/token-tracking/README.md`](_shared/token-tracking/README.md) for the wiring contract.

## Edit here, never in `.claude/`

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`) are **gitignored**. They're regenerated locally from this canonical tree by `bash agents/scripts/core/setup-harness.sh <name>`.

Adapters are **links** (junctions / symlinks / hardlinks) into this `agents/` tree wherever the harness allows — so an edit to `agents/core/architect.md` is visible to Claude Code immediately, no sync step required.

See [`docs/harness/SETUP.md`](../docs/harness/SETUP.md) for per-harness setup instructions.

## Project rules

Repo-wide rules + the agent delegation table live in [`AGENTS.md`](../AGENTS.md) at the repo root.
