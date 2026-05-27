#!/usr/bin/env bash
# test-build-warnings.sh — asserts the ninja-iter-msvc build emits zero `-Wunused-*`
# warnings on Smatchet-owned translation units (Target_Standalone/, Source_Core/,
# Plugins/). FetchContent dependencies under build/<preset>/_deps/ are excluded —
# their warnings are upstream issues, not ours to gate on.
#
# Rationale: dead file-static helpers accumulate silently after refactors. Once
# they exist, every subsequent build re-emits the warning, which trains agents
# and humans to ignore the warning channel. This test fails fast on the first
# unused-function / unused-variable / unused-but-set in our own code.
#
# Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 — zero unused warnings in Smatchet-owned TUs
#   1 — at least one unused warning detected
#   2 — build failed or cmake misconfigured

set -euo pipefail

cd "$(dirname "$0")/../.."

PRESET="${SMATCHET_WARN_PRESET:-ninja-iter-msvc}"
BUILD_DIR="build/${PRESET}"
LOG="${BUILD_DIR}/_warn-build.log"

mkdir -p "$BUILD_DIR"

# Reconfigure only if the preset hasn't been configured yet.
if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "Configuring preset ${PRESET}..."
    cmake --preset "$PRESET" >/dev/null 2>&1 || { echo "cmake preset failed"; exit 2; }
fi

echo "Building preset ${PRESET}..."
cmake --build --preset "$PRESET" 2>&1 | tee "$LOG" >/dev/null || {
    echo "build failed — see ${LOG}"
    exit 2
}

# Match GCC's "warning: ... [-Wunused-..]" tag form on Smatchet-owned paths only.
# The leading "../../" prefix is how ninja prints sources relative to BUILD_DIR; on
# MinGW the separator is "\" so the regex must accept both [\\/].
OWNED_HITS=$(grep -E 'warning:.*\[-Wunused-' "$LOG" \
    | grep -vE '[\\/]_deps[\\/]' \
    | grep -E '(Target_Standalone|Source_Core|Plugins)[\\/]' \
    || true)

if [ -n "$OWNED_HITS" ]; then
    echo
    echo "Unused warnings detected in Smatchet-owned TUs:"
    echo "$OWNED_HITS"
    COUNT=$(echo "$OWNED_HITS" | wc -l | tr -d ' ')
    echo "Passed: 0  Failed: ${COUNT}"
    exit 1
fi

echo "Passed: 1  Failed: 0"
exit 0
