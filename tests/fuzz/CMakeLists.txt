# libFuzzer drivers over Smatchet's untrusted-byte parsers (Slice E2 —
# testing-surface.md §6 P1 Gap 3). Built ONLY under the ninja-fuzzer-linux preset
# (SMATCHET_SANITIZER=fuzzer); the parent tests/CMakeLists.txt early-returns into
# here. Each target links its parser's minimal ImGui-free .cpp closure, applies
# -fsanitize=fuzzer,address,undefined via smatchet_apply_sanitizers() (libFuzzer
# supplies main()), and registers a -runs=0 smoke ctest: it loads the seed corpus
# + runs libFuzzer init and exits 0 — proving the binary is well-formed without a
# long fuzz session. The nightly fuzz-smoke.yml job does the real time-boxed run.
#
# PR 2 (Slice E2b/E2c) adds the remaining five drivers (cpp_lex, markdown_adf,
# ai_sse, ai_ndjson, callstack) via more smatchet_add_fuzz_target() calls below —
# no preset or workflow edits needed (the CI lane auto-discovers fuzz_* binaries).

set(_fuzz_core "${CMAKE_SOURCE_DIR}/Source/Core")

# smatchet_add_fuzz_target(<name>
#     SOURCES  <driver.cpp> [closure .cpp ...]   # driver + the parser's .cpp closure
#     INCLUDES <dir> ...                          # PRIVATE include dirs
#     CORPUS   <subdir>)                          # optional seed dir under corpus/
function(smatchet_add_fuzz_target NAME)
    cmake_parse_arguments(FUZZ "" "CORPUS" "SOURCES;INCLUDES" ${ARGN})
    add_executable(${NAME} ${FUZZ_SOURCES})
    target_include_directories(${NAME} PRIVATE ${FUZZ_INCLUDES})
    target_compile_features(${NAME} PRIVATE cxx_std_14)
    smatchet_apply_sanitizers(${NAME})
    if(FUZZ_CORPUS)
        add_test(NAME ${NAME}_smoke
            COMMAND ${NAME} -runs=0 "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${FUZZ_CORPUS}")
    else()
        add_test(NAME ${NAME}_smoke COMMAND ${NAME} -runs=0)
    endif()
endfunction()

# image_dim::ParseImageDimensionsFromBytes — PNG/GIF/WEBP/JPEG header parser over
# attacker-controlled attachment bytes. Closure: ImageDimensionsPure.cpp only
# (AttachmentMimeUtils.h is header-only).
smatchet_add_fuzz_target(fuzz_image_dims
    SOURCES
        fuzz_image_dims.cpp
        "${_fuzz_core}/src/Ui/ImageDimensionsPure.cpp"
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Ui"
    CORPUS image_dims)
