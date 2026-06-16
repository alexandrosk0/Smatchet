# Sanitizers.cmake — apply ASan / UBSan / TSan / MSan flags to a target based
# on the SMATCHET_SANITIZER cache variable.
#
# Default behaviour is a no-op (SMATCHET_SANITIZER unset or "off"). Sanitizer
# presets in CMakePresets.json set the cache variable; debug-detective picks
# a preset per investigation type.
#
# Compiler matrix (verify if symptoms arise):
#   asan / ubsan : MSVC /fsanitize=address (ASAN-only; UBSAN not available).
#                  Clang -fsanitize=address,undefined (full suite).
#                  CI uses both ninja-msvc-asan and ninja-clang-asan.
#                  Legacy MinGW/MSYS2 GCC does NOT ship ASAN/UBSAN runtimes;
#                  smatchet_verify_mingw_gcc_sanitizer_runtimes() catches that.
#   tsan         : MSVC does NOT support TSAN. TSAN CI job deferred until
#                  a Linux runner is available.
#   msan         : Clang-only. We hard-fail here if the active compiler
#                  is not Clang.
#   fuzzer       : Clang-only (libFuzzer). Native-clang Linux lane only
#                  (ninja-fuzzer-linux); we hard-fail on MSVC / clang-cl.
#                  -fsanitize=fuzzer,address,undefined both instruments
#                  coverage AND links libFuzzer's main() (which drives each
#                  fuzz target's LLVMFuzzerTestOneInput). See tests/fuzz/.
#
# Hard rules (mirrored in agents/build-doctor.md § Stack):
#   * Two sanitizers must not coexist in one build (ASan + TSan, ASan + MSan
#     are mutually exclusive at link/runtime). SMATCHET_SANITIZER is a single
#     string, not a list.
#   * Flags are PRIVATE so vendored FetchContent deps stay untainted.
#   * Sanitizer runtime DLLs / .so files (libasan, libtsan, libubsan,
#     libclang_rt.msan) must be on PATH at launch time — debug-launch
#     failure with "DLL not found" usually means a missing runtime.

