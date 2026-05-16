# Harness setup

Smatchet ships **harness-agnostic** agent definitions:

- [`AGENTS.md`](../../AGENTS.md) at the repo root — project rules, delegation table, capability map.
- [`agents/*.md`](../../agents/) — one file per delegated agent (per the [agents.md spec](https://agents.md/)).
- [`agents/_shared/`](../../agents/_shared/) — shared skills + token-tracking scripts any harness can wire.

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`) are **gitignored** — they're local build output that links into the canonical `agents/` tree and copies a small number of templates. After cloning the repo, run the setup script for the harness you use:

| Harness | Setup command | Details |
|---|---|---|
| Claude Code | `bash scripts/setup-harness.sh claude-code` | [claude-code/setup.md](claude-code/setup.md) |
| Codex / OpenAI Agents | `bash scripts/setup-harness.sh codex` (no-op confirmation) | [codex/setup.md](codex/setup.md) |
| Cursor | `bash scripts/setup-harness.sh cursor` | [cursor/setup.md](cursor/setup.md) |
| Aider / generic | Manual — paste agent files from `agents/` as needed. No adapter dir. | — |

Windows users can substitute `pwsh scripts/setup-harness.ps1 <name>`.

## Why links + copies, not a tracked mirror

The setup script uses **directory junctions** (Windows) / **symlinks** (Unix) for agent definitions and shared skills so edits to `agents/*.md` are picked up by the harness immediately — no sync step, no banner injection, no drift-check.

Templates that the user might locally tweak (`settings.json`, hook shell scripts, `CLAUDE.md`) are **copies**. The script preserves user-modified copies on re-run.

## Adding a new harness

1. Create `docs/harness/<name>/setup.md` with the recreation steps.
2. If the harness needs template files, place them under `docs/harness/<name>/`.
3. Add a `setup_<name>()` function to `scripts/setup-harness.sh` + `.ps1`.
4. Add a row to the table above.
5. Add `.<name>/` to `.gitignore`.
