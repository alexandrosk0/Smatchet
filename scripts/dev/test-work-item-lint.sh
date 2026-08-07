#!/bin/bash
# test-work-item-lint.sh — wrapper for scripts/dev/test_work_item_lint.py, the
# regression suite for agents/scripts/core/work_item_lint.py (work-item close +
# ledger-citation linter; docs/agent-rules/work-items.md).
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob; also run by
# scripts/dev/test-docs.sh / doc-validation.yml (the linter gates doc shapes).
#
# Exit codes follow the test-author convention:
#   0 — every unittest scenario passed
#   1 — at least one failed
#   2 — no working python on PATH
#
# The canonical `Passed: N  Failed: M` line is emitted by the Python suite
# itself; this wrapper only resolves the interpreter (see test-docs.sh for why
# candidates are EXECUTED, not just resolved — the Windows Store `python3`
# stub passes `command -v` but cannot run).

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

PY=""
for _c in python3 python py; do
  if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "" >/dev/null 2>&1; then PY="$_c"; break; fi
done
[ -n "$PY" ] || {
  echo "test-work-item-lint: no working python on PATH" >&2; exit 2; }

"$PY" scripts/dev/test_work_item_lint.py
