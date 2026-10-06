#!/usr/bin/env bash
# agents/scripts/core/safe-merge.sh
# ----------------------------------------------------------------------------
# Safe NON-admin merge wrapper — the single sanctioned entry-point for arming a
# squash-merge on a PR the orchestrator / git-janitor / a session is shipping.
#
# WHY THIS EXISTS (PR-1 § intent-gate-safe-merge-wrapper):
#   A bare `gh pr merge <pr> --squash --auto` is NOT the full gate set. GitHub's
#   auto-merge only waits on the branch-protection REQUIRED contexts; the
#   CodeRabbit, user-comment, and Cursor-Bugbot gates are NOT GitHub-required
#   check contexts (CR stays PR-advisory by design — merge-gates.md § GitHub
#   merge queue). So `--auto` can fire the moment CI is green while a real CR
#   finding / unresolved human comment / open Bugbot thread is still outstanding.
#   It also silently waves through a RED check on the merge-gates *block
#   allow-list* (Coverage / Sanitizer / Perf PR-fast / …) because those are
#   non-required — the exact #923 gate-escape class.
#
#   This wrapper closes that gap: it runs the FULL merge-gates poll first and
#   arms `--auto` ONLY on GATES_PASSED. It is to non-admin merge what
#   safe-admin-merge.sh is to admin merge — the green assertion is an EXIT CODE,
#   never advisory text printed beside an unconditional merge command.
#
#   Sibling contract (mirrors safe-admin-merge.sh):
#     * sources merge-gates.sh for the single-source allow-list + poll_merge_gates
#     * honours the same `*-out-of-band` label semantics (the poll applies them)
#     * REFUSES (exit 1) on any block-allowlist RED check lacking its named
#       `*-out-of-band` override label — by deferring entirely to poll_merge_gates,
#       whose $failing/$downgraded logic IS that refusal
#     * --selftest dogfoods the refuse + arm + obligation paths
#
# Difference from safe-admin-merge.sh: this does NOT pass `--admin` and does NOT
# bypass branch protection. It arms GitHub auto-merge (`--squash --auto`), which
# merges immediately if no queue is configured and enqueues if one is (the
# queue-safe path per merge-gates.md). It is the DEFAULT merge path; admin-merge
# is the narrow stale-BLOCKED carve-out only.
#
# Trust-boundary deferred-test obligation (PR-1 § out-of-band-on-trust-boundary-
# owes-tracked-test): when a load-bearing `tests-out-of-band` / `perf-out-of-band`
# label is what let a RED check pass AND the PR diff touches a strict-zone /
# trust-boundary path, the override is silently buying tech-debt. This wrapper
# AUTO-FILES a tracked deferred-test obligation stub at
# docs/self-improvement/categories/test/<date>-<slug>.md so the debt is visible
# in the backlog instead of evaporating with the label. Filed only on a real
# load-bearing override over a trust-boundary diff (never on a moot label).
#
# Merge-time snapshot (ADR-0017 ledger writer, actor `orchestrator-automerge`):
# after arming, the wrapper polls the PR over REST on a short bounded budget
# (SAFE_MERGE_SNAPSHOT_WAIT_SECONDS). A pass has every check terminal-green
# already, so GitHub normally merges within seconds; on MERGED it appends the
# gate-verdict row itself (redChecks/overrideLabels from the poll's
# GATE_SNAPSHOT line + the labels captured BEFORE the arm — GitHub strips
# override labels post-merge). If the budget expires with auto-merge still
# queued, it prints the paste-ready `merge-snapshot-append.sh` line instead
# (exit 0 — the arm succeeded). Without this the default merge path wrote no
# row at all (tooling/2026-08-19-safe-merge-arms-automerge-and-execs-away-
# before-writing-a-snapshot-row). The row lands uncommitted in the working
# copy; commit it with the next develop-bound commit.
#
# Usage:
#   agents/scripts/core/safe-merge.sh <pr>
#   agents/scripts/core/safe-merge.sh --selftest
#
# Env knobs (this wrapper; merge-gates.sh knobs also apply — ORCH_USER etc.):
#   MERGE_GATES_FLIP_READY      — draft→ready flip before the poll (consumed by
#                                 merge-gates.sh). DEFAULTED TO "true" here when
#                                 unset: invoking safe-merge IS the merge
#                                 authorization (AGENTS.md § Merge gates), so a
#                                 draft PR must not pause an autonomous merge —
#                                 CR's auto_review.drafts:false skips drafts (the
#                                 C4 class → CR gate wedges on NONE) and
#                                 `gh pr merge` refuses drafts outright. An
#                                 explicit caller value (e.g. "false") is
#                                 preserved for poll-only semantics.
#   SAFE_MERGE_DRY_RUN          — "true": print the merge command instead of
#                                 executing it (the gate still runs; no arm, no
#                                 snapshot).
#   SAFE_MERGE_OWNER / _REPO    — owner/repo for poll_merge_gates and the
#                                 post-arm REST poll. Default: read from
#                                 `gh repo view` / gh's `{owner}/{repo}`
#                                 placeholders (the current checkout).
#   SAFE_MERGE_SNAPSHOT_WAIT_SECONDS
#                               — post-arm merge-wait budget before falling back
#                                 to the paste-ready append line (default 120;
#                                 0 = probe once; non-numeric → default).
#   SAFE_MERGE_SNAPSHOT_POLL_SECONDS
#                               — interval between post-arm probes (default 5).
#   MERGE_SNAPSHOT_LEDGER       — ledger path, passed through to
#                                 merge-snapshot-append.sh (tests point it at a
#                                 temp file).
#   SAFE_MERGE_STUB_GATE        — TEST-ONLY: "PASS" / "BLOCK" / "ERROR" — skip the
#                                 real poll_merge_gates call and use this verdict.
#                                 Honoured ONLY together with SAFE_MERGE_DRY_RUN=true
#                                 (nothing can be armed) or SAFE_MERGE_TEST_MODE=1;
#                                 set alone (e.g. leaked from a test shell) the
#                                 wrapper REFUSES with exit 2 — a stubbed PASS
#                                 would otherwise arm a real merge with no gate
#                                 evaluated.
#   SAFE_MERGE_TEST_MODE        — TEST-ONLY: "1" lets a stubbed gate arm through
#                                 whatever `gh` is on PATH (bats sets it beside a
#                                 stub `gh` to exercise the post-arm ledger write).
#   SAFE_MERGE_STUB_GATE_OUT    — TEST-ONLY: stdout the stubbed poll emits (so the
#                                 GATE_SNAPSHOT-bearing PASS path is exercisable).
#   SAFE_MERGE_DIFF_PATHS       — TEST-ONLY (or override): newline/space-separated
#                                 changed-file paths used for the trust-boundary
#                                 obligation check, in place of `gh pr diff --name-only`.
#   SAFE_MERGE_LABELS           — TEST-ONLY (or override): newline/comma-separated
#                                 PR label names, in place of `gh pr view --json labels`.
#   SAFE_MERGE_OBLIGATION_DIR   — override the obligation-stub output dir
#                                 (default docs/self-improvement/categories/test).
#   SAFE_MERGE_OBLIGATION_DATE  — override the stub date (default `date +%F`).
#
# Return codes:
#   0 — gates passed; auto-merge armed (or dry-run printed). The ledger row was
#       written if the merge landed inside the wait budget, else the paste-ready
#       append line was printed — neither outcome changes the exit code
#   1 — REFUSED: merge gates did not pass, or the PR head moved after the
#       gate poll (no merge armed)
#   2 — usage / dependency error (gh or jq missing, bad args, a test-only
#       stub gate set outside test mode)
#   3 — gate-poll precondition error (PR closed/merged, gh API down, the
#       gated head SHA could not be determined, etc.)
#   4 — REFUSED: required-missing-cancelled (gate rc=8) — the required
#       context's pending run was cancelled (concurrency pending-queue
#       collapse, no check-run created); run the printed `gh run rerun <id>`
#       line(s), then re-run safe-merge
#   other — `gh pr merge --squash --auto` itself failed to arm: its exit status
#       is passed through unchanged (the contract the former `exec` gave)
#
# selftest: asserts-failure
# ----------------------------------------------------------------------------

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source merge-gates.sh for poll_merge_gates + the single-source allow-list
# constant. Side-effect-free when sourced (its CLI entry-point is guarded by
# `[ "${BASH_SOURCE[0]}" = "${0}" ]`, false here).
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$SCRIPT_DIR/merge-gates.sh"

