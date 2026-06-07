#!/usr/bin/env bats
# tests/bats/historical_review_survivors.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/historical-review-survivors.sh — the
# historical-code-review survivor extractor.
#
# Core invariant under test: a line introduced by commit A that a LATER commit
# B rewrote must NOT appear in A's survivor set (git blame re-attributes it to
# B); a line A introduced and nobody touched since MUST appear. This is what
# stops a historical re-review from re-flagging already-fixed code.
#
# Requires: bash, git, python (python3/python), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export SCRIPT="$REPO_ROOT/agents/scripts/core/historical-review-survivors.sh"

    SANDBOX="$(mktemp -d)"
    export SANDBOX
    cd "$SANDBOX"
    git init -q
    git config user.email t@t.t
    git config user.name t
    git config commit.gpgsign false
}

teardown() {
    rm -rf "$SANDBOX"
}

@test "surviving line included, later-modified line excluded" {
    printf 'alpha\nbravo\ncharlie\n' > f.txt
    git add f.txt && git commit -qm "A: add three lines"
    A="$(git rev-parse HEAD)"
    # B rewrites only line 2.
    printf 'alpha\nBRAVO-FIXED\ncharlie\n' > f.txt
    git add f.txt && git commit -qm "B: fix line 2"

    run bash "$SCRIPT" "$A"
    [ "$status" -eq 0 ]
    # alpha + charlie survive (untouched since A); bravo does NOT (B rewrote it).
    echo "$output" | grep -q "alpha"
    echo "$output" | grep -q "charlie"
    ! echo "$output" | grep -q "bravo"          # original line 2 gone
    ! echo "$output" | grep -q "BRAVO-FIXED"    # B's line is not A's survivor
    echo "$output" | grep -q "2/3 introduced line(s) still alive"
}

@test "fully superseded commit reports nothing to review" {
    printf 'one\ntwo\n' > g.txt
    git add g.txt && git commit -qm "A: two lines"
    A="$(git rev-parse HEAD)"
    printf 'ONE\nTWO\n' > g.txt          # B rewrites every line A added
    git add g.txt && git commit -qm "B: rewrite all"

    run bash "$SCRIPT" "$A"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "FULLY SUPERSEDED"
}

@test "all lines survive when nothing touched them" {
    printf 'keep1\nkeep2\n' > h.txt
    git add h.txt && git commit -qm "A: two lines"
    A="$(git rev-parse HEAD)"
    echo "unrelated" > other.txt          # B touches a different file
    git add other.txt && git commit -qm "B: unrelated file"

    run bash "$SCRIPT" "$A"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "keep1"
    echo "$output" | grep -q "keep2"
    echo "$output" | grep -q "2/2 introduced line(s) still alive"
}

@test "non-ancestor commit is rejected" {
    printf 'x\n' > k.txt
    git add k.txt && git commit -qm "base"
    git checkout -q -b side
    printf 'y\n' > k.txt
    git add k.txt && git commit -qm "side-only commit"
    SIDE="$(git rev-parse HEAD)"
    git checkout -q -
    run bash "$SCRIPT" "$SIDE"
    [ "$status" -eq 2 ]
    echo "$output" | grep -q "not an ancestor"
}

@test "deleted-since file contributes no survivors" {
    printf 'gone1\ngone2\n' > d.txt
    git add d.txt && git commit -qm "A: add file"
    A="$(git rev-parse HEAD)"
    git rm -q d.txt && git commit -qm "B: delete file"
    run bash "$SCRIPT" "$A"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "FULLY SUPERSEDED"
}
