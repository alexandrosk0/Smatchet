# Claude Code adapter

Generated locally at `.claude/` by:

```bash
bash scripts/setup-harness.sh claude-code
# or, on Windows-native PowerShell:
pwsh scripts/setup-harness.ps1 claude-code
```

Idempotent. Run it after every clone and any time the canonical templates change. The script never overwrites a user-modified template — it prints `skip-copy` and leaves your local edits alone.

## What the script generates

| Path under `.claude/` | Kind | Source |
|---|---|---|
| `CLAUDE.md` | copy | `docs/harness/claude-code/CLAUDE.md.tmpl` |
| `settings.json` | copy | `docs/harness/claude-code/settings.json.tmpl` |
| `hooks/lint-cpp.sh` | copy | `docs/harness/claude-code/hooks/lint-cpp.sh` |
| `hooks/vexp-guard.sh` | copy | `docs/harness/claude-code/hooks/vexp-guard.sh` |
| `hooks/lint-syntax-both.py` | copy | `docs/harness/claude-code/hooks/lint-syntax-both.py` |
| `agents/` | dir link | `agents/` (junction on Windows, symlink on Unix) |
| `skills/grill-with-docs/` | dir link | `agents/_shared/skills/grill-with-docs/` |
| `skills/scratchpad-recall/` | dir link | `agents/_shared/skills/scratchpad-recall/` |
| `skills/agent-tokens/SKILL.md` | file link | `agents/_shared/token-tracking/SKILL.md` |
| `hooks/agent-token-log.py` | file link | `agents/_shared/token-tracking/agent-token-log.py` |
| `hooks/agents-statusline.py` | file link | `agents/_shared/token-tracking/agents-statusline.py` |

## Why links (not copies) for agents + shared skills

`agents/*.md` is the **single source of truth**. The link makes Claude Code's `.claude/agents/` discovery path read the canonical files directly — no sync step, no mirror banner, no drift-check.

Edits to `agents/architect.md` are visible to Claude Code immediately. Adding a new file under `agents/` exposes it without re-running the setup script.

## Why copies for templates

`settings.json` carries hook wiring. Some devs add project-local permissions or extra hooks. If `settings.json` were a link, those edits would silently leak back into the tracked template via `git add`. Copies isolate per-machine tweaks.

The lint hooks (`lint-cpp.sh`, `vexp-guard.sh`, `lint-syntax-both.py`) are copies for the same reason — devs sometimes patch them for local clang-tidy versions or to disable a check.

## Windows specifics

- Directory junctions (`mklink /J`) — no admin or Dev Mode required.
- File hardlinks (`mklink /H`) — no admin required, same-volume only (always true within the repo).
- File symlinks (`mklink`) — preferred if Dev Mode is on; the script tries them first and falls back to hardlinks.

## Hooks

`.claude/settings.json` wires four hook surfaces:

- **SessionStart** — `scripts/clear-session-context.sh` archives the prior session scratchpad.
- **PreToolUse** (Grep/Glob/Regex) — `.claude/hooks/vexp-guard.sh` blocks raw text-search when the vexp daemon is running.
- **PostToolUse** (Edit/Write) — `.claude/hooks/lint-cpp.sh` runs `clang-format`, `cppcheck`, `clang-tidy` on edited C++ files.
- **SubagentStop** — `.claude/hooks/agent-token-log.py` appends per-agent token usage to `.claude/.agent-tokens.jsonl`.

`.claude/.agent-tokens.jsonl` is per-machine state — gitignored, never committed.

## Refreshing after a `git pull`

Re-run `bash scripts/setup-harness.sh claude-code`:

- New shared skill or token-tracking change → the link target already points at the new content; no action needed beyond the link existing.
- Updated template (`CLAUDE.md.tmpl`, `settings.json.tmpl`, lint hook) → script copies it if you haven't locally modified the file, otherwise prints `skip-copy`.

## Adding a project-local agent

Drop a `.md` file under `agents/`. Claude Code picks it up via the junction immediately. No mirror sync. Follow the existing `version:` frontmatter convention if you want telemetry to track it.

## Removing the adapter

```bash
rm -rf .claude
```

Safe — fully regenerable via the setup script. Nothing tracked.
