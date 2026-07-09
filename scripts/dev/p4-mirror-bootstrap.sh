#!/usr/bin/env bash
# scripts/dev/p4-mirror-bootstrap.sh
# ----------------------------------------------------------------------------
# Idempotent provisioning for the GitHub -> p4d one-way mirror (Helix4Git
# graph depot). Codifies docs/perforce/MIRROR.md § 1-3: verify-then-create the
# graph depot + repo spec + `gconn` service user/grant, apt-install git/git-lfs
# on the WSL2 connector host, assert reachability, then emit the manual half
# (connector package + gconn.conf + mirrorhooks + optional Deploy Key) as
# guided prompts. Safe to re-run — every mutating step is skipped when the
# resource already exists.
#
# Usage: p4-mirror-bootstrap.sh [--server-only|--host-only]
#   --server-only  p4d-side provisioning only (depot/repo/user/grant)
#   --host-only    WSL2 host packages + guided prompts only (no p4 needed)
#
# Config (env, all overridable):
#   P4PORT            p4d address        (default: Mainbot:1666)
#   P4ADMIN_USER      admin p4 user      (default: alexk)
#   MIRROR_DEPOT      graph depot name   (default: repo)
#   MIRROR_REPO_PATH  repo path, no //   (default: repo/smatchet)
#   GCONN_USER        service p4 user    (default: gconn)
#   GITHUB_REMOTE     upstream clone URL (default: the Smatchet repo, HTTPS)
#
# Pairs with p4-mirror-healthcheck.sh for a provision->verify loop.
# No `set -e` — each failure dies explicitly with a runbook pointer.
# ----------------------------------------------------------------------------
set -uo pipefail

P4PORT="${P4PORT:-Mainbot:1666}"
P4ADMIN_USER="${P4ADMIN_USER:-alexk}"
MIRROR_DEPOT="${MIRROR_DEPOT:-repo}"
MIRROR_REPO_PATH="${MIRROR_REPO_PATH:-repo/smatchet}"
GCONN_USER="${GCONN_USER:-gconn}"
GITHUB_REMOTE="${GITHUB_REMOTE:-https://github.com/alexandrosk0/Smatchet.git}"

MODE="all"
case "${1:-}" in
    --server-only) MODE="server" ;;
    --host-only)   MODE="host" ;;
    "") ;;
    *) echo "usage: p4-mirror-bootstrap.sh [--server-only|--host-only]" >&2; exit 2 ;;
esac

die() { echo "p4-mirror-bootstrap: FAIL — $*" >&2; exit 1; }
ok()  { echo "  OK       $*"; }
did() { echo "  CREATED  $*"; }

p4a() { p4 -p "$P4PORT" -u "$P4ADMIN_USER" "$@"; }

server_side() {
    command -v p4 >/dev/null 2>&1 \
        || die "'p4' not on PATH — install the Helix client (MIRROR.md § Prerequisites), or use --host-only."
    p4a info >/dev/null 2>&1 \
        || die "cannot reach p4d at $P4PORT as $P4ADMIN_USER — server down / not logged in? See MIRROR.md § Prerequisites."
    ok "p4d reachable at $P4PORT"

    if p4a depots -t graph 2>/dev/null | grep -q "^Depot $MIRROR_DEPOT "; then
        ok "graph depot //$MIRROR_DEPOT exists"
    else
        printf 'Depot:\t%s\nOwner:\t%s\nType:\tgraph\nMap:\t%s/...\n' \
            "$MIRROR_DEPOT" "$P4ADMIN_USER" "$MIRROR_DEPOT" | p4a depot -i >/dev/null \
            || die "graph depot create failed (free tier caps: 10 repos — MIRROR.md § Prerequisites)."
        did "graph depot //$MIRROR_DEPOT"
    fi

    if p4a repos 2>/dev/null | grep -q "^Repo //$MIRROR_REPO_PATH "; then
        ok "repo spec //$MIRROR_REPO_PATH exists"
    else
        printf 'Repo:\t//%s\nOwner:\t%s\n' "$MIRROR_REPO_PATH" "$GCONN_USER" | p4a repo -i >/dev/null \
            || die "repo spec create failed for //$MIRROR_REPO_PATH."
        did "repo spec //$MIRROR_REPO_PATH (Owner: $GCONN_USER)"
    fi

    if p4a users -a 2>/dev/null | awk '{print $1}' | grep -qx "$GCONN_USER"; then
        ok "service user $GCONN_USER exists"
    else
        printf 'User:\t%s\nEmail:\t%s@localhost\nFullName:\tGit Connector service\n' \
            "$GCONN_USER" "$GCONN_USER" | p4a user -f -i >/dev/null \
            || die "service user create failed for $GCONN_USER."
        did "service user $GCONN_USER"
    fi

    if p4a show-permission -d "$MIRROR_DEPOT" 2>/dev/null \
            | grep -q "user[[:space:]]*${GCONN_USER}[[:space:]]*admin"; then
        ok "grant: $GCONN_USER admin on //$MIRROR_DEPOT/..."
    else
        p4a grant-permission -d "$MIRROR_DEPOT" -u "$GCONN_USER" -p admin >/dev/null \
            || die "grant-permission failed (graph ACLs use grant-permission, NOT p4 protect — MIRROR.md § 1)."
        did "grant: $GCONN_USER admin on //$MIRROR_DEPOT/..."
    fi
}

host_side() {
    if command -v git >/dev/null 2>&1 && command -v git-lfs >/dev/null 2>&1; then
        ok "git + git-lfs installed"
    elif command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -qq && sudo apt-get install -y -qq git git-lfs \
            || die "apt-get install git git-lfs failed."
        did "apt packages git + git-lfs"
    else
        die "git/git-lfs missing and no apt-get — run on the WSL2 Ubuntu connector host (MIRROR.md § 2)."
    fi
    git lfs install --skip-repo >/dev/null 2>&1 || die "git lfs install failed."
    ok "git-lfs hooks initialised"
}

guided_prompts() {
    cat <<EOF

Manual half (secrets / package installs stay operator-driven by design):
  1. Install the Helix Git Connector .deb matching the p4d 2025.2 line:
       https://www.perforce.com/downloads/helix-core-git-connector  (MIRROR.md § 2)
  2. Write /opt/perforce/git-connector/gconn.conf (owner git:gconn-auth) and log
     the $GCONN_USER ticket into its p4TicketsFile — MIRROR.md § 4 (a).
  3. Register + first fetch, as the connector OS user 'git':
       runuser -u git -- env GCONN_CONFIG=/opt/perforce/git-connector/gconn.conf \\
         gconn --mirrorhooks add ${MIRROR_REPO_PATH} ${GITHUB_REMOTE}
  4. Wire the 15-min cron + WSL2 keep-alive task — MIRROR.md § 5.
  5. Only if the GitHub repo is private: add a read-only Deploy Key — MIRROR.md § 3.
  Verify end-to-end: MIRROR_RESOLVE=p4 P4PORT=$P4PORT P4USER=$GCONN_USER \\
    scripts/dev/p4-mirror-healthcheck.sh
EOF
}

echo "p4-mirror-bootstrap: provisioning ($MODE) — p4d $P4PORT, repo //$MIRROR_REPO_PATH"
[ "$MODE" != "host" ] && server_side
[ "$MODE" != "server" ] && host_side
guided_prompts
echo "p4-mirror-bootstrap: DONE — re-run any time; existing resources are skipped."
exit 0
