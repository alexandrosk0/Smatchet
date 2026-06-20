#!/usr/bin/env bash
# test-plan-staleness.sh — ADVISORY WARN gate: flag an `active/` plan whose cited
# PRs are ALL merged but whose post-ship sections are still the template stub.
#
# This is the "plan-doc-postship-closeout-stale-active-gate" (PR-8). It is the
# HEURISTIC complement to agents/scripts/core/plan-archival-owed.sh, NOT a
# duplicate — the split is deliberate:
#
#   * plan-archival-owed.sh triggers ONLY on the explicit `Status: shipped`
#     marker (its header rejects the "impl-log-populated + cited-PRs-merged"
#     heuristic precisely because it false-positives on a live multi-batch
#     campaign mid-flight). It nags to finish the `git mv` active -> shipped.
#
#   * THIS gate catches the EARLIER drift it cannot: a plan still marked
#     `Status: active` (or no Status) whose work has actually landed (every cited
#     PR MERGED) yet whose `## Implementation log` / `## Deviations` /
#     `## Verification (actual)` are still the verbatim `*(populated post-ship)*`
#     template stub. That is "shipped but never written up" — the author never
#     even reached the point of flipping Status to shipped. Advisory WARN only:
#     a live campaign with some-but-not-all PRs merged is NOT flagged (the
#     all-merged condition guards against the campaign false-positive), and the
#     gate NEVER blocks (always exits 0 in --check; the WARN is informational).
#
# We do NOT re-implement the Status: shipped check here — that stays owned by
# plan-archival-owed.sh. The two are disjoint: a plan this gate WARNs on is
# active+stub; a plan plan-archival-owed.sh nags on is shipped-marked+unmoved.
#
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob (lives under
# the agents/scripts/project TEST_ROOTS dir). Bucket A (CLI), zero manual steps.
#
# Modes:
#   (default) / --check   scan docs/plans/active/*.md; print WARN lines for each
#                         stale plan; ALWAYS exit 0 (advisory). Emits the
#                         canonical `Passed: N  Failed: 0` line test-all.sh greps.
#   --warn                same as --check but emits a SessionStart-style block
#                         (silent when nothing stale) — for ad-hoc nudge use.
#   --selftest            inline fixtures; assert classifier behaviour; exit 0/1.
#
# gh is OPTIONAL: when absent/unauthenticated the cited-PR-state probe is skipped
# and NO plan is flagged (fail-safe — never a false WARN without PR evidence).
set -uo pipefail
cd "$(dirname "$0")/../../.." || exit 0

MODE="check"
case "${1:-}" in
    --check|"") MODE="check" ;;
    --warn) MODE="warn" ;;
    --selftest) MODE="selftest" ;;
    *) echo "usage: test-plan-staleness.sh [--check|--warn|--selftest]" >&2; exit 2 ;;
esac

ACTIVE_DIR="${PLAN_ACTIVE_DIR:-docs/plans/active}"
REPO="${REPO:-alexandrosk0/Smatchet}"

# The verbatim template stub each post-ship section opens with. A populated
# section replaces this line, so its continued presence == "never written up".
STUB_MARK='*(populated post-ship'

# Post-ship section headings that must be populated once work lands. Parens in
# "Verification (actual)" are matched literally — awk ERE needs them backslash-
# free (an escaped `\(` warns + is treated as a literal `(` anyway), so we use a
# `[(]` char-class which is unambiguous in both awk and grep ERE.
POSTSHIP_HEADINGS='^##[[:space:]]+(Implementation log|Deviations from plan|Verification [(]actual[)])'

# cited_prs <file> — `#<N>` PR numbers under § Implementation log (best-effort).
cited_prs() {
    awk '
        /^##[[:space:]]+Implementation log/ { inlog=1; next }
        /^##[[:space:]]/ && inlog { inlog=0 }
        inlog { print }
    ' "$1" | grep -oE '#[0-9]+' | tr -d '#' | sort -u
}

# all_postship_stub <file> — true iff EVERY post-ship section is still the stub
# (none populated). A section is "stub" when the line immediately following its
# heading still contains STUB_MARK.
all_postship_stub() {
    awk -v stub="$STUB_MARK" -v heads="$POSTSHIP_HEADINGS" '
        $0 ~ heads { expect=1; seen++; next }
        expect==1 {
            # first non-blank line after a post-ship heading
            if ($0 ~ /^[[:space:]]*$/) next
            if (index($0, stub) == 0) { populated=1 }
            expect=0
        }
        END { exit (seen>0 && populated==0) ? 0 : 1 }
    ' "$1"
}

# pr_state <num> — MERGED|OPEN|CLOSED|"" (empty when gh unavailable / errored).
GH_OK=0
if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then GH_OK=1; fi
pr_state() {
    [ "$GH_OK" = "1" ] || { echo ""; return 0; }
    gh pr view "$1" --repo "$REPO" --json state --jq .state 2>/dev/null || echo ""
}

