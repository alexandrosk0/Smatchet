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
    # Required-ABSENT detector OFF by default in this suite. Every fixture below is
    # a MINIMAL synthetic rollup carrying only the one or two contexts its own test
    # is about — none of them model the full POSTMORTEM_REQUIRED_CONTEXTS set above,
    # so the absence detector would (correctly, per production semantics) fire on
    # all of them and drown the signal each test is actually asserting. An absurd
    # grace makes every fixture "still inside the run-creation lag window" and so
    # not judged for absence. This is a TEST-fixture accommodation, NOT a production
    # default (production default is 3600s, exercised live) — the § required-absent
    # tests below opt back in with POSTMORTEM_ABSENT_GRACE_SECONDS=0.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=99999999999

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
    repo)
        # `gh repo view --json nameWithOwner --jq .nameWithOwner` (resolve-repo.sh).
        # Emit $STUB_REPO_SLUG when set; otherwise fail (unresolvable) so the
        # advisory degrade path can be exercised with REPO unset.
        if [ -n "${STUB_REPO_SLUG:-}" ]; then echo "$STUB_REPO_SLUG"; exit 0; fi
        exit 1 ;;
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
    # merge-gates.sh fail-closed-sources its merge-gates.d/ modules (the allow-list
    # constant now lives in 00-common.sh) — the fixture must carry them or the
    # source aborts and postmortem-owed.sh refuses fail-closed (false negative).
    cp -r "$REPO_ROOT/agents/scripts/core/merge-gates.d" "$FIX_REPO/agents/scripts/core/"
    # postmortem-owed.sh now sources lib/resolve-repo.sh (core-scripts-bash-07);
    # the fixture must carry it or the `. lib/resolve-repo.sh` aborts under set -e.
    cp -r "$REPO_ROOT/agents/scripts/core/lib" "$FIX_REPO/agents/scripts/core/"
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

# ============================================================================
# --blocking mode (opt-in enforcement; default modes stay advisory exit 0)
# ============================================================================

