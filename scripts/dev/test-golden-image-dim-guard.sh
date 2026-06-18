#!/usr/bin/env bash
# test-golden-image-dim-guard.sh — regression gate for the GoldenImage.h image
# dimension guard (security audit hardening: crafted-header allocation bomb).
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# g++-compiles tests/support/GoldenImageDimGuardMain.cpp (single TU, same
# toolchain pattern as test-screenshot-diff.sh) and runs it. The CLI crafts
# malicious + honest PNM fixtures and asserts the LoadImage dimension guard
# rejects an oversize-header alloc bomb BEFORE decoding/allocating, accepts
# honest images at/under the 16384 cap, and rejects just over it. The CLI emits
# the canonical `Passed: N  Failed: M` line this script forwards for test-all.sh.
#
# Exit codes (test-author convention):
#   0 — every case passed
#   1 — at least one case failed
#   2 — g++ missing (cannot build the one-TU helper)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

GXX="${CXX:-g++}"
if ! command -v "$GXX" >/dev/null 2>&1; then
    echo "test-golden-image-dim-guard: $GXX not found — cannot compile the one-TU helper." >&2
    echo "  Install g++ (MSYS2/UCRT64: pacman -S mingw-w64-ucrt-x86_64-gcc) or set CXX." >&2
    echo "Passed: 0  Failed: 0  (skipped — g++ missing)"
    exit 2
fi

SRC="tests/support/GoldenImageDimGuardMain.cpp"
if [ ! -f "$SRC" ]; then
    echo "test-golden-image-dim-guard: $SRC not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

TMP_DIR="${TMPDIR:-/tmp}/smatchet-dimguard-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT

BIN="$TMP_DIR/dimguard"
echo "[test-golden-image-dim-guard] compiling via $GXX..."
if ! "$GXX" -std=c++14 -O2 -Wall -Wextra -Wpedantic \
    -Itests/support -ISource/Core/ThirdParty \
    -o "$BIN" "$SRC"; then
    echo "test-golden-image-dim-guard: compile failed" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Run the helper; it writes its own fixtures into the scratch dir and prints
# the canonical Passed/Failed line. Forward its output + exit code verbatim.
"$BIN" "$TMP_DIR"
exit $?
