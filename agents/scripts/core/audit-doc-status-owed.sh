#!/usr/bin/env bash
# audit-doc-status-owed.sh — detect root *_AUDIT.md docs whose findings were
# remediated (their companion remediation plan is archived in
# docs/plans/shipped/) but whose per-finding status was never back-annotated,
# so the doc still presents every finding as open (see
# docs/agent-rules/process-rules.md § Audit-doc status sync).
#
# An audit doc is STATUS-OWED when BOTH hold:
#   * at least one docs/plans/shipped/*.md plan cites the audit doc by
#     filename (the companion-remediation signal), AND
#   * the audit doc itself carries no REMEDIATED / per-finding check marker.
# The marker heuristic is deliberately cheap: a case-insensitive `REMEDIATED`
# token or a `✅` check anywhere in the doc counts as annotated.
#
# Modes:
#   --list     (default) plain "audit-doc status owed: <file> — <why>" lines.
#   --nudge    SessionStart-formatted block (silent when nothing is owed).
#   --selftest run inline fixtures; assert classifier behaviour; exit 0/1.
#
# Sibling of plan-archival-owed.sh / postmortem-owed.sh. Advisory — never
# blocks. Exit 0 always on the scan paths (filesystem-only; no gh needed).

set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --selftest) MODE="selftest" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: audit-doc-status-owed.sh [--list|--nudge|--selftest]" >&2; exit 2 ;;
esac

AUDIT_ROOT="${AUDIT_ROOT:-.}"
SHIPPED_DIR="${SHIPPED_DIR:-docs/plans/shipped}"

# has_status_marker <file> — true when the audit doc carries a remediation
# annotation: a REMEDIATED token (any case) or a ✅ check marker.
has_status_marker() {
    grep -qiE 'REMEDIATED' "$1" || grep -q '✅' "$1"
}

# has_shipped_companion <basename> — true when any shipped plan cites the
# audit doc by filename (e.g. "SECURITY_AUDIT.md" appears in the plan text).
has_shipped_companion() {
    local base="$1" p
    [ -d "$SHIPPED_DIR" ] || return 1
    for p in "$SHIPPED_DIR"/*.md; do
        [ -e "$p" ] || continue
        if grep -qF "$base" "$p"; then return 0; fi
    done
    return 1
}

scan() {
    owed=()
    local f base
    for f in "$AUDIT_ROOT"/*_AUDIT.md; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        if has_shipped_companion "$base" && ! has_status_marker "$f"; then
            owed+=("$base — companion remediation plan shipped, no REMEDIATED/✅ marker")
        fi
    done
}

# --- selftest ----------------------------------------------------------------
if [ "$MODE" = "selftest" ]; then
    fail=0
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/root" "$tmp/shipped"
    # Fixture 1: shipped companion + no marker → OWED.
    printf '# Audit A\nFinding 1: bad thing.\n' > "$tmp/root/STALE_AUDIT.md"
    printf '# Plan\nRemediates STALE_AUDIT.md findings.\n' > "$tmp/shipped/stale-remediation.md"
    # Fixture 2: shipped companion + REMEDIATED banner → not owed.
    printf '# Audit B\n> REMEDIATED (verified)\n' > "$tmp/root/DONE_AUDIT.md"
    printf '# Plan\nRemediates DONE_AUDIT.md findings.\n' > "$tmp/shipped/done-remediation.md"
    # Fixture 3: no shipped companion → not owed (remediation still in flight).
    printf '# Audit C\nFinding 1: open.\n' > "$tmp/root/FRESH_AUDIT.md"
    AUDIT_ROOT="$tmp/root" SHIPPED_DIR="$tmp/shipped" \
        bash agents/scripts/core/audit-doc-status-owed.sh --list 2>/dev/null > "$tmp/out" || true
    assert_has()  { grep -q "audit-doc status owed: $1" "$tmp/out" || { echo "FAIL: expected $1 owed"; fail=1; }; }
    assert_miss() { if grep -q "audit-doc status owed: $1" "$tmp/out"; then echo "FAIL: $1 should NOT be owed"; fail=1; fi; }
    # selftest: asserts-failure — a shipped-companion + unmarked audit fixture must be flagged.
    assert_has  "STALE_AUDIT.md"
    assert_miss "DONE_AUDIT.md"
    assert_miss "FRESH_AUDIT.md"
    if [ "$fail" = "0" ]; then echo "audit-doc-status-owed --selftest: PASS (3/3)"; exit 0; fi
    echo "audit-doc-status-owed --selftest: FAIL"; exit 1
fi

scan

# --- Emit --------------------------------------------------------------------
if [ "${#owed[@]}" -eq 0 ]; then
    [ "$MODE" = "list" ] && echo "audit-doc-status-owed: every remediated root *_AUDIT.md carries a status marker — nothing owed."
    exit 0
fi

if [ "$MODE" = "nudge" ]; then
    echo "## === audit-doc status owed (${#owed[@]}) ==="
    echo "Root audit doc(s) whose companion remediation plan shipped but whose"
    echo "per-finding status was never back-annotated (process-rules.md § Audit-doc"
    echo "status sync): add the REMEDIATED banner / per-finding status table:"
    for o in "${owed[@]}"; do echo "  - $o"; done
else
    for o in "${owed[@]}"; do echo "audit-doc status owed: $o"; done
fi
exit 0
