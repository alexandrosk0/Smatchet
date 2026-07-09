#!/usr/bin/env bash
# remote-container-bootstrap.sh — provision a Claude Code remote container so
# the posix-core-check lane configures without rediscovering the egress
# workaround (infra 2026-07-05-remote-container-fetchcontent-403).
#
# The container's network policy allows `git clone` but returns 403 for GitHub
# release-asset / codeload tarball downloads, so cpr's internal FetchContent of
# curl-7.80.0.tar.xz fails at configure; glfw's configure additionally needs
# xorg-dev + libgl1-mesa-dev (glfw never *builds* in this preset, but its
# find_package(X11 REQUIRED) runs). Steps (validated in the 2026-07-05 session):
#   1. shallow git clone of curl at cpr's pinned tag into
#      .fetchcontent-src/curl-manual (tag mirrors the 403'd tarball version)
#   2. apt-get install xorg-dev libgl1-mesa-dev
#   3. cmake --preset posix-core-check -DFETCHCONTENT_SOURCE_DIR_CURL=<clone>
#
# Idempotent: the clone is skipped when present, apt is skipped when both
# packages are installed. --no-configure runs steps 1+2 only.
#
# Exit: 0 provisioned (+ configured) · 1 a step failed · 2 usage / tool missing.
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "remote-container-bootstrap: not inside a git repo" >&2; exit 2; }
cd "$ROOT" || exit 2

if ! command -v cmake >/dev/null 2>&1; then
    echo "remote-container-bootstrap: cmake not on PATH" >&2; exit 2
fi

CONFIGURE=1
case "${1:-}" in
    "") ;;
    --no-configure) CONFIGURE=0 ;;
    *) echo "usage: remote-container-bootstrap.sh [--no-configure]" >&2; exit 2 ;;
esac

CURL_SRC="$ROOT/.fetchcontent-src/curl-manual"
if [ -d "$CURL_SRC/.git" ]; then
    echo "remote-container-bootstrap: curl-manual clone present — skipping ($CURL_SRC)"
else
    echo "remote-container-bootstrap: cloning curl-7_80_0 (the 403'd tarball's tag) ..."
    git clone --depth 1 --branch curl-7_80_0 https://github.com/curl/curl.git "$CURL_SRC" || {
        echo "remote-container-bootstrap: curl-manual clone failed" >&2; exit 1; }
fi

if dpkg -s xorg-dev libgl1-mesa-dev >/dev/null 2>&1; then
    echo "remote-container-bootstrap: xorg-dev + libgl1-mesa-dev present — skipping apt"
else
    APT="apt-get"
    if [ "$(id -u)" -ne 0 ]; then APT="sudo apt-get"; fi
    echo "remote-container-bootstrap: installing xorg-dev + libgl1-mesa-dev ..."
    if ! { $APT update -qq && $APT install -y -qq xorg-dev libgl1-mesa-dev; }; then
        echo "remote-container-bootstrap: apt install failed" >&2; exit 1
    fi
fi

if [ "$CONFIGURE" -eq 0 ]; then
    echo "remote-container-bootstrap: provisioned (configure skipped — --no-configure)"
    exit 0
fi

cmake --preset posix-core-check "-DFETCHCONTENT_SOURCE_DIR_CURL=$CURL_SRC" || {
    echo "remote-container-bootstrap: posix-core-check configure failed" >&2; exit 1; }
echo "remote-container-bootstrap: done — build with: cmake --build --preset posix-core-check"
