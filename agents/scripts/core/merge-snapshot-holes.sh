#!/usr/bin/env bash
# merge-snapshot-holes.sh — detect recent merges into the protected branch that
# have NO merge-time snapshot row in docs/self-improvement/merge-snapshots.jsonl,
# while they are still inside git-janitor's backfill window, and nudge for the
# repair (process/2026-08-18-merge-snapshot-ledger-28-pr-hole).
#
# WHY: the ledger is the only lossless record of a merge's gate verdict (ADR-0017),
# and a missing row is repairable for a few hours only — `git-janitor.sh
# --post-merge <N>` Step 5.5 backfills it while the merge is younger than
# SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS (default 6), after which the
# retro-compose prohibition makes the hole permanent. No session that skipped
# its own append notices it skipped, so this check fires on the GAP itself:
# merged PR numbers vs ledger PR numbers, inside that window. Twenty-eight and
# then fifty-eight consecutive merges went un-snapshotted before anyone looked.
#
# Data: merged PRs over REST — `gh api repos/<owner>/<repo>/pulls?state=closed
# &base=<branch>&sort=updated&direction=desc` (no GraphQL), keeping rows whose
# merged_at falls inside the window. Paging stops at the first page whose
# oldest `updated_at` predates the window (merged_at <= updated_at, so nothing
# older can qualify) or at MERGE_SNAPSHOT_HOLES_MAX_PAGES. Ledger PR numbers are
# the UNION of the working-tree ledger, origin/<branch>'s committed ledger and
# every sibling worktree's ledger — a row is appended uncommitted by whichever
# session merged, so a row still waiting for its commit in another worktree on
# this machine is not a hole.
#
# Modes:
#   --list     (default) plain "merge-snapshot hole: PR #N …" lines + a clean line.
#   --nudge    SessionStart-formatted block (silent when nothing is owed).
#   --selftest run inline fixtures against a stub gh; exit 0/1.
#
# Advisory — never blocks: exit 0 always (2 on a usage error). A missing /
# unauthenticated gh or an unresolvable repo degrades to a stderr notice; a
# FAILED fetch says "window NOT scanned" on stderr and never prints the clean
# line (a fetch that read nothing must not read as "no holes").
#
# Env (production leaves the test seams unset):
#   SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS  window in hours — the SAME knob that
#                              caps git-janitor's backfill (default 6; 0 = every
#                              merge on the fetched pages; non-integer → 6).
#   MERGE_SNAPSHOT_LEDGER      exact ledger file to read (the appender's seam);
#                              when set, the origin/worktree union is skipped.
#   MERGE_SNAPSHOT_HOLES_BASE  base branch (default project.config.json
#                              branch_protection.branch, else develop).
#   MERGE_SNAPSHOT_HOLES_MAX_PAGES / _PER_PAGE  paging bound (default 3 × 100).
#   MERGE_SNAPSHOT_HOLES_NOW_EPOCH  TEST-ONLY: pin "now" (epoch seconds).
#   REPO                       owner/name override (lib/resolve-repo.sh).
#
# selftest: asserts-failure
# ----------------------------------------------------------------------------

set -uo pipefail

_SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname "$_SCRIPT_PATH")"
cd "$SCRIPT_DIR/../../.." || exit 2

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --selftest) MODE="selftest" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: merge-snapshot-holes.sh [--list|--nudge|--selftest]" >&2; exit 2 ;;
esac

LEDGER_REL="docs/self-improvement/merge-snapshots.jsonl"

# notice <msg> — advisory degrade line. stderr in every mode: a SessionStart
# nudge must not print a "clean-looking" stdout block for a scan that never ran.
notice() { echo "merge-snapshot-holes: $*" >&2; }

# --- selftest ----------------------------------------------------------------
run_selftest() {
    local tmp fail=0 out
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064  # expand $tmp now: the trap must remove THIS dir
    trap "rm -rf '$tmp'" RETURN
    mkdir -p "$tmp/bin"
    cat > "$tmp/bin/gh" <<'STUB'
#!/usr/bin/env bash
case "$1" in
    auth) exit 0 ;;
    api)
        if [ -n "${SELFTEST_GH_FAIL:-}" ]; then echo "HTTP 504: 504 Gateway Timeout" >&2; exit 1; fi
        case "$2" in *"page=1"*) cat "$SELFTEST_PAGE1" ;; *) echo '[]' ;; esac
        exit 0 ;;
esac
exit 0
STUB
    chmod +x "$tmp/bin/gh"
    # now = 2026-10-04T12:00:00Z; window 6 h → cutoff 06:00:00Z.
    cat > "$tmp/page1.json" <<'JSON'
[{"number":901,"merged_at":"2026-10-04T11:00:00Z","updated_at":"2026-10-04T11:00:05Z"},
 {"number":902,"merged_at":"2026-10-04T10:00:00Z","updated_at":"2026-10-04T10:00:05Z"},
 {"number":903,"merged_at":null,"updated_at":"2026-10-04T09:00:00Z"},
 {"number":904,"merged_at":"2026-10-03T09:00:00Z","updated_at":"2026-10-03T09:00:05Z"}]
