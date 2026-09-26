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
# line must itself be the true branch of a ternary whose WHOLE condition is one unnegated `<recv>.IsOk()`
# (the ternary may wrap across the two code lines above). A negated or compound condition (`!c.IsOk()`,
# `a.IsOk() || c.IsOk()`), an unrelated IsOk() check or ternary, comment text, or a valid fallback on a
# previous line never exempts it.
#
# Escape: a comment line // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) on the
# nearest non-blank line above the hit. A marker trailing a code line never hides that line's code.

OFFLINE_WRITE_RE='(Collaboration\(\)|Mutations\(\)|[A-Za-z_]*[Mm]utations[A-Za-z0-9_]*|[A-Za-z_]*[Cc]ollab[A-Za-z0-9_]*)[[:space:]]*(->|\.)[[:space:]]*(AddIssueCommentPlain|AddIssueCommentAnnotateContext|AddWorklog|AddIssueWatcher|UpdateIssueFields|UpdateField|CreateIssue|AttachFilesToIssue|AddIssueToSprint)[[:space:]]*\('
OFFLINE_KIND_COLLAPSE_RE='TrackerErrorUnknown\([[:space:]]*(std::move\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\)|[A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\)'

# `<receiver>.IsOk()` where the receiver is an identifier chain (`a.b`, `a->b`, `a.b()`).
OFFLINE_ISOK_RECV_RE='[A-Za-z_][A-Za-z0-9_]*(\(\))?([[:space:]]*(\.|->)[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(\(\))?)*[[:space:]]*(\.|->)[[:space:]]*IsOk\(\)'
# What may sit right before a ternary condition: `(` `,` `{` `;` `?`, an assignment `=` (not `==` `!=` `<=`
# `>=`), a ternary `:` (not `::`), or `return`. Every other operator binds tighter than `?:`, so it would be
# part of the condition (`!c`, `a || c`, `x == c`) and the condition would no longer be the IsOk() check.
OFFLINE_COND_BOUNDARY_RE='(([(,{;?]|[^=!<>]=|[^:]:)[[:space:]]*|[^A-Za-z0-9_]return[[:space:]]+)'
# Code before a collapse that makes it the fallback: the condition is exactly `<recv>.IsOk()`, then `?`.
OFFLINE_FALLBACK_PREFIX_RE="${OFFLINE_COND_BOUNDARY_RE}${OFFLINE_ISOK_RECV_RE}"'[[:space:]]*\?[[:space:]]*$'

offline_kind_collapse_unexempt() {
    # $1 = code of the hit line, $2 = code of the (up to two) code lines above. Succeeds when some collapse on
    # the hit line is not the true branch of a fallback ternary: the code before it (the lines above joined,
    # then this line up to the collapse) must end in `<boundary> <recv>.IsOk() ?`.
    local rest="$1" seen=" $2 " m pre
    while [[ "$rest" =~ $OFFLINE_KIND_COLLAPSE_RE ]]; do
        m="${BASH_REMATCH[0]}"
        pre="${rest%%"$m"*}"
        [[ "$seen$pre" =~ $OFFLINE_FALLBACK_PREFIX_RE ]] || return 0
        seen="$seen$pre$m"
        rest="${rest#*"$m"}"
    done
    return 1
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
    local lineno=0 prev_dev_rule="" prev1="" prev2="" in_block=0 line s code
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        # Inside a multi-line /* ... */ block nothing is code (so it never enters the prev1/prev2 window).
        if [ "$in_block" -eq 1 ]; then
            case "$line" in *'*/'*) in_block=0 ;; esac
            continue
        fi
        s="${line#"${line%%[![:space:]]*}"}"
        case "$s" in '/*'*) case "$s" in *'*/'*) ;; *) in_block=1 ;; esac ;; esac
        if [[ "$s" == '//'* || "$s" == '/*'* ]] && [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac
        code="${line%%//*}"
        if [ "$write_scope" -eq 1 ] && [ "$suppress" != "offline-write-bypasses-queue" ] \
            && [[ "$code" =~ $OFFLINE_WRITE_RE ]]; then
            printf 'offline-write-bypasses-queue\t%s:%s\n' "$logical" "$lineno"
        fi
        if [ "$kind_scope" -eq 1 ] && [ "$suppress" != "tracker-error-kind-collapsed" ] \
            && [[ "$code" =~ $OFFLINE_KIND_COLLAPSE_RE ]]; then
            # Each collapse on THIS line is checked on its own against the code right before it (this line plus
            # the two code lines above, for a clang-format-wrapped ternary). So neither an unrelated ternary nor a
            # valid fallback on a previous line or elsewhere on this line can exempt a separate collapse.
            if offline_kind_collapse_unexempt "$code" "$prev2 $prev1"; then
                printf 'tracker-error-kind-collapsed\t%s:%s\n' "$logical" "$lineno"
            fi
        fi
        prev2="$prev1"; prev1="$code"
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
