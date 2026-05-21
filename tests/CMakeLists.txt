include(FetchContent)

# doctest — header-only C++ unit test framework. Latest stable v2.4.x.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11)
FetchContent_MakeAvailable(doctest)

# Per-unit linkage: each test target lists the specific Source_Core/*.cpp files
# it exercises. No production target links test code. Refactor to a SmatchetCore
# static lib only if per-test link lists grow unwieldy.
add_executable(SmatchetTests
    test_main.cpp
    Source_Core/MainThreadDispatcherDrain.test.cpp
    Source_Core/JqlProjectScope.test.cpp
    Source_Core/TextMerge.test.cpp
    Source_Core/JsonParseUtil.test.cpp
    Source_Core/TrackerFieldValueParser.test.cpp
    Source_Core/TrackerFieldValueParser.extended.test.cpp
    Source_Core/TrackerFieldValueUtils.test.cpp
    Source_Core/OfflineQueueReplayPolicy.test.cpp
    Source_Core/SmatchetThemeSyntaxColors.test.cpp
    # AI chat Claude-Desktop-parity Phase 5 — AI chat-panel palette per theme.
    # Mirrors SmatchetThemeSyntaxColors.test.cpp; same SmatchetTheme.cpp link.
    Source_Core/SmatchetThemeAiColors.test.cpp
    Source_Core/MarkdownPreviewLangTag.test.cpp
    Source_Core/MarkdownConvertAdf.test.cpp
    Source_Core/IssueDraft.test.cpp
    Source_Core/IssueCreatePipeline.test.cpp
    Source_Core/CallstackParser.test.cpp
    Source_Core/LocalCacheManager.test.cpp
    # AI chat Claude-Desktop-parity Phase 3 — SQLite-backed chat-history CRUD
    # against the same LCM SQLite path. Production target globs LCM.cpp, so the
    # test rig only needs the test TU itself (no separate cpp link).
    Source_Core/LocalCacheManagerChat.test.cpp
    Source_Core/BackendAuditTrail.test.cpp
    Source_Core/IssueCreatePipelineIntegration.test.cpp
    Source_Core/P4BlameParse.test.cpp
    Source_Core/TrackerLabelsPure.test.cpp
    Source_Core/TrackerDateTimePure.test.cpp
    Source_Core/TicketFieldEditorLongTextPure.test.cpp
    Source_Core/TrackerFieldPayloadPure.test.cpp
    Source_Core/TrackerFieldCatalogPure.test.cpp
    Source_Core/TicketSyncService.test.cpp
    Source_Core/OfflineQueueServiceRuntime.test.cpp
    Source_Core/ConfigManager.test.cpp
    Source_Core/ConfigMigration.test.cpp
    Source_Core/SingleClickEditConfig.test.cpp
    Source_Core/AiSseParser.test.cpp
    Source_Core/AgentsMdLoader.test.cpp
    Source_Core/AiClientFactory.test.cpp
    Source_Core/AiNdjsonParser.test.cpp
    Source_Core/AiContextBuilder.test.cpp
    Source_Core/AiErrorRedact.test.cpp
    Source_Core/AiModelCatalog.test.cpp
    Source_Core/AiModelSignature.test.cpp
    Source_Core/AiPrefsValidator.test.cpp
    Source_Core/AiEndpointSanitize.test.cpp
    Source_Core/CliArgCoercion.test.cpp
    Source_Core/DictationInsertionRouter.test.cpp
    Source_Core/AiAssistantInputSeedDecision.test.cpp
    # AI chat Claude-Desktop-parity Phase 1 — pure relative/absolute time
    # formatter for the hover action row. No ImGui dep; production TU is
    # auto-globbed into CORE_SOURCES by the root CMakeLists, this entry
    # links it into the doctest rig.
    Source_Core/AiChatTimestamp.test.cpp
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiChatTimestamp.cpp"
    Source_Core/MarkdownLanguageDefinition.test.cpp
    # MarkdownLanguageDefinition.test.cpp exercises the LD static factories
    # declared in TextEditor.h and defined in the vendored ImGuiColorTextEdit
    # TU. Link it into the test rig so the symbols resolve.
    "${CMAKE_SOURCE_DIR}/Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp"
    # code-syntax-coloring-and-tooltips § Slice 1 — CodeColorView substrate.
    # The .cpp is auto-globbed into CORE_SOURCES by the root CMakeLists but
    # the test rig links a curated subset of Source_Core/src/*.cpp, so the
    # CodeColorView TU has to be named here. SmatchetTheme.cpp + TextEditor.cpp
    # already linked above. `DrawColoredCppText` symbol resolves via the
    # pre-existing local stub in `Source_Core/MarkdownPreviewPlan.test.cpp`
    # (the same stub that lets MarkdownPreviewRender link without pulling
    # SmatchetImGuiFonts/CppSyntaxHighlight). CodeColorView's Tokenize tests
    # never exercise the C++/C `DrawColoredCppText` delegation path, so the
    # stub is sufficient.
    Source_Core/CodeColorViewLang.test.cpp
    "${CMAKE_SOURCE_DIR}/Source_Core/src/CodeColorView.cpp"
    # Slice 4 — pure-logic coverage of SelectableText::GetSelectedText
    # (doc-order range walk + reverse normalisation + block-boundary
    # newline insertion). Constructs a synthetic Context directly so no
    # ImGui context is required.
    Source_Core/SelectableTextRun.test.cpp
    "${CMAKE_SOURCE_DIR}/Source_Core/src/SelectableTextRun.cpp"
    # Perf Opt 1 + Opt B — BuildPlan / HashContent / PreviewPlan API surface.
    # Pure-logic only (the ImGui-side render is bucket E). Links the same
    # MarkdownPreviewRender.cpp the production targets compile.
    Source_Core/MarkdownPreviewPlan.test.cpp
    "${CMAKE_SOURCE_DIR}/Source_Core/src/MarkdownPreviewRender.cpp"
    Source_Core/SubprocessCapturePure.test.cpp
    Source_Core/SubprocessCapture.test.cpp
    "${CMAKE_SOURCE_DIR}/Source_Core/src/SubprocessCapturePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/SubprocessCapture.cpp"
    Plugins/Mcp/McpRequestParser.test.cpp
    Plugins/Mcp/McpEnvelope.test.cpp
    Plugins/Mcp/McpToolSchemas.test.cpp
    Plugins/Mcp/McpDispatch.test.cpp
    "${CMAKE_SOURCE_DIR}/Plugins/Mcp/McpJsonRpcPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/CallstackParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/P4BlameParse.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerLabelsPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerDateTimePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TicketFieldEditorLongTextPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerFieldPayloadPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerFieldCatalogPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/JqlProjectScope.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TextMerge.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerFieldValueParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerFieldValueUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/IssueDraft.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/IssueCreatePipeline.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/IssueCreatePipelineHelpers.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerFieldPayload.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/CompactDateFormat.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/UiPerfMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/MarkdownConvert.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/LocalCacheManager.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/BackendAuditTrail.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/Logger.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/ConfigManager_PathUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/ConfigManager_Views.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/SmatchetTheme.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TicketSyncService.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerHttpUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/NetworkUsageTracker.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/OfflineQueueService.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/Views.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/SmatchetToast.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/SmatchetLocalization.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiSseParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiNdjsonParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiClientFactory.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiErrorRedact.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiEndpointSanitize.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/OpenAiClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AnthropicClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/OllamaClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AgentsMdLoader.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiContextBuilder.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiModelCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiModelSignature.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiPrefsValidator.cpp"
    "${CMAKE_SOURCE_DIR}/Target_Standalone/CliArgCoercion.cpp"
)

