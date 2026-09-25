#!/usr/bin/env bash
# 72-offline-exact.sh — Quality Pillar 6 (offline-first) EXACT rules (sourced by test-lint-rules.sh, not
# run directly). BLOCKING, delta-gated per changed file: a file fails only when it has MORE hits of a
# rule than its merge-base copy, so existing hits are grandfathered. ADR-0026.
#
# offline-write-bypasses-queue — a tracker write (comment, worklog, watcher, field update, create,
# attach, sprint) called straight on the backend outside the queue seam. Offline, that write is lost;
# route it through the offline queue so it replays on reconnect. Exempt: Source/Core/src/Tracker/ (the
# clients), Source/Core/src/Sync/ (queue + replay), FieldEditPipelineService.cpp (commit-or-queue seam).
#
# tracker-error-kind-collapsed — TrackerErrorUnknown(<one variable>) in tracker code. It throws away the
# Transport kind, so an offline failure reads as permanent and callers wipe cached data (the #21b
# collapse behind the PR #2234 postmortem). Classify where the response is in hand. The
# `classified.IsOk() ? TrackerErrorUnknown(x) : classified` fallback is allowed: each collapse on the hit
# line must itself be the true branch of an `IsOk() ?` ternary (which may wrap from the line above). An
# unrelated IsOk() check, ternary, comment text, or a valid fallback on a previous line never exempts it.
#
# Escape: a comment line // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) on the
# nearest non-blank line above the hit. A marker trailing a code line never hides that line's code.

OFFLINE_WRITE_RE='(Collaboration\(\)|Mutations\(\)|[A-Za-z_]*[Mm]utations[A-Za-z0-9_]*|[A-Za-z_]*[Cc]ollab[A-Za-z0-9_]*)[[:space:]]*(->|\.)[[:space:]]*(AddIssueCommentPlain|AddIssueCommentAnnotateContext|AddWorklog|AddIssueWatcher|UpdateIssueFields|UpdateField|CreateIssue|AttachFilesToIssue|AddIssueToSprint)[[:space:]]*\('
OFFLINE_KIND_COLLAPSE_RE='TrackerErrorUnknown\([[:space:]]*(std::move\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\)|[A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\)'

OFFLINE_KIND_FALLBACK_RE='IsOk\(\)[[:space:]]*\?[[:space:]]*TrackerErrorUnknown\('

