#!/usr/bin/env bash
# 50-bare-json.sh — bare-json-parse-untrusted rule (sourced by test-lint-rules.sh, not run directly).

# bare-json-parse-untrusted — BLOCKING over ALL first-party C++ (.cpp/.h/.hpp), repo-wide.
# A bare `nlohmann::json::parse(` (or `json::parse(`) on bytes that cross a trust boundary and do
# NOT route through smatchet::json_safe::ParseBounded is a DoS vector: nlohmann builds the DOM
# iteratively, but a deeply-nested payload ("[[[[...]]]]") overflows the C++ stack in the RECURSIVE
# ~json DOM teardown — an uncatchable crash no try/catch intercepts (issues #1271/#1287/#1290; the
# 3-arg non-throwing form still builds the full DOM). ParseBounded caps depth + node count + byte
# size and never throws, so it is the mandated parse wherever the bytes are not provably
# program-internal.
#
# Why repo-wide default-deny replaced the old curated BARE_JSON_INGRESS_TUS basename allow-list:
# the list lagged the code every time the class recurred — the SECURITY_AUDIT.md sweep (#1566) had
# to extend it, #1573 found an uncurated sibling (TrackerFieldValueParser), and #1592/#1598 then
# fixed bare parses in LinearClient / GitHubClient / JiraClient / PlaneClient, none of which were
# in the curated set, so a regression there would have landed silently. CPP_CODE_AUDIT.md also
# found the class in a HEADER (JsonParseUtil.h) and in the `file >> j` operator>> form the parse
# regex cannot see. Default-deny inverts the maintenance burden: EVERY first-party parse is gated,
# and a site whose bytes ARE program-internal carries an explicit, audit-able
# `// SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; ...)` on the line above. The tree is
# clean today (bounded, or deviation-annotated), so any hit is a regression.
#
# Also matched: the `<stream> >> <json-var>` slurp form (operator>> builds the same unbounded DOM;
# CPP_CODE_AUDIT #11 — ConfigManager_Views/Panes bypassed LoadJsonFile's 64 MiB cap this way). To
# keep false positives ~0 the slurp form fires only when <json-var> was declared `nlohmann::json`
# within the preceding 8 lines (the idiomatic declare-then-slurp shape).

scan_bare_json_parse_file() {
    # $1 = a first-party C++ file (.cpp/.h/.hpp — headers hold inline ingress helpers too). Emits
    # `bare-json-parse-untrusted\t<f>:<line>` per bare json::parse( / stream>>json slurp not routed
    # via ParseBounded.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    # Cheap pre-filter: a file with no json token at all cannot violate — keeps the whole-tree
    # campaign sweep and the --diff base-worktree scan fast (most files skip here).
    grep -qE 'json::parse|nlohmann::json' "$f" 2>/dev/null || return 0
    local lineno=0 prev_dev_rule=""
    # Declare-then-slurp tracking for the operator>> form: the identifiers declared as
    # `nlohmann::json <name>` and the line each was last declared on (parallel indexed arrays —
    # no bash-4 associative-array dependency for the git-bash toolchain).
    local -a jv_names=() jv_lines=()
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        # Capture a SMATCHET_DEVIATION on the line ABOVE to suppress the next non-blank line.
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        # Skip pure-comment lines (prose mentions of json::parse don't fire).
        if [[ "$line" =~ ^[[:space:]]*(//|\*|/\*) ]]; then continue; fi
        # Strip a trailing // comment before matching: otherwise a real parse followed by a
        # quoted mention in the trailing comment (`json::parse(x); // logs "json::parse"`) would
        # match the string-literal exemption and be silently skipped — a false negative in a
        # blocking gate — and, conversely, a trailing-comment mention would false-positive the
        # match itself. (A `//` inside a string literal truncates early; that only narrows an
        # EXEMPTION, so the gate fails closed and a deviation escapes it.)
        local code_only="${line%%//*}"
        # Record `nlohmann::json <ident>` declarations for the slurp form below.
        if [[ "$code_only" =~ (^|[^A-Za-z_:])nlohmann::json[[:space:]]+([A-Za-z_][A-Za-z0-9_]*) ]]; then
            jv_names+=("${BASH_REMATCH[2]}"); jv_lines+=("$lineno")
        fi
        # Match a bare json::parse( call. ParseBounded / ParseBoundedOrDiscarded are the mandated
        # forms, so a line that already routes through them is clean.
        case "$code_only" in
            *ParseBounded*) continue ;;
            *'"'*'json::parse'*'"'*) continue ;;   # `json::parse` inside a string literal
        esac
        if [[ "$code_only" =~ (^|[^A-Za-z_:])(nlohmann::)?json::parse[[:space:]]*\( ]]; then
            if [ "$suppress" != "bare-json-parse-untrusted" ]; then
                printf 'bare-json-parse-untrusted\t%s:%s\n' "$f" "$lineno"
            fi
            continue
        fi
        # operator>> slurp into a json var declared within the preceding 8 lines.
        if [[ "$code_only" == *'>>'* ]] && [ "${#jv_names[@]}" -gt 0 ]; then
            local vi slurpre
            for ((vi = ${#jv_names[@]} - 1; vi >= 0; vi--)); do
                [ $((lineno - jv_lines[vi])) -le 8 ] || break
                slurpre='>>[[:space:]]*'"${jv_names[vi]}"'([^A-Za-z0-9_]|$)'
                if [[ "$code_only" =~ $slurpre ]]; then
                    if [ "$suppress" != "bare-json-parse-untrusted" ]; then
                        printf 'bare-json-parse-untrusted\t%s:%s\n' "$f" "$lineno"
                    fi
                    break
                fi
            done
        fi
    done < "$f"
}

compute_bare_json_parse_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(list_first_party_cpp_files)
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_bare_json_parse_file "$f"; done
}
