#!/usr/bin/env bash
# test-merge-watcher-integration-bats.sh — bats wrapper for
# tests/bats/merge_watcher_integration.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Wraps the Phase-5 end-to-end suite for the merge-watcher daemon — it stubs
# `gh` on PATH with fixture responses to drive a fake PR through the full
# lifecycle (register → poll BLOCKED → poll PASSED → squash-merge → cascade →
# unregister), so it is self-contained / offline-runnable (no live GitHub, no
# gh auth). Emits the canonical `Passed: N  Failed: M` line that test-all.sh
# greps for. Before this wrapper the suite ran under no gate or CI (the
# windows-2022 CI runner has no bats), so regressions rotted silently — it was
# in fact 0/4 on Windows AND structurally broken on every platform until this
# was gated (see below). Mirrors test-merge-watcher-bats.sh.
#
# git-bash friendly: the bats file itself handles the two Windows-only seams —
# (1) driveless LOCALAPPDATA via `cygpath -m` (mirrors PR #527's merge_watcher
# .bats fix), and (2) a `gh.cmd` shim so native-Windows Python's
# shutil.which("gh") + subprocess resolve the stub rather than the real
# gh.EXE (merge-watcher.py pins GH_BIN at import). This wrapper only shells out
# to bats, so no extra path massaging is needed here.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-merge-watcher-integration-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/merge_watcher_integration.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-merge-watcher-integration-bats: $BATS_FILE not found" >&2
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

if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then
    exit 1
fi
exit 0
