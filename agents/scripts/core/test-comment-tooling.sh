#!/usr/bin/env bash
# test-comment-tooling.sh — fixture tests for the reduce-source-comment-bloat tooling
# (comment_lib.py / comment_audit.py / comment_strip.py). Auto-enrolled by test-all.sh via the
# test-*.sh glob. Pure stdlib Python; no build required.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PY="${PYTHON:-python3}"

command -v "$PY" >/dev/null 2>&1 || { echo "test-comment-tooling: $PY not found" >&2; exit 2; }

"$PY" "$REPO_ROOT/tests/dev/comment_tooling/test_comment_tooling.py"
rc=$?

# Smoke: the analyzer must run clean over the real corpus and report a sane comment ratio.
"$PY" "$SCRIPT_DIR/comment_audit.py" >/dev/null || { echo "test-comment-tooling: comment_audit.py crashed on the corpus" >&2; exit 1; }

exit $rc
