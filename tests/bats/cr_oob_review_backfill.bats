#!/usr/bin/env bats
# tests/bats/cr_oob_review_backfill.bats
# ----------------------------------------------------------------------------
# Coverage for agents/scripts/core/cr-oob-review-backfill.sh.
#
# Why this backfill exists: `cr-out-of-band` waives the CodeRabbit merge GATE,
# never the REVIEW, and 90 PRs merged under it. CR will review a merged PR on
# request, but the plan allows one included review at a time — a request inside
# that window is DROPPED, not queued (#1977 still had zero reviews 39h after a
# rate-limited request). So the drip must hold rather than retry blindly, and
# these tests pin that arithmetic.
#
# The script's --selftest covers the pure core; these drive the END-TO-END modes
# through the injectable fixture, which the selftest cannot reach.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/cr-oob-review-backfill.sh"
    export SCRIPT
    FIX="$BATS_TEST_TMPDIR/decision.tsv"
    export FIX
    export SMATCHET_CR_BACKFILL_FIXTURE="$FIX"
    export SMATCHET_CR_BACKFILL_DRY_RUN=1
    # Pin the window so a default change cannot silently rewrite these
    # assertions into vacuous ones.
    export SMATCHET_CR_BACKFILL_WINDOW_SECONDS=2700
}

row() { printf '%s\n' "$*" >> "$FIX"; }

@test "the script's own --selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
}

@test "an open window fires exactly one request, newest PR first" {
    printf 'now\t1000000\nqueue\t1995\nqueue\t1977\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" == *"would request review on #1995"* ]]
    # Exactly one — a second line would mean a burst against a one-at-a-time limit.
    [ "$(printf '%s\n' "$output" | grep -c 'would request')" -eq 1 ]
}

@test "a request inside the quota window posts nothing" {
    printf 'now\t1001000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t1000000\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" != *"would request"* ]]
}

@test "a CR-named cooldown suppresses the request even with an empty history" {
    printf 'now\t1000000\nqueue\t1995\ncooldown\t1000600\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" != *"would request"* ]]
}

@test "an elapsed window resumes the drip at the next unrequested PR" {
    printf 'now\t1010000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t1000000\nconfirmed\t1995\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" == *"would request review on #1977"* ]]
}

@test "a request CR never answered halts the drip instead of marching on" {
    # Without this the backfill posts a comment per queued PR and collects
    # nothing — a spam machine that reports the debt as paid.
    printf 'now\t1010000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t1000000\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" != *"would request"* ]]
}

@test "a failed request lookup posts nothing" {
    # The live regression: the upstream lookup returned empty, every guard went
    # inert at once, and the poller re-posted to the same PR every tick.
    printf 'now\t1000000\nqueue\t1995\nqueue\t1977\nscanfail\t1995\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" != *"would request"* ]]
}

@test "a fully-requested queue posts nothing" {
    printf 'now\t1010000\nqueue\t1995\nrequested\t1995\t100\nconfirmed\t1995\n' > "$FIX"
    run bash "$SCRIPT" --poll
    [ "$status" -eq 0 ]
    [[ "$output" != *"would request"* ]]
}

@test "--list reports the requested/owed split" {
    printf 'now\t1010000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t100\nconfirmed\t1995\n' > "$FIX"
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"queue 2"* ]]
    [[ "$output" == *"requested 1"* ]]
    [[ "$output" == *"owed 1"* ]]
}

@test "an unknown mode is a usage error, not a silent no-op" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}
