#!/usr/bin/env bash
# lock-release-p4.sh — release a plan-lock held via the Perforce counter
# backend. Sibling of `lock-release.sh` (git-ref backend).
#
# CAS-release: `p4 counter --from=1 --to=0 smatchet_lock_<slug>`. Server
# rejects the update if the counter isn't 1, so accidental release of a
# lock we don't own is a hard error rather than a silent corruption.
# Idempotent for the "already released" case: counter == 0 returns success
# with a notice (matches the git-ref release semantics).
#
# After successfully zeroing the state counter, the metadata counter
# `smatchet_lock_<slug>_meta` is best-effort deleted so a subsequent claim
# doesn't see a stale owner record while in the "released, not yet
# re-claimed" window.
#
# Phase 4 of `docs/plans/shipped/git-to-perforce-migration.md`.
#
# Usage:
#   bash scripts/dev/lock-release-p4.sh <slug>
#
# Required environment:
#   P4PORT, P4USER
#
# Optional environment:
#   P4_BIN           — p4 executable (default: `p4`)
#   P4_LOCK_PREFIX   — counter-name prefix (default: smatchet_lock_)
#
# Exit codes:
#   0 — counter set to 0 (or already 0); metadata cleared
#   2 — argument / environment error
#   3 — counter is in some other state (not 0 or 1) — manual fix needed

set -euo pipefail

usage() {
    echo "usage: bash scripts/dev/lock-release-p4.sh <slug>" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
slug="$1"

if ! printf '%s' "$slug" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "lock-release-p4: invalid slug '$slug'" >&2; exit 2
fi

: "${P4PORT:?lock-release-p4: P4PORT not set; see docs/perforce/SETUP.md § 1}"
: "${P4USER:?lock-release-p4: P4USER not set; see docs/perforce/SETUP.md § 1}"
export P4PORT P4USER

p4="${P4_BIN:-p4}"
lock_prefix="${P4_LOCK_PREFIX:-smatchet_lock_}"
state_counter="${lock_prefix}${slug}"
meta_counter="${lock_prefix}${slug}_meta"

if ! "$p4" info >/dev/null 2>&1; then
    echo "lock-release-p4: cannot reach p4 server at ${P4PORT}" >&2; exit 3
fi

# Read current state before attempting release — produces a clearer message
# than relying on the CAS error. Distinguish "p4 read failed" (server
# unreachable, auth, etc.) from "counter is 0 or absent": `|| echo 0`
# coerces both into the phantom "already released" path, masking real
# server-side errors and reporting success while the lock remains held.
if ! current=$("$p4" counter "$state_counter" 2>/dev/null); then
    echo "lock-release-p4: failed to read ${state_counter} (p4 command errored)" >&2
    exit 3
fi
# `p4 counter` reports both "value=0" and "counter doesn't exist" as the
# literal "0", so treat them the same. Defensive whitespace strip.
current="${current//[[:space:]]/}"
case "$current" in
    0)
        echo "lock-release-p4: ${state_counter} already 0 (no-op)"
        # Clean up any stale metadata regardless
        "$p4" counter -d "$meta_counter" >/dev/null 2>&1 || true
        exit 0
        ;;
    1)
        : # proceed
        ;;
    *)
        echo "lock-release-p4: ${state_counter} has unexpected value '${current}' (not 0 or 1); manual fix needed" >&2
        exit 3
        ;;
esac

# CAS 1 → 0
cas_err=$("$p4" counter --from=1 --to=0 "$state_counter" 2>&1 >/dev/null) && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
    # Could have raced with another release call — re-check. Same masking
    # concern as above: distinguish read-error from "counter is 0".
    if ! after=$("$p4" counter "$state_counter" 2>/dev/null); then
        echo "lock-release-p4: CAS failed AND post-CAS read also failed; aborting" >&2
        printf '%s\n' "$cas_err" >&2
        exit 3
    fi
    after="${after//[[:space:]]/}"
    if [ "$after" = "0" ]; then
        echo "lock-release-p4: ${state_counter} concurrently released (no-op)"
        "$p4" counter -d "$meta_counter" >/dev/null 2>&1 || true
        exit 0
    fi
    echo "lock-release-p4: CAS 1→0 on ${state_counter} failed:" >&2
    printf '%s\n' "$cas_err" >&2
    exit 3
fi

# Successfully released — clear metadata.
"$p4" counter -d "$meta_counter" >/dev/null 2>&1 || true
echo "${state_counter} released"
