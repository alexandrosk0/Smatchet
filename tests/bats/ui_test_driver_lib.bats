#!/usr/bin/env bats
# tests/bats/ui_test_driver_lib.bats
# ----------------------------------------------------------------------------
# Bats coverage for scripts/dev/lib/ui-test-driver.sh — the shared bucket-E
# driver preamble — and for the drivers that use it. The real exe is a Windows
# binary that cannot run here, so every case drives the helper (or a real
# driver) against a stub exe: one that dumps the environment it was handed, one
# that backgrounds a long `sleep` holding its stdout (the `--spawn` grandchild
# that outlives the CLI), or one that simply hangs.
#
# Covers: ui_test_isolate_home (throwaway SMATCHET_USER_DATA + update check off,
# seed, platform-dir shadowing, SMATCHET_UI_TEST_HOME pin, trap cleanup);
# ui_test_require_fresh_exe (fresh / STALE exit 2 naming the newest offender
# across the exe-linked trees only — not Source/Mobile or Source/UnrealPlugins —
# with a drift pin on the exe-linked tests/support files /
# SMATCHET_ALLOW_STALE_EXE / CI skip / relative exe path); ui_test_capture
# (wedge-proof against an inherited pipe, timeout -> 124, TERM-ignoring child
# -> 137); the timeout-as-FAILED-row contract in a
# migrated driver and scripts/dev/test-lua-error-log.sh; and an end-to-end run
# of every driver against a passing stub.
#
# selftest: asserts-failure — the stale, timeout and wedge cases assert the
# failure paths (exit 2 / FAILED row / bounded wall-clock), not just a pass.
#
# Requires: bash, coreutils `timeout`, bats; python for the driver-level cases.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    LIB="$REPO_ROOT/scripts/dev/lib/ui-test-driver.sh"
    export LIB
    FIX="$(mktemp -d)"
    export FIX
    ORPHANS="$FIX/orphans"
    export ORPHANS
    : > "$ORPHANS"
    unset SMATCHET_UI_TEST_HOME SMATCHET_ALLOW_STALE_EXE SMATCHET_USER_DATA SMATCHET_UPDATE_CHECK
    command -v timeout >/dev/null 2>&1 || skip "coreutils timeout not on PATH"
}

teardown() {
    # Reap any `sleep` a wedge stub left behind holding its capture file.
    local pid
    if [ -f "${ORPHANS:-}" ]; then
        while read -r pid; do
            [ -n "$pid" ] && kill "$pid" 2>/dev/null
        done < "$ORPHANS"
    fi
    [ -n "${FIX:-}" ] && rm -rf "$FIX"
    return 0
}

_py() {
    local c
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then echo "$c"; return 0; fi
    done
    return 1
}

# _mk_env_stub <path> — a fake exe that records the env vars the helper sets
# into $STUB_ENV_OUT.<pid> and prints a passing ui_test.run envelope.
_mk_env_stub() {
    cat > "$1" <<'EOF'
#!/usr/bin/env bash
env | grep -E '^(SMATCHET_USER_DATA|SMATCHET_UPDATE_CHECK|LOCALAPPDATA|APPDATA|XDG_CONFIG_HOME)=' \
    | sort > "${STUB_ENV_OUT:-/dev/null}.$$"
printf '%s\n' '{"ok":true,"command":"ui_test.run","data":{"passed":3,"failed":0,"tested":3,"log":"ok"}}'
exit 0
EOF
    chmod +x "$1"
}

# _mk_wedge_stub <path> <secs> [json] — a fake exe that prints [json], then
# backgrounds `sleep <secs>` holding its stdout/stderr (the --spawn grandchild
# that outlives the CLI) and exits 0 at once. fd 3 (bats' TAP channel) is closed
# for the sleeper so it cannot hold bats itself open.
_mk_wedge_stub() {
    local json="${3:-{\"ok\":true,\"command\":\"ui_test.run\",\"data\":{\"passed\":1,\"failed\":0,\"log\":\"ok\"\}\}}"
    cat > "$1" <<EOF
#!/usr/bin/env bash
printf '%s\n' '$json'
sleep $2 3>&- &
echo "\$!" >> "$ORPHANS"
exit 0
EOF
    chmod +x "$1"
}

