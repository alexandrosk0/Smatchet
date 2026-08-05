# Harness setup

The project ships **harness-agnostic** agent definitions:

- [`AGENTS.md`](../../AGENTS.md) at the repo root — project rules, delegation table, capability map.
- [`agents/*.md`](../../agents/) — one file per delegated agent (per the [agents.md spec](https://agents.md/)).
- [`agents/_shared/`](../../agents/_shared/) — shared skills + token-tracking scripts any harness can wire.

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`, `.pi/`) are **gitignored** - they're local build output that links into or generates from the canonical `agents/` tree and copies a small number of templates. Codex still reads `AGENTS.md` natively, but setup now also generates Codex-native hooks and custom-agent TOML under `.codex/`. After cloning the repo, run the setup script for the harness you use:

| Harness | Setup command | Details |
|---|---|---|
| Claude Code | `bash agents/scripts/core/setup-harness.sh claude-code` | [claude-code/setup.md](claude-code/setup.md) |
| Codex / OpenAI Agents | `bash agents/scripts/core/setup-harness.sh codex` | [codex/setup.md](codex/setup.md) |
| Cursor | `bash agents/scripts/core/setup-harness.sh cursor` | [cursor/setup.md](cursor/setup.md) |
| pi (earendil-works) | `bash agents/scripts/core/setup-harness.sh pi` | [pi/README.md](pi/README.md) |
| Aider / generic | Manual — paste agent files from `agents/` as needed. No adapter dir. | — |

Windows users can substitute `pwsh agents/scripts/core/setup-harness.ps1 <name>`.

## Concurrent-session HEAD-drift guard — INACTIVE until first setup

> **Bootstrap hole:** the concurrent-session HEAD-drift guard is **INACTIVE until you run `setup-harness` once** in a clone. Until then, the "protected even if you forget to use a worktree" guarantee does not hold.

The guard ([`guard-head-drift.sh`](claude-code/hooks/guard-head-drift.sh)) is the `PreToolUse` hook that **denies** an Edit/Write/commit when another session, your terminal, or a janitor moves the shared HEAD under you — the fix shipped in [PR #913](https://github.com/alexandrosk0/Smatchet/pull/913). It lives in the **gitignored** `.claude/` adapter, which **only `setup-harness.sh claude-code` provisions**. A fresh clone therefore has no hooks at all, so the guard cannot self-bootstrap (there is no committed hook to fire and re-wire itself).

Close the hole:

| Path | What closes it |
|---|---|
| **Recommended launcher** | `nsc <slug>` / `pwsh scripts/dev/worktree.ps1 new <slug>` provisions the new worktree **and**, on first run, the integration tree's `.claude/` — so using the standard launcher closes the hole automatically, no remembering required. |
| **Working directly in the main clone** | Run `bash agents/scripts/core/setup-harness.sh claude-code` **once** before relying on the guard. |
| **Check anytime** | `bash agents/scripts/core/check-harness-provisioned.sh` warns (exit 1) when the current tree's guard hook is missing and prints the fix; exit 0 when wired. `scripts/dev/doctor.sh` runs the same probe as a warn-only preflight check (`[WARN] harness`), so the standard doctor pass surfaces an unprovisioned tree without a hand-run probe. Note the repo-owned git hooks (`scripts/git-hooks/`, e.g. `pre-push`) are ALSO inert in a fresh clone — `core.hooksPath` is set by `setup-harness.sh`, so no git hook can self-report the hole. |

Guard mechanics + recovery: [`process-rules.md`](../agent-rules/process-rules.md) § Concurrent interactive sessions.

## Required CLI tools

`setup-harness.sh` runs `scripts/dev/check-required-tools.sh` as its first step. The probe fails loudly if any of these isn't on `PATH`:

| Tool | Why | Install (Windows) |
|---|---|---|
| `git` | table stakes | bundled with Git for Windows |
| `cmake` | every build preset | `winget install Kitware.CMake` (or MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-cmake`) |
| `ninja` | preset generator | `winget install Ninja-build.Ninja` (or MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-ninja`) |
| `gcc` / `g++` | lint toolchain (clang-format/cppcheck invoke gcc for syntax checks) | MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-gcc` — build itself uses MSVC or Clang |
| `python` | dev scripts (perf-compare, `lockfile.py` drain serialisation, etc.) | python.org installer (3.11+) or `pacman -S mingw-w64-ucrt-x86_64-python` |
| `jq` | test harness only (`merge_gates.bats` mocks `gh` via jq). The merge-gates poller parses via gh's bundled jq (`gh api --jq`) — no standalone jq at runtime. | `winget install jqlang.jq` (or MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-jq`) |
| `gh` | PR ops + merge-gates poller | `winget install GitHub.cli` then add `C:/Program Files/GitHub CLI` to PATH |
| `clang-format`, `clang-tidy` | lint hooks | MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra` |
| `cppcheck` | lint hooks | MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-cppcheck` |

Optional (warn-only — not required for the standard ship-loop):

| Tool | Why |
|---|---|
| `OpenCppCoverage` | Coverage gates only — see "Optional: coverage tooling" below. |

Ad-hoc invocation: `bash scripts/dev/check-required-tools.sh` (add `--quiet` to suppress PASS lines). Re-run anytime; idempotent.

### Installing the missing ones

`scripts/dev/setup-env.sh` is the write-side companion: it installs whatever the probe reports missing, through the host's native package manager (winget or MSYS2 `pacman` on Windows, `apt` on Debian/Ubuntu, `brew` on macOS, `npm` for `bats` / `shellcheck`), then re-runs `check-required-tools.sh` as its verdict.

```bash
bash scripts/dev/setup-env.sh --dry-run   # print the install plan, change nothing
bash scripts/dev/setup-env.sh             # plan, prompt, install, verify
bash scripts/dev/setup-env.sh --yes       # non-interactive (CI / fresh-clone bootstrap)
```

Also `--list` (full tool → package map for this host) and `--with-optional` (include the warn-only tools). Idempotent — already-present tools are skipped, so re-running a completed setup is a no-op. It prepends the same known toolchain dirs the probe does, so a tool that is installed but off the inherited `PATH` is not reinstalled.

It does **not** install the C++ build toolchain (Visual Studio / MSVC or Clang-cl) and does not configure or build the project — those stay with [`BUILD.md`](../../BUILD.md) § Prerequisites and `bash scripts/dev/doctor.sh`. Packages with no mapping on the current host (e.g. `bats` with no `npm`, `OpenCppCoverage`) are printed as an explicit manual hint rather than silently skipped.

Fresh-clone order: `setup-env.sh` → `doctor.sh` → `setup-harness.sh <harness>`.

## VCS mode (git vs Perforce) — per machine

Smatchet's VCS layer is `git` by default (the GitHub ship-line). The Perforce local layer is opt-in via two env vars (AGENTS.md § Dual-VCS topology): `SMATCHET_AGENT_VCS` (`git` | `p4` — ship-loop variant) and `SMATCHET_LOCK_BACKEND` (`git-ref` | `p4-counter` — plan-lock backend). Both must agree, and on **Windows both layers must agree**: PowerShell inherits the Windows User-registry env while git-bash sources `~/.bashrc`, so a divergence (registry=`git`, `.bashrc`=`p4`) silently routes `lock-claim.sh` to the p4 path and fails "P4USER not set".

`scripts/dev/set-vcs-mode.{sh,ps1}` sets **both** layers idempotently — run it **once per machine** to pin the mode you want everywhere:

```powershell
# Windows (authoritative — sets the User registry + the ~/.bashrc managed block)
pwsh scripts/dev/set-vcs-mode.ps1 git    # or: p4
```
```bash
# git-bash / POSIX (also syncs the Windows registry via setx when on Windows)
bash scripts/dev/set-vcs-mode.sh git     # or: p4;  no arg prints the current mode
```

It rewrites a marked block in `~/.bashrc` (between `# >>> smatchet vcs-mode >>>` … `# <<< smatchet vcs-mode <<<`) and, on Windows, the User-registry env. Open a new shell afterwards for the change to take effect. Re-runs and git↔p4 toggles are idempotent (the block is replaced in place); legacy unmarked `export SMATCHET_*` lines are stripped so they can't shadow it. Tests: `tests/bats/set_vcs_mode.bats`.

## Why links + copies, not a tracked mirror

The setup script uses **directory junctions** (Windows) / **symlinks** (Unix) for harnesses that need linked agent definitions and shared skills, so edits to `agents/*.md` are picked up immediately - no sync step, no banner injection, no drift-check. Codex skips links because its native rule discovery path is already `AGENTS.md` + `agents/{core,project}/*.md`; its setup generates `.codex/agents/*.toml` from those same canonical files for Codex custom-subagent spawning.

Templates that the user might locally tweak (`settings.json`, hook shell scripts, `CLAUDE.md`) are **copies**. The script preserves user-modified copies on re-run.

## Per-subsystem leaf discovery

Heavy `Source/Core/src/<ctx>/` subsystems carry a leaf `AGENTS.md` (scoped rules) — plus, for the `Tracker/` exemplar, `CONTEXT.md` + `README.md`. Registry + per-context coverage: root [`CONTEXT-MAP.md`](../../CONTEXT-MAP.md). How a harness picks up the leaf **rules** when you touch a file in that dir:

| Harness | Nested-`AGENTS.md` auto-load? | Mechanism |
|---|---|---|
| Claude Code | **No** — it lazy-loads nested `CLAUDE.md`, not nested `AGENTS.md` | `setup-harness.sh claude-code` generates a gitignored one-line `CLAUDE.md` (`@AGENTS.md`) beside each leaf. Claude Code includes it when it reads a file in that dir (lazy — only that subsystem's rules cost tokens, only when touched). Committed tree stays `AGENTS.md`-only. |
| Codex / OpenAI Agents | **Yes** — reads the nearest `AGENTS.md` per the [agents.md spec](https://agents.md/) | Native; no shim needed. |
| Cursor | Partial — `.cursor/rules` globs, not path-nearest | Use `CONTEXT-MAP.md` + explicit reads, or author a per-glob `.mdc` (follow-up if locality matters). |
| Aider / generic | No | Read the leaf explicitly (point at it via `CONTEXT-MAP.md`). |

**`CONTEXT.md` / `README.md` are never auto-loaded** on any harness — they're read on demand (agent or semantic search). Only the leaf `AGENTS.md` participates in nearest-wins.

**Regression net**: `agents/scripts/core/test-leaf-doc-discovery.sh` (static, auto-enrolled in `test-all.sh`) asserts a `@AGENTS.md` shim sits beside every leaf on a provisioned checkout (skips when unprovisioned, e.g. CI). Its `--live` mode (opt-in, spends tokens — never auto-run) spawns a headless `claude -p` session cd'd into `Source/Core/src/Tracker/` and asserts it cites the leaf `AGENTS.md` as the rule source for a Tracker invariant — end-to-end proof the shim mechanism reaches an agent. Codex (native nearest-`AGENTS.md`) and Cursor (`CONTEXT-MAP.md` pointer) have no headless probe yet — manual residue.

Eager-load caveat: if a harness ever eager-loads *all* nested memory at session start (rather than lazily per touched dir), the per-subsystem token win inverts — every subsystem's rules load every session. Claude Code's nested `CLAUDE.md` is **lazy** (confirmed), so the shim is safe. A future eager-loading harness should stay pointer-only (`CONTEXT-MAP.md`) until it supports lazy nearest-wins.

## Optional: coverage tooling (`OpenCppCoverage`)

`OpenCppCoverage` is **Windows-only** and **not required** for normal development of the project — `scripts/dev/coverage.sh` exits 2 with a clean install hint when the binary is absent, and CI runners install it via Chocolatey (`choco install opencppcoverage`). Install locally only if you're working on coverage gates / threshold tuning or want to inspect line-coverage in `coverage/coverage-html/index.html`.

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

**Upstream fix**: this is the Claude Code SDK's responsibility — the worktree-spawn machinery should default the base to `origin/develop` (or a configurable `claude.worktree.baseBranch`) rather than current local HEAD. Filed as external-blocker in `docs/self-improvement/categories/external-blockers.md`.

**Not the same path as the agentic-handoff runner**: `ClaudeCodeLocalRunner` (the in-repo H3 deliverable for `agent/<proposalId>` worktrees) already bases on `origin/develop` correctly per `Source/Core/src/ClaudeCodeLocalRunner.cpp` + the `handoff.auto_fetch_before_worktree` config flag (default `true`). Only the Claude Code SDK's own session-spawn path (`claude/<id>` worktrees) is affected.

## Hook-authoring checklist — promoting a gate-script into a per-call hook

A `--strict` gate-script built for explicit pre-launch invocation flags EVERY instance; wired raw into an always-on `PreToolUse`/`PostToolUse` hook it blocks trivial calls. When promoting one (proven by the fleet-preflight `Workflow` hook, #1429):

1. **Threshold-gate the BLOCK, not the raw exit code** — parse the scope out of the payload and block only above the documented threshold (e.g. ">2 agents"); the gate's own non-zero exit is necessary but not sufficient as a per-call block signal.
2. **Fail-OPEN on plumbing** — missing dep (`jq`, python), unparseable payload, or wrong tool → exit 0 (allow). A guard that blocks every call when a dep is absent is worse than the gap it closes. Block (exit 2) only when the gate itself reports a real violation.
3. **Test-assert on the hook's OWN output prefix** (e.g. `[my-hook] blocking launch`), never a generic phrase — a hook that echoes the wrapped tool's output makes `[[ "$output" != *"blocking launch"* ]]` false-fail when the wrapped gate emits the same words.
4. **Keep project-prefixed env-name literals out of portable rule-docs** — reference the knobs generically there (`<PROJECT>_*`); the literal names live in the non-portable hook source's header (guard: `test-portable-purity`).

Reference implementations: [`claude-code/hooks/lint-portable-purity.sh`](claude-code/hooks/lint-portable-purity.sh) (fail-open shape), the fleet-preflight `PreToolUse` hook + `tests/bats/pretool_workflow_fleet_preflight.bats` (threshold + own-prefix assertions).

## Adding a new harness

1. Create `docs/harness/<name>/setup.md` with the recreation steps.
2. If the harness needs template files, place them under `docs/harness/<name>/`.
3. Add a `setup_<name>()` function to `agents/scripts/core/setup-harness.sh` + `.ps1`.
4. Add a row to the table above.
5. Add `.<name>/` to `.gitignore`.
