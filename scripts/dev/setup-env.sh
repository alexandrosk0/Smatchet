#!/usr/bin/env bash
# scripts/dev/setup-env.sh — install the Smatchet dev-environment CLI tool set.
#
# The companion probes are read-only:
#   scripts/dev/check-required-tools.sh   PASS/FAIL table for the orchestrator tool set
#   scripts/dev/doctor.sh                 toolchain versions + disk pre-flight
#
# This script closes the loop: it detects which of those tools are missing and
# installs them through the host's native package manager (winget or MSYS2
# pacman on Windows, apt on Debian/Ubuntu, brew on macOS, npm for the two
# node-distributed tools), then re-runs check-required-tools.sh to verify.
#
# Idempotent — already-present tools are skipped, so re-running is a no-op.
# It does NOT install the C++ build toolchain (Visual Studio / MSVC or
# Clang-cl) or configure/build the project; see BUILD.md § Prerequisites and
# docs/harness/SETUP.md for those steps.
#
# Usage:
#   bash scripts/dev/setup-env.sh                # show plan, prompt, install
#   bash scripts/dev/setup-env.sh --yes          # non-interactive install
#   bash scripts/dev/setup-env.sh --dry-run      # print the plan, install nothing
#   bash scripts/dev/setup-env.sh --list         # print the full tool -> package map
#   bash scripts/dev/setup-env.sh --with-optional  # also install warn-only tools
#
# Exit codes:
#   0  every required tool present after the run
#   1  one or more tools still missing (an installer failed, no package manager
#      could provide it, or the user declined the plan at the prompt — the
#      reason is printed per tool)
#   2  usage error

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ASSUME_YES=0
DRY_RUN=0
LIST_ONLY=0
WITH_OPTIONAL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes)       ASSUME_YES=1 ;;
        -n|--dry-run)   DRY_RUN=1 ;;
        -l|--list)      LIST_ONLY=1 ;;
        --with-optional) WITH_OPTIONAL=1 ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "setup-env: unknown argument '$1' (try --help)" >&2
            exit 2
            ;;
    esac
    shift
done

color_red()    { printf '\033[31m%s\033[0m\n' "$*"; }
color_green()  { printf '\033[32m%s\033[0m\n' "$*"; }
color_yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

# --- platform + package-manager detection ------------------------------------

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
    Linux*)               PLATFORM=linux ;;
    Darwin*)              PLATFORM=macos ;;
    *)                    PLATFORM=unknown ;;
esac

# Known toolchain dirs are prepended so a tool that IS installed but absent
# from the inherited PATH isn't reinstalled. Same dir set check-required-tools.sh
# probes, plus winget's Links shim dir (where winget puts its own PATH entries).
prepend_path_dir() {
    local dir="$1"
    [[ -d "$dir" ]] || return 0
    case ":$PATH:" in
        *":$dir:"*) ;;
        *) export PATH="$dir:$PATH" ;;
    esac
}

refresh_path() {
    local win_root pkg_dir
    for win_root in /c /mnt/c; do
        prepend_path_dir "$win_root/msys64/ucrt64/bin"
        prepend_path_dir "$win_root/msys64/usr/bin"
        prepend_path_dir "$win_root/Program Files/CMake/bin"
        prepend_path_dir "$win_root/Program Files/GitHub CLI"
        prepend_path_dir "$win_root/Program Files/LLVM/bin"
        # Machine-scope Python roots. The version is not pinned to the winget id
        # (Python.Python.3.13 → Python313) because a host may carry any of them.
        for pkg_dir in "$win_root"/Python3*; do
            [[ -d "$pkg_dir" ]] || continue
            prepend_path_dir "$pkg_dir"
            prepend_path_dir "$pkg_dir/Scripts"
        done
        [[ -d "$win_root/Users" ]] || continue
        for pkg_dir in "$win_root"/Users/*/AppData/Local/Microsoft/WinGet/Packages/*; do
            [[ -d "$pkg_dir" ]] || continue
            prepend_path_dir "$pkg_dir"
            prepend_path_dir "$pkg_dir/bin"
            prepend_path_dir "$pkg_dir/mingw64/bin"
        done
        # winget's DEFAULT (user) scope for Python.Python.3.13 lands here, not in
        # C:\Python313 — without this the tool we just installed stays invisible.
        for pkg_dir in "$win_root"/Users/*/AppData/Local/Programs/Python/Python3*; do
            [[ -d "$pkg_dir" ]] || continue
            prepend_path_dir "$pkg_dir"
            prepend_path_dir "$pkg_dir/Scripts"
        done
    done
    # NOTE: %LOCALAPPDATA%\Microsoft\WindowsApps is deliberately NOT prepended.
    # It holds the Microsoft Store `python.exe` stub, which resolves under
    # `command -v` but only opens the Store — that would make `have python`
    # true, skip a real install, and let verification pass on a dead tool.
    # check-required-tools.sh omits it for the same reason; stay aligned.
}
refresh_path

