#!/usr/bin/env bash
# 55-catch-all.sh — catch-all-swallow rule (sourced by test-lint-rules.sh, not run directly).

# catch-all-swallow — BLOCKING absolute-0 over ALL first-party C++ (.cpp/.h/.hpp).
# An EMPTY `catch (...) { }` body silently swallows every exception — including the ones the
# exception-handling-policy tiers require to be logged/rethrown — and is a review CRITICAL
# (docs/agent-rules/exception-handling-policy.md hard rule 1: "must log or have inline comment
# justifying silence"). CPP_CODE_AUDIT.md found the missing-guard sibling of this class in the
# Cache/DB tier (#6), and the editor-side hook (docs/harness/claude-code/hooks/lint-catch-all.py)
# already flags it — but a Claude-Code hook is not a CI merge gate, so nothing mechanically
# blocked the pattern from landing. The tree is at 0 empty catch-all bodies today, so any hit is
# a regression (absolute-0, no grandfathering — same model as no-glfw / no-raw-new).
#
# Escapes (matching the policy + the existing hook vocabulary):
#   - a comment INSIDE the body (documented, justified silence) — does not fire;
#   - `// catch-all-ok: <reason>` on the catch line — does not fire;
#   - `// SMATCHET_DEVIATION(rule=catch-all-swallow; ...)` above the catch — does not fire.
# A body with any statement does not fire either (the "catch without LOG" tier stays advisory in
# the editor hook — this CI rule gates only the unambiguous empty-swallow shape).

scan_catch_all_swallow_file() {
    # $1 = a first-party C++ file. Emits `catch-all-swallow\t<f>:<line>` per EMPTY catch (...) body.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    grep -qE 'catch[[:space:]]*\([[:space:]]*\.\.\.' "$f" 2>/dev/null || return 0
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i
    for ((i = 0; i < n; i++)); do
        local raw="${lines[$i]}"
        local s="${raw#"${raw%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac          # comment mention, not code
        # Match on the comment-stripped view so a trailing `// ... catch (...) ...` mention can
        # neither fire the rule nor (via its quotes) mask a real catch on the same line.
        local code_only="${raw%%//*}"
        [[ "$code_only" =~ catch[[:space:]]*\([[:space:]]*\.\.\.[[:space:]]*\) ]] || continue
        case "$code_only" in *'"'*catch*'"'*) continue ;; esac   # string-literal mention
        case "$raw" in *'catch-all-ok:'*) continue ;; esac       # hook-vocabulary escape (a comment)
        # SMATCHET_DEVIATION(rule=catch-all-swallow) on the nearest preceding non-blank lines.
        local k dev=0 cnt=0
        for ((k = i - 1; k >= 0 && cnt < 3; k--)); do
            case "${lines[$k]}" in '') continue ;; esac
            cnt=$((cnt + 1))
            case "${lines[$k]}" in *'SMATCHET_DEVIATION(rule=catch-all-swallow'*) dev=1; break ;; esac
        done
        [ "$dev" -eq 1 ] && continue
        # Body = text between the `{` after the catch closer and the FIRST `}` (an empty body has
        # no nested braces, so the first close IS the close). Window 10 lines; no close found in
        # the window -> a long body -> not empty -> skip.
        local after="${raw#*catch}"
        after="${after#*\)}"
        local bodytext="" found_open=0 closed=0 j2 seg
        for ((j2 = i; j2 < n && j2 <= i + 10; j2++)); do
            if [ "$j2" -eq "$i" ]; then seg="$after"; else seg="${lines[$j2]}"; fi
            if [ "$found_open" -eq 0 ]; then
                case "$seg" in
                    *'{'*) found_open=1; seg="${seg#*\{}" ;;
                    *) continue ;;
                esac
            fi
            case "$seg" in
                *'}'*) bodytext="$bodytext${seg%%\}*}"; closed=1; break ;;
                *)     bodytext="$bodytext$seg"$'\n' ;;
            esac
        done
        [ "$closed" -eq 1 ] || continue
        # A comment inside the body is the policy's documented-silence escape; the catch-all-ok
        # vocabulary inside the body also escapes.
        case "$bodytext" in *'//'*|*'/*'*) continue ;; esac
        if [[ "$bodytext" =~ ^[[:space:]]*$ ]]; then
            printf 'catch-all-swallow\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_catch_all_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(list_first_party_cpp_files)
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_catch_all_swallow_file "$f"; done
}