if [ -z "${MERGE_GATES_BLOCK_ALLOWLIST_RE:-}" ]; then
    echo "safe-merge: merge-gates.sh did not export MERGE_GATES_BLOCK_ALLOWLIST_RE — refusing (fail-closed)." >&2
    exit 2
fi

# The host tree (the obligation stub lands in its backlog). merge-gates.sh, sourced
# above, loads scripts/dev/project-config.sh, whose PROJECT_ROOT is the
# superproject once this script lives in the agent-layer/ submodule; a climb from
# this script's own path would file the stub in the layer instead.
REPO_ROOT="${PROJECT_ROOT:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"

# ----------------------------------------------------------------------------
# Strict-zone / trust-boundary path matcher — SINGLE SOURCE for the obligation
# trigger. Mirrors AGENTS.md § Enforcement contract-card § Strict zones plus the
# trust-boundary surfaces a security/test obligation actually matters for:
#   Tracker / Sync / Persistence / Config / Commands strict zones, the MCP / Lua
#   plugins, and the CLI / HTTP entry surfaces. A diff touching any of these while
#   a load-bearing tests-/perf-out-of-band label suppressed a RED check owes a
#   tracked deferred-test obligation (PR-1 § out-of-band-on-trust-boundary-owes-
#   tracked-test). Kept deliberately broad on the trust-boundary side: the cost
#   of a spurious obligation stub (a backlog entry) is far below a silent
#   strict-zone test gap.
# ----------------------------------------------------------------------------
SAFE_MERGE_TRUST_BOUNDARY_RE='^(Source/Core/(src|include)/(Tracker|Sync|Persistence|Config|Commands)/|Source/Plugins/(Mcp|LuaConsole)/|Source/Plugins/Lua|Source/Standalone/Cli|Source/Core/src/.*Http|Source/Core/.*Http)'

# ----------------------------------------------------------------------------
# diff_touches_trust_boundary <newline-separated paths> — exit 0 if any path is
# under a strict-zone / trust-boundary surface, else exit 1. Pure + testable.
# ----------------------------------------------------------------------------
diff_touches_trust_boundary() {
    local paths="$1" p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        if printf '%s' "$p" | grep -qE "$SAFE_MERGE_TRUST_BOUNDARY_RE"; then
            return 0
        fi
    done <<<"$paths"
    return 1
}

# ----------------------------------------------------------------------------
# loadbearing_oob_labels <gate_stdout> <labels-newline-list> — emit the names of
# the out-of-band labels that were LOAD-BEARING for this pass, one per line. A
# label is load-bearing iff (a) it is present on the PR AND (b) the gate's
# GATE_SNAPSHOT line shows it actually downgraded a RED check (so a moot,
# pre-applied label that bypassed nothing files NO obligation — mirrors the
# postmortem-owed moot-override filter, merge-gates.md § Override-label hygiene).
#
# The GATE_SNAPSHOT line shape (merge-gates.sh PASS path):
#   GATE_SNAPSHOT cr_override=<0|1> downgraded=<comma-joined CI check names>
# `downgraded` is the SAME $downgraded set the poll computed (tests-/perf-/intent-
# /plan-lock-out-of-band turned FAIL→WARN). We only owe a TEST obligation for the
# tests-out-of-band / perf-out-of-band labels (the two that buy a *test/perf*
# coverage gap); intent / plan-lock / cr overrides are out of scope for this.
# ----------------------------------------------------------------------------
loadbearing_oob_labels() {
    local gate_out="$1" labels="$2"
    local snap downgraded
    snap=$(printf '%s\n' "$gate_out" | grep '^GATE_SNAPSHOT ' | tail -1)
    # Everything after "downgraded=" is the verbatim comma-joined name list.
    downgraded="${snap#*downgraded=}"
    [ "$downgraded" = "$snap" ] && downgraded=""   # no downgraded= field present

    local has_tests=false has_perf=false l
    while IFS= read -r l; do
        case "$l" in
            tests-out-of-band) has_tests=true ;;
            perf-out-of-band)  has_perf=true ;;
        esac
    done <<<"$labels"

    # A label is load-bearing only if the snapshot names a check it downgraded.
    # tests-out-of-band → "Test-delta gate"; perf-out-of-band → "Perf PR-fast*".
    if [ "$has_tests" = true ] && printf '%s' "$downgraded" | grep -q 'Test-delta gate'; then
        echo "tests-out-of-band"
    fi
    if [ "$has_perf" = true ] && printf '%s' "$downgraded" | grep -qi 'Perf PR-fast'; then
        echo "perf-out-of-band"
    fi
}

