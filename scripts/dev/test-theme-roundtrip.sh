#!/usr/bin/env bash
# test-theme-roundtrip.sh — dual-capture regression gate for the
# SmatchetDark -> NortonCommander -> SmatchetDark theme round-trip.
#
# Architecture choice (vs the bootstrap-golden pattern used by
# test-screenshot-diff.sh): NO committed golden PNG. The fresh-launch capture
# IS the source of truth for "what SmatchetDark must look like" on this run.
# The round-trip capture must be byte-identical to the fresh capture.
#
# Rationale:
#   * A committed `theme-switch-roundtrip.png` golden is fragile — when the
#     SmatchetDark palette intentionally changes (palette retunes, new color
#     slot adopted), the bootstrap pass captures whatever the current build
#     produces and that becomes the golden. If the post-round-trip state is
#     ever wrong at the moment of bootstrap (the original bug shipped in
#     PR #301 was bootstrapped this way!), the golden is literally a picture
#     of the bug. The gate is then useless — it asserts "broken state must
#     stay broken".
#   * Dual-capture sidesteps the bootstrap trap entirely. Each run picks its
#     own reference (fresh-launch SmatchetDark) and asserts the round-trip
#     can return to it bytewise. The palette can change freely; the only
#     invariant is fresh-launch == round-trip-end.
#
# Pass criteria: cmp(capture_fresh, capture_roundtrip) must return 0 (bytewise
# identical PNG files). Even an L_inf=1 deviation here means some state
# survived NortonCommander -> SmatchetDark which shouldn't have.
#
# Env overrides (same shape as test-screenshot-diff.sh):
#   SMATCHET_EXE          path to Smatchet.exe (default build/ninja-iter-msvc/...)
#   SMATCHET_TEST_PORT    base MCP port (we allocate two; default random)
#   PYTHON                python interpreter (default `python`)
#
# Exit codes:
#   0 — fresh-launch and round-trip captures are bytewise identical
#   1 — captures differ (regression) OR spawn failure / capture missing
#   2 — binary or build is missing

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-build/ninja-iter-msys2/Smatchet.exe}"
PY="${PYTHON:-python}"
PORT_FRESH="${SMATCHET_TEST_PORT_FRESH:-$((40000 + RANDOM % 10000))}"
PORT_RT="${SMATCHET_TEST_PORT_RT:-$((50000 + RANDOM % 10000))}"
TMP_DIR="${TMPDIR:-/tmp}/smatchet-theme-roundtrip-$$"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-iter-msvc --target SmatchetStandalone" >&2
    exit 2
fi

extract_field() {
    local field="$1"
    "$PY" -c "import sys,json,re; t=sys.stdin.read(); m=re.search(r'\{.*\}',t,re.S); d=json.loads(m.group(0)); print(json.dumps(d.get('data',{}).get('$field','MISSING')))"
}

# Drive a single capture given a scenario name. Returns 0 if PNG flushed
# non-empty, non-zero otherwise. Prints any spawn diagnostics.
run_capture() {
    local label="$1" scen="$2" port="$3" out_png="$4" extra_args="${5:-}"
    echo "=== $label ($scen) ==="
    # frames=24 + 6-frame warmup is the dock-gap-sentinel's tuned floor; we
    # match it so both captures get the same opportunity to settle their
    # docking + panel state. theme-switch-roundtrip's internal warmupA/B/Return
    # are 6/6/6 by default which fits.
    # shellcheck disable=SC2086
    local out
    out=$("$EXE" cmd scenario.run \
        --name="$scen" \
        --frames=24 \
        --warmupFrames=20 \
        --screenshotPath="$out_png" \
        --mcp-port="$port" \
        --spawn --yes $extra_args 2>&1 || true)
    echo "$out" | tail -10
    # Deterministic poll — same shape as test-screenshot-diff.sh.
    for _ in $(seq 1 40); do
        [ -s "$out_png" ] && break
        sleep 0.05
    done
    local req
    req=$(echo "$out" | extract_field captureRequested 2>/dev/null || echo "MISSING")
    if [ "$req" != "true" ]; then
        echo "  FAIL  $label captureRequested=$req"
        return 1
    fi
    if [ ! -s "$out_png" ]; then
        echo "  FAIL  $label capture file missing or empty at $out_png"
        return 1
    fi
    echo "  OK    $label captured $(wc -c < "$out_png") bytes"
}

FRESH="$TMP_DIR/fresh.png"
RT="$TMP_DIR/roundtrip.png"

# BOTH captures run the SAME scenario (theme-switch-roundtrip) — only the
# `skipSwitch` arg differs. Same warmup budget, same frame count, same
# OnFinish capture path => same transient state (sync banners, fire-and-forget
# toasts, background work) at the moment of capture. Without the
# same-scenario discipline, the bytewise diff is dominated by transient state
# leaking from the round-trip's longer wall-clock window.

# Capture A: skipSwitch=true => SmatchetDark held for the full warmup window.
# This is the fresh-launch baseline.
run_capture "Capture A: skipSwitch=true (baseline)" "theme-switch-roundtrip" \
    "$PORT_FRESH" "$FRESH" "--skipSwitch=true"

# Capture B: skipSwitch=false (default) => SmatchetDark -> NortonCommander
# -> SmatchetDark. The scenario leaves the theme on SmatchetDark before the
# screenshot fires (see ThemeSwitchRoundtripScenario.cpp OnFinish —
# DELIBERATELY does not restore savedTheme_ so the screenshot captures the
# post-round-trip state).
run_capture "Capture B: round-trip (full switch)" "theme-switch-roundtrip" \
    "$PORT_RT" "$RT"

echo
echo "============================="
echo "Diff: fresh-launch vs round-trip"
echo "============================="
echo "  fresh:     $(md5sum "$FRESH" | awk '{print $1}')  $(wc -c < "$FRESH") bytes"
echo "  roundtrip: $(md5sum "$RT"    | awk '{print $1}')  $(wc -c < "$RT") bytes"

if cmp -s "$FRESH" "$RT"; then
    echo "  PASS  fresh-launch and round-trip are bytewise identical"
    exit 0
else
    echo "  FAIL  fresh-launch and round-trip differ — SmatchetDark state leaked across the round-trip"
    echo
    echo "Hint: copy the captures off to inspect:"
    echo "  cp '$FRESH' /tmp/fresh.png"
    echo "  cp '$RT'    /tmp/roundtrip.png"
    # Trap removes TMP_DIR — copy out before exit so the diff is visible.
    cp "$FRESH" /tmp/theme-roundtrip-fresh.png 2>/dev/null || true
    cp "$RT"    /tmp/theme-roundtrip-rt.png    2>/dev/null || true
    echo "  Auto-copied to /tmp/theme-roundtrip-{fresh,rt}.png"
    exit 1
fi