JSON
    echo '{"pr":902,"mergeCommit":"m902","gates":"GATES_PASSED"}' > "$tmp/ledger.jsonl"
    out=$(PATH="$tmp/bin:$PATH" REPO="x/y" MERGE_SNAPSHOT_LEDGER="$tmp/ledger.jsonl" \
          MERGE_SNAPSHOT_HOLES_NOW_EPOCH=1791115200 SELFTEST_PAGE1="$tmp/page1.json" \
          SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS=6 \
          bash "$_SCRIPT_PATH" --list 2>&1)
    # selftest: asserts-failure — an un-snapshotted in-window merge MUST be flagged.
    case "$out" in *"merge-snapshot hole: PR #901 "*) : ;; *) echo "FAIL: in-window #901 with no row not flagged"; fail=1 ;; esac
    case "$out" in *"PR #902"*) echo "FAIL: #902 has a row but was flagged"; fail=1 ;; esac
    case "$out" in *"PR #903"*) echo "FAIL: closed-unmerged #903 flagged"; fail=1 ;; esac
    case "$out" in *"PR #904"*) echo "FAIL: out-of-window #904 flagged"; fail=1 ;; esac
    # A failed fetch must say NOT scanned, never the clean line.
    out=$(PATH="$tmp/bin:$PATH" REPO="x/y" MERGE_SNAPSHOT_LEDGER="$tmp/ledger.jsonl" \
          MERGE_SNAPSHOT_HOLES_NOW_EPOCH=1791115200 SELFTEST_PAGE1="$tmp/page1.json" \
          SELFTEST_GH_FAIL=1 bash "$_SCRIPT_PATH" --list 2>&1)
    case "$out" in *"NOT scanned"*) : ;; *) echo "FAIL: failed fetch did not say NOT scanned"; fail=1 ;; esac
    case "$out" in *"no ledger holes"*) echo "FAIL: failed fetch printed the clean line"; fail=1 ;; esac
    if [ "$fail" -eq 0 ]; then echo "merge-snapshot-holes --selftest: PASS (6/6)"; return 0; fi
    echo "merge-snapshot-holes --selftest: FAIL"; return 1
}

if [ "$MODE" = "selftest" ]; then
    run_selftest
    exit $?
fi

# --- preconditions (advisory degrade) ------------------------------------------
if ! command -v jq >/dev/null 2>&1; then
    [ "$MODE" = "list" ] && notice "jq not on PATH — skipped (advisory)"
    exit 0
fi
# shellcheck source=agents/scripts/core/lib/resolve-repo.sh
. "$SCRIPT_DIR/lib/resolve-repo.sh"
if ! REPO="$(resolve_repo)"; then
    [ "$MODE" = "list" ] && notice "cannot resolve repo (set REPO=owner/name or authenticate gh) — skipped (advisory)"
    exit 0
fi
if ! command -v gh >/dev/null 2>&1 || ! gh auth status >/dev/null 2>&1; then
    [ "$MODE" = "list" ] && notice "gh unavailable/unauthenticated — skipped (advisory)"
    exit 0
fi

BASE="${MERGE_SNAPSHOT_HOLES_BASE:-}"
if [ -z "$BASE" ] && [ -f project.config.json ]; then
    BASE="$(jq -r '.branch_protection.branch // empty' project.config.json 2>/dev/null)" || BASE=""
fi
BASE="${BASE:-develop}"

MAX_AGE_H="${SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS:-6}"
case "$MAX_AGE_H" in ''|*[!0-9]*) MAX_AGE_H=6 ;; esac
MAX_PAGES="${MERGE_SNAPSHOT_HOLES_MAX_PAGES:-3}"
case "$MAX_PAGES" in ''|*[!0-9]*|0) MAX_PAGES=3 ;; esac
PER_PAGE="${MERGE_SNAPSHOT_HOLES_PER_PAGE:-100}"
case "$PER_PAGE" in ''|*[!0-9]*|0) PER_PAGE=100 ;; esac
NOW_EPOCH="${MERGE_SNAPSHOT_HOLES_NOW_EPOCH:-$(date +%s)}"
case "$NOW_EPOCH" in ''|*[!0-9]*) NOW_EPOCH="$(date +%s)" ;; esac

# ISO-8601 UTC sorts lexicographically in time order, so the window test is a
# plain string compare. 0 = uncapped (the janitor's own meaning of 0).
CUTOFF=""
if [ "$MAX_AGE_H" != "0" ]; then
    CUTOFF="$(jq -rn --argjson e "$NOW_EPOCH" --argjson h "$MAX_AGE_H" '($e - $h * 3600) | todate' 2>/dev/null)" || CUTOFF=""
    if [ -z "$CUTOFF" ]; then
        notice "could not compute the ${MAX_AGE_H}h window cutoff (date/jq) — window NOT scanned"
        exit 0
    fi
