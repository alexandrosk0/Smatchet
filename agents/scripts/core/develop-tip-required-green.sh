#!/usr/bin/env bash
# develop-tip-required-green.sh — SessionStart nudge when the `develop` TIP has a
# RED *required* status check, which under block-on-any-red silently blocks EVERY
# open PR (each PR inherits develop's red onto its own head). The failure is
# otherwise discovered only when the next author opens a PR and trips over it —
# mis-attributing the block to their own change and costing a root-cause dig
# (postmortems.md § 2026-07-10 · PR #1698). This surfaces the red develop tip
# immediately, named to the check + the commit/PR that introduced it.
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
# (status<TAB>conclusion) rows, print each RED required check as `name|why`.
# RED = a required check whose latest run on the tip is COMPLETED and terminal
# non-success (failure/timed_out/cancelled/…). Empty output = all green.
#
# A required check that is simply ABSENT from the tip is NOT red: many required
# contexts are PR-only (Test-delta, Perf PR-fast, Coverage, CR finding gate, …)
# and legitimately never run on a direct develop push — treating absence as red
# false-fires on every session. An in-progress (non-terminal) run is also not
# red (don't nag mid-CI). Detecting a genuinely self-disabled required gate is
# postmortem-owed.sh's absence-present job, which carries the expected-present
# allow-list this script deliberately does not. No I/O — unit-testable.
# ---------------------------------------------------------------------------
devtip_red_required() {
    local required_lines="$1" runs_lines="$2"
    local name status concl found
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        found=""; status=""; concl=""
        while IFS=$'\t' read -r r_name r_status r_concl; do
            [ "$r_name" = "$name" ] || continue
            found=1; status="$r_status"; concl="$r_concl"   # newest last wins
        done <<EOF
$runs_lines
EOF
        [ -n "$found" ] || continue   # absent on develop tip = PR-only/not-run, NOT red
        if [ "$status" = "completed" ] && [ "$concl" != "success" ] && [ "$concl" != "neutral" ] && [ "$concl" != "skipped" ]; then
            printf '%s|latest run terminal %s\n' "$name" "${concl:-<none>}"
        fi
    done <<EOF
$required_lines
EOF
}

# ---------------------------------------------------------------------------
run_selftest() {
    local req runs out
    req="$(printf 'Windows + MSVC\nDoc anchors + agent contract\nCR finding gate\n')"
    # Case 1: a required check RED on the tip -> MUST be reported.
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\nDoc anchors + agent contract\tcompleted\tfailure\nCR finding gate\tcompleted\tsuccess\n')"
    out="$(devtip_red_required "$req" "$runs")"
    if ! printf '%s' "$out" | grep -q '^Doc anchors + agent contract|latest run terminal failure'; then
        echo "develop-tip-required-green --selftest: FAIL — RED required check not reported" >&2; return 1
    fi
    # Case 2: all green -> MUST be silent.
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\nDoc anchors + agent contract\tcompleted\tsuccess\nCR finding gate\tcompleted\tsuccess\n')"
    out="$(devtip_red_required "$req" "$runs")"
    if [ -n "$out" ]; then
        echo "develop-tip-required-green --selftest: FAIL — all-green tip reported a red" >&2; return 1
    fi
    # Case 3: a required check ABSENT from the tip (PR-only check, doesn't run on a
    # develop push) -> MUST be silent (absence is not red — avoids the false-fire).
    runs="$(printf 'Windows + MSVC\tcompleted\tsuccess\n')"
    out="$(devtip_red_required "$req" "$runs")"
    if [ -n "$out" ]; then
        echo "develop-tip-required-green --selftest: FAIL — an absent (PR-only) required check was flagged red" >&2; return 1
    fi
    # Case 4: an in-progress (non-terminal) required check -> NOT red (don't nag mid-run).
    runs="$(printf 'Windows + MSVC\tin_progress\t\nDoc anchors + agent contract\tcompleted\tsuccess\nCR finding gate\tcompleted\tsuccess\n')"
    out="$(devtip_red_required "$req" "$runs")"
    if printf '%s' "$out" | grep -q '^Windows + MSVC|'; then
        echo "develop-tip-required-green --selftest: FAIL — in-progress check flagged as red" >&2; return 1
    fi
    echo "develop-tip-required-green --selftest: PASS — reports terminal-red required checks; silent on green / in-progress / absent(PR-only)."
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
        [ -n "$required_lines" ] || return 0   # no branch protection / no perms -> silent
        # Latest run per check-name on the tip (newest last so the detector's last-wins holds).
        runs_lines="$(gh api "repos/$owner_repo/commits/$tip/check-runs" --paginate \
            --jq '.check_runs[] | [.name, .status, (.conclusion // "")] | @tsv' 2>/dev/null || true)"
        pr_hint="$(gh api "repos/$owner_repo/commits/$tip/pulls" --jq '.[0].number // empty' 2>/dev/null || true)"
    fi

    local reds
    reds="$(devtip_red_required "$required_lines" "$runs_lines")"
    [ -n "$reds" ] || return 0   # all green -> silent

    echo "## === develop tip has a RED required check — blocks EVERY open PR ==="
    echo "The develop tip (${tip:0:12}${pr_hint:+, PR #$pr_hint}) has a RED *required* status check."
    echo "Under block-on-any-red this red is inherited onto every open PR's head — the whole"
    echo "repo is blocked until it's fixed. Root-cause the introducing commit/PR, don't blame"
    echo "the next PR that trips over it:"
    printf '%s\n' "$reds" | while IFS='|' read -r name why; do
        echo "  - RED: $name — $why"
    done
    echo "(A RED here is usually a merge-before-terminal race, #1237-family; see postmortems.md.)"
    return 0
}

case "${1:-}" in
    --selftest) run_selftest; exit $? ;;
    --nudge | "") gather_and_nudge; exit 0 ;;
    *) echo "usage: $0 [--nudge|--selftest]" >&2; exit 2 ;;
esac
