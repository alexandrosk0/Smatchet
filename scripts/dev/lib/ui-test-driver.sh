#!/usr/bin/env bash
# ui-test-driver.sh — shared preamble for the bucket-E drivers (sourced, not run).
# ----------------------------------------------------------------------------
# Every scripts/dev/test-ui-*.sh boots the real exe (`cmd ui_test.run --spawn`).
# The hardening below used to live inline in a few drivers and drift out of the
# rest one script at a time; it lives here so a driver opts in with one call.
#
#   ui_test_require_fresh_exe <exe>
#       Call right after the driver's binary-missing check. Hard-fails (returns
#       2, the binary-missing class) when a source compiled into Smatchet.exe is
#       newer than <exe>, naming the newest offender — a stale exe gives a
#       product verdict on a binary that does not contain the change under test
#       (test 2026-08-06-bucket-e-driver-no-exe-staleness-guard). Only the trees
#       the exe links count (_UI_TEST_EXE_SOURCES): Source/Mobile and
#       Source/UnrealPlugins are not linked, so an edit there can never be
#       rebuilt into the exe and must not mark it stale forever. Delegates to
#       scripts/dev/is-exe-fresh.sh. Opt-out: SMATCHET_ALLOW_STALE_EXE=1 (warns,
#       runs anyway). Skipped when CI is set: CI runs an exe it just built or
#       downloaded, whose mtime against a fresh checkout means nothing.
#
#   ui_test_isolate_home [--seed] [--shadow-platform-dirs]
#       Point the run at a throwaway profile so it never reads or writes the
#       developer's real one (test 2026-08-05-bucket-e-inherits-developer-imgui-ini,
#       tooling 2026-08-07-bucket-e-runners-need-ephemeral-home). Creates a
#       mktemp dir removed by an EXIT trap and exports:
#         SMATCHET_USER_DATA=<dir>  — config, imgui.ini, views, SQLite cache and
#                                     instance.json all resolve under it
#                                     (StandaloneAppBootstrap; inherited by the
#                                     --spawn child);
#         SMATCHET_UPDATE_CHECK=0   — no startup release check, so the
#                                     "Update Available" modal can never open and
#                                     swallow hover / nav / item registration
#                                     (ConfigManager env override; test
#                                     2026-08-06-bucket-e-prefs-body-and-modal-inhibition (c)).
#       --seed writes UI_TEST_SEED_CONFIG as the profile's config: a configured
#       profile (not read-only, no auto-opened Preferences, no
#       ##WhisperSetupBanner floating over window headers). Leave it off for a
#       driver whose tests assert fresh-profile (first-run) defaults.
#       --shadow-platform-dirs also points LOCALAPPDATA / APPDATA /
#       XDG_CONFIG_HOME at the dir, for state ConfigManager resolves from the
#       platform-shared dir rather than the user-data dir.
#       SMATCHET_UI_TEST_HOME pins the dir instead (created if missing, KEPT on
#       exit — the app's own log lives under it, and a failing run is
#       undebuggable once a throwaway dir is gone). Sets UI_TEST_HOME.
#
#   ui_test_capture <timeout-secs> <cmd> [args...]
#       Run <cmd> under `timeout -k5 <secs>` with stdout+stderr redirected to a
#       temp FILE and stdin from /dev/null, then read the file into
#       UI_TEST_OUTPUT (exit status in UI_TEST_RC; returns 0). A `$(...)`
#       capture wedges forever when the `--spawn` grandchild inherits the pipe
#       and outlives the CLI (test
#       2026-08-18-test-lua-error-log-spawn-child-wedges-command-substitution);
#       a file has no reader to block. Without `timeout` on PATH it warns and
#       runs unguarded (the redirect alone still closes the wedge).
#   ui_test_timed_out
#       True when the last ui_test_capture hit its timeout (exit 124, or 137
#       after the -k KILL) — callers report it as a FAILED row.
#
# A driver whose run cannot use the throwaway profile carries a one-line
# `# ui-test-home: opt-out — <reason>` comment instead of the isolate call;
# tests/bats/ui_driver_filters.bats holds every driver to one or the other.
#
# This file is sourced; it defines functions and constants, sets no shell
# options, and has no side effects until a function is called.

_UI_TEST_DRIVER_DEV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The first-party sources compiled into Smatchet.exe (repo-root relative), for
# the staleness guard. Root CMakeLists.txt includes Source/Core, the static
# Source/Plugins, and Source/Standalone into the exe (Source/Mobile is the
# Android .so, Source/UnrealPlugins the DX12/Unreal libs); tests/ui/CMakeLists.txt
# adds the bucket-E TUs plus the tests/support files below (the Jira fixture TU
# and the headers it, StandaloneAppBootstrap.cpp and tests/ui include). The rest
# of tests/support is doctest-only. tests/bats/ui_test_driver_lib.bats pins this
# list against those includes.
_UI_TEST_EXE_SOURCES=(Source/Core Source/Plugins Source/Standalone tests/ui
    tests/support/JiraFakeTrackerFixture.cpp tests/support/JiraFakeTrackerFixture.h
    tests/support/FakeNetworkSwitch.h tests/support/FakeTrackerClient.h
    tests/support/ScriptedTrackerBackendFactory.h)

