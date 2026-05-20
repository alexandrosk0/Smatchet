#!/usr/bin/env bash
# test-doctor.sh -- bucket-A verification for scripts/dev/doctor.sh.
#
# Asserts:
#   1. Running the doctor on the current host exits 0 (toolchain is wired).
#   2. Stripping C:\msys64\ucrt64\bin (or its bash equivalent) from PATH
#      makes the doctor exit >= 2 (WARN or FAIL) AND its output mentions
#      "MSYS2". Exit code is non-zero either way: 2 when a working gcc
#      still resolves elsewhere (e.g. JetBrains-bundled MinGW) and only
#      the PATH check warns, 1 when gcc is also unreachable.
#
# This catches: (a) the doctor regressing into always-passing or always-failing,
# (b) the MSYS2-missing path losing its actionable install hint.
#
# Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 -- every assertion passed
#   1 -- at least one assertion failed
#   2 -- doctor script missing

set -uo pipefail

cd "$(dirname "$0")/../.."

PASSED=0
FAILED=0

note_pass() { echo "  PASS: $1"; PASSED=$((PASSED + 1)); }
note_fail() { echo "  FAIL: $1"; FAILED=$((FAILED + 1)); }

DOCTOR_SH="scripts/dev/doctor.sh"

if [ ! -f "$DOCTOR_SH" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: $DOCTOR_SH not found"
    exit 2
fi

run_doctor_sh() {
    # If canonical MSYS2 UCRT64 bin exists on disk but isn't on PATH, prepend it
    # so we test doctor logic not host PATH ordering. Use cross-platform check.
    local ucrt_win="C:/msys64/ucrt64/bin"
    local ucrt_unix="/c/msys64/ucrt64/bin"
    local ucrt=""
    if [ -x "$ucrt_win/gcc.exe" ]; then ucrt="$ucrt_win"; fi
    if [ -x "$ucrt_unix/gcc.exe" ]; then ucrt="$ucrt_unix"; fi
    if [ -n "$ucrt" ]; then
        case ":$PATH:" in
            *":$ucrt:"*) :;;
            *) PATH="$ucrt:$PATH";;
        esac
    fi
    PATH="$PATH" bash "$DOCTOR_SH" 2>&1
    return $?
}

echo "test-doctor: runner=bash (doctor.sh)"
echo

# ---------------------------------------------------------------------------
# Assertion 1: current host passes
# ---------------------------------------------------------------------------

echo "[1/2] doctor on current host -- expect exit 0"
OUT_PASS=$(run_doctor_sh) || RC_PASS=$?
RC_PASS="${RC_PASS:-0}"
echo "$OUT_PASS" | sed 's/^/    /'
echo "    (exit=$RC_PASS)"

if [ "$RC_PASS" -eq 0 ]; then
    note_pass "doctor exits 0 on current host"
else
    note_fail "expected exit 0, got $RC_PASS -- toolchain may be broken or doctor regressed"
fi
echo

# ---------------------------------------------------------------------------
# Assertion 2: MSYS2 UCRT64 removed -> doctor must fail and mention MSYS2
# ---------------------------------------------------------------------------

echo "[2/2] doctor with C:\\msys64\\ucrt64\\bin stripped from PATH -- expect exit >= 2 + mentions MSYS2"

# Strip every PATH entry whose tail is .../ucrt64/bin (forward or back slashes).
NEW_PATH=$(echo "$PATH" | tr ':' '\n' | grep -vEi '(/|\\)ucrt64(/|\\)bin/?$' | paste -sd: -)
OUT_FAIL=$(PATH="$NEW_PATH" bash "$DOCTOR_SH" 2>&1) || RC_FAIL=$?
RC_FAIL="${RC_FAIL:-0}"
echo "$OUT_FAIL" | sed 's/^/    /'
echo "    (exit=$RC_FAIL)"

if [ "$RC_FAIL" -ge 2 ]; then
    note_pass "doctor exits $RC_FAIL (>= 2) when MSYS2 UCRT64 bin is missing from PATH"
elif [ "$RC_FAIL" -eq 1 ]; then
    note_pass "doctor exits 1 when MSYS2 UCRT64 bin is missing from PATH (gcc also unreachable)"
else
    note_fail "expected exit >= 2 (or 1) with MSYS2 bin stripped, got $RC_FAIL"
fi

if echo "$OUT_FAIL" | grep -qi 'MSYS2'; then
    note_pass "doctor output mentions 'MSYS2' (actionable install hint present)"
else
    note_fail "doctor output did not mention 'MSYS2' -- install hint is missing or worded differently"
fi

# ---------------------------------------------------------------------------

echo
echo "Passed: $PASSED  Failed: $FAILED"

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
