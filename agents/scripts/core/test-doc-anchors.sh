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
    if ! "$PY" -c "" >/dev/null 2>&1; then
        echo "test-doc-anchors: PYTHON='$PY' is not executable" >&2
        exit 2
    fi
else
    for candidate in python python3 py; do
        if command -v "$candidate" >/dev/null 2>&1 && \
           "$candidate" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
            PY="$candidate"
            break
        fi
    done
fi
[ -n "$PY" ] || { echo "test-doc-anchors: python3 (or python) is required" >&2; exit 2; }

cd "$(git rev-parse --show-toplevel)"
exec "$PY" agents/scripts/core/test_doc_anchors.py "$@"
