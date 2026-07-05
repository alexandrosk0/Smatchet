# smatchet_patch_or_die(<var> <find> <replace> <label>)
#   In-place string(REPLACE) on the contents held in the variable named <var>,
#   but message(FATAL_ERROR) if NEITHER <find> NOR <replace> is present in the
#   contents — i.e. the patch target vanished (most likely a dependency version
#   bump changed the upstream source) so the REPLACE would silently no-op and a
#   load-bearing fix would be dropped while the build stays green.
#
#   This is the supply-chain guard the plain `string(REPLACE)` change-detector
#   pattern cannot give: that pattern only diffs before/after and cannot tell
#   "already patched / nothing to do" from "patch target VANISHED → silent
#   no-op → the load-bearing fix is gone."
#
#   Idempotent by construction: re-running after the patch has already been
#   applied is a clean no-op, because once <replace> is present the find-or-
#   already-present guard passes via the <replace> branch and the REPLACE has
#   nothing left to match. This also means a multi-step patch whose later steps
#   only rewrite text produced by earlier steps is safe to route here AS LONG AS
#   the earlier (primary) steps run first — on a fresh tree those later steps see
#   their <replace> text already present (produced by the primary step) and pass
#   via the already-present branch.
#
#   Use this for every patch whose silent no-op would be a real defect. A purely
#   cosmetic best-effort replace (e.g. a cmake_minimum_required bump that only
#   suppresses a deprecation warning) may stay a plain string(REPLACE) — FATAL-ing
#   when such a target legitimately disappears would be wrong.
function(smatchet_patch_or_die _var _find _replace _label)
    set(_contents "${${_var}}")
    string(FIND "${_contents}" "${_find}" _find_pos)
    string(FIND "${_contents}" "${_replace}" _replace_pos)
    if(_find_pos EQUAL -1 AND _replace_pos EQUAL -1)
        message(FATAL_ERROR
            "smatchet_patch_or_die: patch target for '${_label}' not found — neither the "
            "original nor the already-patched text is present. A dependency bump likely changed "
            "the upstream source; re-derive this patch before continuing.")
    endif()
    string(REPLACE "${_find}" "${_replace}" _contents "${_contents}")
    set(${_var} "${_contents}" PARENT_SCOPE)
endfunction()

