#!/usr/bin/env bash
# record-review-verdict.sh — stamp the pre-push review-verdict marker for HEAD
# and print the head-bound PR-body verdict line.
#
# The review step of the [pre-first-push gate] leaves no trace, and a recorded
# verdict that names no commit survives pushes it never covered
# (review-verdict-not-bound-to-head, process 2026-08-13: one verdict rode
# through six review-fix pushes). This script is the single stamping point for
# both enforcement layers:
#   1. Prints `adversarial-code-review: <tail> (head=<sha>)` for the PR body —
#      CI (doc-validation.yml `Intent section`) rejects a body whose head= does
#      not match the PR head, so every push invalidates the prior verdict.
#   2. Writes `$GIT_DIR/review-verdict-<head-sha>` — the pre-push hook
#      (scripts/git-hooks/pre-push, section E) refuses to push a feature branch
#      whose HEAD carries no marker.
# Neither layer can prove the review RAN; they prove a claim was recorded for
# THIS commit, which makes a skipped or stale review visible instead of silent.
#
# Grammar validation alone left a hole a hand-written tail could walk through:
# `"n/a — <plausible reason>"` passes the checker with no review having run at
# all (PR #2219: a verdict was recorded purely to satisfy this gate, before any
# review tool had been invoked, on a diff substantive enough to warrant one).
# So for a SUBSTANTIVE diff (strict-zone touch, or >= REVIEW_LINE_THRESHOLD
# changed first-party C++ lines vs <base-ref> — the exact test
# scripts/dev/pre-ship.sh's push gate already applies) this script additionally
# requires a current, fingerprint-matching `.review-ack` + `.review-findings.json`
# pair before recording ANY tail, findings-form or "n/a" alike — the same
# machine-checkable proof `pre-ship.sh --ack-review` requires, reused via
# agents/scripts/core/lib/review-ack.sh rather than re-implemented here. A
# genuinely trivial diff (docs, comments, renames, non-C++, or first-party C++
# under the line threshold) is untouched: "n/a — <reason>" stays a one-liner,
# no artifact required. This is honest about its limit, same as the artifact
# check it reuses: it closes "recorded without reviewing", not a determined
# forgery of the artifact — see pre-ship.sh's own comment on that trade-off.
# Also honest that "substantive" is scoped to first-party C++ (RA_CPP_GLOBS,
# lib/review-ack.sh) — a diff touching ONLY this script / pre-ship.sh / other
# shell tooling is therefore never substantive under this rule, so the gate
# scripts editing their OWN enforcement are not (yet) forced through it either.
# Pre-existing scope, not something this change narrowed or widened.
#
# Usage:
#   bash record-review-verdict.sh "N findings, <disposition>" [<base-ref>]
#   bash record-review-verdict.sh "n/a — <reason>"            [<base-ref>]
#     # <base-ref> defaults to origin/develop (ra_is_substantive's default) —
#     # pass the SAME <base-ref> here as was used for `pre-ship.sh --ack-review`;
#     # a mismatch fingerprints a different diff and REFUSES as "no proof".
#   bash record-review-verdict.sh --selftest
#
# Exit: 0 = tail validated + marker written + line printed; 1 = tail rejected
# by the verdict grammar (unfilled placeholders included) OR a substantive diff
# has no matching review-ack/artifact pair; 2 = usage / not a git repo / no
# HEAD / the review-ack library is unreadable (infra, not a real gate result).
#
# Bypass (logged, discouraged, mirrors pre-ship.sh): SMATCHET_SKIP_REVIEW_GATE=1
#
# selftest: asserts-failure
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SELF_DIR/$(basename "${BASH_SOURCE[0]}")"
CHECKER="$SELF_DIR/check-pr-intent.sh"
REVIEW_ACK_LIB="$SELF_DIR/lib/review-ack.sh"

