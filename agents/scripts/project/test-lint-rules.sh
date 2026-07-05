#!/usr/bin/env bash
# test-lint-rules.sh — tiered high-integrity C++ enforcement for Smatchet.
#
# Plan: docs/plans/shipped/high-integrity-cpp-enforcement.md (precursor reorg #505).
#
# Zones (single source of truth = AGENTS.md § Tiered enforcement; the globs
# below are asserted identical to AGENTS.md by --selftest):
#   strict — Source/Core/src/{Tracker,Sync,Persistence,Config,Commands}, Source/Plugins/Mcp
#            + matching include/ subdirs. Any rule violation here FAILS.
#   light  — Source/Core/src/Ui, Source/Standalone. Not gated (existing inline
#            exemption-comment vocabulary continues to apply).
#   exempt — ThirdParty, build, non-C++ trees. Not scanned.
#
# Rule ids (stable kebab-case; the linkage between scanner output, the catalog
# section headers, and SMATCHET_DEVIATION(rule=<id>) suppression):
#   no-printf-stderr       printf/fprintf/cerr/cout without an exemption marker
#   no-raw-new             raw `new T` (use make_unique) without an exemption marker
#   no-detach              `.detach()` (use AppController::LaunchBackgroundTask; first-party-wide)
#   no-glfw-in-core-headers  GLFW/glad/OpenGL #include in a Source/Core/include header
#                          (DX12 target compiles them too; absolute-0, no grandfathering)
#   cmake-local-gate-ci-scope  message(FATAL_ERROR ...) keyed on a LOCAL-dev project.config
#                          knob (msvc_toolset_pin) without a NOT DEFINED ENV{CI} scope —
#                          would FATAL every fresh-configure CI runner (absolute-0; #1074)
#   narrowing-conversions  clang-tidy cppcoreguidelines-narrowing-conversions (strict TUs)
#   define-imgui           `#define ImGui...` macro-alias trick
#   deviation-overdue      SMATCHET_DEVIATION whose calendar revisit= has passed
#   function-too-long      function body > 120 lines non-UI / > 200 lines ImGui-draw
#                          (repo-wide, delta-gated; tiered; function_size_audit.py)
#   function-too-branchy   function decision count > 30 (repo-wide, delta-gated, all functions)
#   (advisory)             [func-size] WARN on > 100 lines / > 20 branches — never blocks
#   agent-too-long         agent prompt > 250 lines / AGENTS.md > 150 (delta-gated; agent_size_audit.py)
#   (advisory)             [agent-size] WARN — soft tiers + the docs/agent-rules + skills sinks (~400)
#   bare-json-parse-untrusted  bare json::parse( / stream>>json slurp not routed via
#                          json_safe::ParseBounded (ALL first-party .cpp/.h/.hpp; blocking)
#   catch-all-swallow      EMPTY catch (...) body — no statement, no justifying comment
#                          (all first-party C++; absolute-0; exception-handling-policy hard rule 1)
#   (advisory)             unbounded-recursive-json-walker — self-recursive fn over a
#                          nlohmann::json/sol::object param with no depth/budget token (WARN)
#   (advisory)             unbounded-file-slurp — rdbuf()/istreambuf whole-file read (WARN)
#
# Modes:
#   (no args) / --diff [<ref>]   delta gate: fail only on (rule,basename,hash)
#                                triples present on HEAD but not <ref>
#                                (default origin/develop). Grandfathers existing.
#   --catalog [--refresh]        dump the strict-zone violator set; --refresh
#                                writes docs/high-integrity/baseline.md
#   --scan-file <f>              scan a single file with all rules (bats harness);
#                                zone-agnostic, prints `<rule>\t<file>:<line>`
#   --full                       whole-tree strict-zone scan (human/debug)
#   --selftest                   assert AGENTS.md zone globs == this script's copy
#
# Overrides:
#   SMATCHET_LINT_BASELINE_SET=<file>  read baseline triples from a file instead
#                                      of computing from git (bats stub).
#   SMATCHET_LINT_BYPASS=1             advisory short-circuit (exit 0).
#
# Exit codes: 0 pass, 1 new strict-zone violation / baseline drift, 2 usage error.

set -euo pipefail

# Absolute path to THIS script — so the --diff base scan re-invokes the *current*
# scanner logic (not origin/develop's older copy) against the base worktree.
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

# Resolve a WORKING python interpreter on stdout (empty + rc 1 if none). `command -v`
# alone is insufficient on Windows: the python3 "App Execution Alias" stub passes
# `command -v` but exits 49 ("Python was not found") when run — probe each candidate.
resolve_python() {
    local cand p
    for cand in python3 python py; do
        p="$(command -v "$cand" 2>/dev/null)" || continue
        if "$p" -c "" >/dev/null 2>&1; then printf '%s\n' "$p"; return 0; fi
    done
    return 1
}

# --root <dir> scans an arbitrary tree (used to scan the --diff baseline worktree
# with the current scanner). Default root = repo root relative to this script.
if [ "${1:-}" = "--root" ]; then
    cd "$2"; shift 2
else
    cd "$(dirname "$0")/../../.."
fi
REPO_ROOT="$(pwd)"

BASELINE_FILE="docs/high-integrity/baseline.md"

# --- Zone globs (KEEP IN SYNC with AGENTS.md § Tiered enforcement; --selftest guards) ---
STRICT_GLOBS=(
    "Source/Core/src/Tracker/"
    "Source/Core/src/Sync/"
    "Source/Core/src/Persistence/"
    "Source/Core/src/Config/"
    "Source/Core/src/Commands/"
    "Source/Core/include/Tracker/"
    "Source/Core/include/Sync/"
    "Source/Core/include/Persistence/"
    "Source/Core/include/Config/"
    "Source/Core/include/Commands/"
    "Source/Plugins/Mcp/"
)

# zone_of <path> -> strict|light|exempt
zone_of() {
    local f="$1"
    case "$f" in
        ThirdParty/*|*/ThirdParty/*|build/*|*/build/*) echo exempt; return ;;
    esac
    local g
    for g in "${STRICT_GLOBS[@]}"; do
        case "$f" in "$g"*) echo strict; return ;; esac
    done
    case "$f" in
        Source/Core/src/Ui/*|Source/Core/include/Ui/*|Source/Standalone/*) echo light; return ;;
    esac
    echo exempt
}

# normalise a source line: strip leading ws + trailing line comment, squeeze ws.
normalise_line() {
    sed -E 's@//.*$@@; s@/\*.*\*/@@; s/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]+/ /g'
}

snippet_hash() {
    printf '%s' "$1" | normalise_line | sha1sum | cut -c1-12
}

# Does this line carry an inline exemption marker (existing vocabulary) OR a
# SMATCHET_DEVIATION suppressing this rule? (deviation handled by caller via
# the preceding-comment scan; here we only match the legacy inline markers.)
has_inline_exempt() {
    # Legacy inline markers only. NOLINT is intentionally NOT here: a strict-zone
    # deviation must carry the audit-able SMATCHET_DEVIATION(rule=…; revisit=…)
    # trail, not an ungoverned // NOLINT bypass (CR #507).
    case "$1" in
        *"// CLI stdout"*|*"// pre-logger-init"*|*"// C-ABI"*|*"// custom-deleter"*|*"// pimpl"*) return 0 ;;
    esac
    return 1
}

# ---------------------------------------------------------------------------
# Rule scanners. Each emits lines: `<rule-id>\t<file>:<lineno>\t<rawline>`
# over a given file list (caller filters by zone for gating).
# ---------------------------------------------------------------------------

# Parse SMATCHET_DEVIATION(rule=X; ...; revisit=Y) on the line ABOVE a target.
# Returns the suppressed rule-id via stdout if the comment suppresses `want`.
# Also emits deviation-overdue when revisit is a passed calendar marker.
DEV_RE='SMATCHET_DEVIATION\(([^)]*)\)'

today_ymd() { date +%Y-%m-%d; }

revisit_overdue() {
    # $1 = revisit value. Overdue iff YYYY-MM-DD < today, or YYYY-Qn end < today.
    local r="$1" today; today="$(today_ymd)"
    case "$r" in
        [0-9][0-9][0-9][0-9]-[0-1][0-9]-[0-3][0-9])
            [ "$r" \< "$today" ] && return 0 || return 1 ;;
        [0-9][0-9][0-9][0-9]-Q[1-4])
            local y q endm end
            y="${r%-Q*}"; q="${r#*-Q}"
            case "$q" in 1) endm=03-31;; 2) endm=06-30;; 3) endm=09-30;; 4) endm=12-31;; esac
            end="$y-$endm"
            [ "$end" \< "$today" ] && return 0 || return 1 ;;
        *) return 1 ;;  # slug / never / unknown -> never overdue
    esac
}

# reduce-source-comment-bloat Phase 4 — repo-wide comment-regrowth rule-ids (delta-gated,
# hard-fail anywhere; classified by comment_audit.py --diff). KEEP IN SYNC with AGENTS.md.
COMMENT_RULES=(comment-commented-out-code comment-decorative-banner comment-blank-run)

# decompose-top-20-monoliths Slice 0 — repo-wide function-size rule-ids (delta-gated; classified by
# function_size_audit.py --diff). KEEP IN SYNC with AGENTS.md § Tiered enforcement.
FUNCSIZE_RULES=(function-too-long function-too-branchy)

# reduce-agent-prompt-bloat Slice 0 — agent-prompt / AGENTS.md size rule-id (delta-gated; classified
# by agent_size_audit.py --diff). KEEP IN SYNC with AGENTS.md § Project rules § Prompt/contract size.
AGENTSIZE_RULES=(agent-too-long)

# core-include-dag Phase 0 — Source/Core include-graph acyclicity + layer-DAG rule-id (delta-gated;
# classified by include_cycle_audit.py --diff). BLOCKS (fails CLOSED) on a NEW SCC>1 cycle or a
# NEW low->high named-layer header back-edge. KEEP IN SYNC with AGENTS.md § Tiered enforcement.
INCLUDECYCLE_RULES=(include-cycle)

# appcontroller-fan-in Phase 1 — AppController.h fan-in ratchet rule-id (delta-gated; classified by
# appcontroller_fan_in_audit.py --diff). BLOCKS (fails CLOSED, hard-FAIL) on a NEW Source/-wide
# quote-form `#include "AppController.h"` includer above the merge-base count. KEEP IN SYNC with
# AGENTS.md § Tiered enforcement.
FANIN_RULES=(app-controller-fan-in)

# PR-5 — bare json::parse (repo-wide default-deny since the recurring-findings gate hardening) +
# g_ui request-flag off-thread write (strict-zone, absolute-0). KEEP IN SYNC with AGENTS.md §
# Enforcement contract-card.
BAREJSON_RULES=(bare-json-parse-untrusted)
UIREQFLAG_RULES=(ui-request-flag-off-thread)

# recurring-findings gate — classes mined from SECURITY_AUDIT.md / CPP_CODE_AUDIT.md /
# docs/self-improvement/postmortems.md that kept recurring across remediation PRs. catch-all-swallow
# is BLOCKING absolute-0 (exception-handling-policy.md hard rule 1; the tree is clean today); the
# walker + slurp rules are WARN-first (calibration, same path as the original duplication gate).
# KEEP IN SYNC with AGENTS.md § Enforcement contract-card.
CATCHALL_RULES=(catch-all-swallow)
JSONWALKER_RULES=(unbounded-recursive-json-walker)
SLURP_RULES=(unbounded-file-slurp)

ratio_warn_for() {
    # Advisory soft warning (never blocks): delegate to comment_audit.py --ratio-warn, which warns
    # per changed file whose comment ratio rises vs base AND exceeds 0.50. Always returns 0.
    local base="$1" aud py
    aud="$REPO_ROOT/agents/scripts/core/comment_audit.py"
    py="$(resolve_python || true)"
    [ -n "$py" ] || return 0          # advisory-only; silently skip if no python interpreter
    [ -f "$aud" ] && "$py" "$aud" --ratio-warn "$base" 2>/dev/null || true
    return 0
}

scan_file_rules() {
    # $1 = file. Emits triples for all line/grep rules + deviation-overdue.
    local f="$1"
    [ -f "$f" ] || return 0
    local lineno=0 prev_dev_rule="" prev_dev_revisit=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))

        # Capture a SMATCHET_DEVIATION comment to suppress the NEXT non-blank line.
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}"
            prev_dev_rule=""; prev_dev_revisit=""
            local kv
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do
                kv="${kv# }"
                case "$kv" in
                    rule=*)    prev_dev_rule="${kv#rule=}" ;;
                    revisit=*) prev_dev_revisit="${kv#revisit=}" ;;
                esac
            done
            # deviation-overdue fires on the comment line itself.
            if [ -n "$prev_dev_revisit" ] && revisit_overdue "$prev_dev_revisit"; then
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
        prev_dev_rule=""; prev_dev_revisit=""   # deviation only covers the next non-blank line

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

# clang-tidy narrowing-conversions over strict-zone .cpp TUs. Requires a
# compile_commands.json; warn-skips otherwise (matches the existing missing-
# toolchain fallback for cppcheck/shellcheck).
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
find_compile_db() {
    # Prefer clang dbs — clang-tidy can't parse the MSVC db (PCH + stdlib paths).
    local d
    for d in build/ninja-iter-clang build/ninja-debug-clang build/ninja-iter-msvc build/ninja-test-msvc; do
        [ -f "$d/compile_commands.json" ] && { echo "$d"; return 0; }
    done
    return 1
}

