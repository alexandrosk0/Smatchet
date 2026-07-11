#!/usr/bin/env bash
# test-cppcheck-path-detection.sh — bucket-A regression test for run_cppcheck.py path-filter.
#
# Bug: previously the filter hard-coded the literal "Smatchet" in a regex over each entry's
# absolute path. Any clone path not containing "Smatchet" (CI tmpdirs, evaluator clones like
# Codex_eval_smatchet, contributor workspaces) produced 0 filtered entries and a silent no-op.
# The fix keys the filter off `root = Path(__file__).resolve().parents[3]` via `relative_to`,
# matching first-party TUs by their position under the repo root, not by literal substring.
#
# Strategy: synthesise a compile_commands.json in a temp dir whose path does NOT contain
# "Smatchet". Copy scripts/dev/local/run_cppcheck.py + a minimal directory shadow so the script's
# `parents[3]` resolves to that temp root. Run the script and assert non-zero entry count.
# This is a hermetic unit-style test — no real cmake, no real cppcheck — that exercises only
# the predicate that regressed.
#
# Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 — every assertion passed
#   1 — at least one assertion failed
#   2 — missing python / unable to create scratch dir

set -euo pipefail

cd "$(dirname "$0")/../.."
REPO_ROOT="$(pwd -W 2>/dev/null || pwd)"

PYTHON="${PYTHON:-python}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: python not on PATH"
    exit 2
fi

# Deliberately do NOT include the literal "Smatchet" anywhere in the scratch path.
SCRATCH="$(mktemp -d -t codex-eval-rename.XXXXXX)" || {
    echo "Passed: 0  Failed: 1"
    echo "ERROR: mktemp failed"
    exit 2
}
trap 'rm -rf "$SCRATCH"' EXIT

case "$SCRATCH" in
    *[Ss]matchet*)
        echo "Passed: 0  Failed: 1"
        echo "ERROR: scratch dir $SCRATCH unexpectedly contains 'smatchet'"
        exit 1
        ;;
esac

# Shadow tree mirroring the parents[2] layout the script expects.
mkdir -p "$SCRATCH/scripts/dev/local" "$SCRATCH/Source/Core/src" "$SCRATCH/Source/Plugins/Mcp" "$SCRATCH/Source/Standalone" "$SCRATCH/build"
cp "$REPO_ROOT/scripts/dev/local/run_cppcheck.py" "$SCRATCH/scripts/dev/local/run_cppcheck.py"

# Resolve the absolute scratch root the way Python's Path.resolve() will.
SCRATCH_ABS="$("$PYTHON" -c "import sys, pathlib; print(pathlib.Path(sys.argv[1]).resolve())" "$SCRATCH")"

# Synthesise a compile DB with three first-party entries, one _deps entry, and one
# unrelated absolute path that lies outside the scratch root.
DB="$SCRATCH/compile_commands.json"
"$PYTHON" - "$SCRATCH_ABS" "$DB" <<'PY'
import json
import sys
from pathlib import Path

scratch = Path(sys.argv[1])
db_path = Path(sys.argv[2])

entries = [
    {"directory": str(scratch / "build"), "command": "cc", "file": str(scratch / "Source/Core" / "src" / "AppController.cpp")},
    {"directory": str(scratch / "build"), "command": "cc", "file": str(scratch / "Source" / "Plugins" / "Mcp" / "McpServer.cpp")},
    {"directory": str(scratch / "build"), "command": "cc", "file": str(scratch / "Source/Standalone" / "main.cpp")},
    {"directory": str(scratch / "build"), "command": "cc", "file": str(scratch / "build" / "_deps" / "json-src" / "json.hpp")},
    {"directory": "/tmp", "command": "cc", "file": "/usr/include/stdio.h"},
]
db_path.write_text(json.dumps(entries, indent=2), encoding="utf-8")
PY

PASSED=0
FAILED=0

# Assertion 1 — running the script from the scratch tree filters down to the three first-party entries.
OUT="$("$PYTHON" "$SCRATCH/scripts/dev/local/run_cppcheck.py" --no-run --compile-db "$DB" --out-db "$SCRATCH/build/compile_commands.cppcheck.json" 2>&1 || true)"
echo "$OUT"

LINE="$(echo "$OUT" | grep -E '^wrote [0-9]+ entries' | tail -n 1 || true)"
if [ -z "$LINE" ]; then
    echo "FAIL: no 'wrote N entries' summary line"
    FAILED=$((FAILED + 1))
else
    N="$(echo "$LINE" | sed -E 's/^wrote ([0-9]+) entries.*/\1/')"
    if [ "$N" -eq 3 ]; then
        echo "PASS: filter kept the 3 first-party entries in a non-Smatchet path"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL: expected 3 entries, got $N"
        FAILED=$((FAILED + 1))
    fi
fi

# Assertion 2 — the filtered DB contains exactly the three first-party file paths, no _deps, no system headers.
FILT="$SCRATCH/build/compile_commands.cppcheck.json"
if [ ! -f "$FILT" ]; then
    echo "FAIL: filtered DB not written at $FILT"
    FAILED=$((FAILED + 1))
else
    CHECK="$("$PYTHON" - "$FILT" <<'PY'
import json
import sys
from pathlib import Path

filt = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
files = [Path(e["file"]).name for e in filt]
expected = {"AppController.cpp", "McpServer.cpp", "main.cpp"}
got = set(files)
if got == expected:
    print("OK")
else:
    print(f"BAD: expected {sorted(expected)}, got {sorted(got)}")
PY
)"
    if [ "$CHECK" = "OK" ]; then
        echo "PASS: filtered set is exactly the 3 expected first-party files"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL: $CHECK"
        FAILED=$((FAILED + 1))
    fi
fi

echo "Passed: $PASSED  Failed: $FAILED"

# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
