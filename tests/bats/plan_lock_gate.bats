#!/usr/bin/env bats
# tests/bats/plan_lock_gate.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/plan-lock-gate.sh — Layer C decision logic.
# The decision function is exercised headless (sourced + an injected changed-file
# list + a synthetic lock table via LTC_ROWS_OVERRIDE); the fail-CLOSED base and
# three-dot symmetry are exercised against a throwaway git fixture.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/agents/scripts/core/plan-lock-gate.sh"
    LIB="$REPO_ROOT/agents/scripts/core/lock-table-cache.sh"
    ROWS="$(mktemp)"
    # Dynamic "fresh" epoch (now) so the <14-day-cutoff collision cases never
    # expire on calendar time. A feat/other (cross) lock covering two paths.
    FRESH="$(date -u +%s)"
    {
        printf 'feat/other\t%s\totherslug\tSource/Core/src/Sync/Foo.cpp\n' "$FRESH"
        printf 'feat/other\t%s\totherslug\tSource/Core/src/Persistence/\n' "$FRESH"
    } > "$ROWS"
    export LTC_ROWS_OVERRIDE="$ROWS"
    export LTC_PROJ="$REPO_ROOT"
    export SMATCHET_REGISTRY_OS=posix
}
teardown() { rm -f "$ROWS"; }

# stdin = changed files; sources both libs in a subshell and runs the decider.
run_decide() { # $1 = head ref, $2 = newline-separated changed paths
    ( . "$LIB" >/dev/null 2>&1; . "$GATE" >/dev/null 2>&1
      printf '%s\n' "$2" | plan_lock_gate_decide "$1" )
}

@test "decide: a changed file overlapping an OTHER-branch lock reds (collision)" {
    run run_decide my-feature "Source/Core/src/Sync/Foo.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"otherslug"* ]]
    [[ "$output" == *"::error"* ]]
}

@test "decide: a dir-prefix overlap with an OTHER-branch lock reds" {
    run run_decide my-feature "Source/Core/src/Persistence/Db.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"otherslug"* ]]
}

@test "decide: my OWN branch's lock never blocks me (head == lock branch)" {
    run run_decide feat/other "Source/Core/src/Sync/Foo.cpp"
    [ "$status" -eq 0 ]
}

@test "decide: a changed file under no lock passes" {
    run run_decide my-feature "Source/Core/src/Ui/Unlocked.cpp"
    [ "$status" -eq 0 ]
}

@test "decide: a STALE (>14d) other-branch lock is non-blocking (F3)" {
    printf 'feat/other\t1\toldslug\tSource/Core/src/Sync/Foo.cpp\n' > "$ROWS"
    run run_decide my-feature "Source/Core/src/Sync/Foo.cpp"
    [ "$status" -eq 0 ]
}

@test "decide: an empty/detached lock branch is unattributable (non-blocking)" {
    printf '\t%s\tnobr\tSource/Core/src/Sync/Foo.cpp\n' "$(date -u +%s)" > "$ROWS"
    run run_decide my-feature "Source/Core/src/Sync/Foo.cpp"
    [ "$status" -eq 0 ]
}

@test "decide: an UNDETERMINED lock table (rc=2) FAILS CLOSED (hard-net contract)" {
    # Unlike Layers A/B (advisory, fail-open), the Layer C hard net must red on
    # an unverifiable lock state, not silently pass. Force rc=2 via a stub.
    run bash -c ". '$LIB' >/dev/null 2>&1; . '$GATE' >/dev/null 2>&1
        ltc_covering_slug() { return 2; }
        printf 'Source/Core/src/Sync/Foo.cpp\n' | plan_lock_gate_decide my-feature"
    [ "$status" -eq 1 ]
    [[ "$output" == *"undetermined"* ]]
}

# ---- fail-CLOSED base + three-dot symmetry (against a throwaway git fixture) --

planlock_repo() {
    PROJ="$(mktemp -d)"; PROJ="$(cd "$PROJ" && pwd -P)"
    git -C "$PROJ" init -q -b develop
    git -C "$PROJ" config user.email t@local
    git -C "$PROJ" config user.name t
    echo base > "$PROJ/base.txt"; git -C "$PROJ" add -A; git -C "$PROJ" commit -qm base
    mkdir -p "$PROJ/agents/scripts/core"
    for x in lock-table-cache.sh _lock-json.py session-registry-lib.sh locks-show.sh; do
        cp "$REPO_ROOT/agents/scripts/core/$x" "$PROJ/agents/scripts/core/$x"
    done
}
planlock_repo_teardown() { rm -rf "$PROJ" 2>/dev/null || true; }

@test "main: fail-CLOSED when origin/develop does not resolve (no silent empty set)" {
    planlock_repo
    # No origin/develop ref -> base unresolvable -> red LOUD, not a silent pass.
    run bash -c "cd '$PROJ' && PLAN_LOCK_BASE_REF=develop PLAN_LOCK_HEAD_REF=my-feature LTC_ROWS_OVERRIDE='$ROWS' bash '$GATE'"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not resolve"* ]]
    planlock_repo_teardown
}

@test "main: three-dot origin/develop...HEAD set equals Layer B's merge-base..HEAD set" {
    planlock_repo
    git -C "$PROJ" update-ref refs/remotes/origin/develop develop
    ( cd "$PROJ" && git checkout -q -b my-feature \
        && mkdir -p Source/Core/src/Sync && echo y > Source/Core/src/Sync/Foo.cpp \
        && git add -A && git commit -qm change )
    three="$(git -C "$PROJ" diff --name-only origin/develop...HEAD | sort)"
    twodot="$(git -C "$PROJ" diff --name-only "$(git -C "$PROJ" merge-base origin/develop HEAD)"..HEAD | sort)"
    [ "$three" = "$twodot" ]
    [[ "$three" == *"Source/Core/src/Sync/Foo.cpp"* ]]
    planlock_repo_teardown
}

@test "main: a clean PR (no cross-branch overlap) passes green" {
    planlock_repo
    git -C "$PROJ" update-ref refs/remotes/origin/develop develop
    ( cd "$PROJ" && git checkout -q -b my-feature \
        && mkdir -p Source/Core/src/Ui && echo y > Source/Core/src/Ui/Free.cpp \
        && git add -A && git commit -qm free )
    run bash -c "cd '$PROJ' && PLAN_LOCK_BASE_REF=develop PLAN_LOCK_HEAD_REF=my-feature LTC_ROWS_OVERRIDE='$ROWS' bash '$GATE'"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no cross-branch"* ]]
    planlock_repo_teardown
}

@test "main: a cross-branch overlap reds the gate (end-to-end)" {
    planlock_repo
    git -C "$PROJ" update-ref refs/remotes/origin/develop develop
    ( cd "$PROJ" && git checkout -q -b my-feature \
        && mkdir -p Source/Core/src/Sync && echo y > Source/Core/src/Sync/Foo.cpp \
        && git add -A && git commit -qm collide )
    run bash -c "cd '$PROJ' && PLAN_LOCK_BASE_REF=develop PLAN_LOCK_HEAD_REF=my-feature LTC_ROWS_OVERRIDE='$ROWS' bash '$GATE'"
    [ "$status" -eq 1 ]
    [[ "$output" == *"otherslug"* ]]
    planlock_repo_teardown
}
