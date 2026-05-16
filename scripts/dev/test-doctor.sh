#!/usr/bin/env bash
# test-doctor.sh -- bucket-A verification for scripts/dev/doctor.ps1 (and .sh on non-Windows).
#
# Asserts:
#   1. Running the doctor on the current host exits 0 (toolchain is wired).
#   2. Stripping C:\msys64\ucrt64\bin (or its bash equivalent) from PATH
#      makes the doctor exit 1 AND its output mentions "MSYS2".
#
# This catches: (a) the doctor regressing into always-passing or always-failing,
# (b) the MSYS2-missing path losing its actionable install hint.
#
# Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 -- every assertion passed
#   1 -- at least one assertion failed
#   2 -- doctor script missing or no runner available

set -uo pipefail

cd "$(dirname "$0")/../.."

PASSED=0
FAILED=0

note_pass() { echo "  PASS: $1"; PASSED=$((PASSED + 1)); }
note_fail() { echo "  FAIL: $1"; FAILED=$((FAILED + 1)); }

# Decide which doctor flavour to run.
#  - On Windows / MSYS2, prefer doctor.ps1 via pwsh / powershell.
#  - Otherwise (Linux, mac), use doctor.sh.
PWSH=""
if command -v pwsh >/dev/null 2>&1; then
    PWSH="pwsh"
elif command -v powershell >/dev/null 2>&1; then
    PWSH="powershell"
fi

DOCTOR_PS1="scripts/dev/doctor.ps1"
DOCTOR_SH="scripts/dev/doctor.sh"

if [ ! -f "$DOCTOR_PS1" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: $DOCTOR_PS1 not found"
    exit 2
fi
if [ ! -f "$DOCTOR_SH" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: $DOCTOR_SH not found"
    exit 2
fi

run_doctor_ps1() {
    # If the canonical MSYS2 UCRT64 bin exists on disk but isn't on PATH (e.g.
    # developer uses a JetBrains-bundled MinGW), prepend it before invoking the
    # doctor so we test the doctor's logic, not the host's PATH ordering.
    #
    # We write a small wrapper to a temp file and invoke it with -File. PS 5.1's
    # `-Command "..."` has quirky handling of multi-line scripts with embedded
    # double quotes; -File is reliable.
    local tmpdir wrapper
    tmpdir=$(mktemp -d 2>/dev/null || echo "/tmp")
    wrapper="$tmpdir/doctor-wrap-$$.ps1"
    cat > "$wrapper" <<'PSEOF'
$ucrt = "C:\msys64\ucrt64\bin"
if ((Test-Path -LiteralPath (Join-Path $ucrt "gcc.exe"))) {
    $parts = $env:PATH -split ";" | ForEach-Object { $_.TrimEnd("\") }
    if ($parts -notcontains $ucrt.TrimEnd("\")) {
        $env:PATH = "$ucrt;$env:PATH"
    }
}
& "scripts\dev\doctor.ps1"
exit $LASTEXITCODE
PSEOF
    "$PWSH" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$wrapper" 2>&1
    local rc=${PIPESTATUS[0]}
    rm -f "$wrapper"
    return $rc
}

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

# Try to use doctor.ps1 if a PowerShell runner exists, else fall back to doctor.sh.
USE_PS1=0
if [ -n "$PWSH" ]; then
    USE_PS1=1
fi

echo "test-doctor: runner=$([ "$USE_PS1" -eq 1 ] && echo "$PWSH (doctor.ps1)" || echo "bash (doctor.sh)")"
echo

# ---------------------------------------------------------------------------
# Assertion 1: current host passes
# ---------------------------------------------------------------------------

echo "[1/2] doctor on current host -- expect exit 0"
if [ "$USE_PS1" -eq 1 ]; then
    OUT_PASS=$(run_doctor_ps1) || RC_PASS=$?
else
    OUT_PASS=$(run_doctor_sh) || RC_PASS=$?
fi
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

echo "[2/2] doctor with C:\\msys64\\ucrt64\\bin stripped from PATH -- expect exit 1 + mentions MSYS2"

if [ "$USE_PS1" -eq 1 ]; then
    # Strip both forms (with / without trailing backslash, case-insensitive) inside the
    # PowerShell child so the parent PATH is untouched. Use a temp -File wrapper for
    # robustness (PS 5.1 -Command "..." has quirky multi-line handling).
    STRIP_TMP=$(mktemp -d 2>/dev/null || echo "/tmp")
    STRIP_WRAPPER="$STRIP_TMP/doctor-strip-$$.ps1"
    cat > "$STRIP_WRAPPER" <<'PSEOF'
$ucrt = "C:\msys64\ucrt64\bin"
$norm = $ucrt.TrimEnd("\")
$parts = $env:PATH -split ";" | Where-Object { $_.TrimEnd("\") -ine $norm }
$env:PATH = ($parts -join ";")
& "scripts\dev\doctor.ps1"
exit $LASTEXITCODE
PSEOF
    OUT_FAIL=$("$PWSH" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$STRIP_WRAPPER" 2>&1) || RC_FAIL=$?
    rm -f "$STRIP_WRAPPER"
else
    # Strip every PATH entry whose tail is .../ucrt64/bin (forward or back slashes).
    NEW_PATH=$(echo "$PATH" | tr ':' '\n' | grep -vEi '(/|\\)ucrt64(/|\\)bin/?$' | paste -sd: -)
    OUT_FAIL=$(PATH="$NEW_PATH" bash "$DOCTOR_SH" 2>&1) || RC_FAIL=$?
fi
RC_FAIL="${RC_FAIL:-0}"
echo "$OUT_FAIL" | sed 's/^/    /'
echo "    (exit=$RC_FAIL)"

if [ "$RC_FAIL" -eq 1 ]; then
    note_pass "doctor exits 1 when MSYS2 UCRT64 bin is missing from PATH"
else
    note_fail "expected exit 1 with MSYS2 bin stripped, got $RC_FAIL"
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