# _mk_hang_stub <path> — a fake exe that never returns on its own.
_mk_hang_stub() {
    printf '#!/usr/bin/env bash\nexec sleep 300\n' > "$1"
    chmod +x "$1"
}

# _run_bounded <secs> <outfile> <cmd...> — run <cmd> with output to a FILE (never a
# pipe a leaked grandchild could hold) under a hard cap; sets RC and ELAPSED.
_run_bounded() {
    local cap="$1" out="$2"
    shift 2
    local start=$SECONDS
    RC=0
    timeout "$cap" "$@" > "$out" 2>&1 < /dev/null 3>&- || RC=$?
    ELAPSED=$((SECONDS - start))
}

# ---------------------------------------------------------------------------
# ui_test_isolate_home
# ---------------------------------------------------------------------------

@test "isolate_home exports a throwaway SMATCHET_USER_DATA with the update check off, removed on exit" {
    _mk_env_stub "$FIX/stub"
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_isolate_home
STUB_ENV_OUT="$FIX/env" "$FIX/stub" >/dev/null
echo "HOME_DIR=$UI_TEST_HOME"
[ -d "$UI_TEST_HOME" ] && echo "EXISTS_DURING_RUN"
[ -f "$UI_TEST_HOME/smatchet_config.json" ] && echo SEEDED || echo UNSEEDED
EOF
    run bash "$FIX/drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    local home
    home="$(sed -n 's/^HOME_DIR=//p' <<<"$output")"
    [ -n "$home" ]
    grep -q EXISTS_DURING_RUN <<<"$output"
    grep -q UNSEEDED <<<"$output"
    [ ! -e "$home" ]
    grep -qx "SMATCHET_USER_DATA=$home" "$FIX"/env.*
    grep -qx "SMATCHET_UPDATE_CHECK=0" "$FIX"/env.*
    ! grep -q "LOCALAPPDATA=$home" "$FIX"/env.* || return 1
}

@test "isolate_home --seed writes the configured-profile seed" {
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_isolate_home --seed
cat "$UI_TEST_HOME/smatchet_config.json"
EOF
    run bash "$FIX/drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$output" = '{"read_only_mode": false, "whisper_setup_completed": true, "backend_has_been_reachable": true}' ]
}

@test "isolate_home --shadow-platform-dirs points LOCALAPPDATA / APPDATA / XDG_CONFIG_HOME at the dir" {
    _mk_env_stub "$FIX/stub"
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_isolate_home --shadow-platform-dirs
STUB_ENV_OUT="$FIX/env" "$FIX/stub" >/dev/null
echo "HOME_DIR=$UI_TEST_HOME"
EOF
    run bash "$FIX/drv.sh"
    [ "$status" -eq 0 ]
    local home
    home="$(sed -n 's/^HOME_DIR=//p' <<<"$output")"
    grep -qx "LOCALAPPDATA=$home" "$FIX"/env.*
    grep -qx "APPDATA=$home" "$FIX"/env.*
    grep -qx "XDG_CONFIG_HOME=$home" "$FIX"/env.*
    grep -qx "SMATCHET_USER_DATA=$home" "$FIX"/env.*
}

@test "SMATCHET_UI_TEST_HOME pins the dir: created, used, and kept on exit" {
    _mk_env_stub "$FIX/stub"
    local pin="$FIX/pinned/profile"
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_isolate_home --seed
STUB_ENV_OUT="$FIX/env" "$FIX/stub" >/dev/null
EOF
    SMATCHET_UI_TEST_HOME="$pin" run bash "$FIX/drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q "pinned to $pin (kept on exit)" <<<"$output"
    [ -f "$pin/smatchet_config.json" ]
    grep -qx "SMATCHET_USER_DATA=$pin" "$FIX"/env.*
}

