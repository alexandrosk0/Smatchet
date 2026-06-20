#!/usr/bin/env bash
# plan-lock-gate.sh — Layer C decision logic of the plan-lock enforcement
# (the fail-CLOSED server-side hard net). Run directly in CI by
# .github/workflows/plan-lock-gate.yml (computes the changed set from git), or
# SOURCE it and call plan_lock_gate_decide with an injected changed-file list
# for a headless bats run (so the gate's logic is testable without a real PR).
#
# Reds (exit 1) when this PR's changed files overlap a refs/locks/* claim's
# write_set owned by a DIFFERENT branch (github.head_ref != the lock's `branch`).
# Comparison basis is the declared write_set ONLY (never raw other-open-PR
# changed-file overlap — that re-introduces the file-overlap-as-trigger coupling
# and false-trips adjacent/stacked PRs). Non-circular: does NOT require THIS PR
# to have filed a lock. Identity keyed on BRANCH (never owner/AGENT_ID). Honours
# the 14-day staleness cutoff (symmetric with Layer B); an empty/detached lock
# branch is unattributable -> non-blocking. Reuses the ONE shared coverage
# primitive (lock-table-cache.sh), so a path Layer B blocks, this blocks too.
#
# Unbypassable except the `plan-lock-out-of-band` label (registered in
# merge-gates.sh's CI-downgrade path). See plan-lock-enforcement.md items 5-7.

set -uo pipefail

# Decide on a changed-file list read from stdin (one repo-relative path/line).
# return 0 = clean, 1 = collision (one ::error per colliding file). Requires
# lock-table-cache.sh already sourced (ltc_covering_slug) + LTC_PROJ set.
plan_lock_gate_decide() { # $1 = head ref (this PR's branch)
  local head="$1" now cutoff f slug rc hit=0
  if ! command -v ltc_covering_slug >/dev/null 2>&1; then
    echo "::error::plan-lock-gate: lock-table-cache.sh not sourced (ltc_covering_slug missing)."
    return 1
  fi
  now="$(date -u +%s 2>/dev/null || echo 0)"
  cutoff=$(( now - 14 * 24 * 3600 ))
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    slug="$(ltc_covering_slug "$f" other "$head" "$cutoff" 2>/dev/null)"; rc=$?
    case "$rc" in
      0) ;;            # covered -> slug is the colliding lock (handled below)
      1) slug="" ;;    # table available, no covering lock -> clean
      *)               # 2 (undetermined) / unexpected -> FAIL CLOSED.
        # Layer C is the fail-closed hard net: unlike Layers A/B (advisory,
        # fail-open), an unverifiable lock state here must red, not silently
        # pass — a net that can't evaluate its input must fail loud.
        echo "::error file=$f::plan-lock-gate: lock table unavailable/undetermined (rc=$rc) while evaluating '$f'; the fail-closed gate refuses to pass on an unverifiable lock state. Fix the refs/locks fetch, or apply the 'plan-lock-out-of-band' label."
        hit=1
        continue
        ;;
    esac
    if [ -n "$slug" ]; then
      echo "::error file=$f::plan-lock-gate: '$f' overlaps the write set of plan-lock '$slug', held by a different branch. File or extend a lock (agents/scripts/core/lock-claim-update.sh <slug> <write-set-file>) or coordinate. Override: apply the 'plan-lock-out-of-band' label."
      hit=1
    fi
  done
  return "$hit"
}

_plan_lock_gate_main() {
  local base="${PLAN_LOCK_BASE_REF:-develop}" head="${PLAN_LOCK_HEAD_REF:-}" root lib changed
  root="$(git rev-parse --show-toplevel 2>/dev/null || echo "")"
  [ -n "$root" ] || { echo "::error::plan-lock-gate: not inside a git repo."; exit 1; }
  lib="$root/agents/scripts/core/lock-table-cache.sh"
  [ -f "$lib" ] || { echo "::error::plan-lock-gate: $lib missing."; exit 1; }
  # shellcheck source=agents/scripts/core/lock-table-cache.sh
  . "$lib"
  export LTC_PROJ="$root"

  # Fail-CLOSED on an unresolvable base: a hard net that can't compute its
  # changed set must red LOUD, never silently green on an empty set (the
  # dead-net failure class — NOT the perf-pr-fast `|| true` mask). The
  # plan-lock-out-of-band label is the escape if a transient infra blip wedges
  # a legit PR.
  if ! git rev-parse --verify --quiet "origin/${base}^{commit}" >/dev/null 2>&1; then
    echo "::error::plan-lock-gate: origin/${base} does not resolve (shallow checkout or a missing 'git fetch origin ${base}'). Refusing to evaluate an EMPTY changed-set — fix the base fetch, or apply 'plan-lock-out-of-band'."
    exit 1
  fi
  if ! changed="$(git diff --name-only "origin/${base}...HEAD" 2>/dev/null)"; then
    echo "::error::plan-lock-gate: 'git diff origin/${base}...HEAD' failed."
    exit 1
  fi

  if printf '%s\n' "$changed" | plan_lock_gate_decide "$head"; then
    echo "plan-lock-gate: no cross-branch plan-lock collision."
    exit 0
  fi
  exit 1
}

if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
  _plan_lock_gate_main
fi
