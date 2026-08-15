#!/usr/bin/env bash
# test-screenshot-diff.sh — Phase 7 bucket-C screenshot-diff driver.
#
# For each registered Phase-7 scenario:
#   1. Launch an ephemeral `--spawn` Smatchet.
#   2. `cmd scenario.run --name=<scen> --screenshotPath=<tmp>.png --spawn`
#      drives the UI to a known state + triggers debug.window.screenshot.
#   3. Poll for the PNG to flush (deterministic; no fixed sleep).
#   4. Compare the captured PNG against tests/golden/<scen>.png using the
#      g++-compiled screenshot_diff helper (per-channel L∞ tolerance = 4).
#
# Bootstrap mode (--bootstrap or first run when golden is missing): copies the
# captured PNG into tests/golden/ so the next run has something to diff against.
# Bootstrap runs always PASS — they're a one-time capture, not a regression
# gate. Commit the new goldens by hand to make them authoritative.
#
# Env overrides:
#   SMATCHET_EXE            path to Smatchet.exe (default build/ninja-iter-msvc/...)
#   SMATCHET_TEST_PORT      MCP port for --spawn (default: ephemeral 40000-59999, so parallel runs don't collide; set explicitly to pin a fixed port)
#   PYTHON                  python interpreter (default `python`)
#   SCREENSHOT_TOLERANCE    per-channel L∞ tolerance (default 4)
#   SCREENSHOT_DIFF_BIN     pre-built diff helper path; if unset the script g++-compiles one
#   SMATCHET_GOLDEN_DIR     golden PNG dir (default tests/golden); override to isolate a test harness
#   SMATCHET_LANE_STATUS_FILE  if set, write `status=<ok|broken|fail> passed=<n> failed=<n>` here
#   SMATCHET_GOLDEN_REPORT_FILE  if set, write one TSV verdict row per scenario here (see below)
#
# Exit codes:
#   0 — every scenario captured within tolerance (or bootstrap completed)
#   1 — at least one diff exceeded tolerance / dimension mismatch / spawn failure
#   2 — binary or build is missing
#   3 — BROKEN LANE: Passed==0 && Failed>0 (every scenario failed — the whole
#       harness is dead, e.g. the exe can't boot, NOT a per-scenario regression).
#       Distinct from 1 so the CI lane-integrity step can tell "broken" from
#       "some scenarios regressed" even though this job is continue-on-error.
#       See infra.md 2026-06-07 P1 bucket-lane-launch-smoke + postmortems.md
#       bucket-C/E green-wash (a dead harness ran Passed:0 Failed:3 GREEN for 2
#       weeks because continue-on-error swallowed total harness death like a
#       flaky test).
#
# Lane-status sentinel:
#   When SMATCHET_LANE_STATUS_FILE is set, the final Passed/Failed verdict is
#   written there as `status=<ok|broken|fail> passed=<n> failed=<n>` so a
#   SEPARATE non-advisory CI step can assert lane integrity OUTSIDE this
#   continue-on-error job (the only way a broken-lane signal escapes the mask).
#
# Per-scenario verdict report (masked-step reporting):
#   The CI step that runs this driver is continue-on-error BY DESIGN (llvmpipe
#   captures are not authoritative against per-developer GPU goldens), but a
#   TOTAL mask discards the pass/fail signal rather than downgrading it: a golden
#   that is stale for a perfectly deterministic reason — a deliberate UI change
#   nobody regenerated for — reads exactly like a clean run, so goldens rot
#   silently (tooling 2026-08-06 bucket-c-golden-mask-hides-stale-goldens; seven
#   stale goldens, none of which ever produced a signal). When
#   SMATCHET_GOLDEN_REPORT_FILE is set this driver writes one TAB-separated row
#   per scenario REGARDLESS of verdict, so the reporting CI step can surface it
#   without gaining the power to block the lane:
#     <scenario>\t<verdict>\t<linf>\t<tolerance>\t<golden-mtime-iso>\t<golden-age-days>
#   verdict ∈ pass | fail | bootstrap | missing-capture | spawn-failed.
#   linf / mtime / age are `-` when the run never reached a diff.
#
# Usage:
#   bash scripts/dev/test-screenshot-diff.sh                # diff mode (gate)
#   bash scripts/dev/test-screenshot-diff.sh --bootstrap    # write goldens from clean run

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
# Ephemeral port keeps parallel runs from colliding on the default.
TEST_PORT="${SMATCHET_TEST_PORT:-$((40000 + RANDOM % 20000))}"
TOL="${SCREENSHOT_TOLERANCE:-4}"
# Golden dir is overridable so a test harness can isolate goldens to a scratch
# dir (the broken-lane bats must not clobber the committed tests/golden/ PNGs).
# Unset in CI/dev — production behaviour (gate against committed goldens) holds.
GOLDEN_DIR="${SMATCHET_GOLDEN_DIR:-tests/golden}"
TMP_DIR="${TMPDIR:-/tmp}/smatchet-screenshot-diff-$$"
mkdir -p "$TMP_DIR"
# CI bucket-C job needs the captures to survive past script exit so upload-artifact
# can find them — set SMATCHET_KEEP_SCREENSHOT_CAPTURES=1 in that job. Default
# behaviour (developer / local) cleans up.
if [ "${SMATCHET_KEEP_SCREENSHOT_CAPTURES:-0}" != "1" ]; then
    trap 'rm -rf "$TMP_DIR"' EXIT
