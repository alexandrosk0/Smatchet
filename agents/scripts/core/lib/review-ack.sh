#!/usr/bin/env bash
# review-ack.sh — shared code-review acknowledgement library (sourced, not run).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS
#   docs/agent-rules/process-rules.md § Code-review before every commit mandates
#   that every commit's diff gets a `code-review` agent pass BEFORE it lands. We
#   cannot verify from a shell hook that an LLM/human review actually happened —
#   but we CAN pin an explicit acknowledgement to the EXACT diff content, so a
#   post-review edit invalidates the ack and forces a conscious re-review.
#
#   That fingerprint mechanism shipped inside scripts/dev/pre-ship.sh (2026-06-10)
#   and only ever ran at PRE-PUSH, against `<base>...HEAD` + the working tree, and
#   only when a human/agent chose to invoke pre-ship.sh. The mandate is a
#   PRE-COMMIT one, and scripts/git-hooks/pre-commit had no review check at all —
#   so a session could (and did) stage a substantive C++ diff, commit it, and open
#   a PR having never dispatched `code-review`. This library lifts the mechanism
#   out of pre-ship.sh so BOTH enforcement points share one implementation:
#     * scripts/git-hooks/pre-commit  -> mode `staged`  (the staged tree)
#     * scripts/dev/pre-ship.sh       -> mode `branch`  (base...HEAD + worktree)
#
# CONTRACT
#   Sourced, never executed. Callers own `set -euo pipefail`; every function here
#   is written to be safe under `set -e` (no bare non-zero returns on a normal
#   path) and must be called from inside the repo work tree.
#
# API
#   ra_marker_path                       -> absolute path of the .review-ack marker
#   ra_resolve_python                    -> prints a WORKING python, or empty + rc 1
#   ra_fingerprint <mode> [base_ref]     -> sha256 of the mode's first-party C++ diff
#   ra_changed_lines <mode> [base_ref]   -> added+deleted first-party C++ line count
#   ra_strict_hit <mode> [base_ref]      -> prints the first strict-zone path hit (or empty)
#   ra_is_substantive <mode> [base_ref]  -> rc 0 when the diff requires a review
#   ra_substantive_reason                -> human reason string for the last ra_is_substantive
#   ra_read_marker <mode>                -> prints the recorded sha for <mode> (or empty)
#   ra_write_marker <mode> <sha> [verdict...] -> records <sha> (+ optional verdict)
#   ra_read_verdict <mode>               -> prints the recorded verdict, or empty
#   ra_verdict_from_json <aggregate.json> -> verdict fields from a sidecar result
#
#   <mode> is `branch` (committed-on-branch + working tree, vs base_ref) or
#   `staged` (the index only). base_ref defaults to origin/develop and is ignored
#   by `staged`.
#
# MARKER FORMAT
#   .review-ack (gitignored) holds one record per line, tab-separated:
#
#     <mode>\t<sha256>
#     <mode>\t<sha256>\t<score>\t<recommendation>\t<hard_veto>\t<veto_reason>
#
#   The 6-field form carries a verifier verdict (see
#   docs/plans/verifier-scored-code-review-gate.md); the 2-field form is what a
#   repo with no verifier configured writes, and the two are read
#   interchangeably. A legacy single-line bare sha256 — everything written
#   before this library existed — is read as mode `branch`, so an in-flight ack
#   keeps working.
#
#   Readers must extract fields EXACTLY: an earlier implementation stripped all
#   whitespace after the mode, which silently folded the verdict fields into the
#   fingerprint once they existed (an ack that can never match = a gate that
#   blocks forever).
# ----------------------------------------------------------------------------

# The C++ trees whose diff content the fingerprint covers (mirrors pre-ship.sh's
# clang-format target globs — keep the two in sync).
RA_CPP_GLOBS=(
    'Source/Core/*.cpp' 'Source/Core/*.h'
    'Source/Plugins/*.cpp' 'Source/Plugins/*.h'
    'Source/Standalone/*.cpp' 'Source/Standalone/*.h'
    'tests/*.cpp' 'tests/*.h'
)

