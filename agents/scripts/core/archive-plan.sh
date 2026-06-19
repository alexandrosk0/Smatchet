#!/usr/bin/env bash
# archive-plan.sh — one-shot, CWD-proof plan archival (active/ -> shipped|deferred).
#
# Why: archiving a plan is a scattered manual ritual and dropping any one step
# reds CI. On #1394, hand-archiving caused THREE red-CI waves:
#   1. INDEX.md not regenerated (test-plan-index.sh --fix silently no-op'd from
#      the wrong CWD — it resolves docs/plans/... relative to $PWD).
#   2. The `git mv` dangled every tier-FUL `docs/plans/active/<slug>.md`
#      reference tree-wide; test-plan-ref-integrity.sh wants the move-proof
#      tier-LESS form `docs/plans/<slug>.md`.
#   3. A markdown hyperlink inside the moved plan to a still-active plan dangled
#      (markdown links resolve from the file's dir, so they want a tier-FUL
#      relative path — the OPPOSITE of plain-text refs).
#
# This script folds the whole ritual into one verified command, anchored at the
# git root so CWD can never poison the relative paths the gates use.
#
# What it does (atomically, from the git root):
#   1. Validate <slug> resolves to exactly one docs/plans/active/<slug>.md.
#   2. git mv docs/plans/active/<slug>.md docs/plans/<tier>/<slug>.md.
#   3. Tree-wide rewrite of inbound PLAIN-TEXT refs for THIS slug:
#        docs/plans/active/<slug>.md  ->  docs/plans/<slug>.md  (tier-less).
#   4. Regenerate the index: test-plan-index.sh --fix.
#   5. Verify gates: test-plan-index.sh, test-plan-ref-integrity.sh,
#      test-markdown-links.sh --diff origin/develop. Refuse (exit non-zero) on
#      any red, including a dangling markdown hyperlink the script won't guess.
#
# Markdown hyperlinks: the CORRECT relative href for a `[label](...)` link
# depends on the REFERRING file's tier, so a blind global sed is unsafe (it
# would write a wrong path for files in another tier). Instead we DETECT and
# REPORT dangling hyperlinks via test-markdown-links.sh and FAIL with a clear
# "fix these by hand" message — never silently leave a dangling link.
#
# KNOWN RESIDUAL (non-CI-gating): test-markdown-links.sh EXCLUDES docs/plans/
# shipped/, so a dangling hyperlink inside the just-moved plan's OWN body (e.g. a
# bare `(other-plan.md)` outbound link that now resolves under shipped/) is
# invisible to that gate and is NOT caught here. It is also not a CI failure (CI
# excludes shipped/ too) — only a slightly-wrong link in an archived doc. Catchable
# cases (dangling refs in still-ACTIVE referrers) ARE gated. Closing the moved-file
# self-link gap deterministically (we know the moved file's new location) is a
# tracked follow-up; see the applied self-improvement entry.
#
# Leaves the working tree staged-or-modified for the human to commit. Does NOT
# commit/push (the orchestrator owns git).
#
# Usage:
#   bash agents/scripts/core/archive-plan.sh <slug> [--tier shipped|deferred]
#
# Exit codes:
#   0 — archived; all gates green; staged for commit.
#   1 — a gate failed (tree left for inspection/fixup).
#   2 — usage / validation error (nothing changed).

set -euo pipefail

PROG="archive-plan"

log()  { echo "[$PROG] $*"; }
err()  { echo "[$PROG] ERROR: $*" >&2; }

usage() {
    cat >&2 <<EOF
usage: $0 <slug> [--tier shipped|deferred]
  <slug>   plan filename without .md, living under docs/plans/active/
  --tier   destination tier (default: shipped)
EOF
    exit 2
}

# --- Parse args -------------------------------------------------------------
SLUG=""
TIER="shipped"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --tier)
            [ "$#" -ge 2 ] || { err "--tier needs a value"; usage; }
            TIER="$2"; shift 2 ;;
        --tier=*)
            TIER="${1#--tier=}"; shift ;;
        -h|--help)
            usage ;;
        --*)
            err "unknown option: $1"; usage ;;
        *)
            if [ -n "$SLUG" ]; then err "unexpected extra argument: $1"; usage; fi
            SLUG="$1"; shift ;;
    esac