# Whisper dictation router — link the production TU that matches the parent
# SMATCHET_WITH_WHISPER state so the test exe sees the same bindings/stubs
# split as the standalone build. The test file itself compile-switches on
# SMATCHET_WITH_WHISPER (always-compiled — covers both gating states).
if(SMATCHET_WITH_WHISPER)
    target_sources(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Whisper.cpp")
    target_compile_definitions(SmatchetTests PRIVATE SMATCHET_WITH_WHISPER=1)
else()
    target_sources(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Stubs.cpp")
    # Explicit =0 so the test TU's value-based `#if SMATCHET_WITH_WHISPER` guard
    # evaluates deterministically (matches the ON path's =1 shape).
    target_compile_definitions(SmatchetTests PRIVATE SMATCHET_WITH_WHISPER=0)
endif()



target_include_directories(SmatchetTests PRIVATE _helpers)
target_compile_definitions(SmatchetTests PRIVATE
    "SMATCHET_TESTS_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")

# Phase 3 of ai-chat-claude-desktop-parity. `LocalCacheManager`'s AI-chat
# CRUD surface (`AppendChatMessage` / `LoadChatMessages` / ...) is gated on
# `SMATCHET_WITH_AI` in `LocalCacheManager.cpp`. The chat doctest TU
# (`LocalCacheManagerChat.test.cpp`) self-gates with the same guard so the
# rig keeps building when AI is disabled; mirror the parent option here so
# both surfaces toggle together. Matches the `SMATCHET_WITH_WHISPER` /
# `SMATCHET_WITH_AGENTIC` precedent above.
if(SMATCHET_WITH_AI)
    target_compile_definitions(SmatchetTests PRIVATE SMATCHET_WITH_AI=1)
endif()

# Whisper Phase B — pure helper test surfaces. WhisperApiKeyResolve + WavWriter
# have no third-party deps; WhisperApiClient pulls cpr (already linked) +
# AiErrorRedact (already in CORE sources above) for the multipart Post path.
# Tests cover the pure helpers only — Transcribe() itself is exercised by the
# CLI smoke + future bucket-E scenario, not by ctest (the doctest rig doesn't
# fire live HTTP).
if(SMATCHET_WITH_WHISPER)
    target_sources(SmatchetTests PRIVATE
        Source_Core/WhisperApiKeyResolve.test.cpp
        Source_Core/WhisperApiPayload.test.cpp
        Source_Core/WavWriter.test.cpp
        # Phase C — local backend, downloader catalog, consent gate, mode router.
        Source_Core/ModelCatalog.test.cpp
        Source_Core/WhisperConsentGate.test.cpp
        Source_Core/WhisperModeRouter.test.cpp
        # Phase E — pure hotkey-descriptor parser.
        Source_Core/HotkeyParse.test.cpp
        # Phase F — silence-trim helper + config-fields round-trip.
        Source_Core/SilenceTrim.test.cpp
        Source_Core/WhisperPrefsFields.test.cpp
        Source_Core/WhisperMockSeams.test.cpp
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/WhisperApiKeyResolve.cpp"
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/WhisperApiClient.cpp"
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/WavWriter.cpp"
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/ModelCatalog.cpp"
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/WhisperConsentGate.cpp"
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/HotkeyParse.cpp"
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper/SilenceTrim.cpp"
    )
    target_include_directories(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Plugins/Whisper")
endif()

target_include_directories(SmatchetTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source_Core/include"
    "${CMAKE_SOURCE_DIR}/Source_Core/ThirdParty/ImGuiColorTextEdit"
    "${CMAKE_SOURCE_DIR}/Plugins/Mcp"
    "${CMAKE_SOURCE_DIR}/Target_Standalone"
    "${CMAKE_SOURCE_DIR}/tests/support"
)

# SmatchetTheme links ImGuiLib for ImGui::GetStyle() / CreateContext().
# SQLiteCpp is linked at runtime as of Phase 3 — LocalCacheManager.cpp + IssueCreatePipeline.cpp
# pull SQLite/Statement objects directly. md4c is linked at runtime so MarkdownConvert.cpp
# (transitively used by TrackerFieldPayload) resolves.
target_link_libraries(SmatchetTests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
    ImGuiLib
    SQLiteCpp
    md4c
    cpr::cpr
    ghc_filesystem
)

# ConfigManager.cpp uses CryptProtectData / CryptBinaryToStringA from wincrypt.
# Linked here because the test exe pulls ConfigManager via TrackerFieldValueUtils.
if(WIN32)
    target_link_libraries(SmatchetTests PRIVATE crypt32)
endif()

target_compile_features(SmatchetTests PRIVATE cxx_std_14)

# Static-link the C++ + GCC runtime so ctest can run SmatchetTests.exe from any
# directory (PATH-independent). Mirrors what SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME
# does for SmatchetStandalone in the publish preset.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(SmatchetTests PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

# Helper executables for SubprocessCapture.test.cpp (H1 — agentic-flow
# handoff). These are tiny stdlib-only binaries the doctest spawns to
# exercise exit codes, timeouts, cancel tokens, and stdout caps without
# depending on platform shell builtins. The resolved binary directory is
# threaded into SmatchetTests as a compile-time string so the test
# discovers the helpers without env vars (per the H1 plan's locked
# decision #10 in agentic-coding-handoff.md).
add_executable(subproc_helper_exit _helpers/subproc_helper_exit.cpp)
add_executable(subproc_helper_sleep _helpers/subproc_helper_sleep.cpp)
add_executable(subproc_helper_flood _helpers/subproc_helper_flood.cpp)
target_compile_features(subproc_helper_exit PRIVATE cxx_std_14)
target_compile_features(subproc_helper_sleep PRIVATE cxx_std_14)
target_compile_features(subproc_helper_flood PRIVATE cxx_std_14)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(subproc_helper_exit PRIVATE -static-libgcc -static-libstdc++ -static)
    target_link_options(subproc_helper_sleep PRIVATE -static-libgcc -static-libstdc++ -static)
    target_link_options(subproc_helper_flood PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
add_dependencies(SmatchetTests subproc_helper_exit subproc_helper_sleep subproc_helper_flood)
target_compile_definitions(SmatchetTests PRIVATE
    "SMATCHET_TEST_HELPER_DIR=\"$<TARGET_FILE_DIR:subproc_helper_exit>\"")



add_test(NAME smatchet_tests COMMAND SmatchetTests)
