#!/usr/bin/env bash
# check-layer-pin.sh — is the agent-layer/ pin a commit on the layer's develop?
#
# The host pins the agent layer as a submodule gitlink. A layer PR squash-merges,
# so the commits on its branch never reach the layer's develop: a host bump that
# pins a layer branch commit points at history the layer later drops (the branch
# is deleted after the merge) and a fresh clone stops resolving it. The pin must
# be the develop squash, or any commit already on develop.
#
# The check reads the gitlink from the superproject tree, fetches the layer's
# develop inside the mount, and asks git whether the gitlink commit is an ancestor
# of it. Only develop is fetched, with --no-tags, and always first as a plain
# fetch: in a full mount a --depth fetch would make it shallow, and in a shallow
# mount a --depth fetch can move the shallow boundary towards the tip and drop
# history the mount already held. A plain fetch never shortens. Only when the pin
# is not found and the mount is shallow is it deepened by SMATCHET_LAYER_PIN_DEPTH
# commits (--deepen, relative to the existing boundary, so it never shortens
# either) and the ancestry test repeated. That reaches a pin develop has not left
# far behind.
#
# Fail-closed. A pin that is off develop exits 1. Any state where the pin cannot
# be verified (no gitlink at the path, the mount is not checked out, the fetch
# fails, develop does not resolve) exits 2. The CI step (agent-layer-integration
# .yml) runs the script directly, so both non-zero codes fail the step: a pin that
# cannot be verified must not merge. Exit 2 stays distinct so a reader of the log
# can tell "wrong pin" from "broken environment" without re-running it.
#
# Usage:
#   bash scripts/dev/check-layer-pin.sh      # run from anywhere inside the host repo
#
# Env overrides (value-free flags only; everything configurable is an env var):
#   SMATCHET_LAYER_PIN_PATH    submodule path in the host repo (default agent-layer)
#   SMATCHET_LAYER_PIN_BRANCH  layer branch the pin must be on (default develop)
#   SMATCHET_LAYER_PIN_REV     host revision whose gitlink is checked (default HEAD)
#   SMATCHET_LAYER_PIN_DEPTH   commits a shallow mount is deepened by (default 1000)
#
# The layer remote is the mount's `origin`, which actions/checkout and
# `git submodule update --init` both set from .gitmodules. A fork that repointed
# its mount's origin (the fork recipe under "First checkout" in this repo's
# root AGENTS.md) checks against its own layer.
#
# Exit codes:
#   0 — the pin is an ancestor of (or equal to) the layer branch tip
#   1 — the pin is not on the layer branch: re-pin to the squash commit
#   2 — the pin could not be verified (environment error); fails the CI step too

set -euo pipefail

PREFIX="check-layer-pin"
LAYER_PATH="${SMATCHET_LAYER_PIN_PATH:-agent-layer}"
BRANCH="${SMATCHET_LAYER_PIN_BRANCH:-develop}"
REV="${SMATCHET_LAYER_PIN_REV:-HEAD}"
DEPTH="${SMATCHET_LAYER_PIN_DEPTH:-1000}"

# report <level> <message> — one line to stderr, plus a GitHub annotation in Actions.
report() {
    local level="$1" msg="$2"
    if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
        printf '::%s::%s\n' "$level" "$msg"
    fi
    printf '%s: %s\n' "$PREFIX" "$msg" >&2
}

env_error() {
    report error "cannot verify the agent-layer pin: $1"
    exit 2
}

if [ "$#" -gt 0 ]; then
    case "$1" in
        -h | --help)
            sed -n '2,/^$/p' "$0" | sed -e 's/^# \{0,1\}//'
            exit 0
            ;;
    esac
    env_error "unexpected argument '$1' (configure with SMATCHET_LAYER_PIN_* env vars; see --help)"
fi

case "$DEPTH" in
    '' | *[!0-9]* | 0*) env_error "SMATCHET_LAYER_PIN_DEPTH must be a positive integer, got '$DEPTH'" ;;
esac

HOST_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || env_error "not inside a git work tree"
cd "$HOST_ROOT"

