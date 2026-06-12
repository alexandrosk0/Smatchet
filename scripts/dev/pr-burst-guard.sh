#!/usr/bin/env bash
# pr-burst-guard.sh — advisory guard against opening PRs faster than CodeRabbit's
# hourly per-author review quota (AGENTS.md § Autonomous ship-loop default,
# PR-batching). The class recurred twice (infra P1; postmortems.md 2026-06-06
# 6-PR burst, 2026-06-09 ~13-PR burst), each exhausting CR's quota and forcing
# `cr-out-of-band` on every PR in the burst — losing real review coverage. The
# PR-batching rule was prose-only; this is the cheapest measured backstop named
# in the backlog: count the author's currently-open PRs and WARN before a new one
# would push past the threshold.
#
# ADVISORY by default — exits 0 with a WARN so it never blocks a push or a
# ship-loop. `--strict` makes an over-threshold count exit 1 (for a caller that
# wants a hard stop, e.g. a future ship-loop PR-open guard).
#
# Open-PR count source (first that works):
#   SMATCHET_PR_BURST_COUNT   explicit integer override (testing / offline).
#   gh pr list --author @me --state open --json number --jq length   (live)
# If neither is usable (no gh, offline, non-numeric), it SKIPS with a note and
# exits 0 — a guard that cannot measure must never block the ship-loop.
#
# Threshold: SMATCHET_PR_BURST_THRESHOLD (default 4). CodeRabbit's practical
# per-author hourly review budget is ~4-5; warn at 4 already-open PRs so the
# operator batches the next slice onto an existing branch instead of opening a
# 5th PR the bot can't review in time.
#
# Usage:
#   bash scripts/dev/pr-burst-guard.sh [--strict]
#   bash scripts/dev/pr-burst-guard.sh --selftest
#
# Exit codes:
#   0 — under threshold, advisory over-threshold (no --strict), or skipped.
#   1 — over threshold with --strict, or --selftest failure.
#
# selftest: asserts-failure
set -euo pipefail

THRESHOLD="${SMATCHET_PR_BURST_THRESHOLD:-4}"

# Resolve the author's open-PR count. Echoes the integer on success; returns 2
# when no usable source exists (caller treats that as SKIP, never a block).
_open_pr_count() {
    if [ -n "${SMATCHET_PR_BURST_COUNT:-}" ]; then
        printf '%s\n' "$SMATCHET_PR_BURST_COUNT"
        return 0
    fi
    command -v gh >/dev/null 2>&1 || return 2
    gh pr list --author "@me" --state open --json number --jq 'length' 2>/dev/null || return 2
}

# Pure classifier (no exit / no print of advice) so --selftest can assert it.
# Echoes one of: "OK <n>" | "OVER <n>" | "SKIP -1".
_evaluate() {
    local count
    if ! count="$(_open_pr_count)"; then
        echo "SKIP -1"
        return 0
    fi
    case "$count" in
        '' | *[!0-9]*) echo "SKIP -1"; return 0 ;;
    esac
    if [ "$count" -ge "$THRESHOLD" ]; then
        echo "OVER $count"
    else
        echo "OK $count"
    fi
}

# --selftest — exercises the classifier in all three states and asserts the
# over-threshold + --strict path FAILS (the asserted-failure case the gate-
# selftests meta-gate requires). No network: counts are injected.
if [ "${1:-}" = "--selftest" ]; then
    fail=0
    _expect() { # _expect <want> <label> ; reads $verdict
        if [ "$verdict" = "$1" ]; then
            echo "  ok   [$1] $2"
        else
            echo "  FAIL [want $1, got $verdict] $2"
            fail=1
        fi
    }
    echo "pr-burst-guard --selftest:"

    verdict="$(SMATCHET_PR_BURST_COUNT=1 SMATCHET_PR_BURST_THRESHOLD=4 bash -c '. "$0" --source-only; _evaluate' "$0" 2>/dev/null || true)"
    _expect "OK 1" "under threshold"

    verdict="$(SMATCHET_PR_BURST_COUNT=4 SMATCHET_PR_BURST_THRESHOLD=4 bash -c '. "$0" --source-only; _evaluate' "$0" 2>/dev/null || true)"
    _expect "OVER 4" "at threshold"

    verdict="$(SMATCHET_PR_BURST_COUNT=9 SMATCHET_PR_BURST_THRESHOLD=4 bash -c '. "$0" --source-only; _evaluate' "$0" 2>/dev/null || true)"
    _expect "OVER 9" "over threshold"

    verdict="$(SMATCHET_PR_BURST_COUNT=notanumber bash -c '. "$0" --source-only; _evaluate' "$0" 2>/dev/null || true)"
    _expect "SKIP -1" "non-numeric -> skip"

    # Asserted-failure case: over threshold under --strict must EXIT 1.
    if SMATCHET_PR_BURST_COUNT=9 SMATCHET_PR_BURST_THRESHOLD=4 bash "$0" --strict >/dev/null 2>&1; then
        echo "  FAIL over-threshold --strict did NOT exit 1"
        fail=1
    else
        echo "  ok   over-threshold --strict exits 1"
    fi

    if [ "$fail" -eq 0 ]; then
        echo "pr-burst-guard --selftest: PASS"
        exit 0
    fi
    echo "pr-burst-guard --selftest: FAIL"
    exit 1
fi

# --source-only — define the helpers without running the main body (selftest hook
# so a sub-shell can call _evaluate with injected env). Never used in normal runs.
if [ "${1:-}" = "--source-only" ]; then
    return 0 2>/dev/null || exit 0
fi

strict=0
[ "${1:-}" = "--strict" ] && strict=1

verdict="$(_evaluate)"
state="${verdict%% *}"
count="${verdict##* }"

case "$state" in
    SKIP)
        echo "[pr-burst-guard] SKIP — no usable open-PR count (no gh / offline); not blocking."
        exit 0 ;;
    OK)
        echo "[pr-burst-guard] OK — $count open PR(s) (threshold $THRESHOLD)."
        exit 0 ;;
    OVER)
        echo "[pr-burst-guard] WARN — $count open PR(s) >= threshold $THRESHOLD." >&2
        echo "[pr-burst-guard] CodeRabbit's per-author hourly review budget is ~4-5. Opening another" >&2
        echo "[pr-burst-guard] PR now risks exhausting it -> the whole burst merges with cr-out-of-band" >&2
        echo "[pr-burst-guard] (lost review coverage). Batch the next slice onto an existing branch, or" >&2
        echo "[pr-burst-guard] space the PRs out (AGENTS.md § Autonomous ship-loop default, PR-batching)." >&2
        [ "$strict" -eq 1 ] && exit 1
        exit 0 ;;
esac