@test "blocking mode: clean merge exits 0 (nothing owed)" {
    prlist <<'JSON'
[{"number":9101,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run bash "$SCRIPT" --blocking
    [ "$status" -eq 0 ]
    [[ "$output" == *"no gate escapes owed"* ]]
}

@test "blocking mode: an owed escape exits 1 (default grace 0)" {
    prlist <<'JSON'
[{"number":9102,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b2"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run bash "$SCRIPT" --blocking
    [ "$status" -eq 1 ]
    [[ "$output" == *"postmortem owed: PR #9102 — red-check: Windows + MSVC"* ]]
}

@test "blocking mode: same owed escape is advisory (exit 0) under --list" {
    prlist <<'JSON'
[{"number":9103,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b3"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #9103"* ]]
}

@test "blocking mode: grace absorbs the owed count (exit 0 when count not over grace)" {
    export POSTMORTEM_BLOCKING_GRACE=1
    prlist <<'JSON'
[{"number":9104,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"b4"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run bash "$SCRIPT" --blocking
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #9104"* ]]
}

# ============================================================================
# Repo-slug resolution (core-scripts-bash-07) — no hardcoded fallback
# ============================================================================

@test "REPO unset + gh cannot resolve slug -> advisory skip, exit 0 (no hardcoded fallback)" {
    # Drop the $REPO seam and leave STUB_REPO_SLUG unset so the stub gh's
    # `repo view` exits non-zero: resolve_repo returns unresolvable and the
    # advisory detector must skip cleanly (exit 0), never fall back to a
    # hardcoded owner/name.
    unset REPO
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"cannot resolve repo"* ]]
    [[ "$output" != *"alexandrosk0"* ]]
}

@test "REPO unset + gh resolves slug -> detector runs against the derived repo" {
    # With STUB_REPO_SLUG set, resolve_repo derives the slug via gh and the
    # detector proceeds normally (clean fixture -> owes nothing).
    unset REPO
    export STUB_REPO_SLUG="derived/repo"
    prlist <<'JSON'
[{"number":2002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"no gate escapes owed"* ]]
}

# ============================================================================
# required-check-that-never-reports-is-invisible (infra P1) +
# admin-merge-past-absent-checks-undetected (process P1) — the required-ABSENT
# cross-check. A required context missing from a merged PR's rollup never ran;
# that escape emits no red check, no override label and no revert, so before
# this it was invisible to every trigger in this script. These tests opt back
# into the detector (setup() parks it behind an absurd grace — see there).
# ============================================================================

@test "required-absent: a required context missing from the rollup owes a postmortem" {
    # PR #8000 establishes that "Perf PR-fast" WAS reporting before #8001 merged.
    # That evidence is load-bearing since the effective-date gate landed: absence
    # only means "escape" once the context is known to have existed, and the only
    # available evidence for that is having observed it in the window.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8000,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"f0"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"}]},
 {"number":8001,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f1"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #8001"* ]]
    [[ "$output" == *"required-absent: Perf PR-fast (windows-2022)"* ]]
    # The corroborating PR itself is clean.
    [[ "$output" != *"PR #8000"* ]]
}

@test "required-absent: a rollup carrying every required context owes nothing" {
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8002,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #8002"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

@test "required-absent: PRESENT-but-queued is NOT absent (it is the never-terminal signal instead)" {
    # The absent signal distinguishes "never reported" from "reported and failed" AND
    # from "still running". A required context sitting in QUEUED has produced a rollup
    # row, so it is present — not an ABSENCE escape. It is still an escape, just a
    # differently-named one: see § required-never-terminal below. Asserting both here
    # is the point — the two signals must partition this case, not overlap on it.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8003,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f3"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"QUEUED","conclusion":null,"startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"required-absent"* ]]
    [[ "$output" == *"required-never-terminal: Perf PR-fast (windows-2022)"* ]]
}

# ============================================================================
# required-never-terminal — #1964's actual shape (infra P1). A required context
# can be PRESENT and still never have reported: #1964 merged with ten contexts
# non-green, six still `queued`. Invisible to every other signal — the absence
# check sees it as present, and the red-check curation requires a TERMINAL
# verdict, so QUEUED/IN_PROGRESS is neither red nor absent.
# ============================================================================

@test "never-terminal: the #1964 shape (merged with required contexts still queued) owes" {
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8101,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g1"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"QUEUED","conclusion":null,"startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"IN_PROGRESS","conclusion":null,"startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #8101"* ]]
    [[ "$output" == *"required-never-terminal: Test-delta gate, Windows + MSVC"* ]]
    # The one that DID finish must not be named.
    [[ "$output" != *"required-never-terminal: Test-delta gate, Windows + MSVC, Perf"* ]]
}

@test "never-terminal: an all-terminal rollup owes nothing (negative canary)" {
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8102,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SKIPPED","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"NEUTRAL","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"required-never-terminal"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

@test "never-terminal: a NON-required context left queued is not an escape" {
    # Only branch-protection-required contexts can block a merge, so only they owe
    # a postmortem for never finishing. An advisory lane left queued is normal.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8103,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g3"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Some advisory lane","status":"QUEUED","conclusion":null,"startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"required-never-terminal"* ]]
}

@test "never-terminal: a PENDING StatusContext (not a CheckRun) counts" {
    # The rollup mixes CheckRun and StatusContext nodes; a required context supplied
    # as a status (CodeRabbit's `CR findings` shape) stuck at PENDING never reported
    # either, and must not slip through on node type.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    export POSTMORTEM_REQUIRED_CONTEXTS="Test-delta gate,CR findings (0 actionable)"
    prlist <<'JSON'
[{"number":8104,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g4"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"StatusContext","context":"CR findings (0 actionable)","state":"PENDING","createdAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"required-never-terminal: CR findings (0 actionable)"* ]]
}

@test "never-terminal: a merge inside the grace window is NOT judged (mid-flight != stuck)" {
    # A check that was merely running at merge reaches a terminal state within
    # minutes. Judging inside the lag window would flag every healthy fast merge.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=3600
    printf '[{"number":8105,"mergedAt":"%s","mergeCommit":{"oid":"g5"},"labels":[],"statusCheckRollup":[{"__typename":"CheckRun","name":"Test-delta gate","status":"QUEUED","conclusion":null,"startedAt":"2026-06-10T09:00:00Z"},{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},{"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$PM_PRLIST_FIXTURE"
    run_detector
    [[ "$output" != *"PR #8105"* ]]
}

@test "never-terminal: latest-run dedupe - a QUEUED rerun beside an earlier SUCCESS still counts" {
    # Same dedupe rule the red-check curation uses: the LATEST run per context wins.
    # A re-run sitting queued means the context's current state never reported, even
    # though an older run of it did.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8106,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g6"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T08:00:00Z"},
    {"__typename":"CheckRun","name":"Test-delta gate","status":"QUEUED","conclusion":null,"startedAt":"2026-06-10T09:30:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"required-never-terminal: Test-delta gate"* ]]
}

@test "never-terminal: a POST-merge rerun is not evidence about the merge" {
    # The distinction the test above does not cover. There the QUEUED rerun starts
    # at 09:30 against a 10:00 merge — pre-merge, so at merge time the latest run
    # genuinely had not reported, and flagging it is correct.
    #
    # Here the rerun starts AFTER the merge. A green required run satisfied the
    # merge; someone re-ran it afterwards and that rerun is still queued inside the
    # grace window. Taking the latest run unconditionally read that as
    # `required-never-terminal` — a phantom escape on a PR that merged correctly,
    # and re-runs are routine rather than exotic.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8107,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g7"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Test-delta gate","status":"QUEUED","conclusion":null,"startedAt":"2026-06-10T10:30:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"required-never-terminal"* ]]
}

@test "never-terminal: a run with NO timestamp is still judged (unknown is not 'after')" {
    # Dropping untimestamped runs would be the over-eager version of the fix above
    # and would blind the detector to the #1964 shape it exists for.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8108,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g8"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"QUEUED","conclusion":null},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"required-never-terminal: Test-delta gate"* ]]
}

@test "required-absent: the #1941 --admin merge (zero rollup, no red, no label) owes" {
    # The reported escape: PR #1941 squash-merged via `gh pr merge --squash --admin`
    # with all 22 required contexts absent and its head never built at all
    # (actions/runs?head_sha= -> total_count 0). No red check, no override label,
    # no revert -> the four legacy triggers all read "clean". This is the
    # regression test for "postmortem-owed.sh --list reported no gate escapes owed".
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8004,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f4"},"labels":[],
  "statusCheckRollup":[]}]
JSON
    run_detector
    [[ "$output" == *"PR #8004"* ]]
    [[ "$output" == *"required-absent: Test-delta gate, Windows + MSVC, Perf PR-fast (windows-2022)"* ]]
}

@test "required-absent: a merge inside the run-creation grace window is NOT judged" {
    # Check-suite creation LAGS the push (measured ~27 min on #1976), and no API
    # field distinguishes "not created yet" from "never created". A merge younger
    # than the grace is deferred to the next sweep rather than false-flagged.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=3600
    printf '[{"number":8005,"mergedAt":"%s","mergeCommit":{"oid":"f5"},"labels":[],"statusCheckRollup":[]}]\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$PM_PRLIST_FIXTURE"
    run_detector
    [[ "$output" != *"PR #8005"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

@test "required-absent: a non-numeric grace falls back to the 3600s default" {
    export POSTMORTEM_ABSENT_GRACE_SECONDS="not-a-number"
    printf '[{"number":8006,"mergedAt":"%s","mergeCommit":{"oid":"f6"},"labels":[],"statusCheckRollup":[]}]\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$PM_PRLIST_FIXTURE"
    run_detector
    # Fresh merge + default 3600s grace -> deferred, not flagged (proves the
    # fallback parsed as a number rather than degrading to 0 / an empty compare).
    [[ "$output" != *"PR #8006"* ]]
}

@test "required-absent: flagged on the snapshot path too (snapshots record red, not membership)" {
    # A merge-instant snapshot records redChecks/overrideLabels — it does NOT record
    # which contexts were PRESENT in the rollup, so absence is only ever observable
    # from the live rollup. The cross-check must therefore run on both paths, or an
    # instrumented merge would be exempt from the one signal snapshots cannot carry.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8000,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"f0"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"}]},
 {"number":8007,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f7"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    echo '{"pr":8007,"mergeCommit":"f7","redChecks":[],"overrideLabels":[]}' > "$SNAPSHOT_LEDGER"
    run_detector
    [[ "$output" == *"PR #8007"* ]]
    [[ "$output" == *"required-absent: Perf PR-fast (windows-2022)"* ]]
}

@test "required-absent: empty required set (jq absent / unset) is inert" {
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    export POSTMORTEM_REQUIRED_CONTEXTS=""
    prlist <<'JSON'
[{"number":8008,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"f8"},"labels":[],
  "statusCheckRollup":[]}]
JSON
    run_detector
    [[ "$output" != *"PR #8008"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

# ── required-absent effective dates (required-absent-judges-history-by-todays-
# required-set, tooling P2) ──────────────────────────────────────────────────
# $REQ_CTX_JSON is TODAY's required set applied to every PR in the window, so a
# context promoted after a PR merged is absent from that PR BY DESIGN. The old
# behaviour flagged all of them at once, and with POSTMORTEM_BLOCKING_GRACE=0
# that hard-fails `--blocking` at SessionStart — a routine branch-protection
# change could wedge every new session with phantom flags.

@test "required-absent: a PR that merged BEFORE a context first reported is not flagged" {
    # #8010 merged on the 9th without "Perf PR-fast"; the context is first observed
    # on #8011 the next day. #8010 predates it -> not an escape.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8010,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"g0"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"}]},
 {"number":8011,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"g1"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #8010"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

@test "required-absent: a PR merged AFTER the context first reported is still flagged" {
    # The other side of the same boundary — the fix must not blind the detector to
    # a real escape that happens to sit later in the window.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8012,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"g2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"}]},
 {"number":8013,"mergedAt":"2026-06-11T10:00:00Z","mergeCommit":{"oid":"g3"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-11T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-11T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #8013"* ]]
    [[ "$output" == *"required-absent: Perf PR-fast (windows-2022)"* ]]
    [[ "$output" != *"PR #8012"* ]]
}

@test "required-absent: promoting a context does not flag the whole window" {
    # THE WEDGE, directly. Three historical PRs, none carrying the newly-required
    # context. Previously: three phantom required-absent flags, enough to hard-fail
    # --blocking at SessionStart. Now: nothing owed, one diagnostic.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8020,"mergedAt":"2026-06-08T10:00:00Z","mergeCommit":{"oid":"h0"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-08T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-08T09:00:00Z"}]},
 {"number":8021,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"h1"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"}]},
 {"number":8022,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"h2"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"required-absent"* ]]
    [[ "$output" == *"no gate escapes owed"* ]]
}

@test "required-absent: an undatable context is reported once, not per PR" {
    # Signal preserved, wedge removed: the never-observed context is still named
    # loudly — it may be the #1941 shape rather than a fresh promotion — but as a
    # single diagnostic rather than one flag per PR.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8030,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"i0"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-09T09:00:00Z"}]},
 {"number":8031,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"i1"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"Perf PR-fast (windows-2022)"* ]]
    [[ "$output" == *"present on NO merged PR"* ]]
    # Exactly one mention, not one per PR.
    [ "$(printf '%s\n' "$output" | grep -c 'present on NO merged PR')" -eq 1 ]
}

@test "required-absent: an EMPTY rollup is flagged regardless of dating" {
    # The #1941 / #1972-#1974 shape survives the effective-date gate untouched.
    # "Nothing reported at all" cannot be explained by a late promotion — had the
    # context merely been added since, the OTHER required contexts would still be
    # in the rollup. This is the case the gate must never suppress.
    export POSTMORTEM_ABSENT_GRACE_SECONDS=0
    prlist <<'JSON'
[{"number":8040,"mergedAt":"2026-06-09T10:00:00Z","mergeCommit":{"oid":"j0"},"labels":[],
  "statusCheckRollup":[]},
 {"number":8041,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"j1"},"labels":[],
  "statusCheckRollup":[
    {"__typename":"CheckRun","name":"Test-delta gate","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"},
    {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    # #8040 merged with an empty rollup and predates every observation — still owed.
    [[ "$output" == *"PR #8040"* ]]
    [[ "$output" == *"required-absent"* ]]
}

# ── has_entry dedups on ENTRY HEADINGS, never on free prose ──────────────────
# (postmortem-owed-prose-mention-false-dedup, tooling P1)
# Citing a PR is how these entries are normally written — "shipped in PR #1953",
# "concurrently by PR #1078". While probe 1 was unscoped, any such citation
# PERMANENTLY and SILENTLY exempted that PR from ever being nudged, in the one
# detector whose job is turning escapes into gates. The reported instance was
# #1962: a real override escape reported as a clean window.

@test "dedup: a PROSE mention of a PR does NOT suppress its own owed escape" {
    # The exact shape observed in the ledger: an unrelated entry citing the PR
    # in its body. #2006 has no entry of its own, so it is still owed.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
## 2026-06-09 · PR #1937 · something else entirely

This entry rests on the determinism fix shipped in PR #2006, which is cited
here purely as prose and is not itself postmortemed.
MD
    prlist <<'JSON'
[{"number":2006,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #2006"* ]]
}

@test "dedup: a heading in the documented shape still suppresses" {
    # The other side of the same boundary — scoping must not re-nudge a PR that
    # genuinely has an entry.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
## 2026-06-10 · PR #2006 · Windows + MSVC red at merge
MD
    prlist <<'JSON'
[{"number":2006,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c2"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #2006"* ]]
}

@test "dedup: a prose mention does not suppress a COMBINED-heading sibling either" {
    # Probe 2 was already heading-scoped; this pins that prose cannot satisfy it
    # by supplying the `PR #<other>` half from a body line.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
Body prose naming PR #900 and separately mentioning #907 in the same sentence.
MD
    prlist <<'JSON'
[{"number":907,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c3"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #907"* ]]
}

@test "dedup: a substring PR number does not cross-suppress" {
    # #200 must not be deduped by an entry for #2006 (and vice versa) — the
    # trailing-boundary class is what prevents it.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
## 2026-06-10 · PR #2006 · Windows + MSVC red at merge
MD
    prlist <<'JSON'
[{"number":200,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"c4"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #200"* ]]
}

@test "dedup: a #N in the heading's TRIGGER text does not suppress PR N" {
    # The heading is `## <date> · PR #A[, #B] · <trigger>`, so `·` delimits the
    # fields. Before the field anchor, the combined probe's `.*` spanned the whole
    # heading, so any `#N` in the trigger prose deduped PR N — the same permanent
    # silent suppression as the body-prose case, one field further in.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
## 2026-06-10 · PR #1948 · override used to unwedge a required gate, see /#907
MD
    prlist <<'JSON'
[{"number":907,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d1"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #907"* ]]
}

_combined_heading_case() {  # <heading-line>
    # Shared body: every PR in the list must dedup, INCLUDING the last one, whose
    # match depends on the end-of-line half of the `([^0-9]|$)` boundary rather
    # than a following separator.
    printf '%s\n' "$1" > "$POSTMORTEM_LEDGER"
    prlist <<'JSON'
[{"number":907,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d2"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]},
 {"number":906,"mergedAt":"2026-06-10T11:00:00Z","mergeCommit":{"oid":"d3"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]},
 {"number":908,"mergedAt":"2026-06-10T12:00:00Z","mergeCommit":{"oid":"d6"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #906"* ]]
    [[ "$output" != *"PR #907"* ]]
    [[ "$output" != *"PR #908"* ]]
}

@test "dedup: a SLASH-joined combined-PR heading suppresses every listed PR" {
    _combined_heading_case '## 2026-06-10 · PR #906/#907/#908 · Windows + MSVC red at merge'
}

@test "dedup: a COMMA-separated combined-PR heading suppresses every listed PR" {
    # The separator class carries both forms; only the slash form was covered, so
    # a regression narrowing it to `/` would have gone unnoticed.
    _combined_heading_case '## 2026-06-10 · PR #906, #907, #908 · Windows + MSVC red at merge'
}

@test "dedup: an escape·collateral heading suppresses the collateral-field PRs" {
    # The live ledger's other combined shape (postmortems.md, 2026-06-27 entry)
    # puts escape and collateral PRs in SEPARATE ·-fields:
    #   `## <date> · PR #A (escape) · PR #B, #C, #D (collateral) · <trigger>`.
    # The first-two-fields bound alone cannot reach the third field, so #B/#C
    # silently lost dedup — re-nudged forever, the exact hole the bound was meant
    # to close, one field further out. The later-field probes admit a field only
    # when it STARTS with `PR #` (a PR-list field); trigger prose never does, so
    # the #907-style trigger-text leak stays closed.
    _combined_heading_case '## 2026-06-27 · PR #950 (escape) · PR #906, #907, #908 (collateral) · CANCELLED check merged via head-swap'
}

@test "dedup: PR #N mid-prose in a LATER field still nudges (conservative)" {
    # `PR #907` buried inside third-field trigger prose does NOT dedup: a late
    # field only counts when it starts with `PR #`. Deliberately conservative —
    # a false nudge is noise once, a false dedup is a permanent silent exemption
    # (the P1 this fix family exists for). Pinned so a future widening to
    # "PR #N anywhere" has to flip this test consciously.
    printf '%s\n' '## 2026-06-10 · PR #950 · reverting PR #907 broke the packaging lane' > "$POSTMORTEM_LEDGER"
    prlist <<'JSON'
[{"number":907,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d7"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" == *"PR #907"* ]]
}

@test "dedup: a combined-PR heading ending at the PR list suppresses the last PR" {
    # Exercises the `$` half of the `([^0-9]|$)` boundary. In the two cases above
    # the trailing PR is followed by ` · <trigger>`, so end-of-line is never
    # reached — dropping `|$` from the pattern leaves both of them GREEN. A
    # heading with no trigger field is the only shape that reaches it.
    _combined_heading_case '## 2026-06-10 · PR #906, #907, #908'
}

@test "dedup: a PR named in the heading's trigger AND listed is still suppressed" {
    # Guards against over-correcting: the field anchor must not break a PR that
    # legitimately appears in the list even when the trigger also mentions it.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
## 2026-06-10 · PR #907 · red at merge, follow-up to /#907
MD
    prlist <<'JSON'
[{"number":907,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d4"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #907"* ]]
}

@test "dedup: an unrelated #N before the PR reference does not break dedup" {
    # A real ledger heading carries `#834` before `PR #941`. Anchoring on the
    # first `#` on the line (rather than the `·` field) dropped this entry and
    # would have re-nudged #941 forever — the over-correction direction.
    cat > "$POSTMORTEM_LEDGER" <<'MD'
## 2026-06-07 · coverage.yml (since #834 graduation), fixed by PR #941 · prose-promise gate
MD
    prlist <<'JSON'
[{"number":941,"mergedAt":"2026-06-10T10:00:00Z","mergeCommit":{"oid":"d5"},"labels":[],
  "statusCheckRollup":[{"__typename":"CheckRun","name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-10T09:00:00Z"}]}]
JSON
    run_detector
    [[ "$output" != *"PR #941"* ]]
}
