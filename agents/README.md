# `agents/` — project agent definitions

This directory holds Smatchet's **project-specific** subagents. The generic, portable agents, shared skills and gate scripts live in the agent layer, [`the-unwilling-agentic-bunch`](https://github.com/alexandrosk0/the-unwilling-agentic-bunch), mounted here as the [`agent-layer/`](../agent-layer/) submodule. Every file follows the [agents.md spec](https://agents.md/), so any harness — Claude Code, Codex / OpenAI Agents, Cursor, pi, generic — can read them with no conversion.

Layout (portable / project split — see [`docs/PORTABILITY.md`](../docs/PORTABILITY.md)):

- `agent-layer/agents/core/<name>.md` — **portable** generic engineering roles (architect, build-doctor, code-review, …), reusable in another project; project-specific values come from [`project.config.json`](../project.config.json).
- `agents/project/<name>.md` — **project-specific** subsystem-bound agents (tracker-backend, grid-engine, lua-binder, mcp-toolsmith, offline-sync, unreal-bridge, p4-annotate, command-system, ui-host).
- `agent-layer/agents/_shared/skills/` — skill definitions that more than one harness can wire (auto-linked by `agent-layer/agents/scripts/core/setup-harness.sh`'s skills glob; see the directory for the current set).
- `agent-layer/agents/_shared/workflows/` — **portable** saved Claude-Code `Workflow` scripts (deterministic multi-agent fan-out), auto-linked into the gitignored `.claude/workflows/` by `setup-harness.sh`'s workflows glob; resolved by name via `Workflow({name})`. See [`agent-layer/docs/agent-rules/workflow-orchestration.md`](../agent-layer/docs/agent-rules/workflow-orchestration.md) for when a Workflow is sanctioned + the fan-out-safe roster.
- `agents/project/workflows/` — **project-specific** saved `Workflow` scripts that embed Smatchet literals (paths, subsystem names) and so can't live in the layer's purity-gated `_shared/workflows/`; linked into the same `.claude/workflows/` by the same `setup-harness.sh` loop, resolved identically by name. Current: `historical-review-sweep`.
- `agent-layer/agents/_shared/token-tracking/` — `SubagentStop`-style hook + statusline renderer + slash-skill definition that any harness can wire to log per-agent token usage. See [`_shared/token-tracking/README.md`](../agent-layer/agents/_shared/token-tracking/README.md) for the wiring contract.

Harnesses discover agents flatly at `.claude/agents/*.md`; `agent-layer/agents/scripts/core/setup-harness.sh` materialises that as flat per-agent links into `agent-layer/agents/core/` and `agents/project/`. **After a pull that moves the `agent-layer` pointer, run `git submodule update --init` and re-run `bash agent-layer/agents/scripts/core/setup-harness.sh claude-code` to regenerate the flat links.**

## Edit the source, never `.claude/`

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`, `.pi/`) are **gitignored**, apart from the tracked `.cursor/BUGBOT.md`. They're regenerated locally from `agents/project/` and the layer by `bash agent-layer/agents/scripts/core/setup-harness.sh <name>`.

Adapters are **links** (junctions / symlinks / hardlinks) into those trees wherever the harness allows — so an edit to `agents/project/ui-host.md` is visible to Claude Code immediately, no sync step required. A generic agent (`agent-layer/agents/core/…`) is edited in the layer repo and reaches this project through a pointer bump; an edit made inside the `agent-layer/` mount is not part of this repo's history and never ships from here.

See [`agent-layer/docs/harness/SETUP.md`](../agent-layer/docs/harness/SETUP.md) for per-harness setup instructions.

## Project rules

Repo-wide rules + the agent delegation table live in [`AGENTS.md`](../AGENTS.md) at the repo root, which imports the layer's [`AGENTS.md`](../agent-layer/AGENTS.md).