@test "the throwaway dir is removed even when the driver fails" {
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_isolate_home
echo "$UI_TEST_HOME" > "$FIX/home"
exit 1
EOF
    run bash "$FIX/drv.sh"
    [ "$status" -eq 1 ]
    [ -s "$FIX/home" ]
    [ ! -e "$(cat "$FIX/home")" ]
}

@test "isolate_home refuses an unknown option" {
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_isolate_home --sed
EOF
    run bash "$FIX/drv.sh"
    [ "$status" -eq 2 ]
    grep -q "unknown option: --sed" <<<"$output"
}

# ---------------------------------------------------------------------------
# ui_test_require_fresh_exe — against a throwaway repo layout, since
# is-exe-fresh.sh resolves Source/ + tests/ui/ from its own repo root.
# ---------------------------------------------------------------------------

_mk_fake_repo() {
    R="$FIX/repo"
    mkdir -p "$R/scripts/dev/lib" "$R/Source/Core" "$R/tests/ui" "$R/build/ninja-ui-test-msvc"
    cp "$REPO_ROOT/scripts/dev/is-exe-fresh.sh" "$R/scripts/dev/"
    cp "$LIB" "$R/scripts/dev/lib/"
    : > "$R/Source/Core/a.cpp"
    : > "$R/tests/ui/b.test.cpp"
    touch -t 200101010000 "$R/Source/Core/a.cpp" "$R/tests/ui/b.test.cpp"
    EXE_PATH="$R/build/ninja-ui-test-msvc/Smatchet.exe"
    : > "$EXE_PATH"
    touch -t 200201010000 "$EXE_PATH"
    cat > "$FIX/stale-drv.sh" <<'EOF'
. "$R/scripts/dev/lib/ui-test-driver.sh"
ui_test_require_fresh_exe "$EXE" || exit 2
echo "GUARD_PASSED"
EOF
    export R
}

@test "a fresh exe passes the staleness guard" {
    _mk_fake_repo
    CI="" EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q GUARD_PASSED <<<"$output"
    [[ "$output" != *STALE* ]]
}

@test "a source newer than the exe fails exit 2, naming the newest offender across Source/ + tests/ui/" {
    _mk_fake_repo
    touch -t 200301010000 "$R/Source/Core/a.cpp"
    touch -t 200501010000 "$R/tests/ui/b.test.cpp"
    CI="" EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 2 ]
    grep -q "STALE" <<<"$output"
    grep -q "Newest offending source: tests/ui/b.test.cpp" <<<"$output"
    grep -q "SMATCHET_ALLOW_STALE_EXE=1" <<<"$output"
    [[ "$output" != *GUARD_PASSED* ]]
}

@test "SMATCHET_ALLOW_STALE_EXE=1 downgrades a stale exe to a warning" {
    _mk_fake_repo
    touch -t 200301010000 "$R/Source/Core/a.cpp"
    CI="" SMATCHET_ALLOW_STALE_EXE=1 EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q "STALE" <<<"$output"
    grep -q "WARN: running the STALE exe anyway" <<<"$output"
    grep -q GUARD_PASSED <<<"$output"
}

@test "the staleness guard is skipped when CI is set" {
    _mk_fake_repo
    touch -t 200301010000 "$R/Source/Core/a.cpp"
    CI=true EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q "staleness check skipped (CI=true" <<<"$output"
    grep -q GUARD_PASSED <<<"$output"
}

@test "an edit under Source/Mobile or Source/UnrealPlugins (not linked into the exe) is not stale" {
    _mk_fake_repo
    mkdir -p "$R/Source/Mobile" "$R/Source/UnrealPlugins/P"
    : > "$R/Source/Mobile/m.cpp"
    : > "$R/Source/UnrealPlugins/P/u.h"
    touch -t 200501010000 "$R/Source/Mobile/m.cpp" "$R/Source/UnrealPlugins/P/u.h"
    CI="" EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q GUARD_PASSED <<<"$output"
    [[ "$output" != *STALE* ]]
}