have() {
    command -v "$1" >/dev/null 2>&1 || command -v "$1.exe" >/dev/null 2>&1
}

HAVE_WINGET=0; have winget && HAVE_WINGET=1
HAVE_PACMAN=0; have pacman && HAVE_PACMAN=1
HAVE_APT=0;    have apt-get && HAVE_APT=1
HAVE_BREW=0;   have brew && HAVE_BREW=1
HAVE_NPM=0;    have npm && HAVE_NPM=1

# --- package map --------------------------------------------------------------
#
# One row per package (not per tool) so a package covering several tools —
# LLVM ships clang-format + clang-tidy, gcc ships gcc + g++ — installs once.
#
# Columns: tools | category | winget-id | pacman-pkg | apt-pkg | brew-pkg | npm-pkg
# `-` means "this manager cannot provide it". `tools` is a comma-separated list;
# the package is installed when ANY of its tools is missing. A package cell may
# be a SPACE-separated list when one manager splits what another ships as one
# package (Debian ships clang-format and clang-tidy separately) — winget is the
# exception, `--id` takes exactly one id.

SEP=$'\x1f'
PACKAGES=(
    "git${SEP}REQUIRED${SEP}Git.Git${SEP}git${SEP}git${SEP}git${SEP}-"
    "cmake${SEP}REQUIRED${SEP}Kitware.CMake${SEP}mingw-w64-ucrt-x86_64-cmake${SEP}cmake${SEP}cmake${SEP}-"
    "ninja${SEP}REQUIRED${SEP}Ninja-build.Ninja${SEP}mingw-w64-ucrt-x86_64-ninja${SEP}ninja-build${SEP}ninja${SEP}-"
    "gcc,g++${SEP}REQUIRED${SEP}-${SEP}mingw-w64-ucrt-x86_64-gcc${SEP}build-essential${SEP}gcc${SEP}-"
    "python${SEP}REQUIRED${SEP}Python.Python.3.13${SEP}mingw-w64-ucrt-x86_64-python${SEP}python3 python-is-python3${SEP}python${SEP}-"
    "jq${SEP}REQUIRED${SEP}jqlang.jq${SEP}mingw-w64-ucrt-x86_64-jq${SEP}jq${SEP}jq${SEP}-"
    "gh${SEP}REQUIRED${SEP}GitHub.cli${SEP}-${SEP}gh${SEP}gh${SEP}-"
    "clang-format,clang-tidy${SEP}REQUIRED${SEP}LLVM.LLVM${SEP}mingw-w64-ucrt-x86_64-clang-tools-extra${SEP}clang-format clang-tidy${SEP}llvm${SEP}-"
    "cppcheck${SEP}REQUIRED${SEP}Cppcheck.Cppcheck${SEP}mingw-w64-ucrt-x86_64-cppcheck${SEP}cppcheck${SEP}cppcheck${SEP}-"
    "flock${SEP}REQUIRED${SEP}-${SEP}util-linux${SEP}util-linux${SEP}util-linux${SEP}-"
    "bats${SEP}REQUIRED${SEP}-${SEP}-${SEP}-${SEP}-${SEP}bats"
    "shellcheck${SEP}REQUIRED${SEP}koalaman.shellcheck${SEP}mingw-w64-ucrt-x86_64-shellcheck${SEP}shellcheck${SEP}shellcheck${SEP}shellcheck"
    "OpenCppCoverage${SEP}OPTIONAL${SEP}-${SEP}-${SEP}-${SEP}-${SEP}-"
)

