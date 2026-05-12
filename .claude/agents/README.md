# `.claude/agents/` — auto-generated mirror

This directory is an **auto-generated mirror** of `../../agents/` at the repo root.

It exists so Claude Code's hardcoded subagent-discovery path (`.claude/agents/`) finds the definitions. **Do not edit files here directly** — your changes will be silently overwritten the next time someone runs `scripts/sync-agents.sh`.

## Where to edit

Canonical agent files live at **`agents/<name>.md`** at the repo root. That's where humans (and harnesses like Codex following the [agents.md spec](https://agents.md/)) read and write.

## How to sync

After editing any file under `agents/`:

```bash
# bash
bash scripts/sync-agents.sh

# or PowerShell
pwsh scripts/sync-agents.ps1
```

Both scripts:
- Copy every `agents/*.md` into this directory, injecting a YAML-comment warning at the top of the frontmatter.
- Remove mirror files whose canonical source has been deleted.
- Are idempotent — running twice produces no diff.

## How to verify the mirror is in sync

```bash
bash scripts/check-agents-mirror.sh
```

Exits 0 if the mirror matches a fresh regeneration; non-zero with a diff if it doesn't. Suitable for CI / pre-commit hooks.

## Why dual-located

Claude Code auto-discovers subagents only from `.claude/agents/`. Other harnesses (Codex, Cursor, Aider, generic) follow the [agents.md spec](https://agents.md/) and read `agents/` at the repo root. Maintaining a single canonical source at `agents/` + auto-syncing a mirror here is the simplest way to keep both worlds happy without per-harness configuration.
