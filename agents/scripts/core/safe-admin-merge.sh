#!/usr/bin/env bash
# agents/scripts/core/safe-admin-merge.sh
# ----------------------------------------------------------------------------
# Safe admin-merge guard — the orchestrator MUST call this instead of a bare
# `gh pr merge <pr> --squash --admin`.
#
# The preventing gate for the 2026-06-13 #1180 escape (postmortems.md): the
# orchestrator admin-merged #1180 while Bucket-C/E (on the merge-gates
# `Bucket-*` block allow-list) were RED, because the pre-merge "0 failures"
# re-confirm was an informational `echo` and `gh pr merge --admin` ran as a
# separate unconditional statement — the printed "FAIL" could not stop the
# merge, and `--admin` bypassed branch protection. The legitimate carve-out
# (admin-merge a STALE-BLOCKED PR only when everything is actually green) was
# asserted in prose, not enforced by an exit code.
#
# This guard makes the green assertion an EXIT CODE, not text:
#   * reads the head's statusCheckRollup (`gh pr view --json statusCheckRollup`)
#   * EXITS NON-ZERO without merging if ANY required-or-allow-listed check is
#     non-green (failing OR still pending) — only a genuinely stale-BLOCKED PR
#     whose every gating check is green is allowed through, and only THEN does
#     it run `gh pr merge --squash --admin`.
#
# Single-source-of-truth: the meant-to-block allow-list regex is NOT duplicated
# here — it is SOURCED from merge-gates.sh ($MERGE_GATES_BLOCK_ALLOWLIST_RE).
# Change the allow-list there and this guard follows automatically.
#
# A check BLOCKS the admin-merge when BOTH:
#   (1) it is gating — REQUIRED (name ∈ branch_protection.required_contexts) OR
#       allow-listed (name matches $MERGE_GATES_BLOCK_ALLOWLIST_RE, non-advisory)
#   (2) it is non-green — a CheckRun that is not COMPLETED, or whose conclusion
#       is not in {SUCCESS, NEUTRAL, SKIPPED}; or a StatusContext whose state is
#       not SUCCESS. (Pending counts as non-green: a stale-BLOCKED-green PR has
#       no pending gating checks left.)
# A non-gating red (e.g. an advisory check, or a non-required non-allow-listed
# one) does NOT block — mirroring the merge-gates $failing contract exactly.
#
# `*-out-of-band` PR labels downgrade the matching check, same as the poller:
#   tests-out-of-band → "Test-delta gate"   ·  perf-out-of-band → "Perf PR-fast*"
# (cr-out-of-band is a CodeRabbit-only override and does not apply to a CI check.)
#
# Usage:
#   agents/scripts/core/safe-admin-merge.sh <pr>
#   agents/scripts/core/safe-admin-merge.sh --selftest
#
# Env knobs:
#   SAFE_ADMIN_MERGE_DRY_RUN     — when "true", print the merge command instead
#                                  of executing it (the gate still runs).
#   SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS — override the required-context set
#                                  (newline/comma-separated). When set (even ""),
#                                  bypasses the project.config.json read. Tests
#                                  inject a fixture-matching set this way.
#   SAFE_ADMIN_MERGE_CONFIG_FILE — override project.config.json path.
#   SAFE_ADMIN_MERGE_STUB_ROLLUP — TEST-ONLY: a JSON blob used in place of the
#                                  `gh pr view` call (the full --json object:
#                                  {statusCheckRollup,state,labels}). Lets
#                                  --selftest + bats feed a synthetic rollup with
#                                  zero `gh` involvement.
#
# Return codes:
#   0 — guard passed; admin-merge performed (or dry-run printed)
#   1 — REFUSED: a gating check is non-green (no merge performed)
#   2 — usage / dependency error (gh or jq missing, bad args)
#   3 — PR not in a mergeable precondition (not OPEN)
#
# selftest: asserts-failure
# ----------------------------------------------------------------------------

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source merge-gates.sh ONLY for the single-source allow-list constant. The
# source is side-effect-free (functions + constants; its CLI entry-point is
# guarded by `[ "${BASH_SOURCE[0]}" = "${0}" ]`, false when sourced).
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$SCRIPT_DIR/merge-gates.sh"

