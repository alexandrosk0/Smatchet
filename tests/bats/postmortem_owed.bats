#!/usr/bin/env bats
# tests/bats/postmortem_owed.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/postmortem-owed.sh — the gate-escape
# detector. Covers the three signal-cleanup fixes (backlog tooling.md):
#   1. postmortem-owed-overreports-nonblocking-and-cancelled-twins (P1) — the
#      curated rollup: latest-run-per-context dedupe + terminal-verdict +
#      blocking-scope, so CANCELLED twins / advisory lanes / in-progress checks
#      no longer over-report. A CANCELLED that IS the latest run for a blocking
#      context (no later SUCCESS) owes (perf-pr-fast cancelled-escape, #1566).
#   2. postmortem-owed-moot-override-false-positive (P2) — a non-load-bearing
#      override (its gate passed anyway) owes no postmortem.
#   3. postmortem-owed-direct-push-blindspot (P2) — trigger 4 catches PR-less
#      direct pushes to develop (commits/{sha}/pulls == 0).
#
# Stubs `gh` and `git` on PATH; drives everything through env seams the script
# exposes (POSTMORTEM_LEDGER / POSTMORTEM_REQUIRED_CONTEXTS /
# POSTMORTEM_OVERRIDE_LABELS / SNAPSHOT_LEDGER / POSTMORTEM_DIRECTPUSH_*).
# Real `jq` is used (same dependency as the script under test).
#
# Requires: bash, jq (on PATH), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/postmortem-owed.sh"
    export SCRIPT
    REAL_GIT="$(command -v git)"
    export REAL_GIT

    # Per-test data dir (fixtures the stubs read).
    PM_DATA="$(mktemp -d)"
    export PM_DATA

    # --- script env seams ---------------------------------------------------
    export REPO="test/repo"
    export POSTMORTEM_LEDGER="$PM_DATA/ledger.md"
    : > "$POSTMORTEM_LEDGER"                       # empty → has_entry always false
    export SNAPSHOT_LEDGER="$PM_DATA/snapshots.jsonl"
    : > "$SNAPSHOT_LEDGER"                         # empty → live fallback path
    export POSTMORTEM_REQUIRED_CONTEXTS="Test-delta gate,Windows + MSVC,Perf PR-fast (windows-2022)"
    export POSTMORTEM_OVERRIDE_LABELS="tests-out-of-band perf-out-of-band cr-out-of-band coverage-out-of-band intent-out-of-band"
    export POSTMORTEM_DIRECTPUSH_SINCE="7 days ago"

    # --- fixture defaults (tests overwrite as needed) -----------------------
    export PM_PRLIST_FIXTURE="$PM_DATA/prlist.json"
    echo '[]' > "$PM_PRLIST_FIXTURE"              # no merged PRs by default
    : > "$PM_DATA/gitlog_revert.txt"              # no reverts
    : > "$PM_DATA/gitlog_directpush.txt"          # no commits

    # --- stub gh ------------------------------------------------------------
    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    cat > "$STUB_BIN_DIR/gh" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
    auth) exit 0 ;;
    pr)
        case "${2:-}" in
            list)
                f=""; prev=""
                for a in "$@"; do [ "$prev" = "--jq" ] && { f="$a"; break; }; prev="$a"; done
                if [ -n "$f" ]; then jq -r "$f" "$PM_PRLIST_FIXTURE"; else cat "$PM_PRLIST_FIXTURE"; fi
                exit 0 ;;
            view)
                pr="$3"; fields=""; jqf=""; prev=""
                for a in "$@"; do
                    case "$prev" in --json) fields="$a" ;; --jq) jqf="$a" ;; esac
                    prev="$a"
                done
                case "$fields" in
                    files)             src="$PM_DATA/files_${pr}.json" ;;
                    statusCheckRollup) src="$PM_DATA/rollup_${pr}.json" ;;
                    *)                 src="" ;;
                esac
                if [ -z "$src" ] || [ ! -f "$src" ]; then
                    case "$fields" in
                        files)             src="$PM_DATA/_empty_files.json"; echo '{"files":[]}' > "$src" ;;
                        statusCheckRollup) src="$PM_DATA/_empty_rollup.json"; echo '{"statusCheckRollup":[]}' > "$src" ;;
                        *)                 echo "{}" ; exit 0 ;;
                    esac
                fi
                if [ -n "$jqf" ]; then jq -r "$jqf" "$src"; else cat "$src"; fi
                exit 0 ;;
        esac ;;
    api)
        path="$2"
        sha="${path#*/commits/}"; sha="${sha%/pulls}"
        cntfile="$PM_DATA/pulls_${sha}.txt"
        if [ -f "$cntfile" ]; then cat "$cntfile"; else echo 1; fi
        exit 0 ;;
