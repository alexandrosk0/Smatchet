#!/usr/bin/env bash
# develop-tip-required-green.sh — SessionStart nudge when the `develop` TIP has a
# check that is NOT green. Two failure classes, both surfaced:
#
#   REQUIRED red — under block-on-any-red it silently blocks EVERY open PR (each
#   PR inherits develop's red onto its own head); otherwise discovered only when
#   the next author opens a PR and trips over it, mis-attributed to their change
#   (postmortems.md § 2026-07-10 · PR #1698).
#
#   NON-REQUIRED red — post-merge backstop jobs (installer smokes, LTO publish
#   builds) are the ONLY coverage for what PR checks structurally cannot run; a
#   red there blocks nothing, which is exactly how it hides. #1957's installer
#   smokes sat red on develop across three merges (second occurrence of the
#   class — infra/2026-08-05 entry), so the sweep covers required and
#   non-required alike, matching the merge-gate block-on-any-red philosophy.
#   Checks whose NAME marks them advisory are excluded, same as merge-gates.sh.
#
#   CANCELLED is reported as its own why-wording ("result unknown"), not lumped
#   with terminal failures: a develop run cancelled by a superseding push is
#   precisely how #1957's red never announced itself — unknown is NOT green.
#
# (File name kept from the required-only v1 — the SessionStart hook in
# docs/harness/claude-code/settings.json.tmpl references it by path.)
#
# Modes:
#   --nudge (default)  SILENT unless a required check on the develop tip is RED;
#                      then print a short attributable block. Never blocks (exit 0).
#   --selftest         fixture-driven test of the pure RED-detector; exit 0/1.
#
# Data layer is injectable for the selftest (no live gh needed):
#   SMATCHET_DEVTIP_REQUIRED_FIXTURE   — file: one required check-name per line.
#   SMATCHET_DEVTIP_CHECKRUNS_FIXTURE  — file: `<name>\t<status>\t<conclusion>` rows
#                                        (latest run per name; newest last wins).
#   SMATCHET_DEVTIP_SHA / _PRHINT      — override the reported tip sha / PR hint.
#
# gh / network absent → degrades to SILENT exit 0 (never wedges SessionStart).
#
# selftest: asserts-failure

set -uo pipefail

# ---------------------------------------------------------------------------
# Pure detector: given the required-check names and the tip's latest per-check
# (status<TAB>conclusion) rows, print each NOT-GREEN check on the tip as
# `name|kind|why` (kind: required|non-required). Empty output = all green.
#
# NOT-GREEN = the check's latest run on the tip is COMPLETED with a conclusion
# other than success/neutral/skipped. CANCELLED gets its own why-wording —
# "result unknown", not a terminal verdict — because a run cancelled by a
# superseding push is how a real failure stays unannounced (#1957). A later
# re-run of the same check supersedes an earlier cancelled row (newest last
# wins), so a green re-run silences the earlier cancel.
#
# The sweep iterates the tip's ACTUAL check runs — required and non-required
# alike (block-on-any-red philosophy; non-required post-merge backstops are the
# only coverage for what PR checks cannot run). Checks whose name contains
# "advisory" (any case) are excluded, mirroring the merge-gate allow-list.
#
# A required check that is simply ABSENT from the tip is (still) NOT red: many
# required contexts are PR-only (Test-delta, Perf PR-fast, Coverage, CR finding
# gate, …) and legitimately never run on a direct develop push — treating
# absence as red false-fires on every session. An in-progress (non-terminal)
# run is also not red (don't nag mid-CI). Detecting a genuinely self-disabled
# required gate is postmortem-owed.sh's absence-present job, which carries the
# expected-present allow-list this script deliberately does not. No I/O —
# unit-testable.
# ---------------------------------------------------------------------------
devtip_not_green() {
    local required_lines="$1" runs_lines="$2"
    local names name status concl kind why r_name r_status r_concl
    # Distinct check names present on the tip, first-seen order.
    names="$(printf '%s\n' "$runs_lines" | awk -F'\t' 'NF && !seen[$1]++ {print $1}')"
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$name" in *[Aa][Dd][Vv][Ii][Ss][Oo][Rr][Yy]*) continue ;; esac
        status=""; concl=""
        while IFS=$'\t' read -r r_name r_status r_concl; do
            [ "$r_name" = "$name" ] || continue
            status="$r_status"; concl="$r_concl"   # newest last wins
        done <<EOF
$runs_lines
EOF
        [ "$status" = "completed" ] || continue    # in-progress: don't nag mid-CI
        case "$concl" in success | neutral | skipped) continue ;; esac
        kind="non-required"
        if printf '%s\n' "$required_lines" | grep -Fxq -- "$name"; then
            kind="required"
        fi
        if [ "$concl" = "cancelled" ]; then
            why="latest run CANCELLED — result unknown (superseded/killed), not green"
        else
            why="latest run terminal ${concl:-<none>}"
        fi
        printf '%s|%s|%s\n' "$name" "$kind" "$why"
    done <<EOF