# Set by ra_is_substantive so a caller can render WHY without recomputing. Exported
# so shellcheck sees it as an external read (its consumers are the sourcing scripts,
# scripts/dev/pre-ship.sh and agents/scripts/core/review-ack.sh, not this file).
export RA_SUBSTANTIVE_REASON=""

# ra_marker_path — absolute path of the .review-ack marker for this work tree
# (empty when not inside one). Per-worktree by construction: it resolves against
# `git rev-parse --show-toplevel`, so parallel worktrees never share an ack.
ra_marker_path() {
    local root
    root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    [ -n "$root" ] || return 0
    printf '%s\n' "$root/.review-ack"
}

# Resolve a WORKING python interpreter (empty + rc 1 if none). `command -v python3`
# alone is insufficient on Windows: the python3 "App Execution Alias" stub passes
# `command -v` but exits 49 ("Python was not found") when actually run — so probe
# each candidate by executing it.
ra_resolve_python() {
    # Selftest hooks: force the "no working python" branch so the #1116 fail-closed
    # path is testable in an environment that DOES have python. Never set in normal
    # use. SMATCHET_PRESHIP_FORCE_NO_PY is honoured for pre-ship.sh's existing selftest.
    if [ "${SMATCHET_REVIEW_ACK_FORCE_NO_PY:-0}" = "1" ] || [ "${SMATCHET_PRESHIP_FORCE_NO_PY:-0}" = "1" ]; then
        return 1
    fi
    local cand p
    for cand in python3 python py; do
        p="$(command -v "$cand" 2>/dev/null)" || continue
        if "$p" -c "" >/dev/null 2>&1; then
            printf '%s\n' "$p"
            return 0
        fi
    done
    return 1
}

# ra_fingerprint <mode> [base_ref] — hash the diff CONTENT (not just the file list)
# so any change, including a clang-format reflow, re-arms the gate.
ra_fingerprint() {
    local mode="$1" base="${2:-origin/develop}"
    {
        if [ "$mode" = "staged" ]; then
            git diff --cached -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
        else
            git diff "$base"...HEAD -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
            git diff HEAD -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
        fi
    } | sha256sum | cut -d' ' -f1
}

# ra_changed_files <mode> [base_ref] — first-party C++ paths touched, deletions skipped.
ra_changed_files() {
    local mode="$1" base="${2:-origin/develop}"
    if [ "$mode" = "staged" ]; then
        git diff --cached --name-only --diff-filter=d -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
    else
        git diff --name-only --diff-filter=d "$base"...HEAD -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
        git diff --name-only --diff-filter=d -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
    fi
}

# ra_changed_lines <mode> [base_ref] — added+deleted first-party C++ line count
# for the mode's diff. Renames/deletes contribute 0 via the `-` numstat columns.
ra_changed_lines() {
    local mode="$1" base="${2:-origin/develop}"
    {
        if [ "$mode" = "staged" ]; then
            git diff --cached --numstat -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
        else
            git diff --numstat "$base"...HEAD -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
            git diff --numstat -- "${RA_CPP_GLOBS[@]}" 2>/dev/null || true
        fi
    } | awk '{a+=($1=="-"?0:$1); d+=($2=="-"?0:$2)} END {print a+d+0}'
}

