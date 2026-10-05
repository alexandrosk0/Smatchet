# Smatchet — agent entry point

Smatchet is a C++14 tracker client (standalone GLFW/OpenGL and an Unreal DX12 plugin). Its agent rules, agents, skills, hooks and gate scripts live in a separate public repository, [`the-unwilling-agentic-bunch`](https://github.com/alexandrosk0/the-unwilling-agentic-bunch), mounted here as the `agent-layer/` submodule.

@agent-layer/AGENTS.md

## Where the rules are

- **The canonical rulebook is [`agent-layer/AGENTS.md`](agent-layer/AGENTS.md).** Every `AGENTS.md § <section>` reference in this repository resolves there. Claude Code loads it through the import above; other harnesses read that file directly.
- **Mount contract:** `agent-layer/` is the layer (`AGENT_LAYER_ROOT`); this tree is the project (`PROJECT_ROOT`): `Source/`, `docs/plans/`, `docs/self-improvement/categories/`, `project.config.json`.

## First checkout

```bash
git submodule update --init --recursive
bash agent-layer/agents/scripts/core/setup-harness.sh claude-code
```

Re-run both after every pull that moves the `agent-layer` pointer: `.claude/agents/` holds hardlinks that keep the old content until `setup-harness.sh` re-links them.

A fork that wants its own layer overrides the URL locally, in this order (`git submodule sync` overwrites the URL from `.gitmodules`, so it must come first):

```bash
git submodule sync agent-layer && git config submodule.agent-layer.url https://github.com/<you>/the-unwilling-agentic-bunch.git && git submodule update --init --recursive
```

## Host-only pointers

- [`AI_POLICY.md`](AI_POLICY.md) — who is in control, the loop modes, when to stop.
- [`CONTEXT-MAP.md`](CONTEXT-MAP.md) — subsystem registry; each `Source/Core/src/<ctx>/AGENTS.md` overrides any central summary.
- [`project.config.json`](project.config.json) — build presets, perf budgets, lint zones, governance.
- [`docs/plans/`](docs/plans/) — active and shipped plans.
- [`docs/seed-paths.txt`](docs/seed-paths.txt) — the record of what moved to the layer.
