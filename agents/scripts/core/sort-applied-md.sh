#!/usr/bin/env bash
# agents/scripts/core/sort-applied-md.sh — restore "Latest first" ordering on
# applied.md after a `merge=union` driver concatenation interleaves dates.
#
# Companion to the `merge=union` driver in `.gitattributes` (per
# docs/agent-rules/process-rules.md § Backlog-archive union merge). The driver
# concatenates parallel prepends verbatim — date order may interleave on the
# merge commit. This script re-sorts entries by their own date, descending,
# while preserving:
#   - the file header (lines before the first entry)
#   - each entry's multi-line block (Resolution lines after the title)
#   - blank-line separators between entries
# Both entry shapes count (split by the shared applied_md_lib.py): a legacy
# `- YYYY-MM-DD · …` block sorts by that date, and a per-entry-file block
# (`# <title>` + `**Date**:`-style metadata) by the date in its own metadata,
# not the date of whichever legacy block it follows.
#
# Usage:
#   bash agents/scripts/core/sort-applied-md.sh
#   bash agents/scripts/core/sort-applied-md.sh --check    # exit 1 if reorder needed
#
# Env: SMATCHET_APPLIED_MD=<path>  sort that file instead of applied.md.
#
# Exit codes:
#   0 — applied.md sorted (or already sorted in --check)
#   1 — --check mode, file is out of order; OR Python parse error propagated
#       via `set -e` (cannot distinguish — both surface as exit 1)
#   2 — file not found

set -euo pipefail

SORT_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$SORT_LIB_DIR/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "python3 required (no working interpreter on PATH)" >&2; exit 2; }

# Dual-root bootstrap (plan agent-surface-extraction-repo, Phase A row 3a).
# The climb is location-relative: pre-flip it lands on the repo root, post-flip
# on agent-layer/, and project-config.sh resolves the HOST tree from there.
# Best-effort, with an explicit fallback to the climb this script used before,
# so a reduced tree that carries agents/ without scripts/dev/ behaves as today.
_sam_self_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
if [ -f "$_sam_self_root/scripts/dev/project-config.sh" ]; then
    # shellcheck source=scripts/dev/project-config.sh
    . "$_sam_self_root/scripts/dev/project-config.sh" 2>/dev/null || true
else
    unset PROJECT_ROOT AGENT_LAYER_ROOT  # no config beside this script (a fixture copy): its own tree, never an inherited root
fi
: "${PROJECT_ROOT:=$_sam_self_root}"

# applied.md is HOST content and stays host-side permanently — it is the
# merge=union entries file (grill decision 4), and this script is its repair
# tool: a LAYER script reading a HOST path (plan row 7b names the pair). The
# `cd` climb that used to anchor it lands in the layer post-flip, where the file
# does not exist, so the path is anchored on $PROJECT_ROOT instead. cwd is now
# the host tree for the same reason.
cd "$PROJECT_ROOT"

APPLIED="${SMATCHET_APPLIED_MD:-$PROJECT_ROOT/docs/self-improvement/categories/applied.md}"
CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
fi

if [ ! -f "$APPLIED" ]; then
    echo "sort-applied-md: $APPLIED not found" >&2
    exit 2
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# Read the file. Lines before the first entry are the header (kept verbatim at
# the top). Each entry runs until the next entry or EOF (Resolution / Status /
# Last-reviewed continuation lines, and a per-entry block's `## ` sections, stay
# with it).
"$PY" - "$APPLIED" "$TMP" "$SORT_LIB_DIR" <<'PY'
import sys

src, dst, lib_dir = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, lib_dir)
import applied_md_lib as aml  # noqa: E402  (sibling module; path set above)

# newline="" on both ends: text mode would otherwise translate every newline
# to the platform separator on write, so on Windows the rewritten copy differs
# from the source in line endings alone and the `cmp -s` below never matches:
# --check reports a sorted file as unsorted, and a real run rewrites the whole
# file to CRLF.
with open(src, encoding="utf-8", newline="") as f:
    lines = f.readlines()

header, entries = aml.split_entries(lines)
# Descending by each entry's own date; stable, so equal dates keep file order.
entries = aml.sort_latest_first(entries)

with open(dst, "w", encoding="utf-8", newline="") as f:
    f.writelines(header)
    for _, block in entries:
        f.writelines(block)
PY

if [ "$CHECK_ONLY" -eq 1 ]; then
    if cmp -s "$APPLIED" "$TMP"; then
        echo "sort-applied-md: $APPLIED is sorted"
        exit 0
    else
        echo "sort-applied-md: $APPLIED is NOT sorted (run without --check to fix)" >&2
        exit 1
    fi
fi

if cmp -s "$APPLIED" "$TMP"; then
    echo "sort-applied-md: $APPLIED already sorted; no changes"
    exit 0
fi

mv "$TMP" "$APPLIED"
trap - EXIT
echo "sort-applied-md: restored Latest-first ordering on $APPLIED"
