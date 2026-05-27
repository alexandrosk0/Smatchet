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

## Required CLI tools

`setup-harness.sh` runs `scripts/dev/check-required-tools.sh` as its first step. The probe fails loudly if any of these isn't on `PATH`:

| Tool | Why | Install (Windows) |
|---|---|---|
| `git` | table stakes | bundled with Git for Windows |
| `cmake` | every build preset | `winget install Kitware.CMake` (or MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-cmake`) |
| `ninja` | preset generator | `winget install Ninja-build.Ninja` (or MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-ninja`) |
| `gcc` / `g++` | lint toolchain (clang-format/cppcheck invoke gcc for syntax checks) | MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-gcc` — build itself uses MSVC or Clang |
| `python` | dev scripts (perf-compare, etc.) | python.org installer (3.11+) or `pacman -S mingw-w64-ucrt-x86_64-python` |
| `jq` | merge-gates poller + ad-hoc GraphQL parsing | `winget install jqlang.jq` (or MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-jq`) |
| `gh` | PR ops + merge-gates poller | `winget install GitHub.cli` then add `C:/Program Files/GitHub CLI` to PATH |
| `clang-format`, `clang-tidy` | lint hooks | MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra` |
| `cppcheck` | lint hooks | MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-cppcheck` |
| `flock` | `lint-cpp-drain.sh` queue serialisation | usually built-in (Linux/macOS); MSYS2 needs `pacman -S util-linux` |

Optional (warn-only — not required for the standard ship-loop):

| Tool | Why |
|---|---|
| `OpenCppCoverage` | Coverage gates only — see "Optional: coverage tooling" below. |

Ad-hoc invocation: `bash scripts/dev/check-required-tools.sh` (add `--quiet` to suppress PASS lines). Re-run anytime; idempotent.

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

## Worktree base — known stale-HEAD pitfall

**Symptom**: orchestrator spawns a Claude Code session in an isolated worktree (`.claude/worktrees/<id>/` on branch `claude/<id>`); the branch is rooted on a stale commit (not `origin/develop` HEAD), and every PR opened from the worktree carries an old base. Documented friction from Phase D + Phase E AI-assistant work — the `claude/<id>` branches landed on a months-old "feat: add Google domain verification file" commit (`f2ce5b5`) instead of develop tip.

**Root cause**: the Claude Code SDK uses the parent repo's current local `HEAD` as the base when spawning a worktree. If the parent repo is checked out on an unrelated branch (`fix/<other>`, `feat/<other-feature>`, or a stale checkout of `develop`), that HEAD becomes the new worktree's base.

**Two-track workaround**:

1. **Before opening a new Claude Code session**: `git -C C:/Dev/Smatchet switch develop && git -C C:/Dev/Smatchet pull --ff-only origin develop`. The parent repo is now on the latest develop, and any worktree spawned for this session inherits that base.
2. **If a session is already running and the base is stale**: the orchestrator runs as the **first move** in the worktree —
   ```bash
   git fetch origin develop
   git rebase origin/develop
   ```
   Restages any uncommitted work; the worktree's branch now sits on top of latest develop. Cheap because most `claude/<id>` worktrees have zero or one commit at the time of the rebase.

**Upstream fix**: this is the Claude Code SDK's responsibility — the worktree-spawn machinery should default the base to `origin/develop` (or a configurable `claude.worktree.baseBranch`) rather than current local HEAD. Filed as external-blocker in `docs/backlog/agent-self-improvement/external-blockers.md`.

**Not the same path as the agentic-handoff runner**: `ClaudeCodeLocalRunner` (the in-repo H3 deliverable for `agent/<proposalId>` worktrees) already bases on `origin/develop` correctly per `Source_Core/src/ClaudeCodeLocalRunner.cpp` + the `handoff.auto_fetch_before_worktree` config flag (default `true`). Only the Claude Code SDK's own session-spawn path (`claude/<id>` worktrees) is affected.

## Adding a new harness

1. Create `docs/harness/<name>/setup.md` with the recreation steps.
2. If the harness needs template files, place them under `docs/harness/<name>/`.
3. Add a `setup_<name>()` function to `scripts/setup-harness.sh` + `.ps1`.
4. Add a row to the table above.
5. Add `.<name>/` to `.gitignore`.
