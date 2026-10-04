#!/usr/bin/env bash
# test-coverage-gate-bats.sh — bats wrapper for tests/bats/coverage_gate.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Wraps the bats suites that run the hermetic --selftest of scripts/dev/coverage.sh
# (test-binary-vs-tooling exit split), scripts/dev/coverage-delta-gate.sh (the
# test-light exemption classifier incl. the multi-line wrapped LOG_* join), and
# scripts/dev/coverage-perfile-gate.sh (the per-file >=90% high-risk floor incl.
# its below-floor FAIL + escape/at-floor/fail-open PASS paths) + emits the
# canonical `Passed: N  Failed: M` line that test-all.sh greps for.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-coverage-gate-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

# Both coverage-gate bats suites (named by PATH so test-orphan-bats.sh counts each
# as wrapped). coverage_perfile_gate.bats wraps the per-file >=90% high-risk gate.
BATS_FILES=("tests/bats/coverage_gate.bats" "tests/bats/coverage_perfile_gate.bats")
for f in "${BATS_FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "test-coverage-gate-bats: $f not found" >&2
        echo "Passed: 0  Failed: 1"
        exit 1
    fi
done

# TAP mode so we can parse `ok N` / `not ok N` without depending on reporter shape.
OUT="$(bats --tap "${BATS_FILES[@]}" 2>&1)"
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
