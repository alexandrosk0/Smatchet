#!/usr/bin/env bash
# historical-review-ledger-reconcile.sh — keep the historical-review priority
# list HONEST. Survivor batches in docs/self-improvement/historical-review-findings.md
# are point-in-time snapshots: ordinary merges fix findings without ever touching
# the ledger, so the "STILL OPEN" list rots stale (PR-8 incident:
# docs/self-improvement/categories/tooling/2026-06-20-historical-review-ledger-staleness.md
# — 11 of 14 priority findings were already fixed within ~1 week; acting on the
# list as-is would have chased already-done work).
#
# This runs the two existing dedup probes over each STILL-OPEN finding's cited PR
# and flags the ones now superseded:
#   * finding-already-fixed.sh <pr> — a CLOSED issue references the finding AND/OR
#     a fix token is present on origin/develop.
#   * historical-review-survivors.sh --pr <pr> — "FULLY SUPERSEDED" banner means
#     every line that PR introduced was since rewritten/removed at develop.
# A finding flagged by EITHER probe is "likely superseded — re-verify before
# acting / drop from the list."
#
# It does NOT auto-edit the ledger (preserves the "nudges emit to stdout, never
# write a tracked file" invariant + dodges concurrent-PR conflicts) — it prints a
# reconcile report; the orchestrator removes/annotates the superseded bullets via
# the normal PR flow.
#
# Modes:
#   --reconcile (default)  run both probes over every STILL-OPEN finding; print a
#                          per-finding LIKELY-SUPERSEDED | OPEN report. Exit 0.
#   --nudge                SessionStart freshness nudge: SILENT unless the ledger's
#                          newest dated `## ...` section is older than N days
#                          (LEDGER_STALE_DAYS, default 14) — then print a short
#                          "reconcile is overdue" block. Never blocks (exit 0).
#   --selftest             inline fixtures for the pure parsers; asserts a failure
#                          case; exit 0/1.
#
# gh/git unavailable → probes degrade to "OPEN (unverified)" and the nudge falls
# back to git-commit age; never a hard fail, never a false drop.
#
# Sibling of postmortem-owed.sh / plan-archival-owed.sh / followup-due-nudge.sh.
set -uo pipefail
cd "$(dirname "$0")/../../.." || exit 0

LEDGER="${HRL_LEDGER:-docs/self-improvement/historical-review-findings.md}"
STALE_DAYS="${LEDGER_STALE_DAYS:-14}"
TODAY="${HRL_TODAY:-$(date +%Y-%m-%d 2>/dev/null || echo 1970-01-01)}"
CORE_DIR="$(dirname "$0")"

MODE="reconcile"
case "${1:-}" in
    --reconcile|"") MODE="reconcile" ;;
    --nudge) MODE="nudge" ;;
    --selftest) MODE="selftest" ;;
    *) echo "usage: historical-review-ledger-reconcile.sh [--reconcile|--nudge|--selftest]" >&2; exit 2 ;;
esac

# --- pure parsers (testable; no gh/git) --------------------------------------

# open_findings <ledger> — print the cited PR number of each bullet inside the
# "STILL OPEN (NOT DONE)" block (between that bold header and the next bold
# header or blank-line-then-bold). One PR number per line (the bullet's FIRST
# `#<N>`, which is the finding's own PR — a trailing "→ Issue #1457" is ignored).
open_findings() {
    awk '
        /^\*\*STILL OPEN/         { inblk=1; next }
        inblk && /^\*\*[^*]/      { inblk=0 }      # next bold header ends the block
        inblk && /^- \*\*#[0-9]+/ {
            line=$0
            if (match(line, /#[0-9]+/)) {
                print substr(line, RSTART+1, RLENGTH-1)
            }
        }
    ' "$1"
}

# newest_section_date <ledger> — the most recent YYYY-MM-DD appearing in any
# `## ...` heading. Empty when none parse.
newest_section_date() {
    grep -E '^## ' "$1" 2>/dev/null \
        | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}' \
        | sort -r | head -1
}

# days_between <older-iso> <newer-iso> — integer day delta (newer-older), via
# `date -d`; empty when date math unavailable (caller falls back).
days_between() {
    local a b sa sb
    a="$1"; b="$2"
    sa="$(date -d "$a" +%s 2>/dev/null || true)"
    sb="$(date -d "$b" +%s 2>/dev/null || true)"
    { [ -n "$sa" ] && [ -n "$sb" ]; } || return 0
    echo $(( (sb - sa) / 86400 ))
}

# --- reconcile ---------------------------------------------------------------
run_reconcile() {
    [ -f "$LEDGER" ] || { echo "historical-review-ledger-reconcile: ledger not found: $LEDGER" >&2; return 2; }
    local prs pr verdict superseded=0 total=0
    prs="$(open_findings "$LEDGER")"
    if [ -z "$prs" ]; then
        echo "historical-review-ledger-reconcile: no STILL-OPEN findings parsed — nothing to reconcile."
        return 0
    fi
    echo "# Historical-review ledger reconcile — $(printf '%s' "$prs" | grep -c .) open finding(s)"
    echo "# ledger: $LEDGER   (LIKELY-SUPERSEDED = re-verify before acting / drop the bullet)"
    echo "#"
    for pr in $prs; do
        total=$((total + 1))
        verdict="OPEN"
        local tag=""
        # PRIMARY (precise): survivors. A FULLY SUPERSEDED banner means EVERY line
        # that PR introduced was since rewritten/removed at develop — the cited
        # file:line cannot still hold the original bug. This is the high-confidence
        # drop signal (git-blame attribution, no heuristic).
        if bash "$CORE_DIR/historical-review-survivors.sh" --pr "$pr" 2>/dev/null \
             | grep -q 'FULLY SUPERSEDED'; then
            verdict="LIKELY-SUPERSEDED (fully superseded at develop)"
        fi
        # SECONDARY (advisory tag only — deliberately NOT the verdict): the dedup
        # probe with the PR number as the fix token is LOOSE (a closed issue / a
        # develop commit merely MENTIONING the number trips it), so it over-fires;
        # it is shown as a "[dedup-hint]" annotation to prompt a manual look, never
        # as the supersede verdict. The precise survivors probe above owns the call.
        if bash "$CORE_DIR/finding-already-fixed.sh" "$pr" "#$pr" >/dev/null 2>&1; then
            tag=" [dedup-hint: closed-issue/develop mention — verify]"
        fi
        case "$verdict" in LIKELY-SUPERSEDED*) superseded=$((superseded + 1)) ;; esac
        printf '  #%-6s %s%s\n' "$pr" "$verdict" "$tag"
    done
    echo "#"
    echo "# Summary: $superseded of $total open finding(s) likely superseded — drop/annotate via PR."
    return 0
}

