#!/usr/bin/env bash
# 80-interface-doc.sh — interface-doc symbol-pinned WARN map (sourced by test-lint-rules.sh, not run directly).

# --- interface-doc WARN map (Gap B / Slice 2) — symbol-pinned, NOT coarse header-touched. ---
# Each entry: "<changed-header-regex>|||<leaf-doc>". The rule WARNs only when a `Type::method`
# token *embedded in the leaf doc* also appears in a changed header's diff hunk AND the doc was
# NOT touched — i.e. the doc pins a contract the interface just changed without the doc following.
# A coarse "header changed without doc changed" heuristic was rejected after a noise spike
# (2026-06-08): ~every Tracker Result<>-migration commit would fire (the leaf doc embeds essentially
# one signature). Symbol-pinning fires ~only on genuine drift (the ITrackerIssueMutations::UpdateField
# slip class). NO C++ signature/param parsing. KEEP this list curated + IN SYNC with AGENTS.md.
INTERFACE_DOC_MAP=(
    'Source/Core/include/(ITracker[A-Za-z]*|Tracker/[A-Za-z]*Client)\.h|||Source/Core/src/Tracker/AGENTS.md'
)

# interface_doc_emit <doc> <doc_changed:0|1> <pins_newline> <header_hunk_text>
# Pure decision core (no git) — shared by the diff gate and --selftest. WARNs (stderr, never
# changes exit code) for each doc-pinned `Type::method` whose <method> appears on an added/removed
# line of the changed interface header(s), unless the doc itself was changed in the same diff.
interface_doc_emit() {
    local doc="$1" doc_changed="$2" pins="$3" hunk="$4"
    [ "$doc_changed" = "1" ] && return 0
    [ -n "$pins" ] || return 0
    local pin method
    while IFS= read -r pin; do
        [ -n "$pin" ] || continue
        method="${pin##*::}"
        if printf '%s\n' "$hunk" | grep -qE "\\b${method}\\b"; then
            echo "[interface-doc] WARN: ${doc} documents \`${pin}\` but the interface diff changed \`${method}\` without touching ${doc} — confirm the documented contract still matches (advisory; not blocking; suppress by updating the doc or removing the stale symbol ref)." >&2
        fi
    done <<< "$pins"
}

# interface_doc_warn <base-ref> — git wrapper: resolve merge-base, for each map entry collect the
# changed headers + the doc's pinned `Type::method` tokens + the header diff hunk, then delegate to
# interface_doc_emit. WARN-only (calibration phase, mirrors the dup gate); never touches exit code.
interface_doc_warn() {
    local base="$1"
    local mb; mb="$(git merge-base "$base" HEAD 2>/dev/null || echo "$base")"
    local changed; changed="$(git diff --name-only "$mb" 2>/dev/null || true)"
    [ -n "$changed" ] || return 0
    local entry hdr_re doc chdrs doc_changed pins hunk
    for entry in "${INTERFACE_DOC_MAP[@]}"; do
        hdr_re="${entry%%|||*}"; doc="${entry##*|||}"
        chdrs="$(printf '%s\n' "$changed" | grep -E "^${hdr_re}$" || true)"
        [ -n "$chdrs" ] || continue
        doc_changed=0; printf '%s\n' "$changed" | grep -qxF "$doc" && doc_changed=1
        [ "$doc_changed" = "1" ] && continue
        [ -f "$doc" ] || continue
        # `Type::method` tokens the doc pins (dedup). No backtick/param requirement — a bare
        # qualified-id is the signal; the method-in-hunk check is what scopes it to real drift.
        pins="$(grep -oE '[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*' "$doc" | sort -u || true)"
        [ -n "$pins" ] || continue
        # Added/removed lines of just the changed interface headers (word-quote $chdrs intentionally
        # unquoted so multiple paths expand as separate pathspecs).
        # shellcheck disable=SC2086
        hunk="$(git diff "$mb" -- $chdrs 2>/dev/null | grep -E '^[+-]' || true)"
        interface_doc_emit "$doc" "$doc_changed" "$pins" "$hunk"
    done
}
