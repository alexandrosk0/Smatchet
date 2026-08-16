#!/usr/bin/env bats
# git_janitor.bats — regression suite for agents/scripts/core/git-janitor.sh
# (the post-merge cleanup wrapper).
#
# Scope: the argument-validation + refusal guards that gate the destructive
# Steps 4-6 (checkout develop / ff-merge / branch -D / dual-target build). The
# happy path needs a real MERGED PR + cmake, so it is out of scope here; every
# guard that STOPS the script before it mutates anything is covered:
#   - usage errors (missing --post-merge, missing/non-numeric pr-number) -> 2
#   - uncommitted user work in the tree -> 1 (REFUSE)
#   - PR not MERGED on GitHub -> 1 (REFUSE)
#   - a live sibling session + default DEFER -> 0 (skip HEAD-mutating cleanup)
#
# gh + cmake + git are stubbed on PATH so no network / build / HEAD move occurs.
# The stubs are deliberately minimal — each test sets only what its guard reads.

setup() {
    SCRIPT="$(git rev-parse --show-toplevel)/agents/scripts/core/git-janitor.sh"
    export SCRIPT
    REAL_GIT="$(command -v git)"
    export REAL_GIT
    STUB_BIN="$(mktemp -d)"
    export STUB_BIN
    WORKDIR="$(mktemp -d)"
    export WORKDIR

    # --- stub gh: `pr view --json state`/`headRefName` echo the fixtures ------
    export GH_PR_STATE="MERGED"
    export GH_PR_BRANCH="claude/feature"
    cat > "$STUB_BIN/gh" <<'STUB'
#!/usr/bin/env bash
if [ "$1" = "pr" ] && [ "$2" = "view" ]; then
    # ${VAR-…} (no colon): an explicitly-EMPTY value stays empty (the
    # "gh returned nothing" case) — only a genuinely-unset var defaults.
    case "$*" in
        *"mergeCommit"*)  printf '%s\n' "${GH_PR_MERGED_JSON-}" ;;
        *"state"*)        printf '%s\n' "${GH_PR_STATE-MERGED}" ;;
        *"headRefName"*)  printf '%s\n' "${GH_PR_BRANCH-claude/feature}" ;;
        *) echo "" ;;
    esac
    exit 0
fi
exit 0
STUB
    chmod +x "$STUB_BIN/gh"

    # --- stub cmake: always "build ok" (guards under test never reach it) -----
    printf '#!/usr/bin/env bash\nexit 0\n' > "$STUB_BIN/cmake"
    chmod +x "$STUB_BIN/cmake"

    # --- stub git: intercept the mutating/network verbs; delegate reads -------
    # status/rev-parse/for-each-ref/worktree/show-ref pass through to real git in
    # $WORKDIR; fetch/checkout/merge/branch are no-ops (we assert on the guard
    # that runs BEFORE them, never on the mutation itself).
    cat > "$STUB_BIN/git" <<STUB
#!/usr/bin/env bash
case "\$1" in
    fetch|checkout|merge|branch) exit 0 ;;
    *) exec "$REAL_GIT" "\$@" ;;
esac
STUB
    chmod +x "$STUB_BIN/git"

    # A real git repo for the status / rev-parse reads. `.claude/` is gitignored
    # (as in the real repo) so seeding the session registry under
    # .claude/.active-sessions/ does not dirty the tree and trip Step 1's
    # clean-tree guard.
    git init --quiet -b develop "$WORKDIR"
    git -C "$WORKDIR" config user.email t@t
    git -C "$WORKDIR" config user.name t
    printf '.claude/\n' > "$WORKDIR/.gitignore"
    git -C "$WORKDIR" add .gitignore
    git -C "$WORKDIR" commit --quiet -m seed

    export CLAUDE_SESSION_ID="self-sid"
}

teardown() {
    rm -rf "${STUB_BIN:-}" "${WORKDIR:-}"
}

# Run the janitor from inside $WORKDIR with the stub PATH in front.
janitor() {
    run bash -c 'cd "$1" && PATH="$2:$PATH" bash "$3" "${@:4}"' _ \
        "$WORKDIR" "$STUB_BIN" "$SCRIPT" "$@"
}

@test "no --post-merge flag -> usage exit 2" {
    janitor
    [ "$status" -eq 2 ]
    [[ "$output" == *"Usage:"* ]]
}

@test "missing pr-number -> usage exit 2" {
    janitor --post-merge
    [ "$status" -eq 2 ]
}

@test "non-numeric pr-number -> exit 2" {
    janitor --post-merge abc
    [ "$status" -eq 2 ]
    [[ "$output" == *"must be numeric"* ]]
}

@test "uncommitted work in tree -> REFUSE exit 1" {
    echo dirty > "$WORKDIR/uncommitted.txt"
    janitor --post-merge 123
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSE"* ]]
    [[ "$output" == *"uncommitted"* ]]
}

@test "PR not MERGED -> REFUSE exit 1" {
    GH_PR_STATE="OPEN" janitor --post-merge 123
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSE"* ]]
    [[ "$output" == *"expected MERGED"* ]]
}

