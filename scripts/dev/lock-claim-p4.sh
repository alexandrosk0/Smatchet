#!/usr/bin/env bash
# lock-claim-p4.sh — claim a plan-lock atomically via a Perforce counter.
#
# Sibling of `lock-claim.sh` (git-ref backend) for sessions running with
# `SMATCHET_LOCK_BACKEND=p4-counter`. Atomic compare-and-swap is provided
# by `p4 counter --from=0 --to=1 smatchet_lock_<slug>`: server rejects the
# update if the current value isn't 0, so concurrent claims resolve
# deterministically (first CAS wins, others get "Current value is 1").
#
# Phase 4 of `docs/plans/shipped/git-to-perforce-migration.md`.
#
# Usage:
#   bash scripts/dev/lock-claim-p4.sh <slug> <write-set-file>
#
# Arguments:
#   slug             — kebab-case identifier, [a-z0-9][a-z0-9-]{0,63}
#   write-set-file   — same shape as lock-claim.sh (one path per line)
#
# Required environment:
#   P4PORT, P4USER   — Perforce server + identity (see docs/perforce/SETUP.md § 1)
#
# Optional environment (mirror of lock-claim.sh):
#   AGENT_ID         — identity recorded in claim metadata (default: orchestrator)
#   LOCK_BRANCH      — branch the slice ships on (default: current git HEAD)
#   LOCK_PLAN        — originating plan path (default: empty)
#   LOCK_NOTES       — free-form one-line note (default: empty)
#   P4_BIN           — p4 executable (default: `p4`)
#   P4_LOCK_PREFIX   — counter-name prefix (default: smatchet_lock_)
#
# Exit codes:
#   0 — claim succeeded; counter set to 1, metadata stored
#   1 — lock already held; current owner printed to stderr
#   2 — argument / environment error
#   3 — transient p4 failure after retries

set -euo pipefail

usage() {
    echo "usage: bash scripts/dev/lock-claim-p4.sh <slug> <write-set-file>" >&2
    exit 2
}

[ "$#" -eq 2 ] || usage
slug="$1"
write_set_file="$2"

if ! printf '%s' "$slug" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "lock-claim-p4: invalid slug '$slug'" >&2; exit 2
fi
[ -f "$write_set_file" ] || { echo "lock-claim-p4: write-set file not found: $write_set_file" >&2; exit 2; }

: "${P4PORT:?lock-claim-p4: P4PORT not set; see docs/perforce/SETUP.md § 1}"
: "${P4USER:?lock-claim-p4: P4USER not set; see docs/perforce/SETUP.md § 1}"
export P4PORT P4USER

p4="${P4_BIN:-p4}"
lock_prefix="${P4_LOCK_PREFIX:-smatchet_lock_}"
state_counter="${lock_prefix}${slug}"
meta_counter="${lock_prefix}${slug}_meta"

if ! "$p4" info >/dev/null 2>&1; then
    echo "lock-claim-p4: cannot reach p4 server at ${P4PORT}" >&2; exit 3
fi

# --- collect claim metadata (same shape as git-ref backend uses) ----------
agent_id="${AGENT_ID:-orchestrator}"
branch="${LOCK_BRANCH:-$(git symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)}"
plan="${LOCK_PLAN:-}"
notes="${LOCK_NOTES:-}"
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)

PYBIN="${PYBIN:-}"
if [ -z "$PYBIN" ]; then
    for candidate in python python3; do
        if command -v "$candidate" >/dev/null 2>&1 && \
           "$candidate" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
            PYBIN="$candidate"; break
        fi
    done
fi
[ -n "$PYBIN" ] || { echo "lock-claim-p4: python3 required for claim-JSON helper" >&2; exit 2; }
HELPER="$(dirname "$0")/_lock-json.py"
[ -f "$HELPER" ] || { echo "lock-claim-p4: helper not found at $HELPER" >&2; exit 2; }

claim_json=$(
    SLUG="$slug" WS_FILE="$write_set_file" OWNER="$agent_id" \
    BRANCH="$branch" PLAN="$plan" STARTED="$started" NOTES="$notes" \
    "$PYBIN" "$HELPER" build-claim
)

# --- atomic claim via compare-and-swap counter ----------------------------
# p4 counter exits 0 on successful CAS, non-zero if current value != --from.
# stderr distinguishes three failure modes:
#   "Current value is unset"  → counter never created; bootstrap then retry
#   "Current value is N"      → lock held (someone else claimed)
#   anything else             → transient (server hiccup, network)
#
# Bootstrap step (unconditional `p4 counter <name> 0`) is race-safe: if two
# agents both see "unset" and bootstrap, both set the counter to 0; only one
# subsequent CAS 0→1 succeeds. Bootstrap cannot stomp an existing claim
# because the "unset" detection only fires when no counter exists.
attempt=0
max_attempts=3
backoff=1
bootstrapped=0
while :; do
    attempt=$((attempt + 1))
    # H6: capture stdout AND stderr (was: `2>&1 >/dev/null`, captured stderr
    # only). Older p4d versions emit "Current value is N" on stdout, newer
    # ones on stderr — the old single-stream capture missed the stdout case,
    # leaving cas_err empty and routing a legitimate lock-held into the
    # transient retry loop (eventual exit 3 instead of correct exit 1).
    # On the success path `rc=0` short-circuits cas_err inspection, so any
    # success-path stdout mixed in here is harmless.
    cas_err=$("$p4" counter --from=0 --to=1 "$state_counter" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        # Won the CAS — store metadata. Metadata write is best-effort.
        "$p4" counter "$meta_counter" "$claim_json" >/dev/null 2>&1 || \
            echo "lock-claim-p4: WARNING — failed to store metadata in ${meta_counter}; lock still held" >&2
        echo "${state_counter} claimed (owner=${agent_id}, branch=${branch})"
        exit 0
    fi

    if echo "$cas_err" | grep -qE 'Current value is unset'; then
        if [ "$bootstrapped" = 1 ]; then
            # Already bootstrapped once in this run — something else is wrong.
            echo "lock-claim-p4: persistent 'unset' after bootstrap; aborting" >&2
            exit 3
        fi
        "$p4" counter "$state_counter" 0 >/dev/null 2>&1 || true
        bootstrapped=1
        continue   # retry the CAS now that counter exists at 0
    fi

    if echo "$cas_err" | grep -qE 'Current value is'; then
        existing_meta=$("$p4" counter "$meta_counter" 2>/dev/null || true)
        echo "lock-claim-p4: ${state_counter} already held" >&2
        if [ -n "$existing_meta" ] && [ "$existing_meta" != "0" ]; then
            echo "  meta: $existing_meta" >&2
        fi
        exit 1
    fi

    # Otherwise — transient.
    if [ "$attempt" -ge "$max_attempts" ]; then
        echo "lock-claim-p4: p4 counter failed after ${attempt} attempts:" >&2
        printf '%s\n' "$cas_err" >&2
        exit 3
    fi
    sleep "$backoff"
    backoff=$((backoff * 2))
done
