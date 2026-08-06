# review-guard.sh — the review panel's write guard, as two pure functions.
# Port of Whip-Process Tools/review-guard.ps1; contract comments carried verbatim
# (docs/agent-rules/review-panels.md § Write-guard contracts binds this port).
#
# A review is a READ-ONLY pass that writes exactly one file (the leg's own
# review file). No harness can express that — every leg flag is all-or-nothing
# (a sandbox that blocks all writes blocks the review file too) — so the rule
# is CHECKED after the round rather than trusted to the harness.
#
# CONTRACT, deliberately narrow: the guard sees what `git status` sees. A path
# that git ignores is invisible to it. Widening to --ignored would drown the
# report in build artifacts; the guard's job is the tracked work product, which
# is what a review can actually corrupt.
#
# Shape (the PowerShell original returned hashtables; bash speaks in lines):
#   get_dirty_state <root>
#     stdout — one line per dirty path: "<path>\t<signature>", sorted by path
#     rc 0  — scan performed (zero lines = VERIFIED clean)
#     rc 1  — scan could NOT be performed (the original's $null; see below)
#   get_stray_paths <before-file> <after-file> <expected-file>
#     args  — files of get_dirty_state lines (expected-file: one path per line)
#     stdout — sorted unique stray paths
#
# Sourced by run-review.sh and the bats suite; no side effects at source time.

# get_dirty_state <root>
# Every dirty path (staged, unstaged, untracked) mapped to a content signature.
#
# Signature, not mere membership: a tree is routinely dirty mid-item, and
# keying on path names alone reports clean when a leg edits a file that was
# ALREADY dirty at launch — the guard's normal operating state, and so a
# false-clean exactly where it matters.
#
# Returns rc 1 when the scan could not be performed — NOT an empty set. The
# distinction is load-bearing: a guard that reports clean because git failed
# is worse than no guard: it fails OPEN in a safety check. Callers must treat
# rc 1 as unverified and say so.
get_dirty_state() {
    local root="$1"

    # Test -e, not -d: a linked worktree's .git is a FILE (gitfile pointer),
    # and both shapes are a repo.
    [ -e "$root/.git" ] || return 1

    # -uall lists untracked FILES; the default collapses an untracked directory
    # to its name, which would hide a stray file written inside one (an item
    # folder is untracked for its whole life).
    local status_out
    status_out="$(git -C "$root" status --porcelain --untracked-files=all 2>/dev/null)" || return 1

    # Status letters plus the worktree hash still miss a path whose staged
    # content changed while its status letters and its on-disk bytes stayed put
    # (an `MM` path re-staged from a different blob) — so the index entry is
    # part of the signature.
    local -A index_blob=()
    local line
    while IFS= read -r line; do
        if [[ $line =~ ^[0-9]+\ ([0-9a-f]+)\ [0-9]+[[:space:]](.*)$ ]]; then
            index_blob["${BASH_REMATCH[2]}"]="${BASH_REMATCH[1]}"
        fi
    done < <(git -C "$root" ls-files -s 2>/dev/null)

    local -a out=()
    local xy rel src full hash idx
    while IFS= read -r line; do
        [ "${#line}" -gt 3 ] || continue
        xy="${line:0:2}"
        rel="${line:3}"
        while [[ $rel == \"* ]]; do rel="${rel#\"}"; done
        while [[ $rel == *\" ]]; do rel="${rel%\"}"; done

        # A rename arrives as `R  old -> new`, so the raw substring is BOTH
        # paths in one key — which would hash nothing and mask any later edit
        # of the new path. Record the destination (the path that exists on
        # disk) and the source separately.
        if [[ $rel == *" -> "* ]]; then
            src="${rel%% -> *}"
            out+=("$src"$'\t'"$xy:absent")
            rel="${rel#* -> }"
        fi

        # A deleted path has no content to hash; 'absent' still distinguishes
        # it from any later state.
        full="$root/$rel"
        if [ -f "$full" ]; then
            hash="$(sha256sum "$full" 2>/dev/null | cut -d' ' -f1)" || hash="absent"
            [ -n "$hash" ] || hash="absent"
        else
            hash="absent"
        fi
        idx="${index_blob[$rel]:--}"
        out+=("$rel"$'\t'"$xy:$hash:$idx")
    done <<< "$status_out"

    if [ "${#out[@]}" -gt 0 ]; then
        printf '%s\n' "${out[@]}" | sort
    fi
    return 0
}

# get_stray_paths <before-file> <after-file> <expected-file>
# Paths whose state changed during the round and that are not an expected
# review output. Covers three cases with one comparison: a new path (absent
# from the baseline), a re-edited already-dirty path (different signature),
# and a path a leg deleted (signature becomes 'absent').
get_stray_paths() {
    local before_file="$1" after_file="$2" expected_file="$3"
    local -A before=() after=() expected=()
    local p sig

    while IFS=$'\t' read -r p sig; do
        [ -n "$p" ] && before["$p"]="$sig"
    done < "$before_file"
    while IFS=$'\t' read -r p sig; do
        [ -n "$p" ] && after["$p"]="$sig"
    done < "$after_file"
    while IFS= read -r p; do
        [ -n "$p" ] && expected["$p"]=1
    done < "$expected_file"

    local -a stray=()
    for p in "${!after[@]}"; do
        [ -n "${expected[$p]+x}" ] && continue
        if [ -n "${before[$p]+x}" ] && [ "${before[$p]}" = "${after[$p]}" ]; then
            continue
        fi
        stray+=("$p")
    done

    # Dirty at launch, clean now: a leg reverted someone's in-flight work.
    for p in "${!before[@]}"; do
        [ -n "${after[$p]+x}" ] && continue
        [ -n "${expected[$p]+x}" ] && continue
        stray+=("$p")
    done

    if [ "${#stray[@]}" -gt 0 ]; then
        printf '%s\n' "${stray[@]}" | sort -u
    fi
    return 0
}