esac
echo "stub-gh: unhandled args: $*" >&2
exit 99
STUB
    chmod +x "$STUB_BIN_DIR/gh"

    # --- stub git (intercept `git log` + the origin/develop ledger `show`;
    #     pass everything else through) ---
    cat > "$STUB_BIN_DIR/git" <<'STUB'
#!/usr/bin/env bash
if [ "${1:-}" = "log" ]; then
    case " $* " in
        *--grep*)      cat "$PM_DATA/gitlog_revert.txt" 2>/dev/null || true; exit 0 ;;
        *--no-merges*) cat "$PM_DATA/gitlog_directpush.txt" 2>/dev/null || true; exit 0 ;;
    esac
    exit 0
fi
if [ "${1:-}" = "show" ]; then
    case "${2:-}" in
        origin/develop:*)
            if [ -f "$PM_DATA/develop_ledger.md" ]; then cat "$PM_DATA/develop_ledger.md"; exit 0; fi
            exit 128 ;;
    esac
fi
exec "$REAL_GIT" "$@"
STUB
    chmod +x "$STUB_BIN_DIR/git"

    PATH="$STUB_BIN_DIR:$PATH"
    export PATH
}

teardown() {
    [ -n "${PM_DATA:-}" ] && rm -rf "$PM_DATA"
    [ -n "${STUB_BIN_DIR:-}" ] && rm -rf "$STUB_BIN_DIR"
}

# Write the gh-pr-list fixture from stdin (a JSON array).
prlist() { cat > "$PM_PRLIST_FIXTURE"; }

run_detector() { run bash "$SCRIPT" --list; }

# ============================================================================
# Trigger 1 — curated rollup (item 1)
# ============================================================================

