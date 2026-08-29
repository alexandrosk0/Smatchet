#!/usr/bin/env bash
# test-backlog-counts.sh — self-improvement backlog count tool + regression guard.
#
# History: this used to VERIFY a stored "Live count" column in
# docs/self-improvement/AGENT_SELF_IMPROVEMENT.md against the actual entry counts.
# That stored count was edited by EVERY entry-adding PR, so concurrent PRs
# conflicted on that one line on every add — the highest-frequency self-improvement
# merge conflict. The column was removed 2026-06-03; counts are now on-demand.
#
# So this script now:
#   default  — GUARD: fail if a stored numeric count column was re-introduced
#              into the § Index table (regression of the conflict-prone pattern).
#   --list   — print the live per-category counts. Counts BOTH sources in union:
#              the legacy monolith categories/<cat>.md (grep -c '^- 20' <file>)
#              PLUS the new per-entry files categories/<cat>/*.md (one entry each;
#              see docs/plans/deferred/self-improvement-one-entry-per-file.md). New
#              entries land as their own file so concurrent adds never conflict; the
#              ~135 legacy entries stay in the monolith and are still counted here.
#
# Bypass: SMATCHET_SKIP_BACKLOG_COUNTS=1 (logged when used).
#
# Exit codes:
#   0 — guard clean (or --list printed)
#   1 — a stored numeric count column was re-introduced (conflict-prone)
#   2 — missing index / category file

set -euo pipefail
cd "$(dirname "$0")/../../.."

if [ "${SMATCHET_SKIP_BACKLOG_COUNTS:-0}" = "1" ]; then
    echo "test-backlog-counts: SMATCHET_SKIP_BACKLOG_COUNTS=1 — skipping" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

INDEX="docs/self-improvement/AGENT_SELF_IMPROVEMENT.md"
DIR="docs/self-improvement/categories"
[ -f "$INDEX" ] || { echo "missing index: $INDEX" >&2; exit 2; }

# label -> category file basename.
declare -A FILES=(
    [bug]=bug
    [debt]=debt
    [process]=process
    [tooling]=tooling
    [infra]=infra
    [test]=test
    [security]=security
    [external]=external-blockers
    [applied]=applied
)

# Count the per-entry files for a category: each .md under categories/<basename>/
# is exactly one entry (one-entry-per-file layout). .gitkeep and the legacy
# monolith are excluded. Missing/empty subdir → 0. `applied`/`bug` have no
# subdir (archive stays a union monolith; bug is deprecated) → naturally 0.
_perfile_count() {  # _perfile_count <basename> → integer
    local sub="$DIR/$1" n=0 e
    [ -d "$sub" ] || { printf '0'; return; }
    for e in "$sub"/*.md; do
        [ -f "$e" ] && n=$((n + 1))
    done
    printf '%s' "$n"
}

# --list: print live counts on demand (the replacement for the stored column).
# Counts both sources in union: monolith entry-lines + per-entry files.
if [ "${1:-}" = "--list" ]; then
    for label in bug debt process tooling infra test security external applied; do
        f="$DIR/${FILES[$label]}.md"
        [ -f "$f" ] || { echo "missing category file: $f" >&2; exit 2; }
        # grep -c prints "0" AND exits 1 on zero matches. Under `set -euo pipefail`
        # that nonzero exit aborts the script before the `case` below (so a category
        # with only per-entry files / an empty monolith would crash `--list`), hence
        # the trailing `|| true`. `head -1` guards any multi-line oddity, and the
        # `case` defaults an empty/non-numeric result to 0 → a clean `$(( ))` integer.
        mono="$(grep -c '^- 20' "$f" 2>/dev/null | head -1 || true)"
        case "$mono" in ''|*[!0-9]*) mono=0 ;; esac
        perfile="$(_perfile_count "${FILES[$label]}")"
        # `applied` also counts its rotated monthly partitions (applied-YYYY-MM.md,
        # rotate-applied-md.sh) so the archive total survives rotation.
        rotated=0
        if [ "$label" = applied ]; then
            for part in "$DIR"/applied-*.md; do
                [ -f "$part" ] || continue
                pc="$(grep -c '^- 20' "$part" 2>/dev/null | head -1 || true)"
                case "$pc" in ''|*[!0-9]*) pc=0 ;; esac
                rotated=$((rotated + pc))
            done
        fi
        printf '%-10s %s\n' "$label" "$((mono + perfile + rotated))"
    done
    exit 0
fi

# default: regression guard — the § Index table row shape is `| <label> | <file> |`
# (2 columns). A re-introduced stored count makes the 2nd cell a bare integer.
offenders="$(awk -F'|' '
    /^\| *(bug|process|tooling|infra|test|security|external|applied)/ {
        cell = $3
        gsub(/^[ \t]+|[ \t]+$/, "", cell)
        if (cell ~ /^[0-9]+$/) {
            label = $2
            gsub(/^[ \t]+|[ \t]+$/, "", label)
            print label
        }
    }
' "$INDEX")"

if [ -n "$offenders" ]; then
    echo "test-backlog-counts: a STORED numeric count column was re-introduced into" >&2
    echo "  $INDEX § Index — this is the conflict-prone pattern removed 2026-06-03." >&2
    echo "  Offending row label(s): $offenders" >&2
    echo "  Counts are on-demand: drop the column, use 'test-backlog-counts.sh --list'." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# ADVISORY: applied.md head-boundedness. WARN-only by design — a month boundary
# makes rotation "due" with no accompanying change, so a hard gate would
# spontaneously red CI; archive-backlog-entry.sh rotates for real on the next
# archival. See rotate-applied-md.sh.
if ! bash "$(dirname "$0")/rotate-applied-md.sh" --check >/dev/null 2>&1; then
    echo "test-backlog-counts: WARN — applied.md holds entries older than the previous month;" >&2
    echo "  run 'bash agents/scripts/core/rotate-applied-md.sh' (advisory; not blocking)." >&2
fi

echo "test-backlog-counts: no stored count column (counts are on-demand via --list)."
echo "Passed: 1  Failed: 0"
exit 0
