#!/usr/bin/env bash
# scripts/dev/check-required-tools.sh — probe the orchestrator's standard
# tool set and print a PASS/FAIL summary.
#
# Addresses the iter-loop friction documented in
# docs/self-improvement/categories/tooling.md
# "Required CLI tools must be discoverable + verified at first-setup time".
# Two tools (jq, gh) bit the orchestrator mid-merge-gates-poll last session
# because neither was on the bash-tool PATH nor documented in SETUP.md as
# required. This script makes the absence loud at setup time, not at the
# next high-stakes moment.
#
# Tools probed:
#   - cmake, ninja, g++, gcc       — build toolchain
#   - python                       — dev scripts (perf-compare.py, etc.)
#   - jq, gh                       — merge-gates poller + PR ops
#   - clang-format, clang-tidy,    — lint hooks
#     cppcheck
#   - flock                        — lint-cpp-drain.sh queue serialisation
#   - bats                         — tests/bats/*.bats (merge-gates regression suite)
#   - git                          — table stakes
#
# Exit 0 when every REQUIRED tool resolves. Exit 1 on any missing required tool.
# Optional tools (currently: opencppcoverage) print a WARN line but don't fail.
#
# Wired as the first step of `scripts/setup-harness.sh` (claude-code path) so
# the absence fires at clone time. Also safe to invoke ad-hoc:
#
#   bash scripts/dev/check-required-tools.sh
#   bash scripts/dev/check-required-tools.sh --quiet  # only print failures

set -uo pipefail

QUIET=0
case "${1:-}" in
    --quiet|-q) QUIET=1 ;;
    -h|--help)
        cat >&2 <<'USAGE'
Usage: bash scripts/dev/check-required-tools.sh [--quiet]

Probes the Smatchet orchestrator's standard CLI tool set. Exits 0 iff every
required tool resolves; exits 1 on any missing required tool. Optional tools
warn-only.
USAGE
        exit 2
        ;;
esac

# tool spec — one row per tool, columns: name | category | install-hint
# Use a sentinel separator (\x1f) so install hints can carry spaces / pipes.
# REQUIRED rows fail the script on absence; OPTIONAL rows print WARN.
SEP=$'\x1f'
TOOLS=(
    "git${SEP}REQUIRED${SEP}any platform — table stakes"
    "cmake${SEP}REQUIRED${SEP}Windows: winget install Kitware.CMake | MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-cmake"
    "ninja${SEP}REQUIRED${SEP}Windows: winget install Ninja-build.Ninja | MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-ninja"
    "gcc${SEP}REQUIRED${SEP}MSYS2 UCRT64 (lint toolchain): pacman -S mingw-w64-ucrt-x86_64-gcc — build uses MSVC/Clang"
    "g++${SEP}REQUIRED${SEP}MSYS2 UCRT64 (lint toolchain): pacman -S mingw-w64-ucrt-x86_64-gcc — build uses MSVC/Clang"
    "python${SEP}REQUIRED${SEP}Windows: python.org installer (3.11+) or pacman -S mingw-w64-ucrt-x86_64-python"
    "jq${SEP}REQUIRED${SEP}Test harness only (merge_gates.bats mocks gh via jq). The merge-gates poller + watcher now parse via gh's BUNDLED jq (gh api --jq) — no standalone jq at runtime. winget install jqlang.jq | MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-jq"
    "gh${SEP}REQUIRED${SEP}Windows: winget install GitHub.cli — then add 'C:/Program Files/GitHub CLI' to PATH"
    "clang-format${SEP}REQUIRED${SEP}MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra"
    "clang-tidy${SEP}REQUIRED${SEP}MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra"
    "cppcheck${SEP}REQUIRED${SEP}MSYS2: pacman -S mingw-w64-ucrt-x86_64-cppcheck"
    "flock${SEP}REQUIRED${SEP}Linux/macOS: usually built-in. MSYS2: pacman -S util-linux"
    "bats${SEP}REQUIRED${SEP}Any platform: npm i -g bats (preferred — resolves on the bash-tool PATH) | MSYS2: pacman -S bats | macOS: brew install bats-core — runs tests/bats/*.bats (merge-gates regression suite)"
    "OpenCppCoverage${SEP}OPTIONAL${SEP}Coverage gates only — choco install opencppcoverage (Windows admin shell)"
)

PASS=0
FAIL=0
WARN=0

# Prepend known toolchain dirs idempotently so the probe doesn't false-fail
# on tools that ARE installed but not on the inherited PATH (the exact issue
# `jq` + `gh` hit last session). Mirrors the lint-cpp-common.sh pattern.
case ":$PATH:" in
    *":/c/msys64/ucrt64/bin:"*) ;;
    *) [[ -d /c/msys64/ucrt64/bin ]] && export PATH="/c/msys64/ucrt64/bin:$PATH" ;;
esac
case ":$PATH:" in
    *":/c/Program Files/GitHub CLI:"*) ;;
    *) [[ -d "/c/Program Files/GitHub CLI" ]] && export PATH="/c/Program Files/GitHub CLI:$PATH" ;;
esac

(( QUIET )) || echo "check-required-tools — Smatchet orchestrator tool set"
(( QUIET )) || echo

for row in "${TOOLS[@]}"; do
    IFS="$SEP" read -r tool category hint <<<"$row"
    if command -v "$tool" >/dev/null 2>&1; then
        loc="$(command -v "$tool")"
        (( QUIET )) || printf "  PASS  %-18s -> %s\n" "$tool" "$loc"
        PASS=$((PASS + 1))
    else
        if [[ "$category" == "REQUIRED" ]]; then
            printf "  FAIL  %-18s (REQUIRED) — %s\n" "$tool" "$hint" >&2
            FAIL=$((FAIL + 1))
        else
            printf "  WARN  %-18s (optional) — %s\n" "$tool" "$hint" >&2
            WARN=$((WARN + 1))
        fi
    fi
done

(( QUIET )) || echo
(( QUIET )) || echo "summary: $PASS pass / $FAIL fail (required) / $WARN warn (optional)"

exit $(( FAIL > 0 ? 1 : 0 ))