if [ -z "${MERGE_GATES_BLOCK_ALLOWLIST_RE:-}" ]; then
    echo "safe-admin-merge: merge-gates.sh did not export MERGE_GATES_BLOCK_ALLOWLIST_RE — refusing (fail-closed)." >&2
    exit 2
fi

# ----------------------------------------------------------------------------
# read_required_contexts — newline-separated list of branch-protection required
# context names. Override via SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS (even ""),
# else read project.config.json with jq (UTF-8-safe, matches merge-gates.sh).
# ----------------------------------------------------------------------------
read_required_contexts() {
    if [ -n "${SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS+x}" ]; then
        printf '%s\n' "${SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS//,/$'\n'}"
        return 0
    fi
    local config_file="${SAFE_ADMIN_MERGE_CONFIG_FILE:-$SCRIPT_DIR/../../../project.config.json}"
    if [ -f "$config_file" ] && command -v jq >/dev/null 2>&1; then
        jq -r '.branch_protection.required_contexts[]? // empty' "$config_file" 2>/dev/null || true
    fi
}

# ----------------------------------------------------------------------------
# evaluate_rollup <rollup-json> — the PURE core, fully testable with no `gh`.
# Reads a `gh pr view --json statusCheckRollup,state,labels` object on stdin-arg
# and the required-context list on $2 (newline-separated). Emits the list of
# BLOCKING (gating + non-green) check names on stdout, one per line. Exit 0 iff
# zero blockers (green); exit 1 iff one-or-more blockers.
#
# All decision logic lives in jq so a single program decides gating + greenness
# identically to the merge-gates $failing sub-expression.
# ----------------------------------------------------------------------------
evaluate_rollup() {
    local view_json="$1" req_list="$2"
    local req_json='[]'
    if [ -n "$req_list" ]; then
        req_json=$(printf '%s\n' "$req_list" | jq -R . | jq -sc 'map(select(length > 0))') || req_json='[]'
    fi

    printf '%s' "$view_json" | jq -r \
        --argjson req "$req_json" \
        --arg allow "$MERGE_GATES_BLOCK_ALLOWLIST_RE" '
        # Label-driven downgrades (mirror merge-gates $downgraded).
        ([.labels[]?.name] // []) as $labels
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | ($labels | any(. == "perf-out-of-band")) as $perfOob
        | ((.statusCheckRollup) // [])
        # Resolve each rollup row to a (name, green?) pair.
        | map(
            (if .__typename == "CheckRun" then (.name // "")
             else (.context // "") end) as $name
            | (if .__typename == "CheckRun"
               then (.status == "COMPLETED"
                     and ((.conclusion // "") | ascii_upcase | IN("SUCCESS","NEUTRAL","SKIPPED")))
               else ((.state // "") | ascii_upcase | . == "SUCCESS") end) as $green
            | {name: $name, green: $green}
          )
        # A row GATES iff required OR allow-listed-non-advisory. Bind the row to
        # $r so $req-iteration inside `any` does not shadow `.name`.
        | map(. as $r
              | $r + {gating:
                (($req | any(. == $r.name))
                 or (($r.name | test($allow; "i"))
                     and (($r.name | ascii_downcase | contains("advisory")) | not)))})
        # Apply out-of-band downgrades: a labelled check is no longer gating.
        | map(. as $r
              | $r + {gating:
                ($r.gating
                 and (($testsOob and $r.name == "Test-delta gate") | not)
                 and (($perfOob and ($r.name | startswith("Perf PR-fast"))) | not))})
        # A BLOCKER is a gating row that is not green.
        | [.[] | select(.gating and (.green | not)) | .name]
        | .[]
    '
}

run_selftest() {
    local fails=0

    # CASE 1 — REFUSES on a synthetic rollup with a RED allow-listed check.
    # Bucket-C/E are allow-listed via "Bucket-"; one is FAILURE -> must block.
    local red_rollup
    red_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"FAILURE"},
      {"__typename":"CheckRun","name":"Bucket-E (UI smoke)","status":"COMPLETED","conclusion":"SUCCESS"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    local blockers
    blockers=$(evaluate_rollup "$red_rollup" "Windows + MSVC")
    if [ -n "$blockers" ] && printf '%s' "$blockers" | grep -q 'Bucket-C'; then
        echo "selftest CASE1 PASS — refuses on RED allow-listed Bucket-C"
    else
        echo "selftest CASE1 FAIL — should have flagged Bucket-C as a blocker (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 2 — ALLOWS a stale-BLOCKED PR whose every gating check is green.
    # All required + allow-listed checks SUCCESS; an advisory red is ignored.
    local green_rollup
    green_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"SUCCESS"},
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"Duplication scanner (advisory)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    blockers=$(evaluate_rollup "$green_rollup" $'Windows + MSVC\nTest-delta gate')
    if [ -z "$blockers" ]; then
        echo "selftest CASE2 PASS — allows stale-BLOCKED-all-green (advisory red ignored)"
    else
        echo "selftest CASE2 FAIL — should be clean (got blockers: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 3 — a PENDING allow-listed check counts as non-green (blocks).
    local pending_rollup
    pending_rollup='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Sanitizer","status":"IN_PROGRESS","conclusion":null}]}'
    blockers=$(evaluate_rollup "$pending_rollup" "")
    if printf '%s' "$blockers" | grep -q 'Sanitizer'; then
        echo "selftest CASE3 PASS — pending Sanitizer blocks"
    else
        echo "selftest CASE3 FAIL — pending Sanitizer should block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    # CASE 4 — out-of-band label downgrades a RED gated check to non-blocking.
    local oob_rollup
    oob_rollup='{"state":"OPEN","labels":[{"name":"perf-out-of-band"}],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    blockers=$(evaluate_rollup "$oob_rollup" "")
    if [ -z "$blockers" ]; then
        echo "selftest CASE4 PASS — perf-out-of-band downgrades RED Perf PR-fast"
    else
        echo "selftest CASE4 FAIL — perf-out-of-band check should not block (got: '$blockers')" >&2
        fails=$((fails + 1))
    fi

    if [ "$fails" -eq 0 ]; then
        echo "PASS — safe-admin-merge --selftest (4/4)"
        return 0
    fi
    echo "FAIL — safe-admin-merge --selftest ($fails failing case(s))" >&2
    return 1
}

main() {
    local arg="${1:-}"
    case "$arg" in
        --selftest) run_selftest; exit $? ;;
        ""|-h|--help)
            sed -n '2,60p' "${BASH_SOURCE[0]}"
            [ -z "$arg" ] && exit 2 || exit 0 ;;
    esac

    local pr="$arg"
    if ! [[ "$pr" =~ ^[0-9]+$ ]]; then
        echo "safe-admin-merge: <pr> must be a PR number (got: '$pr')" >&2
        exit 2
    fi

    command -v jq >/dev/null 2>&1 || { echo "safe-admin-merge: jq required" >&2; exit 2; }

    local view_json
    if [ -n "${SAFE_ADMIN_MERGE_STUB_ROLLUP:-}" ]; then
        view_json="$SAFE_ADMIN_MERGE_STUB_ROLLUP"
    else
        command -v gh >/dev/null 2>&1 || { echo "safe-admin-merge: gh required" >&2; exit 2; }
        if ! view_json=$(gh pr view "$pr" --json statusCheckRollup,state,labels 2>&1); then
            echo "safe-admin-merge: gh pr view failed: $view_json" >&2
            exit 2
        fi
    fi

    # Precondition — PR must be OPEN.
    local state
    state=$(printf '%s' "$view_json" | jq -r '.state // "UNKNOWN"' 2>/dev/null) || state="UNKNOWN"
    if [ "$state" != "OPEN" ]; then
        echo "safe-admin-merge: PR #$pr is not OPEN (state=$state) — refusing." >&2
        exit 3
    fi

    local req_list blockers
    req_list=$(read_required_contexts)
    blockers=$(evaluate_rollup "$view_json" "$req_list")

    if [ -n "$blockers" ]; then
        echo "REFUSED — PR #$pr has non-green gating check(s); NOT admin-merging:" >&2
        printf '  RED/PENDING: %s\n' "$blockers" >&2
        echo "A bare 'gh pr merge --admin' would have bypassed branch protection and shipped past these. Fix or use the named *-out-of-band override label." >&2
        exit 1
    fi

    echo "GREEN — PR #$pr: every required-or-allow-listed check is green (genuine stale-BLOCKED carve-out)."
    if [ "${SAFE_ADMIN_MERGE_DRY_RUN:-}" = "true" ]; then
        echo "DRY-RUN: would run: gh pr merge $pr --squash --admin"
        exit 0
    fi
    exec gh pr merge "$pr" --squash --admin
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
