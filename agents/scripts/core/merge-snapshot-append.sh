#!/usr/bin/env bash
# merge-snapshot-append.sh — append one merge-time gate-verdict snapshot to the
# committed JSONL ledger (docs/self-improvement/merge-snapshots.jsonl).
#
# WHY (see docs/adr/0017-merge-time-snapshot-ledger.md): the live
# `statusCheckRollup` the gate-escape detector (postmortem-owed.sh) reads is
# provably lossy — GitHub overwrites rollup contexts by name on re-run, and
# override labels are stripped post-merge (merge-gates.sh). The ONLY lossless
# capture of merge-decision truth is a snapshot written at the decision instant
# by the merge actor. This helper is the shared, idempotent writer used by all
# three merge actors (merge-watcher daemon, in-session orchestrator, git-janitor)
# so every `develop` merge records its gate verdict identically.
#
# Ledger line schema (one compact single-line JSON object per merge, schema 1):
#   {"pr":N,"mergeCommit":"<sha>","headSha":"<sha>","mergedAt":"<iso8601Z>",
#    "gates":"GATES_PASSED","redChecks":[...],"overrideLabels":[...],
#    "mergeActor":"merge-watcher|orchestrator|git-janitor","schema":1}
#
# Idempotency: a re-append for the same `pr`+`mergeCommit` is a no-op (grep-guard)
# so merge-path retries never double-write.
#
# Usage (function or CLI):
#   . agents/scripts/core/merge-snapshot-append.sh    # source: defines the fn
#   append_merge_snapshot <pr> <mergeCommit> <headSha> <gatesVerdict> \
#                         <redChecksCsv> <overrideLabelsCsv> <mergeActor>
#
#   bash agents/scripts/core/merge-snapshot-append.sh \
#        <pr> <mergeCommit> <headSha> <gatesVerdict> \
#        <redChecksCsv> <overrideLabelsCsv> <mergeActor>
#
#   bash agents/scripts/core/merge-snapshot-append.sh --selftest
#
# `mergedAt` is derived (UTC ISO-8601) when not pre-set via SNAPSHOT_MERGED_AT.
# Ledger path overridable via MERGE_SNAPSHOT_LEDGER (used by --selftest + tests).
#
# Passes test-shell-lint.sh (5-rule gate) + the shellcheck fail-set.

set -euo pipefail

# Repo-root-anchored default ledger (this script lives in agents/scripts/core/).
_msa_self="${BASH_SOURCE[0]:-$0}"
_msa_root="$(cd "$(dirname "$_msa_self")/../../.." && pwd)"
: "${MERGE_SNAPSHOT_LEDGER:=$_msa_root/docs/self-improvement/merge-snapshots.jsonl}"

# csv_to_json_array <csv> — turn "a,b , c" into a JSON array ["a","b","c"],
# trimming surrounding whitespace per element and dropping empties. Empty/blank
# input yields []. Uses jq so quoting/UTF-8 is encoded correctly.
csv_to_json_array() {
    local csv="${1:-}"
    if ! command -v jq >/dev/null 2>&1; then
        echo "merge-snapshot-append: jq not on PATH (required)" >&2
        return 1
    fi
    jq -cn --arg csv "$csv" \
        '($csv | split(",") | map(gsub("^\\s+|\\s+$";"")) | map(select(length>0)))'
}

