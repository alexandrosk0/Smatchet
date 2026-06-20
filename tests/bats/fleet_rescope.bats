#!/usr/bin/env bats
# tests/bats/fleet_rescope.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/fleet-rescope.sh — the cascade abort +
# RE-SCOPE planner from docs/agent-rules/workflow-fleets.md § Stall watchdog /
# § Salvage runbook. Exercises the manifest parse, the unfinished-lane set
# difference (manifest − completed deliverables), the watchdog verdict gate, and
# the output modes through the script's seams:
#   --results-dir <dir>       deliverable-dir fixture (no build/<slug> needed)
#   RESCOPE_WATCHDOG_BIN       stub watchdog so the gate is hermetic + decoupled
#                              from the watchdog's own (separately-tested) logic
#
# Requires: bash, GNU coreutils, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/fleet-rescope.sh"
    export SCRIPT

    FR_DATA="$(mktemp -d)"
    export FR_DATA
    RESULTS="$FR_DATA/results"
    export RESULTS
    mkdir -p "$RESULTS"

    MANIFEST="$FR_DATA/lanes.txt"
    export MANIFEST

    # Default stub watchdog: prints a cascade verdict, so the gate passes unless a
    # test overrides RESCOPE_WATCHDOG_BIN. Decouples these tests from the real
    # watchdog's detection (which has its own suite).
    CASCADE_WD="$FR_DATA/wd-cascade.sh"
    export CASCADE_WD
    cat > "$CASCADE_WD" <<'SH'
#!/usr/bin/env bash
echo "workflow-watchdog[$1]: cascade — stub verdict"
SH
    chmod +x "$CASCADE_WD"

    PROGRESSING_WD="$FR_DATA/wd-progressing.sh"
    export PROGRESSING_WD
    cat > "$PROGRESSING_WD" <<'SH'
#!/usr/bin/env bash
echo "workflow-watchdog[$1]: progressing — stub verdict"
SH
    chmod +x "$PROGRESSING_WD"

    export RESCOPE_WATCHDOG_BIN="$CASCADE_WD"
}

teardown() {
    [ -n "${FR_DATA:-}" ] && rm -rf "$FR_DATA"
}

# Write a manifest with the given lane ids (one per line).
make_manifest() { printf '%s\n' "$@" > "$MANIFEST"; }

# Mark a lane "completed" by writing a non-empty deliverable.
complete_lane() { printf 'done\n' > "$RESULTS/$1.md"; }

@test "unfinished = manifest minus completed deliverables" {
    make_manifest lane-a lane-b lane-c lane-d
    complete_lane lane-a
    complete_lane lane-c
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 0 ]
    [[ "$output" == *"completed: 2"* ]]
    [[ "$output" == *"unfinished: 2"* ]]
    [[ "$output" == *"lane-b"* ]]
    [[ "$output" == *"lane-d"* ]]
}

@test "--list-unfinished prints ONLY the unfinished lane ids, newline-separated" {
    make_manifest a b c
    complete_lane b
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS" --list-unfinished
    [ "$status" -eq 0 ]
    # exactly the two unfinished ids, nothing else (no plan prose)
    [ "${lines[0]}" = "a" ]
    [ "${lines[1]}" = "c" ]
    [ "${#lines[@]}" -eq 2 ]
}

@test "a completely-unproduced fleet (no results dir) lists every lane as unfinished" {
    make_manifest lua-sandbox config-sqlite subprocess-exec
    rm -rf "$RESULTS"   # the incident: the fleet wrote nothing
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 0 ]
    [[ "$output" == *"unfinished: 3"* ]]
    [[ "$output" == *"lua-sandbox"* ]]
}

@test "a zero-byte deliverable does NOT count as completed (progress-marker stub)" {
    make_manifest a b
    : > "$RESULTS/a.md"   # empty file
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS" --list-unfinished
    [ "$status" -eq 0 ]
    # both still unfinished — the empty 'a.md' is not a real deliverable
    [[ "$output" == *"a"* ]]
    [[ "$output" == *"b"* ]]
    [ "${#lines[@]}" -eq 2 ]
}

