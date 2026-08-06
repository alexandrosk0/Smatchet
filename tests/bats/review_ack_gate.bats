#!/usr/bin/env bats
# review_ack_gate.bats — regression suite for the commit-time code-review gate:
# agents/scripts/core/review-ack.sh + agents/scripts/core/lib/review-ack.sh, and
# the scripts/git-hooks/pre-commit check (B) that consumes them.
#
# Contract under test:
#   review-ack.sh --check --staged
#     0 — a current ack covers the staged diff, OR the staged diff is not
#         substantive (no strict-zone touch, < REVIEW_LINE_THRESHOLD C++ lines)
#     1 — a substantive staged C++ diff with a missing or stale ack
#     2 — usage error / not a git work tree
#   review-ack.sh --record --staged   writes the fingerprint for mode `staged`
#   pre-commit                        refuses the commit exactly when --check is 1,
#                                     unless SMATCHET_SKIP_REVIEW_GATE=1
#
# Runs against a REAL throwaway git repo (no stubs): the scripts read git diff
# output and the git index, so a real repo is the faithful fixture.

setup() {
    ROOT="$(git rev-parse --show-toplevel)"
    export ROOT
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    git init --quiet -b develop "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    # A repo-shaped fixture: the strict-zone list the gate reads, plus the hook
    # and the scripts under the paths the hook expects to find them at.
    mkdir -p "$REPO_TMP/scripts/git-hooks" \
        "$REPO_TMP/agents/scripts/core/lib" \
        "$REPO_TMP/Source/Core/src/Sync" \
        "$REPO_TMP/Source/Core/src/Ui" \
        "$REPO_TMP/docs"
    cp "$ROOT/scripts/git-hooks/pre-commit" "$REPO_TMP/scripts/git-hooks/pre-commit"
    cp "$ROOT/agents/scripts/core/review-ack.sh" "$REPO_TMP/agents/scripts/core/review-ack.sh"
    cp "$ROOT/agents/scripts/core/lib/review-ack.sh" "$REPO_TMP/agents/scripts/core/lib/review-ack.sh"
    chmod +x "$REPO_TMP/scripts/git-hooks/pre-commit"
    printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > "$REPO_TMP/project.config.json"
    echo "// base" > "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    echo "// base" > "$REPO_TMP/Source/Core/src/Ui/U.cpp"
    git -C "$REPO_TMP" add -A
    git -C "$REPO_TMP" commit --quiet -m base
    git -C "$REPO_TMP" config --local core.hooksPath scripts/git-hooks
}

teardown() {
    rm -rf "${REPO_TMP:-}"
}

# check — run the gate's --check inside the fixture repo.
check() {
    (cd "$REPO_TMP" && bash agents/scripts/core/review-ack.sh --check --staged 2>&1)
}

record() {
    (cd "$REPO_TMP" && bash agents/scripts/core/review-ack.sh --record --staged 2>&1)
}

# stage_lines <path> <n> — append n distinct C++ lines to <path> and stage it.
stage_lines() {
    local path="$1" n="$2" i
    for ((i = 0; i < n; i++)); do
        echo "int fn_${i}() { return ${i}; }" >> "$REPO_TMP/$path"
    done
    git -C "$REPO_TMP" add "$path"
}

commit_in_fixture() {
    (cd "$REPO_TMP" && git commit --quiet -m "$1" 2>&1)
}

@test "docs-only staged diff is not substantive" {
    echo note > "$REPO_TMP/docs/n.md"
    git -C "$REPO_TMP" add docs/n.md
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"N/A"* ]]
}

@test "sub-threshold non-strict C++ diff is not substantive" {
    stage_lines Source/Core/src/Ui/U.cpp 3
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"N/A"* ]]
}

@test "strict-zone touch is substantive and blocks without an ack" {
    stage_lines Source/Core/src/Sync/S.cpp 1
    run check
    [ "$status" -eq 1 ]
    [[ "$output" == *"strict-zone touch"* ]]
}

@test "large non-strict C++ diff is substantive and blocks without an ack" {
    stage_lines Source/Core/src/Ui/U.cpp 80
    run check
    [ "$status" -eq 1 ]
    [[ "$output" == *">= 60"* ]]
}

@test "recorded ack clears the gate" {
    stage_lines Source/Core/src/Sync/S.cpp 1
    record
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"ack current"* ]]
}

@test "a staged edit after the ack re-arms the gate" {
    stage_lines Source/Core/src/Sync/S.cpp 1
    record
    stage_lines Source/Core/src/Sync/S.cpp 1
    run check
    [ "$status" -eq 1 ]
    [[ "$output" == *"is stale"* ]]
}

