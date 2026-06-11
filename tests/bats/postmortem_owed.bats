#!/usr/bin/env bats
# postmortem_owed.bats — regression suite for
# agents/scripts/core/postmortem-owed.sh (gate-escape postmortem nudge;
# plan: docs/plans/memory-inbox-fixes.md).
#
# Covers the owe-vs-soft split (override label owes ONLY alongside a red check;
# label-only = defensive label → soft --list line, silent --nudge) on BOTH the
# snapshot-ledger path and the live-rollup fallback, plus the pre-existing
# behaviours: postmortems-ledger dedupe, Core-scoped de-noise, revert trigger.
# No network: `gh` and `git` are stubbed on PATH (followup_due_nudge.bats
# FAKEBIN convention); ledgers are mktemp fixtures via the env overrides.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/agents/scripts/core/postmortem-owed.sh"
    FIXDIR="$(mktemp -d)"
    export SNAPSHOT_LEDGER="$FIXDIR/merge-snapshots.jsonl"
    export POSTMORTEM_LEDGER="$FIXDIR/postmortems.md"
    : > "$POSTMORTEM_LEDGER"
    stub_tools
}

teardown() {
    rm -rf "$FIXDIR" "${FAKEBIN:-}"
}

# Fake `gh` (auth status OK; `pr list` prints $FAKE_PR_ROWS TSV; `pr view`
# prints $FAKE_PR_FILES paths) + fake `git` (log prints $FAKE_REVERT_LOG) at
# the front of PATH — applied per-test on the run line, never in setup, so
# bats' own git use is untouched. Sets FAKEBIN for teardown.
stub_tools() {
    FAKEBIN="$(mktemp -d)"
    cat > "$FAKEBIN/gh" <<'GH'
#!/usr/bin/env bash
[ "${1:-} ${2:-}" = "auth status" ] && exit 0
case "$*" in
    *"pr list"*) [ -n "${FAKE_PR_ROWS:-}" ] && printf '%s\n' "$FAKE_PR_ROWS"; exit 0 ;;
    *"pr view"*) [ -n "${FAKE_PR_FILES:-}" ] && printf '%s\n' "$FAKE_PR_FILES"; exit 0 ;;
    *) exit 0 ;;
esac
GH
    cat > "$FAKEBIN/git" <<'GIT'
#!/usr/bin/env bash
case "$*" in
    *log*) [ -n "${FAKE_REVERT_LOG:-}" ] && printf '%s\n' "$FAKE_REVERT_LOG"; exit 0 ;;
    *) exit 0 ;;
esac
GIT
    chmod +x "$FAKEBIN/gh" "$FAKEBIN/git"
}

# mkrow <num> <mergeCommit> <space-joined-labels> <comma-joined-redchecks>
# — one live-query TSV row in the script's `gh pr list --jq` shape.
mkrow() { printf '%s\t%s\t%s\t%s' "$1" "$2" "$3" "$4"; }

# mksnap <pr> <mergeCommit> <redChecks-json-array> <overrideLabels-json-array>
mksnap() {
    printf '{"schema":1,"pr":%s,"mergeCommit":"%s","redChecks":%s,"overrideLabels":%s}\n' \
        "$1" "$2" "$3" "$4" >> "$SNAPSHOT_LEDGER"
}

@test "snapshot label-only (defensive) is SILENT in --nudge" {
    mksnap 1110 abc111 '[]' '["cr-out-of-band"]'
    FAKE_PR_ROWS="$(mkrow 1110 abc111 'cr-out-of-band' '')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "snapshot label-only (defensive) shows a soft line in --list, not an owe" {
    mksnap 1110 abc111 '[]' '["cr-out-of-band"]'
    FAKE_PR_ROWS="$(mkrow 1110 abc111 'cr-out-of-band' '')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"postmortem owed:"* ]]
    [[ "$output" == *"defensive label (no postmortem owed): PR #1110"* ]]
    [[ "$output" == *"cr-out-of-band (snapshot: redChecks empty)"* ]]
}

@test "snapshot red + label OWES with both parts" {
    mksnap 901 abc901 '["Sanitizer"]' '["tests-out-of-band"]'
    FAKE_PR_ROWS="$(mkrow 901 abc901 'tests-out-of-band' '')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #901 — red-check: Sanitizer; override: tests-out-of-band"* ]]
}