@test "a newer exe-linked tests/support file is stale; a doctest-only one is not" {
    _mk_fake_repo
    mkdir -p "$R/tests/support" "$R/Source/Plugins/Mcp"
    : > "$R/tests/support/DoctestOnlyHelper.h"
    touch -t 200501010000 "$R/tests/support/DoctestOnlyHelper.h"
    CI="" EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 0 ]
    : > "$R/tests/support/JiraFakeTrackerFixture.cpp"
    touch -t 200401010000 "$R/tests/support/JiraFakeTrackerFixture.cpp"
    CI="" EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 2 ]
    grep -q "Newest offending source: tests/support/JiraFakeTrackerFixture.cpp" <<<"$output"
    : > "$R/Source/Plugins/Mcp/p.cpp"
    touch -t 200601010000 "$R/Source/Plugins/Mcp/p.cpp"
    CI="" EXE="$EXE_PATH" run bash "$FIX/stale-drv.sh"
    [ "$status" -eq 2 ]
    grep -q "Newest offending source: Source/Plugins/Mcp/p.cpp" <<<"$output"
}

# Drift pin: every tests/support file the exe compiles — a TU tests/ui/CMakeLists.txt
# adds, or a header (transitively) included from Source/{Core,Plugins,Standalone} or
# tests/ui — must be in the staleness guard's list, or an edit to it leaves a stale exe
# "fresh".
@test "the staleness guard lists every tests/support file compiled into the exe" {
    # shellcheck source=/dev/null
    . "$LIB"
    local queue=() seen=" " f h i=0 missing=""
    while read -r f; do queue+=("$f"); done < <(
        grep -oE 'tests/support/[A-Za-z0-9_]+\.cpp' "$REPO_ROOT/tests/ui/CMakeLists.txt" | sort -u)
    while read -r h; do
        [ -f "$REPO_ROOT/tests/support/$h" ] && queue+=("tests/support/$h")
    done < <(grep -rhoE '#include "[A-Za-z0-9_]+\.h"' "$REPO_ROOT/Source/Core" "$REPO_ROOT/Source/Plugins" \
        "$REPO_ROOT/Source/Standalone" "$REPO_ROOT/tests/ui" | sed -E 's/#include "(.*)"/\1/' | sort -u)
    [ "${#queue[@]}" -gt 0 ]
    while [ "$i" -lt "${#queue[@]}" ]; do
        f="${queue[$i]}"
        i=$((i + 1))
        [[ "$seen" == *" $f "* ]] && continue
        seen="$seen$f "
        while read -r h; do
            [ -f "$REPO_ROOT/tests/support/$h" ] && queue+=("tests/support/$h")
        done < <(grep -oE '#include "[A-Za-z0-9_]+\.h"' "$REPO_ROOT/$f" | sed -E 's/#include "(.*)"/\1/')
    done
    for f in $seen; do
        [[ " ${_UI_TEST_EXE_SOURCES[*]} " == *" $f "* ]] || missing="$missing $f"
    done
    echo "exe-linked tests/support files:$seen"
    echo "missing from _UI_TEST_EXE_SOURCES:$missing"
    [ -z "$missing" ]
}

@test "a relative exe path resolves from the driver's cwd, not the repo root" {
    _mk_fake_repo
    touch -t 200301010000 "$R/Source/Core/a.cpp"
    cd "$R/build"
    CI="" EXE="ninja-ui-test-msvc/Smatchet.exe" run bash "$FIX/stale-drv.sh"
    echo "$output"
    [ "$status" -eq 2 ]
    grep -q "Newest offending source: Source/Core/a.cpp" <<<"$output"
}

# ---------------------------------------------------------------------------
# ui_test_capture
# ---------------------------------------------------------------------------

