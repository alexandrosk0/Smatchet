#!/usr/bin/env bash
# 60-json-walker.sh — unbounded-recursive-json-walker rule (advisory) (sourced by test-lint-rules.sh, not run directly).

# unbounded-recursive-json-walker — WARN-first over first-party C++ (calibration; never blocks).
# The "DW" (depth-bounded walker) class from the security campaign: a hand-rolled recursive walk
# over attacker-influenced JSON/Lua (AdfToMarkdown, NormalizeTrackerFieldValue, LuaObjectToIssue-
# FieldString) overflows the C++ stack on deep nesting even when the PARSE was bounded — "bounding
# the parse alone does not bound a hand-written recursion" (docs/plans/active/cpp-security-
# hardening.md § Approach; recurrences #1220 / #1237). The bare-json gate is structurally blind to
# this shape, so it gets its own rule.
#
# Heuristic (text proxy, NOT an AST): a COLUMN-0 free-function definition whose PARAMETER LIST
# carries a `nlohmann::json` / `sol::object` token, whose body (up to the next column-0 `}`)
# calls the function's own name, and where neither signature nor body mentions a depth/budget
# token — i.e. a self-recursive JSON walker with no visible bound. Overloaded bounded wrappers,
# mutual recursion, and multi-line signatures are out of scope (WARN tier; the calibration run
# decides whether to graduate). A `// SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker; ...)`
# above the definition escapes.

scan_json_walker_file() {
    # $1 = a first-party C++ file. Emits `unbounded-recursive-json-walker\t<f>:<line>` per
    # unbounded self-recursive json/sol walker definition.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    grep -qE 'nlohmann::json|sol::object' "$f" 2>/dev/null || return 0
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i
    for ((i = 0; i < n; i++)); do
        local raw="${lines[$i]}"
        case "$raw" in [![:space:]]*) ;; *) continue ;; esac     # column-0 defs only
        case "$raw" in *'('*) ;; *) continue ;; esac
        # The json/sol token must sit in the PARAMS (after the first open paren), not just the
        # return type.
        local params="${raw#*\(}"
        case "$params" in *nlohmann::json*|*sol::object*) ;; *) continue ;; esac
        usc_is_def_line "$raw" || continue                        # free-function def shape
        local name="$USC_DEF_NAME"
        # Depth/budget token in the signature = bounded walker (comment-stripped view, so a
        # trailing `// TODO add depth cap` cannot mark an unbounded walker as bounded).
        local bounded=0 def_code="${raw%%//*}"
        case "$def_code" in *depth*|*Depth*|*budget*|*Budget*) bounded=1 ;; esac
        # SMATCHET_DEVIATION above the definition escapes.
        local k dev=0 cnt=0
        for ((k = i - 1; k >= 0 && cnt < 3; k--)); do
            case "${lines[$k]}" in '') continue ;; esac
            cnt=$((cnt + 1))
            case "${lines[$k]}" in *'SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker'*) dev=1; break ;; esac
        done
        [ "$dev" -eq 1 ] && continue
        # Body = up to the next column-0 `}`. Look for a self-call + any depth/budget token.
        local selfcall=0 j2 callre='(^|[^A-Za-z0-9_:.>"])'"$name"'[[:space:]]*\('
        for ((j2 = i + 1; j2 < n; j2++)); do
            local b="${lines[$j2]}"
            case "$b" in '}'*) break ;; esac
            local bs="${b#"${b%%[![:space:]]*}"}"
            case "$bs" in '//'*|'*'*|'/*'*) continue ;; esac
            local b_code="${b%%//*}"
            case "$b_code" in *depth*|*Depth*|*budget*|*Budget*) bounded=1 ;; esac
            if [[ "$b_code" =~ $callre ]]; then selfcall=1; fi
        done
        if [ "$selfcall" -eq 1 ] && [ "$bounded" -eq 0 ]; then
            printf 'unbounded-recursive-json-walker\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_json_walker_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(list_first_party_cpp_files)
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_json_walker_file "$f"; done
}
