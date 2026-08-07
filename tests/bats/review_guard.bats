#!/usr/bin/env bats
# review_guard.bats — regression suite for the review panel's write guard:
# agents/scripts/core/lib/review-guard.sh (port of Whip-Process
# Tools/review-guard.ps1; test semantics ported from Tools/test-run-review.ps1).
#
# Contract under test (docs/agent-rules/review-panels.md § Write-guard contracts):
#   get_dirty_state <root>
#     rc 0 + "path<TAB><XY>:<sha256|absent>:<indexblob|->" lines — scanned
#     rc 1 — could NOT scan (no repo / git failure) = UNVERIFIED, never "clean"
#   get_stray_paths <before> <after> <expected>
#     strays = new paths + signature-changed paths + disappeared-from-dirty
#     paths, minus the expected review outputs
#
# Runs against a REAL throwaway git repo — the guard reads git status/ls-files,
# so a real repo is the faithful fixture.

setup() {
    ROOT="$(git rev-parse --show-toplevel)"
    export ROOT
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    # Guard-state scratch lives OUTSIDE the fixture repo — a state file written
    # inside it would show up in the very scans under test.
    STATE_TMP="$(mktemp -d)"
    export STATE_TMP
    git init --quiet -b develop "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    echo "base" > "$REPO_TMP/tracked.txt"
    git -C "$REPO_TMP" add -A
    git -C "$REPO_TMP" commit --quiet -m base
    # shellcheck source=/dev/null
    source "$ROOT/agents/scripts/core/lib/review-guard.sh"
}

teardown() {
    rm -rf "${REPO_TMP:-}" "${STATE_TMP:-}" "${NOREPO_TMP:-}"
}

# dirty <out-file> — run get_dirty_state on the fixture repo into a file.
dirty() {
    get_dirty_state "$REPO_TMP" > "$1"
}

@test "clean repo: rc 0, zero lines (verified clean, not unverified)" {
    run get_dirty_state "$REPO_TMP"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "no work tree: rc 1 (UNVERIFIED), never an empty clean" {
    NOREPO_TMP="$(mktemp -d)"
    run get_dirty_state "$NOREPO_TMP"
    [ "$status" -eq 1 ]
}

@test ".git file with dead gitdir pointer: rc 1 (git fails, guard must not fail open)" {
    NOREPO_TMP="$(mktemp -d)"
    echo "gitdir: /nonexistent/gitdir/path" > "$NOREPO_TMP/.git"
    run get_dirty_state "$NOREPO_TMP"
    [ "$status" -eq 1 ]
}

@test "baseline captures modified + untracked, with three-part signatures" {
    echo "changed" > "$REPO_TMP/tracked.txt"
    echo "new" > "$REPO_TMP/untracked.txt"
    run get_dirty_state "$REPO_TMP"
    [ "$status" -eq 0 ]
    [[ "$output" == *"tracked.txt"$'\t'" M:"* ]]
    [[ "$output" == *"untracked.txt"$'\t'"??:"* ]]
    # Signature is XY:content-hash:index-blob — path membership alone is not the contract.
    grep -qE $'^tracked.txt\t M:[0-9a-f]{64}:[0-9a-f]+$' <<< "$output"
    grep -qE $'^untracked.txt\t[?][?]:[0-9a-f]{64}:-$' <<< "$output"
}

@test "untracked file inside an untracked directory is a distinct path (-uall)" {
    mkdir -p "$REPO_TMP/newdir"
    echo "hidden" > "$REPO_TMP/newdir/stray.txt"
    run get_dirty_state "$REPO_TMP"
    [ "$status" -eq 0 ]
    [[ "$output" == *"newdir/stray.txt"* ]]
}

@test "expected review file only: no strays" {
    dirty "$STATE_TMP/before"
    echo "review body" > "$REPO_TMP/4-review-1-claude-opus.md"
    dirty "$STATE_TMP/after"
    printf '4-review-1-claude-opus.md\n' > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "new unexpected path: stray" {
    dirty "$STATE_TMP/before"
    echo "oops" > "$REPO_TMP/leg-scribble.txt"
    dirty "$STATE_TMP/after"
    : > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ "$output" = "leg-scribble.txt" ]
}

@test "already-dirty path re-edited during round: stray (the false-clean case)" {
    # The guard's normal operating state: the tree is dirty at launch. A guard
    # keyed on path membership alone reports clean here — the signature must trip.
    echo "in-flight work" > "$REPO_TMP/tracked.txt"
    dirty "$STATE_TMP/before"
    echo "leg overwrote it" > "$REPO_TMP/tracked.txt"
    dirty "$STATE_TMP/after"
    : > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ "$output" = "tracked.txt" ]
}

@test "non-ASCII path re-edited during round: stray (C-quoting must not blind the signature)" {
    # Default git C-quotes non-ASCII paths in status output; without
    # core.quotePath=false the on-disk file is never found, the signature goes
    # constant, and this re-edit would read as clean.
    echo "in-flight work" > "$REPO_TMP/naïve-résumé.txt"
    dirty "$STATE_TMP/before"
    echo "leg overwrote it" > "$REPO_TMP/naïve-résumé.txt"
    dirty "$STATE_TMP/after"
    : > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ "$output" = "naïve-résumé.txt" ]
}

@test "dirty-at-launch path reverted during round: stray (reverted in-flight work)" {
    echo "in-flight work" > "$REPO_TMP/tracked.txt"
    dirty "$STATE_TMP/before"
    echo "base" > "$REPO_TMP/tracked.txt"   # leg restored the committed content
    dirty "$STATE_TMP/after"
    : > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ "$output" = "tracked.txt" ]
}

@test "tracked file deleted during round: stray (signature becomes absent)" {
    dirty "$STATE_TMP/before"
    rm "$REPO_TMP/tracked.txt"
    dirty "$STATE_TMP/after"
    : > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ "$output" = "tracked.txt" ]
}

@test "staged-only index change (same status letters, same bytes): stray via index blob" {
    # An `MM` path re-staged from a different blob: XY and the on-disk bytes
    # both stay put; only the index entry moves. The index blob in the
    # signature is what catches it.
    echo "v2" > "$REPO_TMP/tracked.txt"
    git -C "$REPO_TMP" add tracked.txt
    echo "v4" > "$REPO_TMP/tracked.txt"     # baseline: MM, index=v2, worktree=v4
    dirty "$STATE_TMP/before"
    echo "v3" > "$REPO_TMP/tracked.txt"
    git -C "$REPO_TMP" add tracked.txt
    echo "v4" > "$REPO_TMP/tracked.txt"     # after: MM, index=v3, worktree=v4
    dirty "$STATE_TMP/after"
    : > "$STATE_TMP/expected"
    run get_stray_paths "$STATE_TMP/before" "$STATE_TMP/after" "$STATE_TMP/expected"
    [ "$status" -eq 0 ]
    [ "$output" = "tracked.txt" ]
}

@test "rename records both sides as separate keys" {
    git -C "$REPO_TMP" mv tracked.txt renamed.txt
    run get_dirty_state "$REPO_TMP"
    [ "$status" -eq 0 ]
    # Source: two-part signature (status letters + absent). Destination: full three-part.
    grep -qE $'^tracked.txt\tR.:absent$' <<< "$output"
    grep -qE $'^renamed.txt\tR.:[0-9a-f]{64}:[0-9a-f]+$' <<< "$output"
}