# --- nudge -------------------------------------------------------------------
run_nudge() {
    [ -f "$LEDGER" ] || exit 0     # silent if no ledger
    local newest age
    newest="$(newest_section_date "$LEDGER")"
    if [ -n "$newest" ]; then
        age="$(days_between "$newest" "$TODAY")"
    fi
    # Fallback: if section-date math is unavailable, use git-commit age of the file.
    if [ -z "${age:-}" ]; then
        local last now
        last="$(git log -1 --format='%ct' -- "$LEDGER" 2>/dev/null || true)"
        case "$last" in
            ''|*[!0-9]*) exit 0 ;;   # can't tell age → stay silent (no false nudge)
        esac
        now="$(date +%s 2>/dev/null || echo 0)"
        age=$(( (now - last) / 86400 ))
    fi
    [ "${age:-0}" -ge "$STALE_DAYS" ] || exit 0   # fresh → silent
    echo "## === historical-review ledger stale (${age}d) ==="
    echo "The historical-review findings ledger's newest dated section is ${age} days old"
    echo "(threshold ${STALE_DAYS}d). Open findings rot as ordinary merges fix them without"
    echo "touching the ledger. Reconcile before acting on the priority list:"
    echo "  bash agents/scripts/core/historical-review-ledger-reconcile.sh --reconcile"
    echo "  (drops/annotates findings now superseded on develop; ledger: $LEDGER)"
    exit 0
}

# --- selftest ----------------------------------------------------------------
run_selftest() {
    # tmp is intentionally NOT `local`: the EXIT trap that cleans it fires after
    # this function returns, when a function-local would be out of scope and
    # `set -u` would abort with "tmp: unbound variable".
    local fail=0
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    cat > "$tmp/ledger.md" <<'EOF'
# Historical code-review findings

## Verification pass (2026-06-20)
blah

**STILL OPEN (NOT DONE) — re-verified alive at develop 2026-06-20:**

_Product → Issues:_
- **#1138** (HIGH) `Foo.cpp:10` — race → **Issue #1457**.
- **#1158** (HIGH) `Bar.cpp:20` — latch → **Issue #1458**.

_Tooling backlog:_
- **#329** (HIGH) `baz.sh:30` — leak gate.

**Not individually re-verified this pass:** the rest.

## Batch 1 — PRs #947–951 (swept 2026-06-07)
- **#947** should NOT be parsed (outside STILL-OPEN block).
EOF
    # open_findings must extract exactly the STILL-OPEN PR numbers (first # per
    # bullet), ignoring the trailing Issue #s and the Batch-1 bullet.
    got="$(open_findings "$tmp/ledger.md" | tr '\n' ' ')"
    if [ "$got" != "1138 1158 329 " ]; then
        echo "FAIL: open_findings expected '1138 1158 329 ', got '$got'" >&2; fail=1
    fi
    # newest_section_date: latest date across ## headings.
    nd="$(newest_section_date "$tmp/ledger.md")"
    if [ "$nd" != "2026-06-20" ]; then
        echo "FAIL: newest_section_date expected 2026-06-20, got '$nd'" >&2; fail=1
    fi
    # days_between sanity (skip if date -d unavailable).
    if db="$(days_between 2026-06-01 2026-06-15)" && [ -n "$db" ]; then
        if [ "$db" != "14" ]; then
            echo "FAIL: days_between(2026-06-01,2026-06-15) expected 14, got '$db'" >&2; fail=1
        fi
    fi
    # selftest: asserts-failure — a ledger with NO STILL-OPEN block must yield an
    # EMPTY open-finding list (a regression that scrapes every `#N` bullet,
    # ignoring the block boundary, would wrongly return PRs and trip this).
    cat > "$tmp/noblock.md" <<'EOF'
# Findings
## Batch 1 — PRs #947–951
- **#947** (HIGH) some finding.
- **#948** (HIGH) another.
EOF
    leaked="$(open_findings "$tmp/noblock.md" | tr '\n' ' ')"
    if [ -n "$leaked" ]; then
        echo "FAIL: open_findings leaked findings with no STILL-OPEN block: '$leaked'" >&2; fail=1
    fi
    if [ "$fail" = "0" ]; then
        echo "historical-review-ledger-reconcile --selftest: PASS (open-findings block-scoping + section-date + days-between)"
        echo "Passed: 1  Failed: 0"
        return 0
    fi
    echo "Passed: 0  Failed: 1"
    return 1
}

case "$MODE" in
    reconcile) run_reconcile ;;
    nudge)     run_nudge ;;
    selftest)  run_selftest ;;
esac