# append_merge_snapshot <pr> <mergeCommit> <headSha> <gatesVerdict> \
#                       <redChecksCsv> <overrideLabelsCsv> <mergeActor>
# Composes ONE single-line JSON object and atomically `>>`-appends it, unless a
# line with the same "pr":N and "mergeCommit":"<sha>" already exists.
append_merge_snapshot() {
    if [ "$#" -ne 7 ]; then
        echo "append_merge_snapshot: expected 7 args, got $#" >&2
        echo "usage: append_merge_snapshot <pr> <mergeCommit> <headSha> <gatesVerdict> <redChecksCsv> <overrideLabelsCsv> <mergeActor>" >&2
        return 2
    fi
    local pr="$1" merge_commit="$2" head_sha="$3" gates="$4"
    local red_csv="$5" override_csv="$6" actor="$7"

    # Required-field validation — fail cleanly rather than write a garbage line.
    case "$pr" in
        ''|*[!0-9]*) echo "append_merge_snapshot: pr must be a positive integer (got '$pr')" >&2; return 2 ;;
    esac
    if [ -z "$merge_commit" ]; then
        echo "append_merge_snapshot: mergeCommit must be non-empty" >&2; return 2
    fi
    if [ -z "$head_sha" ]; then
        echo "append_merge_snapshot: headSha must be non-empty" >&2; return 2
    fi
    if [ -z "$gates" ]; then
        echo "append_merge_snapshot: gatesVerdict must be non-empty" >&2; return 2
    fi
    if [ -z "$actor" ]; then
        echo "append_merge_snapshot: mergeActor must be non-empty" >&2; return 2
    fi
    if ! command -v jq >/dev/null 2>&1; then
        echo "merge-snapshot-append: jq not on PATH (required)" >&2
        return 1
    fi

    # Idempotency guard — same pr+mergeCommit already recorded? No-op.
    if [ -f "$MERGE_SNAPSHOT_LEDGER" ] \
        && grep -qF "\"pr\":$pr," "$MERGE_SNAPSHOT_LEDGER" \
        && grep -qF "\"mergeCommit\":\"$merge_commit\"" "$MERGE_SNAPSHOT_LEDGER"; then
        # Both substrings present somewhere — confirm they co-occur on ONE line.
        if grep -qE "\"pr\":$pr,.*\"mergeCommit\":\"$merge_commit\"" "$MERGE_SNAPSHOT_LEDGER"; then
            return 0
        fi
    fi

    local merged_at red_json override_json line
    merged_at="${SNAPSHOT_MERGED_AT:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
    red_json="$(csv_to_json_array "$red_csv")" || return 1
    override_json="$(csv_to_json_array "$override_csv")" || return 1

    # Compose the compact single-line object with jq so every field (labels +
    # redChecks arrays, the ISO timestamp, the actor) encodes UTF-8-safely.
    line="$(jq -cn \
        --argjson pr "$pr" \
        --arg mergeCommit "$merge_commit" \
        --arg headSha "$head_sha" \
        --arg mergedAt "$merged_at" \
        --arg gates "$gates" \
        --argjson redChecks "$red_json" \
        --argjson overrideLabels "$override_json" \
        --arg mergeActor "$actor" \
        '{pr:$pr, mergeCommit:$mergeCommit, headSha:$headSha, mergedAt:$mergedAt, gates:$gates, redChecks:$redChecks, overrideLabels:$overrideLabels, mergeActor:$mergeActor, schema:1}')" \
        || { echo "merge-snapshot-append: jq failed to compose snapshot line" >&2; return 1; }

    mkdir -p "$(dirname "$MERGE_SNAPSHOT_LEDGER")"
    # Single-line atomic append — line-granular `>>` is atomic for a compact
    # one-line record, so parallel-merge appends don't interleave.
    printf '%s\n' "$line" >> "$MERGE_SNAPSHOT_LEDGER"
}

