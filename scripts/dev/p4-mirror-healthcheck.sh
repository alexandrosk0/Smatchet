#!/usr/bin/env bash
# scripts/dev/p4-mirror-healthcheck.sh
# ----------------------------------------------------------------------------
# Health-check for the GitHub -> p4d one-way mirror (Helix4Git graph depot).
#
# Asserts the mirrored ref's SHA in the graph depot matches GitHub's canonical
# ref SHA. Exit 0 = in sync; non-zero + diagnostic on stderr = drift / staleness
# / unreachable. Designed for a WSL2 cron AND ad-hoc operator use.
#
# Runbook: docs/perforce/MIRROR.md. Topology invariant (one-way, non-authoritative):
# docs/perforce/AGENT_FLOWS.md § Topology + § When NOT to use Perforce.
#
# Config (env, all overridable):
#   GITHUB_REMOTE     canonical GitHub remote (default: the Smatchet repo)
#   MIRROR_REMOTE     connector smart-http base, e.g. http://localhost:1680
#                     (required unless MIRROR_RESOLVE=p4)
#   MIRROR_REPO_PATH  graph repo path without leading // (default: repo/smatchet)
#   MIRROR_REF        branch to compare (default: develop)
#   MIRROR_RESOLVE    git (default, smart-http) | p4 (fallback, p4 graph log)
#
# No `set -e` by design — every failure is handled explicitly so the cron gets a
# clean non-zero + a human-readable reason rather than a bare trap.
# ----------------------------------------------------------------------------
set -uo pipefail

GITHUB_REMOTE="${GITHUB_REMOTE:-https://github.com/alexandrosk0/Smatchet}"
MIRROR_REMOTE="${MIRROR_REMOTE:-}"
MIRROR_REPO_PATH="${MIRROR_REPO_PATH:-repo/smatchet}"
MIRROR_REF="${MIRROR_REF:-develop}"
MIRROR_RESOLVE="${MIRROR_RESOLVE:-git}"

die() { echo "p4-mirror-healthcheck: $*" >&2; exit 1; }

# Resolve a single branch SHA from a git remote via ls-remote.
# Returns non-zero if the remote is unreachable; prints empty if the ref is absent.
resolve_git_sha() {
    local remote="$1" ref="$2" out
    out="$(git ls-remote "$remote" "refs/heads/$ref" 2>/dev/null)" || return 1
    printf '%s\n' "$out" | awk 'NR==1 {print $1}'
}

# Mirror-side SHA: smart-http (primary) or `p4 graph log` (fallback when the
# connector does not serve smart-http).
resolve_mirror_sha() {
    if [ "$MIRROR_RESOLVE" = "p4" ]; then
        local out
        command -v p4 >/dev/null 2>&1 || die "MIRROR_RESOLVE=p4 but 'p4' not on PATH — install the Helix client or use MIRROR_RESOLVE=git. See docs/perforce/MIRROR.md."
        out="$(p4 graph log -n 1 -m1 "//${MIRROR_REPO_PATH}" "$MIRROR_REF" 2>/dev/null)" || return 1
        printf '%s\n' "$out" | awk '/^commit /{print $2; exit}'
    else
        [ -n "$MIRROR_REMOTE" ] || die "MIRROR_REMOTE unset (connector smart-http base URL, e.g. http://localhost:1680). See docs/perforce/MIRROR.md."
        resolve_git_sha "${MIRROR_REMOTE%/}/$MIRROR_REPO_PATH" "$MIRROR_REF"
    fi
}

github_sha="$(resolve_git_sha "$GITHUB_REMOTE" "$MIRROR_REF")" \
    || die "cannot reach GitHub remote: $GITHUB_REMOTE"
[ -n "$github_sha" ] || die "GitHub ref refs/heads/$MIRROR_REF not found at $GITHUB_REMOTE"

mirror_sha="$(resolve_mirror_sha)" \
    || die "cannot reach mirror (resolve=$MIRROR_RESOLVE) for //${MIRROR_REPO_PATH} — connector down? smart-http not served? See docs/perforce/MIRROR.md § Health-check."
[ -n "$mirror_sha" ] || die "mirror ref $MIRROR_REF not found for //${MIRROR_REPO_PATH} (repo not populated yet? run an initial connector fetch — see MIRROR.md § Cadence)."

if [ "$github_sha" = "$mirror_sha" ]; then
    echo "p4-mirror-healthcheck: OK — $MIRROR_REF in sync at ${github_sha:0:12} (github == mirror)"
    exit 0
fi

die "DRIFT — $MIRROR_REF github=${github_sha:0:12} mirror=${mirror_sha:0:12} — mirror stale; trigger a connector fetch (MIRROR.md § Cadence)."
