#!/usr/bin/env bash
# test-lock-primitives-bats.sh — bats wrapper for tests/bats/lock_*.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Per docs/evaluation/agentic-infrastructure-2026-05-23.md punch-list item 6
# (bats coverage for lock primitives). Complementary to the existing
# scripts/dev/test-lock-primitives.sh integration test — bats gives
# per-case isolation so an argument-validation regression doesn't poison
# the whole end-to-end run.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-lock-primitives-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

# Force git-ref lock backend regardless of caller env — the operator may
# run the test suite with SMATCHET_LOCK_BACKEND=p4-counter set, which
# dispatches into the p4 sibling script the bats file isn't testing.
unset SMATCHET_LOCK_BACKEND SMATCHET_AGENT_VCS

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_RC=0
for bats_file in tests/bats/lock_*.bats; do
    [ -f "$bats_file" ] || continue
    echo "=== $bats_file ==="
    OUT="$(bats --tap "$bats_file" 2>&1)"
    rc=$?
    echo "$OUT"
    p=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
    f=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
    TOTAL_PASS=$((TOTAL_PASS + p))
    TOTAL_FAIL=$((TOTAL_FAIL + f))
    if [ "$rc" -ne 0 ]; then
        TOTAL_RC=1
    fi
    echo
done

echo "Passed: ${TOTAL_PASS}  Failed: ${TOTAL_FAIL}"

if [ "$TOTAL_FAIL" -gt 0 ] || [ "$TOTAL_RC" -ne 0 ]; then
    exit 1
fi
exit 0