scan_narrowing() {
    # $@ = strict-zone .cpp files. Emits narrowing-conversions triples.
    #
    # OPT-IN (SMATCHET_LINT_NARROWING=1). Default-off because clang-tidy needs a
    # *clang* compile_commands.json: against the MSVC db it errors on the MSVC
    # PCH ("not a valid PCH file") and can't resolve <string> (no clang stdlib
    # paths), yielding only clang-diagnostic-error noise. Until a PCH-free clang
    # db is provisioned in CI this rule is catalogued-only, never gated.
    # See docs/plans/shipped/high-integrity-cpp-enforcement.md § Deviations.
    if [ "${SMATCHET_LINT_NARROWING:-0}" != "1" ]; then
        echo "test-lint-rules: INFO: narrowing-conversions opt-in (SMATCHET_LINT_NARROWING=1 + clang db); skipped" >&2
        return 0
    fi
    if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
        echo "test-lint-rules: WARN: clang-tidy not on PATH; narrowing-conversions skipped" >&2
        return 0
    fi
    local db
    if ! db="$(find_compile_db)"; then
        echo "test-lint-rules: WARN: no compile_commands.json (configure a build); narrowing-conversions skipped" >&2
        return 0
    fi

    # Collect the .cpp TUs to scan (the .h/.hpp the caller passes are not compile
    # units in the db; clang-tidy -p resolves only entries present in the db).
    local f cpp_files=()
    for f in "$@"; do
        case "$f" in *.cpp) cpp_files+=("$f") ;; *) ;; esac
    done
    [ "${#cpp_files[@]}" -eq 0 ] && return 0

    # Parallel clang-tidy fan-out. The old serial `for` loop paid clang-tidy's
    # ~5 s startup+parse once per TU (~100 strict TUs = several minutes; the
    # develop `high-integrity-narrowing` job paid it every merge — tooling.md P2).
    # Fan the TUs out across $jobs workers with `xargs -P`, aggregate raw output,
    # then apply the grep + sed-normalise + first-party/ThirdParty filter ONCE
    # below. Identical findings, several-fold faster wall time.
    #
    # Job count: SMATCHET_LINT_NARROWING_JOBS override > nproc > 4 fallback.
    local jobs="${SMATCHET_LINT_NARROWING_JOBS:-}"
    if [ -z "$jobs" ]; then
        jobs="$(nproc 2>/dev/null || echo 4)"
    fi
    case "$jobs" in ''|*[!0-9]*) jobs=4 ;; esac
    [ "$jobs" -lt 1 ] && jobs=1

    # Raw aggregate of every worker's clang-tidy output. A per-TU clean run that
    # emits no narrowing line is fine — the grep filter below drops the noise.
    # `|| true` on the inner clang-tidy keeps a non-zero per-TU rc (e.g. a
    # clang-diagnostic-error on an unrelated header) from aborting xargs.
    local raw; raw="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/scan_narrowing.$$")"
    # shellcheck disable=SC2016  # $CLANG_TIDY / $db expand in the child sh, by design.
    printf '%s\0' "${cpp_files[@]}" \
        | CT="$CLANG_TIDY" DB="$db" xargs -0 -P "$jobs" -I '{}' \
            sh -c '"$CT" -p "$DB" --checks="-*,cppcoreguidelines-narrowing-conversions" --quiet "$1" 2>/dev/null || true' _ '{}' \
        > "$raw" 2>/dev/null || true

    # clang-tidy lines: <file>:<line>:<col>: warning: ... [cppcoreguidelines-narrowing-conversions]
    # `grep || true`: an aggregate with no narrowing match must NOT make this pipe
    # exit non-zero — under the caller's `set -e -o pipefail` that would abort the
    # whole strict scan (false PASS: catalog computed as empty).
    { grep -E 'narrowing-conversions\]$' "$raw" || true; } | while IFS= read -r w; do
        # Greedy path match so a Windows drive-letter colon (C:\…) is not
        # truncated the way `cut -d:` would; then backslash->slash and strip to
        # a repo-relative Source/… path so the triple basename + hash match the
        # line-rule shape (and stay path-portable across head/base).
        local loc; loc="$(printf '%s' "$w" | sed -E 's@^(.*):([0-9]+):[0-9]+: warning:.*@\1:\2@; s@\\@/@g; s@^.*/(Source/)@\1@')"
        # First-party strict sources only: drop anything that did not normalise
        # under Source/ (FetchContent / system headers) and any vendored
        # ThirdParty header — e.g. stb_image.h compiled into a strict TU via
        # STB_IMAGE_IMPLEMENTATION is not first-party narrowing debt.
        case "$loc" in Source/*) ;; *) continue ;; esac
        case "$loc" in *ThirdParty*) continue ;; esac
        printf 'narrowing-conversions\t%s\t%s\n' "$loc" "$w"
    done
    rm -f "$raw" 2>/dev/null || true
    return 0
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

# cmake-local-gate-ci-scope — ABSOLUTE-0 over CMakeLists.txt / cmake/*.cmake.
# A `message(FATAL_ERROR ...)` keyed on a LOCAL-dev project.config knob
# (msvc_toolset_pin) must be scoped to non-CI (`NOT DEFINED ENV{CI}` /
# `ENV{GITHUB_ACTIONS}`): CI runners configure FRESH every run and use their own
# (consistent, often newer) toolset, so they can NEVER hit the stale-cache
# cl-vs-headers class the toolset guard targets — applied unconditionally the
# guard FATALs every runner whose toolset != the pin (incident #1074 red-walled
# all 5 Windows CI required checks; the fix was `if(... AND NOT DEFINED ENV{CI})`).
# The local-only intent lived in the COMMENT, not the CONDITION — this lint moves
# it into the condition. The tree's one such guard is correctly CI-scoped today, so
# any unguarded hit is a regression (no grandfathering — same model as no-glfw).
# Cheap heuristic (not a CMake AST parse): for each `message(FATAL_ERROR` line, a
# backward window of $CMAKE_CI_SCOPE_WINDOW lines that mentions a local knob but
# carries no ENV{CI}/ENV{GITHUB_ACTIONS} token fires. A
# `# SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; ...)` within the window escapes.
CMAKE_LOCAL_KNOBS_RE='msvc_toolset_pin'           # extend with | for new local knobs
CMAKE_CI_SCOPE_WINDOW="${SMATCHET_CMAKE_CI_WINDOW:-80}"

scan_cmake_ci_scope_file() {
    # $1 = a CMakeLists.txt / *.cmake file. Emits `cmake-local-gate-ci-scope\t<f>:<line>`
    # per un-CI-scoped local-knob FATAL_ERROR. CMake files are small — read fully.
    local f="$1"
    [ -f "$f" ] || return 0
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i j start knob ci dev
    for ((i = 0; i < n; i++)); do
        case "${lines[$i]}" in *'message(FATAL_ERROR'*) ;; *) continue ;; esac
        start=$(( i - CMAKE_CI_SCOPE_WINDOW )); [ "$start" -lt 0 ] && start=0
        knob=0; ci=0; dev=0
        for ((j = start; j <= i; j++)); do
            case "${lines[$j]}" in *"$CMAKE_LOCAL_KNOBS_RE"*) knob=1 ;; esac
            case "${lines[$j]}" in *'ENV{CI}'*|*'ENV{GITHUB_ACTIONS}'*) ci=1 ;; esac
            case "${lines[$j]}" in *'SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope'*) dev=1 ;; esac
        done
        if [ "$knob" -eq 1 ] && [ "$ci" -eq 0 ] && [ "$dev" -eq 0 ]; then
            printf 'cmake-local-gate-ci-scope\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_cmake_ci_scope_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files 2>/dev/null \
            | grep -E '(^|/)CMakeLists\.txt$|(^|/)cmake/.*\.cmake$' \
            | grep -vE '(^|/)(ThirdParty|build)/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_cmake_ci_scope_file "$f"; done
}

# unused-symbol-under-config-guard — ABSOLUTE-0 over first-party C++ .cpp TUs.
# The config-skew -Werror,-Wunused-function class that produced #863: a free
# function DEFINITION sits at file scope UNGUARDED while EVERY reference to it
# lives inside a `#if[def] ... SMATCHET_WITH_* ...` block. In the feature-OFF
# build the definition has zero callers -> Clang's /WX -Werror promotes
# -Wunused-function to a hard error and the sanitized binary never links. MSVC
# /W4 (the PR-time iter/publish presets) does not warn the same way, so the
# break is invisible until the nightly Lua-OFF sanitizer job -- a config-skew
# gate escape (#863, fixed by 61b17427 / #945; this lint is the preventing gate).
#
# Heuristic (NOT a compiler / full AST parse -- a deliberately narrow text+
# preprocessor-depth proxy for the -Wunused-function shape, scoped to keep
# false positives near zero):
#   * Track config-guard depth: +1 on a `#if`/`#ifdef`/`#ifndef` whose header
#     line mentions SMATCHET_WITH_, -1 on the matching `#endif` (full #if/#endif
#     nesting is tracked so a non-config #if inside a config block still closes
#     correctly). A line is "config-guarded" iff config-depth > 0.
#   * A DEFINITION candidate is a COLUMN-0 (file/namespace scope -- methods are
#     indented) line shaped like a free-function definition `... name(... ) {`
#     (or `... name(...)` whose body-brace opens on the next non-blank line),
#     captured ONLY when it sits at config-depth 0 (unguarded). Column-0 + the
#     return-type-then-name-then-paren shape is what restricts this to the
#     -Wunused-function free-function class and excludes calls, declarations
#     (trailing `;`), control-flow (`if (`/`for (`/`while (`/`switch (`), and
#     macro lines.
#   * For each candidate name, scan EVERY OTHER line for a whole-word reference.
#     If there is >=1 reference and ALL references are config-guarded (depth>0)
#     while the definition is unguarded (depth 0) -> flag the definition line.
#   * A `// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; ...)` in the
#     preceding-line window above the definition escapes (legit asymmetry).
# Limitation (documented, accepted per the plan section Risks): this is a
# per-file proxy, not the compiler -- it cannot see cross-TU linkage or the exact
# set of SMATCHET_WITH_* the nightly toggles, and only catches the single-file
# def-unguarded / all-refs-guarded shape. The nightly Lua-OFF sanitizer build
# stays the authoritative backstop; this is the fast PR-time fail.
USCG_CONFIG_RE='SMATCHET_WITH_'   # config-flag family the guard family keys on

usc_is_def_line() {
    # $1 = the COLUMN-0 raw line. Sets the global USC_DEF_NAME to the function
    # name + returns 0 if the line looks like a FREE-function DEFINITION header;
    # returns 1 otherwise. NB: uses a global (not stdout) so the hot Pass-1 caller
    # need not fork a `$(...)` subshell per column-0 line (that fork cost ~100s
    # tree-wide before this change).
    USC_DEF_NAME=""
    local line="$1"
    # Must contain `name(` and must NOT end in `;` (that's a declaration/call stmt).
    case "$line" in
        *';'*) return 1 ;;                       # declaration / statement, not a def
        '#'*) return 1 ;;                        # preprocessor line
        '//'*|'/*'*|'*'*) return 1 ;;            # comment
    esac
    # The "signature head" is everything up to the first `(`.
    local head="${line%%(*}"
    # Reject QUALIFIED definitions (`Type::method`, `ns::fn`): an out-of-line
    # member / namespace-qualified definition is NOT the -Wunused-function
    # free-function class — the symbol is declared in a header and referenced from
    # OTHER TUs, so it is never "unused" the way a file-local free function is.
    # This is the discriminator that keeps AppController::AddAiContext (a public
    # member whose only IN-FILE refs happen to be SMATCHET_WITH_AI-guarded) from
    # firing while LogLuaScriptFileProbe (an unqualified free function — the #863
    # shape) still does. (Real-tree false-positive caught pre-ship; see plan log.)
    case "$head" in *'::'*) return 1 ;; esac
    # Reject type/template/operator declarations that can also open column-0 with `(`.
    case "$head" in
        *operator*|*template*|*'class '*|*'struct '*|*'enum '*|*'union '*|*'namespace '*) return 1 ;;
    esac
    # Extract the LAST identifier in `head` (the token immediately before `(`),
    # in PURE BASH (no sed/printf/head subprocess — this runs per column-0 line
    # over every config-bearing TU; a 3-process pipeline here cost ~100s tree-wide).
    # Trim trailing whitespace, then strip everything up to the last char that is
    # not an identifier char, leaving the trailing run of [A-Za-z0-9_].
    local h="${head%"${head##*[![:space:]]}"}"   # rstrip
    local name="$h"
    # Strip a leading run that ends at the last non-identifier char.
    case "$name" in
        *[!A-Za-z0-9_]*) name="${name##*[!A-Za-z0-9_]}" ;;
    esac
    # name now = trailing identifier run (possibly empty if head ended in punct).
    case "$name" in
        ''|*[!A-Za-z0-9_]*) return 1 ;;          # no clean trailing identifier
    esac
    # Reject control-flow / type-system keywords as the "function name".
    case "$name" in
        if|for|while|switch|return|sizeof|catch|else|do|case|new|delete) return 1 ;;
    esac
    # Require a return-type token BEFORE the name (a leading identifier/`*`/`&`/
    # `>` token), i.e. `void name(` / `Foo* name(` / `Result<T> name(` -- a bare
    # `name(args)` with nothing before it is a CALL, not a definition.
    case "$line" in
        "$name"*) return 1 ;;                    # name at column 0 with no return type = call
    esac
    USC_DEF_NAME="$name"
    return 0
}

scan_unused_under_config_guard_file() {
    # $1 = a first-party .cpp file. Emits
    # `unused-symbol-under-config-guard\t<f>:<defline>` per unguarded definition
    # whose every reference is config-guarded. .cpp only (the -Wunused-function
    # class is a TU-local free function; headers declare, they don't define-then-
    # leave-unused the same way).
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp) ;; *) return 0 ;; esac

    # Fast early-exit: a file with no config-flag token at all has no possible
    # config-guarded reference, so no candidate can ever qualify. Skips ~83% of
    # first-party .cpp with one cheap fixed-string grep, before the per-line walk
    # (the def+ref double-scan is O(defs x lines) per file — too slow tree-wide
    # without this gate; only ~47 of ~275 .cpp carry SMATCHET_WITH_).
    grep -qF "$USCG_CONFIG_RE" "$f" 2>/dev/null || return 0

    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i

    # Pass 1: compute POSITIVE-config-guard depth per line + collect unguarded def
    # candidates. A line counts as config-guarded ONLY when its nearest enclosing
    # config `#if` is a POSITIVE one — `#if defined(SMATCHET_WITH_X)` / `#ifdef
    # SMATCHET_WITH_X` / `#if SMATCHET_WITH_X` — AND we are in that #if's TRUE
    # branch (not its `#else`/`#elif`). This is the discriminator that matches the
    # exact #863 shape (call under `#if defined(SMATCHET_WITH_LUA_AUTOMATION)`) while
    # rejecting the common false-positive idiom of a real impl in the `#else` of a
    # `#if !defined(SMATCHET_WITH_X)` (the call then sits in the FLAG-ON branch, and
    # the def is never unused — e.g. SmatchetWhisperSetupBanner's ComputeBannerHeight).
    local -a cfg_depth=()           # cfg_depth[i] = positive-config-guard depth AT line i
    local -a def_name=()            # parallel: candidate names (unguarded defs)
    local -a def_line=()            # parallel: 1-based line numbers
    local -a def_dev=()             # parallel: 1 if an in-window DEVIATION suppresses
    local depth=0                   # full #if/#endif nesting depth
    local cfg=0                     # positive-config-guard depth (subset of `depth`)
    # per-#if frame: "active" = this frame currently contributes +1 to cfg (a
    # positive config #if whose TRUE branch we are in). On `#else`/`#elif` the
    # frame flips inactive (we've left the positive true-branch).
    local -a frame_iscfg=()         # 1 if this #if header is a positive config guard
    local -a frame_active=()        # 1 if currently counted in cfg
    for ((i = 0; i < n; i++)); do
        local raw="${lines[$i]}"
        # Leading-whitespace-stripped view for preprocessor detection.
        local stripped="${raw#"${raw%%[![:space:]]*}"}"
        case "$stripped" in
            '#if'*|'#ifdef'*|'#ifndef'*)
                depth=$((depth + 1))
                # Positive config guard: mentions SMATCHET_WITH_ and is NOT a
                # negation (`#ifndef`, or `#if !defined`/`#if !` form).
                local iscfg=0
                case "$stripped" in
                    '#ifndef'*) : ;;                                  # negative — not positive
                    *"$USCG_CONFIG_RE"*)
                        case "$stripped" in
                            *'!defined'*|*'! defined'*|*'!'*"$USCG_CONFIG_RE"*) : ;;  # negation
                            *) iscfg=1 ;;
                        esac ;;
                esac
                frame_iscfg+=("$iscfg"); frame_active+=("$iscfg")
                [ "$iscfg" = "1" ] && cfg=$((cfg + 1))
                cfg_depth[$i]=$cfg
                continue ;;
            '#else'*|'#elif'*)
                # Leaving the current frame's TRUE branch: if it was an active
                # positive config guard, drop its contribution for the rest of the
                # frame. (A positive config #elif could re-activate, but that idiom
                # is vanishingly rare for SMATCHET_WITH_ flags — treat #elif as
                # leaving the positive branch, conservative = fewer FPs.)
                local fi=$(( ${#frame_active[@]} - 1 ))
                if [ "$fi" -ge 0 ] && [ "${frame_active[$fi]:-0}" = "1" ]; then
                    [ "$cfg" -gt 0 ] && cfg=$((cfg - 1))
                    frame_active[$fi]=0
                fi
                cfg_depth[$i]=$cfg
                continue ;;
            '#endif'*)
                if [ "$depth" -gt 0 ]; then
                    local fi=$(( ${#frame_active[@]} - 1 ))
                    if [ "$fi" -ge 0 ] && [ "${frame_active[$fi]:-0}" = "1" ] && [ "$cfg" -gt 0 ]; then
                        cfg=$((cfg - 1))
                    fi
                    unset 'frame_active[${#frame_active[@]}-1]'; frame_active=("${frame_active[@]}")
                    unset 'frame_iscfg[${#frame_iscfg[@]}-1]'; frame_iscfg=("${frame_iscfg[@]}")
                    depth=$((depth - 1))
                fi
                cfg_depth[$i]=$cfg
                continue ;;
            *) cfg_depth[$i]=$cfg ;;
        esac

        # Candidate definition: column-0 (no leading whitespace), at config-depth 0,
        # def-shaped, with a `{` opening the body on this line OR the next non-blank.
        [ "$cfg" -eq 0 ] || continue
        case "$raw" in [![:space:]]*) ;; *) continue ;; esac     # column-0 only
        # Cheap pre-filter before the def-shape check: a candidate def line must
        # contain a `(` and must not be a preprocessor / comment / statement line.
        case "$raw" in *'('*) ;; *) continue ;; esac
        local name
        usc_is_def_line "$raw" || continue
        name="$USC_DEF_NAME"
        # Require the body brace `{` here or on the next non-blank line (a real
        # definition, not a multi-line declaration). Scan forward briefly.
        local has_brace=0 j
        case "$raw" in *'{'*) has_brace=1 ;; esac
        if [ "$has_brace" -eq 0 ]; then
            for ((j = i + 1; j < n && j <= i + 4; j++)); do
                local nx="${lines[$j]}"
                case "$nx" in
                    '') continue ;;
                    *';'*) break ;;          # declaration terminator before any brace
                    *'{'*) has_brace=1; break ;;
                esac
            done
        fi
        [ "$has_brace" -eq 1 ] || continue

        # In-window SMATCHET_DEVIATION suppression (preceding up-to-3 non-blank lines).
        local dev=0 k cnt=0
        for ((k = i - 1; k >= 0 && cnt < 3; k--)); do
            case "${lines[$k]}" in '') continue ;; esac
            cnt=$((cnt + 1))
            case "${lines[$k]}" in
                *'SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard'*) dev=1; break ;;
            esac
        done

        def_name+=("$name"); def_line+=("$((i + 1))"); def_dev+=("$dev")
    done

    [ "${#def_name[@]}" -gt 0 ] || return 0

    # Pass 2: for each candidate, classify every OTHER reference by config-guard.
    # Reference lookup is delegated to `grep -nwF` (whole-word, fixed-string) — a
    # single C-fast scan per candidate — rather than a bash per-line `=~` regex
    # loop (which was O(defs x lines): catastrophically slow on the 2000+-line
    # monoliths like AppController.cpp, several minutes tree-wide). grep -n gives
    # `<lineno>:<content>`; cfg_depth[lineno-1] (precomputed in Pass 1) tells us
    # whether that reference is config-guarded. -w is grep's own non-identifier
    # word boundary (digits/letters/underscore), the same whole-word semantics the
    # old regex enforced.
    local d
    for ((d = 0; d < ${#def_name[@]}; d++)); do
        local nm="${def_name[$d]}" dl="${def_line[$d]}"
        [ "${def_dev[$d]}" = "1" ] && continue
        local refs=0 guarded_refs=0 gline gno gtext gs
        while IFS= read -r gline; do
            [ -n "$gline" ] || continue
            gno="${gline%%:*}"
            [ "$gno" = "$dl" ] && continue                  # skip the def line itself
            gtext="${gline#*:}"
            # Skip pure-comment / preprocessor-conditional lines as references
            # (a `#if defined(name)` is config plumbing, a `// name` is prose).
            gs="${gtext#"${gtext%%[![:space:]]*}"}"
            case "$gs" in '//'*|'*'*|'/*'*|'#if'*|'#ifdef'*|'#ifndef'*|'#endif'*|'#else'*|'#elif'*) continue ;; esac
            refs=$((refs + 1))
            [ "${cfg_depth[$((gno - 1))]:-0}" -gt 0 ] && guarded_refs=$((guarded_refs + 1))
        done < <(grep -nwF -- "$nm" "$f" 2>/dev/null || true)
        # Flag iff there is >=1 reference and EVERY reference is config-guarded
        # while the definition is unguarded (depth 0, by construction above).
        if [ "$refs" -ge 1 ] && [ "$refs" -eq "$guarded_refs" ]; then
            printf 'unused-symbol-under-config-guard\t%s:%s\n' "$f" "$dl"
        fi
    done
}

compute_unused_under_config_guard_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
            | grep -E '\.cpp$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_unused_under_config_guard_file "$f"; done
}

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
        # Record `nlohmann::json <ident>` declarations for the slurp form below.
        if [[ "$line" =~ (^|[^A-Za-z_:])nlohmann::json[[:space:]]+([A-Za-z_][A-Za-z0-9_]*) ]]; then
            jv_names+=("${BASH_REMATCH[2]}"); jv_lines+=("$lineno")
        fi
        # Match a bare json::parse( call. ParseBounded / ParseBoundedOrDiscarded are the mandated
        # forms, so a line that already routes through them is clean.
        case "$line" in
            *ParseBounded*) continue ;;
            *'"'*'json::parse'*'"'*) continue ;;   # `json::parse` inside a string literal
        esac
        if [[ "$line" =~ (^|[^A-Za-z_:])(nlohmann::)?json::parse[[:space:]]*\( ]]; then
            if [ "$suppress" != "bare-json-parse-untrusted" ]; then
                printf 'bare-json-parse-untrusted\t%s:%s\n' "$f" "$lineno"
            fi
            continue
        fi
        # operator>> slurp into a json var declared within the preceding 8 lines.
        if [[ "$line" == *'>>'* ]] && [ "${#jv_names[@]}" -gt 0 ]; then
            local vi slurpre
            for ((vi = ${#jv_names[@]} - 1; vi >= 0; vi--)); do
                [ $((lineno - jv_lines[vi])) -le 8 ] || break
                slurpre='>>[[:space:]]*'"${jv_names[vi]}"'([^A-Za-z0-9_]|$)'
                if [[ "$line" =~ $slurpre ]]; then
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
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_bare_json_parse_file "$f"; done
}

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
        [[ "$raw" =~ catch[[:space:]]*\([[:space:]]*\.\.\.[[:space:]]*\) ]] || continue
        local s="${raw#"${raw%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac          # comment mention, not code
        case "$raw" in *'"'*catch*'"'*) continue ;; esac         # string-literal mention
        case "$raw" in *'catch-all-ok:'*) continue ;; esac       # hook-vocabulary escape
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
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_catch_all_swallow_file "$f"; done
}

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
        # Depth/budget token in the signature = bounded walker.
        local bounded=0
        case "$raw" in *depth*|*Depth*|*budget*|*Budget*) bounded=1 ;; esac
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
            case "$b" in *depth*|*Depth*|*budget*|*Budget*) bounded=1 ;; esac
            if [[ "$b" =~ $callre ]]; then selfcall=1; fi
        done
        if [ "$selfcall" -eq 1 ] && [ "$bounded" -eq 0 ]; then
            printf 'unbounded-recursive-json-walker\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_json_walker_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_json_walker_file "$f"; done
}

# unbounded-file-slurp — WARN-first over first-party C++ (calibration; never blocks).
# A whole-file slurp with no byte cap (`ss << f.rdbuf()`, `std::istreambuf_iterator<char>`
# construction) reads an arbitrarily large file into memory before any validation — the class
# behind the Win32-vs-POSIX config-read asymmetry (SECURITY_AUDIT.md #33: the Win32 arm caps at
# 64 MiB, the POSIX arm slurps unbounded) and CPP_CODE_AUDIT #31's unbounded accumulator. Most
# residual sites read developer-authored fixtures — hence WARN-first, diff-scoped, deviation-
# suppressible (`// SMATCHET_DEVIATION(rule=unbounded-file-slurp; ...)` above the read). Prefer a
# size-capped read (stat/tellg + reject, or LoadJsonFile which already caps) for new code.

scan_file_slurp_file() {
    # $1 = a first-party C++ file. Emits `unbounded-file-slurp\t<f>:<line>` per uncapped slurp.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    grep -qE 'rdbuf\(\)|istreambuf_iterator' "$f" 2>/dev/null || return 0
    local lineno=0 prev_dev_rule=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        if [[ "$line" =~ ^[[:space:]]*(//|\*|/\*) ]]; then continue; fi
        case "$line" in *'"'*rdbuf*'"'*|*'"'*istreambuf*'"'*) continue ;; esac
        if [[ "$line" =~ (\<\<[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(\.|-\>)rdbuf\(\)|istreambuf_iterator\<[[:space:]]*char[[:space:]]*\>) ]]; then
            if [ "$suppress" != "unbounded-file-slurp" ]; then
                printf 'unbounded-file-slurp\t%s:%s\n' "$f" "$lineno"
            fi
        fi
    done < "$f"
}

compute_file_slurp_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_file_slurp_file "$f"; done
}

# ui-request-flag-off-thread — strict-zone rule over command-dispatch TUs (Source/Core/src/Commands/
# EXCEPT Scenarios/). A write to a g_ui request-flag field (requestWindowResize / requestWindowWidth
# / requestWindowHeight / requestScreenshot / requestScreenshotPath) from a command handler races
# the standalone main loop, which polls those non-atomic int/bool/std::string fields each frame: a
# command dispatched from an MCP/Lua WORKER thread writing them unsynchronised is a genuine data race
# (UB — the std::string buffer can reallocate under the reader; Pillar-3 never-crash). The conformant
# seam is RunOnUiThreadAsCommandResult / RunOnUiThread<...>(app, [...]{ ...write... }) which marshals
# the write onto the UI thread (see BuiltinCommands_Debug.cpp). So a request-flag WRITE in a command
# TU must sit inside a RunOnUiThread* closure.
#
# Scenarios/ is EXEMPT: IScenario lifecycle methods (OnStart/OnFrame/OnFinish/IsDone) run ON the UI
# thread by the scenario-runner contract, so their direct request-flag writes are correct (and would
# false-positive without the carve-out). The rule is scoped to the command-handler TUs where the
# off-thread risk is real.
#
# Heuristic (NOT an AST/thread-analysis — a text proxy): for each request-flag WRITE line (`X.field =`
# where field is a request-flag and the line is an assignment, not a read/compare), scan BACKWARD
# within a bounded window for an enclosing `RunOnUiThread` token. No enclosing RunOnUiThread in the
# window -> the write is (heuristically) off the UI thread -> fire. The window is generous
# ($SMATCHET_UI_REQFLAG_WINDOW lines) so a multi-statement closure body still sees its RunOnUiThread
# head. A `// SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; ...)` above the write escapes. The
# tree's command handlers all marshal correctly today, so any unwrapped write is a regression
# (absolute-0, no grandfathering — same model as no-glfw / cmake-local-gate-ci-scope).
UI_REQFLAG_RE='requestWindowResize|requestWindowWidth|requestWindowHeight|requestScreenshotPath|requestScreenshot'
UI_REQFLAG_WINDOW="${SMATCHET_UI_REQFLAG_WINDOW:-40}"

scan_ui_request_flag_file() {
    # $1 = a command-dispatch .cpp under Source/Core/src/Commands/ (NOT Scenarios/). Emits
    # `ui-request-flag-off-thread\t<f>:<line>` per request-flag write with no enclosing RunOnUiThread.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp) ;; *) return 0 ;; esac
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i j start
    for ((i = 0; i < n; i++)); do
        local raw="${lines[$i]}"
        # Skip pure-comment lines.
        local s="${raw#"${raw%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac
        # Must be an ASSIGNMENT to a request-flag field: `.<field> =` (single `=`, not `==`/`!=`/`>=`).
        [[ "$raw" =~ \.($UI_REQFLAG_RE)[[:space:]]*=[^=] ]] || continue
        # Scan backward for an enclosing RunOnUiThread (the conformant marshalling seam) OR an
        # in-window SMATCHET_DEVIATION suppressing this rule.
        start=$(( i - UI_REQFLAG_WINDOW )); [ "$start" -lt 0 ] && start=0
        local marshalled=0 dev=0
        for ((j = i; j >= start; j--)); do
            case "${lines[$j]}" in *'RunOnUiThread'*) marshalled=1; break ;; esac
            case "${lines[$j]}" in *'SMATCHET_DEVIATION(rule=ui-request-flag-off-thread'*) dev=1; break ;; esac
        done
        if [ "$marshalled" -eq 0 ] && [ "$dev" -eq 0 ]; then
            printf 'ui-request-flag-off-thread\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_ui_request_flag_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/src/Commands/**' \
            2>/dev/null \
            | grep -E '\.cpp$' | grep -vE '(^|/)(ThirdParty|Commands/Scenarios)/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_ui_request_flag_file "$f"; done
}

# pr-numbered-temporal-comments — WARN-first over first-party C++ comments. A comment that pins a
# DEVELOPMENT pull-request NUMBER (`// PR 5`, `// PR #1104`, `PR#1218`, `PR12`, `PRn`) is a temporal
# scaffold: it documents which PR introduced a change rather than the durable intent, and rots the
# moment the PR is squash-merged + the number forgotten. Rewrite each to present-tense intent (drop
# the PR-number token; keep the technical meaning). This gate stops the pattern re-accumulating after
# the one-shot sweep.
#
# A bash linter cannot tell a dev-PR token from the GitHub PRODUCT term "pull request" by meaning, so
# the regex is deliberately narrow — it fires ONLY on `PR` immediately followed by an OPTIONAL space,
# an OPTIONAL `#`, then a DIGIT (the dev-PR-number shape). Product-domain usages that carry no number
# ("PR-only columns", "type:pr", "per-PR enrichment", "[PR] prefix", "tell PRs apart") do NOT match.
# GitHub Issue refs (`#1081`), ADR refs (`ADR-0012`) and commit hashes never match (no `PR` prefix).
#
# WARN-FIRST / ADVISORY (calibration phase, mirrors the bare-json + unused-symbol + interface-doc
# gates; never touches $rc). Diff-scoped to the CHANGED first-party C++ files (the sweep cleaned the
# tree, so a whole-tree scan would be quiet today, but diff-scoping keeps it fast + focuses the WARN
# on newly-introduced text). A `// SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; ...)` on the
# line above suppresses (rare — e.g. a comment that must cite a specific historical PR for an audit).
# The whole-tree set is available via --scan-pr-comments for a campaign sweep.
PR_COMMENT_RE='\bPR ?#?[0-9]|\bPR[0-9]'

scan_pr_comment_file() {
    # $1 = a first-party .cpp/.h/.hpp file. Emits `pr-numbered-temporal-comments\t<f>:<line>` per
    # COMMENT line carrying a dev-PR-number token. Only comment lines fire (code that happens to
    # contain a `PR<digit>` token in a string/identifier is out of scope — this is a comment rule).
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    local lineno=0 prev_dev_rule=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        # A SMATCHET_DEVIATION on the line ABOVE suppresses the next non-blank line.
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        # Only consider COMMENT lines: a `//` line-comment, or a `*` / `/*` block-comment body.
        case "$line" in
            *'//'*|*'/*'*) ;;
            *'*'*) [[ "$line" =~ ^[[:space:]]*\* ]] || continue ;;
            *) continue ;;
        esac
        if [[ "$line" =~ $PR_COMMENT_RE ]]; then
            if [ "$suppress" != "pr-numbered-temporal-comments" ]; then
                printf 'pr-numbered-temporal-comments\t%s:%s\n' "$f" "$lineno"
            fi
        fi
    done < "$f"
}

compute_pr_comment_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' 'Source/UnrealPlugins/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_pr_comment_file "$f"; done
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
[ "${SMATCHET_LINT_BYPASS:-0}" = "1" ] && { echo "[test-lint-rules] BYPASS"; exit 0; }

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

MODE="diff"; ARG=""; REFRESH=0
# --refresh is a modifier on --catalog; detect it independently of the
# subcommand branch (keeps the subcommand parse value-free).
for _a in "$@"; do [ "$_a" = "--refresh" ] && REFRESH=1; done
case "${1:-}" in
    --diff)        MODE=diff;     ARG="${2:-}" ;;
    --diff=*)      MODE=diff;     ARG="${1#--diff=}" ;;
    --catalog)     MODE=catalog ;;
    --funcsize-baseline) MODE=funcsizebaseline ;;
    --agentsize-baseline) MODE=agentsizebaseline ;;
    --dup-baseline) MODE=dupbaseline ;;
    --include-cycle-baseline) MODE=includecyclebaseline ;;
    --scan-file)   MODE=scanfile; ARG="${2:-}" ;;
    --scan-file=*) MODE=scanfile; ARG="${1#--scan-file=}" ;;
    --full)        MODE=full ;;
    --scan-wide)   MODE=scanwide ;;
    --scan-glfw)   MODE=scanglfw ;;
    --scan-cmake-ci) MODE=scancmakeci ;;
    --scan-unused-cfg) MODE=scanunusedcfg ;;
    --scan-bare-json) MODE=scanbarejson ;;
    --scan-catch-all) MODE=scancatchall ;;
    --scan-json-walkers) MODE=scanjsonwalkers ;;
    --scan-slurps) MODE=scanslurps ;;
    --scan-ui-reqflag) MODE=scanuireqflag ;;
    --scan-pr-comments) MODE=scanprcomments ;;
    --selftest)    MODE=selftest ;;
    "")            MODE=diff ;;
    *) echo "usage: $0 [--diff[=]<ref>|--catalog [--refresh]|--funcsize-baseline|--agentsize-baseline|--dup-baseline|--include-cycle-baseline|--scan-file[=]<f>|--full|--scan-wide|--scan-glfw|--scan-cmake-ci|--scan-unused-cfg|--scan-bare-json|--scan-catch-all|--scan-json-walkers|--scan-slurps|--scan-ui-reqflag|--scan-pr-comments|--selftest]" >&2; exit 2 ;;
esac

case "$MODE" in
  scanfile)
    [ -n "$ARG" ] || { echo "--scan-file needs a path" >&2; exit 2; }
    scan_file_rules "$ARG" | while IFS=$'\t' read -r rule loc raw; do printf '%s\t%s\n' "$rule" "$loc"; done
    ;;

  selftest)
    # Assert each STRICT_GLOBS entry appears in AGENTS.md § Tiered enforcement.
    miss=0
    for g in "${STRICT_GLOBS[@]}"; do
        # only check the canonical src/* + Mcp globs (include/ mirrors are implied)
        case "$g" in Source/Core/include/*) continue ;; esac
        if ! grep -qF "$g" AGENTS.md; then echo "SELFTEST FAIL: '$g' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert each repo-wide comment-regrowth rule-id is documented in AGENTS.md (delta-gated list).
    for r in "${COMMENT_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: comment rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert each repo-wide function-size rule-id is documented in AGENTS.md (delta-gated list).
    for r in "${FUNCSIZE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: function-size rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the agent-prompt-size rule-id is documented in AGENTS.md (delta-gated list).
    for r in "${AGENTSIZE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: agent-size rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the include-cycle rule-id is documented in AGENTS.md (delta-gated, BLOCKING gate).
    for r in "${INCLUDECYCLE_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: include-cycle rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the AppController fan-in rule-id is documented in AGENTS.md (delta-gated, BLOCKING gate).
    for r in "${FANIN_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: fan-in rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Assert the PR-5 rule-ids are documented in AGENTS.md (bare-json WARN-first; ui-reqflag absolute-0).
    for r in "${BAREJSON_RULES[@]}" "${UIREQFLAG_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: PR-5 rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    # Delegate the tiered-cap + UI-classification in-sync assertion to the audit script's own
    # --selftest (single source of truth = function_size_audit.py is_ui_function() vs AGENTS.md).
    # Assert the duplication rule-id (DRY Engineering Pillar 5) is documented in AGENTS.md.
    if ! grep -qF "duplication" AGENTS.md; then echo "SELFTEST FAIL: 'duplication' rule missing from AGENTS.md" >&2; miss=1; fi
    # Assert the no-glfw-in-core-headers rule-id is documented in AGENTS.md (absolute-0 gate).
    if ! grep -qF "no-glfw-in-core-headers" AGENTS.md; then echo "SELFTEST FAIL: 'no-glfw-in-core-headers' rule missing from AGENTS.md" >&2; miss=1; fi
    # Assert the cmake-local-gate-ci-scope rule-id is documented in AGENTS.md (absolute-0 gate).
    if ! grep -qF "cmake-local-gate-ci-scope" AGENTS.md; then echo "SELFTEST FAIL: 'cmake-local-gate-ci-scope' rule missing from AGENTS.md" >&2; miss=1; fi
    # cmake-local-gate-ci-scope: asserts-failure — an unguarded knob-keyed FATAL_ERROR must fire;
    # a CI-scoped one (NOT DEFINED ENV{CI}) must not; a SMATCHET_DEVIATION in the window must not.
    _cmci_tmp="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/cmci_selftest.$$")"
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n  set(_p "msvc_toolset_pin")\n  if(NOT _p STREQUAL _cc)\n    message(FATAL_ERROR "toolset mismatch")\n  endif()\nendif()\n' > "$_cmci_tmp"
    if [ -z "$(scan_cmake_ci_scope_file "$_cmci_tmp")" ]; then
        echo "SELFTEST FAIL: cmake-local-gate-ci-scope did not fire on an unguarded knob-keyed FATAL_ERROR" >&2; miss=1; fi
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT DEFINED ENV{CI})\n  set(_p "msvc_toolset_pin")\n  if(NOT _p STREQUAL _cc)\n    message(FATAL_ERROR "toolset mismatch")\n  endif()\nendif()\n' > "$_cmci_tmp"
    if [ -n "$(scan_cmake_ci_scope_file "$_cmci_tmp")" ]; then
        echo "SELFTEST FAIL: cmake-local-gate-ci-scope fired on a CI-scoped knob-keyed FATAL_ERROR" >&2; miss=1; fi
    printf 'if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n  set(_p "msvc_toolset_pin")\n  # SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; reason=test; owner=x; revisit=2099-01-01)\n  message(FATAL_ERROR "toolset mismatch")\nendif()\n' > "$_cmci_tmp"
    if [ -n "$(scan_cmake_ci_scope_file "$_cmci_tmp")" ]; then
        echo "SELFTEST FAIL: cmake-local-gate-ci-scope fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_cmci_tmp" 2>/dev/null || true
    # Assert the unused-symbol-under-config-guard rule-id is documented in AGENTS.md (absolute-0 gate).
    if ! grep -qF "unused-symbol-under-config-guard" AGENTS.md; then echo "SELFTEST FAIL: 'unused-symbol-under-config-guard' rule missing from AGENTS.md" >&2; miss=1; fi
    # unused-symbol-under-config-guard: asserts-failure — replays the #863 shape (61b17427~1):
    # an UNGUARDED column-0 free-function definition whose only call sites are inside a
    # #if defined(SMATCHET_WITH_LUA_AUTOMATION) block MUST fire; the fixed shape (#945 / 61b17427,
    # definition also wrapped in the guard) MUST be clean; an in-window SMATCHET_DEVIATION MUST escape.
    # The scanner is .cpp-gated, so the fixtures carry a .cpp extension.
    _uscg_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/uscg_selftest.$$.cpp")"
    case "$_uscg_tmp" in *.cpp) ;; *) mv -f "$_uscg_tmp" "$_uscg_tmp.cpp" 2>/dev/null && _uscg_tmp="$_uscg_tmp.cpp" ;; esac
    # Positive: #863 pre-fix shape — def at column 0, unguarded; both call sites guarded.
    printf 'void LogLuaScriptFileProbe(const char* label, const std::string& path) {\n    (void)label; (void)path;\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("SmatchetHooks.lua", "x");\n    LogLuaScriptFileProbe("Automation.lua", "y");\n#endif\n}\n' > "$_uscg_tmp"
    if [ -z "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard did not fire on an unguarded def whose only refs are SMATCHET_WITH_*-guarded (the #863 shape)" >&2; miss=1; fi
    # Negative: #945 fixed shape — def ALSO inside the guard (symmetric). Clean.
    printf 'void InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("SmatchetHooks.lua", "x");\n#endif\n}\n\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\nvoid LogLuaScriptFileProbe(const char* label, const std::string& path) {\n    (void)label; (void)path;\n}\n#endif\n' > "$_uscg_tmp"
    if [ -n "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard fired on the symmetric (def+refs both guarded) #945 fixed shape" >&2; miss=1; fi
    # Negative: a def with an UNGUARDED reference too (legit asymmetry) must not fire.
    printf 'void Helper(int x) {\n    (void)x;\n}\n\nvoid AlwaysCalls() {\n    Helper(1);\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    Helper(2);\n#endif\n}\n' > "$_uscg_tmp"
    if [ -n "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard fired despite an unguarded reference (legit asymmetry)" >&2; miss=1; fi
    # Negative: in-window SMATCHET_DEVIATION above the unguarded def must escape.
    printf '// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; reason=test; owner=x; revisit=2099-01-01)\nvoid LogLuaScriptFileProbe(const char* label) {\n    (void)label;\n}\n\nvoid InitLua() {\n#if defined(SMATCHET_WITH_LUA_AUTOMATION)\n    LogLuaScriptFileProbe("x");\n#endif\n}\n' > "$_uscg_tmp"
    if [ -n "$(scan_unused_under_config_guard_file "$_uscg_tmp")" ]; then
        echo "SELFTEST FAIL: unused-symbol-under-config-guard fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_uscg_tmp" 2>/dev/null || true
    # --- bare-json-parse-untrusted — assert the rule is documented + fires on a bare parse in ANY
    # first-party TU (repo-wide default-deny), in a HEADER, and on the stream>>json slurp form; and
    # stays quiet for a ParseBounded route / a deviation. ---
    if ! grep -qF "bare-json-parse-untrusted" AGENTS.md; then echo "SELFTEST FAIL: 'bare-json-parse-untrusted' rule missing from AGENTS.md" >&2; miss=1; fi
    # selftest: asserts-failure — a bare json::parse in ANY TU must fire; ParseBounded / a
    # deviation must not.
    _bj_tmp="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/bj_selftest.$$")"
    _bj_ingress="$(dirname "$_bj_tmp")/McpPlugin.cpp"
    printf 'void f(const std::string& body) {\n    auto j = nlohmann::json::parse(body);\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -z "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a bare json::parse in an ingress TU (McpPlugin.cpp)" >&2; miss=1; fi
    printf 'void f(const std::string& body) {\n    std::string err;\n    auto j = smatchet::json_safe::ParseBounded(body, err);\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -n "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted fired on a ParseBounded-routed ingress parse" >&2; miss=1; fi
    # Repo-wide default-deny: a NON-curated basename MUST fire too (the curated allow-list is
    # retired — it lagged the code every time the class recurred; #1573/#1592/#1598).
    _bj_other="$(dirname "$_bj_tmp")/SomeUnrelatedThing.cpp"
    printf 'void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s);\n    (void)j;\n}\n' > "$_bj_other"
    if [ -z "$(scan_bare_json_parse_file "$_bj_other")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a bare parse in a non-curated TU (repo-wide default-deny)" >&2; miss=1; fi
    # Headers are in scope (the JsonParseUtil.h class): a bare parse in a .h must fire.
    _bj_hdr="$(dirname "$_bj_tmp")/SomeInlineHelper.h"
    printf 'inline void f(const std::string& s) {\n    auto j = nlohmann::json::parse(s, nullptr, false);\n    (void)j;\n}\n' > "$_bj_hdr"
    if [ -z "$(scan_bare_json_parse_file "$_bj_hdr")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a bare parse in a header" >&2; miss=1; fi
    # The `stream >> json` slurp form (declare-then-slurp within 8 lines) must fire.
    printf 'void f(std::ifstream& file) {\n    nlohmann::json j;\n    file >> j;\n    (void)j;\n}\n' > "$_bj_other"
    if [ -z "$(scan_bare_json_parse_file "$_bj_other")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted did not fire on a stream>>json slurp" >&2; miss=1; fi
    # ...but a >> into a non-json identifier must NOT fire.
    printf 'void f(std::ifstream& file) {\n    int count = 0;\n    file >> count;\n    (void)count;\n}\n' > "$_bj_other"
    if [ -n "$(scan_bare_json_parse_file "$_bj_other")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted fired on a >> into a non-json identifier" >&2; miss=1; fi
    # An in-line SMATCHET_DEVIATION above the parse must escape.
    printf 'void f(const std::string& body) {\n    // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=test; owner=x; revisit=2099-01-01)\n    auto j = nlohmann::json::parse(body);\n    (void)j;\n}\n' > "$_bj_ingress"
    if [ -n "$(scan_bare_json_parse_file "$_bj_ingress")" ]; then
        echo "SELFTEST FAIL: bare-json-parse-untrusted fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_bj_tmp" "$_bj_ingress" "$_bj_other" "$_bj_hdr" 2>/dev/null || true
    # --- catch-all-swallow — assert the rule is documented + fires on an EMPTY catch (...) body and
    # stays quiet for a commented body / catch-all-ok / a LOG body / a deviation. ---
    for r in "${CATCHALL_RULES[@]}" "${JSONWALKER_RULES[@]}" "${SLURP_RULES[@]}"; do
        if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: recurring-findings rule '$r' missing from AGENTS.md" >&2; miss=1; fi
    done
    _ca_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/ca_selftest.$$.cpp")"
    case "$_ca_tmp" in *.cpp) ;; *) mv -f "$_ca_tmp" "$_ca_tmp.cpp" 2>/dev/null && _ca_tmp="$_ca_tmp.cpp" ;; esac
    # Positive: single-line and multi-line empty bodies must fire.
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {}\n}\n' > "$_ca_tmp"
    if [ -z "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow did not fire on a single-line empty catch (...) body" >&2; miss=1; fi
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {\n    }\n}\n' > "$_ca_tmp"
    if [ -z "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow did not fire on a multi-line empty catch (...) body" >&2; miss=1; fi
    # Negative: a justifying comment inside the body (policy escape) must NOT fire.
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {\n        // best-effort stringify for logging; failure renders "?"\n    }\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired despite a justifying comment inside the body" >&2; miss=1; fi
    # Negative: the hook vocabulary `// catch-all-ok:` on the catch line must NOT fire.
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {} // catch-all-ok: destructor path\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired despite a catch-all-ok marker" >&2; miss=1; fi
    # Negative: a body with a statement must NOT fire (the no-LOG tier stays editor-hook-advisory).
    printf 'void f() {\n    try {\n        g();\n    } catch (...) {\n        LOG_WARN("g failed");\n    }\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired on a non-empty catch body" >&2; miss=1; fi
    # Negative: an in-window SMATCHET_DEVIATION above the catch must escape.
    printf 'void f() {\n    try {\n        g();\n    // SMATCHET_DEVIATION(rule=catch-all-swallow; reason=test; owner=x; revisit=2099-01-01)\n    } catch (...) {}\n}\n' > "$_ca_tmp"
    if [ -n "$(scan_catch_all_swallow_file "$_ca_tmp")" ]; then
        echo "SELFTEST FAIL: catch-all-swallow fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_ca_tmp" 2>/dev/null || true
    # --- unbounded-recursive-json-walker — asserts-failure + bounded/deviation negatives. ---
    _jw_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/jw_selftest.$$.cpp")"
    case "$_jw_tmp" in *.cpp) ;; *) mv -f "$_jw_tmp" "$_jw_tmp.cpp" 2>/dev/null && _jw_tmp="$_jw_tmp.cpp" ;; esac
    printf 'static void Walk(const nlohmann::json& n) {\n    for (const auto& c : n) {\n        Walk(c);\n    }\n}\n' > "$_jw_tmp"
    if [ -z "$(scan_json_walker_file "$_jw_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-recursive-json-walker did not fire on an unbounded self-recursive json walker" >&2; miss=1; fi
    printf 'static void Walk(const nlohmann::json& n, int depth) {\n    if (depth > 256) {\n        return;\n    }\n    for (const auto& c : n) {\n        Walk(c, depth + 1);\n    }\n}\n' > "$_jw_tmp"
    if [ -n "$(scan_json_walker_file "$_jw_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-recursive-json-walker fired on a depth-bounded walker" >&2; miss=1; fi
    printf '// SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker; reason=test; owner=x; revisit=2099-01-01)\nstatic void Walk(const nlohmann::json& n) {\n    for (const auto& c : n) {\n        Walk(c);\n    }\n}\n' > "$_jw_tmp"
    if [ -n "$(scan_json_walker_file "$_jw_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-recursive-json-walker fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_jw_tmp" 2>/dev/null || true
    # --- unbounded-file-slurp — asserts-failure + comment/deviation negatives. ---
    _sl_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/sl_selftest.$$.cpp")"
    case "$_sl_tmp" in *.cpp) ;; *) mv -f "$_sl_tmp" "$_sl_tmp.cpp" 2>/dev/null && _sl_tmp="$_sl_tmp.cpp" ;; esac
    printf 'void f(std::ifstream& in) {\n    std::stringstream ss;\n    ss << in.rdbuf();\n}\n' > "$_sl_tmp"
    if [ -z "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp did not fire on an rdbuf() slurp" >&2; miss=1; fi
    printf 'void f(std::ifstream& in) {\n    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());\n}\n' > "$_sl_tmp"
    if [ -z "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp did not fire on an istreambuf_iterator slurp" >&2; miss=1; fi
    printf 'void f(std::ifstream& in) {\n    // a comment mentioning ss << in.rdbuf() must not fire\n    int x = 0;\n    (void)x;\n}\n' > "$_sl_tmp"
    if [ -n "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp fired on a comment mention" >&2; miss=1; fi
    printf 'void f(std::ifstream& in) {\n    std::stringstream ss;\n    // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=test; owner=x; revisit=2099-01-01)\n    ss << in.rdbuf();\n}\n' > "$_sl_tmp"
    if [ -n "$(scan_file_slurp_file "$_sl_tmp")" ]; then
        echo "SELFTEST FAIL: unbounded-file-slurp fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_sl_tmp" 2>/dev/null || true
    # --- ui-request-flag-off-thread (PR-5) — assert the rule is documented + fires on an unwrapped
    # request-flag write and stays quiet inside a RunOnUiThread closure / behind a deviation. ---
    if ! grep -qF "ui-request-flag-off-thread" AGENTS.md; then echo "SELFTEST FAIL: 'ui-request-flag-off-thread' rule missing from AGENTS.md" >&2; miss=1; fi
    # selftest: asserts-failure — a request-flag write outside RunOnUiThread must fire; inside it,
    # and behind a deviation, must not. (.cpp-gated, so the fixture carries a .cpp extension.)
    _ui_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/ui_selftest.$$.cpp")"
    case "$_ui_tmp" in *.cpp) ;; *) mv -f "$_ui_tmp" "$_ui_tmp.cpp" 2>/dev/null && _ui_tmp="$_ui_tmp.cpp" ;; esac
    # Positive: a direct off-thread write (no enclosing RunOnUiThread).
    printf 'void Handler() {\n    g_ui.requestScreenshotPath = path;\n    g_ui.requestScreenshot = true;\n}\n' > "$_ui_tmp"
    if [ -z "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread did not fire on a request-flag write outside a RunOnUiThread closure" >&2; miss=1; fi
    # Negative: the same write inside a RunOnUiThread closure (the conformant marshalling seam).
    printf 'CommandResult Handler(AppController& app) {\n    return RunOnUiThreadAsCommandResult(app, [path]() {\n        g_ui.requestScreenshotPath = path;\n        g_ui.requestScreenshot = true;\n        return CommandResult::Success();\n    });\n}\n' > "$_ui_tmp"
    if [ -n "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread fired on a write inside a RunOnUiThread closure" >&2; miss=1; fi
    # Negative: a READ / compare of a request flag (not an assignment) must NOT fire.
    printf 'void Poll() {\n    if (g_ui.requestScreenshot) {\n        DoShot();\n    }\n}\n' > "$_ui_tmp"
    if [ -n "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread fired on a read/compare (not an assignment)" >&2; miss=1; fi
    # Negative: an in-window SMATCHET_DEVIATION above the write must escape.
    printf 'void Handler() {\n    // SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; reason=test; owner=x; revisit=2099-01-01)\n    g_ui.requestScreenshot = true;\n}\n' > "$_ui_tmp"
    if [ -n "$(scan_ui_request_flag_file "$_ui_tmp")" ]; then
        echo "SELFTEST FAIL: ui-request-flag-off-thread fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_ui_tmp" 2>/dev/null || true
    # --- pr-numbered-temporal-comments — assert the rule is documented + fires on a dev-PR-number
    # comment, and stays quiet for product-domain "PR" (no number) / Issue refs / non-comment lines /
    # a deviation. ---
    if ! grep -qF "pr-numbered-temporal-comments" AGENTS.md; then echo "SELFTEST FAIL: 'pr-numbered-temporal-comments' rule missing from AGENTS.md" >&2; miss=1; fi
    _prc_tmp="$(mktemp --suffix=.cpp 2>/dev/null || echo "${TMPDIR:-/tmp}/prc_selftest.$$.cpp")"
    case "$_prc_tmp" in *.cpp) ;; *) mv -f "$_prc_tmp" "$_prc_tmp.cpp" 2>/dev/null && _prc_tmp="$_prc_tmp.cpp" ;; esac
    # Positive: each dev-PR-number comment shape must fire.
    printf '// PR 6: legacy key removed.\n// PR #1104 review MEDIUM-1.\n// CR PR#1218.\n// PR12 latency fix.\n' > "$_prc_tmp"
    _prc_hits="$(scan_pr_comment_file "$_prc_tmp" | wc -l | tr -d ' ')"
    if [ "$_prc_hits" != "4" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments expected 4 dev-PR-number hits, got $_prc_hits" >&2; miss=1; fi
    # Negative: product-domain "PR" with NO number must NOT fire.
    printf '// GitHub PR-only columns populated by the per-PR enrichment loop.\n// the JQL shorthand type:pr maps to is:pr.\n// a visible [PR] prefix so users tell PRs apart.\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired on product-domain PR usage (no number)" >&2; miss=1; fi
    # Negative: GitHub Issue refs / ADR refs must NOT fire (no PR prefix).
    printf '// capture-then-check (issue #1081); see ADR-0012.\n// drop the legacy field (#823).\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired on a GitHub Issue / ADR ref" >&2; miss=1; fi
    # Negative: a NON-comment code line carrying a PR<digit>-looking token must NOT fire.
    printf 'const int kPR12Threshold = 5;\nReadOnlyMode = true;\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired on a non-comment code line" >&2; miss=1; fi
    # Negative: an in-window SMATCHET_DEVIATION above the comment must escape.
    printf '// SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; reason=test; owner=x; revisit=2099-01-01)\n// PR 6: cited for the audit trail.\n' > "$_prc_tmp"
    if [ -n "$(scan_pr_comment_file "$_prc_tmp")" ]; then
        echo "SELFTEST FAIL: pr-numbered-temporal-comments fired despite an in-window SMATCHET_DEVIATION" >&2; miss=1; fi
    rm -f "$_prc_tmp" 2>/dev/null || true
    # --- interface-doc WARN (Gap B / Slice 2) — assert the rule WARNs on real drift, stays quiet
    # otherwise, and is documented. This exercises a FAILURE case (the pinned symbol changed without
    # a doc touch), satisfying both Slice 2's selftest contract and Slice 3's "assert-a-failure" rule.
    if ! grep -qF "interface-doc" AGENTS.md; then echo "SELFTEST FAIL: 'interface-doc' rule missing from AGENTS.md" >&2; miss=1; fi
    _idoc_pins=$'ITrackerIssueMutations::UpdateField'
    _idoc_hit=$'-    TrackerError UpdateField(const std::string& issueId, const TrackerField& field);\n+    Result<nlohmann::json, TrackerError> UpdateField(const std::string& issueId);'
    _idoc_miss=$'+    void SomeUnrelatedThing();'
    # selftest: asserts-failure — interface-doc must WARN on real drift (pinned symbol changed, doc untouched).
    if [ -z "$(interface_doc_emit "Source/Core/src/Tracker/AGENTS.md" 0 "$_idoc_pins" "$_idoc_hit" 2>&1 1>/dev/null)" ]; then
        echo "SELFTEST FAIL: interface-doc did not WARN when a doc-pinned symbol changed without a doc touch" >&2; miss=1; fi
    if [ -n "$(interface_doc_emit "Source/Core/src/Tracker/AGENTS.md" 1 "$_idoc_pins" "$_idoc_hit" 2>&1 1>/dev/null)" ]; then
        echo "SELFTEST FAIL: interface-doc WARNed despite the leaf doc being touched in the same diff" >&2; miss=1; fi
    if [ -n "$(interface_doc_emit "Source/Core/src/Tracker/AGENTS.md" 0 "$_idoc_pins" "$_idoc_miss" 2>&1 1>/dev/null)" ]; then
        echo "SELFTEST FAIL: interface-doc WARNed when the pinned symbol was absent from the header hunk" >&2; miss=1; fi
    st_py="$(resolve_python || true)"
    if [ -n "$st_py" ]; then
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/function_size_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/dup_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/agent_size_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/include_cycle_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/appcontroller_fan_in_audit.py" --selftest; then miss=1; fi
        if ! "$st_py" "$REPO_ROOT/agents/scripts/core/daemon_subprocess_timeout_audit.py" --selftest; then miss=1; fi
    else
        echo "test-lint-rules: WARN: no python interpreter; skipped function_size_audit.py / dup_audit.py / agent_size_audit.py / include_cycle_audit.py / appcontroller_fan_in_audit.py / daemon_subprocess_timeout_audit.py --selftest" >&2
    fi
    [ "$miss" -eq 0 ] && echo "selftest: AGENTS.md zone globs + comment + function-size + duplication rules in sync" || exit 1
    ;;

  full)
    compute_strict_triples | sed 's/^/  /'
    n=$(compute_strict_triples | wc -l)
    echo "strict-zone violations (grandfather candidates): $n"
    ;;

  scanwide)
    # First-party-wide no-raw-new / deviation-overdue set (debug + bats harness).
    # `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_wide_violations
    ;;

  scanglfw)
    # no-glfw-in-core-headers absolute-0 set over Source/Core/include headers (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_glfw_violations
    ;;

  scancmakeci)
    # cmake-local-gate-ci-scope absolute-0 set over CMakeLists.txt / cmake/*.cmake (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_cmake_ci_scope_violations
    ;;

  scanunusedcfg)
    # unused-symbol-under-config-guard absolute-0 set over first-party .cpp TUs (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_unused_under_config_guard_violations
    ;;

  scanbarejson)
    # bare-json-parse-untrusted set over ALL first-party C++ (.cpp/.h/.hpp) — whole-tree campaign
    # sweep (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_bare_json_parse_violations
    ;;

  scancatchall)
    # catch-all-swallow absolute-0 set over first-party C++ (debug + bats harness).
    # `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_catch_all_violations
    ;;

  scanjsonwalkers)
    # unbounded-recursive-json-walker WARN set over first-party C++ — whole-tree campaign sweep
    # (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_json_walker_violations
    ;;

  scanslurps)
    # unbounded-file-slurp WARN set over first-party C++ — whole-tree campaign sweep (debug +
    # bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_file_slurp_violations
    ;;

  scanuireqflag)
    # ui-request-flag-off-thread set over Source/Core/src/Commands (excl. Scenarios) .cpp TUs
    # (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_ui_request_flag_violations
    ;;

  scanprcomments)
    # pr-numbered-temporal-comments set over first-party C++ (.cpp/.h/.hpp) — whole-tree campaign
    # sweep (debug + bats harness). `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_pr_comment_violations
    ;;

  catalog)
    triples="$(compute_strict_triples)"
    # NB: NO timestamp / commit-sha in the body — the develop post-merge job
    # enforces drift via `git diff --exit-code`, so the file must be a pure
    # function of the violation set (byte-identical when nothing changed).
    # "when did it last change" lives in git history, not the file.
    gen_catalog() {
        echo "# High-Integrity C++ — grandfathered baseline"
        echo
        echo "_Auto-generated. Do not hand-edit; run \`bash agents/scripts/project/test-lint-rules.sh --catalog --refresh\` and commit._"
        echo "_Refreshed on \`develop\` post-merge (fail-on-drift); gate uses live scan vs \`origin/develop\`, not this file._"
        local total=0 rule cnt
        for rule in narrowing-conversions no-printf-stderr no-raw-new define-imgui deviation-overdue; do
            echo
            cnt="$(printf '%s\n' "$triples" | awk -F'\t' -v r="$rule" '$1==r' | grep -c . || true)"
            echo "## strict zone × $rule ($cnt entries)"
            if [ "$cnt" -eq 0 ]; then echo "- (none)"; else
                printf '%s\n' "$triples" | awk -F'\t' -v r="$rule" '$1==r{print "- `"$2"` · `"$3"`"}' | sort
            fi
            total=$((total+cnt))
        done
        echo
        echo "## Totals"
        echo "- strict-zone violators grandfathered: $total"
    }
    if [ "$REFRESH" -eq 1 ]; then
        mkdir -p "$(dirname "$BASELINE_FILE")"
        gen_catalog > "$BASELINE_FILE"
        echo "[test-lint-rules] refreshed $BASELINE_FILE"
    else
        gen_catalog
    fi
    ;;

  funcsizebaseline)
    # Refresh the informational function-size grandfather snapshot. Kept in its OWN file (not
    # co-mingled with $BASELINE_FILE) so the strict-catalog determinism contract is untouched; the
    # gate itself is a live merge-base delta (function_size_audit.py --diff), not this file.
    fs_py="$(resolve_python || true)"
    [ -n "$fs_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --funcsize-baseline" >&2; exit 2; }
    FUNCSIZE_BASELINE_FILE="docs/high-integrity/function-size-baseline.md"
    mkdir -p "$(dirname "$FUNCSIZE_BASELINE_FILE")"
    "$fs_py" "$REPO_ROOT/agents/scripts/core/function_size_audit.py" --baseline-md > "$FUNCSIZE_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $FUNCSIZE_BASELINE_FILE"
    ;;

  agentsizebaseline)
    # Refresh the informational agent-prompt/AGENTS.md size grandfather snapshot. Same contract as
    # --funcsize-baseline: the gate is a live merge-base delta (agent_size_audit.py --diff), not this
    # file. reduce-agent-prompt-bloat Slice 0.
    as_py="$(resolve_python || true)"
    [ -n "$as_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --agentsize-baseline" >&2; exit 2; }
    AGENTSIZE_BASELINE_FILE="docs/high-integrity/agent-size-baseline.md"
    mkdir -p "$(dirname "$AGENTSIZE_BASELINE_FILE")"
    "$as_py" "$REPO_ROOT/agents/scripts/core/agent_size_audit.py" --baseline-md > "$AGENTSIZE_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $AGENTSIZE_BASELINE_FILE"
    ;;

  dupbaseline)
    # Refresh the informational duplication grandfather snapshot (DRY pillar). Same contract as
    # --funcsize-baseline: the gate is a live merge-base delta (dup_audit.py --diff), not this file.
    dup_py="$(resolve_python || true)"
    [ -n "$dup_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --dup-baseline" >&2; exit 2; }
    DUP_BASELINE_FILE="docs/high-integrity/dup-baseline.md"
    mkdir -p "$(dirname "$DUP_BASELINE_FILE")"
    "$dup_py" "$REPO_ROOT/agents/scripts/core/dup_audit.py" --baseline-md > "$DUP_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $DUP_BASELINE_FILE"
    ;;

  includecyclebaseline)
    # Refresh the include-cycle grandfather/ratchet snapshot (core-include-dag Phase 0). Same
    # contract as --dup-baseline: the gate is a live merge-base delta (include_cycle_audit.py
    # --diff), not this file — but unlike dup this snapshot is also the RATCHET ledger (each later
    # phase deletes the line for the edge it kills).
    ic_py="$(resolve_python || true)"
    [ -n "$ic_py" ] || { echo "test-lint-rules: ERROR: no python interpreter for --include-cycle-baseline" >&2; exit 2; }
    INCLUDECYCLE_BASELINE_FILE="docs/high-integrity/include-cycle-baseline.md"
    mkdir -p "$(dirname "$INCLUDECYCLE_BASELINE_FILE")"
    "$ic_py" "$REPO_ROOT/agents/scripts/core/include_cycle_audit.py" --baseline-md > "$INCLUDECYCLE_BASELINE_FILE"
    echo "[test-lint-rules] refreshed $INCLUDECYCLE_BASELINE_FILE"
    ;;

  diff)
    BASE="${ARG:-origin/develop}"
    # narrowing-conversions is catalogue-only (not gated) — the base worktree has
    # no compile db, so it can't be diffed without false positives. Exclude it
    # from both sides of the set-diff. (See § Deviations in the plan.)
    head_set="$(compute_strict_triples | grep -v $'^narrowing-conversions\t' || true)"
    if [ -n "${SMATCHET_LINT_BASELINE_SET:-}" ] && [ -f "$SMATCHET_LINT_BASELINE_SET" ]; then
        base_set="$(sort -u "$SMATCHET_LINT_BASELINE_SET")"
    else
        # Compute the baseline triple-set against <ref> via a temp worktree so the
        # same scanner sees both trees without a checkout dance.
        # Fail closed (CR #507): an unresolved base or failed worktree must NOT
        # silently skip the gate in CI. Fetch origin/develop before invoking
        # locally if you hit this.
        if ! git rev-parse --verify --quiet "$BASE" >/dev/null 2>&1; then
            echo "test-lint-rules: ERROR: base '$BASE' unresolved; cannot compute delta gate" >&2
            exit 2
        fi
        wt="$(mktemp -d)"
        git worktree add -q --detach "$wt" "$BASE" 2>/dev/null || { echo "test-lint-rules: ERROR: worktree add for '$BASE' failed" >&2; exit 2; }
        # Run the CURRENT scanner (--root) against the base worktree so both sides
        # use identical logic, regardless of what scanner version the base tree ships.
        base_set="$(SMATCHET_LINT_BASELINE_SET="" bash "$SELF" --root "$wt" --full 2>/dev/null | sed -n 's/^  //p' | grep -v $'^narrowing-conversions\t' || true)"
        git worktree remove --force "$wt" 2>/dev/null || true
    fi
    # New triples = HEAD \ base.
    new_triples="$(comm -23 <(printf '%s\n' "$head_set" | sort -u) <(printf '%s\n' "$base_set" | sort -u) | grep -E . || true)"

    rc=0
    # --- strict-zone high-integrity rules (delta-gated) ---
    if [ -n "$new_triples" ]; then
        rc=1
        echo
        echo "FAIL: new strict-zone high-integrity violations vs $BASE:"
        printf '%s\n' "$new_triples" | sed 's/^/  /'
        echo "  Fix it, or add SMATCHET_DEVIATION(rule=...; reason=...; owner=...; revisit=...) above the line."
    else
        echo "[test-lint-rules] PASS — no new strict-zone violations vs $BASE"
    fi

    # --- comment-regrowth rules (repo-wide, delta-gated; reduce-source-comment-bloat Phase 4) ---
    # New noise comments (commented-out code / decorative banner / blank-comment run) fail ANYWHERE
    # in first-party C++ (never legitimate). comment_audit.py --diff classifies; a
    # `// SMATCHET_DEVIATION(rule=comment-...)` on the line above escapes a flagged line.
    # Fail CLOSED: a hard-fail gate that silently no-ops (missing python / missing script / crash)
    # would pass dirty PRs as false-clean. comment_audit.py's exit contract: 0=clean, 1=violations
    # (on stdout), >=2=infra error. resolve_python validates the interpreter actually
    # runs (skips the Windows python3 Store-alias stub that exits 49).
    cr_out=""
    cr_aud="$REPO_ROOT/agents/scripts/core/comment_audit.py"
    cr_py="$(resolve_python || true)"
    if [ -z "$cr_py" ]; then
        echo "test-lint-rules: ERROR: no python interpreter; cannot enforce comment-regrowth gate" >&2
        exit 2
    fi
    if [ ! -f "$cr_aud" ]; then
        echo "test-lint-rules: ERROR: missing $cr_aud; cannot enforce comment-regrowth gate" >&2
        exit 2
    fi
    # Capture exit code inside the `if` condition: a bare `x=$(cmd); rc=$?` would, under the CI
    # shell's `set -e`, abort the whole script the instant comment_audit.py exits non-zero (e.g. 1
    # = violations found) — before `rc=$?` ever runs. An assignment used as an `if` condition is
    # exempt from `set -e`, so this reliably captures 0 / 1 / >=2.
    if cr_out="$("$cr_py" "$cr_aud" --diff "$BASE")"; then cr_rc=0; else cr_rc=$?; fi
    if [ "$cr_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: comment_audit.py --diff failed (exit $cr_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$cr_out" ]; then
        rc=1
        echo
        echo "FAIL: new comment-noise vs $BASE (commented-out code / decorative banner / blank-comment run):"
        printf '%s\n' "$cr_out" | sed 's/^/  /'
        echo "  Delete the noise, or add SMATCHET_DEVIATION(rule=<comment-id>; reason=...; owner=...; revisit=...) above it."
    else
        echo "[test-lint-rules] PASS — no new comment-noise vs $BASE"
    fi

    # --- first-party-wide absolute rules (no-raw-new, deviation-overdue) ---
    # Enforced at 0 across ALL first-party C++ (Source/Core, Source/Plugins,
    # Source/Standalone — the comment_audit.py SWEEP_ROOTS), not just the strict
    # zone: every raw `new` must use make_unique or carry an exemption marker, and
    # no SMATCHET_DEVIATION may sit past its revisit= date, ANYWHERE. Absolute (no
    # grandfathering) — the tree is clean today, so any hit is a regression. The
    # other two grep rules stay strict-only: both have legitimate first-party uses
    # outside the strict zone (no-printf-stderr → Standalone CLI stdout; define-imgui
    # → the ImGui localization-alias macro in Ui).
    wide_out="$(compute_wide_violations)"
    if [ -n "$wide_out" ]; then
        rc=1
        echo
        echo "FAIL: first-party no-raw-new / deviation-overdue / no-detach (enforced everywhere, not just the strict zone):"
        printf '%s\n' "$wide_out" | sed 's/^/  /'
        echo "  no-raw-new: use std::unique_ptr + make_unique (or marker // C-ABI handle / // custom-deleter / // pimpl)."
        echo "  no-detach: route the worker through AppController::LaunchBackgroundTask (joined at shutdown), not std::thread().detach()."
        echo "  Or revisit the overdue SMATCHET_DEVIATION / add SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) above the line."
    else
        echo "[test-lint-rules] PASS — no first-party no-raw-new / deviation-overdue / no-detach (whole tree)"
    fi

    # --- no-glfw-in-core-headers (Source/Core/include headers; ABSOLUTE-0) ---
    # GLFW/glad/OpenGL #include in a Source/Core/ header breaks the DX12 dual-target
    # build (SmatchetCore_DX12 compiles these headers with no GL/GLFW toolchain). The
    # tree is GLFW-clean today, so any hit is a regression — absolute-0, no grandfathering
    # (same model as no-raw-new). A SMATCHET_DEVIATION(rule=no-glfw-in-core-headers; ...)
    # above the include escapes.
    glfw_out="$(compute_glfw_violations)"
    if [ -n "$glfw_out" ]; then
        rc=1
        echo
        echo "FAIL: GLFW/glad/OpenGL include in a Source/Core/include header (breaks the DX12 dual-target build):"
        printf '%s\n' "$glfw_out" | sed 's/^/  /'
        echo "  Move the GL/GLFW include into a .cpp (or Source/Standalone), not a Source/Core/ header,"
        echo "  or add SMATCHET_DEVIATION(rule=no-glfw-in-core-headers; reason=...; owner=...; revisit=...) above the include."
    else
        echo "[test-lint-rules] PASS — no GLFW/glad/OpenGL include in Source/Core/include headers"
    fi

    # --- cmake-local-gate-ci-scope (CMakeLists.txt / cmake/*.cmake; ABSOLUTE-0) ---
    # A message(FATAL_ERROR ...) keyed on a LOCAL-dev project.config knob (msvc_toolset_pin)
    # without a NOT DEFINED ENV{CI} scope FATALs every fresh-configure CI runner (incident
    # #1074 red-walled all 5 Windows required checks). The tree's one such guard is correctly
    # CI-scoped today, so any unguarded hit is a regression (absolute-0, no grandfathering —
    # same model as no-glfw). A `# SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; ...)`
    # within the guard window escapes.
    cmci_out="$(compute_cmake_ci_scope_violations)"
    if [ -n "$cmci_out" ]; then
        rc=1
        echo
        echo "FAIL: message(FATAL_ERROR ...) keyed on a LOCAL-dev knob (msvc_toolset_pin) without a NOT DEFINED ENV{CI} scope (FATALs every fresh-configure CI runner; incident #1074):"
        printf '%s\n' "$cmci_out" | sed 's/^/  /'
        echo "  Scope the guard to local dev: 'if(... AND NOT DEFINED ENV{CI})' (CI configures fresh + can't hit the stale-cache class),"
        echo "  or add '# SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; reason=...; owner=...; revisit=...)' within the guard block."
    else
        echo "[test-lint-rules] PASS — no un-CI-scoped local-knob CMake FATAL_ERROR"
    fi

    # --- ui-request-flag-off-thread (Source/Core/src/Commands, excl. Scenarios; ABSOLUTE-0) ---
    # A write to a g_ui request-flag field (requestWindow* / requestScreenshot*) from a command
    # handler must be marshalled onto the UI thread via RunOnUiThread* — the standalone main loop
    # polls those non-atomic fields each frame, so an unsynchronised write from an MCP/Lua worker
    # thread is a data race (UB — Pillar-3 never-crash). The command handlers all marshal correctly
    # today (BuiltinCommands_Debug.cpp), so any unwrapped write is a regression (absolute-0, no
    # grandfathering — same model as no-glfw). Scenarios/ is exempt (IScenario lifecycle runs on the
    # UI thread by contract). A SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; ...) above escapes.
    uireq_out="$(compute_ui_request_flag_violations)"
    if [ -n "$uireq_out" ]; then
        rc=1
        echo
        echo "FAIL: g_ui request-flag write outside a RunOnUiThread* closure in a command-dispatch TU (races the main loop that polls these non-atomic fields — Pillar-3 data race):"
        printf '%s\n' "$uireq_out" | sed 's/^/  /'
        echo "  Wrap the write in RunOnUiThreadAsCommandResult(app, [...]{ ...write... }) (see BuiltinCommands_Debug.cpp),"
        echo "  or add SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; reason=...; owner=...; revisit=...) above the write."
    else
        echo "[test-lint-rules] PASS — no off-UI-thread g_ui request-flag write in command-dispatch TUs"
    fi

    # --- unused-symbol-under-config-guard (CHANGED first-party .cpp; WARN-first) ---
    # A free-function definition unguarded while ALL its references sit inside a
    # POSITIVE #if defined(SMATCHET_WITH_*) block is dead in the feature-OFF build and
    # trips Clang's /WX -Werror,-Wunused-function (invisible to the PR-time MSVC
    # presets) — the #863 config-skew gate escape (fixed by 61b17427 / #945).
    #
    # WARN-FIRST / ADVISORY (calibration phase, mirrors the `duplication` + `interface-doc`
    # gates; never touches $rc) — and scoped to the files CHANGED in this diff, not the
    # whole tree. Rationale (plan § Risks + § Verification fallback): the heuristic is a
    # per-file text proxy, not the compiler, and the real tree carries a handful of
    # benign idioms it cannot statically distinguish from the #863 shape — a helper
    # called only in a SMATCHET_WITH_MCP path inside a TU that is itself MCP-gated, a
    # def whose only ref is in the #else of a `#if !defined(...)`. Shipping this as an
    # absolute-0 block would red-wall develop on those idioms; shipping it WARN-first
    # surfaces the #863 shape at PR time (the signal the gate exists for) without that
    # risk. The nightly Lua-OFF sanitizer build stays the authoritative backstop. It
    # graduates to a blocking rule once the FP rate is calibrated low (same path as the
    # DRY dup gate). A `// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; ...)`
    # above the def suppresses. Diff-scoped keeps it fast (per-file scan is heavy on the
    # 2000+-line monoliths) and focuses the WARN on newly-introduced code.
    uscg_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    uscg_changed="$(git diff --name-only --diff-filter=d "$uscg_mb" 2>/dev/null \
        | grep -E '\.cpp$' | grep -vE '(^|/)ThirdParty/' || true)"
    if [ -n "$uscg_changed" ]; then
        uscg_warn=""
        while IFS= read -r uscg_f; do
            [ -n "$uscg_f" ] || continue
            uscg_warn="$uscg_warn$(scan_unused_under_config_guard_file "$uscg_f")"$'\n'
        done <<< "$uscg_changed"
        uscg_warn="$(printf '%s' "$uscg_warn" | grep -E . || true)"
        if [ -n "$uscg_warn" ]; then
            {
                echo "[unused-symbol-under-config-guard] WARN: a free-function definition appears unguarded while all its in-file references sit under a positive #if defined(SMATCHET_WITH_*) guard — it would be dead (Clang -Werror,-Wunused-function) in the feature-OFF build (the #863 config-skew class). Advisory (calibration); not blocking:"
                printf '%s\n' "$uscg_warn" | sed 's/^/  /'
                echo "  If real: wrap the definition in the SAME #if defined(SMATCHET_WITH_...) as its call sites (see 61b17427 / #945)."
                echo "  If a false positive (e.g. an out-of-line member, an #else-branch impl, or a cross-TU helper): add"
                echo "  // SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; reason=...; owner=...; revisit=...) above the definition."
            } >&2
        fi
    fi

    # --- bare-json-parse-untrusted (ALL first-party C++, repo-wide default-deny; BLOCKING) ---
    # A bare nlohmann::json::parse( — or a `stream >> json` slurp — that does NOT route through
    # smatchet::json_safe::ParseBounded crashes uncatchably on a deeply-nested payload: nlohmann
    # builds the DOM iteratively but the RECURSIVE ~json teardown overflows the stack before any
    # try/catch can fire (issues #1271/#1287). The 3-arg non-throwing form (..., nullptr, false)
    # still builds the DOM and still overflows, so only ParseBounded escapes. BLOCKING (sets $rc=1).
    # Repo-wide default-deny (replaced the curated BARE_JSON_INGRESS_TUS allow-list, which lagged
    # the code every time the class recurred — #1573/#1592/#1598 all fixed TUs the list did not
    # watch); headers are in scope too (the JsonParseUtil.h class). A SMATCHET_DEVIATION(rule=
    # bare-json-parse-untrusted; ...) above the parse suppresses (bytes the program itself
    # serialised). The tree is clean today, so any hit in a changed file is a regression.
    # Diff-scoped to the CHANGED files (fast path); the whole-tree set is available via
    # --scan-bare-json for a campaign sweep and is asserted empty by the gate's bats.
    barejson_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    barejson_changed="$(git diff --name-only --diff-filter=d "$barejson_mb" 2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' \
        | grep -E '^Source/(Core|Plugins|Standalone)/' || true)"
    barejson_out=""
    if [ -n "$barejson_changed" ]; then
        while IFS= read -r barejson_f; do
            [ -n "$barejson_f" ] || continue
            barejson_out="$barejson_out$(scan_bare_json_parse_file "$barejson_f")"$'\n'
        done <<< "$barejson_changed"
        barejson_out="$(printf '%s' "$barejson_out" | grep -E . || true)"
    fi
    if [ -n "$barejson_out" ]; then
        {
            echo "[bare-json-parse-untrusted] FAIL: a bare nlohmann::json::parse( (or stream>>json slurp) appears in first-party C++ without routing through smatchet::json_safe::ParseBounded — a deeply-nested payload stack-overflows the recursive ~json DOM teardown before any try/catch can fire (#1271/#1287). Blocking, repo-wide (the curated-TU allow-list is retired):"
            printf '%s\n' "$barejson_out" | sed 's/^/  /'
            echo "  Route the decode through smatchet::json_safe::ParseBounded(text, errOut[, maxBytes]) or ParseBoundedOrDiscarded(text) (#include \"Json/BoundedJsonParse.h\")."
            echo "  If the bytes are provably program-internal (not external input): add"
            echo "  // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=...; owner=...; revisit=...) above the parse."
        } >&2
        rc=1
    fi

    # --- catch-all-swallow (ALL first-party C++; ABSOLUTE-0, BLOCKING) ---
    # An EMPTY catch (...) {} body silently swallows every exception — exception-handling-policy.md
    # hard rule 1 (review CRITICAL). The editor hook (lint-catch-all.py) flags it per-edit but is
    # not a merge gate; this closes that hole. The tree is at 0 today, so any hit is a regression
    # (absolute-0, no grandfathering — same model as no-glfw / no-raw-new). A comment inside the
    # body (documented silence), `// catch-all-ok: <reason>` on the catch line, or a
    # SMATCHET_DEVIATION(rule=catch-all-swallow; ...) above it escapes.
    catchall_out="$(compute_catch_all_violations)"
    if [ -n "$catchall_out" ]; then
        rc=1
        echo
        echo "FAIL: empty catch (...) body (swallows every exception silently — exception-handling-policy.md hard rule 1):"
        printf '%s\n' "$catchall_out" | sed 's/^/  /'
        echo "  Log with operation context (LOG_WARN/LOG_ERROR + function/key/backend), rethrow, or document the"
        echo "  silence with an inline comment / '// catch-all-ok: <reason>' per the policy's escape hatches,"
        echo "  or add SMATCHET_DEVIATION(rule=catch-all-swallow; reason=...; owner=...; revisit=...) above the catch."
    else
        echo "[test-lint-rules] PASS — no empty catch (...) body in first-party C++ (whole tree)"
    fi

    # --- unbounded-recursive-json-walker (CHANGED first-party C++; WARN-first) ---
    # The DW class from the security campaign (#1220/#1237): a self-recursive walker over a
    # nlohmann::json / sol::object parameter with no depth/budget bound overflows the stack on deep
    # nesting even when the parse was bounded. WARN-FIRST / ADVISORY (calibration, mirrors the
    # duplication precedent; never touches $rc) and diff-scoped to changed files — the two known
    # residuals (FieldCatalogCache::OptionFromJson, TrackerFieldValueParser::TrackerFieldOptionFromJson)
    # are transitively depth-bounded by ParseBounded's 256 cap upstream, so they warn only when
    # touched. Whole-tree sweep via --scan-json-walkers. A SMATCHET_DEVIATION(rule=
    # unbounded-recursive-json-walker; ...) above the definition suppresses.
    jw_changed="$(printf '%s\n' "$barejson_changed" | grep -E . || true)"
    jw_out=""
    if [ -n "$jw_changed" ]; then
        while IFS= read -r jw_f; do
            [ -n "$jw_f" ] || continue
            jw_out="$jw_out$(scan_json_walker_file "$jw_f")"$'\n'
        done <<< "$jw_changed"
        jw_out="$(printf '%s' "$jw_out" | grep -E . || true)"
    fi
    if [ -n "$jw_out" ]; then
        {
            echo "[unbounded-recursive-json-walker] WARN: a self-recursive function over a nlohmann::json / sol::object parameter carries no depth/budget bound — a deeply-nested value overflows the C++ stack even when the parse was bounded (the DW class; #1220/#1237). Advisory (calibration); not blocking:"
            printf '%s\n' "$jw_out" | sed 's/^/  /'
            echo "  Thread an int depth parameter (bail past ~256, mirroring kMaxAdfRecursionDepth) or rewrite to an explicit work-stack."
            echo "  If a false positive (bounded wrapper / mutual recursion misread): add"
            echo "  // SMATCHET_DEVIATION(rule=unbounded-recursive-json-walker; reason=...; owner=...; revisit=...) above the definition."
        } >&2
    fi

    # --- unbounded-file-slurp (CHANGED first-party C++; WARN-first) ---
    # A whole-file rdbuf()/istreambuf slurp with no byte cap reads an arbitrarily large file into
    # memory (the SECURITY_AUDIT #33 Win32-vs-POSIX cap asymmetry class). WARN-FIRST / ADVISORY
    # (calibration; never touches $rc), diff-scoped; residual sites warn only when touched.
    # Whole-tree sweep via --scan-slurps. A SMATCHET_DEVIATION(rule=unbounded-file-slurp; ...)
    # above the read suppresses.
    slurp_out=""
    if [ -n "$jw_changed" ]; then
        while IFS= read -r slurp_f; do
            [ -n "$slurp_f" ] || continue
            slurp_out="$slurp_out$(scan_file_slurp_file "$slurp_f")"$'\n'
        done <<< "$jw_changed"
        slurp_out="$(printf '%s' "$slurp_out" | grep -E . || true)"
    fi
    if [ -n "$slurp_out" ]; then
        {
            echo "[unbounded-file-slurp] WARN: a whole-file rdbuf()/istreambuf_iterator slurp has no byte cap — an oversized file is read fully into memory before any validation (SECURITY_AUDIT #33 class). Advisory (calibration); not blocking:"
            printf '%s\n' "$slurp_out" | sed 's/^/  /'
            echo "  Prefer a size-capped read (stat/tellg + reject over-limit, or ConfigManager::LoadJsonFile which caps at 64 MiB)."
            echo "  If the file is trusted/small by construction: add"
            echo "  // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=...; owner=...; revisit=...) above the read."
        } >&2
    fi

    # --- pr-numbered-temporal-comments (CHANGED first-party C++ comments; WARN-first) ---
    # A comment that pins a DEVELOPMENT pull-request number (`// PR 5`, `// PR #1104`, `PR#1218`,
    # `PR12`) is a temporal scaffold — it rots the moment the PR squash-merges and the number is
    # forgotten. Rewrite to durable present-tense intent (drop the PR-number token; keep the meaning).
    # Product-domain "PR" usages with no number ("PR-only", "type:pr", "per-PR") do NOT match.
    # WARN-FIRST / ADVISORY (calibration; never touches $rc), diff-scoped to the CHANGED first-party
    # C++ files (mirrors the bare-json + unused-symbol WARNs). A SMATCHET_DEVIATION(rule=pr-numbered-
    # temporal-comments; ...) on the line above suppresses. Whole-tree sweep via --scan-pr-comments.
    prc_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    prc_changed="$(git diff --name-only --diff-filter=d "$prc_mb" 2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true)"
    prc_out=""
    if [ -n "$prc_changed" ]; then
        while IFS= read -r prc_f; do
            [ -n "$prc_f" ] || continue
            prc_out="$prc_out$(scan_pr_comment_file "$prc_f")"$'\n'
        done <<< "$prc_changed"
        prc_out="$(printf '%s' "$prc_out" | grep -E . || true)"
    fi
    if [ -n "$prc_out" ]; then
        {
            echo "[pr-numbered-temporal-comments] WARN: a comment pins a development PR number (// PR <n> / PR#<n> / PRn) — a temporal scaffold that rots once the PR squash-merges. Rewrite to durable present-tense intent (drop the PR-number token, keep the meaning). Advisory (calibration); not blocking:"
            printf '%s\n' "$prc_out" | sed 's/^/  /'
            echo "  Rephrase the comment to state what the code does / why, without the dev-PR number (keep GitHub Issue / ADR refs)."
            echo "  If a specific historical PR genuinely must be cited (e.g. an audit trail): add"
            echo "  // SMATCHET_DEVIATION(rule=pr-numbered-temporal-comments; reason=...; owner=...; revisit=...) above the comment."
        } >&2
    fi

    # --- function-size rules (repo-wide, delta-gated, TIERED; decompose-top-20-monoliths Slice 0) ---
    # New functions over the hard cap (>120 lines non-UI / >200 lines ImGui-draw / >30 branches) —
    # or existing ones that JUST crossed it — fail anywhere in first-party C++. function_size_audit.py
    # keys by (rule, basename, qualified-name) and diffs HEAD vs the merge-base of $BASE, so the
    # existing monoliths are grandfathered (a grandfathered function growing further stays
    # grandfathered — same model as the comment-regrowth rules; lowering non-UI to 120 does NOT
    # retroactively fire existing 120-200-line functions — they're in both the HEAD and base sets).
    # Fail CLOSED on infra error, identical contract to the comment gate (0 clean / 1 violations /
    # >=2 infra). A `// SMATCHET_DEVIATION(rule=function-too-long; ...)` above the signature
    # suppresses one. The advisory soft-warning tier (>100 lines / >20 branches) is printed to
    # STDERR by the audit script, so it surfaces to the user but never enters $fs_out / the exit
    # code. $cr_py is the validated interpreter from above.
    fs_aud="$REPO_ROOT/agents/scripts/core/function_size_audit.py"
    if [ ! -f "$fs_aud" ]; then
        echo "test-lint-rules: ERROR: missing $fs_aud; cannot enforce function-size gate" >&2
        exit 2
    fi
    if fs_out="$("$cr_py" "$fs_aud" --diff "$BASE")"; then fs_rc=0; else fs_rc=$?; fi
    if [ "$fs_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: function_size_audit.py --diff failed (exit $fs_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$fs_out" ]; then
        rc=1
        echo
        echo "FAIL: new oversized functions vs $BASE (cap 120 lines non-UI / 200 lines ImGui-draw / 30 branches):"
        printf '%s\n' "$fs_out" | sed 's/^/  /'
        echo "  Decompose it (see docs/plans/shipped/decompose-top-20-monoliths.md § Approach), or add"
        echo "  SMATCHET_DEVIATION(rule=function-too-long; reason=...; owner=...; revisit=...) above the signature"
        echo "  (comma-separate the rule ids — rule=function-too-long,function-too-branchy — to suppress both caps)."
    else
        echo "[test-lint-rules] PASS — no new oversized functions vs $BASE"
    fi

    # --- include-cycle gate (Source/Core quote-include graph; delta-gated; BLOCKS) ---
    # A NEW SCC>1 include cycle OR a NEW low->high named-layer header back-edge in the Source/Core
    # quote-include graph fails the gate (core-include-dag Phase 0). include_cycle_audit.py keys each
    # violation by includer->resolved-target (path-pair, line-independent) and diffs HEAD vs the
    # merge-base of $BASE, so the current grandfathered edges (the ~5 baselined in
    # docs/high-integrity/include-cycle-baseline.md) stay green and only a genuinely-new cycle/back-edge
    # fires. Unlike the dup gate this FAILS CLOSED (exit 1 on a new violation) — the acyclicity ratchet
    # is the load-bearing deliverable that keeps the cleanup from regressing. A
    # `// SMATCHET_DEVIATION(rule=include-cycle; ...)` on the nearest non-blank line above the offending
    # #include suppresses it. Fail CLOSED on infra error too, identical contract to the function-size
    # gate (0 clean / 1 violations / >=2 infra). $cr_py is the validated interpreter from above.
    ic_aud="$REPO_ROOT/agents/scripts/core/include_cycle_audit.py"
    if [ ! -f "$ic_aud" ]; then
        echo "test-lint-rules: ERROR: missing $ic_aud; cannot enforce include-cycle gate" >&2
        exit 2
    fi
    if ic_out="$("$cr_py" "$ic_aud" --diff "$BASE")"; then ic_rc=0; else ic_rc=$?; fi
    if [ "$ic_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: include_cycle_audit.py --diff failed (exit $ic_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$ic_out" ]; then
        rc=1
        echo
        printf '%s\n' "$ic_out" | sed 's/^/  /'
    else
        echo "[test-lint-rules] PASS — no new Source/Core include cycle / layer back-edge vs $BASE"
    fi

    # --- AppController.h fan-in ratchet (Source/-wide quote-includers; delta-gated; BLOCKS) ---
    # A NEW Source/-wide quote-form `#include "AppController.h"` includer above the merge-base count
    # fails the gate (appcontroller-fan-in Phase 1). appcontroller_fan_in_audit.py --diff is DOWN-only
    # and hard-FAILs (exit 1) on a regression; a `// SMATCHET_DEVIATION(rule=app-controller-fan-in; ...)`
    # above the offending #include escapes a genuinely-needed new includer. Fails CLOSED on infra error
    # (0 clean / 1 new includer / >=2 infra), same contract as the include-cycle gate above.
    fi_aud="$REPO_ROOT/agents/scripts/core/appcontroller_fan_in_audit.py"
    if [ ! -f "$fi_aud" ]; then
        echo "test-lint-rules: ERROR: missing $fi_aud; cannot enforce app-controller-fan-in gate" >&2
        exit 2
    fi
    if fi_out="$("$cr_py" "$fi_aud" --diff "$BASE")"; then fi_rc=0; else fi_rc=$?; fi
    if [ "$fi_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: appcontroller_fan_in_audit.py --diff failed (exit $fi_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$fi_out" ]; then
        rc=1
        echo
        printf '%s\n' "$fi_out" | sed 's/^/  /'
    else
        echo "[test-lint-rules] PASS — no new AppController.h includer (fan-in ratchet) vs $BASE"
    fi

    # --- soft comment-ratio warning (ADVISORY — never changes exit code) ---
    ratio_warn_for "$BASE" || true

    # --- duplication BLOCKING (DRY Engineering Pillar 5; fails CLOSED) ---
    # dup_audit.py --diff graduated WARN→blocking on 2026-06-21 (ADR-0015 calibration complete): it
    # prints [dup] FAIL lines to stderr for NEW cross-file copy-paste clones and exits 1, which now
    # FAILS the gate ($rc=1). An infra error (>=2) also fails CLOSED. Exempt a genuine NEW clone with
    # a // SMATCHET_DEVIATION(rule=duplication; ...) marker on/above either occurrence.
    dup_aud="$REPO_ROOT/agents/scripts/core/dup_audit.py"
    if [ -f "$dup_aud" ] && [ -n "$cr_py" ]; then
        if ! "$cr_py" "$dup_aud" --diff "$BASE"; then
            echo "test-lint-rules: FAIL: NEW copy-paste clone(s) (rule=duplication, DRY Pillar 5) — de-duplicate or exempt with SMATCHET_DEVIATION(rule=duplication)" >&2
            rc=1
        fi
    fi

    # --- interface-doc WARN (Gap B / Slice 2; ADVISORY — never changes exit code) ---
    # Symbol-pinned: WARNs when a leaf-doc-embedded `Type::method` token appears in a changed
    # interface header's diff hunk without the doc being touched (the stale-contract-in-leaf-doc
    # class — e.g. the ITrackerIssueMutations::UpdateField signature slip). Pure-bash, no python,
    # no signature parsing; stderr-only, $rc untouched. See INTERFACE_DOC_MAP for the curated map.
    interface_doc_warn "$BASE"

    # --- agent-prompt / AGENTS.md size rule (delta-gated; reduce-agent-prompt-bloat Slice 0) ---
    # New agent prompts (agents/core, agents/project) over 250 lines — or AGENTS.md over 150, or an
    # existing one that JUST crossed its cap — fail. agent_size_audit.py keys by (rule, path) and
    # diffs HEAD vs the merge-base of $BASE, so the current whales (debug-detective, git-janitor,
    # test-author, AGENTS.md) are grandfathered (a grandfathered file shrinking — or even growing —
    # stays grandfathered; only a NEW over-cap file or an under->over crossing fires). The sink
    # classes (docs/agent-rules, agents/_shared/skills) are soft-warn-only and never enter $rc. Fail
    # CLOSED on infra error, identical contract to the function-size gate (0 clean / 1 violations /
    # >=2 infra). A line reading SMATCHET_DEVIATION(rule=agent-too-long; ...) anywhere in the file
    # suppresses it. Advisory [agent-size] WARN lines go to STDERR (never enter $as_out / exit code).
    # NB: this gate ALSO runs as its own required-check step in doc-validation.yml, which (unlike the
    # Windows build job that hosts this script) fires on docs-only PRs — the exact agent-regrowth
    # vector. Keeping it here too covers code PRs + local test-all.sh. $cr_py is the validated interp.
    as_aud="$REPO_ROOT/agents/scripts/core/agent_size_audit.py"
    if [ ! -f "$as_aud" ]; then
        echo "test-lint-rules: ERROR: missing $as_aud; cannot enforce agent-size gate" >&2
        exit 2
    fi
    if as_out="$("$cr_py" "$as_aud" --diff "$BASE")"; then as_rc=0; else as_rc=$?; fi
    if [ "$as_rc" -ge 2 ]; then
        echo "test-lint-rules: ERROR: agent_size_audit.py --diff failed (exit $as_rc) for base '$BASE'" >&2
        exit 2
    fi
    if [ -n "$as_out" ]; then
        rc=1
        echo
        echo "FAIL: new over-budget agent prompts / AGENTS.md vs $BASE (cap 250 lines prompt / 150 lines AGENTS.md):"
        printf '%s\n' "$as_out" | sed 's/^/  /'
        echo "  Extract procedure-bodies to a skill (see docs/agent-rules/AGENT-VS-SKILL.md), or add"
        echo "  SMATCHET_DEVIATION(rule=agent-too-long; reason=...; owner=...; revisit=...) in the file."
    else
        echo "[test-lint-rules] PASS — no new over-budget agent prompts / AGENTS.md vs $BASE"
    fi

    exit "$rc"
    ;;
esac
