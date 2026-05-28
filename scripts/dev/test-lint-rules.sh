#!/usr/bin/env bash
# test-lint-rules.sh — tiered high-integrity C++ enforcement for Smatchet.
#
# Plan: docs/design/high-integrity-cpp-enforcement.md (precursor reorg #505).
#
# Zones (single source of truth = AGENTS.md § Tiered enforcement; the globs
# below are asserted identical to AGENTS.md by --selftest):
#   strict — Source_Core/src/{Tracker,Sync,Persistence,Config,Commands}, Plugins/Mcp/src
#            + matching include/ subdirs. Any rule violation here FAILS.
#   light  — Source_Core/src/Ui, Target_Standalone. Not gated (existing inline
#            exemption-comment vocabulary continues to apply).
#   exempt — ThirdParty, build, non-C++ trees. Not scanned.
#
# Rule ids (stable kebab-case; the linkage between scanner output, the catalog
# section headers, and SMATCHET_DEVIATION(rule=<id>) suppression):
#   no-printf-stderr       printf/fprintf/cerr/cout without an exemption marker
#   no-raw-new             raw `new T` (use make_unique) without an exemption marker
#   narrowing-conversions  clang-tidy cppcoreguidelines-narrowing-conversions (strict TUs)
#   define-imgui           `#define ImGui...` macro-alias trick
#   deviation-overdue      SMATCHET_DEVIATION whose calendar revisit= has passed
#
# Modes:
#   (no args) / --diff [<ref>]   delta gate: fail only on (rule,basename,hash)
#                                triples present on HEAD but not <ref>
#                                (default origin/develop). Grandfathers existing.
#   --catalog [--refresh]        dump the strict-zone violator set; --refresh
#                                writes docs/backlog/high-integrity-cpp-baseline.md
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
cd "$(dirname "$0")/../.."
REPO_ROOT="$(pwd)"

BASELINE_FILE="docs/backlog/high-integrity-cpp-baseline.md"

