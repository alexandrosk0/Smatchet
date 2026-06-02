include(FetchContent)

# doctest — header-only C++ unit test framework. Latest stable v2.4.x.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11)
FetchContent_MakeAvailable(doctest)

# Per-unit linkage: each test target lists the specific Core/*.cpp files
# it exercises. No production target links test code. Refactor to a SmatchetCore
# static lib only if per-test link lists grow unwieldy.
add_executable(SmatchetTests
    test_main.cpp
    Core/AnnotateAnalysisConfig.test.cpp
    Core/AnnotateLocalization.test.cpp
    Core/ConfigSaveConcurrency.test.cpp
    Core/MainThreadDispatcherDrain.test.cpp
    Core/CacheEvictionPolicy.test.cpp
    Core/MemoryTelemetry.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/MemoryTelemetry.cpp"
    Core/BackgroundWorkerReap.test.cpp
    Core/JqlProjectScope.test.cpp
    # Per-project Jira component dropdowns (PR #672) — pure coverage of issue-key-prefix
    # extraction, distinct-project collection, and per-project option-map lookup.
    # ProjectResolver.cpp depends only on the header-only ITrackerConnectivity interface
    # (no cpr/sqlite), safe in the doctest rig.
    Core/ProjectComponentResolution.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/ProjectResolver.cpp"
    Core/TextMerge.test.cpp
    Core/CachedTicketFieldValueRef.test.cpp
    Core/JsonParseUtil.test.cpp
    Core/TrackerFieldValueParser.test.cpp
    Core/TrackerFieldValueParser.extended.test.cpp
    Core/TrackerFieldValueUtils.test.cpp
    # Fix C of jira-components-multiselect (PR #672) — scoped catalog save→load round-trip
    # asserting `components.AllowedValueOptions` survive under the project-scoped cache key
    # (and that the unscoped key never sees them). FieldCatalogCache.cpp depends only on
    # ConfigManager (already linked) + nlohmann — no cpr/sqlite, safe in the doctest rig.
    Core/FieldCatalogCacheScopedRoundTrip.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/FieldCatalogCache.cpp"
    Core/OfflineQueueReplayPolicy.test.cpp
    Core/SmatchetThemeSyntaxColors.test.cpp
    # AI chat Claude-Desktop-parity Phase 5 — AI chat-panel palette per theme.
    # Mirrors SmatchetThemeSyntaxColors.test.cpp; same SmatchetTheme.cpp link.
    Core/SmatchetThemeAiColors.test.cpp
    # Regression guard for the inverted Norton Commander palette (blue-dominant, NC 2.01 menu).
    Core/SmatchetThemeNortonPalette.test.cpp
    Core/MarkdownPreviewLangTag.test.cpp
    Core/MarkdownConvertAdf.test.cpp
    Core/MarkdownConvert.test.cpp
    Core/IssueDraft.test.cpp
    Core/IssueCreatePipeline.test.cpp
    Core/CallstackParser.test.cpp
    Core/LocalCacheManager.test.cpp
    # AI chat Claude-Desktop-parity Phase 3 — SQLite-backed chat-history CRUD
    # against the same LCM SQLite path. Production target globs LCM.cpp, so the
    # test rig only needs the test TU itself (no separate cpp link).
    Core/LocalCacheManagerChat.test.cpp
    Core/BackendAuditTrail.test.cpp
    Core/IssueCreatePipelineIntegration.test.cpp
    Core/P4AnnotateParse.test.cpp
    # Slice 3 of autonomous-debugging-no-creds — runner-seam fake exercises
    # P4Annotate.cpp's P4RunCommand path via AnnotateAnalysisConfig::P4RunOverride,
    # without spawning the real `p4` binary. FakeP4Runner.h lives in
    # tests/support/ (already on the include path).
    Core/P4AnnotateE2E.test.cpp
    Core/P4DescribeCacheE2E.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/P4Annotate.cpp"
    Core/TrackerLabelsPure.test.cpp
    Core/TrackerDateTimePure.test.cpp
    Core/TicketFieldEditorLongTextPure.test.cpp
    Core/IsDescriptionLikeFieldId.test.cpp
    Core/TrackerFieldPayloadPure.test.cpp
    Core/TrackerFieldCatalogPure.test.cpp
    Core/TicketSyncService.test.cpp
    Core/OfflineQueueServiceRuntime.test.cpp
    Core/OfflineFieldEditMerge.test.cpp
    Core/ConfigManager.test.cpp
    Core/ConfigMigration.test.cpp
    Core/ToolbarConfig.test.cpp
    Core/IconCatalog.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetIconCatalog.cpp"
    Core/SingleClickEditConfig.test.cpp
    Core/AiSseParser.test.cpp
    # autonomous-debugging-no-creds § Slice 4 — verifies the StubAiClient
    # test-infrastructure header (tests/support/StubAiClient.h) honours its
    # AiCancelToken contract. Pure C++14 — no HTTP, no cpr, no production
    # AI client. Header-only stub; no extra .cpp source.
    Core/StubAiClientCancel.test.cpp
    Core/AgentsMdLoader.test.cpp
    Core/AiClientFactory.test.cpp
    Core/AiNdjsonParser.test.cpp
    Core/AiContextBuilder.test.cpp
    Core/AiErrorRedact.test.cpp
    Core/AiModelCatalog.test.cpp
    Core/AiModelSignature.test.cpp
    # Pure helpers extracted from AiAssistantController::RunRequest (monoliths
    # decomposition). Header-only — the helpers are `inline` in
    # AiAssistantController.h; no extra .cpp source.
    Core/AiAssistantSystemPrompt.test.cpp
    Core/AiPrefsValidator.test.cpp
    Core/AiEndpointSanitize.test.cpp
    Core/CliArgCoercion.test.cpp
    Core/DictationInsertionRouter.test.cpp
    Core/AiAssistantInputSeedDecision.test.cpp
    Core/AiAssistantSendButton.test.cpp
    # AI chat Claude-Desktop-parity Phase 1 — pure relative/absolute time
    # formatter for the hover action row. No ImGui dep; production TU is
    # auto-globbed into CORE_SOURCES by the root CMakeLists, this entry
    # links it into the doctest rig.
    Core/AiChatTimestamp.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiChatTimestamp.cpp"
    # Slice 5 of autonomous-debugging-no-creds — snapshot test pinning the set
    # of scenario names registered by RegisterAllScenarios(). The test calls
    # ListNames() only — never invokes the factory lambdas — so it does not
    # need any scenario implementation TUs linked, only ScenarioRunner.cpp
    # (for ListNames/RegisterFactory), Command.cpp (for CommandResult), and
    # the registry TU itself.
    Core/SmatchetScenarioRegistry.test.cpp
    Core/SmatchetScenarioRegistry.stubs.cpp
    Core/ScenarioCaptureSizing.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/ScenarioCaptureSizing.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/ScenarioRunner.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Command.cpp"
    # PR2 of docs/design/github-tracker-backend.md — pure helpers + label diff.
    Core/LabelEditDiffPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LabelEditDiffPure.cpp"
    Core/GitHubClientHelpers.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubClientHelpers.cpp"
    # docs/plans/shipped/log-a-bug-github.md Slice 2 — pure half of the bug-report
    # service (target/PAT resolution + markdown body) + redaction + censor. The
    # heavy half (BugReportService.cpp: cpr/host/AppController) is NOT linked here.
    Core/BugReportBody.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Diagnostics/BugReportBody.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Privacy/TextRedaction.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Imaging/ScreenshotCensor.cpp"
    # log-a-bug-github Phase 2 — crash marker survival + next-launch consume.
    Core/CrashSink.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Diagnostics/CrashSink.cpp"
    # PR5 of docs/design/github-tracker-backend.md — JQL → GitHub-search translator.
    Core/GitHubQueryFromJql.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubQueryFromJql.cpp"
    # PR4 follow-up — pure helper ComputeGitHubFetchPlan deciding repo-scoped vs
    # cross-repo fetch + the empty-config fallback warning. Pure TU (no cpr).
    Core/GitHubFetchPlan.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubFetchPlan.cpp"
    # PR12 — pure-logic JSON → CachedTicket mapping (PR detection, status
    # encoding, [PR] summary prefix) + per-PR field enrichment helper. Split
    # out of GitHubIssueSearch.cpp (which pulls cpr) so doctest can link it
    # without HTTP.
    Core/GitHubIssueSearchMapping.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubIssueSearchMapping.cpp"
    # Slice 1 of deterministic-jira-test-backend — pure
    # Jira JSON → CachedTicket mapper + field-list builder extracted from
    # JiraIssueSearch.cpp (which pulls cpr). Same pure-seam shape as the
    # Plane and GitHub splits above.
    Core/JiraIssueMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraIssueMappingPure.cpp"
    # Slice 2 of deterministic-jira-test-backend — Jira
    # fixture parser + FakeTrackerClient configurator. Exercises LoadFromString,
    # jiraSearchPages round-trip through AppendCachedTicketFromJiraSearchIssue,
    # multi-fetch sequencing, and mutation reply queues.
    Core/JiraFakeTrackerFixture.test.cpp
    "${CMAKE_SOURCE_DIR}/tests/support/JiraFakeTrackerFixture.cpp"
    # Slice 1 of docs/design/autonomous-debugging-no-creds.md — fixture-driven
    # GitHub mapping coverage (basic mapping / PR-vs-issue filter / assignee +
    # state + labels / pagination boundary). Reuses GitHubIssueSearchMapping.cpp
    # (already linked above for GitHubIssueSearchMapping.test.cpp).
    Core/GitHubIssueMappingPure.test.cpp
    # Slice 2 of docs/design/autonomous-debugging-no-creds.md — pure Plane
    # JSON → CachedTicket mapper extracted out of PlaneIssueSearch.cpp (which
    # pulls cpr). Same shape as the GitHub PR12 split above.
    Core/PlaneIssueMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneIssueMappingPure.cpp"
    # Monoliths Phase A (decompose-top-20-monoliths.md) — pure Plane field-catalog
    # property → TrackerField mapper extracted out of PlaneFieldCatalog.cpp (which
    # pulls cpr). Same pure-seam shape as PlaneIssueMappingPure above.
    Core/PlaneFieldCatalogPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneFieldCatalogPure.cpp"
    Core/MarkdownLanguageDefinition.test.cpp
    # MarkdownLanguageDefinition.test.cpp exercises the LD static factories
    # declared in TextEditor.h and defined in the vendored ImGuiColorTextEdit
    # TU. Link it into the test rig so the symbols resolve.
    "${CMAKE_SOURCE_DIR}/Source/Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp"
    # code-syntax-coloring-and-tooltips § Slice 1 — CodeColorView substrate.
    # The .cpp is auto-globbed into CORE_SOURCES by the root CMakeLists but
    # the test rig links a curated subset of Source/Core/src/*.cpp, so the
    # CodeColorView TU has to be named here. SmatchetTheme.cpp + TextEditor.cpp
    # already linked above. `DrawColoredCppText` symbol resolves via the
    # pre-existing local stub in `Core/MarkdownPreviewPlan.test.cpp`
    # (the same stub that lets MarkdownPreviewRender link without pulling
    # SmatchetImGuiFonts/CppSyntaxHighlight). CodeColorView's Tokenize tests
    # never exercise the C++/C `DrawColoredCppText` delegation path, so the
    # stub is sufficient.
    Core/CodeColorViewLang.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/CodeColorView.cpp"
    # Slice 4 — pure-logic coverage of SelectableText::GetSelectedText
    # (doc-order range walk + reverse normalisation + block-boundary
    # newline insertion). Constructs a synthetic Context directly so no
    # ImGui context is required.
    Core/SelectableTextRun.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SelectableTextRun.cpp"
    # Function-size compliance PR A9-A10 — pure byte-level image-dimension parser
    # extracted from SmatchetAttachmentPreviewUi.cpp's ParseImageDimensions. No
    # ImGui / file-I/O dependency; parses untrusted header bytes, so the test
    # pins every signature/bounds/offset/endianness path (PNG/GIF/WEBP/JPEG).
    Core/ParseImageDimensions.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/ImageDimensionsPure.cpp"
    # Perf Opt 1 + Opt B — BuildPlan / HashContent / PreviewPlan API surface.
    # Pure-logic only (the ImGui-side render is bucket E). Links the same
    # MarkdownPreviewRender.cpp the production targets compile.
    Core/MarkdownPreviewPlan.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/MarkdownPreviewRender.cpp"
    Core/SubprocessCapturePure.test.cpp
    Core/SubprocessCapture.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/SubprocessCapturePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/SubprocessCapture.cpp"
    Plugins/Mcp/McpRequestParser.test.cpp
    Plugins/Mcp/McpEnvelope.test.cpp
    Plugins/Mcp/McpToolSchemas.test.cpp
    Plugins/Mcp/McpDispatch.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Plugins/Mcp/McpJsonRpcPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/CallstackParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/P4AnnotateParse.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerLabelsPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerDateTimePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/TicketFieldEditorLongTextPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldCatalogPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JqlProjectScope.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/TextMerge.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldValueParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldValueUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueDraft.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueCreatePipeline.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueCreatePipelineHelpers.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldPayload.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/CompactDateFormat.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/UiPerfMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/MarkdownConvert.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/LocalCacheManager.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/BackendAuditTrail.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Logger.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_PathUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Views.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ToolbarConfig.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/ConfigSaveWorker.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetTheme.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/TicketSyncService.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerHttpUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/NetworkUsageTracker.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/OfflineQueueService.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/Views.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetToast.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/SmatchetLocalization.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiSseParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiNdjsonParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiClientFactory.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiErrorRedact.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiEndpointSanitize.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/OpenAiClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AnthropicClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/OllamaClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AgentsMdLoader.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiContextBuilder.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiModelCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiModelSignature.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiPrefsValidator.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Standalone/CliArgCoercion.cpp"
    # Natural-order key/issuekey sort — exercises CompareIssueKeyNatural
    # (inline in StringUtil.h) which CompareFieldValuesForSort dispatches to.
    Core/TicketGridModelKeySort.test.cpp
)