fi

# --- merged PRs inside the window (REST, bounded pages) ------------------------
merged_rows=()   # "<number><TAB><merged_at>"
page=1
while [ "$page" -le "$MAX_PAGES" ]; do
    err_file="$(mktemp)"
    if ! body="$(gh api "repos/$REPO/pulls?state=closed&base=$BASE&sort=updated&direction=desc&per_page=$PER_PAGE&page=$page" 2>"$err_file")"; then
        first_err="$(head -n 1 "$err_file" 2>/dev/null || true)"
        rm -f "$err_file"
        notice "merged-PR fetch failed (${first_err:-no error text}) — window NOT scanned"
        exit 0
    fi
    rm -f "$err_file"
    # `|`-joined, not @tsv: tab is IFS-whitespace, so `read` would collapse an
    # empty merged_at (a closed-unmerged PR) and shift updated_at into it.
    if ! rows="$(printf '%s' "$body" | jq -r '.[] | [(.number | tostring), (.merged_at // ""), (.updated_at // "")] | join("|")' 2>/dev/null)"; then
        notice "merged-PR fetch returned unparseable JSON — window NOT scanned"
        exit 0
    fi
    n_items=0
    oldest_updated=""
    while IFS='|' read -r num merged_at updated_at; do
        [ -n "$num" ] || continue
        n_items=$((n_items + 1))
        if [ -n "$updated_at" ] && { [ -z "$oldest_updated" ] || [[ "$updated_at" < "$oldest_updated" ]]; }; then
            oldest_updated="$updated_at"
        fi
        [ -n "$merged_at" ] || continue                        # closed, never merged
        if [ -n "$CUTOFF" ] && [[ "$merged_at" < "$CUTOFF" ]]; then continue; fi
        merged_rows+=("$num"$'\t'"$merged_at")
    done <<< "$rows"
    [ "$n_items" -lt "$PER_PAGE" ] && break                    # last page
    if [ -n "$CUTOFF" ] && [ -n "$oldest_updated" ] && [[ "$oldest_updated" < "$CUTOFF" ]]; then
        break                                                  # the rest is older
    fi
    page=$((page + 1))
done

# --- ledger PR numbers (union) ---------------------------------------------------
ledger_prs() {
    if [ -n "${MERGE_SNAPSHOT_LEDGER:-}" ]; then
        [ -f "$MERGE_SNAPSHOT_LEDGER" ] && jq -r '.pr // empty' "$MERGE_SNAPSHOT_LEDGER" 2>/dev/null
        return 0
    fi
    [ -f "$LEDGER_REL" ] && jq -r '.pr // empty' "$LEDGER_REL" 2>/dev/null
    git show "origin/$BASE:$LEDGER_REL" 2>/dev/null | jq -r '.pr // empty' 2>/dev/null
    local wt
    while IFS= read -r wt; do
        wt="${wt#worktree }"
        [ -f "$wt/$LEDGER_REL" ] && jq -r '.pr // empty' "$wt/$LEDGER_REL" 2>/dev/null
    done < <(git worktree list --porcelain 2>/dev/null | grep '^worktree ' || true)
    return 0
}
declare -A HAVE_ROW=()
while IFS= read -r p; do
    [ -n "$p" ] && HAVE_ROW[$p]=1
done < <(ledger_prs | tr -d '\r' || true)

holes=()
for r in "${merged_rows[@]+"${merged_rows[@]}"}"; do
    num="${r%%$'\t'*}"
    merged_at="${r#*$'\t'}"
    [ -n "${HAVE_ROW[$num]:-}" ] && continue
    holes+=("PR #$num merged $merged_at")
done

# --- emit --------------------------------------------------------------------------
if [ "$MAX_AGE_H" = "0" ]; then
    window_desc="on the fetched pages"; repair_window="uncapped"
else
    window_desc="in the last ${MAX_AGE_H}h"; repair_window="${MAX_AGE_H}h"
fi
if [ "${#holes[@]}" -eq 0 ]; then
    [ "$MODE" = "list" ] && echo "merge-snapshot-holes: no ledger holes among the ${#merged_rows[@]} PR(s) merged into $BASE $window_desc."
    exit 0
fi

if [ "$MODE" = "nudge" ]; then
    echo "## === merge-snapshot holes (${#holes[@]}) ==="
    echo "Merged into $BASE $window_desc with NO row in $LEDGER_REL (ADR-0017)."
    echo "Still repairable inside the git-janitor backfill window ($repair_window): run"
    echo "\`bash agents/scripts/core/git-janitor.sh --post-merge <N>\` (Step 5.5 backfills),"
    echo "or the arming session's paste-ready merge-snapshot-append.sh line; then commit the row:"
    for h in "${holes[@]}"; do echo "  - $h"; done
    exit 0
fi
for h in "${holes[@]}"; do echo "merge-snapshot hole: $h — no ledger row (backfill window $repair_window)"; done
exit 0
