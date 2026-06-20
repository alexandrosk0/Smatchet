#!/usr/bin/env bash
# test-fetchcontent-cache-path-drift.sh — keep CI's FetchContent cache `path:`
# values in lockstep with the FETCHCONTENT_BASE_DIR default in CMakeLists.txt.
# ----------------------------------------------------------------------------
# Class of bug this kills (PR-15 / fetchcontent-cache-path-drift):
# the root CMakeLists.txt defaults FETCHCONTENT_BASE_DIR to
# "${CMAKE_SOURCE_DIR}/.fetchcontent-src", and every `actions/cache` step in
# .github/workflows/build-and-test.yml that caches the FetchContent dep sources
# points its `path:` at that same `.fetchcontent-src`. If someone renames the
# CMake default (e.g. back to the implicit `${CMAKE_BINARY_DIR}/_deps`, or to a
# new dir) WITHOUT updating the workflow cache paths — or vice versa — the CI
# cache silently caches the wrong directory: every run is a cold clone, the cache
# is never populated, and nobody notices except as slow builds. This cheap text
# gate diffs the two and FAILS LOUDLY on any mismatch.
#
# What it compares (basenames, the only portable cross-OS unit):
#   * CMake side: the trailing path component of the FETCHCONTENT_BASE_DIR default
#     in CMakeLists.txt — i.e. `.fetchcontent-src`.
#   * CI side: the `path:` value of every `actions/cache` step in
#     build-and-test.yml whose `key:` contains "fetchcontent" (those are the
#     dep-source caches; non-fetchcontent caches like sccache are ignored).
#   Every CI fetchcontent cache path's basename MUST equal the CMake basename.
#
# Fail-CLOSED: a missing file / unreadable input / zero fetchcontent caches found
# / unparseable CMake default is an infra error (exit 2), never a silent pass —
# a zero-cache result almost certainly means the parser broke, not that the
# caches vanished.
#
# Modes:
#   (no args) | --check   scan the real tree; exit 0 in lockstep, 1 on drift.
#   --selftest            dogfood: plant a drifted workflow -> assert FAIL; an
#                         aligned one -> assert PASS. Asserts a failure case.
#
# Exit: 0 in lockstep · 1 drift detected · 2 infra error.
# selftest: asserts-failure
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

CMAKE_FILE_DEFAULT="CMakeLists.txt"
WORKFLOW_DEFAULT=".github/workflows/build-and-test.yml"

# cmake_basename <cmakelists-path> — echo the basename of the FETCHCONTENT_BASE_DIR
# default. Returns 2 on unreadable/unparseable input (fail-closed).
cmake_basename() {
    local f="$1" line dir
    [ -r "$f" ] || { echo "test-fetchcontent-cache-path-drift: unreadable CMake file: $f" >&2; return 2; }
    # Match: set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.fetchcontent-src" ...)
    # Tolerate single/double quotes and arbitrary leading whitespace.
    line="$(grep -nE 'set[ \t]*\([ \t]*FETCHCONTENT_BASE_DIR[ \t]+["'"'"']' "$f" | head -1)"
    if [ -z "$line" ]; then
        echo "test-fetchcontent-cache-path-drift: could not find a FETCHCONTENT_BASE_DIR set() in $f" >&2
        return 2
    fi
    # Extract the quoted value, then take its trailing path component.
    dir="$(printf '%s\n' "$line" | sed -E 's/.*FETCHCONTENT_BASE_DIR[ \t]+["'"'"']([^"'"'"']*)["'"'"'].*/\1/')"
    if [ -z "$dir" ] || [ "$dir" = "$line" ]; then
        echo "test-fetchcontent-cache-path-drift: could not parse the FETCHCONTENT_BASE_DIR value from: $line" >&2
        return 2
    fi
    # Basename: strip any trailing slash, then everything up to the last slash.
    dir="${dir%/}"
    printf '%s\n' "${dir##*/}"
}

# ci_basenames <workflow-path> — echo, one per line, the basename of every
# actions/cache `path:` whose step caches FetchContent (key contains
# "fetchcontent"). Returns 2 on unreadable input. Empty output is the caller's
# fail-closed signal.
#
# Parser: walk the YAML linearly. Track the most recent `path:` and `key:` seen
# inside a `with:`/`uses: actions/cache` block; when a `key:` containing
# "fetchcontent" appears, emit the basename of the path that pairs with it. CI
# convention here is `path:` immediately followed by `key:` in each cache step, so
# we emit on the key line using the last-seen path.
ci_basenames() {
    local f="$1" last_path="" p base
    [ -r "$f" ] || { echo "test-fetchcontent-cache-path-drift: unreadable workflow: $f" >&2; return 2; }
    while IFS= read -r raw; do
        case "$raw" in
            *path:*)
                # Capture the path value (strip leading "...path:" + surrounding ws/quotes).
                p="$(printf '%s\n' "$raw" | sed -E 's/^[[:space:]]*path:[[:space:]]*//; s/^["'"'"']//; s/["'"'"'][[:space:]]*$//; s/[[:space:]]*$//')"
                last_path="$p"
                ;;
            *key:*fetchcontent*)
                if [ -n "$last_path" ]; then
                    base="${last_path%/}"
                    printf '%s\n' "${base##*/}"
                fi
                ;;
        esac
    done < "$f"
    return 0
}

