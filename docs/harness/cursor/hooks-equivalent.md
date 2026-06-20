# Cursor — Pillar 2 static scanner invocation

The project's Pillar 2 static gate ships as a canonical bash script at
`scripts/dev/pillar2-scan.sh` (see `docs/plans/shipped/pillar-1-2-perf-review-system.md`
§ Slice 2). The script is fully harness-agnostic — no Cursor-specific glue is
required to invoke it.

## How Cursor should invoke it

Cursor doesn't expose a `PostToolUse`-style hook system for agent-driven edits.
The two viable invocation paths:

### 1. Pre-commit hook (recommended)

Same as Codex (and any other harness): wire the tracked hook path with
`git config --local core.hooksPath scripts/git-hooks` so any commit touching
first-party C++ files runs the scan. Git enforces it independently of which
agent harness produced the change. See `docs/harness/codex/hooks-equivalent.md`
for the shared hook contract.

### 2. Manual agent invocation

For Cursor sessions: ask the agent to run

```bash
bash scripts/dev/pillar2-scan.sh <files-you-just-edited>
```

at end-of-turn. Exit 1 blocks the change; the agent must add the
`/* PILLAR2_WORKER_ONLY */ // est-latency:` annotation or move the call to
a worker before reporting done.

### 3. CI fallback (always-on)

PR-fast workflow at `.github/workflows/perf-pr-fast.yml` (Slice 3) runs the
scanner against changed files. Even without harness-side enforcement, CI
catches violations before merge.

## Why no template ships under `docs/harness/cursor/`

Cursor's rule system (`.cursor/rules/*.mdc`) is for prompt-level guidance,
not for executable shell hooks. The canonical bash script is the same one
Claude Code and Codex call; only the trigger differs.