function(smatchet_prepare_cpr)
    # Bundled libcurl CMake probes ioctlsocket(FIONBIO) with int*; GCC 14+
    # rejects int* vs u_long*. Winsock uses u_long, so force the successful
    # result for GCC and clang-cl (native MSVC handles this internally).
    if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set(HAVE_IOCTLSOCKET_FIONBIO 1 CACHE INTERNAL "curl: ioctlsocket FIONBIO (GCC14+ / clang-cl probe fix)" FORCE)
    endif()
    # Android: cpr 1.9.2 (cpr-src/CMakeLists.txt) FATAL_ERRORs at configure time if
    # no SSL backend is selected, because cpr::Response/Header/Error are PUBLIC types
    # the whole tree depends on. On Windows cpr auto-selects WinSSL; on Android there
    # is no system TLS the bundled curl can find, so we force the OpenSSL backend and
    # rely on a per-ABI prebuilt OpenSSL provisioned by the caller (CI / preset) via
    # OPENSSL_ROOT_DIR + explicit OPENSSL_{SSL,CRYPTO}_LIBRARY / OPENSSL_INCLUDE_DIR.
    #
    # CMAKE_FIND_ROOT_PATH_MODE_* default to ONLY under android.toolchain.cmake, which
    # re-roots every find_package() probe into the NDK sysroot and hides an OpenSSL
    # installed outside it. Both find_package(OpenSSL) calls (cpr's, then bundled
    # curl's) must succeed, so widen the modes to BOTH for this configure. The actual
    # absolute paths stay caller-supplied (runtime-dependent: $RUNNER_TEMP in CI) — do
    # not hardcode them here. This is the Slice 1 TLS-backend spike deliverable.
    #
    # This block is the SINGLE authoritative home for these TLS-backend cache vars: it
    # runs before add_subdirectory(cpr) and FORCE-wins over any pre-seed, so it applies
    # to a raw `cmake -DANDROID=...` invocation too — they are deliberately NOT mirrored
    # in the android-ndk-arm64 preset (that copy was removed to keep this DRY).
    if(ANDROID)
        set(CPR_FORCE_OPENSSL_BACKEND ON CACHE BOOL "Android: force cpr OpenSSL backend (no system TLS)" FORCE)
        set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Android: link the prebuilt static OpenSSL" FORCE)
        set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH CACHE STRING "Android: find OpenSSL outside the NDK sysroot" FORCE)
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH CACHE STRING "Android: find OpenSSL outside the NDK sysroot" FORCE)
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH CACHE STRING "Android: find OpenSSL outside the NDK sysroot" FORCE)

        # CA trust (WS2 / Issue #1068). A stock Android device exposes no CA bundle to the
        # NDK-built libcurl, and libcurl (unlike the curl CLI) never reads the CURL_CA_BUNDLE
        # *env* var — only the compile-time CURL_CA_BUNDLE define (its default CAINFO) or an
        # explicit CURLOPT_CAINFO.
        #
        # PRIMARY trust anchor is now the RUNTIME seam, NOT these compile-time defines:
        # android_main.cpp extracts the APK-bundled Mozilla cacert.pem to the app private dir at
        # boot and feeds the actual path into Core via TrackerHttpPure::SetCaBundlePath; every
        # tracker verb then sets an explicit CURLOPT_CAINFO from it (TrackerHttpUtils::
        # MakeTrackerSslOptions). That path is resolved at runtime — it does not assume a fixed
        # applicationId or user id, so it is correct on user 0, secondary users, and work profiles.
        #
        # The defines below are DEMOTED to a documented defense-in-depth fallback (kept, not
        # removed, to avoid a fail-closed regression should the runtime seam ever be skipped):
        #   * CURL_CA_BUNDLE — baked default CAINFO at the deterministic primary-user path. Only
        #     consulted if no explicit CURLOPT_CAINFO is set; the runtime seam normally sets one,
        #     so this is the backstop. (TLS proven live earlier: read HTTP 200 + write PUT HTTP 204
        #     against real Jira.)
        #   * CURL_CA_PATH=none — we FORCE it to disable curl's autodetected
        #     /system/etc/security/cacerts default CApath, but this does NOT survive: cpr's own
        #     CMakeLists.txt re-sets CURL_CA_PATH back to the system store AFTER this block runs
        #     (it is included via add_subdirectory below). Harmless — the runtime CAINFO is an
        #     explicit cafile and takes precedence, so the stale system CApath is never consulted.
        #     Left in for documentation + in case cpr's ordering changes upstream.
        # CURL_CA_FALLBACK stays on so OpenSSL's SSL_CERT_FILE env (also set by the host shell)
        # is honored as a secondary net if both the runtime CAINFO and the baked default are unset.
        set(CURL_CA_BUNDLE "/data/user/0/com.smatchet.mobile/files/cacert.pem" CACHE STRING "Android: defense-in-depth fallback CAINFO; runtime TrackerHttpPure seam is primary" FORCE)
        set(CURL_CA_PATH "none" CACHE STRING "Android: attempt to disable the autodetected system CApath (cpr re-sets this; explicit CAINFO wins anyway)" FORCE)
        set(CURL_CA_FALLBACK ON CACHE BOOL "Android: also honor OpenSSL SSL_CERT_FILE as a secondary CA net" FORCE)

        # Gradle multi-ABI convenience. CI / the arm64 preset pin OPENSSL_ROOT_DIR (+ the
        # explicit lib/include paths) per single-ABI build. The Gradle externalNativeBuild
        # path drives BOTH abis from one invocation and cannot interpolate ${ANDROID_ABI}
        # into a static -D argument, so when the caller did NOT pin OPENSSL_ROOT_DIR we
        # derive the per-ABI prebuilt from one base dir + ${ANDROID_ABI} (x86_64 /
        # arm64-v8a subdirs, the layout build-android-openssl.sh installs). The base may
        # arrive via -D or the environment; the explicit FindOpenSSL vars below make the
        # later find_package(OpenSSL) a direct path lookup (no sysroot probe).
        if(NOT OPENSSL_ROOT_DIR)
            if(NOT SMATCHET_ANDROID_OPENSSL_BASE AND DEFINED ENV{SMATCHET_ANDROID_OPENSSL_BASE})
                set(SMATCHET_ANDROID_OPENSSL_BASE "$ENV{SMATCHET_ANDROID_OPENSSL_BASE}")
            endif()
            if(SMATCHET_ANDROID_OPENSSL_BASE)
                set(_smatchet_ossl_abi "${SMATCHET_ANDROID_OPENSSL_BASE}/${ANDROID_ABI}")
                # Issue #1068: a complete static OpenSSL for the ABI means ALL THREE —
                # libssl.a, libcrypto.a, and the public headers. A partial tree (e.g. a
                # half-finished build-android-openssl.sh run, or a base dir missing one
                # ABI) used to emit message(WARNING) and fall through to a sysroot probe.
                # A stock NDK sysroot has NO OpenSSL, so that probe finds nothing and the
                # build silently ships a TLS-broken APK (no Jira over HTTPS). Fail-fast at
                # configure instead — a loud, actionable error beats a runtime TLS failure.
                if(EXISTS "${_smatchet_ossl_abi}/lib/libssl.a"
                   AND EXISTS "${_smatchet_ossl_abi}/lib/libcrypto.a"
                   AND EXISTS "${_smatchet_ossl_abi}/include/openssl/opensslv.h")
                    set(OPENSSL_ROOT_DIR "${_smatchet_ossl_abi}" CACHE PATH "Android per-ABI OpenSSL (derived from base)" FORCE)
                    set(OPENSSL_INCLUDE_DIR "${_smatchet_ossl_abi}/include" CACHE PATH "Android per-ABI OpenSSL include (derived)" FORCE)
                    set(OPENSSL_SSL_LIBRARY "${_smatchet_ossl_abi}/lib/libssl.a" CACHE FILEPATH "Android per-ABI libssl (derived)" FORCE)
                    set(OPENSSL_CRYPTO_LIBRARY "${_smatchet_ossl_abi}/lib/libcrypto.a" CACHE FILEPATH "Android per-ABI libcrypto (derived)" FORCE)
                    message(STATUS "Smatchet: derived Android OpenSSL for ${ANDROID_ABI}: ${_smatchet_ossl_abi}")
                else()
                    message(FATAL_ERROR
                        "SMATCHET_ANDROID_OPENSSL_BASE='${SMATCHET_ANDROID_OPENSSL_BASE}' is set but a complete "
                        "static OpenSSL for ABI '${ANDROID_ABI}' was not found under '${_smatchet_ossl_abi}' "
                        "(need lib/libssl.a, lib/libcrypto.a, include/openssl/opensslv.h). "
                        "Android requires a pinned static OpenSSL — silent sysroot fallback ships a TLS-broken build. "
                        "Build it with scripts/dev/build-android-openssl.sh, or pin OPENSSL_ROOT_DIR explicitly. (Issue #1068)")
                endif()
            endif()
        endif()
    endif()
    # Smatchet uses libcurl for HTTPS transport but does not need zlib-backed
    # transfer decoding in the standalone runtime.
    set(CURL_ZLIB OFF CACHE BOOL "Disable optional curl zlib support" FORCE)