# ----------------------------------------------------------------------------
# file_obligation_stub <pr> <label> <date> <dir> <trust-paths-csv> — write a
# tracked deferred-test obligation per-entry stub (idempotent per pr+label+date).
# Returns the written path on stdout. Format mirrors a category entry
# (docs/self-improvement/AGENT_SELF_IMPROVEMENT.md § Workflow + the existing
# categories/<cat>/<date>-<slug>.md shape).
# ----------------------------------------------------------------------------
file_obligation_stub() {
    local pr="$1" label="$2" date="$3" dir="$4" trust_paths="$5"
    mkdir -p "$dir" || { echo "safe-merge: cannot create obligation dir $dir" >&2; return 1; }
    local slug="${date}-oob-deferred-test-pr${pr}-${label}"
    local out="$dir/${slug}.md"
    # Idempotent: a re-run for the same pr+label+date does not duplicate.
    if [ -f "$out" ]; then
        printf '%s' "$out"
        return 0
    fi
    {
        printf -- '- %s · safe-merge.sh · [test] · P2 — deferred-test obligation: PR #%s merged with a load-bearing `%s` over a trust-boundary diff\n' \
            "$date" "$pr" "$label"
        printf '  Details: safe-merge.sh armed the squash-merge of PR #%s after the `%s` label\n' "$pr" "$label"
        printf '    downgraded a RED check to WARN, while the diff touched a strict-zone /\n'
        printf '    trust-boundary surface. The override bought a test/perf coverage gap that\n'
        printf '    would otherwise evaporate with the label post-merge; this stub tracks the\n'
        printf '    owed coverage so it stays visible in the backlog (PR-1 §\n'
        printf '    out-of-band-on-trust-boundary-owes-tracked-test).\n'
        printf '    Trust-boundary paths in the diff:\n'
        local p
        while IFS= read -r p; do
            [ -n "$p" ] || continue
            printf '      - %s\n' "$p"
        done <<<"$trust_paths"
        printf '  Concrete next action: add the missing test/perf coverage for the changed\n'
        printf '    trust-boundary surface (route to test-rig / test-author / perf-gatekeeper\n'
        printf '    per the touched zone), then archive this stub to applied.md.\n'
        printf '  Status: open\n'
        printf '  Last-reviewed: %s\n' "$date"
    } > "$out" || { echo "safe-merge: cannot write obligation stub $out" >&2; return 1; }
    printf '%s' "$out"
}

# ----------------------------------------------------------------------------
# pr_label_names <pr> — the PR's label names, one per line: from
# SAFE_MERGE_LABELS (test seam / override; newline- or comma-separated) or
# `gh pr view`. Empty on a gh failure — every caller treats "no labels" as the
# conservative default (no obligation filed; no override recorded).
# ----------------------------------------------------------------------------
pr_label_names() {
    local pr="$1"
    if [ -n "${SAFE_MERGE_LABELS+x}" ]; then
        printf '%s\n' "${SAFE_MERGE_LABELS//,/$'\n'}"
    elif command -v gh >/dev/null 2>&1; then
        gh pr view "$pr" --json labels --jq '.labels[].name' 2>/dev/null || true
    fi
}

# ----------------------------------------------------------------------------
# maybe_file_obligations <pr> <gate_out> — the orchestration: gather labels +
# diff paths, find load-bearing oob labels, and (when the diff crosses a trust
# boundary) file one obligation stub per load-bearing label. Emits the filed
# paths on stdout (one per line). No-op + silent when nothing is owed.
# ----------------------------------------------------------------------------
maybe_file_obligations() {
    local pr="$1" gate_out="$2"

    local labels
    labels=$(pr_label_names "$pr")

    local lb_labels
    lb_labels=$(loadbearing_oob_labels "$gate_out" "$labels")
    [ -n "$lb_labels" ] || return 0   # no load-bearing tests/perf override → nothing owed

    # Diff paths — from override env (tests) or `gh pr diff --name-only`.
    local diff_paths=""
    if [ -n "${SAFE_MERGE_DIFF_PATHS+x}" ]; then
        diff_paths="${SAFE_MERGE_DIFF_PATHS//[[:space:]]/$'\n'}"
    elif command -v gh >/dev/null 2>&1; then
        diff_paths=$(gh pr diff "$pr" --name-only 2>/dev/null) || diff_paths=""
    fi

    diff_touches_trust_boundary "$diff_paths" || return 0   # not a trust-boundary diff → no obligation

    # Collect just the trust-boundary paths for the stub body.
    local trust_paths p
    trust_paths=""
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        if printf '%s' "$p" | grep -qE "$SAFE_MERGE_TRUST_BOUNDARY_RE"; then
            trust_paths+="$p"$'\n'
        fi
    done <<<"$diff_paths"

    local date dir l filed
    date="${SAFE_MERGE_OBLIGATION_DATE:-$(date +%F)}"
    dir="${SAFE_MERGE_OBLIGATION_DIR:-$REPO_ROOT/docs/self-improvement/categories/test}"
    while IFS= read -r l; do
        [ -n "$l" ] || continue
        if filed=$(file_obligation_stub "$pr" "$l" "$date" "$dir" "$trust_paths"); then
            echo "$filed"
        fi
    done <<<"$lb_labels"
}

# ----------------------------------------------------------------------------
# default_flip_ready — safe-merge is BY CONTRACT an authorized-merge caller
# (invoking it IS the per-PR / standing merge authorization — AGENTS.md § Merge
# gates), so the draft→ready flip defaults ON here. A PR opened draft (remote /
# web harnesses open drafts by default) must not pause an autonomous merge:
# without the flip, CodeRabbit's auto_review.drafts:false never reviews the
# draft (the C4 draft-PR bypass class), the CR gate blocks on NONE past the
# grace window, and the standing governance.auto_merge grant wedges — and even
# a passing poll would then fail the arm step (`gh pr merge` refuses drafts).
# An EXPLICIT caller value (including empty) is preserved, so poll-only
# semantics remain reachable via MERGE_GATES_FLIP_READY=false.
# ----------------------------------------------------------------------------
default_flip_ready() {
    if [ -z "${MERGE_GATES_FLIP_READY+x}" ]; then
        export MERGE_GATES_FLIP_READY=true
        echo "safe-merge: MERGE_GATES_FLIP_READY defaulted to true (authorized-merge caller — a draft PR is flipped ready, not left to wedge the poll)."
    fi
}

# ----------------------------------------------------------------------------
# snapshot_red_csv <gate_out> — the ledger `redChecks` csv for this pass: what
# an override label BYPASSED at the decision instant, read from the PASS-path
# `GATE_SNAPSHOT cr_override=<0|1> downgraded=<names>` line — every downgraded
# name, plus the literal "CodeRabbit" when cr_override=1. Same convention as
# merge-watcher.py `_append_merge_snapshot` and safe-admin-merge.sh, so all
# writers agree; empty on a clean pass (a moot label never double-flags in
# postmortem-owed.sh).
# ----------------------------------------------------------------------------
snapshot_red_csv() {
    local gate_out="$1" snap head red
    snap=$(printf '%s\n' "$gate_out" | grep '^GATE_SNAPSHOT ' | tail -1)
    red="${snap#*downgraded=}"
    [ "$red" = "$snap" ] && red=""   # no downgraded= field present
    head="${snap%%downgraded=*}"
    case " $head " in
        *" cr_override=1 "*) red="${red:+$red,}CodeRabbit" ;;
    esac
    printf '%s' "$red"
}