@test "REVIEW_LINE_THRESHOLD tunes the substantive threshold" {
    stage_lines Source/Core/src/Ui/U.cpp 10
    run env REVIEW_LINE_THRESHOLD=5 bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --check --staged 2>&1"
    [ "$status" -eq 1 ]
}

@test "no working python fails CLOSED on a sub-threshold diff" {
    # #1116: the strict-zone list is unreadable without a working python, so the
    # gate must engage conservatively rather than N/A-pass a possible strict touch.
    stage_lines Source/Core/src/Ui/U.cpp 3
    run env SMATCHET_REVIEW_ACK_FORCE_NO_PY=1 bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --check --staged 2>&1"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no working python"* ]]
}

@test "a legacy bare-sha marker is migrated to the branch record" {
    printf '%064d\n' 0 > "$REPO_TMP/.review-ack"
    stage_lines Source/Core/src/Sync/S.cpp 1
    record
    run cat "$REPO_TMP/.review-ack"
    [ "$status" -eq 0 ]
    [[ "$output" == *"branch"* ]]
    [[ "$output" == *"staged"* ]]
}

@test "pre-commit refuses an unacked substantive commit" {
    stage_lines Source/Core/src/Sync/S.cpp 1
    run commit_in_fixture "feat: unreviewed"
    [ "$status" -ne 0 ]
    [[ "$output" == *"REFUSING a commit with no code review"* ]]
}

@test "pre-commit allows the commit once the ack is recorded" {
    stage_lines Source/Core/src/Sync/S.cpp 1
    record
    run commit_in_fixture "feat: reviewed"
    [ "$status" -eq 0 ]
}

@test "pre-commit allows a docs-only commit with no ack" {
    echo note > "$REPO_TMP/docs/n.md"
    git -C "$REPO_TMP" add docs/n.md
    run commit_in_fixture "docs: note"
    [ "$status" -eq 0 ]
}

@test "SMATCHET_SKIP_REVIEW_GATE bypasses the hook and says so" {
    stage_lines Source/Core/src/Sync/S.cpp 1
    run env SMATCHET_SKIP_REVIEW_GATE=1 bash -c "cd '$REPO_TMP' && git commit --quiet -m bypass 2>&1"
    [ "$status" -eq 0 ]
    [[ "$output" == *"bypassed"* ]]
}

@test "pre-commit exempts a conflicted merge commit" {
    # Catch-up sync (`git merge origin/develop`) stages the whole merged C++ diff.
    # Build a real conflicting merge so MERGE_HEAD is present at commit time.
    git -C "$REPO_TMP" checkout --quiet -b sibling
    stage_lines Source/Core/src/Sync/S.cpp 1
    (cd "$REPO_TMP" && bash agents/scripts/core/review-ack.sh --record --staged >/dev/null)
    git -C "$REPO_TMP" commit --quiet -m "sibling edit"
    git -C "$REPO_TMP" checkout --quiet develop
    echo "int conflicting() { return 9; }" >> "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" add Source/Core/src/Sync/S.cpp
    (cd "$REPO_TMP" && bash agents/scripts/core/review-ack.sh --record --staged >/dev/null)
    git -C "$REPO_TMP" commit --quiet -m "develop edit"
    run git -C "$REPO_TMP" merge sibling
    [ "$status" -ne 0 ]  # conflicted, as designed
    echo "int resolved() { return 0; }" > "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" add Source/Core/src/Sync/S.cpp
    rm -f "$REPO_TMP/.review-ack"
    run commit_in_fixture "merge sibling"
    [ "$status" -eq 0 ]
    [[ "$output" == *"MERGE_HEAD"* ]]
}

@test "pre-commit fails open when the gate script is missing" {
    rm -f "$REPO_TMP/agents/scripts/core/review-ack.sh"
    stage_lines Source/Core/src/Sync/S.cpp 1
    run commit_in_fixture "feat: no gate script"
    [ "$status" -eq 0 ]
}

@test "a missing library is infra (rc 2), not a review refusal" {
    # Regression: `set -e` turned a failed source into rc 1, which pre-commit
    # renders as "REFUSING a commit with no code review" — so a half-installed
    # checkout blocked even docs-only commits, blaming the author.
    rm -f "$REPO_TMP/agents/scripts/core/lib/review-ack.sh"
    run bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --check --staged 2>&1"
    [ "$status" -eq 2 ]
}

@test "pre-commit warns and allows when the library is missing" {
    rm -f "$REPO_TMP/agents/scripts/core/lib/review-ack.sh"
    echo note > "$REPO_TMP/docs/n.md"
    git -C "$REPO_TMP" add docs/n.md
    run commit_in_fixture "docs: note"
    [ "$status" -eq 0 ]
    [[ "$output" != *"REFUSING"* ]]
}

@test "--check rejects an unknown flag with rc 2" {
    run bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --check --bogus 2>&1"
    [ "$status" -eq 2 ]
}