offline_mask_kind_fallbacks() {
    # $1 = code of the hit line, $2 = code of the (up to two) lines above. Prints the hit line with every
    # `IsOk() ? TrackerErrorUnknown(` fallback collapse renamed, so only a collapse outside a fallback still
    # matches OFFLINE_KIND_COLLAPSE_RE. Handles the same-line form and a ternary clang-format wrapped
    # before `?` or before the collapse.
    local masked="$1" prevc="$2" m trimmed
    while [[ "$masked" =~ $OFFLINE_KIND_FALLBACK_RE ]]; do
        m="${BASH_REMATCH[0]}"
        masked="${masked/"$m"/IsOk() ? FallbackUnknown(}"
    done
    trimmed="${masked#"${masked%%[![:space:]]*}"}"
    if { [[ "$prevc" =~ IsOk\(\)[[:space:]]*\?[[:space:]]*$ ]] && [[ "$trimmed" =~ ^TrackerErrorUnknown\( ]]; } \
        || { [[ "$prevc" =~ IsOk\(\)[[:space:]]*$ ]] && [[ "$trimmed" =~ ^\?[[:space:]]*TrackerErrorUnknown\( ]]; }; then
        masked="${masked/TrackerErrorUnknown(/FallbackUnknown(}"
    fi
    printf '%s' "$masked"
}

offline_code_of() {
    # $1 = raw source line. Prints its code part: empty for a comment-only line, else the text before `//`.
    local l="$1" t
    t="${l#"${l%%[![:space:]]*}"}"
    case "$t" in '//'*|'*'*|'/*'*) return 0 ;; esac
    printf '%s' "${l%%//*}"
}

scan_offline_exact_file() {
    # $1 = file to read; $2 = logical repo path for scope + output (defaults to $1).
    local f="$1" logical="${2:-$1}"
    [ -f "$f" ] || return 0
    case "$logical" in Source/*.cpp|Source/*.h|Source/*.hpp) ;; *) return 0 ;; esac
    case "$logical" in */ThirdParty/*) return 0 ;; esac
    local write_scope=0 kind_scope=0
    case "$logical" in *.cpp) write_scope=1 ;; esac
    case "$logical" in
        Source/Core/src/Tracker/*|Source/Core/src/Sync/*|*/FieldEditPipelineService.cpp) write_scope=0 ;;
    esac
    case "$logical" in
        Source/Core/src/Tracker/*|Source/Core/include/Tracker/*|Source/Core/include/ITracker*.h) kind_scope=1 ;;
    esac
    [ "$write_scope" -eq 1 ] || [ "$kind_scope" -eq 1 ] || return 0
    local lineno=0 prev_dev_rule="" prev1="" prev2="" line s code
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        s="${line#"${line%%[![:space:]]*}"}"
        if [[ "$s" == '//'* || "$s" == '/*'* ]] && [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            prev2="$prev1"; prev1="$line"
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        case "$s" in '//'*|'*'*|'/*'*) prev2="$prev1"; prev1="$line"; continue ;; esac
        code="${line%%//*}"
        if [ "$write_scope" -eq 1 ] && [ "$suppress" != "offline-write-bypasses-queue" ] \
            && [[ "$code" =~ $OFFLINE_WRITE_RE ]]; then
            printf 'offline-write-bypasses-queue\t%s:%s\n' "$logical" "$lineno"
        fi
        if [ "$kind_scope" -eq 1 ] && [ "$suppress" != "tracker-error-kind-collapsed" ] \
            && [[ "$code" =~ $OFFLINE_KIND_COLLAPSE_RE ]]; then
            # Mask the collapses on THIS line that are the true branch of an `IsOk() ?` fallback (same line, or
            # wrapped from the code of the lines above), then fire only if a collapse is still left. So neither an
            # unrelated ternary nor a valid fallback on a previous line can exempt a separate collapse here.
            if [[ "$(offline_mask_kind_fallbacks "$code" "$(offline_code_of "$prev2") $(offline_code_of "$prev1")")" \
                =~ $OFFLINE_KIND_COLLAPSE_RE ]]; then
                printf 'tracker-error-kind-collapsed\t%s:%s\n' "$logical" "$lineno"
            fi
        fi
        prev2="$prev1"; prev1="$line"
    done < "$f"
}

compute_offline_exact_violations() {
    local f
    while IFS= read -r f; do [ -n "$f" ] && scan_offline_exact_file "$f"; done < <(list_first_party_cpp_files)
}

offline_delta_hits() {
    # $1 = scanner fn, $2 = merge-base, $3.. = rule ids. Scans each CHANGED first-party C++ file once at
    # HEAD and once at the merge-base, and prints the HEAD hits of every rule whose HEAD count exceeds the
    # merge-base count (a new file counts from zero, so a moved line never fails). A renamed file is
    # compared with its merge-base source path (scanned under that path's scope), so a rename alone
    # never un-grandfathers the hits it carries.
    local fn="$1" mb="$2"
    shift 2
    local changed status src f head_out base_out rule head_n base_n tmp
    changed="$(git diff --name-status -M --diff-filter=d "$mb" 2>/dev/null || true)"
    [ -n "$changed" ] || return 0
    tmp="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/offline_delta.$$")"
    while IFS=$'\t' read -r status src f; do
        case "$status" in R*|C*) ;; *) f="$src" ;; esac
        [[ "$f" =~ ^Source/.*\.(cpp|h|hpp)$ ]] || continue
        case "$f" in */ThirdParty/*) continue ;; esac
        head_out="$("$fn" "$f" "$f")"
        [ -n "$head_out" ] || continue
        base_out=""
        if git show "$mb:$src" > "$tmp" 2>/dev/null; then
            base_out="$("$fn" "$tmp" "$src")"
        fi
        for rule in "$@"; do
            head_n="$(printf '%s\n' "$head_out" | grep -cF "${rule}"$'\t' || true)"
            [ "$head_n" -gt 0 ] || continue
            base_n="$(printf '%s\n' "$base_out" | grep -cF "${rule}"$'\t' || true)"
            if [ "$head_n" -gt "$base_n" ]; then
                printf '%s\n' "$head_out" | grep -F "${rule}"$'\t'
            fi
        done
    done <<< "$changed"
    rm -f "$tmp" 2>/dev/null || true
}