# ra_strict_hit <mode> [base_ref] — print the first changed path inside a
# project.config.json `lint.zones.strict` prefix, or nothing.
#
# #1116 fail-CLOSED: with no WORKING python the strict-zone list is unreadable.
# Do NOT silently skip detection (that fail-OPENs a sub-threshold strict-zone
# diff) — report a sentinel so the caller treats the diff as review-requiring.
ra_strict_hit() {
    local mode="$1" base="${2:-origin/develop}" py f z
    local -a files=() zones=()
    mapfile -t files < <(ra_changed_files "$mode" "$base")
    # Drop empty entries so a no-C++ diff never trips the fail-closed branch below.
    local -a real=()
    for f in "${files[@]}"; do [ -n "$f" ] && real+=("$f"); done
    [ "${#real[@]}" -gt 0 ] || return 0

    py="$(ra_resolve_python || true)"
    if [ -z "$py" ]; then
        printf '%s\n' "(strict-zone detection unavailable — no working python)"
        return 0
    fi
    # tr -d '\r': native Windows python prints CRLF; an un-stripped \r silently
    # defeats the `case "$f" in "$z"*` prefix match.
    mapfile -t zones < <(
        "$py" -c "import json;print('\n'.join(json.load(open('project.config.json'))['lint']['zones']['strict']))" \
            2>/dev/null | tr -d '\r' || true
    )
    for f in "${real[@]}"; do
        for z in "${zones[@]:-}"; do
            [ -n "$z" ] || continue
            case "$f" in "$z"*)
                printf '%s\n' "$f"
                return 0
                ;;
            esac
        done
    done
    return 0
}

# ra_is_substantive <mode> [base_ref] — rc 0 when the diff must carry a review ack:
# it touches a strict zone, or changes >= REVIEW_LINE_THRESHOLD first-party C++ lines.
# Sets RA_SUBSTANTIVE_REASON for the caller's message.
ra_is_substantive() {
    local mode="$1" base="${2:-origin/develop}"
    local threshold="${REVIEW_LINE_THRESHOLD:-60}" hit lines
    hit="$(ra_strict_hit "$mode" "$base")"
    lines="$(ra_changed_lines "$mode" "$base")"
    RA_SUBSTANTIVE_REASON=""
    if [ -n "$hit" ]; then
        RA_SUBSTANTIVE_REASON="strict-zone touch ($hit)"
        return 0
    fi
    if [ "${lines:-0}" -ge "$threshold" ]; then
        RA_SUBSTANTIVE_REASON="$lines changed C++ lines >= $threshold"
        return 0
    fi
    RA_SUBSTANTIVE_REASON="$lines C++ lines, no strict-zone touch"
    return 1
}

# ra_read_marker <mode> — print the recorded FINGERPRINT for <mode>, or nothing.
# Reads both the 2-field and the verdict-bearing record shapes, plus the legacy
# bare-sha marker written before this library existed.
ra_read_marker() {
    local mode="$1" marker line rest
    marker="$(ra_marker_path)"
    [ -n "$marker" ] && [ -f "$marker" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        [ -n "$line" ] || continue
        case "$line" in
            "$mode"[[:space:]]*)
                # Take ONLY the fingerprint field. The original implementation ran
                # `tr -d '[:space:]'` over everything after the mode, which was
                # correct for a 2-field record but silently concatenates the
                # verdict fields into the sha once they exist — a stale-ack
                # comparison that can never match, i.e. a gate that blocks
                # forever. Field-exact extraction is the fix.
                rest="${line#"$mode"}"
                rest="${rest#"${rest%%[![:space:]]*}"}"   # ltrim
                printf '%s\n' "${rest%%[[:space:]]*}"     # first field only
                return 0
                ;;
        esac
        # Legacy pre-library marker: a bare 64-hex sha means the branch fingerprint.
        if [ "$mode" = "branch" ] && printf '%s' "$line" | grep -qE '^[0-9a-f]{64}$'; then
            printf '%s\n' "$line"
            return 0
        fi
    done < "$marker"
    return 0
}

