#!/usr/bin/env bats
# tests/bats/script_freshness.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/lib/script-freshness.sh — the shared
# gate-logic staleness detector (gate-tooling-run-from-stale-session-branch,
# process P1). Extracted from merge-gates.sh so pre-ship.sh and the other core
# gates inherit it; staleness in pre-ship fails in the WORSE direction (false
# GREENS — it passes a diff current develop would reject).
#
# Two layers are covered:
#   1. script_freshness_verdict — the fresh/stale/unverifiable classification,
#      driven through the positional blob-injection seam so no git/network is
#      needed, plus real-git cases in a throwaway repo.
#   2. warn_if_script_stale — the advisory wrapper's print/silence policy, and
#      its `set -e` safety (see below).
#
# THE `set -e` CASES ARE THE POINT, not padding. This helper is SOURCED into
# scripts that own their own shell options, several of which run `set -euo
# pipefail`. A bare `var="$(cmd)"` assignment propagates cmd's status, so a
# missing file would kill the calling gate outright. merge-gates.sh's inline
# original was accidentally immune (it used `local var="$(cmd)"`, and `local`'s
# own success masks the failure) — a mask the extraction loses. That regression
# was real: it took down pre-ship.sh's --selftest on first wiring. These tests
# pin the fix.
#
# Requires: bash, git, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    LIB="$REPO_ROOT/agents/scripts/core/lib/script-freshness.sh"
    export LIB
    [ -r "$LIB" ]
    TMP="$(mktemp -d)"
    export TMP
}

teardown() {
    # `if`, not `[ … ] && rm`: as the LAST command in teardown a false test makes
    # teardown itself exit non-zero, and bats reports that instead of the real
    # failure. When setup dies at the `[ -r "$LIB" ]` assertion above, TMP is
    # never set — exactly when the true cause matters most. Same last-command
    # exit-status trap the library documents at its `unverifiable` branch.
    if [ -n "${TMP:-}" ]; then
        rm -rf "$TMP"
    fi
    return 0
}

# ============================================================================
# script_freshness_verdict — classification via the injection seam
# ============================================================================

@test "verdict: identical fingerprints -> fresh" {
    run bash -c '. "$LIB"; script_freshness_verdict "aaa" "aaa" some/path.sh; echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "fresh" ]]
}

@test "verdict: differing fingerprints -> stale" {
    run bash -c '. "$LIB"; script_freshness_verdict "aaa" "bbb" some/path.sh; echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "stale" ]]
}

@test "verdict: empty dev-side fingerprint -> unverifiable (never 'stale')" {
    # An absent develop blob means we could not LOOK, which is a different
    # operator response from "the logic is out of date". Collapsing the two
    # would cry wolf on every offline run.
    run bash -c '. "$LIB"; script_freshness_verdict "aaa" "" some/path.sh; echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "unverifiable" ]]
}

@test "verdict: no relpaths declared -> unverifiable (nothing knowable)" {
    run bash -c '. "$LIB"; script_freshness_verdict "" ""; echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "unverifiable" ]]
}

@test "verdict: outside any git checkout -> unverifiable, not a crash" {
    run bash -c 'cd "$TMP"; . "$LIB"; script_freshness_verdict "" "" some/path.sh; echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "unverifiable" ]]
}

@test "verdict: real git repo with no origin remote -> unverifiable (fetch fails, no stale-compare)" {
    # A failed fetch must NOT fall through to comparing against whatever the local
    # origin/develop happens to be — that would silently measure against an
    # arbitrarily old ref and report a confident, meaningless verdict.
    (
        set -e
        cd "$TMP"; git init -q -b main .
        git config user.email t@l; git config user.name t
        echo x > file.sh; git add -A; git commit -qm base
    )
    run bash -c 'cd "$TMP"; . "$LIB"; script_freshness_verdict "" "" file.sh; echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "unverifiable" ]]
}

# ============================================================================
# Declared globs — expanded against the repo root, never the caller's CWD
# ============================================================================

# A repo with a REAL origin/develop, so fresh/stale is decidable rather than
# collapsing to `unverifiable` the way the no-remote cases above do. File-path
# remote: no network, deterministic.
_mk_repo_with_origin() {
    (
        set -e
        mkdir -p "$TMP/origin"
        git init -q --bare -b develop "$TMP/origin"
        git clone -q "$TMP/origin" "$TMP/work"
        cd "$TMP/work"
        git config user.email t@l; git config user.name t
        mkdir -p rules.d sub
        echo entry            > entry.sh
        echo a                > rules.d/10-a.sh
        echo b                > rules.d/20-b.sh
        echo keep             > sub/.keep
        git add -A; git commit -qm base
        git push -q origin HEAD:develop
        git fetch -q origin develop
    )
}