@test "a bare (extension-less) deliverable counts as completed" {
    make_manifest a b
    printf 'done\n' > "$RESULTS/a"   # results/a, no extension
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS" --list-unfinished
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "b" ]
    [ "${#lines[@]}" -eq 1 ]
}

@test "all lanes complete -> nothing to relaunch (converged)" {
    make_manifest a b
    complete_lane a; complete_lane b
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 0 ]
    [[ "$output" == *"unfinished: 0"* ]]
    [[ "$output" == *"converged"* ]]
}

@test "manifest ignores blank lines and # comments and trims whitespace" {
    printf '%s\n' '# header comment' '  lane-a  ' '' 'lane-b' '   ' '# trailing' > "$MANIFEST"
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS" --list-unfinished
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "lane-a" ]
    [ "${lines[1]}" = "lane-b" ]
    [ "${#lines[@]}" -eq 2 ]
}

@test "--lanes-csv works and unions with --lanes (de-duped)" {
    make_manifest a b
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --lanes-csv "b,c" --results-dir "$RESULTS" --list-unfinished
    [ "$status" -eq 0 ]
    # union {a,b,c}, b appears once
    [[ "$output" == *"a"* ]]
    [[ "$output" == *"c"* ]]
    [ "${#lines[@]}" -eq 3 ]
}

@test "an invalid lane id (glob/slash) is skipped with a WARN, not matched" {
    make_manifest 'a' 'b/../etc' 'c*'
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS" --list-unfinished
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN skipping invalid lane id"* ]]
    # only the valid 'a' survives the manifest
    [[ "$output" == *"a"* ]]
}

# --- verdict gate ------------------------------------------------------------

@test "verdict gate REFUSES (exit 3) when the watchdog says not-cascade" {
    make_manifest a b
    export RESCOPE_WATCHDOG_BIN="$PROGRESSING_WD"
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 3 ]
    [[ "$output" == *"REFUSED"* ]]
    [[ "$output" == *"progressing"* ]]
}

@test "--force emits the plan even on a non-cascade verdict (and skips the probe)" {
    make_manifest a b
    complete_lane a
    export RESCOPE_WATCHDOG_BIN="$PROGRESSING_WD"
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS" --force
    [ "$status" -eq 0 ]
    [[ "$output" == *"--force"* ]]
    [[ "$output" == *"unfinished: 1"* ]]
}

@test "a cascade verdict passes the gate and emits the abort+re-scope plan" {
    make_manifest a b c
    complete_lane a
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict: cascade"* ]]
    [[ "$output" == *"ABORT + RELAUNCH"* ]]
    [[ "$output" == *"never kills"* ]]
    [[ "$output" == *"smaller model"* ]]
}

@test "a missing watchdog is fail-open with a WARN (plan still emitted)" {
    make_manifest a b
    export RESCOPE_WATCHDOG_BIN="$FR_DATA/does-not-exist.sh"
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 0 ]
    [[ "$output" == *"UNCONFIRMED"* ]]
    [[ "$output" == *"unfinished: 2"* ]]
}

# --- usage -------------------------------------------------------------------

@test "no fleet-slug is a usage error (exit 2)" {
    run bash "$SCRIPT" --lanes "$MANIFEST"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}

@test "no manifest is a usage error (exit 2)" {
    run bash "$SCRIPT" demo
    [ "$status" -eq 2 ]
    [[ "$output" == *"manifest is required"* ]]
}

@test "an empty manifest (only comments) is a usage error (exit 2)" {
    printf '%s\n' '# nothing here' '   ' > "$MANIFEST"
    run bash "$SCRIPT" demo --lanes "$MANIFEST" --results-dir "$RESULTS"
    [ "$status" -eq 2 ]
    [[ "$output" == *"zero lanes"* ]]
}
