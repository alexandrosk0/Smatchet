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
endfunction()