endfunction()

function(smatchet_patch_cpr_after_fetch cpr_source_dir)
    # MSVC C4244: Body(File) used int for tellg() (streamoff). Patch vendored
    # header after fetch rather than carrying a fork.
    set(_cpr_body_h "${cpr_source_dir}/include/cpr/body.h")
    if(EXISTS "${_cpr_body_h}")
        file(READ "${_cpr_body_h}" _cpr_body_h_contents)
        set(_cpr_body_h_patched "${_cpr_body_h_contents}")
        # Load-bearing: these silence MSVC C4244 under /WX. A silent no-op after a
        # cpr bump would resurface the warning-as-error; FATAL at configure is the
        # clearer failure (names which patch target vanished).
        smatchet_patch_or_die(_cpr_body_h_patched
            "int length = is.tellg();"
            "const std::streamoff length = is.tellg();"
            "cpr body.h tellg streamoff")
        smatchet_patch_or_die(_cpr_body_h_patched
            "buffer.resize(length);"
            "buffer.resize(static_cast<size_t>(length));"
            "cpr body.h buffer.resize cast")
        smatchet_patch_or_die(_cpr_body_h_patched
            "is.read(&buffer[0], length);"
            "is.read(buffer.data(), static_cast<std::streamsize>(length));"
            "cpr body.h is.read cast")
        if(NOT _cpr_body_h_patched STREQUAL _cpr_body_h_contents)
            file(WRITE "${_cpr_body_h}" "${_cpr_body_h_patched}")
            message(STATUS "Patched cpr include/cpr/body.h (streamoff / resize / read).")
        endif()
    endif()