fi

BOOTSTRAP=0
if [ "${1:-}" = "--bootstrap" ]; then
    BOOTSTRAP=1
    echo "[test-screenshot-diff] BOOTSTRAP mode — writing goldens from captured PNGs"
fi

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-iter-msvc --target SmatchetStandalone" >&2
    exit 2
fi

mkdir -p "$GOLDEN_DIR"

# Build the diff helper once (TU-local; no CMake side-effects).
DIFF_BIN="${SCREENSHOT_DIFF_BIN:-$TMP_DIR/screenshot_diff}"
if [ -z "${SCREENSHOT_DIFF_BIN:-}" ]; then
    # g++ is mandatory on the MSYS2/UCRT64 dev env — same toolchain as ninja-iter-msvc.
    GXX="${CXX:-g++}"
    if ! command -v "$GXX" >/dev/null 2>&1; then
        echo "FAIL: $GXX not found. Set SCREENSHOT_DIFF_BIN to a prebuilt helper or install g++." >&2
        exit 2
    fi
    echo "[test-screenshot-diff] compiling diff helper via $GXX..."
    "$GXX" -std=c++14 -O2 -Wall -Wextra -Wpedantic \
        -Itests/support -ISource/Core/ThirdParty \
        -o "$DIFF_BIN" \
        tests/support/ScreenshotDiffMain.cpp
fi

# pink-clear dock-gap scan (pink-clear-dock-gap-scan): the dock-gap-sentinel
# scenario arms a magenta clear-color during warm-up so any uncovered viewport
# pixel renders pink. This helper counts sentinel pixels; the gate asserts zero.
PINK_BIN="${PINK_PIXEL_COUNT_BIN:-$TMP_DIR/pink_pixel_count}"
if [ -z "${PINK_PIXEL_COUNT_BIN:-}" ]; then
    GXX="${CXX:-g++}"
    if ! command -v "$GXX" >/dev/null 2>&1; then
        echo "FAIL: $GXX not found. Set PINK_PIXEL_COUNT_BIN to a prebuilt helper or install g++." >&2
        exit 2
    fi
    echo "[test-screenshot-diff] compiling pink-pixel-count helper via $GXX..."
    "$GXX" -std=c++14 -O2 -Wall -Wextra -Wpedantic \
        -Itests/support -ISource/Core/ThirdParty \
        -o "$PINK_BIN" \
        tests/support/PinkPixelCountMain.cpp
