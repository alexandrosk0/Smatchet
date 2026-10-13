# Plan-lock edit hook resolves every worktree session against the main tree's branch

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-10-04
- **Observed on**: the shutdown-cancel-all-pane-syncs branch, writing `.review-findings.json` in
  the worktree `.claude/worktrees/pensive-stonebraker-d3db80`
- **Status**: open

## What happened

`.claude/hooks/guard-plan-lock.sh` (the PreToolUse `Write`/`Edit` guard) takes its project root
from `CLAUDE_PROJECT_DIR`. For a session running in a worktree under `.claude/worktrees/<id>/`, that
variable is the main tree (`C:/Dev/Smatchet`, on `develop`). The hook therefore:

- reads `mybr` as `develop` instead of the worktree's branch, and
- computes the path as `.claude/worktrees/<id>/<file>` instead of `<file>`.

It allows a write only when a lock whose `branch` equals `mybr` covers `rel`, so a worktree session
can never satisfy it. Even after `lock-claim.sh` claimed `refs/locks/shutdown-cancel-all-pane-syncs`
for the worktree's branch with both path forms in the write set, the `Write` was refused, naming
branch `develop`. The read-only code-review agent hit the same refusal on the same file, so review
step 7 (`agents/core/code-review.md`) could not complete. The artifact was finally written with a
Bash heredoc, which this hook does not guard.

A related quirk: `lock-release.sh` deletes the ref with a push, and the pre-push plan-lock check
judges the current branch's diff against the very lock being released, so releasing a stale lock
that overlaps your diff needs `SMATCHET_ALLOW_UNLOCKED_PUSH=1`.

## Why it matters

The worktree-per-session rule (AGENTS.md § Concurrent sessions) is the sanctioned way to work, and
in it this guard cannot be satisfied by doing the right thing (claiming a lock). It can only be
routed around, which teaches agents to bypass it and leaves the guard protecting nothing. It also
blocks gitignored review artifacts (`.review-findings.json`, `.review-ack`) that no other session
can collide on.

## Concrete next action

In `guard-plan-lock.sh`, resolve the root from the edited file's own worktree
(`git -C "$(dirname "$FP")" rev-parse --show-toplevel`), not `CLAUDE_PROJECT_DIR`, and take `mybr`
from that same worktree. Exempt gitignored paths (`git check-ignore -q`). Add a bats case: a
worktree on branch B holding a lock for path P may write P. In `lock-release.sh`, skip the pre-push
collision stage for a pure `refs/locks/*` delete.