endfunction()

function(smatchet_prepare_sqlitecpp)
    set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "Disable linting" FORCE)
    set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "Disable cppcheck" FORCE)
endfunction()

function(smatchet_patch_sqlitecpp_after_populate sqlitecpp_source_dir)
    # Cosmetic / best-effort: only suppresses a CMake deprecation warning about
    # pre-3.10 compatibility. Left a plain string(REPLACE) — if a future SQLiteCpp
    # bumps its own cmake_minimum_required past 3.10 this target legitimately
    # disappears, and FATAL-ing on that would be wrong. Not routed through
    # smatchet_patch_or_die.
    set(_sqlitecpp_cmakelists "${sqlitecpp_source_dir}/CMakeLists.txt")
    if(EXISTS "${_sqlitecpp_cmakelists}")
        file(READ "${_sqlitecpp_cmakelists}" _sqlitecpp_cmakelists_contents)
        string(REPLACE "cmake_minimum_required(VERSION 3.5)"
                       "cmake_minimum_required(VERSION 3.10)"
                       _sqlitecpp_patched_contents
                       "${_sqlitecpp_cmakelists_contents}")
        if(NOT _sqlitecpp_patched_contents STREQUAL _sqlitecpp_cmakelists_contents)
            file(WRITE "${_sqlitecpp_cmakelists}" "${_sqlitecpp_patched_contents}")
        endif()
    endif()

    # MinGW/GCC: fixed-width integer typedefs no longer leak from libstdc++
    # transitive includes; SQLiteCpp 3.3.1 uses int32_t/int64_t/... without
    # <cstdint> in Statement.h. Load-bearing on MinGW/GCC (compile failure if
    # dropped) → routed through smatchet_patch_or_die.
    #
    # The outer `NOT MATCHES "#include <cstdint>"` is a real idempotency guard
    # here (NOT redundant with the helper): the patch's <find> is "#include
    # <memory>", which stays a substring of the patched "#include <memory>\n
    # #include <cstdint>" form, so re-running the bare helper would append a
    # second <cstdint>. The guard skips entirely once <cstdint> is present. On a
    # fresh fetch (new dep version) where <cstdint> is absent, the guard lets the
    # helper run — and the helper FATALs if "#include <memory>" itself vanished.
    set(_sqlitecpp_statement_h "${sqlitecpp_source_dir}/include/SQLiteCpp/Statement.h")
    if(EXISTS "${_sqlitecpp_statement_h}")
        file(READ "${_sqlitecpp_statement_h}" _sqlitecpp_statement_h_contents)
        if(NOT _sqlitecpp_statement_h_contents MATCHES "#include <cstdint>")
            set(_sqlitecpp_statement_h_patched "${_sqlitecpp_statement_h_contents}")
            smatchet_patch_or_die(_sqlitecpp_statement_h_patched
                "#include <memory>"
                "#include <memory>\n#include <cstdint>"
                "SQLiteCpp Statement.h <cstdint> for MinGW/GCC")
            if(NOT _sqlitecpp_statement_h_patched STREQUAL _sqlitecpp_statement_h_contents)
                file(WRITE "${_sqlitecpp_statement_h}" "${_sqlitecpp_statement_h_patched}")
                message(STATUS "Patched SQLiteCpp include/SQLiteCpp/Statement.h (<cstdint> for MinGW/GCC).")
            endif()
        endif()
    endif()
endfunction()

function(smatchet_prepare_httplib)
    # We only use the plain HTTP server surface today. Letting cpp-httplib
    # auto-detect compression/TLS support pulls unnecessary runtime
    # dependencies into the standalone publish artifact.
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib OpenSSL auto-linking" FORCE)
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib zlib auto-linking" FORCE)
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib brotli auto-linking" FORCE)
    # cpp-httplib >= v0.20 adds a zstd auto-detect (v0.49 bump, #1588). Same
    # rationale as the three above — plus a cross-compile hazard: find_package
    # on a host with zstd dev headers marks the INTERFACE target
    # CPPHTTPLIB_ZSTD_SUPPORT, and the Android NDK sysroot then fails
    # `#include <zstd.h>` while compiling SmatchetMergeWatchNotifyServer.cpp.
    set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "Disable optional cpp-httplib zstd auto-linking" FORCE)
endfunction()
