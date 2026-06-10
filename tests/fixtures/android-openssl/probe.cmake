# android-openssl/probe.cmake — behavioral driver for the Issue #1068 fail-fast.
# -----------------------------------------------------------------------------
# Runs the REAL per-ABI static-OpenSSL resolve block by invoking the production
# smatchet_prepare_cpr() in `cmake -P` script mode with ANDROID forced on. No NDK,
# no compiler, no cpr fetch needed — script mode evaluates the EXISTS-triple guard
# and its message(FATAL_ERROR) exactly as a real Android configure would.
#
# Exit 0  : a complete per-ABI tree resolved (or no base was supplied, so the block
#           was skipped — neither case is fatal).
# Non-zero: an incomplete per-ABI tree tripped the FATAL_ERROR — the #1068 invariant.
#
# This is the BEHAVIORAL companion to the static text gate in
# agents/scripts/project/test-mobile-security.sh (which proves the marker is bound
# to a message(FATAL_ERROR), not its severity/guard logic). Driven by the ctest
# entries in tests/CMakeLists.txt and tests/bats/android_openssl_failfast.bats.
#
# Required -D: SMATCHET_THIRDPARTY_CMAKE (abs path to cmake/SmatchetThirdParty.cmake).
# Optional -D: ANDROID_ABI (default arm64-v8a), SMATCHET_ANDROID_OPENSSL_BASE.
cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED SMATCHET_THIRDPARTY_CMAKE)
    message(FATAL_ERROR "probe: pass -DSMATCHET_THIRDPARTY_CMAKE=<abs path to cmake/SmatchetThirdParty.cmake>")
endif()
if(NOT EXISTS "${SMATCHET_THIRDPARTY_CMAKE}")
    message(FATAL_ERROR "probe: SMATCHET_THIRDPARTY_CMAKE does not exist: ${SMATCHET_THIRDPARTY_CMAKE}")
endif()

# Force the Android branch of smatchet_prepare_cpr without an NDK toolchain file.
set(ANDROID 1)
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()

include("${SMATCHET_THIRDPARTY_CMAKE}")
smatchet_prepare_cpr()

message(STATUS "PROBE_OK: smatchet_prepare_cpr resolved OpenSSL for ABI '${ANDROID_ABI}'")
