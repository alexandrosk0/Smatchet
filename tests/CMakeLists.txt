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
    Source_Core/JqlProjectScope.test.cpp
    Source_Core/TextMerge.test.cpp
    Source_Core/JsonParseUtil.test.cpp
    Source_Core/TrackerFieldValueParser.test.cpp
    Source_Core/TrackerFieldValueParser.extended.test.cpp
    Source_Core/TrackerFieldValueUtils.test.cpp
    Source_Core/OfflineQueueReplayPolicy.test.cpp
    Source_Core/SmatchetThemeSyntaxColors.test.cpp
    Source_Core/MarkdownPreviewLangTag.test.cpp
    Source_Core/IssueDraft.test.cpp
    Source_Core/IssueCreatePipeline.test.cpp
    Source_Core/CallstackParser.test.cpp
    Source_Core/LocalCacheManager.test.cpp
    Source_Core/BackendAuditTrail.test.cpp
    Source_Core/IssueCreatePipelineIntegration.test.cpp
    Source_Core/P4BlameParse.test.cpp
    Source_Core/TrackerLabelsPure.test.cpp
    Source_Core/TrackerDateTimePure.test.cpp
    Source_Core/TrackerFieldPayloadPure.test.cpp
    Source_Core/TrackerFieldCatalogPure.test.cpp
    Source_Core/TicketSyncService.test.cpp
    Source_Core/OfflineQueueServiceRuntime.test.cpp
    Source_Core/ConfigManager.test.cpp
    Source_Core/ConfigMigration.test.cpp
    Source_Core/AiSseParser.test.cpp
    Source_Core/AiClientFactory.test.cpp
    Source_Core/AiNdjsonParser.test.cpp
    Source_Core/AgentsMdLoader.test.cpp
    Source_Core/AiContextBuilder.test.cpp
    Source_Core/AiErrorRedact.test.cpp
    Source_Core/AiModelCatalog.test.cpp
    Source_Core/AiPrefsValidator.test.cpp
    Source_Core/AiEndpointSanitize.test.cpp
    Plugins/Mcp/McpRequestParser.test.cpp
    Plugins/Mcp/McpEnvelope.test.cpp
    Plugins/Mcp/McpToolSchemas.test.cpp
    Plugins/Mcp/McpDispatch.test.cpp
    "${CMAKE_SOURCE_DIR}/Plugins/Mcp/McpJsonRpcPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/CallstackParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/P4BlameParse.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerLabelsPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source_Core/src/TrackerDateTimePure.cpp"
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
    "${CMAKE_SOURCE_DIR}/Source_Core/src/AiPrefsValidator.cpp"
)

target_include_directories(SmatchetTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source_Core/include"
    "${CMAKE_SOURCE_DIR}/Plugins/Mcp"
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

add_test(NAME smatchet_tests COMMAND SmatchetTests)