# is_stale <file> — true iff (a) at least one cited PR, (b) gh available and
# EVERY cited PR is MERGED, (c) all post-ship sections still stub. Without gh,
# never stale (fail-safe). A single non-merged cited PR (live campaign) → not stale.
is_stale() {
    local f="$1" prs pr st
    prs="$(cited_prs "$f")"
    [ -n "$prs" ] || return 1                 # no cited PR → nothing to judge
    [ "$GH_OK" = "1" ] || return 1            # no PR evidence → fail-safe skip
    for pr in $prs; do
        st="$(pr_state "$pr")"
        [ "$st" = "MERGED" ] || return 1      # any non-merged → live campaign
    done
    all_postship_stub "$f" || return 1        # already written up → not stale
    return 0
}

scan() {
    stale=()
    [ -d "$ACTIVE_DIR" ] || return 0
    local f base
    for f in "$ACTIVE_DIR"/*.md; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        [ "$base" = "_plan-template.md" ] && continue
        if is_stale "$f"; then stale+=("${base%.md}"); fi
    done
}

# --- selftest ----------------------------------------------------------------
if [ "$MODE" = "selftest" ]; then
    fail=0
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

    # Unit-test the two pure classifiers directly (no gh dependency).
    # all_postship_stub: all three sections still stub -> exit 0 (stale).
    cat > "$tmp/allstub.md" <<'EOF'
# Plan
## Implementation log
*(populated post-ship per AGENTS.md ...)*
## Deviations from plan
*(populated post-ship ...)*
## Verification (actual)
*(populated post-ship ...)*
EOF
    # one section populated -> exit 1 (not all stub).
    cat > "$tmp/onepop.md" <<'EOF'
# Plan
## Implementation log
- abc1234 · landed the thing
## Deviations from plan
*(populated post-ship ...)*
## Verification (actual)
*(populated post-ship ...)*
EOF
    # selftest: asserts-failure — the all-stub fixture MUST classify as stub
    # (exit 0); a regression that fails to detect the stub state would break this.
    if ! all_postship_stub "$tmp/allstub.md"; then
        echo "FAIL: all-stub plan not detected as stub" >&2; fail=1
    fi
    if all_postship_stub "$tmp/onepop.md"; then
        echo "FAIL: a populated section wrongly counted as all-stub" >&2; fail=1
    fi

    # cited_prs extraction.
    cat > "$tmp/cited.md" <<'EOF'
# Plan
## Implementation log
- shipped via #101 and #102
## Approach
not in log: #999
EOF
    got="$(cited_prs "$tmp/cited.md" | tr '\n' ' ')"
    if [ "$got" != "101 102 " ]; then
        echo "FAIL: cited_prs expected '101 102 ', got '$got'" >&2; fail=1
    fi

    # is_stale fail-safe: with gh forced off, an all-stub all-cited plan is NOT
    # flagged (no PR evidence) — guards against false WARN.
    GH_OK=0
    if is_stale "$tmp/allstub_cited.md" 2>/dev/null; then :; fi  # file absent -> false anyway
    cat > "$tmp/stale_nogh.md" <<'EOF'
# Plan
## Implementation log
- shipped via #101
*(populated post-ship ...)*
## Deviations from plan
*(populated post-ship ...)*
## Verification (actual)
*(populated post-ship ...)*
EOF
    if GH_OK=0 is_stale "$tmp/stale_nogh.md"; then
        echo "FAIL: stale flagged with gh unavailable (should fail-safe)" >&2; fail=1
    fi

    if [ "$fail" = "0" ]; then
        echo "test-plan-staleness --selftest: PASS (stub-classifier + cited-prs + gh fail-safe)"
        echo "Passed: 1  Failed: 0"
        exit 0
    fi
    echo "Passed: 0  Failed: 1"
    exit 1
fi

scan

# --- emit (advisory; never blocks) -------------------------------------------
if [ "$MODE" = "warn" ]; then
    if [ "${#stale[@]}" -gt 0 ]; then
        echo "## === plan post-ship closeout stale (${#stale[@]}) ==="
        echo "Active plan(s) whose cited PRs are ALL merged but whose post-ship sections"
        echo "(§ Implementation log / Deviations / Verification (actual)) are still the"
        echo "template stub. Write them up, flip Status -> shipped, then archive"
        echo "(plan-archival-owed.sh takes over the git mv from there):"
        for s in "${stale[@]}"; do echo "  - $s"; done
    fi
    exit 0
fi

# --check / default
if [ "${#stale[@]}" -gt 0 ]; then
    echo "WARN: ${#stale[@]} active plan(s) shipped (all cited PRs merged) but post-ship sections still stub:" >&2
    for s in "${stale[@]}"; do echo "  - $s (write up § Implementation log / Deviations / Verification (actual))" >&2; done
    echo "WARN (advisory): plan-doc post-ship closeout owed — see test-plan-staleness.sh --warn." >&2
fi
# Advisory: ALWAYS report Failed: 0 so test-all.sh stays green. The WARN above is
# informational; this gate intentionally never reds the suite.
echo "Passed: 1  Failed: 0"
exit 0
