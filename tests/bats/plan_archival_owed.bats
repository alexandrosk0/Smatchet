#!/usr/bin/env bats
# plan_archival_owed.bats — regression suite for
# agents/scripts/core/plan-archival-owed.sh (the archive-owed SessionStart nudge;
# rule: docs/agent-rules/process-rules.md § Archive on revision).
#
# Covers the corrected discriminator: FIRE on an explicit `Status: shipped`
# marker only; NEVER on a live plan whose prose merely mentions a shipped slice,
# NEVER on the deferred-tier banner, graceful on pre-Status legacy plans, and the
# secondary cited-PR-not-merged annotation via a STUBBED gh (no network).

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/agents/scripts/core/plan-archival-owed.sh"
    ACT="$(mktemp -d)/active"
    mkdir -p "$ACT"
    export PLAN_ACTIVE_DIR="$ACT"
    export REPO="x/y"
}

teardown() {
    rm -rf "$ACT" "${FAKEBIN:-}"
}

# mkplan <basename> <status-line> [extra lines...]
mkplan() {
    local f="$ACT/$1"; shift
    printf '# Plan — %s\n%s\n' "$1" "$2" > "$f"
    shift 2 2>/dev/null || true
    for extra in "$@"; do printf '%s\n' "$extra" >> "$f"; done
}

# Fake gh: `auth status` ok; `pr view <n> ... state` echoes $FAKE_STATE.
stub_gh() {
    FAKEBIN="$(mktemp -d)"
    cat > "$FAKEBIN/gh" <<'GH'
#!/usr/bin/env bash
[ "${1:-} ${2:-}" = "auth status" ] && exit 0
case "$*" in *"pr view"*"state"*) echo "${FAKE_STATE:-MERGED}" ;; *) exit 0 ;; esac
GH
    chmod +x "$FAKEBIN/gh"
}

@test "selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "explicit Status: shipped FIREs" {
    mkplan a.md "done" "> **Status**: shipped — archived 2026-06-05."
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"plan archival owed: a"* ]]
}

@test "all-caps STATUS: SHIPPED banner FIREs" {
    mkplan b.md "legacy" "> **STATUS: SHIPPED** (banner form)"
    run bash "$SCRIPT" --list
    [[ "$output" == *"plan archival owed: b"* ]]
}

@test "live plan with 'shipped' only in prose does NOT fire (FP guard)" {
    mkplan c.md "live" "> **Status**: active — shipped slice 1 only; B5-B11 live."
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "deferred banner does NOT fire" {
    mkplan d.md "parked" "> **STATUS: DEFERRED (pending demand)**"
    run bash "$SCRIPT" --nudge
    [ -z "$output" ]
}

@test "legacy plan without a Status field is invisible (graceful)" {
    mkplan e.md "old" "> **Slug**: \`old\`."
    run bash "$SCRIPT" --nudge
    [ -z "$output" ]
}

@test "_plan-template.md is excluded even when marked shipped" {
    mkplan _plan-template.md "template" "> **Status**: shipped"
    run bash "$SCRIPT" --nudge
    [ -z "$output" ]
}

@test "nudge block header counts owed plans" {
    mkplan a.md "done a" "> **Status**: shipped"
    mkplan b.md "done b" "> **Status**: shipped"
    run bash "$SCRIPT" --nudge
    [[ "$output" == *"plan archival owed (2)"* ]]
}

@test "secondary check: cited un-merged PR is annotated (stubbed gh OPEN)" {
    stub_gh
    mkplan a.md "done" "> **Status**: shipped" "" "## Implementation log" "- abc123 · shipped via #999"
    FAKE_STATE=OPEN PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"plan archival owed: a"* ]]
    [[ "$output" == *"#999(OPEN) not merged"* ]]
}

@test "secondary check: all-merged cited PRs add no annotation (stubbed gh MERGED)" {
    stub_gh
    mkplan a.md "done" "> **Status**: shipped" "" "## Implementation log" "- abc123 · shipped via #777"
    FAKE_STATE=MERGED PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [[ "$output" == *"plan archival owed: a"* ]]
    [[ "$output" != *"not merged"* ]]
}