# Windows notes for the packages winget cannot supply.
manual_hint() {
    case "$1" in
        "gcc,g++") echo "MSYS2 UCRT64 toolchain — install MSYS2 (winget install MSYS2.MSYS2), then: pacman -S mingw-w64-ucrt-x86_64-gcc" ;;
        flock)     echo "MSYS2: pacman -S util-linux (Git-Bash alone does not ship flock)" ;;
        bats)      echo "npm i -g bats (install Node.js first: winget install OpenJS.NodeJS.LTS)" ;;
        OpenCppCoverage) echo "coverage gates only — choco install opencppcoverage, or the GitHub releases installer" ;;
        *)         echo "no package-manager mapping for this platform — see docs/harness/SETUP.md § Required CLI tools" ;;
    esac
}

# `pkgs_for` prints the manager choice for a row as "<manager> <package>", or
# empty when nothing on this host can provide it. Order encodes preference:
# native platform manager first, npm last (it only owns bats + shellcheck).
pkgs_for() {
    local winget_id="$1" pacman_pkg="$2" apt_pkg="$3" brew_pkg="$4" npm_pkg="$5"
    if [[ "$PLATFORM" == windows ]]; then
        if (( HAVE_WINGET )) && [[ "$winget_id" != "-" ]]; then echo "winget $winget_id"; return; fi
        if (( HAVE_PACMAN )) && [[ "$pacman_pkg" != "-" ]]; then echo "pacman $pacman_pkg"; return; fi
    elif [[ "$PLATFORM" == linux ]]; then
        if (( HAVE_APT )) && [[ "$apt_pkg" != "-" ]]; then echo "apt $apt_pkg"; return; fi
    elif [[ "$PLATFORM" == macos ]]; then
        if (( HAVE_BREW )) && [[ "$brew_pkg" != "-" ]]; then echo "brew $brew_pkg"; return; fi
    fi
    if (( HAVE_NPM )) && [[ "$npm_pkg" != "-" ]]; then echo "npm $npm_pkg"; return; fi
    echo ""
}

# install_with <manager> <package>... — every manager but winget takes the whole
# list in one transaction; winget's --id is single-valued, so it loops.
install_with() {
    local manager="$1"
    shift
    (( $# > 0 )) || return 1
    case "$manager" in
        winget)
            local id
            for id in "$@"; do
                winget install --exact --id "$id" \
                    --accept-package-agreements --accept-source-agreements \
                    --disable-interactivity --silent || return 1
            done
            ;;
        pacman) pacman -S --needed --noconfirm "$@" ;;
        apt)    sudo apt-get install -y "$@" ;;
        brew)   brew install "$@" ;;
        npm)    npm install -g "$@" ;;
        *)      return 1 ;;
    esac
}

# --- plan ---------------------------------------------------------------------

echo "setup-env — Smatchet dev environment"
echo "  repo:     $REPO_ROOT"
echo "  platform: $PLATFORM"
printf '  managers:'
(( HAVE_WINGET )) && printf ' winget'
(( HAVE_PACMAN )) && printf ' pacman'
(( HAVE_APT ))    && printf ' apt'
(( HAVE_BREW ))   && printf ' brew'
(( HAVE_NPM ))    && printf ' npm'
printf '\n\n'

PLAN_ROWS=()
UNMAPPED_ROWS=()
PRESENT=0

for row in "${PACKAGES[@]}"; do
    IFS="$SEP" read -r tools category winget_id pacman_pkg apt_pkg brew_pkg npm_pkg <<<"$row"

    if [[ "$category" == "OPTIONAL" ]] && (( ! WITH_OPTIONAL )) && (( ! LIST_ONLY )); then
        continue
    fi

    missing=""
    IFS=',' read -r -a tool_list <<<"$tools"
    for tool in "${tool_list[@]}"; do
        have "$tool" || missing="${missing:+$missing,}$tool"
    done

    choice="$(pkgs_for "$winget_id" "$pacman_pkg" "$apt_pkg" "$brew_pkg" "$npm_pkg")"

    if (( LIST_ONLY )); then
        printf '  %-24s %-9s %s\n' "$tools" "$category" "${choice:-<no mapping> — $(manual_hint "$tools")}"
        continue
    fi

    if [[ -z "$missing" ]]; then
        printf '  %-24s present\n' "$tools"
        PRESENT=$((PRESENT + 1))
        continue
    fi

    if [[ -z "$choice" ]]; then
        UNMAPPED_ROWS+=("${tools}${SEP}${category}")
        continue
    fi
    PLAN_ROWS+=("${tools}${SEP}${missing}${SEP}${choice}")
done

(( LIST_ONLY )) && exit 0