@test "capture records stdout + stderr and the exit status, with stdin at EOF" {
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_capture 30 bash -c 'echo to-out; echo to-err >&2; if read -r x; then echo "read:$x"; fi; exit 3'
echo "RC=$UI_TEST_RC"
printf '%s\n' "$UI_TEST_OUTPUT"
ui_test_timed_out && echo TIMED_OUT || echo NOT_TIMED_OUT
EOF
    printf 'leaked-stdin\n' > "$FIX/stdin"
    bash "$FIX/drv.sh" < "$FIX/stdin" > "$FIX/out" 2>&1
    cat "$FIX/out"
    grep -qx "RC=3" "$FIX/out"
    grep -qx "to-out" "$FIX/out"
    grep -qx "to-err" "$FIX/out"
    ! grep -q "read:" "$FIX/out" || return 1
    grep -qx NOT_TIMED_OUT "$FIX/out"
}

@test "control: a grandchild holding stdout wedges a plain \$(...) capture until it exits" {
    _mk_wedge_stub "$FIX/wedge" 4
    local start=$SECONDS out
    out="$("$FIX/wedge")"
    [ $((SECONDS - start)) -ge 3 ]
    grep -q '"passed":1' <<<"$out"
}

@test "capture returns at once when a --spawn-style grandchild outlives the command" {
    _mk_wedge_stub "$FIX/wedge" 300
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_capture 60 "$FIX/wedge"
echo "RC=$UI_TEST_RC"
printf '%s\n' "$UI_TEST_OUTPUT"
EOF
    _run_bounded 30 "$FIX/out" bash "$FIX/drv.sh"
    cat "$FIX/out"
    [ "$RC" -eq 0 ]
    [ "$ELAPSED" -lt 15 ]
    grep -qx "RC=0" "$FIX/out"
    grep -q '"passed":1' "$FIX/out"
}

@test "a hung command times out (exit 124) and ui_test_timed_out reports it" {
    _mk_hang_stub "$FIX/hang"
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_capture 1 "$FIX/hang"
echo "RC=$UI_TEST_RC"
ui_test_timed_out && echo TIMED_OUT
EOF
    _run_bounded 30 "$FIX/out" bash "$FIX/drv.sh"
    cat "$FIX/out"
    [ "$ELAPSED" -lt 15 ]
    grep -qx "RC=124" "$FIX/out"
    grep -qx TIMED_OUT "$FIX/out"
}

@test "a command ignoring TERM is KILLed after the grace period (exit 137) and still counts as timed out" {
    cat > "$FIX/drv.sh" <<'EOF'
. "$LIB"
ui_test_capture 1 bash -c 'trap "" TERM; sleep 300'
echo "RC=$UI_TEST_RC"
ui_test_timed_out && echo TIMED_OUT
EOF
    _run_bounded 30 "$FIX/out" bash "$FIX/drv.sh"
    cat "$FIX/out"
    [ "$ELAPSED" -lt 20 ]
    grep -qx "RC=137" "$FIX/out"
    grep -qx TIMED_OUT "$FIX/out"
}

# ---------------------------------------------------------------------------
# Drivers on the helper
# ---------------------------------------------------------------------------

@test "a timeout-wrapped driver survives a --spawn grandchild that outlives the CLI" {
    PYTHON="$(_py)" || skip "no working python"
    export PYTHON
    _mk_wedge_stub "$FIX/wedge" 300
    cd "$REPO_ROOT"
    SMATCHET_EXE="$FIX/wedge" SMATCHET_TEST_PORT=0 \
        _run_bounded 30 "$FIX/out" bash scripts/dev/test-ui-ai-assistant-model-change.sh
    cat "$FIX/out"
    [ "$RC" -eq 0 ]
    [ "$ELAPSED" -lt 15 ]
    grep -q "Passed: 1  Failed: 0" "$FIX/out"
}

