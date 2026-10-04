#!/usr/bin/env bash
# test-doc-anchors.sh — verify every `AGENTS.md § <section>` reference resolves.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
# Implementation in Python next to this shim (bash regex too brittle for the
# heading-extraction + reference-matching shape).

set -uo pipefail

PY="${PYTHON:-}"
if [ -n "$PY" ]; then
    if ! "$PY" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' >/dev/null 2>&1; then
        echo "test-doc-anchors: PYTHON='$PY' is not an executable Python 3" >&2
        exit 2
    fi
else
    for candidate in python python3 py; do
        _resolved="$(command -v "$candidate" 2>/dev/null)" || continue
        [ -n "$_resolved" ] || continue
        if "$_resolved" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
            PY="$_resolved"
            break
        fi
    done
fi
[ -n "$PY" ] || { echo "test-doc-anchors: python3 (or python) is required" >&2; exit 2; }

# The references are scanned in PROJECT_ROOT (scripts/dev/project-config.sh — the
# superproject once this script lives in the agent-layer/ submodule); the anchors
# they must resolve to are layer content, which test_doc_anchors.py reads from its
# own tree. The .py is run by its own path: after the flip the host has no
# agents/scripts/core/ of its own.
_tda_self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$_tda_self/../../../scripts/dev/project-config.sh" ]; then
    # shellcheck source=scripts/dev/project-config.sh
    PC_ROOTS_ONLY=1 . "$_tda_self/../../../scripts/dev/project-config.sh" || true
else
    unset PROJECT_ROOT AGENT_LAYER_ROOT  # no config beside this script (a fixture copy): its own tree, never an inherited root
fi
cd "${PROJECT_ROOT:-$(git rev-parse --show-toplevel)}" || exit 2
export PROJECT_ROOT="${PROJECT_ROOT:-$(pwd)}"
exec "$PY" "$_tda_self/test_doc_anchors.py" "$@"
