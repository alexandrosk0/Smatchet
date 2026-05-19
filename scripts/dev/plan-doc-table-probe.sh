#!/bin/bash
# plan-doc-table-probe: scan a plan-doc's § File-level changes table; for
# every symbol / CMake variable named in recognised columns, run git grep
# (symbols) or CMakeLists.txt grep (variables); report hits / misses.
#
# Exit 0 if every named row resolves; exit 1 if any miss.
# Exit 2 on argument / file errors.
# Exit 0 with `no-tables-found` log when the plan has no recognised tables
# (some plans don't carry a file-level table — not a failure).
#
# Usage: bash scripts/dev/plan-doc-table-probe.sh <plan-doc-path>
#
# Recognised column headers (case-insensitive):
#   Symbol | Function | Caller | Definer    -> treated as symbol; git grep -nF
#   CMake variable | CMake target | Variable -> treated as CMake var; grep ... CMakeLists.txt
#
# Section delimiter: starts at the first heading matching
#   ^#+\s+File-level\s+(changes|edits|writes)
# ends at the next heading at the same or higher level (smaller or equal #).
#
# Plan: docs/design/process-backlog-tighten-1-2-3-9-11-12.md § Slice 4

set -euo pipefail

PLAN="${1:-}"
if [ -z "$PLAN" ]; then
    echo "usage: $0 <plan-doc-path>" >&2
    exit 2
fi
if [ ! -f "$PLAN" ]; then
    echo "plan-doc-table-probe: file not found: $PLAN" >&2
    exit 2
fi

# ----------------------------------------------------------------------------
# Extract the § File-level changes section.
# ----------------------------------------------------------------------------
section=$(awk '
    /^#+[[:space:]]+File-level[[:space:]]+(changes|edits|writes)/ {
        match($0, /^#+/)
        section_level = RLENGTH
        in_section = 1
        next
    }
    in_section && /^#+[[:space:]]/ {
        match($0, /^#+/)
        if (RLENGTH <= section_level) {
            in_section = 0
            exit
        }
    }
    in_section { print }
' "$PLAN")

if [ -z "$section" ]; then
    echo "plan-doc-table-probe: no-tables-found (no § File-level changes section in $PLAN)" >&2
    exit 0
fi

# ----------------------------------------------------------------------------
# Parse markdown tables within the section. A markdown table is:
#   | header1 | header2 |
#   |---------|---------|
#   | cell    | cell    |
#
# For each table, identify columns whose header matches a recognised name;
# emit "SYMBOL\t<cell>" or "VAR\t<cell>" lines for cells under those columns.
# ----------------------------------------------------------------------------
extracted=$(printf '%s\n' "$section" | awk '
    BEGIN {
        FS = "|"
        in_table = 0
        sym_cols_str = ""
        var_cols_str = ""
    }
    /^\|/ {
        if (!in_table) {
            # Header row.
            n = NF
            sym_cols_str = ""
            var_cols_str = ""
            for (i = 2; i <= n - 1; i++) {
                h = tolower($i)
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", h)
                if (h == "symbol" || h == "function" || h == "caller" || h == "definer") {
                    sym_cols_str = sym_cols_str " " i
                } else if (h == "cmake variable" || h == "cmake target" || h == "variable") {
                    var_cols_str = var_cols_str " " i
                }
            }
            in_table = 1
            row_idx = 0
            next
        }
        # Separator row (---|---|---) — skip.
        if ($0 ~ /^\|[[:space:]]*[-:]+/) {
            next
        }
        # Data row.
        row_idx++
        n = NF
        split(sym_cols_str, sym_cols, " ")
        split(var_cols_str, var_cols, " ")
        for (i in sym_cols) {
            c = sym_cols[i]
            if (c == "") continue
            cell = $c
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", cell)
            gsub(/`/, "", cell)
            if (cell != "" && cell !~ /^[-—]+$/) {
                print "SYMBOL\t" cell
            }
        }
        for (i in var_cols) {
            c = var_cols[i]
            if (c == "") continue
            cell = $c
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", cell)
            gsub(/`/, "", cell)
            if (cell != "" && cell !~ /^[-—]+$/) {
                print "VAR\t" cell
            }
        }
        next
    }
    {
        # Blank line or non-table line — end the current table.
        in_table = 0
    }
')

if [ -z "$extracted" ]; then
    echo "plan-doc-table-probe: no-tables-found (no recognised Symbol/CMake columns in $PLAN)" >&2
    exit 0
fi

# ----------------------------------------------------------------------------
# Probe each extracted row.
# ----------------------------------------------------------------------------
miss_count=0
hit_count=0

while IFS=$'\t' read -r kind value; do
    [ -z "$value" ] && continue
    case "$kind" in
        SYMBOL)
            # Multi-token cells (e.g. "FooBar.cpp"): grep the literal cell.
            if hits=$(git grep -nF -l -- "$value" 2>/dev/null); then
                count=$(printf '%s\n' "$hits" | wc -l | tr -d ' ')
                first=$(printf '%s\n' "$hits" | head -1)
                printf '%s:SYMBOL\t%s\t%s\t%s\n' "$PLAN" "$value" "$count" "$first"
                hit_count=$((hit_count + 1))
            else
                printf '%s:SYMBOL\t%s\tMISS\n' "$PLAN" "$value"
                miss_count=$((miss_count + 1))
            fi
            ;;
        VAR)
            if hits=$(grep -nE "(set|file\s*\(\s*GLOB).*${value}" CMakeLists.txt tests/CMakeLists.txt 2>/dev/null); then
                count=$(printf '%s\n' "$hits" | wc -l | tr -d ' ')
                first=$(printf '%s\n' "$hits" | head -1)
                printf '%s:VAR\t%s\t%s\t%s\n' "$PLAN" "$value" "$count" "$first"
                hit_count=$((hit_count + 1))
            else
                printf '%s:VAR\t%s\tMISS\n' "$PLAN" "$value"
                miss_count=$((miss_count + 1))
            fi
            ;;
    esac
done <<< "$extracted"

echo "---" >&2
echo "plan-doc-table-probe: $hit_count hits, $miss_count misses" >&2

if [ "$miss_count" -gt 0 ]; then
    exit 1
fi
exit 0
