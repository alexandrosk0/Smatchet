#!/usr/bin/env bash
# 10-line-rules.sh — per-line grep rules (printf/new/detach/glfw/imgui/deviation-overdue) + strict/wide/glfw sweeps (sourced by test-lint-rules.sh, not run directly).

scan_file_rules() {
    # $1 = file. Emits triples for all line/grep rules + deviation-overdue.
    local f="$1"
    [ -f "$f" ] || return 0
    local lineno=0 prev_dev_rule="" prev_dev_revisit="" prev_dev_revisit_seen=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))

        # Capture a SMATCHET_DEVIATION comment to suppress the NEXT non-blank line.
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}"
            prev_dev_rule=""; prev_dev_revisit=""; prev_dev_revisit_seen=""
            local kv
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do
                # Trim the whole field, not just one leading space: `  revisit=x` missed the
                # `revisit=*` glob entirely and folded into the absent-field case below.
                dev_trim "$kv"; kv="$DEV_TRIMMED"
                case "$kv" in
                    rule=*)    prev_dev_rule="${kv#rule=}" ;;
                    # Trim the VALUE too. `revisit=2099-12-31 ` kept its trailing space, missed the
                    # 10-char date glob and was reported overdue on a 2099 date; `revisit= 2020-01-01`
                    # kept its leading space, missed the date shape entirely and bought a permanent
                    # silent exemption on a past date. Presence is tracked apart from the value so an
                    # EMPTY `revisit=` fails closed below instead of folding into absent-field.
                    revisit=*) dev_trim "${kv#revisit=}"; prev_dev_revisit="$DEV_TRIMMED"
                               prev_dev_revisit_seen=1 ;;
                esac
            done
            # deviation-overdue fires on the comment line itself. `revisit=` typed but left EMPTY is
            # a malformed attempt, not the sanctioned absent-field case: fail closed, matching the
            # reference implementation, rather than mint a permanent exemption from a typo.
            if [ -n "$prev_dev_revisit_seen" ] && \
               { [ -z "$prev_dev_revisit" ] || revisit_overdue "$prev_dev_revisit"; }; then
                printf 'deviation-overdue\t%s:%s\t%s\n' "$f" "$lineno" "$line"
            fi
            continue
        fi

        # Blank lines do NOT consume an active deviation — it covers the next
        # NON-blank line (CR #507). Skip before touching prev_dev_rule.
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi

        # define-imgui — macro-alias trick.
        if [[ "$line" =~ \#define[[:space:]]+ImGui ]]; then
            printf 'define-imgui\t%s:%s\t%s\n' "$f" "$lineno" "$line"
        fi

        # Determine if the active SMATCHET_DEVIATION suppresses a rule on THIS line.
        local suppress="$prev_dev_rule"
        prev_dev_rule=""; prev_dev_revisit=""; prev_dev_revisit_seen=""   # covers next non-blank line only

        # Skip pure-comment lines (leading //, leading * doc-continuation, leading /*)
        # for the printf/new heuristics — these are prose, not code.
        if [[ "$line" =~ ^[[:space:]]*(//|\*|/\*) ]]; then continue; fi

        # no-printf-stderr (bash regex — no subprocess per line). Word-ish
        # boundary: token at start or preceded by a non-identifier char.
        if [[ "$line" =~ (^|[^A-Za-z_])(printf|fprintf|std::cerr|std::cout)([^A-Za-z0-9_]|$) ]]; then
            case "$line" in *'#include'*) :;; *)
                if ! has_inline_exempt "$line" && [ "$suppress" != "no-printf-stderr" ]; then
                    printf 'no-printf-stderr\t%s:%s\t%s\n' "$f" "$lineno" "$line"
                fi ;;
            esac
        fi

        # no-raw-new — `new Type` (capitalised). Exclude identifiers containing
        # "new" and `new` appearing inside a string literal.
        if [[ "$line" =~ (^|[^A-Za-z_])new[[:space:]]+[A-Z] ]]; then
            case "$line" in
                *_new*|*new_*|*renewed*) : ;;
                *'"'*new*'"'*) : ;;          # `new` inside a string literal
                *)
                    if ! has_inline_exempt "$line" && [ "$suppress" != "no-raw-new" ]; then
                        printf 'no-raw-new\t%s:%s\t%s\n' "$f" "$lineno" "$line"
                    fi ;;
            esac
        fi

        # no-detach — `.detach()` is forbidden first-party-wide. A raw
        # std::thread(...).detach() leaves an unjoined worker that use-after-frees
        # captured app/backend/dispatcher state when ~AppController runs mid-flight
        # (Pillar-3 never-crash). Use the joined AppController::LaunchBackgroundTask
        # pool (joined at shutdown via JoinBackgroundTasks) instead. Pure-comment
        # lines are already skipped above, so prose mentions of `.detach()` (incl.
        # migration notes) don't fire; also exclude string-literal mentions.
        if [[ "$line" =~ \.detach\(\) ]]; then
            case "$line" in
                *'"'*.detach*'"'*) : ;;      # `.detach()` inside a string literal
                *)
                    # no-detach is an ABSOLUTE rule: legacy inline markers (// CLI stdout / // pimpl
                    # / etc., via has_inline_exempt) must NOT suppress it — only an explicit
                    # SMATCHET_DEVIATION(rule=no-detach; ...) may (CodeRabbit PR #657).
                    if [ "$suppress" != "no-detach" ]; then
                        printf 'no-detach\t%s:%s\t%s\n' "$f" "$lineno" "$line"
                    fi ;;
            esac
        fi

        # no-glfw-in-core-headers — GLFW / glad / OpenGL includes must not appear in
        # a Source/Core/ HEADER: the DX12 target (SmatchetCore_DX12) compiles those
        # headers too and has no GL/GLFW toolchain, so a window-system include there
        # breaks the dual-target build (AGENTS.md § Project rules § Don't). Fires only
        # for header files (.h/.hpp) — GLFW in a .cpp or in Source/Standalone is fine;
        # the diff/wide gate restricts the enforced surface to Source/Core/include/.
        # Pure-comment lines are already skipped above; the string-literal guard keeps
        # a path mentioned in a string from firing. Absolute-0: any hit = regression,
        # only a SMATCHET_DEVIATION(rule=no-glfw-in-core-headers; ...) above escapes.
        case "$f" in
            *.h|*.hpp)
                if [[ "$line" =~ \#include[[:space:]]*[\<\"](GLFW/|glad|GL/gl) ]] \
                   || [[ "$line" =~ \#include[[:space:]]*[\<\"][^\>\"]*glfw3\.h ]]; then
                    case "$line" in
                        *'"'*'#include'*) : ;;   # `#include` inside a string literal
                        *)
                            if [ "$suppress" != "no-glfw-in-core-headers" ]; then
                                printf 'no-glfw-in-core-headers\t%s:%s\t%s\n' "$f" "$lineno" "$line"
                            fi ;;
                    esac
                fi ;;
        esac
    done < "$f"
}

# Produce the full strict-zone violator triple-set for a working tree.
# Output: sorted lines `<rule>\t<basename>\t<hash>` (one per violation).
compute_strict_triples() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        # Discover canonical (Source/…) roots AND the pre-consolidation legacy
        # roots (Source_Core/…, Plugins/…). The legacy globs let the --diff base
        # scan — which re-runs THIS scanner against an origin/develop worktree —
        # find the same grandfathered strict-zone violators while develop still
        # carries the old layout (refactor/source-root-consolidation). Once
        # develop adopts the Source/ layout the legacy globs match nothing and
        # are harmless; remove them in a follow-up cleanup. Triple keys are
        # (rule, basename, hash) — path-independent — so a legacy-path match
        # cancels its new-path twin exactly.
        git ls-files \
            'Source/Core/src/**' 'Source/Core/include/**' 'Source/Plugins/Mcp/**' \
            'Source_Core/src/**'  'Source_Core/include/**'  'Plugins/Mcp/src/**' \
            2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' || true
    )
    local strict_files=()
    for f in "${files[@]}"; do
        # Classify on the canonicalised path (legacy roots -> Source/ layout) so
        # zone_of + STRICT_GLOBS stay single-sourced against AGENTS.md and the
        # --selftest assertion is untouched; scan the REAL path so the file opens.
        local canon="$f"
        case "$f" in
            Source_Core/*) canon="Source/Core/${f#Source_Core/}" ;;
            Plugins/*)     canon="Source/Plugins/${f#Plugins/}" ;;
        esac
        [ "$(zone_of "$canon")" = strict ] && strict_files+=("$f")
    done
    {
        for f in "${strict_files[@]}"; do scan_file_rules "$f"; done
        scan_narrowing "${strict_files[@]}"
    } | while IFS=$'\t' read -r rule loc raw; do
        printf '%s\t%s\t%s\n' "$rule" "$(basename "${loc%%:*}")" "$(snippet_hash "$raw")"
    done | sort -u
}

# First-party C++ violations for the always-on (absolute) rules. `no-raw-new` and
# `deviation-overdue` are enforced at 0 across ALL first-party C++ — not just the
# strict zone — over the comment_audit.py SWEEP_ROOTS (Source/Core, Source/Plugins,
# Source/Standalone; ThirdParty + tests excluded). Emits `<rule>\t<path>:<line>`.
# Exemption markers (// C-ABI handle / // custom-deleter / // pimpl) and
# SMATCHET_DEVIATION(rule=...) still suppress — that handling lives in scan_file_rules.
compute_wide_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_file_rules "$f"; done \
        | awk -F'\t' '$1=="no-raw-new" || $1=="deviation-overdue" || $1=="no-detach" { print $1"\t"$2 }'
    return 0
}

# no-glfw-in-core-headers — ABSOLUTE-0 over Source/Core/include HEADERS only (.h/.hpp).
# The DX12 target (SmatchetCore_DX12) compiles these headers and ships no GL/GLFW
# toolchain, so a window-system include here breaks the dual-target build. The tree is
# GLFW-clean today, so any hit is a regression (no grandfathering — same model as
# no-raw-new). A SMATCHET_DEVIATION(rule=no-glfw-in-core-headers; ...) above the include
# escapes (handled in scan_file_rules). Emits `<rule>\t<path>:<line>`.
compute_glfw_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/include/**' \
            2>/dev/null \
        | grep -E '\.(h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_file_rules "$f"; done \
        | awk -F'\t' '$1=="no-glfw-in-core-headers" { print $1"\t"$2 }'
    return 0
}