# Whisper dictation router — link the production TU that matches the parent
# SMATCHET_WITH_WHISPER state so the test exe sees the same bindings/stubs
# split as the standalone build. The test file itself compile-switches on
# SMATCHET_WITH_WHISPER (always-compiled — covers both gating states).
if(SMATCHET_WITH_WHISPER)
    target_sources(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Source/Core/src/DictationInsertionRouter_Whisper.cpp")
    target_compile_definitions(SmatchetTests PRIVATE SMATCHET_WITH_WHISPER=1)
else()
    target_sources(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Source/Core/src/DictationInsertionRouter_Stubs.cpp")
    # Explicit =0 so the test TU's value-based `#if SMATCHET_WITH_WHISPER` guard
    # evaluates deterministically (matches the ON path's =1 shape).
    target_compile_definitions(SmatchetTests PRIVATE SMATCHET_WITH_WHISPER=0)
endif()



# Slice 7 of autonomous-debugging-no-creds — header-only NDJSON helper test.
# Gated under SMATCHET_BUILD_AGENT_DEBUG_TESTS (default ON when SMATCHET_BUILD_TESTS).
# Forces SMATCHET_AGENT_DEBUG=1 only on the test TU (the production build's
# OFF/ON state is independent — verified by the test's own `#ifndef` guard).
if(SMATCHET_BUILD_AGENT_DEBUG_TESTS)
    target_sources(SmatchetTests PRIVATE
        Core/SmatchetAgentDebug.test.cpp)
    set_source_files_properties(
        Core/SmatchetAgentDebug.test.cpp
        PROPERTIES COMPILE_DEFINITIONS "SMATCHET_AGENT_DEBUG=1")
    target_include_directories(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/_debug")
endif()

target_include_directories(SmatchetTests PRIVATE _helpers)
target_compile_definitions(SmatchetTests PRIVATE
    "SMATCHET_TESTS_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")

# Phase 3 of ai-chat-claude-desktop-parity. `LocalCacheManager`'s AI-chat
# CRUD surface (`AppendChatMessage` / `LoadChatMessages` / ...) is gated on
# `SMATCHET_WITH_AI` in `LocalCacheManager.cpp`. The chat doctest TU
# (`LocalCacheManagerChat.test.cpp`) self-gates with the same guard so the
# rig keeps building when AI is disabled; mirror the parent option here so
# both surfaces toggle together. Matches the `SMATCHET_WITH_WHISPER`
# precedent above.
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
        Core/WhisperApiKeyResolve.test.cpp
        Core/WhisperApiPayload.test.cpp
        Core/WavWriter.test.cpp
        # Phase C — local backend, downloader catalog, consent gate, mode router.
        Core/ModelCatalog.test.cpp
        Core/WhisperConsentGate.test.cpp
        Core/WhisperModeRouter.test.cpp
        # Phase E — pure hotkey-descriptor parser.
        Core/HotkeyParse.test.cpp
        # Phase F — silence-trim helper + config-fields round-trip.
        Core/SilenceTrim.test.cpp
        Core/WhisperPrefsFields.test.cpp
        Core/WhisperMockSeams.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/WhisperApiKeyResolve.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/WhisperApiClient.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/WavWriter.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/ModelCatalog.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/WhisperConsentGate.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/HotkeyParse.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/SilenceTrim.cpp"
    )
    target_include_directories(SmatchetTests PRIVATE
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper")
endif()

target_include_directories(SmatchetTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source/Core/include"
    # Subsystem subdirs from source-core-dir-reorg — the test rig compiles
    # moved .cpp directly, so it needs the moved headers' subdirs on the path
    # (bare #includes, no rewrites). Mirrors the main-target fix in CMakeLists.txt.
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Tracker"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Sync"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Persistence"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Config"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Ui"
    # log-a-bug-github — new Core subsystems (bare #includes in their tests/TUs).
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Diagnostics"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Privacy"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Imaging"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui"
    "${CMAKE_SOURCE_DIR}/Source/Core/ThirdParty/ImGuiColorTextEdit"
    # IconCatalog.test.cpp links SmatchetIconCatalog.cpp, which includes the FA6
    # header + generated catalog (parity with the production SmatchetCoreInterface).
    "${CMAKE_SOURCE_DIR}/Source/Core/ThirdParty/IconsFontAwesome6"
    "${CMAKE_SOURCE_DIR}/Source/Plugins/Mcp"
    "${CMAKE_SOURCE_DIR}/Source/Standalone"
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

# Helper executables for SubprocessCapture.test.cpp. These are tiny
# stdlib-only binaries the doctest spawns to exercise exit codes, timeouts,
# cancel tokens, and stdout caps without depending on platform shell
# builtins. The resolved binary directory is threaded into SmatchetTests as
# a compile-time string so the test discovers the helpers without env vars.
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