function(smatchet_apply_sanitizers tgt)
    if(NOT TARGET "${tgt}")
        return()
    endif()
    if(NOT DEFINED SMATCHET_SANITIZER OR
       "${SMATCHET_SANITIZER}" STREQUAL "" OR
       "${SMATCHET_SANITIZER}" STREQUAL "off")
        return()
    endif()

    set(_flags "")
    set(_link  "")
    set(_msvc FALSE)
    set(_clang_cl FALSE)
    if(MSVC)
        set(_msvc TRUE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            set(_clang_cl TRUE)
        endif()
    endif()

    if("${SMATCHET_SANITIZER}" STREQUAL "asan")
        if(_msvc)
            list(APPEND _flags
                /fsanitize=address
                -D_DISABLE_STRING_ANNOTATION
                -D_DISABLE_VECTOR_ANNOTATION)
            if(_clang_cl)
                # clang-cl needs the ASAN dynamic runtime linked explicitly. The
                # runtime lives at <clang-install>/lib/clang/<major>/lib/windows.
                # Resolve <major> from the *actual* resolved compiler instead of a
                # hardcoded version list: under vcvars64, bare `clang-cl` may resolve
                # to the VS-bundled LLVM (e.g. 19) rather than standalone LLVM
                # (e.g. 22), and a fixed list silently misses it (find fails -> the
                # asan libs are never linked -> unresolved __asan_* at link time).
                cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _clang_bin)
                cmake_path(GET _clang_bin PARENT_PATH _clang_root)
                set(_clang_lib_root "${_clang_root}/lib/clang")
                set(_asan_hints "")
                string(REGEX MATCH "^[0-9]+" _clang_major "${CMAKE_CXX_COMPILER_VERSION}")
                if(_clang_major)
                    list(APPEND _asan_hints "${_clang_lib_root}/${_clang_major}/lib/windows")
                endif()
                # Fallback: enumerate every versioned runtime dir this exact toolchain
                # ships (an LLVM install carries only its own major version dir), so
                # any present/future clang version resolves without a code change.
                file(GLOB _asan_glob "${_clang_lib_root}/*/lib/windows")
                if(_asan_glob)
                    list(APPEND _asan_hints ${_asan_glob})
                endif()
                find_library(_clang_asan_lib
                    NAMES clang_rt.asan_dynamic-x86_64
                    HINTS ${_asan_hints}
                    NO_DEFAULT_PATH)
                find_library(_clang_asan_thunk
                    NAMES clang_rt.asan_dynamic_runtime_thunk-x86_64
                    HINTS ${_asan_hints}
                    NO_DEFAULT_PATH)
                if(_clang_asan_lib AND _clang_asan_thunk)
                    list(APPEND _link "${_clang_asan_lib}" "${_clang_asan_thunk}")
                else()
                    message(WARNING
                        "clang-cl ASAN: could not find clang_rt.asan_dynamic libs under "
                        "${_clang_lib_root}/*/lib/windows (compiler ${CMAKE_CXX_COMPILER}, "
                        "version ${CMAKE_CXX_COMPILER_VERSION}); link may fail.")
                endif()
            endif()
        else()
            list(APPEND _flags
                -fsanitize=address
                -fsanitize=undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all)
            list(APPEND _link
                -fsanitize=address
                -fsanitize=undefined)
        endif()
    elseif("${SMATCHET_SANITIZER}" STREQUAL "tsan")
        if(_msvc)
            message(WARNING
                "SMATCHET_SANITIZER=tsan is not supported by MSVC. "
                "Ignoring for target '${tgt}'.")
            return()
        endif()
        list(APPEND _flags
            -fsanitize=thread
            -fno-omit-frame-pointer)
        list(APPEND _link
            -fsanitize=thread)
    elseif("${SMATCHET_SANITIZER}" STREQUAL "msan")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR
                "SMATCHET_SANITIZER=msan requires Clang. "
                "Active C++ compiler is '${CMAKE_CXX_COMPILER_ID}' "
                "(${CMAKE_CXX_COMPILER}). "
                "Use a Clang preset (e.g. ninja-debug-clang with "
                "-DSMATCHET_SANITIZER=msan) and confirm clang is installed.")
        endif()
        list(APPEND _flags
            -fsanitize=memory
            -fsanitize-memory-track-origins=2
            -fno-omit-frame-pointer)
        list(APPEND _link
            -fsanitize=memory)
    elseif("${SMATCHET_SANITIZER}" STREQUAL "fuzzer")
        # libFuzzer is a Clang runtime. clang-cl's -fsanitize=fuzzer is fragile
        # on the MSVC ABI (it would need the same manual clang_rt lib-hunt the
        # ASan path above does), so the fuzz lane is native-clang/Linux only
        # (ninja-fuzzer-linux). Hard-fail loudly on any MSVC-ABI compiler rather
        # than emit a half-linked binary.
        if(_msvc)
            message(FATAL_ERROR
                "SMATCHET_SANITIZER=fuzzer targets native Clang on Linux. "
                "libFuzzer is a Clang runtime and clang-cl's -fsanitize=fuzzer "
                "is fragile on the MSVC ABI. Active compiler is MSVC-ABI "
                "'${CMAKE_CXX_COMPILER}'. Use the ninja-fuzzer-linux preset on "
                "a Linux clang toolchain (apt-get install -y clang lld).")
        endif()
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR
                "SMATCHET_SANITIZER=fuzzer requires Clang (libFuzzer). "
                "Active C++ compiler is '${CMAKE_CXX_COMPILER_ID}' "
                "(${CMAKE_CXX_COMPILER}).")
        endif()
        # -fsanitize=fuzzer instruments coverage AND links libFuzzer's main(),
        # which drives each target's LLVMFuzzerTestOneInput(). Compose with
        # ASan+UBSan so a fuzz-found input that reads OOB / triggers UB aborts
        # into a reproducible crash- artifact; -fno-sanitize-recover=all makes
        # UBSan fatal (default is warn-and-continue).
        list(APPEND _flags
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        list(APPEND _link
            -fsanitize=fuzzer,address,undefined)
    else()
        message(WARNING
            "SMATCHET_SANITIZER='${SMATCHET_SANITIZER}' not recognised "
            "(expected: asan, tsan, msan, fuzzer, off). Ignoring for target '${tgt}'.")
        return()
    endif()

    message(STATUS
        "Sanitizers: applying ${SMATCHET_SANITIZER} to target '${tgt}' "
        "(compiler=${CMAKE_CXX_COMPILER_ID})")

    target_compile_options(${tgt} PRIVATE ${_flags})
    if(_link)
        target_link_options(${tgt} PRIVATE ${_link})
    endif()
endfunction()

# Strip MSVC /RTC* runtime-check flags from the global Debug flag variables when
# an ASan build is active. /RTC1 (and /RTCs /RTCu /RTCc) are MUTUALLY EXCLUSIVE
# with /fsanitize=address: the runtime-check stack-frame guards collide with
# ASan's redzone/stack instrumentation, and on an exception-unwind path (e.g. a
# doctest CHECK_THROWS_AS) the combination SIGABRTs at runtime — silently
# halting the test run. CMake injects /RTC1 by default in CMAKE_CXX_FLAGS_DEBUG
# (and _C_) for MSVC, so the ninja-msvc-asan preset (CMAKE_BUILD_TYPE=Debug)
# inherits it unless we strip it here. The RelWithDebInfo ASan presets
# (ninja-clang-asan, ninja-ui-test-asan-*) never carry /RTC* and are unaffected.
#
# Called from the root CMakeLists immediately after include(cmake/Sanitizers.cmake)
# and BEFORE any target is defined, so the cleaned flags apply tree-wide. Scoped
# to real MSVC cl (not clang-cl, which does not emit /RTC* defaults) and only when
# SMATCHET_SANITIZER=asan. PARENT_SCOPE writes propagate to the including scope;
# these are normal (non-cache) directory-scope variables that override the cache
# default for the configure.
function(smatchet_strip_msvc_rtc_for_asan)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        return()  # only real cl.exe injects /RTC* defaults
    endif()
    if(NOT DEFINED SMATCHET_SANITIZER OR NOT "${SMATCHET_SANITIZER}" STREQUAL "asan")
        return()
    endif()
    set(_changed FALSE)
    foreach(_var
            CMAKE_CXX_FLAGS_DEBUG CMAKE_C_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_RELWITHDEBINFO)
        if(DEFINED ${_var} AND "${${_var}}" MATCHES "/RTC")
            string(REGEX REPLACE "/RTC[1csu]+" "" _cleaned "${${_var}}")
            string(REGEX REPLACE "  +" " " _cleaned "${_cleaned}")
            string(STRIP "${_cleaned}" _cleaned)
            set(${_var} "${_cleaned}" PARENT_SCOPE)
            set(_changed TRUE)
        endif()
    endforeach()
    if(_changed)
        message(STATUS
            "Sanitizers: stripped MSVC /RTC* from Debug/RelWithDebInfo flags "
            "(incompatible with /fsanitize=address).")
    endif()
endfunction()

# Fail configure (not link) when MinGW GCC cannot resolve sanitizer import
# libraries. Avoids opaque ld: "cannot find -lasan" from toolchains that omit
# the sanitizer runtime (common with IDE-bundled MinGW).
#
# Set -DSMATCHET_VERIFY_SANITIZER_RUNTIME=OFF only if you know what you are
# doing (link will likely still fail without the import libs / DLLs).
function(smatchet_verify_mingw_gcc_sanitizer_runtimes)
    if(DEFINED SMATCHET_VERIFY_SANITIZER_RUNTIME AND NOT SMATCHET_VERIFY_SANITIZER_RUNTIME)
        return()
    endif()
    if(NOT WIN32)
        return()
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()
    if(NOT MINGW)
        return()
    endif()
    if(NOT DEFINED SMATCHET_SANITIZER OR
       "${SMATCHET_SANITIZER}" STREQUAL "" OR
       "${SMATCHET_SANITIZER}" STREQUAL "off")
        return()
    endif()

    set(_libs "")
    set(_what "")
    if("${SMATCHET_SANITIZER}" STREQUAL "asan")
        list(APPEND _libs libasan.dll.a libubsan.dll.a)
        set(_what "AddressSanitizer and UndefinedBehaviorSanitizer")
    elseif("${SMATCHET_SANITIZER}" STREQUAL "tsan")
        list(APPEND _libs libtsan.dll.a)
        set(_what "ThreadSanitizer (TSan is often missing or partial on Windows MinGW)")
    else()
        return()
    endif()

    foreach(_name IN LISTS _libs)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${_name}
            OUTPUT_VARIABLE _path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0 OR NOT EXISTS "${_path}")
            message(FATAL_ERROR
                "SMATCHET_SANITIZER=${SMATCHET_SANITIZER}: '${CMAKE_CXX_COMPILER}' "
                "cannot locate ${_name} (print-file-name returned '${_path}', rc=${_rc}).\n"
                "This MinGW install does not ship ${_what} import libraries next to g++. "
                "JetBrains CLion's bundled MinGW is a common case.\n"
                "Fix: use an MSVC or Clang preset for sanitizer builds "
                "(e.g. ninja-msvc-asan, ninja-clang-asan), or clear SMATCHET_SANITIZER / "
                "CMake cache if you did not intend a sanitizer build.\n"
                "Emergency bypass (expect link/runtime failure): "
                "-DSMATCHET_VERIFY_SANITIZER_RUNTIME=OFF")
        endif()
    endforeach()
endfunction()