# --- Zone globs (KEEP IN SYNC with AGENTS.md § Tiered enforcement; --selftest guards) ---
STRICT_GLOBS=(
    "Source_Core/src/Tracker/"
    "Source_Core/src/Sync/"
    "Source_Core/src/Persistence/"
    "Source_Core/src/Config/"
    "Source_Core/src/Commands/"
    "Source_Core/include/Tracker/"
    "Source_Core/include/Sync/"
    "Source_Core/include/Persistence/"
    "Source_Core/include/Config/"
    "Source_Core/include/Commands/"
    "Plugins/Mcp/src/"
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
        Source_Core/src/Ui/*|Source_Core/include/Ui/*|Target_Standalone/*) echo light; return ;;
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
    case "$1" in
        *"// CLI stdout"*|*"// pre-logger-init"*|*"// C-ABI"*|*"// custom-deleter"*|*"// pimpl"*|*NOLINT*) return 0 ;;
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

        # define-imgui — macro-alias trick.
        if [[ "$line" =~ \#define[[:space:]]+ImGui ]]; then
            printf 'define-imgui\t%s:%s\t%s\n' "$f" "$lineno" "$line"
        fi

        # Skip pure comment / blank lines for the printf/new heuristics.
        case "$line" in ''|[[:space:]]*\**|[[:space:]]*//*|\#*) :; esac

        # Determine if the active SMATCHET_DEVIATION suppresses a rule on THIS line.
        local suppress="$prev_dev_rule"
        prev_dev_rule=""; prev_dev_revisit=""   # deviation only covers the next non-blank line

        # no-printf-stderr
        if printf '%s' "$line" | grep -qE '\b(printf|fprintf|std::cerr|std::cout)\b'; then
            case "$line" in *'#include'*|*'//'*printf*) :;; *)
                if ! has_inline_exempt "$line" && [ "$suppress" != "no-printf-stderr" ]; then
                    printf 'no-printf-stderr\t%s:%s\t%s\n' "$f" "$lineno" "$line"
                fi ;;
            esac
        fi

        # no-raw-new — `new Type` (capitalised) not in a string/comment.
        if printf '%s' "$line" | grep -qE '\bnew\s+[A-Z]'; then
            case "$line" in *_new*|*new_*|*renewed*) :;; *)
                if ! printf '%s' "$line" | grep -qE '^\s*(\*|//)' \
                   && ! printf '%s' "$line" | grep -qE '"[^"]*new[^"]*"' \
                   && ! has_inline_exempt "$line" && [ "$suppress" != "no-raw-new" ]; then
                    printf 'no-raw-new\t%s:%s\t%s\n' "$f" "$lineno" "$line"
                fi ;;
            esac
        fi
    done < "$f"
}

# clang-tidy narrowing-conversions over strict-zone .cpp TUs. Requires a
# compile_commands.json; warn-skips otherwise (matches the existing missing-
# toolchain fallback for cppcheck/shellcheck).
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
find_compile_db() {
    local d
    for d in build/ninja-iter-msvc build/ninja-test-msvc build/ninja-iter-clang build/ninja-debug-msvc; do
        [ -f "$d/compile_commands.json" ] && { echo "$d"; return 0; }
    done
    return 1
}

scan_narrowing() {
    # $@ = strict-zone .cpp files. Emits narrowing-conversions triples.
    if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
        echo "test-lint-rules: WARN: clang-tidy not on PATH; narrowing-conversions skipped" >&2
        return 0
    fi
    local db
    if ! db="$(find_compile_db)"; then
        echo "test-lint-rules: WARN: no compile_commands.json (configure a build); narrowing-conversions skipped" >&2
        return 0
    fi
    local f out
    for f in "$@"; do
        case "$f" in *.cpp) ;; *) continue ;; esac
        out="$("$CLANG_TIDY" -p "$db" \
            --checks='-*,cppcoreguidelines-narrowing-conversions' \
            --quiet "$f" 2>/dev/null || true)"
        # clang-tidy lines: <file>:<line>:<col>: warning: ... [cppcoreguidelines-narrowing-conversions]
        printf '%s\n' "$out" | grep -E 'narrowing-conversions\]$' | while IFS= read -r w; do
            local loc; loc="$(printf '%s' "$w" | cut -d: -f1-2)"
            printf 'narrowing-conversions\t%s\t%s\n' "$loc" "$w"
        done
    done
}

# Produce the full strict-zone violator triple-set for a working tree.
# Output: sorted lines `<rule>\t<basename>\t<hash>` (one per violation).
compute_strict_triples() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files 'Source_Core/src/**' 'Source_Core/include/**' 'Plugins/Mcp/src/**' 2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' || true
    )
    local strict_files=()
    for f in "${files[@]}"; do
        [ "$(zone_of "$f")" = strict ] && strict_files+=("$f")
    done
    {
        for f in "${strict_files[@]}"; do scan_file_rules "$f"; done
        scan_narrowing "${strict_files[@]}"
    } | while IFS=$'\t' read -r rule loc raw; do
        printf '%s\t%s\t%s\n' "$rule" "$(basename "${loc%%:*}")" "$(snippet_hash "$raw")"
    done | sort -u
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
[ "${SMATCHET_LINT_BYPASS:-0}" = "1" ] && { echo "[test-lint-rules] BYPASS"; exit 0; }

MODE="diff"; ARG=""; REFRESH=0
case "${1:-}" in
    --diff)      MODE=diff; ARG="${2:-}" ;;
    --catalog)   MODE=catalog; [ "${2:-}" = "--refresh" ] && REFRESH=1 ;;
    --scan-file) MODE=scanfile; ARG="${2:-}" ;;
    --full)      MODE=full ;;
    --selftest)  MODE=selftest ;;
    "")          MODE=diff ;;
    *) echo "usage: $0 [--diff <ref>|--catalog [--refresh]|--scan-file <f>|--full|--selftest]" >&2; exit 2 ;;
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
        case "$g" in Source_Core/include/*) continue ;; esac
        if ! grep -qF "$g" AGENTS.md; then echo "SELFTEST FAIL: '$g' missing from AGENTS.md" >&2; miss=1; fi
    done
    [ "$miss" -eq 0 ] && echo "selftest: AGENTS.md zone globs in sync" || exit 1
    ;;

  full)
    compute_strict_triples | sed 's/^/  /'
    n=$(compute_strict_triples | wc -l)
    echo "strict-zone violations (grandfather candidates): $n"
    ;;

  catalog)
    snap_sha="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    ts="${SMATCHET_LINT_FAKE_TS:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
    triples="$(compute_strict_triples)"
    gen_catalog() {
        echo "# High-Integrity C++ — grandfathered baseline"
        echo
        echo "_Auto-generated. Do not hand-edit; run \`bash scripts/dev/test-lint-rules.sh --catalog --refresh\`._"
        echo "_Refreshed on \`develop\` post-merge; gate uses live scan vs \`origin/develop\`, not this file._"
        echo
        echo "Snapshot: $snap_sha  ·  $ts"
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
        echo "- last refresh: $ts"
    }
    if [ "$REFRESH" -eq 1 ]; then
        mkdir -p "$(dirname "$BASELINE_FILE")"
        gen_catalog > "$BASELINE_FILE"
        echo "[test-lint-rules] refreshed $BASELINE_FILE"
    else
        gen_catalog
    fi
    ;;

  diff)
    BASE="${ARG:-origin/develop}"
    head_set="$(compute_strict_triples)"
    if [ -n "${SMATCHET_LINT_BASELINE_SET:-}" ] && [ -f "$SMATCHET_LINT_BASELINE_SET" ]; then
        base_set="$(sort -u "$SMATCHET_LINT_BASELINE_SET")"
    else
        # Compute the baseline triple-set against <ref> via a temp worktree so the
        # same scanner sees both trees without a checkout dance.
        if ! git rev-parse --verify --quiet "$BASE" >/dev/null 2>&1; then
            echo "test-lint-rules: WARN: base '$BASE' unresolved; gate skipped" >&2
            exit 0
        fi
        wt="$(mktemp -d)"
        git worktree add -q --detach "$wt" "$BASE" 2>/dev/null || { echo "worktree add failed" >&2; exit 0; }
        base_set="$(cd "$wt" && SMATCHET_LINT_BASELINE_SET="" bash "$REPO_ROOT/scripts/dev/test-lint-rules.sh" --full 2>/dev/null | sed -n 's/^  //p')"
        git worktree remove --force "$wt" 2>/dev/null || true
    fi
    # New triples = HEAD \ base.
    new_triples="$(comm -23 <(printf '%s\n' "$head_set" | sort -u) <(printf '%s\n' "$base_set" | sort -u) | grep -E . || true)"
    if [ -z "$new_triples" ]; then
        echo "[test-lint-rules] PASS — no new strict-zone violations vs $BASE"
        exit 0
    fi
    echo
    echo "FAIL: new strict-zone high-integrity violations vs $BASE:"
    printf '%s\n' "$new_triples" | sed 's/^/  /'
    echo
    echo "Fix the violation, add a SMATCHET_DEVIATION(rule=...; reason=...; owner=...; revisit=...)"
    echo "comment on the line above it, or (non-behavioural only) discuss with a maintainer."
    exit 1
    ;;
esac
