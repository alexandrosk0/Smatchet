#!/usr/bin/env bats
# work_item_owed.bats — regression suite for
# agents/scripts/core/work-item-owed.sh (the open-work-item SessionStart nudge;
# rule: docs/agent-rules/work-items.md — the oldest open item finishes first).
#
# Covers the stage classifier walked backwards (latest artifact wins), the
# silent-nudge contract on empty/missing items dirs, and the nudge block shape.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/agents/scripts/core/work-item-owed.sh"
    ITEMS="$(mktemp -d)/items"
    mkdir -p "$ITEMS"
    export WORK_ITEMS_DIR="$ITEMS"
}

teardown() {
    rm -rf "$(dirname "$ITEMS")"
}

# mkitem <slug> [artifact...]
mkitem() {
    local d="$ITEMS/$1"; shift
    mkdir -p "$d"
    local a
    for a in "$@"; do touch "$d/$a"; done
}

@test "selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "empty scaffold: next is authoring the specification" {
    mkitem 07-fresh
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"work item open: 07-fresh — next: author 1-specification.md"* ]]
}

@test "latest artifact wins: plan + pre-review classifies to resolving the review" {
    mkitem 08-inflight 1-specification.md 2-design.md 3-plan.md 4-review-1-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"08-inflight — next: resolve the pre-implementation review"* ]]
}

@test "pre-gate resolution present: next is user sign-off, then implementation" {
    mkitem 09-gated 3-plan.md 4-review-1-claude-opus.md 4-resolution-1-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"09-gated — next: user sign-off on the pre gate, then implementation"* ]]
}

@test "post-review resolution present: next is retro + close" {
    mkitem 10-closing 5-review-1-claude-opus.md 5-resolution-1-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"10-closing — next: retro + close"* ]]
}

@test "partial post resolution: review round 2 unresolved stays at resolve, not close" {
    mkitem 13-post-partial 5-review-1-claude-opus.md 5-review-2-codex-sol.md \
        5-resolution-1-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"13-post-partial — next: resolve the post-implementation review"* ]]
}

@test "partial pre resolution: review round 2 unresolved stays at resolve, not sign-off" {
    mkitem 14-pre-partial 3-plan.md 4-review-1-claude-opus.md 4-review-2-codex-sol.md \
        4-resolution-1-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"14-pre-partial — next: resolve the pre-implementation review"* ]]
}

@test "resolution named for highest round covers earlier rounds: next is retro + close" {
    mkitem 15-multi-closed 5-review-1-claude-opus.md 5-review-2-codex-sol.md \
        5-resolution-2-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"15-multi-closed — next: retro + close"* ]]
}

@test "double-digit round compares numerically, not lexically" {
    mkitem 16-ten-rounds 5-review-10-claude-opus.md 5-resolution-9-claude-opus.md
    run bash "$SCRIPT" --list
    [[ "$output" == *"16-ten-rounds — next: resolve the post-implementation review"* ]]
}

@test "nudge is silent when no item is open" {
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "nudge is silent and exits 0 when the items dir does not exist" {
    WORK_ITEMS_DIR="$ITEMS/nonexistent" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "nudge block header counts open items" {
    mkitem 11-a 1-specification.md
    mkitem 12-b 1-specification.md 2-design.md
    run bash "$SCRIPT" --nudge
    [[ "$output" == *"open work item(s) (2)"* ]]
    [[ "$output" == *"11-a — next: author 2-design.md"* ]]
    [[ "$output" == *"12-b — next: author 3-plan.md"* ]]
}

@test "unknown flag exits 2 with usage" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}
