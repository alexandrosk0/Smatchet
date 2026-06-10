#!/usr/bin/env bats
# tests/bats/android_openssl_failfast.bats
# ----------------------------------------------------------------------------
# Behavioral coverage for the Issue #1068 Android OpenSSL fail-fast in
# cmake/SmatchetThirdParty.cmake (function smatchet_prepare_cpr). Drives the REAL
# per-ABI static-OpenSSL resolve block via tests/fixtures/android-openssl/probe.cmake
# in `cmake -P` script mode (no NDK, no compiler, no cpr fetch) and asserts the
# EXISTS-triple guard's outcome on committed fixture trees.
#
# This is the BEHAVIORAL companion to the STATIC text gate in
# agents/scripts/project/test-mobile-security.sh — that gate proves the marker
# sentence is bound to a message(FATAL_ERROR) (not a WARNING); this suite proves
# the guard LOGIC actually fails the configure when a per-ABI tree is incomplete
# (a regression that kept the FATAL text but flipped AND->OR or mis-named a file
# would pass the static gate but fail here).
#
# Cases (fixtures under tests/fixtures/android-openssl/):
#   complete/        — libssl.a + libcrypto.a + opensslv.h present  -> exit 0, "PROBE_OK"
#   missing-crypto/  — no libcrypto.a                               -> FATAL, marker
#   missing-header/  — no include/openssl/opensslv.h                -> FATAL, marker
#   complete/ + ABI=mips — complete tree but unbuilt ABI            -> FATAL, marker
#   (no base supplied) — block is skipped, not fatal                -> exit 0, "PROBE_OK"
#
# Requires: bash, bats, cmake on PATH. ctest mirrors the exit-code contract for CI
# (tests/CMakeLists.txt); this suite adds the marker-text precision locally.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export TP="$REPO_ROOT/cmake/SmatchetThirdParty.cmake"
    export PROBE="$REPO_ROOT/tests/fixtures/android-openssl/probe.cmake"
    export FIX="$REPO_ROOT/tests/fixtures/android-openssl"
    export MARKER="Android requires a pinned static OpenSSL"
    command -v cmake >/dev/null 2>&1 || skip "cmake not on PATH (see BUILD.md § Dev-script CLI tools)"
    [ -f "$TP" ]    || skip "missing $TP"
    [ -f "$PROBE" ] || skip "missing $PROBE"
}

# cmake word-wraps message(FATAL_ERROR) text at ~76 cols, so the marker sentence
# spans a newline ("...opensslv.h).  Android\n  requires a pinned static OpenSSL").
# Squeeze every whitespace run (incl. newlines) to a single space so a contiguous
# substring match works — mirrors the flexible-whitespace PCRE the static gate uses.
_norm() { printf '%s' "$1" | tr -s '[:space:]' ' '; }

@test "complete per-ABI tree resolves (exit 0 + PROBE_OK)" {
    run cmake -DSMATCHET_THIRDPARTY_CMAKE="$TP" \
        -DSMATCHET_ANDROID_OPENSSL_BASE="$FIX/complete" \
        -DANDROID_ABI=arm64-v8a -P "$PROBE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PROBE_OK"* ]]
    [[ "$output" == *"derived Android OpenSSL for arm64-v8a"* ]]
    [[ "$output" != *"$MARKER"* ]]
}

@test "missing libcrypto.a trips the fail-fast (nonzero + marker)" {
    run cmake -DSMATCHET_THIRDPARTY_CMAKE="$TP" \
        -DSMATCHET_ANDROID_OPENSSL_BASE="$FIX/missing-crypto" \
        -DANDROID_ABI=arm64-v8a -P "$PROBE"
    [ "$status" -ne 0 ]
    [[ "$(_norm "$output")" == *"$MARKER"* ]]
    [[ "$(_norm "$output")" == *"Issue #1068"* ]]
    [[ "$output" != *"PROBE_OK"* ]]
}

@test "missing openssl headers trips the fail-fast (nonzero + marker)" {
    run cmake -DSMATCHET_THIRDPARTY_CMAKE="$TP" \
        -DSMATCHET_ANDROID_OPENSSL_BASE="$FIX/missing-header" \
        -DANDROID_ABI=arm64-v8a -P "$PROBE"
    [ "$status" -ne 0 ]
    [[ "$(_norm "$output")" == *"$MARKER"* ]]
    [[ "$output" != *"PROBE_OK"* ]]
}

@test "complete base but an unbuilt ABI trips the fail-fast (nonzero + marker names the ABI)" {
    run cmake -DSMATCHET_THIRDPARTY_CMAKE="$TP" \
        -DSMATCHET_ANDROID_OPENSSL_BASE="$FIX/complete" \
        -DANDROID_ABI=mips -P "$PROBE"
    [ "$status" -ne 0 ]
    [[ "$(_norm "$output")" == *"$MARKER"* ]]
    [[ "$(_norm "$output")" == *"ABI 'mips'"* ]]
    [[ "$output" != *"PROBE_OK"* ]]
}

@test "no OpenSSL base supplied: block is skipped, not fatal (exit 0 + PROBE_OK)" {
    # env -u: a true no-base case. The probe falls back to ENV{SMATCHET_ANDROID_OPENSSL_BASE}
    # when the CMake var is unset, and CI exports that var for the real Android build — strip
    # it so this case is deterministic regardless of the ambient environment.
    run env -u SMATCHET_ANDROID_OPENSSL_BASE cmake -DSMATCHET_THIRDPARTY_CMAKE="$TP" \
        -DANDROID_ABI=arm64-v8a -P "$PROBE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PROBE_OK"* ]]
    [[ "$output" != *"$MARKER"* ]]
}