# Seed for a configured profile (see ui_test_isolate_home --seed).
UI_TEST_SEED_CONFIG='{"read_only_mode": false, "whisper_setup_completed": true, "backend_has_been_reachable": true}'

_ui_test_tag() { basename "$0" .sh; }

ui_test_require_fresh_exe() {
    local exe="$1" tag exe_dir exe_abs out rc preset src
    local srcs=()
    tag="$(_ui_test_tag)"
    case "${CI:-}" in
        "" | false | 0) ;;
        *)
            echo "[$tag] exe staleness check skipped (CI=${CI}: the exe's mtime is not comparable to a fresh checkout)." >&2
            return 0
            ;;
    esac
    exe_dir="$(cd "$(dirname "$exe")" 2>/dev/null && pwd)" || return 0
    exe_abs="$exe_dir/$(basename "$exe")"
    preset="$(basename "$exe_dir")"
    for src in "${_UI_TEST_EXE_SOURCES[@]}"; do srcs+=(--src "$src"); done
    out="$(bash "$_UI_TEST_DRIVER_DEV_DIR/is-exe-fresh.sh" --exe "$exe_abs" --preset "$preset" \
        "${srcs[@]}" 2>&1)" && rc=0 || rc=$?
    [ "$rc" -eq 3 ] || return 0
    printf '%s\n' "$out" >&2
    if [ "${SMATCHET_ALLOW_STALE_EXE:-0}" = "1" ]; then
        echo "[$tag] WARN: running the STALE exe anyway (SMATCHET_ALLOW_STALE_EXE=1) — its verdict may not cover your change." >&2
        return 0
    fi
    echo "FAIL: STALE exe $exe — rebuild it, or set SMATCHET_ALLOW_STALE_EXE=1 to run it anyway." >&2
    return 2
}

_ui_test_cleanup_home() { rm -rf "${UI_TEST_HOME:?}"; }

# shellcheck disable=SC2120 # every option is optional; a bare call is the common case
ui_test_isolate_home() {
    local seed=0 shadow=0 tag
    tag="$(_ui_test_tag)"
    while [ $# -gt 0 ]; do
        case "$1" in
            --seed) seed=1 ;;
            --shadow-platform-dirs) shadow=1 ;;
            *)
                echo "ui_test_isolate_home: unknown option: $1" >&2
                return 2
                ;;
        esac
        shift
    done
    if [ -n "${SMATCHET_UI_TEST_HOME:-}" ]; then
        UI_TEST_HOME="$SMATCHET_UI_TEST_HOME"
        mkdir -p "$UI_TEST_HOME"
        echo "[$tag] user-data dir pinned to $UI_TEST_HOME (kept on exit)"
        echo "[$tag] profile state (imgui.ini, config) persists across pinned runs — a red here may be"
        echo "[$tag] stale state; re-run unpinned to confirm before believing it."
    else
        UI_TEST_HOME="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/smatchet-ui-test-$$")"
        mkdir -p "$UI_TEST_HOME"
        trap _ui_test_cleanup_home EXIT
    fi
    if [ "$seed" -eq 1 ]; then
        printf '%s\n' "$UI_TEST_SEED_CONFIG" > "$UI_TEST_HOME/smatchet_config.json"
    fi
    export UI_TEST_HOME
    export SMATCHET_USER_DATA="$UI_TEST_HOME" SMATCHET_UPDATE_CHECK=0
    if [ "$shadow" -eq 1 ]; then
        export LOCALAPPDATA="$UI_TEST_HOME" APPDATA="$UI_TEST_HOME" XDG_CONFIG_HOME="$UI_TEST_HOME"
    fi
}

# shellcheck disable=SC2034 # UI_TEST_OUTPUT is this function's result, read by the sourcing driver
ui_test_capture() {
    local secs="$1" out_file
    shift
    UI_TEST_OUTPUT=""
    UI_TEST_RC=0
    out_file="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/smatchet-ui-test-capture-$$")"
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 5 "$secs" "$@" >"$out_file" 2>&1 </dev/null || UI_TEST_RC=$?
    else
        echo "[$(_ui_test_tag)] WARN: 'timeout' not found — running without a wall-clock guard." >&2
        "$@" >"$out_file" 2>&1 </dev/null || UI_TEST_RC=$?
    fi
    UI_TEST_OUTPUT="$(cat "$out_file" 2>/dev/null || true)"
    # An orphaned grandchild may still hold the file open (Windows refuses the
    # delete then); a leftover temp file is harmless, a failed driver is not.
    rm -f "$out_file" 2>/dev/null || true
    return 0
}

ui_test_timed_out() { [ "${UI_TEST_RC:-0}" -eq 124 ] || [ "${UI_TEST_RC:-0}" -eq 137 ]; }