done

[ -n "$SLUG" ] || { err "missing <slug>"; usage; }

case "$TIER" in
    shipped|deferred) : ;;
    *) err "invalid --tier '$TIER' (want: shipped|deferred)"; usage ;;
esac

# Reject a slug that smells like a path or carries an extension — the slug is a
# bare basename; the script supplies the dir + .md.
case "$SLUG" in
    */*|*\\*) err "<slug> must be a bare basename, not a path: '$SLUG'"; usage ;;
    *.md)     err "<slug> must omit the .md extension: '$SLUG'"; usage ;;
esac

# --- Anchor at the git root (the #1 bug class) ------------------------------
# CWD must never poison the relative docs/plans/... paths the gates resolve.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || { err "not in a git repository"; exit 2; }
cd "$ROOT" || { err "cannot cd to git root '$ROOT'"; exit 2; }

SRC="docs/plans/active/$SLUG.md"
DST="docs/plans/$TIER/$SLUG.md"

# --- Validate ---------------------------------------------------------------
if [ ! -f "$SRC" ]; then
    err "no plan at $SRC"
    # Helpful: is it already in the target (or sibling) tier?
    for t in shipped deferred; do
        if [ -f "docs/plans/$t/$SLUG.md" ]; then
            err "  (note: docs/plans/$t/$SLUG.md already exists — already archived?)"
        fi
    done
    exit 2
fi

if [ -e "$DST" ]; then
    err "destination already exists: $DST (already in tier '$TIER'?)"
    exit 2
fi

log "root      : $ROOT"
log "archiving : $SRC -> $DST"

# Locate the sibling core scripts relative to THIS script (not CWD), so the
# helper works regardless of where it was invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDX="$SCRIPT_DIR/test-plan-index.sh"
REF="$SCRIPT_DIR/test-plan-ref-integrity.sh"
MDL="$SCRIPT_DIR/test-markdown-links.sh"
for s in "$IDX" "$REF" "$MDL"; do
    [ -f "$s" ] || { err "sibling script missing: $s"; exit 2; }
done

# --- 1. Move ----------------------------------------------------------------
git mv "$SRC" "$DST" || { err "git mv failed"; exit 1; }
log "moved (staged): $DST"

# --- 2. Tree-wide tier-less rewrite of inbound PLAIN-TEXT refs --------------
# Rewrite every `docs/plans/active/<slug>.md` -> `docs/plans/<slug>.md` across
# tracked + untracked files, scoped to THIS slug only. This is the move-proof
# form test-plan-ref-integrity.sh resolves against any tier. We exclude the
# just-moved plan file's own body from the destination-tier list? No — the moved
# file is also a tracked file that may itself contain inbound plain-text refs to
# its OWN active path (rare) or to other slugs (left untouched). git grep -l
# finds the literal; sed rewrites only the exact this-slug literal.
#
# NOTE: a plain-text occurrence INSIDE a markdown `[label](href)` hyperlink whose
# href is exactly `docs/plans/active/<slug>.md` is also a repo-relative path that
# resolves fine as tier-less — so rewriting it is correct. The dangerous
# hyperlinks are the tier-FUL RELATIVE ones (`(../active/<slug>.md)` /
# `(<slug>.md)`) whose correct path depends on the referring file's tier; those
# are NOT matched by this literal and are handled by the detect-and-report gate.
OLD="docs/plans/active/$SLUG.md"
NEW="docs/plans/$SLUG.md"

# git grep -l over tracked + untracked (honours .gitignore); fixed-string match.
# Exclude this script and the ref-integrity script (their bodies carry example
# literals). `|| true` so a no-match (no inbound refs) isn't fatal under set -e.
mapfile -t HITS < <(git grep -lF --untracked -- "$OLD" \
    ':(exclude)agents/scripts/core/archive-plan.sh' \
    ':(exclude)agents/scripts/core/test-archive-plan.sh' \
    ':(exclude)agents/scripts/core/test-plan-ref-integrity.sh' \
    2>/dev/null | sort -u || true)

if [ "${#HITS[@]}" -gt 0 ]; then
    log "rewriting plain-text refs ($OLD -> $NEW) in ${#HITS[@]} file(s):"
    # Escape for a sed s/// program: BRE metacharacters in the path. The slug is
    # kebab-case but be defensive (dots, dashes). Escape / . & and the delimiter.
    esc_old="$(printf '%s' "$OLD" | sed -e 's/[\/&.[\*^$]/\\&/g')"
    esc_new="$(printf '%s' "$NEW" | sed -e 's/[\/&]/\\&/g')"
    for f in "${HITS[@]}"; do
        [ -f "$f" ] || continue
        log "  - $f"
        # In-place, no backup. POSIX-portable invocation.
        sed -i "s/$esc_old/$esc_new/g" "$f"
    done
else
    log "no inbound plain-text refs to rewrite."
fi

# --- 3. Regenerate the index ------------------------------------------------
log "regenerating plan index (test-plan-index.sh --fix)"
if ! bash "$IDX" --fix; then
    err "test-plan-index.sh --fix failed"
    exit 1
fi

# --- 4. Verify gates; refuse on red -----------------------------------------
GATE_FAIL=0

run_gate() {
    local name="$1"; shift
    log "gate: $name"
    if bash "$@"; then
        log "gate OK: $name"
    else
        err "gate FAILED: $name"
        GATE_FAIL=1
    fi
}

run_gate "test-plan-index"         "$IDX"
run_gate "test-plan-ref-integrity" "$REF"

# Markdown links: diff-scope vs origin/develop. A dangling hyperlink here is the
# wave-3 bug — a tier-ful relative link inside the moved (or referring) file that
# the move broke. We DETECT and REPORT; we do NOT guess the fix.
# test-markdown-links diff-scopes against origin/develop; if that ref is absent
# (fresh clone / no fetch) the diff is empty and the gate would scan ZERO files
# and false-pass (exit 0). Guard the base ref exists first. (Cursor Bugbot 177778fc.)
if ! git rev-parse --verify --quiet origin/develop >/dev/null 2>&1; then
    log "origin/develop missing — fetching it for the markdown-links diff scope"
    git fetch --quiet origin develop || true
fi
MDL_OUT="$(mktemp)"
log "gate: test-markdown-links (diff vs origin/develop)"
if ! git rev-parse --verify --quiet origin/develop >/dev/null 2>&1; then
    GATE_FAIL=1
    err "gate FAILED: test-markdown-links — origin/develop unavailable, cannot diff-scope"
    err "  (a missing base ref makes the gate scan zero files + false-pass; refusing)."
elif bash "$MDL" >"$MDL_OUT" 2>&1; then
    log "gate OK: test-markdown-links"
else
    GATE_FAIL=1
    err "gate FAILED: test-markdown-links — dangling markdown hyperlink(s) after the move:"
    # Surface the offending links (stderr lines carry 'BROKEN_LINK').
    grep -E 'BROKEN_LINK|Failed:' "$MDL_OUT" >&2 || cat "$MDL_OUT" >&2
    err "  ^ Fix these context-dependent hyperlinks BY HAND: a markdown link's"
    err "    correct relative href depends on the REFERRING file's tier, so it"
    err "    cannot be rewritten safely here. Then re-run the gates."
fi
rm -f "$MDL_OUT"

if [ "$GATE_FAIL" -ne 0 ]; then
    err "one or more gates failed — the tree is moved+staged but NOT clean."
    err "Inspect/fix the above, re-run the gates, then commit by hand."
    exit 1
fi

# Stage EVERYTHING the archival touched. The git mv was already staged, but the
# plain-text ref rewrites and the INDEX.md regen were left UNSTAGED — so a plain
# `git commit` (no -a) would capture only the move and DROP the ref + INDEX
# fixes, reintroducing the very CI failures this helper exists to prevent.
# (Cursor Bugbot b7856a1b.) Done only after all gates are green.
STAGE=( "$DST" "docs/plans/INDEX.md" )
[ "${#HITS[@]}" -gt 0 ] && STAGE+=( "${HITS[@]}" )
if ! git add -- "${STAGE[@]}"; then
    err "git add (staging archival changes) failed — stage by hand before commit."
    exit 1
fi

log "DONE: $SLUG archived to '$TIER'. All gates green; ALL changes staged for commit."
log "Review with: git diff --staged   (commit is YOURS to make)."
exit 0