@test "glob: a declared pattern expands against the repo root, not the caller's CWD" {
    # The bug this pins: pre-ship.sh never cd's to the repo root, so an unquoted
    # pattern at the call site expanded against wherever the gate was invoked
    # from. From any subdirectory it matched nothing, bash passed the literal
    # pattern through, the fingerprint blanked, and the verdict degraded to
    # `unverifiable` — which is SILENT. "Safe to push" with the guard dead.
    _mk_repo_with_origin
    run bash -c 'cd "$TMP/work/sub"; . "$LIB"
        script_freshness_verdict "" "" entry.sh "rules.d/*.sh"
        echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "fresh" ]]
}

@test "glob: the same pattern from the repo root agrees with the subdirectory run" {
    # CWD-independence is the actual property; equality across both is the test.
    _mk_repo_with_origin
    run bash -c 'cd "$TMP/work"; . "$LIB"
        script_freshness_verdict "" "" entry.sh "rules.d/*.sh"
        echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "fresh" ]]
}

@test "glob: an edit under the pattern is SEEN from a subdirectory (not silently dropped)" {
    # Proves the glob's members are really fingerprinted after expansion, rather
    # than the run passing for some unrelated reason.
    _mk_repo_with_origin
    echo drifted > "$TMP/work/rules.d/20-b.sh"
    run bash -c 'cd "$TMP/work/sub"; . "$LIB"
        script_freshness_verdict "" "" entry.sh "rules.d/*.sh"
        echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "stale" ]]
}

@test "glob: a pattern matching NOTHING is unverifiable, never a silently smaller set" {
    # The hole one level up: dropping an unmatched pattern would shrink the
    # declared set and let the remaining files compare clean — a false 'fresh'.
    _mk_repo_with_origin
    run bash -c 'cd "$TMP/work"; . "$LIB"
        script_freshness_verdict "" "" entry.sh "no-such-dir/*.sh"
        echo "$SCRIPT_FRESHNESS_VERDICT"'
    [ "$status" -eq 0 ]
    [[ "$output" == "unverifiable" ]]
}

@test "glob: expansion survives set -euo pipefail (no unmatched-glob crash)" {
    _mk_repo_with_origin
    run bash -c 'set -euo pipefail; cd "$TMP/work"; . "$LIB"
        script_freshness_verdict "" "" "no-such-dir/*.sh"
        echo "verdict=$SCRIPT_FRESHNESS_VERDICT"
        echo SURVIVED'
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict=unverifiable"* ]]
    [[ "$output" == *"SURVIVED"* ]]
}

# ============================================================================
# `set -e` safety — the regression that broke pre-ship.sh --selftest
# ============================================================================

