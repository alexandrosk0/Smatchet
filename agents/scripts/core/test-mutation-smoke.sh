#!/usr/bin/env bash
# test-mutation-smoke.sh — bats wrapper for tests/bats/mutation_smoke.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Wraps the bats regression suite for scripts/dev/mutation-smoke.sh (the
# mutation-smoke classify/floor/honesty logic) + emits the canonical
# `Passed: N  Failed: M` line that test-all.sh greps for.
#
# NOTE: mutation_smoke.bats shipped in #1698 (Slice F) WITHOUT this wrapper, so
# the test-orphan-bats gate went red on develop and blocked every open PR under
# block-on-any-red until this wrapper landed. Adding a .bats without its wrapper
# is exactly the gap test-orphan-bats.sh exists to catch.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-mutation-smoke: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/mutation_smoke.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-mutation-smoke: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Run bats in TAP mode so we can parse `ok N` / `not ok N` lines without
# depending on `--count` or version-specific reporter shapes.
OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?

echo "$OUT"

PASSED=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)

echo "Passed: ${PASSED}  Failed: ${FAILED}"

# Zero-run floor (fail-open shape Z): a bats suite that parses to ZERO tests
# (vanished file / TAP parse error) leaves PASSED=FAILED=0 and would exit green.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the bats suite ran ZERO tests (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then
    exit 1
fi
exit 0
