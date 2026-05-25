# Sanitizers.cmake — apply ASan / UBSan / TSan / MSan flags to a target based
# on the SMATCHET_SANITIZER cache variable.
#
# Default behaviour is a no-op (SMATCHET_SANITIZER unset or "off"). Sanitizer
# presets in CMakePresets.json set the cache variable; debug-detective picks
# a preset per investigation type.
#
# Compiler matrix (verify if symptoms arise):
#   asan / ubsan : MSVC /fsanitize=address (ASAN-only; UBSAN not available).
#                  MSYS2 UCRT64 GCC/Clang do NOT ship ASAN/UBSAN runtimes
#                  (confirmed 2026-05). CI uses MSVC for ASAN coverage.
#                  smatchet_verify_mingw_gcc_sanitizer_runtimes() catches the
#                  GCC case and fails fast with a helpful message.
#   tsan         : MSVC does NOT support TSAN. MinGW GCC/Clang on MSYS2
#                  do NOT ship libtsan. TSAN CI job is deferred until a
#                  Linux runner is available.
#   msan         : Clang-only. MSYS2 GCC does NOT ship libmsan; the
#                  msan preset must select clang++/clang. We hard-fail
#                  here if the active compiler is not Clang.
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
    if(MSVC)
        set(_msvc TRUE)
    endif()

    if("${SMATCHET_SANITIZER}" STREQUAL "asan")
        if(_msvc)
            list(APPEND _flags
                /fsanitize=address
                -D_DISABLE_STRING_ANNOTATION
                -D_DISABLE_VECTOR_ANNOTATION)
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
                "Use the ninja-debug-msys2-msan preset (which selects "
                "clang/clang++ off PATH) and confirm clang is installed.")
        endif()
        list(APPEND _flags
            -fsanitize=memory
            -fsanitize-memory-track-origins=2
            -fno-omit-frame-pointer)
        list(APPEND _link
            -fsanitize=memory)
    else()
        message(WARNING
            "SMATCHET_SANITIZER='${SMATCHET_SANITIZER}' not recognised "
            "(expected: asan, tsan, msan, off). Ignoring for target '${tgt}'.")
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
                "Fix: point CLion CMake/toolchain at MSYS2 UCRT64 g++ (e.g. "
                "C:/msys64/ucrt64/bin/g++.exe) and use CMake preset "
                "'ninja-debug-msys2-${SMATCHET_SANITIZER}', or clear SMATCHET_SANITIZER / "
                "CMake cache if you did not intend a sanitizer build.\n"
                "Emergency bypass (expect link/runtime failure): "
                "-DSMATCHET_VERIFY_SANITIZER_RUNTIME=OFF")
        endif()
    endforeach()
endfunction()
