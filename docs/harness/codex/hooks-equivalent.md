# Codex Hook Equivalents

Codex does not currently expose a stable project hook surface equivalent to
Claude Code's `.claude/settings.json` events. The project therefore uses repo-owned
git hooks plus explicit commands for the checks that can be enforced outside the
agent runtime.

## Install

Run:

```bash
bash agents/scripts/core/setup-harness.sh codex
```

This sets `core.hooksPath=scripts/git-hooks` when the local config is unset,
still points at the default `.git/hooks`, or already points there. It does not
overwrite a custom hook path.

Manual opt-in:

```bash
git config --local core.hooksPath scripts/git-hooks
```

Manual opt-out:

```bash
git config --local --unset core.hooksPath
```

## Coverage Matrix

| Claude Code surface | Codex repo-owned equivalent | Status |
|---|---|---|
| `SessionStart` scratchpad reset / nudges | Run the referenced scripts manually when needed (`memory-drain-nudge.sh`, `postmortem-owed.sh`, `plan-archival-owed.sh`, etc.) | Manual |
| `PreToolUse` raw-search vexp guard | Use semantic search when available; otherwise follow `AGENTS.md` text-search fallback | Manual |
| `PreToolUse` HEAD/shared-tree guards | Prefer Codex worktrees under `.codex/worktrees/`; run `git status --short --branch` before edits/commits | Manual |
| `PostToolUse` edit lint | `scripts/git-hooks/pre-commit` runs staged-file Pillar 2 scan; run lint scripts manually for broader checks | Partial |
| `PostToolUse` Bash PR autoregistration | Merge-watcher registration remains manual unless an external watcher is running | Manual |
| `SubagentStop` token telemetry | Wire `agents/_shared/token-tracking/agent-token-log.py` only if Codex exposes a future hook | Not automatic |
| `Stop` deferred lint / pre-ship gate | Run `bash scripts/dev/pre-ship.sh` and targeted build/test commands before push | Manual |
| Git push safety | `scripts/git-hooks/pre-push` blocks pushes to merged/closed PR branches when `gh` can resolve the PR | Enforced |

## Pre-Commit: Pillar 2 Static Scanner

The tracked pre-commit hook scans staged first-party C++ files:

```bash
scripts/git-hooks/pre-commit
```

It runs:

```bash
bash scripts/dev/pillar2-scan.sh <staged-cpp-files>
```

No C++ delta means no scanner invocation, so docs-only and plan-only commits do
not fail with a usage error.

For interactive Codex sessions, run the same scanner directly after editing
UI-reachable C++ files:

```bash
bash scripts/dev/pillar2-scan.sh <files-you-just-edited>
```

Exit code `1` is merge-blocking and must be fixed or annotated before commit.

## Broader Local Checks

Codex should run the same repo scripts as any other harness:

```bash
bash agents/scripts/project/test-lint-rules.sh --diff origin/develop
bash scripts/dev/pre-ship.sh
```

For C++ product changes, also run the relevant CMake build/test commands named
by `docs/agent-rules/build.md` and the active plan.

## CI Fallback

PR workflows still run the canonical gates. The local hooks exist to catch cheap
failures before push; they do not replace CI or merge-gate polling.

## Why Not Copy Claude Hooks Into `.codex/`

Claude Code hook templates rely on event names and payload shapes that Codex
does not guarantee. Copying them would create an attractive no-op. The Codex
adapter keeps the stable contract small: native `AGENTS.md` discovery plus
git-enforced repo checks.