# ----------------------------------------------------------------------------
# snapshot_cr_state <gate_out> — the CodeRabbit verdict of the passing poll,
# i.e. the `CodeRabbit: <verdict> (<n> open)` segment of the LAST `Poll …`
# line (e.g. "COMMENTED (0 actionable)", "NONE+grace-expired"). Recorded as
# the ledger's optional crState so review evidence survives the merge
# (cr-gate-greens-with-no-cr-status-on-head item 3). Empty when no poll line
# carries it (a stubbed gate) — the row then simply omits the field.
# ----------------------------------------------------------------------------
snapshot_cr_state() {
    local line cr
    line=$(printf '%s\n' "$1" | grep '^Poll ' | tail -1)
    case "$line" in
        *" | CodeRabbit: "*" | Bugbot: "*) ;;
        *) return 0 ;;
    esac
    cr="${line#* | CodeRabbit: }"
    cr="${cr%% | Bugbot: *}"
    printf '%s' "${cr% (*open)}"
}

# ----------------------------------------------------------------------------
# snapshot_override_csv <labels-newline-list> — comma-joined subset of the
# labels that are override labels, via safe-admin-merge.sh's
# override_labels_csv (the one projection every ledger writer shares: config
# merge_gates.override_labels plus its complete fallback set). Sourced in a
# SUBSHELL, git-janitor.sh's pattern: its top level can `exit` fail-closed and
# it defines its own main/run_selftest, which must not replace ours. Non-zero
# on a projection failure so the caller skips the write rather than record an
# authoritative-looking "no overrides" row.
# ----------------------------------------------------------------------------
snapshot_override_csv() {
    local labels="$1" view_json
    command -v jq >/dev/null 2>&1 || return 1
    view_json=$(printf '%s\n' "$labels" \
        | jq -Rn '{labels: [inputs | select(length > 0) | {name: .}]}') || return 1
    # Not followed: the subshell deliberately discards its globals (SCRIPT_DIR…).
    # shellcheck source=/dev/null
    ( . "$SCRIPT_DIR/safe-admin-merge.sh" >/dev/null 2>&1
      override_labels_csv "$view_json" ) 2>/dev/null
}

# ----------------------------------------------------------------------------
# print_snapshot_paste <pr> <mergeCommit> <headSha> <red> <override> <mergedAt>
#                      [crState]
# — the ready-to-paste append line for a merge this run could not record
# itself. Known values are pre-filled (shell-quoted); unknown ones stay as
# <placeholders> to fill from `gh pr view <pr> --json mergeCommit,mergedAt`.
# ----------------------------------------------------------------------------
print_snapshot_paste() {
    local pr="$1" mc="${2:-<mergeCommit>}" head="${3:-<headSha>}"
    local red="$4" override="$5" merged_at="${6:-<mergedAt>}" cr_state="${7:-}" cr_env=""
    [ -n "$cr_state" ] && cr_env="SNAPSHOT_CR_STATE=$(_paste_quote "$cr_state") "
    printf '  %sSNAPSHOT_MERGED_AT=%s bash %s %s %s %s GATES_PASSED %s %s orchestrator-automerge\n' \
        "$cr_env" "$merged_at" "$(_paste_quote "$SCRIPT_DIR/merge-snapshot-append.sh")" "$pr" "$mc" "$head" \
        "$(_paste_quote "$red")" "$(_paste_quote "$override")"
}

# _paste_quote <value> — single-quote a value for a paste-ready shell line; a
# `<placeholder>` stays bare so pasting it unfilled fails loudly (a redirect
# error) instead of recording the placeholder text.
_paste_quote() {
    case "$1" in
        "<"*">") printf '%s' "$1" ;;
        *) printf "'%s'" "${1//\'/\'\\\'\'}" ;;
    esac
}

