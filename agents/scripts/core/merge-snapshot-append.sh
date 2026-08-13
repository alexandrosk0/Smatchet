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
# Ledger line schema (one compact single-line JSON object per merge, schema 2):
#   {"pr":N,"mergeCommit":"<sha>","headSha":"<sha>","mergedAt":"<iso8601Z>",
#    "gates":"GATES_PASSED","redChecks":[...],"overrideLabels":[...],
#    "requiredContexts":[...],"mergeActor":"merge-watcher|orchestrator|git-janitor","schema":2}
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

    # requiredContexts — the branch-protection required set IN FORCE AT THIS
    # MERGE (undatable-required-context-never-fails-blocking, tooling P2). The
    # detector's effective-date heuristic infers promotion dates from the scan
    # window and cannot date a context that appears in NO rollup — which is
    # either a fresh promotion (benign) or a context that never reports (the
    # #1941 escape), indistinguishable from the window alone, so it degrades to
    # a WARN. Persisting the set at the decision instant turns "was this
    # required when this PR merged?" into a lookup and restores per-PR blocking
    # for snapshotted merges. Source: SNAPSHOT_REQUIRED_CONTEXTS (csv test
    # seam; may be set-but-empty) else project.config.json
    # § branch_protection.required_contexts — the file
    # setup-branch-protection.sh applies, so it IS the set in force. A config
    # read failure records [] (schema-1-equivalent: absent info, never a
    # guess), which the detector treats as "no merge-time set" fallback.
    local req_json
    if [ -n "${SNAPSHOT_REQUIRED_CONTEXTS+x}" ]; then
        req_json="$(csv_to_json_array "$SNAPSHOT_REQUIRED_CONTEXTS")" || return 1
    else
        req_json="$(jq -c '[.branch_protection.required_contexts[]?]' \
            "${MERGE_SNAPSHOT_CONFIG_FILE:-$_msa_root/project.config.json}" 2>/dev/null)" || req_json='[]'
        [ -n "$req_json" ] || req_json='[]'
    fi

    # Compose the compact single-line object with jq so every field (labels +
    # redChecks arrays, the ISO timestamp, the actor) encodes UTF-8-safely.
    # schema 2 = requiredContexts added (additive; schema-1 rows stay valid and
    # readers key on field presence, not the schema number).
    line="$(jq -cn \
        --argjson pr "$pr" \
        --arg mergeCommit "$merge_commit" \
        --arg headSha "$head_sha" \
        --arg mergedAt "$merged_at" \
        --arg gates "$gates" \
        --argjson redChecks "$red_json" \
        --argjson overrideLabels "$override_json" \
        --argjson requiredContexts "$req_json" \
        --arg mergeActor "$actor" \
        '{pr:$pr, mergeCommit:$mergeCommit, headSha:$headSha, mergedAt:$mergedAt, gates:$gates, redChecks:$redChecks, overrideLabels:$overrideLabels, requiredContexts:$requiredContexts, mergeActor:$mergeActor, schema:2}')" \
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
    if [ "$(jq -r '.schema' "$tmp")" != "2" ]; then echo "selftest FAIL: schema must be 2" >&2; fails=$((fails+1)); fi
    # requiredContexts self-derives from the config (or the env seam) and is
    # always an array — [] on a config-read failure, never absent on schema 2.
    if [ "$(jq -r '.requiredContexts | type' "$tmp")" != "array" ]; then echo "selftest FAIL: requiredContexts must be an array" >&2; fails=$((fails+1)); fi

    # 1b. The env seam pins the recorded set (and set-but-empty records []).
    local tmp2
    tmp2="$(mktemp)"
    MERGE_SNAPSHOT_LEDGER="$tmp2" SNAPSHOT_REQUIRED_CONTEXTS="A ctx, B ctx" \
        append_merge_snapshot 7 seam1 h1 GATES_PASSED "" "" orchestrator
    if [ "$(jq -r '.requiredContexts | join("|")' "$tmp2")" != "A ctx|B ctx" ]; then
        echo "selftest FAIL: SNAPSHOT_REQUIRED_CONTEXTS seam not recorded" >&2; fails=$((fails+1)); fi
    MERGE_SNAPSHOT_LEDGER="$tmp2" SNAPSHOT_REQUIRED_CONTEXTS="" \
        append_merge_snapshot 8 seam2 h2 GATES_PASSED "" "" orchestrator
    if [ "$(jq -rs '.[1].requiredContexts | length' "$tmp2")" != "0" ]; then
        echo "selftest FAIL: set-but-empty seam should record []" >&2; fails=$((fails+1)); fi
    rm -f "$tmp2"

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
    # selftest: asserts-failure — known-bad args must return non-zero (the failure path).
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
