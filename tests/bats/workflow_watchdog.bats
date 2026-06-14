#!/usr/bin/env bats
# tests/bats/workflow_watchdog.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/workflow-watchdog.sh — the fleet stall
# classifier from docs/agent-rules/workflow-fleets.md § Stall watchdog. Drives
# every branch deterministically through the script's env seams:
#   WATCHDOG_NOW            pins "now" so age is reproducible
#   WATCHDOG_RESULTS_DIR    deliverable count fixture
#   WATCHDOG_TRANSCRIPT_DIR *.jsonl mtime fixture (set via `touch -d @epoch`)
#   WATCHDOG_STATE_FILE     prior-poll baseline (crawl-vs-frozen depends on it)
#   WATCHDOG_FRESH_SECS / WATCHDOG_FROZEN_SECS  thresholds (defaults 120 / 600)
#
# Requires: bash, GNU find (-printf), GNU touch (-d @epoch), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/workflow-watchdog.sh"
    export SCRIPT

    WD_DATA="$(mktemp -d)"
    export WD_DATA

    export WATCHDOG_RESULTS_DIR="$WD_DATA/results"
    export WATCHDOG_TRANSCRIPT_DIR="$WD_DATA/transcripts"
    export WATCHDOG_STATE_FILE="$WD_DATA/state"
    mkdir -p "$WATCHDOG_RESULTS_DIR" "$WATCHDOG_TRANSCRIPT_DIR"

    export WATCHDOG_NOW=1000000   # fixed clock
    # Hermetic cascade defaults: a value inherited from the runner env would
    # silently shift the default-threshold / recency-window assertions.
    unset WATCHDOG_MAX_CASCADE_VICTIMS WATCHDOG_CASCADE_WINDOW_SECS
    # leave FRESH_SECS=120 / FROZEN_SECS=600 at their defaults
}

teardown() {
    [ -n "${WD_DATA:-}" ] && rm -rf "$WD_DATA"
}

# Create N deliverable files in the results dir.
make_results() {
    local n="$1" i
    for i in $(seq 1 "$n"); do : > "$WATCHDOG_RESULTS_DIR/agent_${i}.md"; done
}

# Drop one transcript whose mtime is the given epoch second.
transcript_at() {
    local epoch="$1"
    : > "$WATCHDOG_TRANSCRIPT_DIR/agent-x.jsonl"
    touch -d "@${epoch}" "$WATCHDOG_TRANSCRIPT_DIR/agent-x.jsonl"
}

# Seed the prior-poll baseline "<count> <mtime>".
baseline() { printf '%s %s\n' "$1" "$2" > "$WATCHDOG_STATE_FILE"; }

# Drop a cascade-VICTIM transcript: an agent-*.jsonl carrying BOTH a compaction
# summary and an interrupt marker (the compact->interrupt->restart signature).
victim_transcript() {
    cat > "$WATCHDOG_TRANSCRIPT_DIR/agent-$1.jsonl" <<'JSONL'
{"type":"user","message":{"content":[{"type":"text","text":"lane task prompt"}]}}
{"type":"system","isCompactSummary":true,"message":{"content":"[compacted]"}}
{"type":"user","message":{"content":[{"type":"text","text":"[Request interrupted by user]"}]}}
JSONL
}

@test "fresh transcript (age ≤ FRESH) → progressing" {
    make_results 2
    transcript_at 999970            # age 30 ≤ 120
    run bash "$SCRIPT" demo
    [ "$status" -eq 0 ]
    [[ "$output" == *"progressing"* ]]
}

@test "mtime advanced vs baseline, not fresh → crawl (do NOT kill)" {
    make_results 2
    transcript_at 999700            # age 300 (between fresh and frozen)
    baseline 2 999600               # newest_mtime advanced 999600 → 999700
    run bash "$SCRIPT" demo
    [[ "$output" == *"crawl"* ]]
    [[ "$output" == *"do NOT kill"* ]]
}

@test "result count grew vs baseline, mtime static → crawl" {
    make_results 3
    transcript_at 999700            # age 300
    baseline 1 999700               # same mtime, but count 1 → 3
    run bash "$SCRIPT" demo
    [[ "$output" == *"crawl"* ]]
}

