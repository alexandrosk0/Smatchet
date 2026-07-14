#!/usr/bin/env bash
# plan-archival-owed.sh — detect plan-docs that shipped but were never moved out
# of docs/plans/active/, and nudge for the `git mv` active → shipped (the step
# that historically drops off the post-ship doc PR; see
# docs/agent-rules/process-rules.md § Archive on revision).
#
# A plan is ARCHIVE-OWED when its `> **Status**:` header is `shipped` while the
# file is still in docs/plans/active/. The explicit Status marker is the robust
# discriminator: a live multi-batch campaign keeps `Status: active` (it is never
# marked `shipped` until ALL slices land), so — unlike the rejected
# "impl-log-populated + cited-PRs-merged" heuristic, which false-positives on a
# live campaign mid-flight — this never flags a live plan. The all-cited-PRs-merged
# probe (gh, optional) is a secondary CONSISTENCY annotation, never the trigger.
#
# Modes:
#   --list     (default) plain "plan archival owed: <slug> — <why>" lines.
#   --nudge    SessionStart-formatted block (silent when nothing is owed).
#   --selftest run inline fixtures; assert classifier behaviour; exit 0/1.
#
# Sibling of postmortem-owed.sh / followup-due-nudge.sh. Advisory — never blocks.
# Exit 0 always (even without gh: the filesystem marker is self-sufficient).

set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --selftest) MODE="selftest" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: plan-archival-owed.sh [--list|--nudge|--selftest]" >&2; exit 2 ;;
esac

ACTIVE_DIR="${PLAN_ACTIVE_DIR:-docs/plans/active}"

# Resolve the target repo dynamically (no hardcoded slug — core-scripts-bash-07).
# $REPO overrides (test seam); else derive via gh. The PRIMARY scan is a
# filesystem check (Status marker in active/ plans) that needs no repo, so an
# unresolved slug does NOT block — it only disables the optional gh consistency
# note (GH_OK gates on a non-empty REPO below).
# shellcheck source=agents/scripts/core/lib/resolve-repo.sh
. agents/scripts/core/lib/resolve-repo.sh
REPO="$(resolve_repo || true)"

# is_shipped_marker <file> — true if the plan's Status header VALUE is `shipped`.
# Keys on the value immediately after `Status:` so a live plan whose prose merely
# mentions a shipped slice ("Status: active — shipped slice 1 only") is NOT caught,
# and the deferred-tier banner (`> **STATUS: DEFERRED ...**`) is NOT caught.
is_shipped_marker() {
    grep -iqE '^[[:space:]]*>?[[:space:]]*\*{0,2}status\*{0,2}[[:space:]]*:[[:space:]]*\*{0,2}shipped' "$1"
}

# cited_prs <file> — the `#<N>` PR numbers cited under § Implementation log
# (best-effort; used only for the optional secondary gh consistency check).
cited_prs() {
    awk '
        /^##[[:space:]]+Implementation log/ { inlog=1; next }
        /^##[[:space:]]/ && inlog { inlog=0 }
        inlog { print }
    ' "$1" | grep -oE '#[0-9]+' | tr -d '#' | sort -u
}

GH_OK=0
if [ -n "$REPO" ] && command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
    GH_OK=1
fi

# unmerged_note <file> — if gh is available, return a one-line warning when a
# cited PR is NOT in a MERGED state (an inconsistency: marked shipped but an
# Implementation-log PR is still open). Empty otherwise. Never the trigger.
unmerged_note() {
    [ "$GH_OK" = "1" ] || return 0
    local f="$1" pr state open=""
    for pr in $(cited_prs "$f"); do
        state="$(gh pr view "$pr" --repo "$REPO" --json state --jq .state 2>/dev/null || echo "")"
        case "$state" in
            ""|MERGED) : ;;
            *) open="${open:+$open,}#$pr($state)" ;;
        esac
    done
    if [ -n "$open" ]; then printf ' [check: cited %s not merged]' "$open"; fi
    return 0   # MUST return 0 — called in `owed+=("$(unmerged_note ...)")`; a
               # non-zero return would propagate to that assignment and trip set -e.
}