fi
# Sentinel RGB + tolerance + max-allowed for the pink-clear dock-gap assertion.
PINK_RGB_R=255
PINK_RGB_G=0
PINK_RGB_B=255
PINK_TOL="${PINK_TOLERANCE:-8}"
PINK_MAX_ALLOWED="${PINK_MAX_ALLOWED:-0}"

PASSED=0
FAILED=0
# Truncate the verdict report up front: report_row APPENDS, so a re-run against a
# pre-existing file (a repeated local run, a reused CI workspace) would otherwise
# stack this run's rows on top of the last one's and the summary would render a
# mix of both — stale verdicts reported as current is the exact failure this
# report exists to prevent.
if [ -n "${SMATCHET_GOLDEN_REPORT_FILE:-}" ]; then
    : > "$SMATCHET_GOLDEN_REPORT_FILE"
fi
SCENARIOS=(
    "dock-gap-sentinel"
    "command-palette-fuzzy"
    "code-syntax-coloring"
    "user-info-desktop-unified"
    "user-info-desktop-separate"
    "user-info-narrow-unified"
    "user-info-narrow-separate"
)
# NOTE: theme-switch-roundtrip is NOT in this list. Its assertion shape is
# different — it does not gate against a committed golden PNG. Instead the
# fresh-launch capture serves as the per-run reference and the round-trip
# capture must be bytewise-identical to it. See scripts/dev/test-theme-roundtrip.sh
# for that test driver.

# Extract a JSON field from the CLI's text-wrapped output.
extract() {
    local field="$1"
    "$PY" -c "import sys,json,re; t=sys.stdin.read(); m=re.search(r'\{.*\}',t,re.S); d=json.loads(m.group(0)); print(json.dumps(d.get('data',{}).get('$field','MISSING')))"
}

assert() {
    local label="$1" condition="$2"
    if [ "$condition" = "ok" ]; then
        echo "  PASS  $label"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL  $label  — $condition"
        FAILED=$((FAILED + 1))
    fi
}

# Append one per-scenario verdict row to the report (no-op when the caller set no
# report file). Called on EVERY scenario outcome — the point of the report is
# that a masked step still reports, so a row missing here is a scenario whose
# verdict the mask would swallow again.
report_row() {
    local scen="$1" verdict="$2" linf="${3:--}" golden="${4:-}"
    [ -n "${SMATCHET_GOLDEN_REPORT_FILE:-}" ] || return 0
    local mtime="-" age="-"
    if [ -n "$golden" ] && [ -f "$golden" ]; then
        # `stat -c %Y` (GNU/Git-Bash) with a BSD `stat -f %m` fallback; both
        # absent (or a python-less box) leaves the age columns as '-' rather
        # than failing the driver over a reporting nicety.
        local epoch=""
        epoch="$(stat -c %Y "$golden" 2>/dev/null || stat -f %m "$golden" 2>/dev/null || true)"
        # Digits-only guard: a non-numeric stat result would abort the whole
        # driver in the arithmetic below (set -e) — a reporting nicety must never
        # take the gate down.
        case "$epoch" in ''|*[!0-9]*) epoch="" ;; esac
        if [ -n "$epoch" ]; then
            mtime="$(date -u -d "@$epoch" '+%Y-%m-%d' 2>/dev/null || date -u -r "$epoch" '+%Y-%m-%d' 2>/dev/null || echo '-')"
            age=$(( ( $(date -u '+%s') - epoch ) / 86400 ))
        fi
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$scen" "$verdict" "$linf" "$TOL" "$mtime" "$age" \
        >> "$SMATCHET_GOLDEN_REPORT_FILE"
}