@test "gh pr view returns empty state -> exit 1" {
    GH_PR_STATE="" janitor --post-merge 123
    [ "$status" -eq 1 ]
    [[ "$output" == *"cannot verify merge state"* ]]
}

@test "a live sibling session + default DEFER -> exit 0 (skips HEAD mutation)" {
    # Seed a live sibling entry (a different session id, fresh ts, this test's
    # own pid so the authoritative-pid liveness check reads it as alive).
    regdir="$WORKDIR/.claude/.active-sessions"
    mkdir -p "$regdir"
    now="$(date -u +%s)"
    {
        echo "branch=develop"
        echo "ppid=$$"
        echo "ts=$now"
    } > "$regdir/other-sid"
    SMATCHET_REGISTRY_OS=posix janitor --post-merge 123
    [ "$status" -eq 0 ]
    [[ "$output" == *"DEFER"* ]]
}

@test "own session entry does NOT trigger DEFER (self excluded)" {
    # Only the caller's OWN entry is present -> live-sibling count is 0, so the
    # janitor proceeds past the confinement guard (and the stubbed mutations +
    # cmake let it reach the success report).
    regdir="$WORKDIR/.claude/.active-sessions"
    mkdir -p "$regdir"
    now="$(date -u +%s)"
    {
        echo "branch=develop"
        echo "ppid=$$"
        echo "ts=$now"
    } > "$regdir/self-sid"
    SMATCHET_REGISTRY_OS=posix janitor --post-merge 123
    [ "$status" -eq 0 ]
    [[ "$output" != *"DEFER"* ]]
}

# ---------- Step 5.5: merge-snapshot ledger backfill (merge-pipeline-02) ------

@test "backfill: un-ledgered fresh merge appends a BACKFILLED row (actor git-janitor)" {
    export MERGE_SNAPSHOT_LEDGER="$STUB_BIN/ledger.jsonl"
    export GH_PR_MERGED_JSON='{"mergeCommit":{"oid":"mcbf1"},"mergedAt":"'"$(date -u +%Y-%m-%dT%H:%M:%SZ)"'","headRefOid":"headbf1","labels":[{"name":"perf-out-of-band"}],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    janitor --post-merge 4242
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SNAPSHOT_LEDGER" ]
    run cat "$MERGE_SNAPSHOT_LEDGER"
    [[ "$output" == *'"pr":4242,'* ]]
    [[ "$output" == *'"mergeCommit":"mcbf1"'* ]]
    [[ "$output" == *'"gates":"BACKFILLED"'* ]]
    [[ "$output" == *'"redChecks":["Perf PR-fast (windows-2022)"]'* ]]
    [[ "$output" == *'"overrideLabels":["perf-out-of-band"]'* ]]
    [[ "$output" == *'"mergeActor":"git-janitor"'* ]]
}

@test "backfill: row already present -> no duplicate, no BACKFILLED write" {
    export MERGE_SNAPSHOT_LEDGER="$STUB_BIN/ledger.jsonl"
    printf '%s\n' '{"pr":4242,"mergeCommit":"mcbf1","headSha":"headbf1","mergedAt":"2026-08-16T10:00:00Z","gates":"GATES_PASSED","redChecks":[],"overrideLabels":[],"mergeActor":"orchestrator","schema":2}' > "$MERGE_SNAPSHOT_LEDGER"
    export GH_PR_MERGED_JSON='{"mergeCommit":{"oid":"mcbf1"},"mergedAt":"'"$(date -u +%Y-%m-%dT%H:%M:%SZ)"'","headRefOid":"headbf1","labels":[],"statusCheckRollup":[]}'
    janitor --post-merge 4242
    [ "$status" -eq 0 ]
    [[ "$output" == *"already present"* ]]
    run bash -c 'wc -l < "$MERGE_SNAPSHOT_LEDGER"'
    [ "${output// /}" = "1" ]
}

@test "backfill: merge older than the age cap is LEFT AS A HOLE (stale line worse than hole)" {
    export MERGE_SNAPSHOT_LEDGER="$STUB_BIN/ledger.jsonl"
    export GH_PR_MERGED_JSON='{"mergeCommit":{"oid":"mcbf2"},"mergedAt":"2020-01-01T00:00:00Z","headRefOid":"headbf2","labels":[],"statusCheckRollup":[]}'
    janitor --post-merge 4243
    [ "$status" -eq 0 ]
    [ ! -f "$MERGE_SNAPSHOT_LEDGER" ]
    [[ "$output" == *"worse than a hole"* ]]
}

@test "backfill: PR metadata unavailable -> skip note, cleanup still exit 0" {
    export MERGE_SNAPSHOT_LEDGER="$STUB_BIN/ledger.jsonl"
    export GH_PR_MERGED_JSON=""
    janitor --post-merge 4244
    [ "$status" -eq 0 ]
    [ ! -f "$MERGE_SNAPSHOT_LEDGER" ]
    [[ "$output" == *"backfill skipped"* ]]
}
