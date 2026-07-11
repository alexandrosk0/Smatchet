#!/usr/bin/env bash
# test-ai-prefs-validator.sh — exercise ai.validate-prefs against the
# persisted ConfigManager state. Cheap (no network). Auto-enrolled by
# test-all.sh via the test-*.sh glob.
#
# Validates the CLI command surface end-to-end:
#   - The command is registered.
#   - It returns a coherent JSON envelope ({ok, errors, warnings, ...}).
#
# Exit codes:
#   0 — command ran and emitted a valid envelope.
#   1 — envelope shape invalid or command missing.
#   2 — exe not found.

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-./build/ninja-iter-msvc/Smatchet.exe}"
if [ ! -x "$EXE" ]; then
    # Fallback: look at common alternative locations a developer may have.
    for alt in \
        ./build/ninja-publish-msvc/Smatchet.exe \
        ./build/ninja-debug-msvc/Smatchet.exe \
        ./build/ninja-test-msvc/Smatchet.exe ; do
        if [ -x "$alt" ]; then
            EXE="$alt"
            break
        fi
    done
fi
if [ ! -x "$EXE" ]; then
    echo "test-ai-prefs-validator: Smatchet.exe not built at any common preset"
    exit 2
fi

PASSED=0
FAILED=0

# Run ai.validate-prefs via --spawn so the test doesn't depend on the user
# having Smatchet already running. The command returns OK envelope even when
# validation itself reports errors — "did the command run" is the gate here,
# not "is the user's config valid".
OUT="$("$EXE" cmd ai.validate-prefs --spawn 2>&1)" || true

if echo "$OUT" | grep -q '"command".*"ai.validate-prefs"' \
   && echo "$OUT" | grep -q '"errors"' \
   && echo "$OUT" | grep -q '"warnings"' ; then
    echo "  ok: ai.validate-prefs emitted a coherent JSON envelope"
    PASSED=$((PASSED + 1))
else
    echo "  FAIL: ai.validate-prefs did not emit the expected envelope"
    echo "  output:"
    echo "$OUT" | sed 's/^/    /'
    FAILED=$((FAILED + 1))
fi

echo
echo "Passed: $PASSED  Failed: $FAILED"
# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
