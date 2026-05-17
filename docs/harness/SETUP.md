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

## Optional: coverage tooling (`OpenCppCoverage`)

`OpenCppCoverage` is **Windows-only** and **not required** for normal Smatchet development — `scripts/dev/coverage.sh` exits 2 with a clean install hint when the binary is absent, and CI runners install it via Chocolatey (`choco install opencppcoverage`). Install locally only if you're working on coverage gates / threshold tuning or want to inspect line-coverage in `coverage/coverage-html/index.html`.

Local install options:

- **Chocolatey** (recommended): `choco install opencppcoverage` from an admin PowerShell.
- **Direct download**: grab the latest installer from [github.com/OpenCppCoverage/OpenCppCoverage/releases](https://github.com/OpenCppCoverage/OpenCppCoverage/releases) and add the install dir (default `C:\Program Files\OpenCppCoverage\`) to PATH.

Verify with:

```bash
SMATCHET_DOCTOR_CHECK_COVERAGE=1 bash scripts/dev/doctor.sh
```

This enables the otherwise-opt-in doctor check. The check is `YELLOW`-class (warn-only — the build never fails for missing OpenCppCoverage); without the env var the check is skipped entirely so contributors not working on coverage don't see noise.

POSIX runners would fall back to `lcov` + `gcov` for the same purpose — that path is documented inline in `scripts/dev/coverage.sh`'s header but not yet wired into a workflow (re-evaluate when a POSIX CI runner is provisioned).

## Adding a new harness

1. Create `docs/harness/<name>/setup.md` with the recreation steps.
2. If the harness needs template files, place them under `docs/harness/<name>/`.
3. Add a `setup_<name>()` function to `scripts/setup-harness.sh` + `.ps1`.
4. Add a row to the table above.
5. Add `.<name>/` to `.gitignore`.