@test "set -e: a MISSING declared file degrades to unverifiable, does not kill the caller" {
    run bash -c 'set -euo pipefail; . "$LIB"
        script_freshness_verdict "" "" no/such/file.sh
        echo "verdict=$SCRIPT_FRESHNESS_VERDICT"
        echo "SURVIVED"'
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict=unverifiable"* ]]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "set -e: warn_if_script_stale on a missing file returns 0 and the caller continues" {
    run bash -c 'set -euo pipefail; . "$LIB"
        warn_if_script_stale "some gate" no/such/file.sh
        echo "SURVIVED"'
    [ "$status" -eq 0 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "set -e: warn_if_script_stale outside a git checkout does not kill the caller" {
    run bash -c 'set -euo pipefail; cd "$TMP"; . "$LIB"
        warn_if_script_stale "some gate" any/path.sh
        echo "SURVIVED"'
    [ "$status" -eq 0 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "set -u: sourcing and calling with no optional env set is clean" {
    run bash -c 'set -euo pipefail; . "$LIB"
        script_freshness_verdict "aaa" "aaa" p.sh
        warn_if_script_stale "g" p.sh
        echo "SURVIVED"'
    [ "$status" -eq 0 ]
    [[ "$output" == *"SURVIVED"* ]]
}

# ============================================================================
# warn_if_script_stale — print / silence policy
# ============================================================================

@test "warn: stale prints a caveat naming the out-of-date-logic risk" {
    # Real-git stale case: a tracked file whose on-disk content differs from
    # origin/develop. Uses the repo under test (which has an origin) and dirties
    # a file in place is not safe here, so drive it through the seam instead and
    # assert the wrapper's own message path via a stale verdict.
    run bash -c '. "$LIB"
        script_freshness_verdict() { SCRIPT_FRESHNESS_VERDICT=stale; }
        warn_if_script_stale "pre-ship gate logic" p.sh'
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"pre-ship gate logic"* ]]
    [[ "$output" == *"differs from origin/develop"* ]]
    [[ "$output" == *"OUT-OF-DATE"* ]]
}

@test "warn: fresh is completely silent (no noise on the common path)" {
    run bash -c '. "$LIB"
        script_freshness_verdict() { SCRIPT_FRESHNESS_VERDICT=fresh; }
        warn_if_script_stale "pre-ship gate logic" p.sh'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "warn: unverifiable is silent by default (offline is the common local case)" {
    run bash -c '. "$LIB"
        script_freshness_verdict() { SCRIPT_FRESHNESS_VERDICT=unverifiable; }
        warn_if_script_stale "pre-ship gate logic" p.sh'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "warn: unverifiable prints under SCRIPT_FRESHNESS_VERBOSE=1" {
    run bash -c '. "$LIB"
        script_freshness_verdict() { SCRIPT_FRESHNESS_VERDICT=unverifiable; }
        SCRIPT_FRESHNESS_VERBOSE=1 warn_if_script_stale "pre-ship gate logic" p.sh'
    [ "$status" -eq 0 ]
    [[ "$output" == *"freshness unverifiable"* ]]
}

# ============================================================================
# Wiring — the callers actually declare a set that includes the detector itself
# ============================================================================

@test "wiring: merge-gates.sh sources the lib and fingerprints it" {
    grep -q 'lib/script-freshness.sh' "$REPO_ROOT/agents/scripts/core/merge-gates.sh"
    # The detector must be inside its own declared set: a stale copy of it is the
    # one blind spot that would hide every other file's staleness.
    grep -q '"agents/scripts/core/lib/script-freshness.sh"' "$REPO_ROOT/agents/scripts/core/merge-gates.sh"
}

@test "wiring: pre-ship.sh passes its rules glob QUOTED, so the lib expands it" {
    # Without the quotes the shell expands the pattern at the call site against
    # the invoking CWD and the root-relative expansion in the lib never runs —
    # the fix is inert and the silent-degrade returns. Cheap to un-fix by
    # "tidying" the quotes away, so pin it.
    run grep -n 'lint-rules\.d' "$REPO_ROOT/scripts/dev/pre-ship.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"agents/scripts/project/lint-rules.d/*.sh"'* ]]
}

@test "wiring: pre-ship.sh's declared non-glob relpaths all resolve under the repo root" {
    # A typo'd or relocated declared path fingerprints as missing and degrades the
    # whole check to `unverifiable`, silently. Assert every literal one exists.
    local rel
    for rel in scripts/dev/pre-ship.sh \
               agents/scripts/project/test-lint-rules.sh \
               agents/scripts/core/lib/review-ack.sh \
               agents/scripts/core/lib/script-freshness.sh; do
        grep -q "\"$rel\"" "$REPO_ROOT/scripts/dev/pre-ship.sh"
        [ -e "$REPO_ROOT/$rel" ]
    done
    # …and the glob resolves to a non-empty set.
    run bash -c 'set -- "$REPO_ROOT"/agents/scripts/project/lint-rules.d/*.sh; echo "$#"'
    [ "$status" -eq 0 ]
    [ "$output" -gt 0 ]
}

@test "wiring: pre-ship.sh warns before its PASS line, not at startup" {
    # A caveat printed ahead of minutes of gate output has scrolled away by the
    # time the verdict lands — and the verdict is what it qualifies.
    local warn_line pass_line
    warn_line="$(grep -n 'warn_if_script_stale' "$REPO_ROOT/scripts/dev/pre-ship.sh" | tail -1 | cut -d: -f1)"
    pass_line="$(grep -n 'pre-ship: PASS — formatted' "$REPO_ROOT/scripts/dev/pre-ship.sh" | tail -1 | cut -d: -f1)"
    [ -n "$warn_line" ]
    [ -n "$pass_line" ]
    [ "$warn_line" -lt "$pass_line" ]
}
