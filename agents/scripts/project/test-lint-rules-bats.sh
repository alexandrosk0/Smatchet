#!/usr/bin/env bash
# test-lint-rules-bats.sh — bats wrapper for tests/bats/lint_rules.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob. Complements
# agents/scripts/project/test-lint-rules.sh (the scanner / delta gate itself) — this gives
# per-case isolation for the rule + deviation + diff-mode logic.
#
# Plan: docs/plans/active/high-integrity-cpp-enforcement.md.
#
# Exit codes (test-author convention):
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools)

set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-lint-rules-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/lint_rules.bats"
[ -f "$BATS_FILE" ] || { echo "missing $BATS_FILE" >&2; exit 1; }

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
rc=$?
echo "$OUT"
p=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
f=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${p}  Failed: ${f}"
[ "$f" -gt 0 ] || [ "$rc" -ne 0 ] && exit 1
exit 0
