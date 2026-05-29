# Codex — Pillar 2 static scanner invocation

Smatchet's Pillar 2 static gate ships as a canonical bash script at
`scripts/dev/pillar2-scan.sh` (see `docs/plans/shipped/pillar-1-2-perf-review-system.md`
§ Slice 2). The script is fully harness-agnostic — no `.codex/` or
Claude-Code-specific glue is required.

## How Codex should invoke it

Codex doesn't have a `PostToolUse:Edit` hook equivalent to Claude Code's
end-of-turn drain pipeline. The two viable invocation paths are:

### 1. Pre-commit hook (recommended)

Wire the scanner into `.git/hooks/pre-commit` so any commit touching first-party
C++ files runs the scan. Use a multi-line installer (the previous single-line
form expanded to zero args on commits with no C++ delta, which made
`pillar2-scan.sh` exit 2 and block every non-C++ commit — CodeRabbit PR #323 #5):

```bash
cat > .git/hooks/pre-commit <<'HOOK'
#!/usr/bin/env bash
set -euo pipefail
mapfile -t p2_files < <(git diff --cached --name-only --diff-filter=ACM \
    | grep -E '\.(cpp|h|hpp)$' || true)
[[ ${#p2_files[@]} -eq 0 ]] && exit 0
bash scripts/dev/pillar2-scan.sh "${p2_files[@]}"
HOOK
chmod +x .git/hooks/pre-commit
```

The scanner's first-party + UI-reachable filters mean non-UI changes pass
through with zero cost; nothing to gate when the diff doesn't touch a
UI-reachable file.

### 2. Manual invocation in agent prompts

For interactive Codex sessions: add a one-line check at end-of-turn:

```bash
bash scripts/dev/pillar2-scan.sh <files-you-just-edited>
# Exit 1 = blocks merge; agent must annotate or fix before responding.
```

### 3. CI fallback (always-on)

PR-fast workflow at `.github/workflows/perf-pr-fast.yml` (Slice 3) runs the
scanner against changed files. Even without per-harness hook integration,
the CI gate catches anything that slipped through.

## Why this is documented vs auto-installed

Codex's hook system (codex.toml / task.json mechanisms) isn't stable enough
across Codex versions to ship a checked-in template. Smatchet's
`scripts/setup-harness.sh codex` deliberately does NOT copy hook templates —
it only emits this guidance file. Pre-commit hook is the recommended path
because git enforces it regardless of which agent harness ran the change.
