#!/usr/bin/env bats
# tests/bats/fleet_preflight.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/fleet-preflight.sh — the Workflow-fleet
# pre-launch linter. One assertion family per § of docs/agent-rules/
# workflow-fleets.md § Pre-launch checklist:
#   1. model-pin        — every agent() call must carry a model: key
#   2. input-size       — every staged file ≤ 1/3 context window
#   3. out-of-workspace — no ~/.claude / %TEMP% / abs paths in the script
#   4. checkpoint-step  — prompts must reference a build/ repo-file write
#   5. concurrency      — reminder note when sibling sessions are live
#   6. fanout-width     — parallel()/pipeline() width ≤ min(16,cores-2) slots
#
# Drives everything through the script's env seams (PREFLIGHT_WINDOW_TOKENS /
# PREFLIGHT_ACTIVE_SESSIONS / PREFLIGHT_CONCURRENCY_CAP / PREFLIGHT_SLOTS) and absolute fixture
# paths (the script cd's to repo root, so relative fixture paths would not
# resolve). No gh/git stubs needed — the script is pure static analysis.
#
# Requires: bash, awk, grep, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/fleet-preflight.sh"
    export SCRIPT

    FP_DATA="$(mktemp -d)"
    export FP_DATA

    # Hermetic concurrency seam: default to an EMPTY registry so the § Concurrency
    # note never fires unless a test opts in (the real .claude/.active-sessions may
    # carry live sessions on the dev box and would pollute unrelated assertions).
    export PREFLIGHT_ACTIVE_SESSIONS="$FP_DATA/no-sessions"
    mkdir -p "$PREFLIGHT_ACTIVE_SESSIONS"
}

teardown() {
    [ -n "${FP_DATA:-}" ] && rm -rf "$FP_DATA"
}

# Write a workflow-script fixture from stdin; echo its absolute path.
fixture() {
    local f="$FP_DATA/wf_${BATS_TEST_NUMBER}.js"
    cat > "$f"
    printf '%s' "$f"
}

# ============================================================================
# Check 1 — model-pin
# ============================================================================

@test "unpinned agent() call warns (advisory, exit 0)" {
    f="$(fixture <<'JS'
const x = await agent('find bugs', {schema: BUGS})
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"agent() call without model: pin"* ]]
}

@test "pinned agent() call does not warn for model" {
    f="$(fixture <<'JS'
const x = await agent('find bugs', {model: 'sonnet', schema: BUGS})
log('writing to build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"without model: pin"* ]]
}

@test "mixed pinned + unpinned reports exactly one model warning" {
    f="$(fixture <<'JS'
const a = await agent('one', {model: 'sonnet'})
const b = await agent('two', {schema: S})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    n=$(printf '%s\n' "$output" | grep -c 'without model: pin')
    [ "$n" -eq 1 ]
    [[ "$output" == *"2 agent() call(s)"* ]]
}

@test "subagent( is not mistaken for an agent( call (word-boundary guard)" {
    f="$(fixture <<'JS'
const helper = subagent('not a real fan-out call', {})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 agent() call(s)"* ]]
    [[ "$output" != *"without model: pin"* ]]
}

@test "--strict turns a warning into a non-zero exit" {
    f="$(fixture <<'JS'
const x = await agent('unpinned', {schema: BUGS})
JS
)"
    run bash "$SCRIPT" "$f" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"without model: pin"* ]]
}

# ============================================================================
# Check 2 — input-size
# ============================================================================

@test "oversized staged input warns against the 1/3-window cap" {
    f="$(fixture <<'JS'
const x = await agent('mine evidence', {model: 'sonnet'})
log('build/salvage/results')
JS
)"
    # Tiny window so a small fixture file blows the 1/3 cap (60/3*4 = 80 bytes).
    export PREFLIGHT_WINDOW_TOKENS=60
    mkdir -p "$FP_DATA/fleet"
    printf 'x%.0s' $(seq 1 200) > "$FP_DATA/fleet/big.md"
    run bash "$SCRIPT" "$f" "$FP_DATA/fleet"
    [ "$status" -eq 0 ]
    [[ "$output" == *"over 1/3-window cap"* ]]
    [[ "$output" == *"big.md"* ]]
}

@test "in-cap staged input does not warn on size" {
    f="$(fixture <<'JS'
const x = await agent('mine evidence', {model: 'sonnet'})
log('build/salvage/results')
JS
)"
    mkdir -p "$FP_DATA/fleet"
    printf 'small\n' > "$FP_DATA/fleet/ok.md"
    run bash "$SCRIPT" "$f" "$FP_DATA/fleet"
    [ "$status" -eq 0 ]
    [[ "$output" != *"over 1/3-window cap"* ]]
}

# ============================================================================
# Check 3 — out-of-workspace paths
# ============================================================================

@test "a ~/.claude session path in the script warns" {
    f="$(fixture <<'JS'
const data = read('~/.claude/projects/foo/transcript.jsonl')
const x = await agent('mine', {model: 'sonnet'})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"out-of-workspace path"* ]]
}

@test "a %TEMP% path in the script warns" {
    f="$(fixture <<'JS'
const p = '%TEMP%\\scratch'
const x = await agent('mine', {model: 'sonnet'})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [[ "$output" == *"out-of-workspace path"* ]]
}

@test "an in-workspace build/ path does not warn" {
    f="$(fixture <<'JS'
const out = 'build/salvage/results/agent.md'
const x = await agent('mine', {model: 'sonnet'})
JS
)"
    run bash "$SCRIPT" "$f"
    [[ "$output" != *"out-of-workspace path"* ]]
}