# The gitlink, from the tree at REV. ls-tree prints "<mode> <type> <sha>\t<path>".
ENTRY="$(git ls-tree "$REV" -- "$LAYER_PATH" 2>/dev/null)" || env_error "cannot read the tree of '$REV'"
[ -n "$ENTRY" ] || env_error "'$REV' has no '$LAYER_PATH' entry (no agent-layer submodule)"
read -r MODE TYPE PIN _ <<<"$ENTRY"
if [ "$MODE" != "160000" ] || [ "$TYPE" != "commit" ]; then
    env_error "'$LAYER_PATH' at '$REV' is a $TYPE, not a submodule gitlink"
fi

# The mount must be a git repository of its own, not the host repo seen through
# a plain directory (git -C would otherwise fall through to the superproject).
MOUNT="$HOST_ROOT/$LAYER_PATH"
MOUNT_TOP="$(git -C "$MOUNT" rev-parse --show-toplevel 2>/dev/null)" ||
    env_error "'$LAYER_PATH' is not checked out (git submodule update --init $LAYER_PATH)"
if [ "$(cd "$MOUNT_TOP" && pwd -P)" != "$(cd "$MOUNT" && pwd -P)" ]; then
    env_error "'$LAYER_PATH' is not checked out (git submodule update --init $LAYER_PATH)"
fi
git -C "$MOUNT" remote get-url origin >/dev/null 2>&1 || env_error "'$LAYER_PATH' has no 'origin' remote"

TRACKING="refs/remotes/origin/$BRANCH"
SHALLOW="$(git -C "$MOUNT" rev-parse --is-shallow-repository 2>/dev/null)" || SHALLOW="false"

# fetch_branch [extra fetch args...] — fetch BRANCH into TRACKING; exit 2 on failure.
fetch_branch() {
    local err
    if ! err="$(git -C "$MOUNT" fetch --quiet --no-tags --no-recurse-submodules "$@" \
        origin "+refs/heads/$BRANCH:$TRACKING" 2>&1)"; then
        env_error "fetching '$BRANCH' from the layer's origin failed: ${err:-no output}"
    fi
    TIP="$(git -C "$MOUNT" rev-parse --verify --quiet "$TRACKING^{commit}")" ||
        env_error "the layer's '$BRANCH' does not resolve after the fetch"
}

# pin_on_branch — 0 when PIN is an ancestor of (or equal to) TIP, 1 when it is not
# in the fetched history; any other git failure exits 2. A pin absent from the
# mount after the fetch is not on the branch: fetching the branch brings every
# ancestor of its tip down to the mount's shallow boundary, if it has one.
pin_on_branch() {
    local rc=0
    git -C "$MOUNT" cat-file -e "$PIN^{commit}" 2>/dev/null || return 1
    git -C "$MOUNT" merge-base --is-ancestor "$PIN" "$TIP" 2>/dev/null || rc=$?
    case "$rc" in
        0 | 1) return "$rc" ;;
        *) env_error "git merge-base --is-ancestor ${PIN:0:12} ${TIP:0:12} failed (rc $rc)" ;;
    esac
}

report_ok() {
    printf '%s: OK — %s pins %s, on the layer %s (tip %s)\n' \
        "$PREFIX" "$LAYER_PATH" "${PIN:0:12}" "$BRANCH" "${TIP:0:12}"
    exit 0
}

TIP=""
fetch_branch
pin_on_branch && report_ok
if [ "$SHALLOW" = "true" ]; then
    fetch_branch "--deepen=$DEPTH"
    pin_on_branch && report_ok
fi

SHORT_PIN="${PIN:0:12}"
SHORT_TIP="${TIP:0:12}"
MSG="$LAYER_PATH pins $SHORT_PIN, which is not on the layer $BRANCH (tip $SHORT_TIP). A layer branch commit"
MSG="$MSG never reaches $BRANCH (layer PRs squash-merge): re-pin $LAYER_PATH to the layer $BRANCH squash"
MSG="$MSG commit after the layer PR merges."
if [ "$SHALLOW" = "true" ]; then
    MSG="$MSG The mount is shallow and was deepened by only $DEPTH commits of $BRANCH;"
    MSG="$MSG raise SMATCHET_LAYER_PIN_DEPTH if the pin is older."
fi
report error "$MSG"
exit 1
