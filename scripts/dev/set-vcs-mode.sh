#!/usr/bin/env bash
# scripts/dev/set-vcs-mode.sh — set the Smatchet VCS backend mode (git | p4) on this machine.
#
# Smatchet's VCS layer is selected by two env vars (AGENTS.md § Dual-VCS topology):
#   SMATCHET_AGENT_VCS     git | p4            — ship-loop variant (git ship-line vs P4-gated)
#   SMATCHET_LOCK_BACKEND  git-ref | p4-counter — plan-lock backend the lock-*.sh scripts dispatch to
#
# Both vars must agree, and on Windows BOTH layers must agree: PowerShell inherits
# the Windows User-registry env, while git-bash sources ~/.bashrc which can
# override it. A silent divergence (registry=git, .bashrc=p4) routes lock-claim.sh
# to the p4 path and fails "P4USER not set". This script sets BOTH layers
# idempotently so every shell on the machine resolves the same mode.
#
# Usage:
#   bash scripts/dev/set-vcs-mode.sh git    # git ship-line (default mode)
#   bash scripts/dev/set-vcs-mode.sh p4     # opt into the Perforce local layer
#   bash scripts/dev/set-vcs-mode.sh        # print the current mode and exit
#
# What it does:
#   1. Rewrites a marked block in the shell rc file (default ~/.bashrc) between
#      `# >>> smatchet vcs-mode >>>` and `# <<< smatchet vcs-mode <<<`, exporting
#      both vars. Re-running replaces the block in place (idempotent); legacy
#      unmarked `export SMATCHET_AGENT_VCS=` / `export SMATCHET_LOCK_BACKEND=`
#      lines are stripped first so they can't shadow the managed block.
#   2. On Windows (MSYS / Git-bash), also writes the two vars to the Windows User
#      registry via setx so PowerShell + new GUI processes inherit the same mode.
#
# Re-source the rc file (or open a new shell) for the change to take effect —
# already-running processes keep their old env.
#
# Test hooks (used by tests/bats/set_vcs_mode.bats — not for normal use):
#   SMATCHET_VCS_RC=<path>        override the rc file the managed block is written to
#   SMATCHET_VCS_SKIP_REGISTRY=1  skip the Windows registry sync (setx)
#
# Exit codes:
#   0 — mode set (or current mode printed)
#   2 — bad / unknown argument

set -euo pipefail

readonly BLOCK_BEGIN="# >>> smatchet vcs-mode >>>"
readonly BLOCK_END="# <<< smatchet vcs-mode <<<"

is_windows() {
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW* | MSYS* | CYGWIN*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rewrite the managed block in the rc file, stripping any prior managed block +
# legacy unmarked exports first. Idempotent: a second call with the same mode
# yields a byte-identical file.
write_rc_block() {
    local rc="$1" vcs="$2" backend="$3"
    [ -f "$rc" ] || : >"$rc"

    local tmp
    tmp="$(mktemp)"
    # Drop: the prior managed block (between the markers, inclusive), the legacy
    # `# Smatchet …VCS…` comment lines this script used to emit, and any unmarked
    # SMATCHET_{AGENT_VCS,LOCK_BACKEND} export so it can't shadow the new block.
    awk '
        $0 == "'"$BLOCK_BEGIN"'" { skip = 1; next }
        $0 == "'"$BLOCK_END"'"   { skip = 0; next }
        skip { next }
        /^# Smatchet (dual-VCS opt-in|VCS mode)/ { next }
        /^[[:space:]]*export[[:space:]]+SMATCHET_AGENT_VCS=/ { next }
        /^[[:space:]]*export[[:space:]]+SMATCHET_LOCK_BACKEND=/ { next }
        { print }
    ' "$rc" >"$tmp"

    {
        printf '%s\n' "$BLOCK_BEGIN"
        printf '# Managed by scripts/dev/set-vcs-mode.{sh,ps1} -- do not edit by hand.\n'
        printf 'export SMATCHET_AGENT_VCS=%s\n' "$vcs"
        printf 'export SMATCHET_LOCK_BACKEND=%s\n' "$backend"
        printf '%s\n' "$BLOCK_END"
    } >>"$tmp"

    mv "$tmp" "$rc"
}

# On Windows, mirror the mode into the User-registry env so PowerShell agrees
# with git-bash. setx writes HKCU\Environment; it affects NEW processes only.
sync_windows_registry() {
    local vcs="$1" backend="$2"
    if [ "${SMATCHET_VCS_SKIP_REGISTRY:-}" = "1" ]; then return 0; fi
    if ! is_windows; then return 0; fi
    if ! command -v setx >/dev/null 2>&1; then
        echo "set-vcs-mode: setx not found — Windows User-registry env NOT updated;" >&2
        echo "  PowerShell shells may disagree with git-bash until you set it manually." >&2
        return 0
    fi
    setx SMATCHET_AGENT_VCS "$vcs" >/dev/null
    setx SMATCHET_LOCK_BACKEND "$backend" >/dev/null
}

main() {
    local mode="${1:-}"
    local vcs backend

    case "$mode" in
        git) vcs="git"; backend="git-ref" ;;
        p4) vcs="p4"; backend="p4-counter" ;;
        "")
            echo "SMATCHET_AGENT_VCS=${SMATCHET_AGENT_VCS:-<unset> (default git)}"
            echo "SMATCHET_LOCK_BACKEND=${SMATCHET_LOCK_BACKEND:-<unset> (default git-ref)}"
            exit 0
            ;;
        -h | --help)
            echo "Usage: bash scripts/dev/set-vcs-mode.sh <git|p4>   (no arg prints current mode)"
            exit 0
            ;;
        *)
            echo "set-vcs-mode: unknown mode '$mode' — use 'git' or 'p4'." >&2
            exit 2
            ;;
    esac

    local rc="${SMATCHET_VCS_RC:-$HOME/.bashrc}"
    write_rc_block "$rc" "$vcs" "$backend"
    sync_windows_registry "$vcs" "$backend"

    echo "set-vcs-mode: mode '$mode' set."
    echo "  rc file: $rc"
    echo "    export SMATCHET_AGENT_VCS=$vcs"
    echo "    export SMATCHET_LOCK_BACKEND=$backend"
    if is_windows && [ "${SMATCHET_VCS_SKIP_REGISTRY:-}" != "1" ]; then
        echo "  Windows User registry: SMATCHET_AGENT_VCS=$vcs, SMATCHET_LOCK_BACKEND=$backend (via setx)"
    fi
    echo "  Open a new shell (or 'source $rc') for the change to take effect."
}

main "$@"
