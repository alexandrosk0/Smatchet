#!/usr/bin/env bats
# lock_staleness_sweep.bats — regression suite for
# agents/scripts/core/lock-staleness-sweep.sh (Phase 4 of git-ref-plan-locks:
# surface stale refs/locks/* as GitHub Issues; never delete a ref).
#
# Behaviour under test:
#   - no refs/locks/* present            -> exit 0, "No refs/locks/* present".
#   - a FRESH lock (age < THRESHOLD_DAYS) -> "fresh: <slug>", no Issue written.
#   - a STALE lock (age >= threshold), no existing Issue -> "STALE" +
#     `gh issue create` invoked with the `plan-lock-stale` label.
#   - a STALE lock WITH an existing Issue -> `gh issue edit` (update in place),
#     never a second create.
#   - REPO unset and `gh repo view` cannot resolve it -> exit 1.
#
# Runs the REAL script (so its real _lock-json.py helper + python staleness math
# are exercised) against a THROWAWAY git repo whose refs/locks/<slug> are built
# with plumbing (hash-object / mktree / commit-tree / update-ref). gh is stubbed
# and logs each `issue create`/`issue edit` to $GH_LOG for assertions.
#
# Requires: bash, git, python(3), bats.

setup() {
    SCRIPT="$(git rev-parse --show-toplevel)/agents/scripts/core/lock-staleness-sweep.sh"
    export SCRIPT
    # Exec-validate the interpreter: a bare `command -v python3` matches the
    # Windows WinStore alias stub, which resolves on PATH but exits non-zero on
    # run — `iso_ago` below would then emit an empty timestamp into every fixture.
    PYBIN=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then
            PYBIN="$(command -v "$c")"; break
        fi
    done
    export PYBIN
    [ -n "$PYBIN" ] || skip "no working python interpreter"
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    STUB_BIN="$(mktemp -d)"
    export STUB_BIN
    GH_LOG="$REPO_TMP/gh.log"
    export GH_LOG

    git init --quiet -b develop "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    git -C "$REPO_TMP" commit --quiet --allow-empty -m seed

    export REPO="test/repo"
    export THRESHOLD_DAYS=14
    export GH_EXISTING_ISSUE=""   # `gh issue list` returns this (empty => none)

    # --- stub gh: auth ok; repo view -> $GH_REPO_SLUG; issue list -> existing;
    #     issue create/edit -> logged. ------------------------------------------
    cat > "$STUB_BIN/gh" <<'STUB'
#!/usr/bin/env bash
case "$1 $2" in
    "auth status") exit 0 ;;
    "repo view")   printf '%s\n' "${GH_REPO_SLUG-test/repo}" ;;
    "issue list")  printf '%s\n' "${GH_EXISTING_ISSUE-}" ;;
    "issue create") printf 'CREATE %s\n' "$*" >> "$GH_LOG" ;;
    "issue edit")   printf 'EDIT %s\n' "$*" >> "$GH_LOG" ;;
    *) : ;;
esac
exit 0
STUB
    chmod +x "$STUB_BIN/gh"
}

teardown() {
    rm -rf "${REPO_TMP:-}" "${STUB_BIN:-}"
}

# iso_ago <days> — ISO-8601 UTC timestamp <days> before now (via python, so it
# matches the helper's own parsing and is OS-portable).
iso_ago() {
    "$PYBIN" - "$1" <<'PY'
import sys, datetime
d = int(sys.argv[1])
t = datetime.datetime.utcnow() - datetime.timedelta(days=d)
print(t.strftime("%Y-%m-%dT%H:%M:%SZ"))
PY
}

# make_lock <slug> <age-days> — create refs/locks/<slug> whose claim.json has
# started=updated=<age-days> ago.
make_lock() {
    local slug="$1" age="$2" ts claim blob tree commit
    ts="$(iso_ago "$age")"
    claim="$REPO_TMP/claim_${slug}.json"
    cat > "$claim" <<JSON
{"started":"$ts","updated":"$ts","owner":"alice","branch":"claude/$slug","originating_plan":"docs/plans/active/$slug.md","write_set":["a.cpp","b.cpp"]}
JSON
    blob="$(git -C "$REPO_TMP" hash-object -w "$claim")"
    tree="$(printf '100644 blob %s\tclaim.json\n' "$blob" | git -C "$REPO_TMP" mktree)"
    commit="$(git -C "$REPO_TMP" commit-tree "$tree" -m "lock $slug")"
    git -C "$REPO_TMP" update-ref "refs/locks/$slug" "$commit"
}

# Run the sweep from inside the temp repo with the gh stub in front.
sweep() {
    run bash -c 'cd "$1" && PATH="$2:$PATH" bash "$3"' _ "$REPO_TMP" "$STUB_BIN" "$SCRIPT"
}

@test "no refs/locks/* -> exit 0, nothing to sweep" {
    sweep
    [ "$status" -eq 0 ]
    [[ "$output" == *"No refs/locks/* present"* ]]
    [ ! -f "$GH_LOG" ]
}

@test "a FRESH lock -> 'fresh:' line, no Issue written" {
    make_lock alpha 1
    sweep
    [ "$status" -eq 0 ]
    [[ "$output" == *"fresh: alpha"* ]]
    [ ! -f "$GH_LOG" ]
}

@test "a STALE lock with no existing Issue -> STALE warning + gh issue create" {
    make_lock bravo 30
    sweep
    [ "$status" -eq 0 ]
    [[ "$output" == *"STALE: bravo"* ]]
    [ -f "$GH_LOG" ]
    grep -q "^CREATE" "$GH_LOG"
    grep -q "plan-lock-stale" "$GH_LOG"
}

@test "a STALE lock WITH an existing Issue -> gh issue edit, not a 2nd create" {
    make_lock charlie 30
    GH_EXISTING_ISSUE="42" sweep
    [ "$status" -eq 0 ]
    [ -f "$GH_LOG" ]
    grep -q "^EDIT" "$GH_LOG"
    ! grep -q "^CREATE" "$GH_LOG"
}

@test "fresh + stale mix -> only the stale one opens an Issue" {
    make_lock keepfresh 2
    make_lock gostale 40
    sweep
    [ "$status" -eq 0 ]
    [[ "$output" == *"fresh: keepfresh"* ]]
    [[ "$output" == *"STALE: gostale"* ]]
    grep -c "^CREATE" "$GH_LOG" | grep -q '^1$'
}

@test "REPO unset + gh repo view cannot resolve -> exit 1" {
    make_lock echo1 1
    run bash -c 'cd "$1" && PATH="$2:$PATH" GH_REPO_SLUG="" bash -c "unset REPO; bash \"$3\""' _ "$REPO_TMP" "$STUB_BIN" "$SCRIPT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REPO unset"* ]]
}
