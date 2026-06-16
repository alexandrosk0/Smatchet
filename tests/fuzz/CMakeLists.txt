# libFuzzer drivers over Smatchet's untrusted-byte parsers (Slice E2 —
# testing-surface.md §6 P1 Gap 3). Built ONLY under the ninja-fuzzer-linux preset
# (SMATCHET_SANITIZER=fuzzer); the parent tests/CMakeLists.txt early-returns into
# here. Each target links its parser's minimal ImGui-free .cpp closure, applies
# -fsanitize=fuzzer,address,undefined via smatchet_apply_sanitizers() (libFuzzer
# supplies main()), and registers a -runs=0 smoke ctest: it loads the seed corpus
# + runs libFuzzer init and exits 0 — proving the binary is well-formed without a
# long fuzz session. The nightly fuzz-smoke.yml job does the real time-boxed run.
#
# The remaining five parsers ship across two PRs (no preset/workflow edits — the
# CI lane auto-discovers fuzz_* binaries): PR 2a adds cpp_lex, callstack, and
# markdown_adf (below); PR 2b adds ai_sse and ai_ndjson.

set(_fuzz_core "${CMAKE_SOURCE_DIR}/Source/Core")

# smatchet_add_fuzz_target(<name>
#     SOURCES  <driver.cpp> [closure .cpp ...]   # driver + the parser's .cpp closure
#     INCLUDES <dir> ...                          # PRIVATE include dirs
#     LIBS     <target> ...                       # optional PRIVATE link libs (md4c, nlohmann)
#     CORPUS   <subdir>)                          # optional seed dir under corpus/
function(smatchet_add_fuzz_target NAME)
    cmake_parse_arguments(FUZZ "" "CORPUS" "SOURCES;INCLUDES;LIBS" ${ARGN})
    add_executable(${NAME} ${FUZZ_SOURCES})
    target_include_directories(${NAME} PRIVATE ${FUZZ_INCLUDES})
    if(FUZZ_LIBS)
        target_link_libraries(${NAME} PRIVATE ${FUZZ_LIBS})
    endif()
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

# --- PR 2a -------------------------------------------------------------------

# Shared minimal logger closure. LOG_* routes through a function-local-static
# singleton (Logger::Instance()), so it is safe to call uninitialised during
# fuzzing. TextRedaction.cpp is Logger.cpp's only first-party transitive dep
# (regex + std, ImGui-free); the LOG_AGENT_DEBUG bridge stays behind
# #if defined(SMATCHET_AGENT_DEBUG) (off under the fuzz preset).
set(_fuzz_logger_srcs
    "${_fuzz_core}/src/Logger.cpp"
    "${_fuzz_core}/src/Privacy/TextRedaction.cpp")

# CppLexLine + the callstack-detect/lex pair — annotate syntax highlighter over
# untrusted pasted/annotated source. Closure: CppSyntaxLex.cpp only (algorithm +
# cstring, no Logger).
smatchet_add_fuzz_target(fuzz_cpp_lex
    SOURCES
        fuzz_cpp_lex.cpp
        "${_fuzz_core}/src/Ui/CppSyntaxLex.cpp"
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Ui"
    CORPUS cpp_lex)

# ParseCallstackText — untrusted pasted callstack text -> structured frames.
# Closure: CallstackParser.cpp (regex + cctype + algorithm, no Logger). The header
# pulls Config/ConfigManager.h for PathRemapRule (ApplyPathRemaps is defined in the
# same .cpp), which bare-includes KeybindingsConfig.h + ToolbarConfig.h (under
# include/Config) and <nlohmann/json_fwd.hpp> — hence the extra include dir + the
# nlohmann interface lib, mirroring fuzz_markdown_adf below.
smatchet_add_fuzz_target(fuzz_callstack
    SOURCES
        fuzz_callstack.cpp
        "${_fuzz_core}/src/CallstackParser.cpp"
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Config"
    LIBS
        nlohmann_json::nlohmann_json
    CORPUS callstack)

# MarkdownConvert::MarkdownToAdf — untrusted Markdown (issue descriptions/comments)
# -> ADF JSON via md4c. Closure: MarkdownConvert.cpp + the shared logger srcs;
# links md4c (compiled C parser, propagates its PUBLIC include dir) + nlohmann
# (the ADF document tree type).
smatchet_add_fuzz_target(fuzz_markdown_adf
    SOURCES
        fuzz_markdown_adf.cpp
        "${_fuzz_core}/src/Ui/MarkdownConvert.cpp"
        ${_fuzz_logger_srcs}
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Ui"
    LIBS
        md4c
        nlohmann_json::nlohmann_json
    CORPUS markdown_adf)