# run_check <root> [cmake-rel] [workflow-rel]
run_check() {
    local root="$1"
    local cmake_rel="${2:-$CMAKE_FILE_DEFAULT}"
    local wf_rel="${3:-$WORKFLOW_DEFAULT}"
    local cmake_base ci_list b rc=0 n_ci=0 drift=()

    command -v sed >/dev/null 2>&1 || { echo "test-fetchcontent-cache-path-drift: sed not on PATH" >&2; return 2; }

    cmake_base="$(cmake_basename "$root/$cmake_rel")" || return 2
    ci_list="$(ci_basenames "$root/$wf_rel")" || return 2

    if [ -z "$ci_list" ]; then
        echo "test-fetchcontent-cache-path-drift: FAIL — found ZERO FetchContent cache steps in" >&2
        echo "    $wf_rel (expected several). The parser is likely broken — fail-closed." >&2
        return 2
    fi

    while IFS= read -r b; do
        [ -n "$b" ] || continue
        n_ci=$((n_ci + 1))
        if [ "$b" != "$cmake_base" ]; then
            drift+=("$b")
        fi
    done <<EOF
$ci_list
EOF

    if [ "${#drift[@]}" -ne 0 ]; then
        echo "test-fetchcontent-cache-path-drift: FAIL — CI FetchContent cache path drift." >&2
        echo "  CMakeLists.txt FETCHCONTENT_BASE_DIR default basename: '$cmake_base'" >&2
        echo "  but these CI cache step path basenames differ:" >&2
        local d
        for d in "${drift[@]}"; do echo "    '$d'  (in $wf_rel)" >&2; done
        echo "  Align the actions/cache 'path:' values in $wf_rel with the CMake default" >&2
        echo "  (or update the CMake default), so CI actually caches the dep sources." >&2
        rc=1
    else
        echo "test-fetchcontent-cache-path-drift: PASS — all $n_ci CI FetchContent cache paths"
        echo "  match the CMakeLists.txt default basename ('$cmake_base')."
    fi
    # test-all.sh summary line (it parses `^Passed: N  Failed: M`).
    if [ "$rc" -eq 0 ]; then
        echo "Passed: 1  Failed: 0"
    else
        echo "Passed: 0  Failed: ${#drift[@]}"
    fi
    return "$rc"
}

# run_selftest — plant a drifted workflow (must FAIL) then an aligned one (must
# PASS) against a synthetic root. Asserts the failure case.
run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)" || { echo "test-fetchcontent-cache-path-drift selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/.github/workflows"

    cat > "$tmp/CMakeLists.txt" <<'CM'
project(Fake)
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.fetchcontent-src" CACHE PATH "x")
endif()
CM

    # DRIFTED workflow: cache path points at a DIFFERENT dir than the CMake default.
    cat > "$tmp/.github/workflows/build-and-test.yml" <<'WF'
jobs:
  a:
    steps:
      - name: Cache FetchContent _deps
        uses: actions/cache@v5
        with:
          path: _deps
          key: ${{ runner.os }}-fetchcontent-msvc-x
WF
    # selftest: asserts-failure — drifted path must be flagged.
    if run_check "$tmp" >/dev/null 2>&1; then
        echo "test-fetchcontent-cache-path-drift selftest: FAIL — drift was NOT detected" >&2
        rc=1
    fi

    # ALIGNED workflow: cache path basename matches the CMake default.
    cat > "$tmp/.github/workflows/build-and-test.yml" <<'WF'
jobs:
  a:
    steps:
      - name: Cache FetchContent _deps
        uses: actions/cache@v5
        with:
          path: .fetchcontent-src
          key: ${{ runner.os }}-fetchcontent-msvc-x
WF
    if ! run_check "$tmp" >/dev/null 2>&1; then
        echo "test-fetchcontent-cache-path-drift selftest: FAIL — aligned paths wrongly flagged" >&2
        rc=1
    fi

    # ZERO fetchcontent caches -> infra error (exit 2), not a silent pass.
    cat > "$tmp/.github/workflows/build-and-test.yml" <<'WF'
jobs:
  a:
    steps:
      - name: Cache sccache
        uses: actions/cache@v5
        with:
          path: .sccache
          key: ${{ runner.os }}-sccache-x
WF
    run_check "$tmp" >/dev/null 2>&1
    if [ "$?" -ne 2 ]; then
        echo "test-fetchcontent-cache-path-drift selftest: FAIL — zero-cache case did not fail-closed (exit 2)" >&2
        rc=1
    fi

    if [ "$rc" -eq 0 ]; then echo "test-fetchcontent-cache-path-drift --selftest: PASS"; fi
    return "$rc"
}

case "${1:---check}" in
    --check)    run_check "$ROOT" ;;
    --selftest) run_selftest ;;
    -h|--help)  echo "usage: test-fetchcontent-cache-path-drift.sh [--check|--selftest]" ;;
    *)          echo "usage: test-fetchcontent-cache-path-drift.sh [--check|--selftest]" >&2; exit 2 ;;
esac
