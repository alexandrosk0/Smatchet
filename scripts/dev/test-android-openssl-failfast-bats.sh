#!/usr/bin/env bash
# test-android-openssl-failfast-bats.sh — bats wrapper for
# tests/bats/android_openssl_failfast.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob. BEHAVIORAL
# companion to the static text gate agents/scripts/project/test-mobile-security.sh:
# it drives the REAL Issue #1068 fail-fast (cmake/SmatchetThirdParty.cmake's
# smatchet_prepare_cpr) in `cmake -P` script mode against committed fixture trees,
# proving the EXISTS-triple guard LOGIC actually fails the configure — not just
# that the marker text is present.
#
# Plan: docs/plans/active/mobile-mvp-completion.md (WS5 item 19).
#
# Exit codes (test-author convention):
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools)

set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-android-openssl-failfast-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/android_openssl_failfast.bats"
[ -f "$BATS_FILE" ] || { echo "missing $BATS_FILE" >&2; exit 1; }

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
rc=$?
echo "$OUT"
p=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
f=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${p}  Failed: ${f}"
[ "$f" -gt 0 ] || [ "$rc" -ne 0 ] && exit 1
exit 0