$names
EOF
}

# ---------------------------------------------------------------------------
run_selftest() {
    local req runs out
    req="$(printf 'Windows + MSVC\nDoc anchors + agent contract\nCR finding gate\n')"
    # Case 1: a required check RED on the tip -> reported, kind=required.
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\nDoc anchors + agent contract\tcompleted\tfailure\nCR finding gate\tcompleted\tsuccess\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if ! printf '%s' "$out" | grep -q '^Doc anchors + agent contract|required|latest run terminal failure'; then
        echo "develop-tip-required-green --selftest: FAIL — RED required check not reported as required" >&2; return 1
    fi
    # Case 2: all green -> MUST be silent.
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\nDoc anchors + agent contract\tcompleted\tsuccess\nCR finding gate\tcompleted\tsuccess\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if [ -n "$out" ]; then
        echo "develop-tip-required-green --selftest: FAIL — all-green tip reported a red" >&2; return 1
    fi
    # Case 3: a required check ABSENT from the tip (PR-only check, doesn't run on a
    # develop push) -> MUST be silent (absence is not red — avoids the false-fire).
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if [ -n "$out" ]; then
        echo "develop-tip-required-green --selftest: FAIL — an absent (PR-only) required check was flagged red" >&2; return 1
    fi
    # Case 4: an in-progress (non-terminal) check -> NOT red (don't nag mid-run).
    runs="$(printf 'Windows + MSVC\tin_progress\t\nDoc anchors + agent contract\tcompleted\tsuccess\nCR finding gate\tcompleted\tsuccess\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if printf '%s' "$out" | grep -q '^Windows + MSVC|'; then
        echo "develop-tip-required-green --selftest: FAIL — in-progress check flagged as red" >&2; return 1
    fi
    # Case 5 (#1957 class): a NON-required backstop RED on the tip -> reported,
    # kind=non-required. The required-only v1 was blind to exactly this.
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\nWindows x64 installer smoke\tcompleted\tfailure\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if ! printf '%s' "$out" | grep -q '^Windows x64 installer smoke|non-required|latest run terminal failure'; then
        echo "develop-tip-required-green --selftest: FAIL — red non-required backstop not reported" >&2; return 1
    fi
    # Case 6 (#1957 hiding shape): a CANCELLED develop run -> reported as
    # unknown-not-green, with the distinct why-wording.
    runs="$(printf 'Windows x64 installer smoke\tcompleted\tcancelled\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if ! printf '%s' "$out" | grep -q '^Windows x64 installer smoke|non-required|latest run CANCELLED — result unknown'; then
        echo "develop-tip-required-green --selftest: FAIL — cancelled run not reported as unknown-not-green" >&2; return 1
    fi
    # Case 7: an advisory-named check red on the tip -> silent (merge-gate parity).
    runs="$(printf 'Mobile texture-guard smoke (Mesa headless GL, advisory)\tcompleted\tfailure\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if [ -n "$out" ]; then
        echo "develop-tip-required-green --selftest: FAIL — advisory check was flagged" >&2; return 1
    fi
    # Case 8: a cancelled run superseded by a later green re-run of the same
    # check (newest last) -> silent; the re-run resolves the unknown.
    runs="$(printf 'Windows x64 installer smoke\tcompleted\tcancelled\nWindows x64 installer smoke\tcompleted\tsuccess\n')"
    out="$(devtip_not_green "$req" "$runs")"
    if [ -n "$out" ]; then
        echo "develop-tip-required-green --selftest: FAIL — superseded cancel not silenced by the green re-run" >&2; return 1
    fi
    echo "develop-tip-required-green --selftest: PASS — reports required AND non-required not-green (cancelled = unknown); silent on green / in-progress / absent(PR-only) / advisory / superseded cancel."
    return 0
}