# ra_write_marker <mode> <sha> [score] [recommendation] [hard_veto] [veto_reason]
# The trailing four are the verifier verdict and are OPTIONAL: a record written
# without them is byte-identical to the 2-field form, so a gate with no verifier
# configured behaves exactly as it did before the verdict existed.
ra_write_marker() {
    local mode="$1" sha="$2" marker line
    local score="${3:-}" rec="${4:-}" veto="${5:-}" reason="${6:-}"
    marker="$(ra_marker_path)"
    [ -n "$marker" ] || return 1
    local -a kept=()
    if [ -f "$marker" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            [ -n "$line" ] || continue
            case "$line" in "$mode"[[:space:]]*) continue ;; esac
            # A legacy bare sha is this file's `branch` record — drop it when
            # rewriting branch, otherwise migrate it to the tagged form.
            if printf '%s' "$line" | grep -qE '^[0-9a-f]{64}$'; then
                [ "$mode" = "branch" ] && continue
                kept+=("$(printf 'branch\t%s' "$line")")
                continue
            fi
            kept+=("$line")
        done < "$marker"
    fi
    if [ -n "$score$rec$veto$reason" ]; then
        # Tabs are the field separator, so a veto reason containing one would
        # shift every later field. Fold whitespace to single spaces.
        reason="$(printf '%s' "$reason" | tr '\t\n' '  ')"
        kept+=("$(printf '%s\t%s\t%s\t%s\t%s\t%s' "$mode" "$sha" "$score" "$rec" "$veto" "$reason")")
    else
        kept+=("$(printf '%s\t%s' "$mode" "$sha")")
    fi
    printf '%s\n' "${kept[@]}" > "$marker"
}

# ra_read_verdict <mode> — print the recorded verdict as
# `<score>\t<recommendation>\t<hard_veto>\t<veto_reason>`, or nothing when the
# record carries no verdict (the 2-field form, or a legacy bare sha).
ra_read_verdict() {
    local mode="$1" marker
    marker="$(ra_marker_path)"
    [ -n "$marker" ] && [ -f "$marker" ] || return 0
    awk -F'\t' -v m="$mode" '$1 == m && NF >= 6 {
        printf "%s\t%s\t%s\t%s\n", $3, $4, $5, $6; exit
    }' "$marker"
}

# ra_verdict_from_json <aggregate.json> — extract the verdict fields from a
# `verifier-sidecar.py aggregate` result, printed as
# `<score>\t<recommendation>\t<hard_veto>\t<veto_reason>`. Reads candidates[0]:
# the commit gate scores exactly one candidate (this diff's review), so a
# multi-candidate ranking result is out of contract here.
#
# Fails (rc 1) rather than guessing when jq is absent or the JSON has no
# candidate — a gate must not invent a verdict.
ra_verdict_from_json() {
    local file="$1"
    [ -r "$file" ] || return 1
    command -v jq >/dev/null 2>&1 || return 1
    # Validate the blocking-relevant fields BY TYPE before emitting anything.
    #
    # Every `//` default here is a chance to invent a clean verdict out of a
    # broken file, and the one that matters is `hard_veto`: `if .hard_veto then`
    # renders a MISSING key as "false", so a truncated or foreign-schema file
    # would silently report "no veto" — dropping the single signal this gate
    # blocks on. An empty `candidates` array does the same thing one level up.
    # Both fail closed instead: no output, non-zero rc, and the caller (which
    # treats that as rc 2 infra) refuses to record a presence-only ack behind
    # the author's back.
    #
    # `recommendation` and `veto_reasons` stay optional-with-default on purpose
    # — they are advisory metadata that can never block, so a missing one is not
    # worth failing a review over.
    jq -e -r '
        .candidates[0]
        | select(type == "object")
        | select((.overall_score | type) == "number")
        | select((.hard_veto | type) == "boolean")
        | [ (.overall_score | tostring),
            (.recommendation // "unknown"),
            (if .hard_veto then "true" else "false" end),
            ((.veto_reasons // []) | if type == "array" and length > 0 then (.[0] | tostring) else "" end)
          ] | @tsv
    ' "$file" 2>/dev/null
}