@test "static transcript ≥ FROZEN, nothing advanced → frozen" {
    make_results 2
    transcript_at 999300            # age 700 ≥ 600
    baseline 2 999300               # count + mtime both unchanged
    run bash "$SCRIPT" demo
    [[ "$output" == *"frozen"* ]]
    [[ "$output" == *"kill"* ]]
}

@test "first poll (no baseline) with results + stale mtime leans crawl, not frozen" {
    make_results 3
    transcript_at 999300            # age 700, but no baseline → count 3 > prev 0
    run bash "$SCRIPT" demo
    [[ "$output" == *"crawl"* ]]
    [[ "$output" != *"frozen"* ]]
}

@test "--nudge is silent unless frozen" {
    make_results 2
    transcript_at 999970            # progressing
    run bash "$SCRIPT" demo --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--nudge prints a SessionStart block when frozen" {
    make_results 2
    transcript_at 999300            # age 700
    baseline 2 999300
    run bash "$SCRIPT" demo --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"workflow fleet frozen: demo"* ]]
    [[ "$output" == *"Salvage runbook"* ]]
}

@test "the poll writes the current count + mtime as the next baseline" {
    make_results 4
    transcript_at 999700
    run bash "$SCRIPT" demo
    [ "$status" -eq 0 ]
    [ -f "$WATCHDOG_STATE_FILE" ]
    read -r c m < "$WATCHDOG_STATE_FILE"
    [ "$c" -eq 4 ]
    [ "$m" -eq 999700 ]
}

# --- restart cascade ---------------------------------------------------------

@test "victims at-or-above MAX_CASCADE_VICTIMS yield a cascade verdict (overrides progressing)" {
    make_results 2
    transcript_at 999970                 # fresh -> would be 'progressing' absent cascade
    victim_transcript a; victim_transcript b; victim_transcript c
    run bash "$SCRIPT" demo
    [ "$status" -eq 0 ]
    [[ "$output" == *"]: cascade"* ]]
    [[ "$output" == *"3 cascade victim"* ]]
    [[ "$output" == *"RE-SCOPE"* ]]
}

@test "victims below MAX_CASCADE_VICTIMS keep the normal verdict (counted, not cascade)" {
    make_results 2
    transcript_at 999970
    victim_transcript a; victim_transcript b   # 2 < default 3
    run bash "$SCRIPT" demo
    [[ "$output" != *"]: cascade"* ]]
    [[ "$output" == *"2 cascade victim"* ]]
}

@test "--max-cascade-victims lowers the trigger" {
    make_results 1
    transcript_at 999970
    victim_transcript a; victim_transcript b   # 2 victims
    run bash "$SCRIPT" demo --max-cascade-victims 2
    [ "$status" -eq 0 ]
    [[ "$output" == *"]: cascade"* ]]
}

@test "--nudge prints a restart-cascade block when cascade detected" {
    transcript_at 999970
    victim_transcript a; victim_transcript b; victim_transcript c
    run bash "$SCRIPT" demo --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"restart-cascade: demo"* ]]
    [[ "$output" == *"RE-SCOPE"* ]]
}

@test "clean transcripts (no compaction) do not trigger cascade" {
    make_results 2
    transcript_at 999970
    printf '%s\n' '{"type":"user","message":{"content":"hi"}}' > "$WATCHDOG_TRANSCRIPT_DIR/agent-clean.jsonl"
    run bash "$SCRIPT" demo
    [[ "$output" != *"]: cascade"* ]]
    [[ "$output" == *"0 cascade victim"* ]]
}

@test "stale victims outside the recency window are not counted (run-scoped)" {
    make_results 1
    transcript_at 999970
    victim_transcript a; victim_transcript b; victim_transcript c
    # NOW=1000000, default window 1800 -> cutoff 998200; age these victims to
    # epoch 990000 (older than the cutoff) so a stale historical incident in the
    # shared ~/.claude/projects history does not force cascade on a healthy run.
    touch -d @990000 "$WATCHDOG_TRANSCRIPT_DIR/agent-a.jsonl" \
                     "$WATCHDOG_TRANSCRIPT_DIR/agent-b.jsonl" \
                     "$WATCHDOG_TRANSCRIPT_DIR/agent-c.jsonl"
    run bash "$SCRIPT" demo
    [ "$status" -eq 0 ]
    [[ "$output" != *"]: cascade"* ]]
    [[ "$output" == *"0 cascade victim"* ]]
}

@test "no fleet-slug argument is a usage error (exit 2)" {
    run bash "$SCRIPT"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}