@test "snapshot red-only OWES and the --nudge block fires" {
    mksnap 902 abc902 '["Coverage"]' '[]'
    FAKE_PR_ROWS="$(mkrow 902 abc902 '' '')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed (1)"* ]]
    [[ "$output" == *"PR #902 — red-check: Coverage"* ]]
}

@test "snapshot clean (no red, no labels) is silent everywhere" {
    mksnap 903 abc903 '[]' '[]'
    FAKE_PR_ROWS="$(mkrow 903 abc903 '' '')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"no gate escapes owed"* ]]
    [[ "$output" != *"defensive label"* ]]
}

@test "snapshot is authoritative: live red ignored when snapshot says clean" {
    mksnap 904 abc904 '[]' '[]'
    FAKE_PR_ROWS="$(mkrow 904 abc904 '' 'Sanitizer')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "live fallback (no snapshot row): red check OWES" {
    FAKE_PR_ROWS="$(mkrow 905 abc905 '' 'Sanitizer')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #905 — red-check: Sanitizer"* ]]
}

@test "live fallback: red + override label OWES with both parts" {
    FAKE_PR_ROWS="$(mkrow 906 abc906 'tests-out-of-band' 'Sanitizer')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #906 — red-check: Sanitizer; override: tests-out-of-band"* ]]
}

@test "live fallback: label-only is a soft --list line, silent --nudge" {
    FAKE_PR_ROWS="$(mkrow 1124 abc124 'tests-out-of-band' '')"
    PATH="$FAKEBIN:$PATH" FAKE_PR_ROWS="$FAKE_PR_ROWS" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"postmortem owed:"* ]]
    [[ "$output" == *"defensive label (no postmortem owed): PR #1124"* ]]
    [[ "$output" == *"tests-out-of-band (live: no red check)"* ]]
    PATH="$FAKEBIN:$PATH" FAKE_PR_ROWS="$FAKE_PR_ROWS" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "mixed PRs: --nudge lists only the owed one, --list shows both kinds" {
    mksnap 907 abc907 '["Sanitizer"]' '[]'
    mksnap 908 abc908 '[]' '["perf-out-of-band"]'
    rows="$(mkrow 907 abc907 '' ''; printf '\n'; mkrow 908 abc908 'perf-out-of-band' '')"
    PATH="$FAKEBIN:$PATH" FAKE_PR_ROWS="$rows" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed (1)"* ]]
    [[ "$output" == *"PR #907"* ]]
    [[ "$output" != *"PR #908"* ]]
    PATH="$FAKEBIN:$PATH" FAKE_PR_ROWS="$rows" run bash "$SCRIPT" --list
    [[ "$output" == *"postmortem owed: PR #907"* ]]
    [[ "$output" == *"defensive label (no postmortem owed): PR #908"* ]]
}

@test "postmortems-ledger entry dedupes an owed PR" {
    echo "## 2026-06-01 — PR #777 escape RCA" > "$POSTMORTEM_LEDGER"
    mksnap 777 abc777 '["Sanitizer"]' '[]'
    FAKE_PR_ROWS="$(mkrow 777 abc777 '' '')" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "Core-scoped red on a non-Core PR is de-noised (dropped)" {
    mksnap 909 abc909 '["Test-delta gate"]' '[]'
    FAKE_PR_ROWS="$(mkrow 909 abc909 '' '')" \
        FAKE_PR_FILES="docs/notes.md" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "Core-scoped red on a Core-cpp PR still OWES" {
    mksnap 910 abc910 '["Test-delta gate"]' '[]'
    FAKE_PR_ROWS="$(mkrow 910 abc910 '' '')" \
        FAKE_PR_FILES="Source/Core/src/Tracker/JiraClient.cpp" \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #910 — red-check: Test-delta gate"* ]]
}

@test "revert subject on develop OWES via the commit scan" {
    FAKE_REVERT_LOG='abcd123 Revert "feat(x): thing (#555)"' \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"postmortem owed: PR #555 — revert: abcd123"* ]]
}

@test "revert prose in a body line does NOT owe (subject-gated)" {
    FAKE_REVERT_LOG='abcd124 docs: explain how the widget Reverts state' \
        PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}