@test "clean merge (all SUCCESS) owes nothing" {
    prlist <<'JSON'
[{"number":2001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" == *"no gate escapes owed"* ]]
    [[ "$output" != *"PR #2001"* ]]
}

@test "CANCELLED concurrency twin beside a later SUCCESS owes nothing" {
    prlist <<'JSON'
[{"number":2002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"CANCELLED","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:10Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #2002"* ]]
}

@test "latest-run CANCELLED (no SUCCESS twin) on a required context owes a postmortem" {
    prlist <<'JSON'
[{"number":2003,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a3"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"CANCELLED","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #2003 — red-check: Windows + MSVC"* ]]
}

@test "latest-run CANCELLED on an allow-list non-required check owes a postmortem (#1566 shape)" {
    prlist <<'JSON'
[{"number":2013,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a13"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Fuzz smoke (windows-2022)","status":"COMPLETED","conclusion":"CANCELLED","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #2013 — red-check: Fuzz smoke (windows-2022)"* ]]
}

@test "advisory non-allowlist red owes nothing" {
    prlist <<'JSON'
[{"number":2004,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a4"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Mobile — Android NDK arm64-v8a (advisory)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #2004"* ]]
}

@test "IN_PROGRESS (non-terminal) required check owes nothing" {
    prlist <<'JSON'
[{"number":2005,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a5"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"IN_PROGRESS","conclusion":null,"startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #2005"* ]]
}

@test "required-context terminal FAILURE owes a postmortem" {
    prlist <<'JSON'
[{"number":2006,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a6"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #2006 — red-check: Windows + MSVC"* ]]
}

@test "allow-list non-required (Coverage) terminal FAILURE owes a postmortem" {
    prlist <<'JSON'
[{"number":2007,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a7"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Coverage (windows-2022 + OpenCppCoverage)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #2007 — red-check: Coverage"* ]]
}

@test "ledger entry suppresses an otherwise-owed escape (dedup)" {
    echo "## RCA for PR #2006 — Windows + MSVC red" > "$POSTMORTEM_LEDGER"
    prlist <<'JSON'
[{"number":2006,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a6"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #2006"* ]]
}

@test "slash-joined combined-PR ledger heading dedups a trailing PR (#784)" {
    # One RCA covering several PRs written `PR #906/#907/#908` — the middle/trailing
    # PRs are preceded by `/`, not a comma/space. has_entry must dedup them; they
    # used to re-flag every SessionStart (the `/`-less separator class, #784).
    echo "## RCA for PR #906/#907/#908 — Windows + MSVC red" > "$POSTMORTEM_LEDGER"
    prlist <<'JSON'
[{"number":907,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b9"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #907"* ]]
}

# --- sourced-scope proof (de-dup of the blocking-scope constant) -------------
# The blocking-scope regex is SOURCED from merge-gates.sh's
# MERGE_GATES_BLOCK_ALLOWLIST_RE rather than hand-copied (#1258 drift guard).
# Under block-on-any-red the sourced scope covers every non-advisory-named
# check, so a merged-red Bucket-E now OWES; an advisory-NAMED red does not.

@test "merged-red non-required (Bucket-E UI tests) OWES a postmortem (block-on-any-red)" {
    # Was the #1258 "owes nothing" guard while Bucket-* sat off the curated
    # list. The sourced scope now blocks every non-advisory-named check, so a
    # PR merged past a red Bucket-E is a genuine gate-escape → owed. The
    # sourced-constant lock-step (the actual #1258 lesson) is still proven:
    # this expectation flipped BECAUSE the sourced constant flipped, with no
    # hand-synced copy to drift.
    prlist <<'JSON'
[{"number":2008,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a8"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Bucket-E UI tests (Mesa headless GL)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #2008"* ]]
}

@test "merged-red advisory-NAMED check owes nothing (the block-on-any-red escape)" {
    # The one non-gating shape left: a check whose NAME carries the "advisory"
    # token. A red on it is not a gate-escape (it never gated).
    prlist <<'JSON'
[{"number":2009,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"a9"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Duplication scanner (advisory)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #2009"* ]]
}

@test "allow-list is sourced from merge-gates.sh, not hand-duplicated (drift guard)" {
    # Guards the exact duplication that drifted and caused #1258: there must be a
    # `source merge-gates.sh`, and the ONLY ALLOW_LIST_RE assignment must read the
    # sourced constant (never re-hardcode the regex literal).
    run grep -cE 'source .*merge-gates\.sh' "$SCRIPT"
    [ "$status" -eq 0 ]
    run grep -nE 'ALLOW_LIST_RE=' "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *'ALLOW_LIST_RE="$MERGE_GATES_BLOCK_ALLOWLIST_RE"'* ]]
    [[ "$output" != *"Coverage"* ]]
}

# ============================================================================
# Trigger 2 — moot vs load-bearing override (item 2)
# ============================================================================

@test "moot tests-out-of-band (Test-delta gate SUCCESS + test delta) owes nothing" {
    prlist <<'JSON'
[{"number":3001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b1"},"labels":[{"name":"tests-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_3001.json"
    echo '{"files":[{"path":"tests/Core/Foo.test.cpp"}]}' > "$PM_DATA/files_3001.json"
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #3001"* ]]
}

@test "load-bearing tests-out-of-band (Test-delta gate FAILURE) owes a postmortem" {
    prlist <<'JSON'
[{"number":3002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b2"},"labels":[{"name":"tests-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_3002.json"
    echo '{"files":[{"path":"Source/Core/src/Tracker/X.cpp"}]}' > "$PM_DATA/files_3002.json"
    run_detector
    [[ "$output" == *"PR #3002"* ]]
    [[ "$output" == *"override: tests-out-of-band"* || "$output" == *"red-check: Test-delta gate"* ]]
}

@test "tests-out-of-band with Test-delta SUCCESS but NO test delta is load-bearing (owes)" {
    prlist <<'JSON'
[{"number":3003,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b3"},"labels":[{"name":"tests-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_3003.json"
    echo '{"files":[{"path":"Source/Core/src/Tracker/X.cpp"}]}' > "$PM_DATA/files_3003.json"
    run_detector
    [[ "$output" == *"PR #3003 — override: tests-out-of-band"* ]]
}

@test "moot perf-out-of-band (Perf PR-fast SUCCESS) owes nothing" {
    prlist <<'JSON'
[{"number":3004,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b4"},"labels":[{"name":"perf-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_3004.json"
    run_detector
    [[ "$output" != *"PR #3004"* ]]
}

@test "moot intent-out-of-band (Intent section SUCCESS) owes nothing" {
    prlist <<'JSON'
[{"number":3010,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"bi1"},"labels":[{"name":"intent-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Intent section","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Intent section","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_3010.json"
    run_detector
    [[ "$output" != *"PR #3010"* ]]
}

@test "load-bearing intent-out-of-band (Intent section FAILURE) owes a postmortem" {
    prlist <<'JSON'
[{"number":3011,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"bi2"},"labels":[{"name":"intent-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Intent section","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Intent section","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_3011.json"
    run_detector
    [[ "$output" == *"PR #3011"* ]]
    [[ "$output" == *"override: intent-out-of-band"* || "$output" == *"red-check: Intent section"* ]]
}

# ============================================================================
# Trigger 4 — PR-less direct push (item 3)
# ============================================================================

@test "direct push (no PR backing) owes a postmortem" {
    printf 'deadbee\tfeat: sneaky direct push\n' > "$PM_DATA/gitlog_directpush.txt"
    echo 0 > "$PM_DATA/pulls_deadbee.txt"
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" == *"commit deadbee — direct push (no PR/CI/CR)"* ]]
}

@test "commit with (#N) suffix is a squash merge, not a direct push" {
    printf 'cafe123\tfeat: normal squash merge (#1234)\n' > "$PM_DATA/gitlog_directpush.txt"
    echo 0 > "$PM_DATA/pulls_cafe123.txt"
    run_detector
    [[ "$output" != *"cafe123"* ]]
}

@test "no-(#N) commit that IS PR-backed (pulls>0) owes nothing" {
    printf 'feedfac\tdocs: archive plan\n' > "$PM_DATA/gitlog_directpush.txt"
    echo 1 > "$PM_DATA/pulls_feedfac.txt"
    run_detector
    [[ "$output" != *"feedfac"* ]]
}

@test "no-(#N) commit whose sha is a known mergeCommit skips the gh call (PR-backed)" {
    # Production shape: gh pr list gives a FULL 40-char oid; git log gives a SHORT
    # %h prefix. The shortcut must match by prefix.
    prlist <<'JSON'
[{"number":4001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"beaded1f0123456789abcdef0123456789abcdef0"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    printf 'beaded1f\tchore: squash with non-standard subject\n' > "$PM_DATA/gitlog_directpush.txt"
    echo 0 > "$PM_DATA/pulls_beaded1f.txt"   # would flag if the merged-commit prefix shortcut failed
    run_detector
    [[ "$output" != *"beaded1f"* ]]
}

@test "direct push already in the ledger is deduped" {
    echo "## RCA for commit deadbee — direct push" > "$POSTMORTEM_LEDGER"
    printf 'deadbee\tfeat: sneaky direct push\n' > "$PM_DATA/gitlog_directpush.txt"
    echo 0 > "$PM_DATA/pulls_deadbee.txt"
    run_detector
    [[ "$output" != *"deadbee"* ]]
}

@test "direct push deduped by free-form sha mention (backtick / 'direct push <sha>')" {
    # Real ledger phrasing never says "commit <sha>" — it backticks the sha or
    # writes "PR-less direct push <sha>". has_sha_entry must still dedup it.
    printf '## 2026-06-07 · PR-less direct push 93c63d0f · model change\nCommit `93c63d0f` landed on develop.\n' > "$POSTMORTEM_LEDGER"
    printf '93c63d0f\tchore: code-review model change\n' > "$PM_DATA/gitlog_directpush.txt"
    echo 0 > "$PM_DATA/pulls_93c63d0f.txt"
    run_detector
    [[ "$output" != *"93c63d0f"* ]]
}

# ============================================================================
# Snapshot-ledger path (PRIMARY) still authoritative with the new filters
# ============================================================================

@test "snapshot ledger redChecks drives the trigger when present" {
    prlist <<'JSON'
[{"number":5001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"pr":5001,"mergeCommit":"c1","redChecks":["Windows + MSVC"],"overrideLabels":[]}' > "$SNAPSHOT_LEDGER"
    run_detector
    [[ "$output" == *"PR #5001"* ]]
    [[ "$output" == *"red-check: Windows + MSVC"* ]]
}

@test "snapshot override is NOT moot-filtered even when live rollup is green (HIGH fix)" {
    # The watcher writes redChecks=[] for override merges, so a load-bearing
    # override that healed green post-merge must NOT be re-judged moot against the
    # lossy live rollup. Snapshot override-only line + green live rollup → FLAG.
    prlist <<'JSON'
[{"number":5002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c2"},"labels":[{"name":"perf-out-of-band"}],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"pr":5002,"mergeCommit":"c2","redChecks":[],"overrideLabels":["perf-out-of-band"]}' > "$SNAPSHOT_LEDGER"
    # Live rollup says Perf is SUCCESS (healed) — would WRONGLY suppress if the
    # snapshot path moot-filtered.
    echo '{"statusCheckRollup":[{"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}' > "$PM_DATA/rollup_5002.json"
    run_detector
    [[ "$output" == *"PR #5002"* ]]
    [[ "$output" == *"override: perf-out-of-band"* ]]
}

# ============================================================================
# PR-3 — bucket-lane-status-broken-sentinel-auditable (broken-lane → WARN)
# ============================================================================

@test "broken-lane: a block-scope red whose name is in POSTMORTEM_BROKEN_LANES is an auditable WARN, not an owed escape" {
    # Coverage is allow-listed (would normally OWE on a terminal FAILURE — see the
    # 'Coverage terminal FAILURE owes' case above). Register it as a known broken
    # lane → its RED downgrades to a WARN (stderr) and owes NO postmortem.
    export POSTMORTEM_BROKEN_LANES="Coverage (windows-2022 + OpenCppCoverage)"
    prlist <<'JSON'
[{"number":6001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Coverage (windows-2022 + OpenCppCoverage)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    # Auditable WARN present (never silently dropped); NOT an owed escape.
    [[ "$output" == *"WARN — PR #6001 — broken-lane"* ]]
    [[ "$output" != *"postmortem owed: PR #6001"* ]]
}

@test "broken-lane downgrade does NOT apply when a real red rides alongside the broken lane" {
    # Coverage (broken) + Windows + MSVC (genuine required red) → the real red
    # still owes; the broken lane must not launder it.
    export POSTMORTEM_BROKEN_LANES="Coverage (windows-2022 + OpenCppCoverage)"
    prlist <<'JSON'
[{"number":6002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Coverage (windows-2022 + OpenCppCoverage)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"postmortem owed: PR #6002"* ]]
    [[ "$output" == *"Windows + MSVC"* ]]
}

@test "broken-lane registry empty (production default): a broken lane's red still owes (no silent laundering)" {
    # No POSTMORTEM_BROKEN_LANES set → is_broken_lane never downgrades; Coverage red owes.
    prlist <<'JSON'
[{"number":6003,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d3"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Coverage (windows-2022 + OpenCppCoverage)","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"postmortem owed: PR #6003"* ]]
}

# ============================================================================
# PR-3 — merge-gate-absence-blind-nonrequired-allowlist (present-assertion)
# ============================================================================

@test "absence-present: an expected-present allow-listed check ABSENT from the rollup owes a postmortem (fail-closed)" {
    # Coverage is expected-present; the PR's rollup carries only a green Windows +
    # MSVC (Coverage absent — the PR self-disabled the gate). Flag it.
    export POSTMORTEM_EXPECTED_PRESENT="Coverage (windows-2022 + OpenCppCoverage)"
    prlist <<'JSON'
[{"number":7001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"e1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #7001"* ]]
    [[ "$output" == *"absent-allowlisted: Coverage"* ]]
}

@test "absence-present: an expected-present check that IS in the rollup owes nothing" {
    export POSTMORTEM_EXPECTED_PRESENT="Coverage (windows-2022 + OpenCppCoverage)"
    prlist <<'JSON'
[{"number":7002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"e2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Coverage (windows-2022 + OpenCppCoverage)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #7002"* ]]
}

@test "absence-present: empty expected-present set (production default) is inert" {
    # POSTMORTEM_EXPECTED_PRESENT unset → no absence flagging even with a sparse rollup.
    prlist <<'JSON'
[{"number":7003,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"e3"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #7003"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

# ============================================================================
# Ledger pinned to origin/develop (tooling 2026-06-19 — phantom owes /
# false suppression). POSTMORTEM_LEDGER unset → has_entry reads the ref the
# merge scans trust, not the cwd working-tree file.
# ============================================================================

@test "entry on origin/develop dedupes even when the working tree lacks it (no phantom owe)" {
    unset POSTMORTEM_LEDGER
    # origin/develop ledger (git-show stub) HAS the postmortem for #8001.
    echo "## 2026-06-19 · PR #8001 — gate escape RCA" > "$PM_DATA/develop_ledger.md"
    prlist <<'JSON'
[{"number":8001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #8001"* ]]
}

@test "entry only in the working tree does NOT suppress an owed postmortem (false-negative guard)" {
    unset POSTMORTEM_LEDGER
    # Fixture repo whose WORKING-TREE ledger carries the entry while the
    # origin/develop ledger (git-show stub) is empty — the pre-fix code read
    # the working file and silently suppressed the owe.
    : > "$PM_DATA/develop_ledger.md"
    FIX_REPO="$PM_DATA/repo"
    mkdir -p "$FIX_REPO/agents/scripts/core" "$FIX_REPO/docs/self-improvement"
    cp "$REPO_ROOT/agents/scripts/core/postmortem-owed.sh" \
       "$REPO_ROOT/agents/scripts/core/merge-gates.sh" "$FIX_REPO/agents/scripts/core/"
    if [ -f "$REPO_ROOT/agents/scripts/core/merge-gates-prompt.sh" ]; then
        cp "$REPO_ROOT/agents/scripts/core/merge-gates-prompt.sh" "$FIX_REPO/agents/scripts/core/"
    fi
    echo "## 2026-06-19 · PR #8002 — local-only RCA draft" \
        > "$FIX_REPO/docs/self-improvement/postmortems.md"
    prlist <<'JSON'
[{"number":8002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f2"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run bash "$FIX_REPO/agents/scripts/core/postmortem-owed.sh" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #8002"* ]]
}
