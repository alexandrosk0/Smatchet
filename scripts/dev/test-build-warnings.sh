#!/usr/bin/env bash
# test-build-warnings.sh — asserts the ninja-iter-msvc build emits zero `-Wunused-*`
# warnings on Smatchet-owned translation units (Source/Standalone/, Source/Core/,
# Source/Plugins/). FetchContent dependencies under build/<preset>/_deps/ are excluded —
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

# Test / re-scan seam: SMATCHET_WARN_LOG_OVERRIDE points the scan at an existing
# build log and skips configure+build, so tests/bats/build_warnings_gate.bats can
# exercise the toolchain-format matcher against fixture logs without a compiler.
if [ -n "${SMATCHET_WARN_LOG_OVERRIDE:-}" ]; then
    LOG="$SMATCHET_WARN_LOG_OVERRIDE"
    [ -f "$LOG" ] || { echo "override log not found: ${LOG}" >&2; exit 2; }
else
    command -v cmake >/dev/null 2>&1 || { echo "cmake required" >&2; exit 2; }
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
fi

# Match the unused-* warning family on Smatchet-owned paths only, across BOTH
# toolchains this gate can build under. The default ninja-iter-msvc preset compiles
# with cl.exe, whose diagnostics NEVER carry the GCC "[-Wunused-..]" tag — so matching
# only that form left the gate permanently blind (always "Passed: 1") under MSVC:
#   • MSVC cl.exe            → "warning C4101/C4189/C4505:" = unused var / unused-but-set / unused fn.
#   • GCC / Clang / clang-cl → "warning: ... [-Wunused-..]" tag form.
# FetchContent deps under _deps/ are upstream, excluded. Path separator may be "/"
# (ninja) or "\" (MSVC), so the owned-path filter accepts both.
OWNED_HITS=$(grep -E 'warning:.*\[-Wunused-|warning C(4101|4189|4505):' "$LOG" \
    | grep -vE '[\\/]_deps[\\/]' \
    | grep -E '(Source[\\/]Standalone|Source[\\/]Core|Plugins)[\\/]' \
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