run_scenario() {
    local scen="$1"
    local captured="$TMP_DIR/$scen.png"
    local golden="$GOLDEN_DIR/$scen.png"

    echo
    echo "=== Scenario: $scen ==="

    # EVERY golden must be config-INDEPENDENT: the real user-data dir may carry
    # cached tracker creds, which load a live backend (populated grid rows,
    # activity/groups async, sync toasts, an online-vs-offline status bar, a
    # relative "-1d" Updated cell that rots with the wall-clock date) whose
    # content varies run to run and machine to machine. Point every scenario at
    # a throwaway EMPTY user-data dir (SMATCHET_USER_DATA, honoured by
    # StandaloneAppBootstrap and inherited by the --spawn child) so every capture
    # starts from the same clean first-launch state.
    # The three ambient scenarios (dock-gap-sentinel, command-palette-fuzzy,
    # code-syntax-coloring) ran with the ambient env until 2026-08-05 and flaked
    # on exactly that tracker-connectivity race (captures landing in the TRACKER
    # OFFLINE state at L_inf 200-240); their goldens were re-bootstrapped under
    # isolation with the flake fix.
    local iso_dir="$TMP_DIR/userdata-$scen"
    mkdir -p "$iso_dir"
    # SMATCHET_UPDATE_CHECK=0 — the startup "newer release on GitHub?" round-trip
    # is async and network-timed, so its completion frame lands at a different
    # point in every run. When it landed mid-capture it painted the whole "Update
    # Available" modal over the frame (whole-frame L_inf ~240, and release-note
    # text that would rot on every release). The scenario quiesce suppresses that
    # modal once it exists; disabling the check outright means there is no
    # completion event to race with in the first place.
    local iso_env=(env "SMATCHET_USER_DATA=$iso_dir" "SMATCHET_UPDATE_CHECK=0")

    # 20 warm-up frames keeps the docking + palette layout settled before the
    # screenshot trigger. Frames argument feeds the spawn-mode timeout calc
    # but also bounds the scenario's internal warm-up via --warmupFrames.
    local out
    out=$("${iso_env[@]}" "$EXE" cmd scenario.run \
        --name="$scen" \
        --frames=20 \
        --warmupFrames=16 \
        --screenshotPath="$captured" \
        --mcp-port="$TEST_PORT" \
        --spawn --yes 2>&1 || true)
    echo "$out" | tail -20

    # Deterministic poll: the JSON outPath fires from OnFinish, while the
    # screenshot fires from the post-swap handler one frame later. Wait up to
    # 2 s for the PNG file to appear and reach a non-zero size — far snappier
    # than a fixed sleep on fast hardware, safe on slow hardware.
    for _ in $(seq 1 40); do
        [ -s "$captured" ] && break
        sleep 0.05
    done

    # Verify the JSON envelope reported captureRequested:true.
    local req
    req=$(echo "$out" | extract captureRequested 2>/dev/null || echo "MISSING")
    if [ "$req" != "true" ]; then
        report_row "$scen" "spawn-failed" "-" "$golden"
        assert "$scen captureRequested" "envelope missing captureRequested=true; got=$req"
        return
    fi

    # Verify the captured PNG exists and is non-empty.
    if [ ! -s "$captured" ]; then
        report_row "$scen" "missing-capture" "-" "$golden"
        assert "$scen captured PNG" "captured file missing or empty at $captured"
        return
    fi
    assert "$scen captureRequested" "ok"

    # pink-clear dock-gap scan (pink-clear-dock-gap-scan): the dock-gap-sentinel
    # scenario armed a magenta clear-color over its warm-up frames so any viewport
    # region the dockspace fails to cover renders pink. Assert ZERO sentinel pixels
    # in the capture — a hard, headless, golden-independent gate (runs even in
    # bootstrap mode, since it doesn't compare against a committed golden). Any
    # pink = a real dock gap leak. Skip for non-sentinel scenarios.
    if [ "$scen" = "dock-gap-sentinel" ]; then
        local pink_out pink_rc
        set +e
        pink_out=$("$PINK_BIN" "$captured" "$PINK_RGB_R" "$PINK_RGB_G" "$PINK_RGB_B" "$PINK_TOL" \
            "$PINK_MAX_ALLOWED" 2>&1)
        pink_rc=$?
        set -e
        echo "  $pink_out"
        if [ "$pink_rc" -eq 0 ]; then
            assert "$scen pink-pixels == 0 (no dock gap)" "ok"
        else
            assert "$scen pink-pixels == 0 (no dock gap)" "$pink_out"
        fi
    fi

    # Bootstrap mode: copy capture → golden. No diff, always PASS.
    if [ "$BOOTSTRAP" -eq 1 ]; then
        cp "$captured" "$golden"
        echo "  BOOTSTRAP  wrote $golden ($(wc -c < "$golden") bytes)"
        report_row "$scen" "bootstrap" "-" "$golden"
        assert "$scen golden bootstrap" "ok"
        return
    fi

    # First-run convenience: if the golden is missing AND we're not in
    # bootstrap mode, write it anyway and warn the user — but DO mark a soft
    # PASS so the gate doesn't fail on a fresh checkout that hasn't run
    # bootstrap yet. The next run will diff against this golden.
    if [ ! -f "$golden" ]; then
        cp "$captured" "$golden"
        echo "  WARN  $golden was missing — bootstrapped from this run."
        echo "        Commit the new golden so future runs gate against it:"
        echo "          git add $golden"
        report_row "$scen" "bootstrap" "-" "$golden"
        assert "$scen golden auto-bootstrapped" "ok"
        return
    fi

    # Diff captured vs golden. Run the helper, capture stdout/stderr + rc
    # explicitly (the rc of a command substitution doesn't propagate).
    local diff_out diff_rc
    set +e
    diff_out=$("$DIFF_BIN" "$captured" "$golden" "$TOL" 2>&1)
    diff_rc=$?
    set -e
    echo "  $diff_out"
    # linf=<N> is the helper's first stdout token; '-' when it never printed one
    # (load failure / dimension mismatch write only stderr).
    local linf
    linf="$(printf '%s' "$diff_out" | sed -n 's/.*linf=\([0-9-]*\).*/\1/p' | head -1)"
    [ -n "$linf" ] || linf="-"
    if [ "$diff_rc" -eq 0 ]; then
        report_row "$scen" "pass" "$linf" "$golden"
        assert "$scen L_inf <= $TOL" "ok"
    else
        report_row "$scen" "fail" "$linf" "$golden"
        assert "$scen L_inf <= $TOL" "$diff_out"
    fi
}