echo
if (( ${#PLAN_ROWS[@]} == 0 )) && (( ${#UNMAPPED_ROWS[@]} == 0 )); then
    color_green "Nothing to install — all $PRESENT packages already present."
else
    echo "Install plan:"
    # ${A[@]+"${A[@]}"} — bash < 4.4 (macOS /bin/bash 3.2) treats a plain
    # "${A[@]}" on an EMPTY array as unbound under `set -u`.
    for row in ${PLAN_ROWS[@]+"${PLAN_ROWS[@]}"}; do
        IFS="$SEP" read -r tools missing choice <<<"$row"
        printf '  + %-24s (missing: %s)  via %s\n' "$tools" "$missing" "$choice"
    done
    for row in ${UNMAPPED_ROWS[@]+"${UNMAPPED_ROWS[@]}"}; do
        IFS="$SEP" read -r tools category <<<"$row"
        color_yellow "  ! $tools ($category) — no automatic install on this host: $(manual_hint "$tools")"
    done
fi

if (( DRY_RUN )); then
    echo
    echo "--dry-run: nothing installed."
    exit 0
fi

if (( ${#PLAN_ROWS[@]} > 0 )) && (( ! ASSUME_YES )); then
    echo
    read -r -p "Proceed with the install plan above? [y/N] " reply
    case "$reply" in
        y|Y|yes|YES) ;;
        # exit 1, not 0: the required tools are still missing, and a caller
        # gating on the status must not walk into a half-provisioned env.
        *) echo "Aborted — nothing installed."; exit 1 ;;
    esac
fi

# --- install ------------------------------------------------------------------

INSTALL_FAILURES=0
for row in ${PLAN_ROWS[@]+"${PLAN_ROWS[@]}"}; do
    IFS="$SEP" read -r tools missing choice <<<"$row"
    read -r -a choice_parts <<<"$choice"
    manager="${choice_parts[0]}"
    echo
    echo "==> installing $tools ($choice)"
    if install_with "$manager" "${choice_parts[@]:1}"; then
        color_green "    ok: $tools"
    else
        color_red "    FAILED: $manager install of '${choice_parts[*]:1}' (for $tools)"
        INSTALL_FAILURES=$((INSTALL_FAILURES + 1))
    fi
done

# winget/npm write PATH entries this shell never saw; re-scan the known dirs so
# verification below reflects what was just installed rather than the stale PATH.
refresh_path

# --- verify -------------------------------------------------------------------

echo
echo "==> verifying with scripts/dev/check-required-tools.sh"
echo
PROBE_RC=0
bash "$SCRIPT_DIR/check-required-tools.sh" || PROBE_RC=$?

# The probe does not cover every row in this map (shellcheck and bats are lint /
# test-harness tools it never looks for), so a GREEN probe alone would let a
# failed shellcheck install report success while the pre-push gate stays broken.
# Re-check every REQUIRED tool here; this is the authoritative verdict.
RESIDUAL=""
for row in "${PACKAGES[@]}"; do
    IFS="$SEP" read -r tools category _rest <<<"$row"
    [[ "$category" == "REQUIRED" ]] || continue
    IFS=',' read -r -a tool_list <<<"$tools"
    for tool in "${tool_list[@]}"; do
        have "$tool" || RESIDUAL="${RESIDUAL:+$RESIDUAL }$tool"
    done
done

if (( PROBE_RC == 0 )) && [[ -z "$RESIDUAL" ]]; then
    echo
    color_green "setup-env: GREEN — required tool set complete."
    echo "Next: bash scripts/dev/doctor.sh          # build toolchain + disk pre-flight"
    echo "      bash agents/scripts/core/setup-harness.sh claude-code   # wire agent hooks"
    exit 0
fi

echo
color_red "setup-env: RED — tools still missing after the install pass."
if [[ -n "$RESIDUAL" ]]; then
    color_red "  still missing: $RESIDUAL"
fi
if (( INSTALL_FAILURES > 0 )); then
    echo "  $INSTALL_FAILURES package install(s) failed above — read their output first."
fi
cat <<'HINT'
  If a tool WAS just installed, its PATH entry may only exist in a fresh shell:
  reopen the terminal (or `hash -r`) and re-run check-required-tools.sh before
  treating this as a real failure.
  Install hints for anything still missing: docs/harness/SETUP.md § Required CLI tools.
HINT
exit 1
