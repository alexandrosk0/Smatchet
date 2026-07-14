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
# markdown_adf (below); PR 2b adds ai_sse and ai_ndjson. PR 3 (Slice E2c) adds the
# tracker-response *consuming* layer: github_map, plane_map, linear_map — the pure
# JSON -> CachedTicket mappers that walk an already-ParseBounded DOM.

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
        # MarkdownConvert god-file split (god-file-splits plan): the ADF entry point in
        # the spine delegates to md_detail:: engines defined in these companion TUs.
        "${_fuzz_core}/src/Ui/MarkdownToAdf.cpp"
        "${_fuzz_core}/src/Ui/MarkdownToHtml.cpp"
        "${_fuzz_core}/src/Ui/AdfToMarkdown.cpp"
        ${_fuzz_logger_srcs}
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Ui"
        "${_fuzz_core}/include/Privacy"
    LIBS
        md4c
        nlohmann_json::nlohmann_json
    CORPUS markdown_adf)

# --- PR 2b -------------------------------------------------------------------
# Both AI-stream drivers compile ${_fuzz_logger_srcs}, so both need
# include/Privacy: TextRedaction.cpp bare-includes "TextRedaction.h" (the header
# lives under include/Privacy/). The markdown target above carries the same dir
# for the same reason. No include/Ui — neither parser nor the logger srcs touch Ui.

# AiSseParser::Feed — untrusted Server-Sent-Events byte stream (OpenAI/Anthropic
# chat completions) -> framed events. Closure: AiSseParser.cpp + the shared logger
# srcs (LOG_ERROR on the 4 MiB cap-exceeded path). No nlohmann (Event is plain
# std::string).
smatchet_add_fuzz_target(fuzz_ai_sse
    SOURCES
        fuzz_ai_sse.cpp
        "${_fuzz_core}/src/AiSseParser.cpp"
        ${_fuzz_logger_srcs}
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Privacy"
    CORPUS ai_sse)

# AiNdjsonParser::Feed — untrusted newline-delimited JSON byte stream (Ollama
# /api/chat) -> parsed objects. Closure: AiNdjsonParser.cpp + the shared logger
# srcs; links nlohmann (the parsed line type, included by the parser header).
smatchet_add_fuzz_target(fuzz_ai_ndjson
    SOURCES
        fuzz_ai_ndjson.cpp
        "${_fuzz_core}/src/AiNdjsonParser.cpp"
        ${_fuzz_logger_srcs}
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Privacy"
    LIBS
        nlohmann_json::nlohmann_json
    CORPUS ai_ndjson)

# --- PR 3 -------------------------------------------------------------------
# Tracker-response *consuming* layer (Slice E2c). The raw json::parse on every
# tracker HTTP body is already depth/node-bounded by json_safe::ParseBounded (the
# bare-json-parse-untrusted lint), so these three drivers fuzz the layer ABOVE the
# parser: the pure JSON -> CachedTicket mappers that walk an already-parsed DOM
# with structural assumptions (labels as objects-or-strings, detail-vs-flat
# fallbacks, nested state/assignee sub-objects, string-sliced url ids). Each maps
# a single tracker backend and links only its cpr-free `*MappingPure.cpp` closure
# (the network TU that pulls cpr / ConfigManager / SQLite stays out) + nlohmann.
# The driver feeds fuzz bytes through ParseBounded first — exactly what the
# production fetcher hands the mapper — then drives every pure entry point.

# smatchet::github::Map*JsonToCachedTicket + GraphQL->REST adapters over untrusted
# GitHub REST/GraphQL issue/PR/commit bodies. Closure: GitHubIssueSearchMapping.cpp
# only (deliberately Logger-free for doctest purity — no logger srcs needed).
smatchet_add_fuzz_target(fuzz_github_map
    SOURCES
        fuzz_github_map.cpp
        "${_fuzz_core}/src/Tracker/GitHubIssueSearchMapping.cpp"
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Tracker"
    LIBS
        nlohmann_json::nlohmann_json
    CORPUS github_map)

# smatchet::plane::MapPlaneWorkItem*ToCachedTicket + pagination/query helpers over
# untrusted Plane REST work-item bodies (the richest detail-vs-flat type-confusion
# surface). Closure: PlaneIssueMappingPure.cpp (+ the header-only PlaneJsonFieldPure.h
# it includes from src/Tracker — hence that dir on the include path).
smatchet_add_fuzz_target(fuzz_plane_map
    SOURCES
        fuzz_plane_map.cpp
        "${_fuzz_core}/src/Tracker/PlaneIssueMappingPure.cpp"
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Tracker"
        "${_fuzz_core}/src/Tracker"
    LIBS
        nlohmann_json::nlohmann_json
    CORPUS plane_map)

# smatchet::linear::MapLinearIssueNode(s)ToCachedTicket over untrusted Linear
# GraphQL issue nodes. Closure: LinearIssueMappingPure.cpp only (header + <string>,
# no logger). Robustness check on its documented never-throw Pillar-3 contract.
smatchet_add_fuzz_target(fuzz_linear_map
    SOURCES
        fuzz_linear_map.cpp
        "${_fuzz_core}/src/Tracker/LinearIssueMappingPure.cpp"
    INCLUDES
        "${_fuzz_core}/include"
        "${_fuzz_core}/include/Tracker"
    LIBS
        nlohmann_json::nlohmann_json
    CORPUS linear_map)

