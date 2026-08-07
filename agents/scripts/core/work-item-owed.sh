#!/usr/bin/env bash
# work-item-owed.sh — detect open work items (docs/work/items/NN-slug/) and
# nudge at SessionStart with each item's inferred next step, so an open item
# never silently stalls across sessions (docs/agent-rules/work-items.md — at
# most one item is in active development; the oldest open item finishes first).
#
# An item is OPEN when its folder exists under docs/work/items/. The next step
# is inferred from the artifact sequence present in the folder (1- … 5-,
# resolutions per docs/agent-rules/review-panels.md naming). The nudge is a
# reminder with a pointer, not a judgement: the two user-sign-off gates
# (ship-loops.md exception 7) still pause where they pause — this script only
# surfaces that the item exists and where it sits.
#
# Modes:
#   --list     (default) plain "work item open: NN-slug — next: <step>" lines.
#   --nudge    SessionStart-formatted block (silent when no item is open).
#   --selftest run inline fixtures; assert the stage classifier; exit 0/1.
#
# Sibling of plan-archival-owed.sh / postmortem-owed.sh. Advisory — never
# blocks. Exit 0 always in --list/--nudge (pure filesystem scan, no gh).

set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --selftest) MODE="selftest" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: work-item-owed.sh [--list|--nudge|--selftest]" >&2; exit 2 ;;
esac

ITEMS_DIR="${WORK_ITEMS_DIR:-docs/work/items}"

# has_file <dir> <glob> — true if the item folder holds a match for the glob.
# find(1) rather than an unquoted-variable glob keeps shellcheck's SC2086
# fail-set clean while still matching the pattern.
has_file() {
    local d="$1" g="$2"
    find "$d" -maxdepth 1 -name "$g" -print -quit 2>/dev/null | grep -q .
}

# next_step <dir> — classify the item's next step from the artifact sequence
# present. Walked BACKWARDS (latest artifact wins): a folder holding 3-plan.md
# and 4-review-* is past authoring and inside the pre gate. The two
# "user sign-off" steps are exactly the two gates of ship-loops.md exception 7.
next_step() {
    local d="$1"
    if has_file "$d" "5-resolution-*.md"; then
        echo "retro + close (drain Spawned.md, collapse to docs/work/closed/ — close-work-item skill)"
    elif has_file "$d" "5-review-*.md"; then
        echo "resolve the post-implementation review (address-review-feedback skill)"
    elif has_file "$d" "4-resolution-*.md"; then
        echo "user sign-off on the pre gate, then implementation"
    elif has_file "$d" "4-review-*.md"; then
        echo "resolve the pre-implementation review (address-review-feedback skill)"
    elif has_file "$d" "3-plan.md"; then
        echo "pre-implementation review (run-review.sh --gate pre)"
    elif has_file "$d" "2-design.md"; then
        echo "author 3-plan.md"
    elif has_file "$d" "1-specification.md"; then
        echo "author 2-design.md"
    else
        echo "author 1-specification.md (folder is empty scaffold)"
    fi
}

scan() {
    owed=()
    [ -d "$ITEMS_DIR" ] || return 0
    local d base
    for d in "$ITEMS_DIR"/*/; do
        [ -d "$d" ] || continue
        base="$(basename "$d")"
        owed+=("$base — next: $(next_step "${d%/}")")
    done
}

# --- selftest ----------------------------------------------------------------
if [ "$MODE" = "selftest" ]; then
    fail=0
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    items="$tmp/items"
    mkdir -p "$items/01-spec-only" "$items/02-plan-ready" "$items/03-pre-gated" \
             "$items/04-implementing" "$items/05-post-reviewed" "$items/06-closing"
    touch "$items/01-spec-only/1-specification.md"
    touch "$items/02-plan-ready/1-specification.md" "$items/02-plan-ready/2-design.md" \
          "$items/02-plan-ready/3-plan.md"
    touch "$items/03-pre-gated/3-plan.md" "$items/03-pre-gated/4-review-1-claude-opus.md"
    touch "$items/04-implementing/3-plan.md" "$items/04-implementing/4-review-1-claude-opus.md" \
          "$items/04-implementing/4-resolution-1-claude-opus.md"
    touch "$items/05-post-reviewed/3-plan.md" "$items/05-post-reviewed/5-review-1-claude-opus.md"
    touch "$items/06-closing/5-review-1-claude-opus.md" "$items/06-closing/5-resolution-1-claude-opus.md"
    out="$tmp/out"
    WORK_ITEMS_DIR="$items" bash agents/scripts/core/work-item-owed.sh --list > "$out" || true
    assert_next() {
        grep -qF "work item open: $1 — next: $2" "$out" \
            || { echo "FAIL: $1 expected next '$2'; got: $(grep -F "$1" "$out" || echo '<missing>')"; fail=1; }
    }
    # selftest: asserts-failure — an open item fixture must be detected and
    # classified to the right next step; a wrong or missing classification fails.
    assert_next "01-spec-only"     "author 2-design.md"
    assert_next "02-plan-ready"    "pre-implementation review (run-review.sh --gate pre)"
    assert_next "03-pre-gated"     "resolve the pre-implementation review (address-review-feedback skill)"
    assert_next "04-implementing"  "user sign-off on the pre gate, then implementation"
    assert_next "05-post-reviewed" "resolve the post-implementation review (address-review-feedback skill)"
    assert_next "06-closing"       "retro + close (drain Spawned.md, collapse to docs/work/closed/ — close-work-item skill)"
    # Empty items dir → silent nudge (no block emitted).
    empty="$tmp/empty"; mkdir -p "$empty"
    nout="$(WORK_ITEMS_DIR="$empty" bash agents/scripts/core/work-item-owed.sh --nudge)"
    [ -z "$nout" ] || { echo "FAIL: nudge not silent on empty items dir"; fail=1; }
    # Missing items dir → silent, exit 0 (fresh clone before Phase 1 tree).
    mout="$(WORK_ITEMS_DIR="$tmp/nonexistent" bash agents/scripts/core/work-item-owed.sh --nudge)" \
        || { echo "FAIL: missing items dir must exit 0"; fail=1; }
    [ -z "$mout" ] || { echo "FAIL: nudge not silent on missing items dir"; fail=1; }
    if [ "$fail" = "0" ]; then echo "work-item-owed --selftest: PASS (8/8)"; exit 0; fi
    echo "work-item-owed --selftest: FAIL"; exit 1
fi

scan

# --- Emit --------------------------------------------------------------------
if [ "${#owed[@]}" -eq 0 ]; then
    [ "$MODE" = "list" ] && echo "work-item-owed: no open work item — nothing owed."
    exit 0
fi

if [ "$MODE" = "nudge" ]; then
    echo "## === open work item(s) (${#owed[@]}) ==="
    echo "Open item(s) under docs/work/items/ (docs/agent-rules/work-items.md — the oldest"
    echo "open item finishes before the next begins implementing; sign-off gates still pause):"
    for o in "${owed[@]}"; do echo "  - $o"; done
else
    for o in "${owed[@]}"; do echo "work item open: $o"; done
fi
exit 0