# _require_review_proof <base_ref> — rc 0 when the current branch diff either
# isn't substantive or carries a current, fingerprint-matching review-ack +
# .review-findings.json artifact; rc 1 (with operator instructions) otherwise.
# Split out of _record so the artifact check runs before ANY tail — including a
# grammatically-perfect "n/a" — reaches the marker file.
_require_review_proof() {
    local base_ref="$1"
    if [ ! -r "$REVIEW_ACK_LIB" ]; then
        echo "record-review-verdict: cannot read $REVIEW_ACK_LIB (incomplete checkout?)" >&2
        return 2
    fi
    # shellcheck source=agents/scripts/core/lib/review-ack.sh
    if ! . "$REVIEW_ACK_LIB"; then
        echo "record-review-verdict: failed to source $REVIEW_ACK_LIB" >&2
        return 2
    fi
    if [ "${SMATCHET_SKIP_REVIEW_GATE:-0}" = "1" ]; then
        echo "record-review-verdict: WARN — review-artifact requirement bypassed (SMATCHET_SKIP_REVIEW_GATE=1)." >&2
        return 0
    fi
    # An unresolvable base_ref makes ra_changed_files/ra_changed_lines fail
    # silently (git diff ... 2>/dev/null || true), so ra_is_substantive would
    # see zero files/lines and report "not substantive" — the exact
    # proof-evasion hole this script exists to close (a bad/unfetched
    # <base-ref> would otherwise let a substantive diff record an unproven
    # verdict). Fail loud instead of silently treating it as trivial.
    if ! git rev-parse --verify --quiet "${base_ref}^{commit}" >/dev/null; then
        echo "record-review-verdict: base ref '$base_ref' does not resolve — cannot judge substantiveness (fetch it or pass a valid <base-ref>)" >&2
        return 2
    fi
    ra_is_substantive branch "$base_ref" || return 0
    local reason="$RA_SUBSTANTIVE_REASON" want_fp have_fp findings_fp
    want_fp="$(ra_fingerprint branch "$base_ref")"
    have_fp="$(ra_read_marker branch)"
    findings_fp="$(ra_findings_fingerprint || true)"
    if [ "$have_fp" = "$want_fp" ] && [ -n "$findings_fp" ] && [ "$findings_fp" = "$want_fp" ]; then
        echo "record-review-verdict: review-artifact proof verified for this diff (fingerprint matches)."
        return 0
    fi
    cat >&2 <<EOF
record-review-verdict: REFUSED — substantive diff ($reason) has no proof a review ran.
  A verdict — findings-form OR "n/a" alike — on THIS diff needs a current,
  fingerprint-matching .review-ack + .review-findings.json pair: the same proof
  scripts/dev/pre-ship.sh --ack-review already requires before push
  (ship-loops.md § [pre-first-push gate] item 1). This closes the hole where a
  hand-written "n/a — <reason>" satisfied the gate with no review behind it.
  1) run the code-review skill/agent on the diff vs $base_ref (it writes
     .review-findings.json),
  2) bash scripts/dev/pre-ship.sh --ack-review $base_ref
  3) re-run this script.
  (Emergency bypass: SMATCHET_SKIP_REVIEW_GATE=1 — logged, discouraged.)
EOF
    return 1
}

_record() {
    local tail_text="$1" base_ref="${2:-origin/develop}"
    local gitdir head line body out
    gitdir="$(git rev-parse --git-dir 2>/dev/null)" \
        || { echo "record-review-verdict: not inside a git repository" >&2; return 2; }
    head="$(git rev-parse --verify HEAD 2>/dev/null)" \
        || { echo "record-review-verdict: no HEAD commit to bind the verdict to" >&2; return 2; }
    _require_review_proof "$base_ref" || return $?
    line="adversarial-code-review: ${tail_text} (head=${head:0:12})"
    # Validate through the REAL checker (never a second copy of the grammar —
    # the CI copy already drifted once; see --check-workflow-sync there), so an
    # unfilled placeholder tail ("<disposition>", "<reason …>") is rejected here
    # at recording time, not later on the PR. The synthetic body exists only to
    # satisfy the checker's Intent requirement; PR_HEAD_SHA exercises the head
    # binding against the sha we just stamped.
    body="## Intent"$'\n\n'"(record-review-verdict validation shim)"$'\n\n'"$line"
    if ! out="$(printf '%s' "$body" | PR_HEAD_SHA="$head" bash "$CHECKER" 2>&1)"; then
        echo "record-review-verdict: verdict tail rejected by the verdict grammar:" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        echo "record-review-verdict: expected \"N findings, <disposition>\" or \"n/a — <reason>\"" >&2
        echo "  with the placeholders FILLED (ship-loops.md § [pre-first-push gate] item 5)." >&2
        return 1
    fi
    printf '%s\n' "$line" > "$gitdir/review-verdict-$head"
    printf '%s\n' "$line"
}