scan() {
    owed=()
    [ -d "$ACTIVE_DIR" ] || return 0
    local f base
    for f in "$ACTIVE_DIR"/*.md; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        [ "$base" = "_plan-template.md" ] && continue   # the template carries the example marker
        if is_shipped_marker "$f"; then
            owed+=("${base%.md}$(unmerged_note "$f")")
        fi
    done
}

# --- selftest ----------------------------------------------------------------
if [ "$MODE" = "selftest" ]; then
    fail=0
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    # Fixture dir kept free of the literal "docs/plans/active/<slug>.md" path so
    # test-plan-ref-integrity's git grep does not mistake these for real refs.
    act="$tmp/active"
    mkdir -p "$act"
    cat > "$act/done-a.md"       <<'EOF'
# Plan — done a
> **Status**: shipped — archived 2026-06-05; see § Implementation log.
EOF
    cat > "$act/done-b.md"       <<'EOF'
# Plan — done b
> **STATUS: SHIPPED** (legacy all-caps banner form)
EOF
    cat > "$act/live.md"         <<'EOF'
# Plan — live campaign
> **Status**: active — shipped slice 1 only; B5–B11 still live.
EOF
    cat > "$act/deferred.md"     <<'EOF'
# Plan — parked
> **STATUS: DEFERRED (pending demand)**
EOF
    cat > "$act/nostatus.md"     <<'EOF'
# Plan — old plan, no Status field
> **Slug**: `nostatus`.
EOF
    cat > "$act/_plan-template.md" <<'EOF'
# Plan — <feature>
> **Status**: shipped
EOF
    # Absolute fixture dir so the re-invoked script's own `cd` to repo-root does
    # not redirect the scan back at the real active/ tree. Run from repo-root
    # (current cwd post-`cd`) via the repo-relative script path.
    PLAN_ACTIVE_DIR="$act" REPO="x/y" \
        bash agents/scripts/core/plan-archival-owed.sh --list 2>/dev/null > "$tmp/out" || true
    assert_has()  { grep -q "plan archival owed: $1\b" "$tmp/out" || { echo "FAIL: expected $1 owed"; fail=1; }; }
    assert_miss() { grep -q "plan archival owed: $1\b" "$tmp/out" && { echo "FAIL: $1 should NOT be owed"; fail=1; } || true; }
    # selftest: asserts-failure — a shipped-but-unarchived plan fixture must be detected as owed (the gate's flag path).
    assert_has  "done-a"        # explicit shipped marker
    assert_has  "done-b"        # all-caps legacy form
    assert_miss "live"          # active value, shipped only in prose
    assert_miss "deferred"      # deferred banner, not shipped
    assert_miss "nostatus"      # no Status field → graceful skip
    assert_miss "_plan-template" # template excluded
    if [ "$fail" = "0" ]; then echo "plan-archival-owed --selftest: PASS (6/6)"; exit 0; fi
    echo "plan-archival-owed --selftest: FAIL"; exit 1
fi

scan

# --- Emit --------------------------------------------------------------------
if [ "${#owed[@]}" -eq 0 ]; then
    [ "$MODE" = "list" ] && echo "plan-archival-owed: no active plan marked shipped — nothing owed."
    exit 0
fi

if [ "$MODE" = "nudge" ]; then
    echo "## === plan archival owed (${#owed[@]}) ==="
    echo "Plan(s) marked \`Status: shipped\` but still in docs/plans/active/. Finish the"
    echo "archive (process-rules.md § Archive on revision): \`git mv\` active → shipped,"
    echo "sweep refs (\`grep -rln plans/active/<slug>\`), then \`test-plan-index.sh --fix\`:"
    for o in "${owed[@]}"; do echo "  - $o"; done
else
    for o in "${owed[@]}"; do echo "plan archival owed: $o"; done
fi
exit 0
