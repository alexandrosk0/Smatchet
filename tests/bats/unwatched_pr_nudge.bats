#!/usr/bin/env bats
# tests/bats/unwatched_pr_nudge.bats
# ----------------------------------------------------------------------------
# Coverage for agents/scripts/core/unwatched-pr-nudge.sh.
#
# Why this nudge exists (infra 2026-08-16): a `send_later` check-in that fires
# while the remote container is suspended is recorded as delivered but does no
# work, and the ship-loop re-arms SILENTLY on a quiet tick — so a lost tick and
# a quiet tick are indistinguishable from outside. PR #2028 sat 11.5h with all
# CI green and no review requested, and nothing surfaced it. The nudge is
# absence-aware on purpose: it asks GitHub what is still open rather than
# trusting any signal from the process that died.
#
# The script's own --selftest covers the pure detector; these tests drive the
# END-TO-END modes through the injectable fixture, which is the part the
# selftest cannot reach.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    NUDGE="$REPO_ROOT/agents/scripts/core/unwatched-pr-nudge.sh"
    export NUDGE
    FIX="$BATS_TEST_TMPDIR/prs.tsv"
    export FIX
    export SMATCHET_UNWATCHED_PR_FIXTURE="$FIX"
    export SMATCHET_UNWATCHED_PR_NOW="2026-08-16T12:00:00Z"
    export SMATCHET_UNWATCHED_PR_STALE_SECONDS=7200
    # An inherited branch filter would silently change what the agent-branch and
    # human-branch cases below actually assert — run_selftest unsets it for the
    # same reason.
    unset SMATCHET_UNWATCHED_PR_BRANCH_RE
    # A working interpreter is required; skip cleanly when absent (mirrors the
    # other suites — the script itself degrades silent, which would make every
    # assertion here vacuous rather than failing).
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    [ -n "$PY" ] || skip "no working python interpreter"
}

row() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$FIX"; }

@test "the script's own --selftest passes" {
    run bash "$NUDGE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "--nudge names a quiet agent PR and says what to do about it" {
    row 2028 claude/backlog-review-w9h2n7 2026-08-16T00:30:00Z false
    run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]                       # advisory: never blocks SessionStart
    [[ "$output" == *"PR #2028"* ]]
    [[ "$output" == *"11h"* ]]
    # The nudge must carry the ACTION, not just the fact — the failure it exists
    # for looked green on every surface, so "re-poll before trusting" is the
    # whole point.
    [[ "$output" == *"re-arm"* ]] || [[ "$output" == *"Re-poll"* ]]
}

@test "--nudge is SILENT when every open PR is fresh" {
    row 2100 claude/x 2026-08-16T11:59:00Z false
    run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--nudge is SILENT with no open PRs at all" {
    : > "$FIX"
    run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# Absence of data must never read as "everything is stale": this runs on every
# SessionStart, including checkouts with no auth and no network. Simulated with
# a FAILING gh rather than by emptying PATH — an empty PATH loses bash itself
# (exit 127) and would assert nothing about this script.
@test "a failing gh degrades to silence, not to a false alarm" {
    unset SMATCHET_UNWATCHED_PR_FIXTURE
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/usr/bin/env bash\necho "gh: not authenticated" >&2\nexit 1\n' > "$BATS_TEST_TMPDIR/bin/gh"
    chmod +x "$BATS_TEST_TMPDIR/bin/gh"
    PATH="$BATS_TEST_TMPDIR/bin:$PATH" run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# The other half of the same property: gh absent entirely. Asserted directly on
# a machine that has no gh rather than by hand-building a minimal PATH — the
# hand-built PATH kept losing a coreutil the script legitimately needs (dirname
# first), which failed the script for the wrong reason and proved nothing.
@test "gh absent degrades to silence" {
    command -v gh >/dev/null 2>&1 && skip "gh present here; the failing-gh test covers the degrade property"
    unset SMATCHET_UNWATCHED_PR_FIXTURE
    run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "a draft PR is never reported (parked on purpose)" {
    row 2101 claude/x 2026-08-01T00:00:00Z true
    run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "a human branch is out of scope" {
    row 2102 feature/hand-written 2026-08-01T00:00:00Z false
    run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the branch filter is overridable" {
    row 2103 mybot/thing 2026-08-01T00:00:00Z false
    run bash "$NUDGE" --nudge
    [ -z "$output" ]
    SMATCHET_UNWATCHED_PR_BRANCH_RE='^mybot/' run bash "$NUDGE" --nudge
    [[ "$output" == *"PR #2103"* ]]
}

@test "SMATCHET_UNWATCHED_PR_STALE_SECONDS=0 disables the nudge" {
    row 2104 claude/x 2026-08-01T00:00:00Z false
    SMATCHET_UNWATCHED_PR_STALE_SECONDS=0 run bash "$NUDGE" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--list emits one machine-readable row per stale PR" {
    row 2028 claude/a 2026-08-16T00:30:00Z false
    row 2029 agent/b  2026-08-15T12:00:00Z false
    row 2030 claude/c 2026-08-16T11:59:00Z false   # fresh: must not appear
    run bash "$NUDGE" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"stale: PR #2028 — claude/a quiet 11h"* ]]
    [[ "$output" == *"stale: PR #2029 — agent/b quiet 24h"* ]]
    [[ "$output" != *"2030"* ]]
}

# A malformed row must not take the nudge down mid-SessionStart, and must not be
# guessed at either — an unparseable timestamp is "cannot judge", not "stale".
@test "a malformed row is skipped without failing the run" {
    printf 'not-a-number\tclaude/x\tgarbage\tfalse\n' >> "$FIX"
    printf '2105\tclaude/x\tnot-a-date\tfalse\n'      >> "$FIX"
    row 2028 claude/a 2026-08-16T00:30:00Z false
    run bash "$NUDGE" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #2028"* ]]
    [[ "$output" != *"2105"* ]]
}

# The threshold is operator-configurable, so a sub-hour quiet time must not
# render as `quiet 0h` — that drops the only number the line carries.
@test "a sub-hour quiet time reports minutes, not 0h" {
    row 2106 claude/x 2026-08-16T11:30:00Z false
    SMATCHET_UNWATCHED_PR_STALE_SECONDS=600 run bash "$NUDGE" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"quiet 30m"* ]]
    [[ "$output" != *"quiet 0h"* ]]
}

# An offset-less NOW parses to a naive datetime, and .timestamp() would read it
# as LOCAL time — so this asserts the same verdict under a non-UTC TZ.
@test "an offset-less NOW is read as UTC, not local time" {
    row 2107 claude/x 2026-08-16T00:30:00Z false
    SMATCHET_UNWATCHED_PR_NOW="2026-08-16T12:00:00" TZ="Asia/Tokyo" run bash "$NUDGE" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"stale: PR #2107 — claude/x quiet 11h"* ]]
}

@test "an unknown argument is an infra error, not a silent full run" {
    run bash "$NUDGE" --bogus
    [ "$status" -eq 2 ]
}

@test "the nudge is wired into the SessionStart hooks, not just defined" {
    grep -q 'unwatched-pr-nudge.sh' "$REPO_ROOT/docs/harness/claude-code/settings.json.tmpl"
    grep -q 'unwatched-pr-nudge.sh' "$REPO_ROOT/docs/harness/codex/hooks.json.tmpl"
}
