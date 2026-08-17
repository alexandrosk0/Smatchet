#!/usr/bin/env bash
# 00-common.sh — shared helpers: zone globs, hashing, deviation parsing, rule-id registry, file listing (sourced by test-lint-rules.sh, not run directly).
# shellcheck disable=SC2034  # DEV_RE + the rule-id registries are consumed by the entry point and sibling modules.

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

# Parse SMATCHET_DEVIATION(rule=X; ...; revisit=Y) on the line ABOVE a target.
# Returns the suppressed rule-id via stdout if the comment suppresses `want`.
# Also emits deviation-overdue when revisit is a passed calendar marker.
DEV_RE='SMATCHET_DEVIATION\(([^)]*)\)'

# Companion to DEV_RE for the `revisit=` field only. DEV_RE's body capture is `[^)]*`, so it stops
# at the FIRST ')': a reason= carrying a parenthetical ("... include set (ConfigManager / backends);
# owner=...; revisit=2026-12-31)") hides owner= and revisit= from the split above. Suppression
# survives that (rule= precedes any paren) but the EXPIRY does not — the marker becomes permanently
# un-auditable, which is the fail-open direction. Read revisit= off the whole marker line instead,
# bounded by the next ';' or ')'.
DEV_REVISIT_RE='revisit=([^;)]+)'

today_ymd() { date +%Y-%m-%d; }

# True if $1 LOOKS like someone meant a calendar revisit (starts YYYY-) but is not a value
# revisit_overdue can compare. Every such value used to fall through to the slug branch and
# silently become a PERMANENT exemption — the fail-open direction, and the failure is invisible
# because the marker still suppresses. Three shapes were reaching that branch:
#   2026-19-30  month 19 passes the [0-1][0-9] glob and string-sorts after today -> never overdue
#   2020-1-1    not zero-padded, misses the glob entirely -> read as a slug -> never overdue
#   2026-02-30  a day that does not exist in that month; the comparison is lexicographic, not a
#               date parse, so nothing ever rejects it
# Pure bash (no `date -d`) so the git-bash toolchain behaves the same as the ubuntu runner.
revisit_datelike_but_invalid() {
    local r="$1" y m d dmax
    case "$r" in
        [0-9][0-9][0-9][0-9]-Q[1-4]) return 1 ;;                       # valid quarter
        [0-9][0-9][0-9][0-9]-*) ;;                                     # date-shaped, keep checking
        *) return 1 ;;                                                 # slug / never — not a date attempt
    esac
    case "$r" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
        *) return 0 ;;                                                 # e.g. 2020-1-1, 2026-Q5
    esac
    y="${r%%-*}"; m="${r:5:2}"; d="${r:8:2}"
    case "$m" in 0[1-9]|1[0-2]) ;; *) return 0 ;; esac                 # e.g. 2026-19-30
    case "$m" in
        01|03|05|07|08|10|12) dmax=31 ;;
        04|06|09|11)          dmax=30 ;;
        02) if [ $((10#$y % 4)) -eq 0 ] && { [ $((10#$y % 100)) -ne 0 ] || [ $((10#$y % 400)) -eq 0 ]; }; then
                dmax=29
            else
                dmax=28
            fi ;;
    esac
    [ "$((10#$d))" -ge 1 ] && [ "$((10#$d))" -le "$dmax" ] && return 1
    return 0                                                           # e.g. 2026-02-30
}

revisit_overdue() {
    # $1 = revisit value. Overdue iff YYYY-MM-DD < today, or YYYY-Qn end < today. A value that was
    # clearly MEANT as a date but is not one is reported overdue too — failing closed, so a typo'd
    # revisit gets re-written instead of quietly buying a permanent exemption.
    local r="$1" today; today="$(today_ymd)"
    if revisit_datelike_but_invalid "$r"; then return 0; fi
    case "$r" in
        [0-9][0-9][0-9][0-9]-[0-1][0-9]-[0-3][0-9])
            [ "$r" \< "$today" ] && return 0 || return 1 ;;
        [0-9][0-9][0-9][0-9]-Q[1-4])
            local y q endm end
            y="${r%-Q*}"; q="${r#*-Q}"
            case "$q" in 1) endm=03-31;; 2) endm=06-30;; 3) endm=09-30;; 4) endm=12-31;; esac
            end="$y-$endm"
            [ "$end" \< "$today" ] && return 0 || return 1 ;;
        *) return 1 ;;  # slug / never -> never overdue
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

# Shared first-party C++ file listing for the whole-tree compute_* sweeps below (one place so the
# four sweeps cannot drift; ThirdParty excluded, tests out of scope by root).
list_first_party_cpp_files() {
    git ls-files \
        'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
        2>/dev/null \
        | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
}
