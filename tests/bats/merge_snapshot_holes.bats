#!/usr/bin/env bats
# tests/bats/merge_snapshot_holes.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/merge-snapshot-holes.sh — the SessionStart
# detector for merges with no merge-snapshot ledger row while git-janitor can
# still backfill them (process/2026-08-18-merge-snapshot-ledger-28-pr-hole).
#
# Stubs `gh` on PATH: `gh api repos/<r>/pulls?...&page=N` prints
# $HOLES_DATA/page_N.json (else []), and every call is logged to
# $HOLES_DATA/gh.log. The ledger is a fixture file via MERGE_SNAPSHOT_LEDGER
# (the appender's own seam), and "now" is pinned with
# MERGE_SNAPSHOT_HOLES_NOW_EPOCH = 2026-10-04T12:00:00Z.
#
# Requires: bash, jq (on PATH), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/merge-snapshot-holes.sh"
    export SCRIPT

    HOLES_DATA="$(mktemp -d)"
    export HOLES_DATA
    export REPO="test/repo"
    export MERGE_SNAPSHOT_LEDGER="$HOLES_DATA/ledger.jsonl"
    : > "$MERGE_SNAPSHOT_LEDGER"
    export MERGE_SNAPSHOT_HOLES_NOW_EPOCH=1791115200   # 2026-10-04T12:00:00Z
    unset SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS MERGE_SNAPSHOT_HOLES_BASE
    unset MERGE_SNAPSHOT_HOLES_PER_PAGE MERGE_SNAPSHOT_HOLES_MAX_PAGES

    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    cat > "$STUB_BIN_DIR/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$HOLES_DATA/gh.log"
case "$1" in
    auth) exit 0 ;;
    api)
        if [ -n "${STUB_GH_FAIL:-}" ]; then
            echo "HTTP 504: 504 Gateway Timeout (https://api.github.com/repos/x)" >&2
            exit 1
        fi
        page="${2##*page=}"; page="${page%%&*}"
        if [ -f "$HOLES_DATA/page_${page}.json" ]; then cat "$HOLES_DATA/page_${page}.json"; else echo '[]'; fi
        exit 0 ;;
esac
echo "stub-gh: unhandled args: $*" >&2
exit 99
STUB
    chmod +x "$STUB_BIN_DIR/gh"
    PATH="$STUB_BIN_DIR:$PATH"
    export PATH
}

teardown() {
    rm -rf "$HOLES_DATA" "$STUB_BIN_DIR"
}

# page <n> — write the page-N fixture from stdin (a JSON array of pulls).
page() { cat > "$HOLES_DATA/page_$1.json"; }

@test "--selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge-snapshot-holes --selftest: PASS"* ]]
}

@test "an in-window merge with no ledger row is a hole; one with a row is not" {
    page 1 <<'JSON'
[{"number":2301,"merged_at":"2026-10-04T11:30:00Z","updated_at":"2026-10-04T11:30:05Z"},
 {"number":2300,"merged_at":"2026-10-04T09:00:00Z","updated_at":"2026-10-04T09:00:05Z"}]
JSON
    echo '{"pr":2300,"mergeCommit":"m2300","gates":"GATES_PASSED","mergeActor":"orchestrator-automerge","schema":2}' > "$MERGE_SNAPSHOT_LEDGER"
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge-snapshot hole: PR #2301 merged 2026-10-04T11:30:00Z — no ledger row (backfill window 6h)"* ]]
    [[ "$output" != *"PR #2300"* ]]
}

@test "a merge older than the backfill window is not reported (permanent hole, not repairable)" {
    page 1 <<'JSON'
[{"number":2290,"merged_at":"2026-10-04T05:59:59Z","updated_at":"2026-10-04T06:10:00Z"}]
JSON
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #2290"* ]]
    [[ "$output" == *"no ledger holes among the 0 PR(s) merged into develop in the last 6h"* ]]
}

@test "SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS (the janitor's own knob) sets the window" {
    page 1 <<'JSON'
[{"number":2280,"merged_at":"2026-10-04T03:00:00Z","updated_at":"2026-10-04T03:00:05Z"}]
JSON
    export SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS=12
    run bash "$SCRIPT" --list
    [[ "$output" == *"merge-snapshot hole: PR #2280"* ]]
    [[ "$output" == *"(backfill window 12h)"* ]]
    export SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS=2
    run bash "$SCRIPT" --list
    [[ "$output" != *"PR #2280"* ]]
}