run_selftest() {
    local tmpd out head marker
    tmpd="$(mktemp -d)" \
        || { echo "record-review-verdict --selftest: FAIL — mktemp failed" >&2; return 1; }
    if ! git -C "$tmpd" init -q \
        || ! git -C "$tmpd" -c user.email=selftest@local -c user.name=selftest \
               -c commit.gpgsign=false commit -q --allow-empty -m seed; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — could not build the temp repo" >&2
        return 1
    fi
    head="$(git -C "$tmpd" rev-parse HEAD)"
    marker="$(git -C "$tmpd" rev-parse --git-dir)"
    case "$marker" in /*) ;; *) marker="$tmpd/$marker" ;; esac
    marker="$marker/review-verdict-$head"
    # Rejection cases run FIRST — they must leave NO marker behind, and the
    # ordering makes that assertable (the pass case below stamps this same path).
    # An unfilled findings-form placeholder MUST be rejected, for the grammar
    # reason, with nothing stamped.
    out="$(cd "$tmpd" && bash "$SELF" "0 findings, <disposition>" HEAD 2>&1)" && {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — accepted an unfilled findings placeholder" >&2
        return 1
    }
    case "$out" in
        *"rejected by the verdict grammar"*) ;;
        *) rm -rf "$tmpd"
           echo "record-review-verdict --selftest: FAIL — placeholder tail rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    if [ -e "$marker" ]; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — marker stamped despite a rejected tail" >&2
        return 1
    fi
    # The unfilled n/a placeholder MUST be rejected too.
    out="$(cd "$tmpd" && bash "$SELF" "n/a — <reason the diff is trivial>" HEAD 2>&1)" && {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — accepted an unfilled n/a placeholder" >&2
        return 1
    }
    if [ -e "$marker" ]; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — marker stamped despite a rejected n/a tail" >&2
        return 1
    fi
    # An empty tail is a usage error, not a stamp.
    out="$(cd "$tmpd" && bash "$SELF" "" HEAD 2>&1)" && {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — accepted an empty tail" >&2
        return 1
    }
    # A valid tail MUST stamp the marker and print the head-bound line.
    out="$(cd "$tmpd" && bash "$SELF" "0 findings" HEAD)" || {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — rejected a valid bare-count tail" >&2
        return 1
    }
    case "$out" in
        *"(head=${head:0:12})"*) ;;
        *) rm -rf "$tmpd"
           echo "record-review-verdict --selftest: FAIL — printed line is not bound to HEAD:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    if [ ! -f "$marker" ]; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — valid tail left no marker at $marker" >&2
        return 1
    fi

    # --- Substantive-diff artifact-proof gate (the PR #2219 hole) -----------------
    # A SEPARATE fixture: a strict-zone C++ edit, so ra_is_substantive trips. Mirrors
    # scripts/dev/pre-ship.sh's own selftest fixture (same shared lib, same rule).
    local tmp2 fhead fmarker fp
    tmp2="$(mktemp -d)" \
        || { rm -rf "$tmpd"; echo "record-review-verdict --selftest: FAIL — mktemp failed (tmp2)" >&2; return 1; }
    if ! (
        set -e
        cd "$tmp2"
        git init -q -b base .
        git config user.email selftest@local
        git config user.name selftest
        printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > project.config.json
        mkdir -p Source/Core/src/Sync
        echo "// base" > Source/Core/src/Sync/SelfTest.cpp
        git add -A && git commit -qm base
        git checkout -qb feature
        printf '// edit\nint self_test_fn() { return 1; }\n' >> Source/Core/src/Sync/SelfTest.cpp
        git commit -aqm edit
    ); then
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — could not build the substantive-diff fixture" >&2
        return 1
    fi
    fhead="$(git -C "$tmp2" rev-parse HEAD)"
    fmarker="$(git -C "$tmp2" rev-parse --git-dir)"
    case "$fmarker" in /*) ;; *) fmarker="$tmp2/$fmarker" ;; esac
    fmarker="$fmarker/review-verdict-$fhead"
    # A substantive (strict-zone) diff with NO review-ack / artifact MUST be
    # refused — even a grammatically-perfect "n/a" (the exact #2219 shape: a
    # plausible-sounding justification with no review behind it).
    out="$(cd "$tmp2" && bash "$SELF" "n/a — trivial" base 2>&1)" && {
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — recorded an n/a verdict on a substantive diff with NO review proof" >&2
        return 1
    }
    case "$out" in
        *"has no proof a review ran"*) ;;
        *) rm -rf "$tmpd" "$tmp2"
           echo "record-review-verdict --selftest: FAIL — unproven substantive diff rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    if [ -e "$fmarker" ]; then
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — marker stamped despite no review proof" >&2
        return 1
    fi
    # Same diff, findings-form tail — the proof gate blocks the TAIL SHAPE, not
    # just the n/a escape, so this MUST be refused too.
    if (cd "$tmp2" && bash "$SELF" "3 findings, all fixed" base >/dev/null 2>&1); then
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — recorded a findings-form verdict on a substantive diff with NO review proof" >&2
        return 1
    fi
    # A review-ack + artifact recorded for some OTHER diff (mismatched
    # fingerprint) MUST still be refused — a stale proof is not proof.
    (cd "$tmp2" && printf 'branch\t%064d\n' 0 > .review-ack &&
        printf '{"fingerprint":"%064d"}\n' 0 > .review-findings.json)
    if (cd "$tmp2" && bash "$SELF" "n/a — trivial" base >/dev/null 2>&1); then
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — recorded a verdict against a stale review-ack fingerprint" >&2
        return 1
    fi
    # A CURRENT, matching review-ack + artifact pair -> the verdict records
    # normally (both the artifact-proof check and the underlying grammar check
    # pass; the recorded fingerprint is the diff's REAL one, from ra_fingerprint
    # itself — never re-derived here, to avoid a second copy of that hash rule).
    fp="$(
        cd "$tmp2" || exit 1
        # shellcheck source=agents/scripts/core/lib/review-ack.sh
        . "$REVIEW_ACK_LIB"
        ra_fingerprint branch base
    )" || {
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — could not compute the fixture's own fingerprint" >&2
        return 1
    }
    (cd "$tmp2" && printf 'branch\t%s\n' "$fp" > .review-ack &&
        printf '{"fingerprint":"%s","reviewer":"selftest"}\n' "$fp" > .review-findings.json)
    out="$(cd "$tmp2" && bash "$SELF" "0 findings" base)" || {
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — refused a verdict with a current, matching review-ack + artifact" >&2
        return 1
    }
    case "$out" in
        *"(head=${fhead:0:12})"*) ;;
        *) rm -rf "$tmpd" "$tmp2"
           echo "record-review-verdict --selftest: FAIL — proven substantive-diff verdict not bound to its HEAD:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    if [ ! -f "$fmarker" ]; then
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — a proven substantive-diff verdict left no marker" >&2
        return 1
    fi
    # SMATCHET_SKIP_REVIEW_GATE=1 bypasses the artifact requirement (documented
    # emergency escape, mirrors pre-ship.sh) even with NO proof at all.
    (cd "$tmp2" && rm -f .review-ack .review-findings.json)
    if ! (cd "$tmp2" && SMATCHET_SKIP_REVIEW_GATE=1 bash "$SELF" "n/a — bypass" base >/dev/null 2>&1); then
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — SMATCHET_SKIP_REVIEW_GATE=1 bypass broken" >&2
        return 1
    fi
    # An unresolvable base_ref MUST be refused, not silently treated as "no
    # substantive diff" — ra_changed_files/ra_changed_lines swallow a bad ref's
    # git-diff error (2>/dev/null || true), so without this guard a substantive
    # diff against a never-fetched <base-ref> would record an unproven verdict.
    out="$(cd "$tmp2" && bash "$SELF" "n/a — trivial" no-such-ref-xyz 2>&1)" && {
        rm -rf "$tmpd" "$tmp2"
        echo "record-review-verdict --selftest: FAIL — recorded a verdict against an unresolvable base_ref" >&2
        return 1
    }
    case "$out" in
        *"does not resolve"*) ;;
        *) rm -rf "$tmpd" "$tmp2"
           echo "record-review-verdict --selftest: FAIL — unresolvable base_ref rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac

    rm -rf "$tmpd" "$tmp2"
    echo "record-review-verdict --selftest: PASS"
}

case "${1:-}" in
    --selftest) run_selftest; exit $? ;;
    --help | -h) sed -n '2,56p' "$0"; exit 0 ;;
    "")
        echo "record-review-verdict: missing verdict tail." >&2
        echo "  usage: bash record-review-verdict.sh \"N findings, <disposition>\" | \"n/a — <reason>\" [<base-ref>]" >&2
        exit 2
        ;;
    *) _record "$1" "${2:-}" ;;
esac