for scen in "${SCENARIOS[@]}"; do
    run_scenario "$scen"
done

echo
echo "============================="
echo "Passed: $PASSED  Failed: $FAILED"
echo "============================="

# Broken-lane guard: a lane that passes NOTHING while failing something is a
# dead harness, not a flaky regression — the bucket-C/E green-wash class. Emit a
# distinct exit code (3) + a GitHub ::error:: annotation + the lane-status
# sentinel so a non-advisory CI step can hard-fail OUTSIDE this
# continue-on-error job. Order matters: check broken BEFORE the ordinary
# Failed>0 path so the broken signal wins.
LANE_STATUS="ok"
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -gt 0 ]; then
    LANE_STATUS="broken"
elif [ "$FAILED" -gt 0 ]; then
    LANE_STATUS="fail"
fi

if [ -n "${SMATCHET_LANE_STATUS_FILE:-}" ]; then
    printf 'status=%s passed=%s failed=%s\n' "$LANE_STATUS" "$PASSED" "$FAILED" \
        > "$SMATCHET_LANE_STATUS_FILE"
fi

if [ "$LANE_STATUS" = "broken" ]; then
    echo "::error::bucket-C lane BROKEN — Passed:0 Failed:$FAILED. Every scenario failed: the harness is dead (exe can't boot / no GL), NOT a per-scenario regression. See the launch-smoke step + infra.md bucket-lane-launch-smoke." >&2
    exit 3
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
