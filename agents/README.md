# `agents/` — project agent layer

The generic, portable agents, shared skills and gate scripts live in the agent layer, [`the-unwilling-agentic-bunch`](https://github.com/alexandrosk0/the-unwilling-agentic-bunch), mounted here as the [`agent-layer/`](../agent-layer/) submodule. This directory holds only what embeds Smatchet literals and so cannot live in the layer. Every file follows the [agents.md spec](https://agents.md/), so any harness — Claude Code, Codex / OpenAI Agents, Cursor, pi, generic — can read it with no conversion.

Layout (portable / project split — see [`docs/PORTABILITY.md`](../docs/PORTABILITY.md)):

- `agent-layer/agents/core/<name>.md` — the three **portable** subagents: `architect` (design spanning two or more subsystems or a shared interface), `code-review` (before every commit) and `security-review` (trust-boundary diffs). Project-specific values come from [`project.config.json`](../project.config.json). Everything else runs in the main thread with the skill or doc the root [`AGENTS.md`](../AGENTS.md) on-demand table names.
- `agent-layer/agents/_shared/skills/` — skills (debugging, perf, test authoring, PR-bot triage, git cleanup, …), auto-linked by `agent-layer/agents/scripts/core/setup-harness.sh`'s skills glob; see the directory for the current set.
- `agent-layer/agents/_shared/workflows/` — **portable** saved Claude-Code `Workflow` scripts (deterministic multi-agent fan-out), auto-linked into the gitignored `.claude/workflows/` by `setup-harness.sh`'s workflows glob; resolved by name via `Workflow({name})`. See [`agent-layer/docs/agent-rules/workflow-fleets.md`](../agent-layer/docs/agent-rules/workflow-fleets.md) for when a Workflow is sanctioned.
- `agents/project/workflows/` — **project-specific** saved `Workflow` scripts that embed Smatchet literals (paths, subsystem names) and so can't live in the layer's purity-gated `_shared/workflows/`; linked into the same `.claude/workflows/` by the same `setup-harness.sh` loop, resolved identically by name. Current: `historical-review-sweep`. `agents/project/` holds no agent definitions.
- `agent-layer/agents/_shared/token-tracking/` — `SubagentStop`-style hook + statusline renderer + slash-skill definition that any harness can wire to log per-agent token usage. See [`_shared/token-tracking/README.md`](../agent-layer/agents/_shared/token-tracking/README.md) for the wiring contract.

Project knowledge does not live in agents. Subsystem rules are the leaf `Source/**/AGENTS.md` files registered in [`CONTEXT-MAP.md`](../CONTEXT-MAP.md); procedures are the guides under [`docs/guides/`](../docs/guides/); [`docs/area-map.json`](../docs/area-map.json) maps each path to its owning area and the docs to read before changing it.

Harnesses discover agents flatly at `.claude/agents/*.md`; `agent-layer/agents/scripts/core/setup-harness.sh` materialises that as flat per-agent links into `agent-layer/agents/core/`. **After a pull that moves the `agent-layer` pointer, run `git submodule update --init` and re-run `bash agent-layer/agents/scripts/core/setup-harness.sh claude-code` to regenerate the flat links.**

## Edit the source, never `.claude/`

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`, `.pi/`) are **gitignored**, apart from the tracked `.cursor/BUGBOT.md`. They're regenerated locally from `agents/project/` and the layer by `bash agent-layer/agents/scripts/core/setup-harness.sh <name>`.

Adapters are **links** (junctions / symlinks / hardlinks) into those trees wherever the harness allows — so an edit to `agents/project/workflows/<name>.js` is visible to Claude Code immediately, no sync step required. A layer agent or skill is edited in the layer repo and reaches this project through a pointer bump; an edit made inside the `agent-layer/` mount is not part of this repo's history and never ships from here.

See [`agent-layer/docs/harness/SETUP.md`](../agent-layer/docs/harness/SETUP.md) for per-harness setup instructions.

## Project rules

Repo-wide rules live in [`AGENTS.md`](../AGENTS.md) at the repo root, which imports the layer's [`AGENTS.md`](../agent-layer/AGENTS.md).
