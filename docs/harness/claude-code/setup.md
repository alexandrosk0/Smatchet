# Claude Code adapter

Generated locally at `.claude/` by:

```bash
bash agents/scripts/core/setup-harness.sh claude-code
# or, on Windows-native PowerShell:
pwsh agents/scripts/core/setup-harness.ps1 claude-code
```

Idempotent. Run it after every clone and any time the canonical templates change. The script never overwrites a user-modified template — it prints `skip-copy` and leaves your local edits alone.

## What the script generates

| Path under `.claude/` | Kind | Source |
|---|---|---|
| `CLAUDE.md` | copy | `docs/harness/claude-code/CLAUDE.md.tmpl` |
| `settings.json` | copy | `docs/harness/claude-code/settings.json.tmpl` |
| `hooks/lint-cpp.sh` | copy | `docs/harness/claude-code/hooks/lint-cpp.sh` |
| `hooks/lint-syntax-both.py` | copy | `docs/harness/claude-code/hooks/lint-syntax-both.py` |
| `hooks/autoregister-pr.sh` | copy | `docs/harness/claude-code/hooks/autoregister-pr.sh` |
| `agents/` | dir link | `agents/` (junction on Windows, symlink on Unix) |
| `skills/grill-with-docs/` | dir link | `agents/_shared/skills/grill-with-docs/` |
| `skills/scratchpad-recall/` | dir link | `agents/_shared/skills/scratchpad-recall/` |
| `skills/agent-tokens/SKILL.md` | file link | `agents/_shared/token-tracking/SKILL.md` |
| `hooks/agent-token-log.py` | file link | `agents/_shared/token-tracking/agent-token-log.py` |
| `hooks/agents-statusline.py` | file link | `agents/_shared/token-tracking/agents-statusline.py` |

## Why links (not copies) for agents + shared skills

`agents/*.md` is the **single source of truth**. The link makes Claude Code's `.claude/agents/` discovery path read the canonical files directly — no sync step, no mirror banner, no drift-check.

Edits to `agents/core/architect.md` are visible to Claude Code immediately. Adding a new file under `agents/` exposes it without re-running the setup script.

> **Windows hardlink-fallback caveat.** "Visible immediately" holds only when the adapter is a directory junction or symlinks (Unix, or Windows with Dev Mode on). When the script falls back to **per-file hardlinks** (Windows, Dev Mode off — see [Windows specifics](#windows-specifics)), the `.claude/` copy shares the canonical file's *inode*, not its path. Editing the canonical via a tool that **rewrites the file** (`Edit`/`Write` replace the inode rather than patching in place) silently breaks the hardlink: `.claude/agents/<name>.md` keeps the OLD content, so the harness spawns the agent with the stale model/effort/prompt — invisibly, until the next setup run. After editing any `agents/{core,project}/*.md` on such a machine, **re-run `bash agents/scripts/core/setup-harness.sh claude-code`** before relying on that agent in a spawn. (`test-agent-contract` validates the canonical file only and passes regardless — it does not catch adapter staleness; tracked as the `agent-adapter-drift` gate-gap in `docs/self-improvement/categories/tooling.md`.)

## Why copies for templates

`settings.json` carries hook wiring. Some devs add project-local permissions or extra hooks. If `settings.json` were a link, those edits would silently leak back into the tracked template via `git add`. Copies isolate per-machine tweaks.

The lint hooks (`lint-cpp.sh`, `lint-syntax-both.py`) are copies for the same reason — devs sometimes patch them for local clang-tidy versions or to disable a check.

## Windows specifics

- Directory junctions (`mklink /J`) — no admin or Dev Mode required.
- File hardlinks (`mklink /H`) — no admin required, same-volume only (always true within the repo).
- File symlinks (`mklink`) — preferred if Dev Mode is on; the script tries them first and falls back to hardlinks.

## Windows text-plumbing — Bash tool vs PowerShell tool

On Windows the agent drives two shells with opposite strengths; using the wrong one corrupts commit/PR bodies and multi-line file writes. The split:

- **Commit / PR bodies → POSIX heredoc piped to `-F -` / `--body-file -` (Bash tool).** `git -C <path> commit -F - <<'EOF'` … `EOF`; `gh pr create --body-file - <<'EOF'` … `EOF` (or `gh pr edit N --body-file -`). **Never pipe a PowerShell here-string `@'...'@` to git/gh** — in bash the `@` is literal, so `@'...'@` prepends/appends a stray `@` line that corrupts the commit subject + PR body (hit twice in PR #1197 — the subject became a bare `@`). Reserve `@'...'@` for the PowerShell tool only.
- **Multi-line FILE writes → the `Write` tool first; when you must write via a shell, use the PowerShell tool's single-quoted here-string + `[IO.File]::WriteAllText($path,$c,(New-Object System.Text.UTF8Encoding $false))` (UTF-8 no-BOM).** A large `cat > file <<'EOF'` through the **Bash tool** is unreliable on Windows — the body parses as shell (`unexpected EOF looking for matching '`), especially with apostrophes or non-ASCII em-dashes. `Set-Content -Encoding utf8` in PS 5.1 adds a BOM and mojibakes existing UTF-8 on a Get/Set round-trip, so use `WriteAllText` with the no-BOM encoder, not `Set-Content`.
- **Reserve the Bash tool for the repo's own `.sh` tooling** (`test-lint-rules.sh`, `with-msvc-env.sh`, `merge-gates.sh`, bats); PowerShell is the primary interactive shell. The one MSYS gotcha is **path-mangling** — set `MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'` ONLY for a colon-path `git show "ref:path"`, never for `git/gh -C`.

## Hooks

`.claude/settings.json` wires these hook surfaces:

- **SessionStart** — `agents/scripts/core/clear-session-context.sh` archives the prior session scratchpad.
- **PostToolUse** (Edit/Write) — `.claude/hooks/lint-cpp.sh` runs `clang-format`, `cppcheck`, `clang-tidy` on edited C++ files.
- **PostToolUse** (Bash) — `.claude/hooks/autoregister-pr.sh` registers a newly-created PR (`gh pr create`) with smatchet-merge-watcher so the daemon owns it (gate-poll + auto-merge).
- **SubagentStop** — `.claude/hooks/agent-token-log.py` appends per-agent token usage to `.claude/.agent-tokens.jsonl`.

`.claude/.agent-tokens.jsonl` is per-machine state — gitignored, never committed.

## Remote container (cloud sandbox)

In the Claude Code remote container, provision the `posix-core-check` build lane with `bash scripts/dev/remote-container-bootstrap.sh` — the container's egress policy 403s GitHub release tarballs (cpr's curl FetchContent) and lacks `xorg-dev`/`libgl1-mesa-dev`; the script wraps the validated workaround. Details: [`docs/agent-rules/build.md`](../../agent-rules/build.md) § Remote-container posix-core-check bootstrap.

## Refreshing after a `git pull`

Re-run `bash agents/scripts/core/setup-harness.sh claude-code`:

- New shared skill or token-tracking change → the link target already points at the new content; no action needed beyond the link existing.
- Updated template (`CLAUDE.md.tmpl`, `settings.json.tmpl`, lint hook) → script copies it if you haven't locally modified the file, otherwise prints `skip-copy`.

## Adding a project-local agent

Drop a `.md` file under `agents/`. Claude Code picks it up via the junction immediately. No mirror sync. Follow the existing `version:` frontmatter convention if you want telemetry to track it.

## Removing the adapter

```bash
rm -rf .claude
```

Safe — fully regenerable via the setup script. Nothing tracked.
