# Codex Hook Equivalents

Codex supports project-local hooks through `.codex/hooks.json` or inline
`[hooks]` tables in `.codex/config.toml` after the project `.codex/` layer is
trusted. The project uses `hooks.json` so the hook wiring is easy to diff, review,
and regenerate.

## Install

Run:

```bash
bash agents/scripts/core/setup-harness.sh codex
```

This creates:

- `.codex/config.toml` - enables lifecycle hooks for this trusted project layer.
- `.codex/hooks.json` - safe SessionStart/Stop command hooks.
- `.codex/agents/*.toml` - generated Codex custom agents from canonical markdown prompts.

It also sets `core.hooksPath=scripts/git-hooks` when the local config is unset,
still points at the default `.git/hooks`, or already points there. It does not
overwrite a custom hook path.

Manual git-hook opt-in:

```bash
git config --local core.hooksPath scripts/git-hooks
```

Manual opt-out:

```bash
git config --local --unset core.hooksPath
```

## Coverage Matrix

| Claude Code surface | Codex equivalent | Status |
|---|---|---|
| `SessionStart` scratchpad reset / nudges | `.codex/hooks.json` runs memory drain, postmortem owed, due follow-up, and plan archival nudges | Codex-native after trust |
| `PreToolUse` raw-search vexp guard | Use semantic search when available; otherwise follow `AGENTS.md` text-search fallback | Manual |
| `PreToolUse` HEAD/shared-tree guards | Prefer Codex worktrees under `.codex/worktrees/`; run `git status --short --branch` before edits/commits | Manual |
| `PostToolUse` edit lint | `scripts/git-hooks/pre-commit` runs staged-file Pillar 2 scan; run lint scripts manually for broader checks | Partial |
| `PostToolUse` Bash PR autoregistration | Merge-watcher registration remains manual unless an external watcher is running | Manual |
| `SubagentStop` token telemetry | Wire `agents/_shared/token-tracking/agent-token-log.py` only after Codex payload compatibility is verified | Not automatic |
| `Stop` deferred lint / pre-ship gate | `.codex/hooks.json` runs the committed-diff pre-ship gate when the branch is clean and ahead of `origin/develop` | Codex-native after trust |
| `Stop` wrong-worktree warning | `.codex/hooks.json` runs `check-main-repo-clean.sh` | Codex-native after trust |
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

## Why Not Copy Every Claude Hook Into Codex

Claude Code hook templates use Claude event names, environment variables, and
payload shapes. Codex now has hook events, but payload-dependent blockers still
need Codex-specific validation before they can safely deny a tool call or mutate
session state. The Codex adapter wires stable repo-root command hooks now and
keeps the risky payload-dependent hooks documented as gaps.