# --selftest — write to a temp ledger and assert the contract.
run_selftest() {
    if ! command -v jq >/dev/null 2>&1; then
        echo "selftest: SKIP — jq not on PATH (required for compose)" >&2
        return 0
    fi
    local tmp rc
    tmp="$(mktemp)"
    export MERGE_SNAPSHOT_LEDGER="$tmp"
    local fails=0

    # 1. Append works + line is valid JSON with the expected fields.
    append_merge_snapshot 42 abc123 def456 GATES_PASSED "" "cr-out-of-band,perf-out-of-band" orchestrator
    local n
    n="$(wc -l < "$tmp")"
    if [ "$n" -ne 1 ]; then echo "selftest FAIL: expected 1 line, got $n" >&2; fails=$((fails+1)); fi
    if ! jq -e . "$tmp" >/dev/null 2>&1; then echo "selftest FAIL: ledger line is not valid JSON" >&2; fails=$((fails+1)); fi
    if [ "$(jq -r '.pr' "$tmp")" != "42" ]; then echo "selftest FAIL: pr field wrong" >&2; fails=$((fails+1)); fi
    if [ "$(jq -r '.overrideLabels | length' "$tmp")" != "2" ]; then echo "selftest FAIL: overrideLabels not a 2-elem array" >&2; fails=$((fails+1)); fi
    if [ "$(jq -r '.redChecks | length' "$tmp")" != "0" ]; then echo "selftest FAIL: redChecks should be empty array" >&2; fails=$((fails+1)); fi
    if [ "$(jq -r '.schema' "$tmp")" != "1" ]; then echo "selftest FAIL: schema must be 1" >&2; fails=$((fails+1)); fi

    # 2. Idempotent re-append (same pr+mergeCommit) is a no-op.
    append_merge_snapshot 42 abc123 def456 GATES_PASSED "" "cr-out-of-band,perf-out-of-band" orchestrator
    n="$(wc -l < "$tmp")"
    if [ "$n" -ne 1 ]; then echo "selftest FAIL: idempotent re-append wrote a 2nd line (got $n)" >&2; fails=$((fails+1)); fi

    # 3. A different mergeCommit for the same PR DOES append (distinct merge).
    append_merge_snapshot 42 zzz999 def456 GATES_PASSED "MSVC /WX" "" git-janitor
    n="$(wc -l < "$tmp")"
    if [ "$n" -ne 2 ]; then echo "selftest FAIL: distinct mergeCommit should append (got $n lines)" >&2; fails=$((fails+1)); fi
    if [ "$(jq -rs '.[1].redChecks | length' "$tmp")" != "1" ]; then echo "selftest FAIL: redChecks csv->array wrong on line 2" >&2; fails=$((fails+1)); fi

    # 4. Missing/malformed args fail cleanly (non-zero, no line written).
    rc=0; append_merge_snapshot 1 2 3 >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then echo "selftest FAIL: too-few-args should fail" >&2; fails=$((fails+1)); fi
    rc=0; append_merge_snapshot abc m h GATES_PASSED "" "" orchestrator >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then echo "selftest FAIL: non-numeric pr should fail" >&2; fails=$((fails+1)); fi
    rc=0; append_merge_snapshot 7 "" h GATES_PASSED "" "" orchestrator >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then echo "selftest FAIL: empty mergeCommit should fail" >&2; fails=$((fails+1)); fi
    # The failing calls must NOT have grown the ledger.
    n="$(wc -l < "$tmp")"
    if [ "$n" -ne 2 ]; then echo "selftest FAIL: malformed args wrote a line (ledger now $n)" >&2; fails=$((fails+1)); fi

    rm -f "$tmp"
    if [ "$fails" -eq 0 ]; then
        echo "merge-snapshot-append --selftest: PASS"
        return 0
    fi
    echo "merge-snapshot-append --selftest: FAIL ($fails assertion(s))" >&2
    return 1
}

# Dispatch only when executed directly (sourcing just defines the functions).
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    case "${1:-}" in
        --selftest)
            run_selftest
            ;;
        ''|-h|--help)
            echo "usage: merge-snapshot-append.sh <pr> <mergeCommit> <headSha> <gatesVerdict> <redChecksCsv> <overrideLabelsCsv> <mergeActor>" >&2
            echo "       merge-snapshot-append.sh --selftest" >&2
            [ "${1:-}" = "" ] && exit 2 || exit 0
            ;;
        *)
            append_merge_snapshot "$@"
            ;;
    esac
fi
