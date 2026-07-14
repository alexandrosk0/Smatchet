#!/usr/bin/env bash
# test-doctor.sh -- bucket-A verification for scripts/dev/doctor.sh.
#
# Asserts:
#   1. Running the doctor on the current host exits 0 (toolchain is wired).
#   2. Stripping the C++ compiler (cl.exe / clang-cl) from PATH makes the
#      doctor exit >= 1 (compiler is a hard requirement) AND its output names
#      the compiler with an actionable install hint.
#
# NOTE: PR #463 ("doctor checks Windows MSVC toolchain, not Linux gcc")
# reworked doctor.sh away from gcc/MSYS2 to cl.exe/clang-cl. Assertion 2 used
# to strip C:\msys64\ucrt64\bin and grep for "MSYS2"; that concept no longer
# exists in doctor.sh, so the assertion now targets the actual required tool
# (the C++ compiler).
#
# This catches: (a) the doctor regressing into always-passing or always-failing,
# (b) the missing-compiler path losing its actionable install hint.
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
    PATH="$PATH" bash "$DOCTOR_SH" "$@" 2>&1
    return $?
}

echo "test-doctor: runner=bash (doctor.sh)"
echo

# ---------------------------------------------------------------------------
# Assertion 1: current host does not RED for THIS environment's declared tier.
# On a full Windows box the MSVC compiler is present (windows-dev -> GREEN). In a
# Linux container / ci-ubuntu the MSVC toolchain is [n/a] by declaration
# (linux-container tier, project.config.json § environments), so the doctor must
# NOT flat-RED on a toolchain that is not this environment's job (finding C3 /
# Proposal P5). "not RED" = exit != 1 (GREEN=0 or YELLOW=2 both acceptable) so a
# genuinely-relevant optional-tool WARN (e.g. cppcheck) does not fail this gate.
# ---------------------------------------------------------------------------

if command -v cl.exe >/dev/null 2>&1 || command -v clang-cl >/dev/null 2>&1; then
    ENV_TIER="windows-dev"
else
    ENV_TIER="linux-container"
fi

echo "[1/2] doctor --tier $ENV_TIER on current host -- expect not-RED (exit != 1)"
OUT_PASS=$(run_doctor_sh --tier "$ENV_TIER") || RC_PASS=$?
RC_PASS="${RC_PASS:-0}"
echo "$OUT_PASS" | sed 's/^/    /'
echo "    (exit=$RC_PASS)"

if [ "$RC_PASS" -ne 1 ]; then
    note_pass "doctor does not RED for tier '$ENV_TIER' on current host (exit $RC_PASS)"
else
    note_fail "expected not-RED for tier '$ENV_TIER', got RED (exit 1) -- toolchain broken or doctor regressed"
fi
echo

# ---------------------------------------------------------------------------
# Assertion 2: C++ compiler removed -> doctor must fail and name the compiler
# ---------------------------------------------------------------------------

echo "[2/2] doctor with the C++ compiler (cl.exe / clang-cl) stripped from PATH -- expect exit >= 1 + actionable compiler hint"

# Locate the dirs hosting cl.exe / clang-cl so we can strip exactly those,
# forcing doctor's required-compiler check (PR #463) down its failure path.
_strip_dirs=()
for _tool in cl.exe clang-cl; do
    _tp="$(command -v "$_tool" 2>/dev/null || true)"
    [ -n "$_tp" ] && _strip_dirs+=("$(dirname "$_tp")")
done

# Preserve host delimiter style (':' on Unix-like shells, ';' on Windows PATH).
if [[ "$PATH" == *';'* ]]; then
    PATH_SEP=';'
else
    PATH_SEP=':'
fi
IFS="$PATH_SEP" read -r -a _path_parts <<< "$PATH"
_filtered_parts=()
for _p in "${_path_parts[@]}"; do
    _drop=0
    for _d in "${_strip_dirs[@]}"; do
        # Normalize trailing slashes (both seps) and case (Windows PATH is
        # case-insensitive) before comparing, so neither bypasses stripping.
        _pn="${_p%/}"
        _dn="${_d%/}"
        if [ "$PATH_SEP" = ';' ]; then
            _pn="${_pn,,}"
            _dn="${_dn,,}"
        fi
        [ "$_pn" = "$_dn" ] && _drop=1 && break
    done
    [ "$_drop" -eq 0 ] && _filtered_parts+=("$_p")
done
NEW_PATH=$(IFS="$PATH_SEP"; echo "${_filtered_parts[*]}")
OUT_FAIL=$(PATH="$NEW_PATH" bash "$DOCTOR_SH" 2>&1) || RC_FAIL=$?
RC_FAIL="${RC_FAIL:-0}"
echo "$OUT_FAIL" | sed 's/^/    /'
echo "    (exit=$RC_FAIL)"

if [ "$RC_FAIL" -ge 1 ]; then
    note_pass "doctor exits $RC_FAIL (>= 1) when no C++ compiler is on PATH"
else
    note_fail "expected exit >= 1 with cl.exe/clang-cl stripped, got $RC_FAIL"
fi

if echo "$OUT_FAIL" | grep -qiE 'cl\.exe|clang-cl|compiler'; then
    note_pass "doctor output names the C++ compiler (actionable install hint present)"
else
    note_fail "doctor output did not mention the C++ compiler -- install hint missing or reworded"
fi

# ---------------------------------------------------------------------------

echo
echo "Passed: $PASSED  Failed: $FAILED"

# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
