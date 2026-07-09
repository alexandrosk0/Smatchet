#!/usr/bin/env bash
# test-required-context-adr-consistency.sh — flag a required_contexts addition
# that an ADR / shipped plan explicitly rejected.
# ----------------------------------------------------------------------------
# WHY (tooling backlog 2026-07-05 adr-reversal-required-context-gate)
#   The all-gates-blocking flip's first draft silently added `Intent section` +
#   `Plan-lock gate` to branch_protection.required_contexts — a route ADR-0022
#   and plan-lock-enforcement Q7 had EXPLICITLY REJECTED (their label hatches
#   cannot reach GitHub branch protection, so a red + override label would be
#   unmergeable). Code review caught it by hand; this gate catches the class.
#
# WHAT IT ASSERTS
#   For each context name ADDED to project.config.json
#   branch_protection.required_contexts relative to the base ref
#   (origin/develop), grep docs/adr/*.md + docs/plans/shipped/*.md for that
#   name; if a mention sits inside a rejection context (±2 lines matching
#   "reject" / "NOT a required" / "do not add" / "must not", case-insensitive),
#   FAIL with the citation — forcing a superseding ADR note before the
#   promotion ships. Removals and pre-existing names are never checked.
#
# SEAMS (production leaves all unset)
#   RC_ADR_ROOT            repo root override (fixture tree with its own
#                          project.config.json + docs/).
#   RC_ADR_BASE_CONTEXTS   newline-separated base context set; overrides the
#                          `git show <base>:project.config.json` read entirely
#                          (set-but-EMPTY honoured — every head name is "added").
#   RC_ADR_BASE_REF        base ref for the git read (default origin/develop).
#
# MODES
#   (no args) | --check   diff live config vs base; FAIL (1) on any
#                         ADR-rejected addition. Base ref unreadable and no
#                         override → WARN + pass (shallow local clone; CI
#                         checkouts use fetch-depth 0 so the ref exists there).
#   --selftest            fixture dogfood: a rejected addition FAILS; a clean
#                         addition and a pre-existing rejected name PASS.
#
# EXIT  0 clean · 1 rejected addition / selftest failure · 2 infra error.
# Goes through test-shell-lint.sh + the test-gate-selftests.sh marker check.
# selftest: asserts-failure
# ----------------------------------------------------------------------------
set -uo pipefail

command -v jq >/dev/null 2>&1 || { echo "test-required-context-adr-consistency: jq required" >&2; exit 2; }

REJECT_RE='reject|NOT a required|do not add|must not'

# head_contexts <root> — the working-tree required_contexts, one per line.
head_contexts() {
    local cfg="$1/project.config.json"
    [ -f "$cfg" ] || { echo "test-required-context-adr-consistency: missing $cfg" >&2; return 2; }
    jq -r '.branch_protection.required_contexts[]? // empty' "$cfg"
}

# doc_files <root> — the rejection corpus.
doc_files() {
    local root="$1" f
    for f in "$root"/docs/adr/*.md "$root"/docs/plans/shipped/*.md; do
        [ -f "$f" ] && printf '%s\n' "$f"
    done
    return 0
}

# check_added <root> <base-contexts-newline> — grep each ADDED name's doc
# mentions for a rejection window. Prints citations; returns 0 clean / 1 hit.
check_added() {
    local root="$1" base="$2" name f ln start end window rc=0 added=0
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case $'\n'"$base"$'\n' in *$'\n'"$name"$'\n'*) continue ;; esac   # pre-existing
        added=$((added + 1))
        while IFS= read -r f; do
            while IFS=: read -r ln _; do
                [ -n "$ln" ] || continue
                start=$((ln > 2 ? ln - 2 : 1)); end=$((ln + 2))
                window="$(sed -n "${start},${end}p" "$f")"
                if printf '%s' "$window" | grep -qiE "$REJECT_RE"; then
                    echo "  - '$name' — rejected at $f:$ln (add a superseding ADR note or drop the promotion)" >&2
                    rc=1
                fi
            done < <(grep -nF -- "$name" "$f" 2>/dev/null || true)
        done < <(doc_files "$root")
    done < <(head_contexts "$root")
    if [ "$rc" -ne 0 ]; then
        echo "test-required-context-adr-consistency: FAIL — required_contexts addition(s) an ADR/shipped plan explicitly rejected (above)." >&2
        return 1
    fi
    echo "test-required-context-adr-consistency: PASS — $added added required context(s), none ADR-rejected."
    return 0
}

run_check() {
    local root base_ref base
    root="${RC_ADR_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
    if [ -n "${RC_ADR_BASE_CONTEXTS+x}" ]; then
        base="$RC_ADR_BASE_CONTEXTS"
    else
        base_ref="${RC_ADR_BASE_REF:-origin/develop}"
        if ! base="$(git -C "$root" show "$base_ref:project.config.json" 2>/dev/null \
                        | jq -r '.branch_protection.required_contexts[]? // empty')"; then
            base=""
        fi
        if [ -z "$base" ]; then
            echo "test-required-context-adr-consistency: WARN — cannot read $base_ref:project.config.json (shallow clone?); skipping (CI runs with fetch-depth 0)." >&2
            return 0
        fi
    fi
    check_added "$root" "$base"
}

run_selftest() {
    local tmp
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    mkdir -p "$tmp/docs/adr" "$tmp/docs/plans/shipped"
    cat > "$tmp/docs/adr/0001-fixture.md" <<'MD'
# ADR-0001 — gate promotion routes

`Phantom gate` was considered and EXPLICITLY REJECTED as a
branch-protection required context — do not add it to
branch_protection.required_contexts (its label hatch cannot reach GitHub).

`Legacy gate` was likewise rejected as a required context.
MD

    # 1. ADDED name with a documented rejection -> FAIL (the asserted failure case).
    printf '{"branch_protection":{"required_contexts":["Old gate","Phantom gate"]}}\n' > "$tmp/project.config.json"
    if RC_ADR_ROOT="$tmp" RC_ADR_BASE_CONTEXTS="Old gate" run_check >/dev/null 2>&1; then
        echo "test-required-context-adr-consistency --selftest: FAIL — rejected addition not caught" >&2
        return 1
    fi

    # 2. ADDED name with no rejection anywhere -> PASS.
    printf '{"branch_protection":{"required_contexts":["Old gate","Clean gate"]}}\n' > "$tmp/project.config.json"
    if ! RC_ADR_ROOT="$tmp" RC_ADR_BASE_CONTEXTS="Old gate" run_check >/dev/null 2>&1; then
        echo "test-required-context-adr-consistency --selftest: FAIL — clean addition was flagged" >&2
        return 1
    fi

    # 3. PRE-EXISTING rejected name (in base too) -> PASS (only additions checked).
    printf '{"branch_protection":{"required_contexts":["Legacy gate"]}}\n' > "$tmp/project.config.json"
    if ! RC_ADR_ROOT="$tmp" RC_ADR_BASE_CONTEXTS="Legacy gate" run_check >/dev/null 2>&1; then
        echo "test-required-context-adr-consistency --selftest: FAIL — pre-existing name was flagged" >&2
        return 1
    fi

    echo "test-required-context-adr-consistency --selftest: PASS — catches an ADR-rejected addition; passes clean/pre-existing names."
    return 0
}

case "${1:-}" in
    --selftest) run_selftest; exit $? ;;
    ""|--check) run_check; exit $? ;;
    *) echo "usage: test-required-context-adr-consistency.sh [--check|--selftest]" >&2; exit 2 ;;
esac