# ----------------------------------------------------------------------------
# await_merge_and_snapshot <pr> <gate_out> <labels> <gated_sha> — after the arm, poll the PR
# over REST (`gh api repos/<owner>/<repo>/pulls/<pr>`) on the bounded budget
# SAFE_MERGE_SNAPSHOT_WAIT_SECONDS. On merged=true, append the ledger row (actor
# orchestrator-automerge, SNAPSHOT_MERGED_AT from the API). On timeout, or any
# condition that blocks the write, print the paste-ready append line. NEVER
# fails the caller — the arm already succeeded, and a missed row degrades to
# git-janitor's 6 h backfill / merge-snapshot-holes.sh / the postmortem-owed
# live fallback, never blindness.
# A merge whose head is NOT <gated_sha> (commits landed after the poll) is not
# recorded as GATES_PASSED: the gates never saw that head.
# ----------------------------------------------------------------------------
await_merge_and_snapshot() {
    local pr="$1" gate_out="$2" labels="$3" gated_sha="${4:-}"
    local budget="${SAFE_MERGE_SNAPSHOT_WAIT_SECONDS:-120}"
    local interval="${SAFE_MERGE_SNAPSHOT_POLL_SECONDS:-5}"
    case "$budget" in ''|*[!0-9]*) budget=120 ;; esac
    case "$interval" in ''|*[!0-9]*|0) interval=5 ;; esac

    local red_csv override_csv override_ok=1 cr_state
    red_csv=$(snapshot_red_csv "$gate_out")
    cr_state=$(snapshot_cr_state "$gate_out")
    override_csv=$(snapshot_override_csv "$labels") || { override_ok=0; override_csv="<override-labels-csv>"; }

    if ! command -v jq >/dev/null 2>&1; then
        echo "safe-merge: WARN — jq not on PATH; cannot read the merge result, ledger row NOT written. Once PR #$pr merges, run:" >&2
        print_snapshot_paste "$pr" "" "" "$red_csv" "$override_csv" "" "$cr_state" >&2
        return 0
    fi

    local api_path
    api_path="$(pr_api_path "$pr")"

    local waited=0 view merged="" state="" mc="" merged_at="" head_sha=""
    while :; do
        # stdout only — folding stderr in would let a gh notice corrupt the JSON.
        if view=$(gh api "$api_path" 2>/dev/null); then
            merged=$(jq -r '.merged // false' <<<"$view" 2>/dev/null) || merged=""
            state=$(jq -r '.state // ""' <<<"$view" 2>/dev/null) || state=""
            head_sha=$(jq -r '.head.sha // ""' <<<"$view" 2>/dev/null) || head_sha=""
            if [ "$merged" = "true" ]; then
                mc=$(jq -r '.merge_commit_sha // ""' <<<"$view" 2>/dev/null) || mc=""
                merged_at=$(jq -r '.merged_at // ""' <<<"$view" 2>/dev/null) || merged_at=""
                break
            fi
            if [ "$state" = "closed" ]; then
                echo "safe-merge: PR #$pr was CLOSED without merging after the arm — no merge, no ledger row owed."
                return 0
            fi
        fi
        [ "$waited" -ge "$budget" ] && break
        sleep "$interval"
        waited=$((waited + interval))
    done

    if [ "$merged" != "true" ]; then
        echo "SNAPSHOT PENDING — PR #$pr: auto-merge is armed but had not merged after ${budget}s; no ledger row written. Once it merges at the gated head ${gated_sha:-<headSha>}, append it (fill <mergeCommit>/<mergedAt> from 'gh pr view $pr --json mergeCommit,mergedAt'):"
        print_snapshot_paste "$pr" "" "${gated_sha:-$head_sha}" "$red_csv" "$override_csv" "" "$cr_state"
        return 0
    fi
    if [ -n "$gated_sha" ] && [ -n "$head_sha" ] && [ "$head_sha" != "$gated_sha" ]; then
        echo "safe-merge: WARN — PR #$pr merged at head ${head_sha}, but the gates passed on ${gated_sha}: commits landed after the poll and were merged unvetted. NO GATES_PASSED ledger row written — treat this merge as a gate escape (postmortem-owed)." >&2
        return 0
    fi
    if [ -z "$mc" ] || [ -z "$head_sha" ] || [ -z "$merged_at" ] || [ "$override_ok" -ne 1 ]; then
        echo "safe-merge: WARN — PR #$pr merged but mergeCommit/headSha/mergedAt or the override-label projection is unavailable; ledger row NOT written. Fill the gaps and run:" >&2
        print_snapshot_paste "$pr" "$mc" "$head_sha" "$red_csv" "$override_csv" "$merged_at" "$cr_state" >&2
        return 0
    fi
    if SNAPSHOT_CR_STATE="$cr_state" SNAPSHOT_MERGED_AT="$merged_at" bash "$SCRIPT_DIR/merge-snapshot-append.sh" \
        "$pr" "$mc" "$head_sha" GATES_PASSED "$red_csv" "$override_csv" orchestrator-automerge; then
        echo "Merge snapshot appended for PR #$pr (actor orchestrator-automerge; redChecks: ${red_csv:-none}; overrides: ${override_csv:-none}; CR: ${cr_state:-n/a}). Commit it with your next develop-bound commit (chore(ledger) if nothing else is in flight)."
    else
        echo "safe-merge: WARN — merge-snapshot-append failed; ledger row NOT written. Retry with:" >&2
        print_snapshot_paste "$pr" "$mc" "$head_sha" "$red_csv" "$override_csv" "$merged_at" "$cr_state" >&2
    fi
    return 0
}

# ----------------------------------------------------------------------------
# stub_gate_permitted — rc 0 when a SAFE_MERGE_STUB_GATE verdict may be used:
# under SAFE_MERGE_DRY_RUN=true (nothing can be armed) or the explicit
# SAFE_MERGE_TEST_MODE=1 switch bats sets. Anywhere else a stub is a leak.
# ----------------------------------------------------------------------------
stub_gate_permitted() {
    [ "${SAFE_MERGE_DRY_RUN:-}" = "true" ] || [ "${SAFE_MERGE_TEST_MODE:-}" = "1" ]
}

# ----------------------------------------------------------------------------
# pr_api_path <pr> — the REST path for PR <pr> (SAFE_MERGE_OWNER/_REPO, else
# gh's {owner}/{repo} placeholders for the current checkout).
# ----------------------------------------------------------------------------
pr_api_path() {
    if [ -n "${SAFE_MERGE_OWNER:-}" ] && [ -n "${SAFE_MERGE_REPO:-}" ]; then
        printf 'repos/%s/%s/pulls/%s' "$SAFE_MERGE_OWNER" "$SAFE_MERGE_REPO" "$1"
    else
        printf 'repos/{owner}/{repo}/pulls/%s' "$1"
    fi
}

# ----------------------------------------------------------------------------
# pr_head_sha <pr> — the PR's current head SHA over REST; empty when it cannot
# be read (no gh/jq, API error). Callers decide what an empty answer means.
# ----------------------------------------------------------------------------
pr_head_sha() {
    local view
    if ! command -v gh >/dev/null 2>&1 || ! command -v jq >/dev/null 2>&1; then return 0; fi
    # stdout only — a gh notice on stderr must not corrupt the JSON.
    view=$(gh api "$(pr_api_path "$1")" 2>/dev/null) || return 0
    jq -r '.head.sha // ""' <<<"$view" 2>/dev/null || true
}

# ----------------------------------------------------------------------------
# gated_head_sha <gate_out> — the head SHA the PASSING poll evaluated: the
# `GATE_HEAD <sha>` line poll_merge_gates prints beside GATE_SNAPSHOT. Empty
# when absent (a stubbed gate or an older poller).
# ----------------------------------------------------------------------------
gated_head_sha() {
    local line
    line=$(printf '%s\n' "$1" | grep '^GATE_HEAD ' | tail -1)
    line="${line#GATE_HEAD }"
    printf '%s' "${line%% *}"
}

# ----------------------------------------------------------------------------
# run_gate <pr> — run poll_merge_gates (or the stub). Writes the poll's stdout to
# the global GATE_OUT and its return code to the global GATE_RC. Globals (not a
# `$(...)` capture) so the rc propagates to the caller — a subshell capture would
# strand GATE_RC inside the subshell. The stub path lets --selftest/bats inject a
# verdict with zero `gh` involvement.
# ----------------------------------------------------------------------------
run_gate() {
    local pr="$1"
    if [ -n "${SAFE_MERGE_STUB_GATE:-}" ]; then
        if ! stub_gate_permitted; then
            echo "safe-merge: SAFE_MERGE_STUB_GATE is set outside test mode — refusing to use a stubbed verdict." >&2
            GATE_OUT=""
            GATE_RC=3
            return 0
        fi
        case "$SAFE_MERGE_STUB_GATE" in
            PASS)  GATE_OUT="${SAFE_MERGE_STUB_GATE_OUT:-GATES_PASSED}"; GATE_RC=0 ;;
            BLOCK) GATE_OUT="${SAFE_MERGE_STUB_GATE_OUT:-Poll 1/1 — blocked}"; GATE_RC=1 ;;
            *)     GATE_OUT="${SAFE_MERGE_STUB_GATE_OUT:-PR_MERGED}"; GATE_RC=4 ;;
        esac
        return 0
    fi
    local owner repo
    owner="${SAFE_MERGE_OWNER:-}"
    repo="${SAFE_MERGE_REPO:-}"
    if [ -z "$owner" ] || [ -z "$repo" ]; then
        local nwo
        nwo=$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null) || nwo=""
        owner="${owner:-${nwo%%/*}}"
        repo="${repo:-${nwo##*/}}"
    fi
    if [ -z "$owner" ] || [ -z "$repo" ]; then
        echo "safe-merge: cannot resolve owner/repo (set SAFE_MERGE_OWNER / SAFE_MERGE_REPO or run inside the repo)." >&2
        GATE_OUT=""
        GATE_RC=3
        return 0
    fi
    GATE_OUT=$(poll_merge_gates "$owner" "$repo" "$pr")
    GATE_RC=$?
}

