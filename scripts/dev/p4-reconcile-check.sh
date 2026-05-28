#!/usr/bin/env bash
# p4-reconcile-check.sh — pre-push helper for the dual-VCS contract.
#
# Plan: docs/design/git-to-perforce-migration.md § Files to modify §
#       scripts/git-hooks/pre-push:1 — "extend with `p4 reconcile -n` check
#       when client is configured."
#
# Contract: at the canonical tree, `git status --porcelain` and
# `p4 reconcile -n //smatchet/main/...` must agree about which files are
# modified. Drift (a file p4 sees as changed that doesn't appear in git's
# working-tree status) means the p4 side has uncommitted edits that the
# git push is about to leave stranded.
#
# This script:
#   1. Skips silently when SMATCHET_AGENT_VCS != p4 (default git users
#      pay zero cost).
#   2. Skips silently when P4PORT is unset or `p4` is not on PATH.
#   3. Skips silently when `p4 info` is unreachable (server down).
#   4. Otherwise: diffs `p4 reconcile -n` against `git status --porcelain`
#      and warns about any path p4 sees that git doesn't.
#
# Default behaviour: warn (stderr), exit 0.
# Override: SMATCHET_P4_PRE_PUSH_BLOCK=1 → block (exit 2) on drift.
# Override: SMATCHET_AGENT_VCS=p4 + drift detected → block by default.
#
# Mirror of the warn/block pattern in
# docs/harness/claude-code/hooks/pretool-edit-p4-lock-check.sh.

set -euo pipefail

# 1. Bail when the session has not opted into the p4 layer.
if [ "${SMATCHET_AGENT_VCS:-git}" != "p4" ] && [ "${SMATCHET_P4_PRE_PUSH_FORCE:-0}" != "1" ]; then
    exit 0
fi

# 2. Bail when p4 client environment is incomplete.
if [ -z "${P4PORT:-}" ] || ! command -v p4 >/dev/null 2>&1; then
    exit 0
fi

# 3. Bail when server unreachable.
if ! p4 info >/dev/null 2>&1; then
    exit 0
fi

# 4. Collect p4-side modified set.
#    `p4 reconcile -n` lines look like:
#      //smatchet/main/path/to/file.cpp#3 - reconcile modify ...
#    Or:
#      //smatchet/main/new/file.txt - reconcile add ...
#    We want the local path, repo-relative.
p4_view=$(p4 client -o 2>/dev/null | awk '/^View:/ {flag=1; next} flag && /^[ \t]+\//')
client_root=$(p4 client -o 2>/dev/null | awk '/^Root:/ {print $2; exit}')

# Capture only `reconcile modify` / `reconcile add` / `reconcile delete` lines.
# Skip "no file(s) to reconcile" silence + every other p4 emission.
p4_reported=$(p4 reconcile -n //... 2>/dev/null \
    | awk '/reconcile (modify|add|delete)/ {print $1}' \
    | sed 's/#[0-9][0-9]*$//' \
    || true)

if [ -z "$p4_reported" ]; then
    exit 0
fi

# 5. Convert each `//smatchet/main/...` depot path to a repo-relative path.
#    Heuristic: strip the depot prefix matching `//smatchet/main/` (Phase 1
#    canonical stream). Falls back to the depot path on miss; the diff below
#    will surface the miss as drift, which is correct.
p4_local=$(printf '%s\n' "$p4_reported" | sed 's|^//smatchet/main/||')

# 6. Git's side.
git_changed=$(git status --porcelain | awk '{print $NF}')

# 7. Diff: lines in p4 set that aren't in git set.
drift=$(comm -23 <(printf '%s\n' "$p4_local" | sort -u) <(printf '%s\n' "$git_changed" | sort -u))

if [ -z "$drift" ]; then
    exit 0
fi

cat >&2 <<EOF

p4-reconcile-check: DRIFT detected.

The following files are open / modified on the p4 side (via
\`p4 reconcile -n\`) but NOT reflected in git's working-tree status:

EOF
# Intentional word-splitting: $drift is a newline-separated list, each entry
# becomes one printf arg → one '  <path>\n' line.
# shellcheck disable=SC2086
printf '  %s\n' $drift >&2
cat >&2 <<EOF

Pushing now would land a git-side state that ignores those p4 edits.
Either:
  (a) submit them via p4 — \`p4 reconcile && p4 submit\` — and re-push, OR
  (b) revert the p4-side opens — \`p4 revert -a\` — if they were noise.

Override (rare, usually wrong): SMATCHET_P4_PRE_PUSH_BLOCK=0 to warn-only.
EOF

# Block mode: opt-in via env; defaults on under SMATCHET_AGENT_VCS=p4.
if [ "${SMATCHET_P4_PRE_PUSH_BLOCK:-1}" = "1" ]; then
    exit 2
fi

echo "p4-reconcile-check: WARN-ONLY mode (SMATCHET_P4_PRE_PUSH_BLOCK=0)." >&2
exit 0
