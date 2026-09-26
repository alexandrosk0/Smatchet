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
# Both rules read only CODE: comments and literal contents are stripped first (offline_code_lines), so a
# comment or string never fires and code before or after a comment on the same line is still scanned.
#
# Escape: a comment-only line // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) on the
# nearest non-blank line above the hit. A marker on a line that also holds code never hides that code.

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

# One output line per input line holding only that line's CODE: `//` and `/* */` comments (tracked across lines)
# are removed, and string / char / raw-string literal contents are blanked to `""` (raw strings tracked across
# lines). So a comment never counts as code, code around a comment is still scanned, and a `//` or `/*` inside
# a literal changes nothing. A `'` after an identifier or number that is not a char prefix (L u U u8) is a C++14
# digit separator, not a char literal.
# shellcheck disable=SC2016  # an awk program: its $0 / fields are awk's, never shell expansions.
OFFLINE_CODE_LEXER_AWK='
function prev_ident(s, i,   k) {
    k = i - 1
    while (k >= 1 && substr(s, k, 1) ~ /[A-Za-z0-9_]/) k--
    return substr(s, k + 1, i - 1 - k)
}
function skip_quoted(s, i, q, n,   d) {
    while (i <= n) {
        d = substr(s, i, 1)
        if (d == "\\") { i += 2; continue }
        if (d == q) return i + 1
        i++
    }
    return n + 1
}
{
    line = $0; n = length(line); out = ""; i = 1
    while (i <= n) {
        if (blk) {
            j = index(substr(line, i), "*/")
            if (j == 0) break
            i += j + 1; blk = 0; out = out " "; continue
        }
        if (rawend != "") {
            j = index(substr(line, i), rawend)
            if (j == 0) break
            i += j - 1 + length(rawend); rawend = ""; out = out "\""; continue
        }
        c = substr(line, i, 1); c2 = substr(line, i, 2)
        if (c2 == "//") break
        if (c2 == "/*") { blk = 1; i += 2; continue }
        if (c == "\"") {
            t = prev_ident(line, i)
            j = index(substr(line, i + 1), "(")
            if ((t == "R" || t == "LR" || t == "uR" || t == "UR" || t == "u8R") && j > 0) {
                rawend = ")" substr(line, i + 1, j - 1) "\""
                out = out "\""; i += j + 1; continue
            }
            out = out "\"\""; i = skip_quoted(line, i + 1, "\"", n); continue
        }
        if (c == "\047") {
            t = prev_ident(line, i)
            if (t == "" || t == "L" || t == "u" || t == "U" || t == "u8") {
                out = out "\047\047"; i = skip_quoted(line, i + 1, "\047", n); continue
            }
        }
        out = out c; i++
    }
    print out
}'

offline_code_lines() {
    # $1 = file. Prints the code of every line (see OFFLINE_CODE_LEXER_AWK), one output line per input line.
    awk "$OFFLINE_CODE_LEXER_AWK" "$1"
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
    local lineno=0 prev_dev_rule="" prev1="" prev2="" line code suppress body kv kvs
    # fd 3 = the raw lines (deviation markers live in comments), fd 4 = their code (offline_code_lines).
    # shellcheck disable=SC2094  # both descriptors only READ $f (raw lines + their code); nothing writes it.
    while { IFS= read -r line <&3 || [ -n "$line" ]; } && IFS= read -r code <&4; do
        lineno=$((lineno+1))
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        suppress="$prev_dev_rule"; prev_dev_rule=""
        if [[ "$code" =~ ^[[:space:]]*$ ]]; then
            # A comment-only line: a SMATCHET_DEVIATION on it escapes the next non-blank line. A marker on a line
            # that also holds code never hides that code.
            if [[ "$line" =~ $DEV_RE ]]; then
                body="${BASH_REMATCH[1]}"
                IFS=';' read -ra kvs <<< "$body"
                for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            fi
            continue
        fi
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
    done 3< "$f" 4< <(offline_code_lines "$f")
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