run_selftest() {
    local fails=0

    # CASE 1 — diff_touches_trust_boundary detects a strict-zone path.
    if diff_touches_trust_boundary $'README.md\nSource/Core/src/Tracker/JiraClient.cpp'; then
        echo "selftest CASE1 PASS — strict-zone Tracker path detected as trust-boundary"
    else
        echo "selftest CASE1 FAIL — Tracker path should be a trust boundary" >&2
        fails=$((fails + 1))
    fi

    # CASE 2 — a docs-only diff is NOT a trust boundary.
    if diff_touches_trust_boundary $'README.md\ndocs/agent-rules/merge-gates.md'; then
        echo "selftest CASE2 FAIL — docs-only diff should NOT be a trust boundary" >&2
        fails=$((fails + 1))
    else
        echo "selftest CASE2 PASS — docs-only diff is not a trust boundary"
    fi

    # CASE 3 — loadbearing_oob_labels: a present label + matching downgrade is load-bearing.
    local snap_out lb
    snap_out=$'Poll 1/1\nGATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate\nGATES_PASSED'
    lb=$(loadbearing_oob_labels "$snap_out" $'tests-out-of-band')
    if [ "$lb" = "tests-out-of-band" ]; then
        echo "selftest CASE3 PASS — load-bearing tests-out-of-band detected"
    else
        echo "selftest CASE3 FAIL — should detect tests-out-of-band (got: '$lb')" >&2
        fails=$((fails + 1))
    fi

    # CASE 4 — a MOOT label (present but downgraded nothing) is NOT load-bearing.
    snap_out=$'Poll 1/1\nGATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    lb=$(loadbearing_oob_labels "$snap_out" $'tests-out-of-band')
    if [ -z "$lb" ]; then
        echo "selftest CASE4 PASS — moot label (downgraded nothing) is not load-bearing"
    else
        echo "selftest CASE4 FAIL — moot label should not be load-bearing (got: '$lb')" >&2
        fails=$((fails + 1))
    fi

    # CASE 5 — perf-out-of-band downgrading a Perf PR-fast check is load-bearing.
    snap_out=$'GATE_SNAPSHOT cr_override=0 downgraded=Perf PR-fast (windows-2022)\nGATES_PASSED'
    lb=$(loadbearing_oob_labels "$snap_out" $'perf-out-of-band')
    if [ "$lb" = "perf-out-of-band" ]; then
        echo "selftest CASE5 PASS — load-bearing perf-out-of-band detected"
    else
        echo "selftest CASE5 FAIL — should detect perf-out-of-band (got: '$lb')" >&2
        fails=$((fails + 1))
    fi

    # CASE 6 — the obligation stub is filed on a trust-boundary override and is
    # idempotent on a second call (same pr+label+date → same file, no dup).
    local tmpdir stub stub2
    tmpdir="$(mktemp -d)"
    stub=$(file_obligation_stub 1400 tests-out-of-band 2026-06-20 "$tmpdir" $'Source/Core/src/Tracker/JiraClient.cpp\n')
    if [ -f "$stub" ] && grep -q 'deferred-test obligation' "$stub" \
       && grep -q 'Source/Core/src/Tracker/JiraClient.cpp' "$stub" \
       && grep -q '\[test\]' "$stub"; then
        echo "selftest CASE6 PASS — obligation stub filed with trust path + [test] tag"
    else
        echo "selftest CASE6 FAIL — obligation stub malformed or missing (path: '$stub')" >&2
        fails=$((fails + 1))
    fi
    stub2=$(file_obligation_stub 1400 tests-out-of-band 2026-06-20 "$tmpdir" $'Source/Core/src/Tracker/JiraClient.cpp\n')
    if [ "$stub" = "$stub2" ] && [ "$(ls "$tmpdir"/*.md | wc -l)" -eq 1 ]; then
        echo "selftest CASE7 PASS — obligation stub is idempotent (no duplicate)"
    else
        echo "selftest CASE7 FAIL — second call duplicated the stub" >&2
        fails=$((fails + 1))
    fi
    rm -rf "$tmpdir"

    # CASE 8 — maybe_file_obligations files NOTHING when the diff is docs-only,
    # even with a load-bearing label (no trust boundary crossed).
    local tmpdir2 out2
    tmpdir2="$(mktemp -d)"
    out2=$(SAFE_MERGE_LABELS="tests-out-of-band" \
           SAFE_MERGE_DIFF_PATHS="docs/x.md README.md" \
           SAFE_MERGE_OBLIGATION_DIR="$tmpdir2" \
           SAFE_MERGE_OBLIGATION_DATE=2026-06-20 \
           maybe_file_obligations 1401 \
             $'GATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate\nGATES_PASSED')
    if [ -z "$out2" ] && [ -z "$(ls -A "$tmpdir2" 2>/dev/null)" ]; then
        echo "selftest CASE8 PASS — no obligation filed for a docs-only diff"
    else
        echo "selftest CASE8 FAIL — should not file on docs-only diff (out: '$out2')" >&2
        fails=$((fails + 1))
    fi
    rm -rf "$tmpdir2"

    # CASE 9 — maybe_file_obligations FILES a stub for a load-bearing label over a
    # trust-boundary diff.
    local tmpdir3 out3
    tmpdir3="$(mktemp -d)"
    out3=$(SAFE_MERGE_LABELS="tests-out-of-band" \
           SAFE_MERGE_DIFF_PATHS="Source/Plugins/Mcp/Server.cpp" \
           SAFE_MERGE_OBLIGATION_DIR="$tmpdir3" \
           SAFE_MERGE_OBLIGATION_DATE=2026-06-20 \
           maybe_file_obligations 1402 \
             $'GATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate\nGATES_PASSED')
    if [ -n "$out3" ] && [ -f "$out3" ] && grep -q 'Source/Plugins/Mcp/Server.cpp' "$out3"; then
        echo "selftest CASE9 PASS — obligation filed for trust-boundary override"
    else
        echo "selftest CASE9 FAIL — should file for Mcp trust-boundary diff (out: '$out3')" >&2
        fails=$((fails + 1))
    fi
    rm -rf "$tmpdir3"

    # CASE 10 — run_gate honours SAFE_MERGE_STUB_GATE=BLOCK (refuse path drives exit 1).
    GATE_OUT=""; GATE_RC=99
    SAFE_MERGE_TEST_MODE=1 SAFE_MERGE_STUB_GATE=BLOCK run_gate 1234
    if [ "$GATE_RC" -eq 1 ]; then
        echo "selftest CASE10 PASS — stub BLOCK verdict yields gate rc=1 (refuse)"
    else
        echo "selftest CASE10 FAIL — BLOCK stub should set GATE_RC=1 (got: $GATE_RC)" >&2
        fails=$((fails + 1))
    fi

    # CASE 11 — run_gate honours SAFE_MERGE_STUB_GATE=PASS.
    GATE_OUT=""; GATE_RC=99
    SAFE_MERGE_TEST_MODE=1 SAFE_MERGE_STUB_GATE=PASS run_gate 1234
    if [ "$GATE_RC" -eq 0 ] && [[ "$GATE_OUT" == *"GATES_PASSED"* ]]; then
        echo "selftest CASE11 PASS — stub PASS verdict yields gate rc=0"
    else
        echo "selftest CASE11 FAIL — PASS stub should set GATE_RC=0 (got: $GATE_RC, out: '$GATE_OUT')" >&2
        fails=$((fails + 1))
    fi

    # CASE 12 — default_flip_ready sets MERGE_GATES_FLIP_READY=true when unset
    # (authorized-merge caller: a draft PR is flipped ready, never a pause).
    if (unset MERGE_GATES_FLIP_READY; default_flip_ready >/dev/null; \
        [ "${MERGE_GATES_FLIP_READY:-}" = "true" ]); then
        echo "selftest CASE12 PASS — flip-ready defaults to true when unset"
    else
        echo "selftest CASE12 FAIL — flip-ready should default to true" >&2
        fails=$((fails + 1))
    fi

    # CASE 13 — an EXPLICIT caller value is preserved (opt-out stays reachable).
    if (export MERGE_GATES_FLIP_READY=false; default_flip_ready >/dev/null; \
        [ "$MERGE_GATES_FLIP_READY" = "false" ]); then
        echo "selftest CASE13 PASS — explicit flip-ready=false is preserved"
    else
        echo "selftest CASE13 FAIL — explicit caller value must not be overridden" >&2
        fails=$((fails + 1))
    fi

    # CASE 14 — snapshot_red_csv records what an override bypassed: every
    # downgraded name plus "CodeRabbit" for a load-bearing cr-out-of-band.
    local red
    red=$(snapshot_red_csv $'Poll 1/1\nGATE_SNAPSHOT cr_override=1 downgraded=Test-delta gate, Perf PR-fast (windows-2022)\nGATES_PASSED')
    if [ "$red" = "Test-delta gate, Perf PR-fast (windows-2022),CodeRabbit" ]; then
        echo "selftest CASE14 PASS — snapshot redChecks = downgraded names + CodeRabbit"
    else
        echo "selftest CASE14 FAIL — wrong snapshot redChecks (got: '$red')" >&2
        fails=$((fails + 1))
    fi

    # CASE 15 — a clean pass (or no GATE_SNAPSHOT line at all) records NO red
    # check, so a moot label never reads as a bypass in postmortem-owed.
    red=$(snapshot_red_csv $'GATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED')
    red+=$(snapshot_red_csv 'GATES_PASSED')
    if [ -z "$red" ]; then
        echo "selftest CASE15 PASS — clean pass records empty redChecks"
    else
        echo "selftest CASE15 FAIL — clean pass must record no redChecks (got: '$red')" >&2
        fails=$((fails + 1))
    fi

    # CASE 16 — snapshot_cr_state reads the CodeRabbit verdict off the LAST poll
    # line (the passing one), and yields nothing when no poll line carries it.
    local cr
    cr=$(snapshot_cr_state $'Poll 1/40 — CI: 3/4 pass (0 fail, 1 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: NONE+pending (poll 1/10) (0 open) | Bugbot: NONE (0 open) | User: 0 | reviewDecision: NONE\nPoll 2/40 — CI: 4/4 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: COMMENTED (0 actionable) (0 open) | Bugbot: CLEAN (0 open) | User: 0 | reviewDecision: NONE\nGATES_PASSED')
    cr+="|$(snapshot_cr_state 'GATES_PASSED')"
    if [ "$cr" = "COMMENTED (0 actionable)|" ]; then
        echo "selftest CASE16 PASS — snapshot crState = the passing poll's CodeRabbit verdict"
    else
        echo "selftest CASE16 FAIL — wrong snapshot crState (got: '$cr')" >&2
        fails=$((fails + 1))
    fi

    # CASE 17 — a stubbed gate outside dry-run / test mode is refused, never a
    # PASS (a leaked SAFE_MERGE_STUB_GATE=PASS must not arm a real merge).
    if (unset SAFE_MERGE_TEST_MODE SAFE_MERGE_DRY_RUN
        GATE_OUT=""; GATE_RC=99
        SAFE_MERGE_STUB_GATE=PASS run_gate 1234 2>/dev/null
        [ "$GATE_RC" -ne 0 ]); then
        echo "selftest CASE17 PASS — a stubbed gate outside test mode is refused"
    else
        echo "selftest CASE17 FAIL — a leaked stub PASS must not be honoured" >&2
        fails=$((fails + 1))
    fi

    # CASE 18 — gated_head_sha reads the poll's GATE_HEAD line (empty if absent).
    local gh_line
    gh_line="$(gated_head_sha $'Poll 1/1\nGATE_HEAD 0123abc\nGATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED')|$(gated_head_sha 'GATES_PASSED')"
    if [ "$gh_line" = "0123abc|" ]; then
        echo "selftest CASE18 PASS — gated head read from GATE_HEAD"
    else
        echo "selftest CASE18 FAIL — wrong gated head (got: '$gh_line')" >&2
        fails=$((fails + 1))
    fi

    if [ "$fails" -eq 0 ]; then
        echo "PASS — safe-merge --selftest (18/18)"
        return 0
    fi
    echo "FAIL — safe-merge --selftest ($fails failing case(s))" >&2
    return 1
}