# --- nightly-monkey-tester Layer 3 ------------------------------------------
# Three security-sensitive pure string parsers with clean single-.cpp closures (no
# Logger / nlohmann / cpr) — the raw-string trust-boundary functions that scripted
# tests exercise with a handful of cases but never with adversarial bytes.

# tracker_jql::QuoteLiteral — escapes a foreign value before it is spliced into a
# double-quoted JQL literal (JQL-injection boundary, security H3 + E1). Closure:
# JqlEscape.cpp only (own header + <string>). Raw-string entry point.
smatchet_add_fuzz_target(fuzz_jql_escape
    SOURCES
        fuzz_jql_escape.cpp
        "${_fuzz_core}/src/Tracker/JqlEscape.cpp"
    INCLUDES
        "${_fuzz_core}/include"
    CORPUS jql_escape)

# smatchet::ai::pure::RedactProviderErrorBody — strips secrets/ids from an untrusted
# AI-provider error body before it reaches UI/logs (also redacts nlohmann parse-error
# text, #1286). Closure: AiErrorRedact.cpp only (own header + <cctype>/<cstring>).
smatchet_add_fuzz_target(fuzz_ai_error_redact
    SOURCES
        fuzz_ai_error_redact.cpp
        "${_fuzz_core}/src/AiErrorRedact.cpp"
    INCLUDES
        "${_fuzz_core}/include"
    CORPUS ai_error_redact)

# smatchet::ai::pure::SanitizeAiEndpointUrl + ExtractUrlHost — validates an
# attacker-controlled AI endpoint URL against config-write SSRF (cloud-metadata /
# link-local / RFC1918 / header-smuggling). The densest raw-URL byte surface in the
# AI layer. Closure: AiEndpointSanitize.cpp only (own header + std).
smatchet_add_fuzz_target(fuzz_ai_endpoint_sanitize
    SOURCES
        fuzz_ai_endpoint_sanitize.cpp
        "${_fuzz_core}/src/AiEndpointSanitize.cpp"
    INCLUDES
        "${_fuzz_core}/include"
    CORPUS ai_endpoint_sanitize)

# smatchet::l10n::ConversionSpecifiers / FormatSpecifiersMatch — the printf
# conversion-specifier guard that gates an attacker-influenceable Locales/<lang>.json
# override before it reaches vsnprintf in SmatchetLocalization::Format (format-string
# / arbitrary-write boundary, SECURITY_AUDIT.md #1). Closure: header-only
# (SmatchetLocalizationFormatGuard.h + std) — no SmatchetLocalization.cpp
# (ConfigManager/Logger) needed. Raw-string entry point.
smatchet_add_fuzz_target(fuzz_locale_format_guard
    SOURCES
        fuzz_locale_format_guard.cpp
    INCLUDES
        "${_fuzz_core}/include"
    CORPUS locale_format_guard)

# --- command-input-hardening (shared bounded-JSON ingress) -------------------

# smatchet::json_safe::ParseBounded / ParseBoundedOrDiscarded — the single
# depth/node/byte-bounded parse every attacker-controlled JSON ingress routes
# through (Lua decode_json, MCP REST/JSON-RPC POST, Lua-MCP-tool params, and the
# command-arg `ParamType::Json` coercion + `config.set` value parse added by the
# command-input-hardening plan). Its Pillar-3 contract: no throw escapes, and a
# depth-bomb is rejected BEFORE nlohmann builds a deep DOM that overflows on
# recursive teardown. Header-only closure (BoundedJsonParse.h + nlohmann) — no
# first-party .cpp; links only the nlohmann interface lib.
smatchet_add_fuzz_target(fuzz_bounded_json
    SOURCES
        fuzz_bounded_json.cpp
    INCLUDES
        "${_fuzz_core}/include"
    LIBS
        nlohmann_json::nlohmann_json
    CORPUS bounded_json)

# --- hand-rolled-parser fuzz gap (CPP_CODE_AUDIT.md #3/#17/#19) --------------

# TicketGridDurationSortPure::ParseDurationToSecondsForSort — the UI-thread sort comparator's
# time-tracking duration parser over tracker-supplied (untrusted) cell values. A non-unit char
# after a number used to stall the cursor forever (permanent UI freeze, #3) and a huge magnitude
# used to overflow long long (#19); both fixed, this keeps them fixed. Closure: the pure .cpp only
# (<cctype>/<limits>/<string>, no Logger/ImGui).
smatchet_add_fuzz_target(fuzz_duration_parse
    SOURCES
        fuzz_duration_parse.cpp
        "${_fuzz_core}/src/TicketGridDurationSortPure.cpp"
    INCLUDES
        "${_fuzz_core}/include"
    CORPUS duration_parse)

# smatchet::version::ParseSemanticVersion — parses a GitHub release tag straight from an
# update-check HTTP response (untrusted ingress). std::atoi on a digit run past INT_MAX is UB
# (#17); each segment is all-digit-gated then range-checked via std::stoll. Closure: the pure
# .cpp only (extracted from AttachmentAppUpdateService so no cpr/Logger is dragged in).
smatchet_add_fuzz_target(fuzz_semver_parse
    SOURCES
        fuzz_semver_parse.cpp
        "${_fuzz_core}/src/SemanticVersionPure.cpp"
    INCLUDES
        "${_fuzz_core}/include"
    CORPUS semver_parse)