@test "a timeout-wrapped driver reports a hung run as a FAILED row" {
    PYTHON="$(_py)" || skip "no working python"
    export PYTHON
    _mk_hang_stub "$FIX/hang"
    cd "$REPO_ROOT"
    SMATCHET_EXE="$FIX/hang" SMATCHET_TEST_PORT=0 UI_TEST_TIMEOUT=1 \
        _run_bounded 30 "$FIX/out" bash scripts/dev/test-ui-callstack-tooltip-hover.sh
    cat "$FIX/out"
    [ "$RC" -eq 1 ]
    [ "$ELAPSED" -lt 15 ]
    grep -q "exceeded 1s hard timeout" "$FIX/out"
    grep -q "Passed: 0  Failed: 1" "$FIX/out"
}

@test "test-lua-error-log.sh records each hung snippet run as a FAILED row instead of wedging" {
    PYTHON="$(_py)" || skip "no working python"
    export PYTHON
    _mk_hang_stub "$FIX/hang"
    cd "$REPO_ROOT"
    SMATCHET_EXE="$FIX/hang" SMATCHET_TEST_PORT=0 SMATCHET_RUN_TIMEOUT_SECS=1 \
        _run_bounded 40 "$FIX/out" bash scripts/dev/test-lua-error-log.sh
    cat "$FIX/out"
    [ "$RC" -eq 1 ]
    [ "$ELAPSED" -lt 30 ]
    [ "$(grep -c "timed out after 1s" "$FIX/out")" -eq 4 ]
    grep -q "Passed: 0  Failed: 4" "$FIX/out"
}

@test "test-lua-error-log.sh returns promptly when the --spawn child outlives the CLI" {
    PYTHON="$(_py)" || skip "no working python"
    export PYTHON
    _mk_wedge_stub "$FIX/wedge" 300 \
        '{"ok":true,"data":{"ok_snippet":true,"log_lines":["hello-from-test"],"window_requested":false,"err_lines":[]}}'
    cd "$REPO_ROOT"
    SMATCHET_EXE="$FIX/wedge" SMATCHET_TEST_PORT=0 \
        _run_bounded 40 "$FIX/out" bash scripts/dev/test-lua-error-log.sh
    cat "$FIX/out"
    [ "$ELAPSED" -lt 20 ]
    # Test 1's four assertions match the stub's envelope; the rest legitimately fail.
    grep -qE "Passed: 4  Failed: [1-9]" "$FIX/out"
}

@test "every bucket-E driver runs green against a passing stub and leaves no throwaway profile behind" {
    PYTHON="$(_py)" || skip "no working python"
    export PYTHON
    _mk_env_stub "$FIX/stub"
    cd "$REPO_ROOT"
    local d name n=0 bad=0 home
    for d in scripts/dev/test-ui-*.sh; do
        case "$d" in *-bats.sh) continue ;; esac
        grep -q 'lib/ui-test-driver.sh\|ui-test-home: opt-out' "$d" || continue
        name="$(basename "$d")"
        rm -f "$FIX"/env.*
        n=$((n + 1))
        if ! CI="" STUB_ENV_OUT="$FIX/env" SMATCHET_EXE="$FIX/stub" SMATCHET_TEST_PORT=0 \
            bash "$d" > "$FIX/out.$name" 2>&1 < /dev/null; then
            echo "FAIL: $name exited non-zero against a passing stub"; tail -5 "$FIX/out.$name"; bad=1; continue
        fi
        if grep -qE '^[[:space:]]*ui_test_isolate_home' "$d" || grep -q 'exec bash' "$d"; then
            home="$(sed -n 's/^SMATCHET_USER_DATA=//p' "$FIX"/env.* 2>/dev/null | head -n 1)"
            [ -n "$home" ] || { echo "FAIL: $name ran the exe without SMATCHET_USER_DATA"; bad=1; continue; }
            grep -qx "SMATCHET_UPDATE_CHECK=0" "$FIX"/env.* || { echo "FAIL: $name left the update check on"; bad=1; }
            [ ! -e "$home" ] || { echo "FAIL: $name left $home behind"; bad=1; }
        fi
    done
    echo "ran $n driver(s)"
    [ "$n" -ge 10 ]
    [ "$bad" -eq 0 ]
}
