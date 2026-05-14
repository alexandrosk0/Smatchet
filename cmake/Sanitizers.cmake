# Sanitizers.cmake — apply ASan / UBSan / TSan / MSan flags to a target based
# on the SMATCHET_SANITIZER cache variable.
#
# Default behaviour is a no-op (SMATCHET_SANITIZER unset or "off"). Sanitizer
# presets in CMakePresets.json set the cache variable; debug-detective picks
# a preset per investigation type.
#
# Compiler matrix (verify if symptoms arise):
#   asan / ubsan : MSYS2 UCRT64 GCC supported; clang supported.
#   tsan         : MinGW-w64 GCC support is partial / version-dependent.
#                  Linux clang/gcc reliable. Wire the preset; if it fails
#                  hand off to build-doctor.
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

    if("${SMATCHET_SANITIZER}" STREQUAL "asan")
        list(APPEND _flags
            -fsanitize=address
            -fsanitize=undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        list(APPEND _link
            -fsanitize=address
            -fsanitize=undefined)
    elseif("${SMATCHET_SANITIZER}" STREQUAL "tsan")
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
    target_link_options(${tgt}    PRIVATE ${_link})
endfunction()