# ============================================================================
# Check 4 — checkpoint-step
# ============================================================================

@test "agent() present but no build/ reference warns on the checkpoint contract" {
    f="$(fixture <<'JS'
const x = await agent('mine evidence', {model: 'sonnet'})
return x
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no agent prompt references a repo-file checkpoint step"* ]]
}

@test "a build/ reference satisfies the checkpoint contract check" {
    f="$(fixture <<'JS'
const x = await agent('mine; write result to build/x/results/me.md', {model: 'sonnet'})
JS
)"
    run bash "$SCRIPT" "$f"
    [[ "$output" != *"checkpoint step"* ]]
}

@test "a script with no agent() calls skips the checkpoint check" {
    f="$(fixture <<'JS'
log('this workflow spawns nothing yet')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 agent() call(s)"* ]]
    [[ "$output" != *"checkpoint step"* ]]
}

# ============================================================================
# Check 5 — concurrency reminder
# ============================================================================

@test "live sibling sessions emit a concurrency reminder note (non-failing)" {
    f="$(fixture <<'JS'
const x = await agent('mine', {model: 'sonnet'})
log('build/x/results')
JS
)"
    : > "$PREFLIGHT_ACTIVE_SESSIONS/sess-aaaa"
    export PREFLIGHT_CONCURRENCY_CAP=5
    run bash "$SCRIPT" "$f" --strict
    # A note must NOT trip --strict (it is informational, not a WARN).
    [ "$status" -eq 0 ]
    [[ "$output" == *"sibling session(s) live"* ]]
    [[ "$output" == *"≤ 5"* ]]
}

# ============================================================================
# Check 6 — fan-out width vs slot count
# ============================================================================

@test "fan-out width over the slot count warns (pipeline, advisory exit 0)" {
    f="$(fixture <<'JS'
const CLUSTERS = [
  {k: 'a', model: 'sonnet'},
  {k: 'b', model: 'sonnet'},
  {k: 'c', model: 'sonnet'},
  {k: 'd', model: 'sonnet'},
  {k: 'e', model: 'sonnet'},
]
await pipeline(CLUSTERS, c => agent(c.k, {model: 'sonnet'}))
log('build/x/results')
JS
)"
    export PREFLIGHT_SLOTS=2
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"fan-out width 5 > local slot count 2"* ]]
}

@test "fan-out width within the slot count emits a note, not a warning" {
    f="$(fixture <<'JS'
await parallel([
  () => agent('one', {model: 'sonnet'}),
  () => agent('two', {model: 'sonnet'}),
])
log('build/x/results')
JS
)"
    export PREFLIGHT_SLOTS=12
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"fan-out width 2 ≤ 12 slot(s)"* ]]
    [[ "$output" != *"fan-out width 2 >"* ]]
}

@test "a wide array with no parallel/pipeline does not trigger check 6" {
    f="$(fixture <<'JS'
const DATA = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]
const x = await agent('mine', {model: 'sonnet'})
log('build/x/results')
JS
)"
    export PREFLIGHT_SLOTS=2
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"fan-out width"* ]]
}

@test "--strict promotes an over-width fan-out to a non-zero exit" {
    f="$(fixture <<'JS'
const CLUSTERS = [
  {k: 'a', model: 'sonnet'},
  {k: 'b', model: 'sonnet'},
  {k: 'c', model: 'sonnet'},
]
await pipeline(CLUSTERS, c => agent(c.k, {model: 'sonnet'}))
log('build/x/results')
JS
)"
    export PREFLIGHT_SLOTS=1
    run bash "$SCRIPT" "$f" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"fan-out width 3 > local slot count 1"* ]]
}

# ============================================================================
# Check 7 — read-discipline (broad scope needs a windowed-read directive)
# ============================================================================

@test "broad-scope prompt with no windowed-read directive warns (read-discipline)" {
    f="$(fixture <<'JS'
const x = await agent('Examine all files in Source/Core/src/Ui/ and summarize', {model: 'sonnet'})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"broad scope with no windowed-read directive"* ]]
}

@test "broad-scope prompt WITH a windowed-read directive does not warn" {
    f="$(fixture <<'JS'
const x = await agent('Examine Source/Core/src/Ui/ — Grep + offset/limit windowed reads, stay under 40 tool calls', {model: 'sonnet'})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"broad scope with no windowed-read directive"* ]]
}

# ============================================================================
# Check 8 — path-hygiene (no bare-basename source file in a prompt)
# ============================================================================

@test "a bare source basename in a prompt warns (path-hygiene)" {
    f="$(fixture <<'JS'
const x = await agent('Read AiErrorRedact.cpp and explain the redaction path', {model: 'sonnet'})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"bare source basename"* ]]
}

@test "a path-prefixed source file does not trip path-hygiene" {
    f="$(fixture <<'JS'
const x = await agent('Read Source/Core/src/Tracker/AiErrorRedact.cpp:42 and explain', {model: 'sonnet'})
log('build/x/results')
JS
)"
    run bash "$SCRIPT" "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"bare source basename"* ]]
}

# ============================================================================
# Argument handling
# ============================================================================

@test "no workflow-script argument is a usage error (exit 2)" {
    run bash "$SCRIPT"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}

@test "a missing workflow-script file is an error (exit 2)" {
    run bash "$SCRIPT" "$FP_DATA/does-not-exist.js"
    [ "$status" -eq 2 ]
    [[ "$output" == *"not found"* ]]
}