@test "a closed-unmerged PR (merged_at null) is never a hole" {
    page 1 <<'JSON'
[{"number":2270,"merged_at":null,"updated_at":"2026-10-04T11:00:00Z"}]
JSON
    run bash "$SCRIPT" --list
    [[ "$output" != *"PR #2270"* ]]
    [[ "$output" == *"no ledger holes"* ]]
}

@test "--nudge is silent when nothing is owed" {
    page 1 <<'JSON'
[{"number":2260,"merged_at":"2026-10-04T11:00:00Z","updated_at":"2026-10-04T11:00:05Z"}]
JSON
    echo '{"pr":2260,"mergeCommit":"m"}' > "$MERGE_SNAPSHOT_LEDGER"
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--nudge prints a SessionStart block naming the hole and the repair" {
    page 1 <<'JSON'
[{"number":2250,"merged_at":"2026-10-04T11:00:00Z","updated_at":"2026-10-04T11:00:05Z"}]
JSON
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"## === merge-snapshot holes (1) ==="* ]]
    [[ "$output" == *"  - PR #2250 merged 2026-10-04T11:00:00Z"* ]]
    [[ "$output" == *"git-janitor.sh --post-merge <N>"* ]]
}

@test "a failed fetch says window NOT scanned (stderr) and never prints the clean line" {
    export STUB_GH_FAIL=1
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"merged-PR fetch failed (HTTP 504: 504 Gateway Timeout"*"window NOT scanned"* ]]
    [[ "$output" != *"no ledger holes"* ]]
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"NOT scanned"* ]]
    [[ "$output" != *"=== merge-snapshot holes"* ]]
}

@test "queries the REST pulls endpoint for the configured base branch (no GraphQL)" {
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    grep -q '^api repos/test/repo/pulls?state=closed&base=develop&sort=updated&direction=desc&per_page=100&page=1$' "$HOLES_DATA/gh.log"
    ! grep -q 'graphql' "$HOLES_DATA/gh.log"
    export MERGE_SNAPSHOT_HOLES_BASE=main
    run bash "$SCRIPT" --list
    grep -q 'base=main&' "$HOLES_DATA/gh.log"
}

@test "pages until a page predates the window, bounded by MAX_PAGES" {
    export MERGE_SNAPSHOT_HOLES_PER_PAGE=2
    page 1 <<'JSON'
[{"number":2244,"merged_at":"2026-10-04T11:00:00Z","updated_at":"2026-10-04T11:00:05Z"},
 {"number":2243,"merged_at":null,"updated_at":"2026-10-04T10:00:00Z"}]
JSON
    page 2 <<'JSON'
[{"number":2242,"merged_at":"2026-10-04T08:00:00Z","updated_at":"2026-10-04T08:00:05Z"},
 {"number":2241,"merged_at":"2026-10-03T08:00:00Z","updated_at":"2026-10-03T08:00:05Z"}]
JSON
    page 3 <<'JSON'
[{"number":2240,"merged_at":"2026-10-04T10:30:00Z","updated_at":"2026-10-02T00:00:00Z"}]
JSON
    run bash "$SCRIPT" --list
    [[ "$output" == *"PR #2244"* ]]
    [[ "$output" == *"PR #2242"* ]]
    [[ "$output" != *"PR #2241"* ]]
    # page 2's oldest updated_at predates the cutoff → page 3 is never fetched.
    ! grep -q 'page=3' "$HOLES_DATA/gh.log"
    # and MAX_PAGES=1 stops after the first page.
    : > "$HOLES_DATA/gh.log"
    export MERGE_SNAPSHOT_HOLES_MAX_PAGES=1
    run bash "$SCRIPT" --list
    ! grep -q 'page=2' "$HOLES_DATA/gh.log"
    [[ "$output" != *"PR #2242"* ]]
}

@test "unresolvable repo / gh missing degrades to an advisory skip (exit 0)" {
    unset REPO
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"skipped (advisory)"* ]]
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the SessionStart template wires --nudge" {
    # The template launches SessionStart scripts through layer-run.sh: `layer-run.sh" agents/scripts/core/<script> --nudge`.
    grep -q 'agents/scripts/core/merge-snapshot-holes.sh --nudge' "$REPO_ROOT/docs/harness/claude-code/settings.json.tmpl"
}