main() {
    local arg="${1:-}"
    case "$arg" in
        --selftest) run_selftest; exit $? ;;
        ""|-h|--help)
            sed -n '2,121p' "${BASH_SOURCE[0]}"
            [ -z "$arg" ] && exit 2 || exit 0 ;;
    esac

    local pr="$arg"
    if ! [[ "$pr" =~ ^[0-9]+$ ]]; then
        echo "safe-merge: <pr> must be a PR number (got: '$pr')" >&2
        exit 2
    fi

    # A stubbed gate is a TEST-ONLY seam. Leaked into a real shell it would arm
    # `gh pr merge --squash --auto` with no gate evaluated at all, so outside
    # dry-run / explicit test mode it is a hard refusal, never a silent pass.
    if [ -n "${SAFE_MERGE_STUB_GATE:-}" ] && ! stub_gate_permitted; then
        echo "safe-merge: REFUSED — SAFE_MERGE_STUB_GATE=${SAFE_MERGE_STUB_GATE} is a TEST-ONLY seam, set here without SAFE_MERGE_DRY_RUN=true or SAFE_MERGE_TEST_MODE=1. A stubbed verdict never arms a real merge; unset SAFE_MERGE_STUB_GATE." >&2
        exit 2
    fi
    if [ -z "${SAFE_MERGE_STUB_GATE:-}" ]; then
        command -v gh >/dev/null 2>&1 || { echo "safe-merge: gh required" >&2; exit 2; }
    fi

    # 0. Draft never pauses an authorized merge — default the flip-ready knob ON
    #    (explicit caller values, including "false", are preserved).
    default_flip_ready

    # 1. Run the FULL merge-gates poll. This is the gate that a bare
    #    `gh pr merge --auto` skips (CR + user-comment + Bugbot are not
    #    GitHub-required contexts) and the block-allowlist refusal.
    # The head as the poll starts — the fallback binding when the poll output
    # names no gated head (a stubbed gate, an older poller).
    local head_before
    head_before=$(pr_head_sha "$pr")
    GATE_OUT=""
    GATE_RC=0
    run_gate "$pr"
    local gate_out="$GATE_OUT"
    printf '%s\n' "$gate_out"

    case "$GATE_RC" in
        0) : ;;  # GATES_PASSED → arm below
        1)
            echo "REFUSED — PR #$pr: merge gates did NOT pass; NOT arming auto-merge." >&2
            echo "A bare 'gh pr merge --auto' would skip the CodeRabbit / user-comment / Bugbot gates (none are GitHub-required) and could fire past a RED block-allowlist check. Fix the blocker or apply the named *-out-of-band override label." >&2
            exit 1 ;;
        8)
            echo "REFUSED — PR #$pr: required-missing-cancelled (rc=8) — the required context's pending run was CANCELLED by the concurrency-group collapse and left no check-run; waiting cannot clear it. Execute the 'gh run rerun <id>' line(s) printed in the gate output above, then re-run safe-merge. NOT arming auto-merge." >&2
            exit 4 ;;
        *)
            echo "REFUSED — PR #$pr: gate-poll precondition error (rc=$GATE_RC: PR closed/merged, gh API down, pagination overflow, or config error). NOT arming auto-merge." >&2
            exit 3 ;;
    esac

    # 2. Trust-boundary deferred-test obligation — file a tracked stub when a
    #    load-bearing tests-/perf-out-of-band label rode a strict-zone diff.
    local filed
    filed=$(maybe_file_obligations "$pr" "$gate_out")
    if [ -n "$filed" ]; then
        echo "OBLIGATION — filed deferred-test stub(s) for a load-bearing override on a trust-boundary diff:" >&2
        printf '  %s\n' "$filed" >&2
        echo "Commit the stub(s) so the owed coverage stays tracked (PR-1 § out-of-band-on-trust-boundary-owes-tracked-test)." >&2
    fi

    # 3. Bind the arm to the head the gates passed on. The poll names it
    #    (GATE_HEAD); a head that moved since (a commit pushed mid-poll or after
    #    it) was never gated, so refuse instead of arming. --match-head-commit
    #    makes GitHub enforce the same binding at merge time.
    local gated_sha head_now
    gated_sha=$(gated_head_sha "$gate_out")
    [ -n "$gated_sha" ] || gated_sha="$head_before"
    if [ -z "$gated_sha" ]; then
        echo "REFUSED — PR #$pr: could not determine the head SHA the gates passed on (no GATE_HEAD line and the PR head read failed); NOT arming an unbound auto-merge." >&2
        exit 3
    fi
    head_now=$(pr_head_sha "$pr")
    if [ -n "$head_now" ] && [ "$head_now" != "$gated_sha" ]; then
        echo "REFUSED — PR #$pr: the head moved after the gate poll (gated ${gated_sha}, now ${head_now}); the new commits were never gated. Re-run safe-merge. NOT arming auto-merge." >&2
        exit 1
    fi

    # 4. Arm the merge. --squash --auto is the queue-safe non-admin path: merges
    #    now if no queue is set, enqueues if one is. NOT --admin (no branch-
    #    protection bypass).
    echo "GATES_PASSED — PR #$pr: arming squash auto-merge (non-admin) at head ${gated_sha}."
    if [ "${SAFE_MERGE_DRY_RUN:-}" = "true" ]; then
        echo "DRY-RUN: would run: gh pr merge $pr --squash --auto --match-head-commit $gated_sha"
        exit 0
    fi
    command -v gh >/dev/null 2>&1 || { echo "safe-merge: gh required to arm the merge" >&2; exit 2; }
    # Labels are captured BEFORE the arm: GitHub strips override labels
    # post-merge, so a post-merge read would record a bypassed gate as clean.
    local labels
    labels=$(pr_label_names "$pr")
    # Belt-and-braces: never arm on a lingering draft. The poll's flip (step 1,
    # via MERGE_GATES_FLIP_READY) covers the normal path; this covers a flip
    # WARN plus a pass that never probed draft state (e.g. a CR-exempt
    # self-improvement docs-only PR). Mirrors merge-watcher.py's unconditional
    # ensure_pr_ready_for_review before its merge call.
    gh_pr_ready_idempotent "$pr" || \
        echo "WARN: gh_pr_ready_idempotent returned non-zero; arming may fail if PR #$pr is still draft." >&2
    # Not `exec`: the script must outlive the arm to record the merge-time
    # snapshot (step 5). A failed arm keeps the former exec contract — gh's own
    # exit status is the script's.
    local arm_rc=0
    gh pr merge "$pr" --squash --auto --match-head-commit "$gated_sha" || arm_rc=$?
    if [ "$arm_rc" -ne 0 ]; then
        echo "safe-merge: 'gh pr merge $pr --squash --auto' failed (rc=$arm_rc) — auto-merge NOT armed." >&2
        exit "$arm_rc"
    fi

    # 5. Merge-time snapshot — append the ledger row once the merge lands
    #    (bounded wait), else print the paste-ready append line. Never fails.
    await_merge_and_snapshot "$pr" "$gate_out" "$labels" "$gated_sha"
    exit 0
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
