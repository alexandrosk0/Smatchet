#!/usr/bin/env bats
# followup_due_nudge.bats — regression suite for
# agents/scripts/core/followup-due-nudge.sh (triggered-follow-up SessionStart
# nudge; plan: docs/plans/triggered-followup-tracking.md).
#
# Covers the 4 trigger kinds (date / plan-shipped / pr-count via a STUBBED gh /
# file-age), fired= idempotency suppression, malformed-when= WARN (no false
# fire), backward-compat (no-trigger entries invisible), and the offline
# (#N)-grep fallback shape. No network: pr-count uses a fake `gh` on PATH.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/agents/scripts/core/followup-due-nudge.sh"
    CATDIR="$(mktemp -d)"
    export FOLLOWUP_CAT_DIR="$CATDIR"
    export FOLLOWUP_TODAY="2026-06-05"
}

teardown() {
    rm -rf "$CATDIR" "${FAKEBIN:-}"
}

# mkentry <file> <priority> <trigger-line>
mkentry() {
    {
        printf -- '- 2026-06-05 · orchestrator · [process] · %s — fixture entry\n' "$2"
        printf '  Details: fixture\n'
        printf '  %s\n' "$3"
        printf '  Status: open\n\n'
    } >> "$CATDIR/$1"
}

# Put a fake `gh` (handles `auth status` + echoes $FAKE_PR_COUNT for `pr list`)
# at the front of PATH. Sets FAKEBIN for teardown.
stub_gh() {
    FAKEBIN="$(mktemp -d)"
    cat > "$FAKEBIN/gh" <<'GH'
#!/usr/bin/env bash
[ "${1:-} ${2:-}" = "auth status" ] && exit 0
case "$*" in *"pr list"*) echo "${FAKE_PR_COUNT:-0}" ;; *) exit 0 ;; esac
GH
    chmod +x "$FAKEBIN/gh"
}

@test "selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"Failed: 0"* ]]
}

@test "date trigger in the past FIREs" {
    mkentry a.md P2 "Triggered-follow-up: when=date:2026-01-01; action=do the overdue thing; baseline=metric; fired=never"
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"follow-up due (1)"* ]]
    [[ "$output" == *"do the overdue thing"* ]]
    [[ "$output" == *"baseline: metric"* ]]
}

@test "date trigger in the future is silent" {
    mkentry a.md P2 "Triggered-follow-up: when=date:2099-01-01; action=future thing; baseline=x; fired=never"
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "fired= date suppresses the entry (idempotency)" {
    mkentry a.md P2 "Triggered-follow-up: when=date:2026-01-01; action=already done; baseline=x; fired=2026-06-01"
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "plan-shipped FIREs for an existing shipped plan" {
    mkentry a.md P1 "Triggered-follow-up: when=plan-shipped:gate-escape-postmortem; action=ship the ledger; baseline=x; fired=never"
    run bash "$SCRIPT" --list
    [[ "$output" == *"ship the ledger"* ]]
}

@test "plan-shipped is silent for an absent plan" {
    mkentry a.md P1 "Triggered-follow-up: when=plan-shipped:no-such-plan-xyz; action=nope; baseline=x; fired=never"
    run bash "$SCRIPT" --nudge
    [ -z "$output" ]
}

@test "malformed when= WARNs and does not fire" {
    mkentry a.md P2 "Triggered-follow-up: when=date:not-a-date; action=bad; baseline=x; fired=never"
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"follow-up due (1)"* ]]
    [[ "$output" == *"MALFORMED"* ]]
}

@test "entry without a Triggered-follow-up line is invisible (backward-compat)" {
    printf -- '- 2026-06-05 · orchestrator · [process] · P2 — no trigger here\n  Details: x\n' > "$CATDIR/a.md"
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "pr-count FIREs when (stubbed) gh count >= n" {
    stub_gh
    mkentry a.md P2 "Triggered-follow-up: when=pr-count:base=develop;since=2026-01-01;n=10; action=remeasure now; baseline=x; fired=never"
    FAKE_PR_COUNT=12 PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"remeasure now"* ]]
}

@test "pr-count SKIPs when (stubbed) gh count < n" {
    stub_gh
    mkentry a.md P2 "Triggered-follow-up: when=pr-count:base=develop;since=2026-01-01;n=10; action=remeasure now; baseline=x; fired=never"
    FAKE_PR_COUNT=3 PATH="$FAKEBIN:$PATH" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "priority sort: P1 before P3 in the nudge block" {
    mkentry a.md P3 "Triggered-follow-up: when=date:2026-01-01; action=low prio; baseline=x; fired=never"
    mkentry b.md P1 "Triggered-follow-up: when=date:2026-01-01; action=high prio; baseline=x; fired=never"
    run bash "$SCRIPT" --list
    [[ "$output" == *"follow-up due (2)"* ]]
    # high prio (P1) must appear before low prio (P3)
    hi="$(printf '%s\n' "$output" | grep -n 'high prio' | head -1 | cut -d: -f1)"
    lo="$(printf '%s\n' "$output" | grep -n 'low prio' | head -1 | cut -d: -f1)"
    [ "$hi" -lt "$lo" ]
}

@test "offline (#N) fallback grep shape excludes merge + non-PR subjects" {
    # The fallback counts `(#N)` squash subjects; a merge-commit subject and a
    # direct-push subject must NOT be counted. This asserts the grep contract
    # the evaluator relies on (the same shape postmortem-owed uses).
    sample="$(printf '%s\n' \
        'feat(x): a thing (#101)' \
        'Merge pull request #102 from foo/bar' \
        'chore: direct push no pr' \
        'fix(y): another (#103)')"
    count="$(printf '%s\n' "$sample" | grep -cE '\(#[0-9]+\)')"
    [ "$count" -eq 2 ]
}
