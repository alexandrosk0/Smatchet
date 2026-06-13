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
    --scan-file)   MODE=scanfile; ARG="${2:-}" ;;
    --scan-file=*) MODE=scanfile; ARG="${1#--scan-file=}" ;;
    --full)        MODE=full ;;
    --scan-wide)   MODE=scanwide ;;
    --scan-glfw)   MODE=scanglfw ;;
    --scan-cmake-ci) MODE=scancmakeci ;;
    --scan-unused-cfg) MODE=scanunusedcfg ;;
    --selftest)    MODE=selftest ;;
    "")            MODE=diff ;;
    *) echo "usage: $0 [--diff[=]<ref>|--catalog [--refresh]|--funcsize-baseline|--agentsize-baseline|--dup-baseline|--scan-file[=]<f>|--full|--scan-wide|--scan-glfw|--scan-cmake-ci|--scan-unused-cfg|--selftest]" >&2; exit 2 ;;
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
    else
        echo "test-lint-rules: WARN: no python interpreter; skipped function_size_audit.py / dup_audit.py / agent_size_audit.py --selftest" >&2
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

    # --- soft comment-ratio warning (ADVISORY — never changes exit code) ---
    ratio_warn_for "$BASE" || true

    # --- duplication WARN (DRY Engineering Pillar 5; ADVISORY — never changes exit code) ---
    # dup_audit.py --diff is WARN-first (calibration phase per ADR-0015): it prints [dup] WARN
    # lines to stderr for NEW cross-file copy-paste clones and ALWAYS exits 0. It graduates to a
    # blocking rule only once the FP rate is < 10% over ~20 PRs (dry-pillar-dup-gate § Verification);
    # until then it must NEVER touch $rc. An infra error (>=2) is surfaced but stays non-fatal here.
    dup_aud="$REPO_ROOT/agents/scripts/core/dup_audit.py"
    if [ -f "$dup_aud" ] && [ -n "$cr_py" ]; then
        "$cr_py" "$dup_aud" --diff "$BASE" || \
            echo "test-lint-rules: WARN: dup_audit.py --diff exited non-zero (advisory; not failing the gate)" >&2
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
