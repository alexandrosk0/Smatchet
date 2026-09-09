#!/usr/bin/env bash
# test-project-config-roots-bats.sh — bats wrapper for tests/bats/project_config_roots.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# The suite covers the dual-root config seam in scripts/dev/project-config.sh:
# the four config-resolution rungs (PC_CONFIG_FILE override, SMATCHET_PROJECT_CONFIG,
# superproject root, own root), the AGENT_LAYER_ROOT / PROJECT_ROOT exports, and
# that PC_SCHEMA_FILE follows whichever config actually resolved.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-project-config-roots-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

# The suite asserts the resolution ladder from a clean environment; an inherited
# override from the caller's shell would mask a rung.
unset PC_CONFIG_FILE SMATCHET_PROJECT_CONFIG AGENT_LAYER_ROOT PROJECT_ROOT

BATS_FILE="tests/bats/project_config_roots.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-project-config-roots-bats: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$(bats --tap "$BATS_FILE" 2>&1)"; rc=$?
echo "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAIL=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo
echo "Passed: ${PASS}  Failed: ${FAIL}"
if [ "$FAIL" -gt 0 ] || [ "$rc" -ne 0 ]; then exit 1; fi
exit 0
