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
    TEMPLATE="$REPO_ROOT/.github/pull_request_template.md"
    OUT_DIR="$(mktemp -d)"
}

teardown() {
    rm -rf "${OUT_DIR:-}"
}

# parse_step_script — the `Parse lock-slug from PR body` step's run: block,
# de-indented, with its one `${{ }}` expression stubbed. Running the workflow's
# own text keeps these tests from passing against a private copy of the logic.
parse_step_script() {
    awk '
        /^      - name: Parse lock-slug from PR body[[:space:]]*$/ { instep = 1; next }
        instep && /^      - name:/ { exit }
        instep && /^        run: \|[[:space:]]*$/ { inrun = 1; next }
        inrun {
            if ($0 ~ /^[[:space:]]*$/) { print ""; next }
            if ($0 !~ /^          /) exit
            print substr($0, 11)
        }
    ' "$WF" | sed 's/\${{ github\.event\.pull_request\.number }}/123/g'
}

# run_parse <body> — run the parse step with PR_BODY=<body>; the step's
# GITHUB_OUTPUT lands in $OUT_DIR/out.
run_parse() {
    local script
    script="$(parse_step_script)"
    : > "$OUT_DIR/out"
    run env PR_BODY="$1" GITHUB_OUTPUT="$OUT_DIR/out" bash -c "$script"
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

# ---------- branch-keyed release (lock-release-on-close.sh) ----------
# The body marker is mutable and easy to omit; the claim.json `branch` field is
# not. The script is agent-layer content; its behaviour is covered by the layer's
# lock_release_on_close.bats — these pin only that the workflow calls it, with the
# inputs its guards need.

@test "the release script is reached through the agent-layer submodule" {
    grep -qE '^[[:space:]]*AGENT_LAYER_ROOT:[[:space:]]*agent-layer[[:space:]]*$' "$WF"
    grep -qE '^[[:space:]]*submodules:[[:space:]]*recursive[[:space:]]*$' "$WF"
    grep -qE 'bash "\$AGENT_LAYER_ROOT/agents/scripts/core/lock-release-on-close\.sh"' "$WF"
}

@test "the workflow releases by claim.json branch via lock-release-on-close.sh --branch" {
    grep -qE 'lock-release-on-close\.sh" --branch "\$HEAD_REF"' "$WF"
}

@test "the branch-match step runs AFTER the body-marker delete step" {
    local marker_line branch_line
    marker_line="$(grep -nE 'name: Delete refs/locks/<slug> if present' "$WF" | cut -d: -f1)"
    branch_line="$(grep -nE 'lock-release-on-close\.sh" --branch' "$WF" | cut -d: -f1)"
    [ -n "$marker_line" ]
    [ -n "$branch_line" ]
    [ "$branch_line" -gt "$marker_line" ]
}

@test "the branch-match step gets head ref, head repo, base repo and body through env" {
    grep -qE 'HEAD_REF:[[:space:]]*\$\{\{[[:space:]]*github\.event\.pull_request\.head\.ref[[:space:]]*\}\}' "$WF"
    grep -qE 'HEAD_REPO:[[:space:]]*\$\{\{[[:space:]]*github\.event\.pull_request\.head\.repo\.full_name[[:space:]]*\}\}' "$WF"
    grep -qE 'BASE_REPO:[[:space:]]*\$\{\{[[:space:]]*github\.repository[[:space:]]*\}\}' "$WF"
    grep -qE 'PR_BODY:[[:space:]]*\$\{\{[[:space:]]*github\.event\.pull_request\.body[[:space:]]*\}\}' "$WF"
}

@test "the release script is checked out from develop, never the PR's ref" {
    grep -qE '^[[:space:]]*ref:[[:space:]]*develop[[:space:]]*$' "$WF"
    run grep -E '^[[:space:]]*ref:[[:space:]]*\$\{\{' "$WF"
    [ -z "$output" ]
}

@test "the workflow keeps contents: write (both release paths delete refs)" {
    grep -qE '^[[:space:]]*contents:[[:space:]]*write' "$WF"
}

# ---------- active-work guard + base-branch filter ----------

@test "the workflow fires only for PRs into develop (a stacked intermediate's close keeps the lock)" {
    grep -qE '^[[:space:]]*branches:[[:space:]]*\[develop\][[:space:]]*$' "$WF"
}

@test "an open-PR guard step runs the script's --check-open into GITHUB_OUTPUT" {
    grep -qE 'lock-release-on-close\.sh" --check-open "\$HEAD_REF" >> "\$GITHUB_OUTPUT"' "$WF"
    grep -qE '^[[:space:]]*id:[[:space:]]*active[[:space:]]*$' "$WF"
    grep -qE '^[[:space:]]*pull-requests:[[:space:]]*read' "$WF"
}

@test "the body-marker delete is gated on the open-PR guard (both release paths respect live work)" {
    grep -qE "^[[:space:]]*if:[[:space:]]*steps\.parse\.outputs\.slug != '' && steps\.active\.outputs\.active == 'false'[[:space:]]*$" "$WF"
    local guard_line delete_line
    guard_line="$(grep -nE 'id:[[:space:]]*active' "$WF" | cut -d: -f1)"
    delete_line="$(grep -nE 'name: Delete refs/locks/<slug> if present' "$WF" | cut -d: -f1)"
    [ "$guard_line" -lt "$delete_line" ]
}

# ---------- commented-out marker warning (parse step, run as written) ----------
# A `lock-slug:` left inside the template's `<!-- -->` never matches the
# anchored regex and renders as nothing, so the parse step names it with a
# ::warning:: (not an error: the branch-match step still releases the head
# branch's locks). The template's own placeholder slug must stay silent.

@test "the parse step's run: block is extractable from the workflow" {
    run parse_step_script
    [ "$status" -eq 0 ]
    [[ "$output" == *'GITHUB_OUTPUT'* ]]
    [[ "$output" == *'lock-slug:'* ]]
}

@test "a bare lock-slug line parses to slug=<slug> with no warning" {
    run_parse "$(printf '## Intent\n\nShip it.\n\nlock-slug: armed-slug\n')"
    [ "$status" -eq 0 ]
    grep -qx 'slug=armed-slug' "$OUT_DIR/out"
    [[ "$output" != *'::warning::'* ]]
}

@test "a commented-out real slug warns and names it, without failing the run" {
    run_parse "$(printf '## Intent\n\nShip it.\n\n<!-- lock-slug: forgot-to-arm -->\n')"
    [ "$status" -eq 0 ]
    [[ "$output" == *'::warning::'*'forgot-to-arm'* ]]
    grep -qx 'slug=' "$OUT_DIR/out"
}

@test "the commented template placeholder stays silent" {
    run_parse "$(printf '## Intent\n\nShip it.\n\n<!-- lock-slug: your-slug-here -->\n')"
    [ "$status" -eq 0 ]
    [[ "$output" != *'::warning::'* ]]
    grep -qx 'slug=' "$OUT_DIR/out"
}

@test "the unedited PR template body stays silent" {
    run_parse "$(cat "$TEMPLATE")"
    [ "$status" -eq 0 ]
    [[ "$output" != *'::warning::'* ]]
    grep -qx 'slug=' "$OUT_DIR/out"
}

@test "a bare line wins over a commented one (no warning)" {
    run_parse "$(printf '<!-- lock-slug: your-slug-here -->\n<!-- lock-slug: old-slug -->\nlock-slug: real-slug\n')"
    [ "$status" -eq 0 ]
    grep -qx 'slug=real-slug' "$OUT_DIR/out"
    [[ "$output" != *'::warning::'* ]]
}

@test "the template keeps both markers commented on one line each (parser contract)" {
    # A marker on its own line inside a multi-line comment would MATCH the
    # anchored regex (lock-slug) or switch off the branch-match release on every
    # PR (holds-lock). Both must stay single-line `<!-- … -->`.
    grep -qxF '<!-- lock-slug: your-slug-here -->' "$TEMPLATE"
    grep -qE '^<!-- holds-lock: your-slug-here .*-->$' "$TEMPLATE"
    run grep -E '^[[:space:]]*(lock-slug|holds-lock):' "$TEMPLATE"
    [ -z "$output" ]
}
