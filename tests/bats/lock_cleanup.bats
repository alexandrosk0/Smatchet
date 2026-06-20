#!/usr/bin/env bats
# tests/bats/lock_cleanup.bats
# ----------------------------------------------------------------------------
# Pins the .github/workflows/lock-cleanup.yml contract that the eager-seed
# adoption step (plan-lock-enforcement item 8) depends on, and the closed-
# unmerged relaxation (item 11):
#   - the `lock-slug: <slug>` parse regex (so the seed's PR-body line and the
#     cleanup parser can never drift),
#   - `holds-lock:` (stacked intermediates) is deliberately NOT matched,
#   - the release job no longer gates on `merged == true` (a closed-unmerged
#     PR with a lock-slug line releases its ref too).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    WF="$REPO_ROOT/.github/workflows/lock-cleanup.yml"
    # The single source of truth for the slug line, pinned here so a change to
    # the workflow regex (or the seed) without updating the other reds a test.
    EXPECTED_PAT='^[[:space:]]*lock-slug:[[:space:]]*[a-z0-9][a-z0-9-]{0,63}[[:space:]]*$'
}

@test "the cleanup workflow pins the exact lock-slug regex (seed must emit this)" {
    grep -qF "$EXPECTED_PAT" "$WF"
}

@test "the regex extracts the slug from a lock-slug line in a multi-line body" {
    run bash -c "printf 'intro\nlock-slug: my-slug-1\noutro\n' | grep -oE '$EXPECTED_PAT' | sed -E 's/^[[:space:]]*lock-slug:[[:space:]]*//; s/[[:space:]]*\$//'"
    [ "$status" -eq 0 ]
    [ "$output" = "my-slug-1" ]
}

@test "holds-lock (stacked-intermediate marker) is NOT matched" {
    run bash -c "printf 'holds-lock: my-slug\n' | grep -oE '$EXPECTED_PAT'"
    [ -z "$output" ]
}

@test "the release job no longer gates on merged==true (closed-unmerged releases)" {
    ! grep -qE "if:[[:space:]]*\\\$?\{?\{?[[:space:]]*github\.event\.pull_request\.merged[[:space:]]*==[[:space:]]*true" "$WF"
}

@test "the workflow still triggers on pull_request: closed" {
    grep -qE "types:[[:space:]]*\[closed\]" "$WF"
}

@test "ref existence check uses the SINGULAR GET endpoint (/git/ref/), not the removed plural" {
    # GET /git/refs/{ref} (plural) was removed by GitHub and 404s, which would
    # make the existence check always fail and silently skip every deletion.
    grep -qE 'git/ref/\$\{ref\}' "$WF"
    ! grep -qE 'gh api "repos/[^"]*/git/refs/\$\{ref\}"[[:space:]]*>/dev/null' "$WF"
}

@test "ref delete uses the PLURAL endpoint (/git/refs/)" {
    grep -qE '\-X DELETE "repos/[^"]*/git/refs/\$\{ref\}"' "$WF"
}