# ---------------------------------------------------------------------------
gather_and_nudge() {
    local owner_repo tip required_lines runs_lines pr_hint

    if [ -n "${SMATCHET_DEVTIP_REQUIRED_FIXTURE:-}" ]; then
        required_lines="$(cat "$SMATCHET_DEVTIP_REQUIRED_FIXTURE" 2>/dev/null || true)"
        runs_lines="$(cat "${SMATCHET_DEVTIP_CHECKRUNS_FIXTURE:-/dev/null}" 2>/dev/null || true)"
        tip="${SMATCHET_DEVTIP_SHA:-<fixture>}"
        pr_hint="${SMATCHET_DEVTIP_PRHINT:-}"
    else
        command -v gh >/dev/null 2>&1 || return 0   # no gh -> silent (never wedge)
        owner_repo="$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" || return 0
        [ -n "$owner_repo" ] || return 0
        tip="$(gh api "repos/$owner_repo/commits/develop" --jq .sha 2>/dev/null)" || return 0
        [ -n "$tip" ] || return 0
        required_lines="$(gh api "repos/$owner_repo/branches/develop/protection/required_status_checks" \
            --jq '.contexts[]?' 2>/dev/null || true)"
        # No branch protection / no perms -> silent, INCLUDING the non-required
        # sweep: without the required set every red would be labeled
        # non-required, and a mislabeled required red ("doesn't block PRs")
        # is worse than one quiet session on a misconfigured checkout.
        [ -n "$required_lines" ] || return 0
        # Latest run per check-name on the tip. --paginate does NOT guarantee chronological row
        # order (a re-run can arrive newest-first), so sort ascending by started_at/completed_at to
        # make "newest last" actually hold before the detector's last-wins keys off it — otherwise a
        # stale run could shadow a terminal-red or emit a stale red (#1752).
        runs_lines="$(gh api "repos/$owner_repo/commits/$tip/check-runs" --paginate \
            --jq '[.check_runs[]] | sort_by(.started_at // .completed_at // "") | .[] | [.name, .status, (.conclusion // "")] | @tsv' 2>/dev/null || true)"
        pr_hint="$(gh api "repos/$owner_repo/commits/$tip/pulls" --jq '.[0].number // empty' 2>/dev/null || true)"
    fi

    local reds
    reds="$(devtip_not_green "$required_lines" "$runs_lines")"
    [ -n "$reds" ] || return 0   # all green -> silent

    echo "## === develop tip is NOT GREEN — root-cause the introducing commit/PR ==="
    echo "The develop tip (${tip:0:12}${pr_hint:+, PR #$pr_hint}) has check(s) that are not green."
    echo "A RED *required* check is inherited onto every open PR's head under block-on-any-red —"
    echo "the whole repo is blocked until it's fixed. A red *non-required* backstop blocks"
    echo "nothing, which is exactly how it hides (#1957: red across three merges) — it is the"
    echo "only coverage for what PR checks structurally cannot run, so fix it, don't ignore it:"
    printf '%s\n' "$reds" | while IFS='|' read -r name kind why; do
        echo "  - RED [$kind]: $name — $why"
    done
    echo "(A required RED is usually a merge-before-terminal race, #1237-family; a CANCELLED"
    echo " develop run was superseded/killed and proves nothing — re-run it to learn the truth.)"
    return 0
}

case "${1:-}" in
    --selftest) run_selftest; exit $? ;;
    --nudge | "") gather_and_nudge; exit 0 ;;
    *) echo "usage: $0 [--nudge|--selftest]" >&2; exit 2 ;;
esac
