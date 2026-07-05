include(FetchContent)

# doctest — header-only C++ unit test framework. Latest stable v2.4.x.
# Pinned to the exact commit the v2.4.11 lightweight tag resolves to (reproducible
# builds / supply chain) — same content the floating tag points to today, zero
# behaviour change. Lightweight tag; resolved via:
# git ls-remote https://github.com/doctest/doctest.git refs/tags/v2.4.11
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG ae7a13539fb71f270b87eb2e874fbac80bc8dda2  # doctest v2.4.11
    GIT_SUBMODULES "")  # #1031: skip `git submodule update` (doctest has none) so a fresh
                        # worktree configure under bash+vcvars doesn't die in git-sh-setup.
FetchContent_MakeAvailable(doctest)

# ============================================================================
# Curated ThreadSanitizer subset (hardening #10 + multi-grid Slice-5 TSan).
#
# When the headless Linux Clang TSan preset (ninja-tsan-linux) is active, build
# ONLY SmatchetTsanTests over the portable, ImGui-FREE threading subset and skip
# the full SmatchetTests rig entirely (it links ImGuiLib → GLFW/X11/OpenGL, which
# we deliberately do not configure on the Linux runner — see SMATCHET_BUILD_APP in
# the root CMakeLists). Gated on UNIX so a Windows tsan misconfig can never reach
# here (MSVC no-ops TSan in cmake/Sanitizers.cmake anyway).
#
# Scope rationale: this subset is the genuinely-decoupled threading surface —
# each TU below pulls NO ImGui and NO cpr header. The GridLiveContext /
# TicketSyncService sync layer (ADR-0012 atomic backend-swap race, ADR-0020
# ISyncCache seam) is now IN scope. Two prior blockers were cleared: (1) the ImGui
# coupling (TicketSyncService → SmatchetToastManager::Push → SmatchetToast.h →
# <imgui.h>) was decoupled in #1318; (2) the residual cpr coupling — TicketSyncService
# classified transport errors via IsTrackerTransportErrorText declared in
# TrackerHttpUtils.h, which #includes <cpr/cpr.h> at the header level — was cleared by
# moving that pure function to TrackerHttpPure.h/.cpp (cpr-free) and swapping the
# include. A BFS include-taint scan of the full post-swap compile closure confirmed it
# reaches zero cpr/imgui/GLFW/glad/OpenGL headers. Tests construct via header-only
# fakes (FakeTicketSyncDeps / FakeTrackerClient / FakeSyncCache) — no SQLite, no HTTP
# at runtime. See docs/plans/active/build-quality-velocity-hardening.md § Verification
# (#10) and the debt-ledger TSan-subset-expand entry.
#
# smatchet_apply_sanitizers() (cmake/Sanitizers.cmake, included at root) appends
# -fsanitize=thread -fno-omit-frame-pointer (compile) + -fsanitize=thread (link)
# PRIVATE to this target under a non-MSVC compiler.
# ============================================================================
# ============================================================================
# libFuzzer harness (Slice E2 — testing-surface.md §6 P1 Gap 3).
#
# When the headless Linux Clang libFuzzer preset (ninja-fuzzer-linux) is active,
# build ONLY the fuzz drivers under tests/fuzz/ and skip the full SmatchetTests
# rig (it links ImGuiLib → GLFW/X11/OpenGL, deliberately not configured on the
# Linux runner — see SMATCHET_BUILD_APP in the root CMakeLists). Mirrors the tsan
# guard below. Gated on UNIX so a Windows fuzzer misconfig can never reach here
# (cmake/Sanitizers.cmake FATALs on MSVC/clang-cl anyway). The doctest rig is not
# linked; libFuzzer supplies main() via -fsanitize=fuzzer.
# ============================================================================
if(UNIX AND DEFINED SMATCHET_SANITIZER AND SMATCHET_SANITIZER STREQUAL "fuzzer")
    add_subdirectory(fuzz)
    return()
endif()

if(UNIX AND DEFINED SMATCHET_SANITIZER AND SMATCHET_SANITIZER STREQUAL "tsan")
    add_executable(SmatchetTsanTests
        test_main.cpp
        # --- Pure-logic units (no threads) — prove the subset compiles + links
        #     under -fsanitize=thread end-to-end on the runner (pipeline de-risk).
        Core/TrackerLabelsPure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerLabelsPure.cpp"
        Core/TrackerDateTimePure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerDateTimePure.cpp"
        # Gap map Tier 1 #3 — grid-cell render-model builders (untrusted tracker-value parsing).
        # No threads; proves the pure TU compiles/links under TSan and runs its doctests on the
        # Linux subset (TrackerFieldValueParser/Utils closure already linked below).
        Core/TrackerGridFieldDisplayPure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerGridFieldDisplayPure.cpp"
        # Gap map Tier 1 #5 — the JQL / Plane query-suggest engines behind their catalog-vector
        # seams (keystroke-frequency untrusted-input parsers), + their shared-helper closure
        # (TrackerQuerySuggestCommon + JqlEscape — cpr/ImGui-free). No threads.
        Core/JqlSuggestEnginePure.test.cpp
        Core/PlaneQuerySuggestEnginePure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JqlSuggestEnginePure.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneQuerySuggestEnginePure.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JqlEscape.cpp"
        # Gap map Tier 1 #6 — the merge-watch notify endpoint's request-validation core
        # (SECURITY_AUDIT-flagged localhost listener). Pure decision surface; no httplib.
        Core/MergeWatchNotifyPure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/MergeWatchNotifyPure.cpp"
        # linear-tracker-backend.md Slice 1 — pure GraphQL/parse helpers.
        Core/LinearClientHelpers.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearClientHelpers.cpp"
        Core/LinearIssueMappingPure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearIssueMappingPure.cpp"
        Core/LinearQueryFromJql.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearQueryFromJql.cpp"
        # multi-grid-tabs.md Slice 5a — pure hidden-pane LRU eviction decision
        # (header-only; no extra .cpp). No threads — proves the policy compiles +
        # links cleanly under -fsanitize=thread end-to-end on the runner.
        Core/GridPaneEvictionPolicy.test.cpp
        # issue #1457 — gridContexts_ map-structure race (User Info worker snapshot-find
        # vs UI-thread retire-erase / emplace). Seam-level fixture mirroring the map-mutex
        # discipline; header-only (no product .cpp), highest-value being the TSan pass.
        Core/GridContextsMapRace1457.test.cpp
        # MainThreadDispatcher: the keystone of the UI-thread-marshalling discipline. Every
        # cross-thread g_ui write (AiAssistant streaming MakeOnDelta, merge-watch notify, sync
        # toasts) is race-free ONLY because workers PostToMainThread and the UI thread runs the
        # lambda inside Drain(). This case hammers PostToMainThread from N producers + QueueLen()
        # from a reader against a single Drain()-ing thread, with a NON-atomic canary that is
        # race-free iff Drain serialises tasks onto one thread. Header-only product surface
        # (MainThreadDispatcher.h); the SMATCHET_UI_PERF_SCOPE in Drain() pulls UiPerfMonitor.cpp
        # (ImGui-free), linked just below.
        Core/MainThreadDispatcherConcurrent.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/UiPerfMonitor.cpp"
        # --- LocalCacheManager: SQLite-backed ticket cache carrying std::mutex /
        #     std::atomic state — real ThreadSanitizer surface. ImGui-free closure:
        #     LocalCacheManager.cpp + IssueDraft.cpp + Logger.cpp (header deps only).
        Core/LocalCacheManager.test.cpp
        # multi-grid-tabs.md Slice 5 TSan pass — concurrent GetAllTickets (seed-reader
        # background task) + SaveTicket (UI-thread write) from two std::threads sharing
        # one LocalCacheManager instance. LCM.cpp already linked above; no extra .cpp.
        Core/LocalCacheConcurrentSeed.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/LocalCacheManager.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueDraft.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Logger.cpp"
        # Logger.cpp now routes the file sink through RedactLogLine (audit
        # 2026-06-13 LOW) — link the pure redactor so the closure resolves.
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Privacy/TextRedaction.cpp"
        # --- Sync layer: the ADR-0012 atomic backend-swap race + ADR-0020 ISyncCache
        #     seam. GridLiveContext owns the live TicketSyncService (atomic Backend slot,
        #     latched strong handles surviving live swaps); TicketSyncService drives the
        #     sync state machine. Both carry real cross-thread state — the highest-value
        #     TSan surface. cpr-free closure: TicketSyncService now classifies transport
        #     errors via TrackerHttpPure.h (IsTrackerTransportErrorText moved there), so
        #     no TU here pulls <cpr/cpr.h>. Tests construct via header-only fakes
        #     (FakeTicketSyncDeps / FakeTrackerClient / FakeSyncCache) — no SQLite, no HTTP.
        Core/GridLiveContext.test.cpp
        Core/TicketSyncService.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/GridLiveContext.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/TicketSyncService.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerHttpPure.cpp"
        # --- EditMetaCacheService (AppController god-object decomposition Phase 1): per-issue +
        #     per-issue-type editmeta cache (editMetaMutex_) loaded from BOTH the UI thread and
        #     the background warm worker — a genuine cross-thread mutex, highest-value TSan
        #     surface. EditMetaCacheConcurrent.test runs the warm worker on a real std::thread
        #     against a concurrent CanEditFieldForIssue reader (sibling of TicketSyncService
        #     above). Service .cpp pulls GridLiveContext / ConfigManager / TrackerFieldValueUtils
        #     / TrackerFieldValueParser (all already linked above) + ProjectResolver
        #     (ExtractIssueKeyPrefix) — add ProjectResolver here for the TSan-target closure
        #     (it is in the SmatchetTests block but not this one). cpr/ImGui/SQLite-free; tests
        #     construct via header-only fakes (FakeEditMetaDeps / FakeTrackerClient).
        Core/EditMetaCacheService.test.cpp
        Core/EditMetaCacheConcurrent.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/EditMetaCacheService.cpp"
        # --- FieldEditPipelineService (AppController god-object decomposition Phase 2): the
        #     field-edit network pipeline. Holds NO long-lived mutex (call-frame-scoped ctx), so
        #     it is TSan-neutral, but its .cpp calls EditMetaCacheService (above) on the same
        #     thread + the audit pair (linked below for OfflineQueueService) — include it here so
        #     the service-test TU links in the TSan rig too. cpr/ImGui/SQLite-free (header-only
        #     FakeFieldEditDeps / FakeEditMetaDeps / FakeTrackerClient).
        Core/FieldEditPipelineService.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/FieldEditPipelineService.cpp"
        # --- ConnectivityMonitorService (AppController god-object decomposition Phase 3): the
        #     tracker connectivity-probe FSM. UI-thread-only by contract (no mutex) but it owns the
        #     ONE cross-thread handle in the service — the std::async reachability probe future.
        #     Include it here so the probe-future handoff (launch on one thread, get() on the FSM
        #     thread) is TSan-instrumented. .cpp pulls ConfigManager / TrackerHttpUtils (already
        #     linked above) + ITrackerConnectivity (header-only). cpr/ImGui/SQLite-free (header-only
        #     FakeConnectivityDeps / FakeTrackerClient).
        Core/ConnectivityMonitorService.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/ConnectivityMonitorService.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/ProjectResolver.cpp"
        # ConfigManager closure (split across 4 TUs). TicketSyncService calls
        # ConfigManager::Load / LoadViewsOrBootstrap / NormalizeViewsBackendKey, which
        # reach across the split — link the TUs that own those symbols + ToolbarConfig
        # (ConfigManager.cpp references ToolbarConfig::). All headless-Linux-clean
        # (wincrypt is _WIN32-guarded; whisper/lua feature guards have OFF branches).
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_PathUtils.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Views.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ToolbarConfig.cpp"
        # ConfigManager's deeper closure: Load() round-trips the keybindings block
        # (KeybindingsConfig::Defaults/SetBindingHotkey/FindBindingIndex + its
        # to_json/from_json), and ConfigManager_Views/_PathUtils guard disk writes
        # with UiThreadAffinity::WarnIfOnUiThread. Both are cpr/ImGui/GL-free.
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/KeybindingsConfig.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/UiThreadAffinity.cpp"
        # TicketSyncService.test scripts its backend through the header-only
        # ScriptedTrackerBackendFactory, whose impl lives in JiraFakeTrackerFixture
        # (builds a header-only FakeTrackerClient — no real JiraClient, no cpr). The
        # fixture maps fixture JSON via JiraIssueMappingPure (pure mapper, cpr-free).
        support/JiraFakeTrackerFixture.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraIssueMappingPure.cpp"
        # JiraIssueMappingPure fans into the field-value parser: it calls
        # JsonGetStringIfString / ParseComments / ParseChangelog / NormalizeTrackerFieldValue
        # / FormatWorkDurationFromSeconds / FormatTrackerTimetrackingDisplay — all defined in
        # TrackerFieldValueParser.cpp, which in turn calls TrackerFieldValueUtils helpers
        # (option-resolve + duration/comment template load via ConfigManager, already linked).
        # Both TUs are cpr/ImGui/GL-free (includes: parser.h + Utils.h + Logger + StringUtil +
        # ConfigManager + std only).
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldValueParser.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldValueUtils.cpp"
        # --- Offline-queue replay + config-save concurrency (debt P2 TSan-subset-expand,
        #     #1339 follow-up). OfflineQueueService drains PendingCreate / PendingFieldEdit
        #     on the sync/reconnect thread while a live backend swap can land mid-replay
        #     (ADR-0012 atomic Backend slot — same race family as GridLiveContext above);
        #     ConfigSaveWorker persists config off the UI thread (std::thread + mutex/cv).
        #     Both are real ThreadSanitizer surfaces. All TUs cpr/ImGui/GL-free; the only new
        #     ThirdParty link is md4c (pure stateless C Markdown parser — no threads).
        Core/OfflineQueueServiceRuntime.test.cpp
        Core/OfflineQueueBackendSwap.test.cpp
        Core/ConfigSaveConcurrency.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/OfflineQueueService.cpp"
        # OfflineQueueService link closure: the audit pair (BackendAuditTrail +
        # FieldEditAuditSource), 3-way rich-text merge (TextMerge), markdown<->ADF/HTML for
        # field-edit replay (MarkdownConvert -> md4c), and create-replay through the live
        # pipeline (IssueCreatePipeline + Helpers + TrackerFieldPayload[Pure]). IssueDraft.cpp
        # already linked above; the offline-queue conflict/replay policies are header-only.
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/BackendAuditTrail.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/FieldEditAuditSource.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/TextMerge.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/MarkdownConvert.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueCreatePipeline.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueCreatePipelineHelpers.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldPayload.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp"
        # TrackerFieldPayloadPure date<->API formatting: the pure half of CompactDateFormat
        # (TryParseJiraDateTime / FormatJiraDateOrDateTimeForApi), split out of the ImGui-bound
        # CompactDateFormat.cpp so it links here without dragging <imgui.h>.
        "${CMAKE_SOURCE_DIR}/Source/Core/src/CompactDateFormatPure.cpp"
        # ConfigSaveConcurrency closure: ConfigSaveWorker (ConfigManager.cpp already linked).
        "${CMAKE_SOURCE_DIR}/Source/Core/src/ConfigSaveWorker.cpp"
        # --- AiAssistant streaming hand-off (slice 2c — tsan-imgui-linked-target.md). After slices
        #     2a/2b, AiAssistantController is g_ui-/ImGui-free (depends on MainThreadDispatcher +
        #     IAiAssistantUiState only), so its worker→PostToMainThread→Drain hand-off + internal
        #     cancel/state machine link here — the playbook's "AI streaming cancel/submit race".
        #     cpr edge broken by tests/support/FakeAiClientFactory.cpp (the real AiClientFactory →
        #     Anthropic/OpenAi/Ollama clients → cpr is NOT linked); the test injects StubAiClient via
        #     SetTestOverride + a FakeAiAssistantUiState. The closure (AiContextBuilder/AgentsMdLoader/
        #     AiModelSignature/AiEndpointPolicy/AiEndpointSanitize/AiChatTimestamp/SmatchetChatPersistWorker)
        #     is cpr/ImGui-free; BackendAuditTrail + ConfigManager already linked above. Needs
        #     SMATCHET_WITH_AI (target-wide below — ConfigManager carries one AI-gated field).
        Core/AiAssistantStreamHandoff.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiAssistantController.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiContextBuilder.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AgentsMdLoader.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiModelSignature.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiEndpointPolicy.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiEndpointSanitize.cpp"
        # Gap map Tier 1 #8 — pre-network probe plan (no threads; Linux-runnable so the
        # container sessions doing this work can validate locally).
        Core/AiPrefsTestConnectionPure.test.cpp
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiPrefsTestConnectionPure.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/AiChatTimestamp.cpp"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/SmatchetChatPersistWorker.cpp"
        "${CMAKE_SOURCE_DIR}/tests/support/FakeAiClientFactory.cpp"
    )
    target_include_directories(SmatchetTsanTests PRIVATE
        _helpers
        "${CMAKE_SOURCE_DIR}/Source/Core/include"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Tracker"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Sync"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Persistence"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Config"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Ui"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Privacy"
        "${CMAKE_SOURCE_DIR}/Source/Core/include/Types"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Config"
        "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync"
        "${CMAKE_SOURCE_DIR}/tests/support"
    )
    target_compile_definitions(SmatchetTsanTests PRIVATE
        "SMATCHET_TESTS_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\""
        # slice 2c: AiAssistantController + its closure are #if defined(SMATCHET_WITH_AI). Target-wide
        # (NOT per-source): ConfigManager carries one AI-gated field, so a per-source flag would
        # ODR-mismatch its struct layout against the other TUs that include ConfigManager.h.
        SMATCHET_WITH_AI=1)
    target_link_libraries(SmatchetTsanTests PRIVATE
        doctest::doctest
        nlohmann_json::nlohmann_json
        SQLiteCpp
        ghc_filesystem
        # md4c — MarkdownConvert (offline field-edit replay md<->ADF/HTML). Pure stateless
        # C parser; its PUBLIC include dir propagates md4c.h. No threads (TSan-neutral).
        md4c
    )
    target_compile_features(SmatchetTsanTests PRIVATE cxx_std_14)
    smatchet_apply_sanitizers(SmatchetTsanTests)
    add_test(NAME smatchet_tsan_tests COMMAND SmatchetTsanTests)
    # Skip the full SmatchetTests rig below (ImGuiLib-linked).
    return()
endif()

# Per-unit linkage: each test target lists the specific Core/*.cpp files
# it exercises. No production target links test code. Refactor to a SmatchetCore
# static lib only if per-test link lists grow unwieldy.
add_executable(SmatchetTests
    test_main.cpp
    # build-quality-velocity-hardening #21 — project-local Optional<T>/Result<T,E>
    # value types (C++14 bans std::optional/std::variant). Header-only
    # (Source/Core/include/SmatchetResult.h); no production .cpp to link.
    Core/SmatchetResult.test.cpp
    # close-gate-gaps Slice 1a — low-layer UI-thread registry behind the Pillar-2
    # blocking-wrapper gate. Depends only on Logger (already linked below).
    Core/UiThreadAffinity.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/UiThreadAffinity.cpp"
    Core/AnnotateAnalysisConfig.test.cpp
    Core/AnnotateLocalization.test.cpp
    # savediscard-locales — pin the 5 Assistant Save/Discard prefs keys so a key
    # typo (key resolves to English fallback under fr-FR) can't regress.
    # SmatchetLocalization.cpp already linked below.
    Core/PrefsAssistantLocalization.test.cpp
    Core/ConfigSaveConcurrency.test.cpp
    Core/MainThreadDispatcherDrain.test.cpp
    Core/CacheEvictionPolicy.test.cpp
    # SECURITY_AUDIT_2026-06-13 synthesis #9 — pure pre-decode dimension/pixel-budget gate behind
    # SmatchetImageTextureCache's stbi_info pre-check. Header-only
    # (Source/Core/include/Persistence/IconDimensionsPolicy.h); no production .cpp to link.
    Core/IconDimensionsPolicy.test.cpp
    # mobile P1.4 — pure area-average RGBA32 downscale behind the non-Windows attachment-thumbnail
    # decode arm (stb_image cannot decode-scale; WIC does on Windows). Header-only
    # (Source/Core/include/Ui/RgbaDownscalePure.h); no production .cpp to link.
    Core/RgbaDownscalePure.test.cpp
    # multi-grid-tabs.md Slice 5a (plan item 21) — pure hidden-pane LRU eviction decision
    # (least-recently-visible first; visible + busy-sync never evicted). Header-only policy.
    Core/GridPaneEvictionPolicy.test.cpp
    # issue #1457 — gridContexts_ map-structure race (worker snapshot-find vs UI
    # retire-erase / emplace). Seam-level fixture; header-only, also in SmatchetTsanTests.
    Core/GridContextsMapRace1457.test.cpp
    Core/MemoryTelemetry.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/MemoryTelemetry.cpp"
    # perf-p99-emitter — pure ring-buffer + percentile seam behind UiPerfMonitor's
    # snapshot-time p99Ms row field (Pillar-1 p99 ceiling). Header-only
    # (Source/Core/include/Ui/PerfSampleRing.h); no production .cpp to link.
    Core/PerfSampleRing.test.cpp
    # Warmup-frame exclusion (tooling.md p99-gate-warmup-frame-exclusion, PR #963):
    # the Record/Reset/GetLastFrameRows(true) seam the runner's post-warmup
    # UiPerfMonitor::Reset() drives — cold-start spikes dropped, a real
    # steady-state regression still trips the ceiling. Links UiPerfMonitor.cpp
    # (already in the SmatchetTests source list below).
    Core/UiPerfMonitorWarmup.test.cpp
    # Worker-thread exception firewall (issue #1081 / PR #1080) — pure containment helper
    # behind AppController::LaunchBackgroundTask (Pillar-3 never-crash). Header-only
    # (Source/Core/include/BackgroundTaskFirewall.h); no production .cpp to link.
    Core/BackgroundTaskFirewall.test.cpp
    Core/BackgroundWorkerReap.test.cpp
    # WS-A cooperative future cancellation (#734/#1150) — pure coverage of the
    # CancelToken shared atomic flag + CancellableFutureSet owner helper
    # (signal-all-cancel, non-blocking abandon, teardown join). Header-only
    # (Source/Core/include/Ui/CancelToken.h); no production .cpp to link.
    Core/CancelToken.test.cpp
    # WS-A bucket-E (#734): bulk-import futures-clear must not block the UI frame for
    # the still-running create workers' duration. Header-only (CancelToken.h) re-creation
    # of the BulkImportAbandonFutures(d) signal-then-abandon path against slow std::futures.
    Core/BulkImportAbandonNonBlocking.test.cpp
    # WS-A ASan #1150 repro (Pillar-3): the SmatchetUserInfoUi activity-worker UAF shape —
    # a worker holding a raw controller pointer + cancel token, torn down mid-scan. Asserts
    # UAF-clean + fast (signal-then-join) teardown. Header-only (CancelToken.h).
    Core/UserInfoActivityCancelUaf.test.cpp
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
    # security H3 + E1 — shared JQL string-literal escape (tracker_jql::QuoteLiteral) used by
    # BuildJqlUserInsert / BuildKeyInJql / BuildActivityDiscoveryJql. Pure string helper,
    # no cpr/sqlite — safe in the doctest rig.
    Core/JqlEscape.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JqlEscape.cpp"
    # dedup-tracker-query-and-ai-client-clones cluster A — backend-agnostic query-suggest
    # helpers extracted from JqlSuggestEngine.cpp + PlaneQuerySuggestEngine.cpp. The reparam
    # to const std::vector<TrackerField>& (away from AppController) makes the field-catalog
    # helpers bucket-A-testable; the TU pulls only StringUtil (header-inline) + TrackerFieldSchema.
    Core/TrackerQuerySuggestCommon.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp"
    # Fix C of jira-components-multiselect (PR #672) — scoped catalog save→load round-trip
    # asserting `components.AllowedValueOptions` survive under the project-scoped cache key
    # (and that the unscoped key never sees them). FieldCatalogCache.cpp depends only on
    # ConfigManager (already linked) + nlohmann — no cpr/sqlite, safe in the doctest rig.
    Core/FieldCatalogCacheScopedRoundTrip.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/FieldCatalogCache.cpp"
    Core/OfflineQueueReplayPolicy.test.cpp
    # Pure filter/key/label helpers extracted from SmatchetProjectPicker::Draw. The _detail TU
    # depends only on StringUtil (header-inline) + plain struct headers — no ImGui, no AppController.
    Core/ProjectPickerFilter.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetProjectPicker_detail.cpp"
    # Pure chip-fade + right-cluster width helpers extracted from DrawGridHeaderToolbar. The
    # _detail TU depends on nothing but <algorithm> — no ImGui, no AppController.
    Core/GridHeaderToolbarLayout.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetGridHeaderUi_detail.cpp"
    # Pure cached-project filter extracted from DrawJqlQueryEditorEmbedded's project pill. The
    # _detail TU reads only CachedProjectEntry struct fields — no ImGui, no FieldCatalogCache call.
    Core/JqlProjectPillFilter.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetJqlProjectPill_detail.cpp"
    # Pure log-line formatting / clipboard-join helpers extracted from SmatchetUI::drawLogWindow.
    # The _detail TU depends only on Logger.h types (header-only enum + struct) — no ImGui.
    Core/LogViewLines.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetLog_detail.cpp"
    # Pure option-filter + catalog-dropdown predicates extracted from RenderNewIssueDraftRow. The
    # _detail TU depends only on StringUtil (header-inline) + TrackerFieldSchema — no ImGui.
    Core/NewIssueDraftFieldDecision.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetNewIssueDraftUi_detail.cpp"
    # Pure date-format index<->option helpers lifted out of
    # DrawLocalAndAppearancePreferencesTabs during its function-size decomposition.
    # SmatchetPreferencesUi_detail.h defines them `inline` and only forward-declares
    # UI classes — no ImGui, no AppController — so the rig needs no extra .cpp link.
    Core/PreferencesDateFormatIndex.test.cpp
    # dual-ui-mode batch — pure inline string/scale converters for the UI
    # layout-mode + mobile-shell enums (SmatchetUiModeIds.h). Header-only +
    # ImGui-free, so no extra .cpp link.
    Core/UiModeIds.test.cpp
    # Issue #979 — pure credential-trim helper applied by onPreferencesSaveAndSync
    # (trailing-space Jira email → persistent 401). Inline in the same _detail
    # header; no extra .cpp link.
    Core/PreferencesCredentialTrim.test.cpp
    # feat/dec-savediscard — pure working-copy helpers (CopyAssistantAiFields /
    # AssistantAiFieldsDiffer) behind the Assistant tab's explicit Save/Discard +
    # dirty-tab flow. Inline in the same _detail header (guarded by
    # SMATCHET_WITH_AI, which this target defines); no extra .cpp link.
    Core/PreferencesAssistantWorkingCopy.test.cpp
    # Function-size decomposition (feat/funcsize-annotate-analysis) — pure
    # display-classification helpers extracted from AnnotateAnalysisUi::DrawContent
    # (the largest function in the tree) to cut its branch count. The _detail TU is
    # ImGui-free + AppController-free, so the test rig links just that one cpp.
    Core/AnnotateRowDisplayPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/AnnotateAnalysisUi_Window_detail.cpp"
    # Pure audit-window filter/pagination helpers extracted from SmatchetUI::drawAuditWindow.
    # The _detail TU depends only on nlohmann/json — no ImGui, no AppController.
    Core/AuditWindowFilter.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetAudit_detail.cpp"
    # Pure field-categorization / column-label helpers lifted out of
    # SmatchetUI::drawViewsDashboardWindow during its function-size decomposition.
    # SmatchetViewsDashboardUi_detail.h defines them all `inline` and pulls only
    # plain struct headers (Views.h + TrackerFieldSchema.h) — no ImGui, no
    # AppController — so the rig needs no extra .cpp link.
    Core/ViewsFieldCategorization.test.cpp
    Core/SmatchetThemeSyntaxColors.test.cpp
    # AI chat Claude-Desktop-parity Phase 5 — AI chat-panel palette per theme.
    # Mirrors SmatchetThemeSyntaxColors.test.cpp; same SmatchetTheme.cpp link.
    Core/SmatchetThemeAiColors.test.cpp
    # Regression guard for the inverted Norton Commander palette (blue-dominant, NC 2.01 menu).
    Core/SmatchetThemeNortonPalette.test.cpp
    # Mobile P1.6 — SmatchetDark accent WCAG-AA darken (0.35,0.55,0.95)->(0.26,0.42,0.72).
    # Pins both constraints: white-on-fill >= 4.5 AND accent-on-WindowBg >= 3.0. Same
    # SmatchetTheme.cpp link as the sibling theme tests; computes contrast in-test.
    Core/SmatchetThemeAccentContrast.test.cpp
    # Mobile host-injection seam #13 (PR #1043) — pure ShouldApplyDensityScale predicate
    # (SmatchetThemeDensity.h, header-only inline) behind ReapplyHostDensityScale. No extra
    # .cpp link: header-only, and SmatchetTheme.cpp + include/Ui are already in the rig.
    Core/SmatchetThemeDensity.test.cpp
    # Mobile #1122 dynamic-texture orphan guard — pure SmatchetDrawCmdTextureNeedsRebind
    # predicate (SmatchetImGuiTextureGuard.h, header-only inline, ImGui-free). No extra .cpp
    # link: the predicate is value-only; android_main.cpp pins the status constants to the
    # real ImTextureStatus_* with static_assert.
    Core/SmatchetImGuiTextureGuard.test.cpp
    # Reset-layout docking regression (PR #1198) — pure default dock-slot table +
    # key-iteration accessors (SmatchetDockNodeIds.cpp) that drive both initial docking
    # and the live-reset force-redock. ImGui-free at runtime (constexpr IDs + strcmp); the
    # rig already links ImGuiLib + include/Ui so imgui.h resolves.
    Core/SmatchetDockNodeIds.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetDockNodeIds.cpp"
    # jql-omnibox-search Stream A — pure icon/text fallback-selection predicate
    # behind SmatchetIconButton (the icon-only Refresh + icon-leading Save&Sync
    # helpers). SmatchetIconButtonLabel.h is header-only + ImGui-free; the ImGui
    # wrapper SmatchetIconButtons.cpp depends on SmatchetAreFaIconsLoaded()
    # (SmatchetImGuiFonts.cpp, not linked here) so it is deliberately NOT in the
    # rig — only the pure predicate is. No extra .cpp link.
    Core/SmatchetIconButtons.test.cpp
    Core/MarkdownPreviewLangTag.test.cpp
    Core/MarkdownConvertAdf.test.cpp
    Core/MarkdownConvert.test.cpp
    Core/IssueDraft.test.cpp
    Core/IssueCreatePipeline.test.cpp
    Core/CallstackParser.test.cpp
    # Function-size decomposition (feat/funcsize-cpp-syntax-highlight) — pure
    # ImGui-free lexers extracted from CppSyntaxHighlight.cpp's draw helpers to
    # bring DrawColoredCppLine / DrawColoredCallstackLine under the 30-branch
    # cap. CppSyntaxLex.cpp has no ImGui dependency (header is pure), so the
    # test rig links just that one TU.
    Core/CppSyntaxLex.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/CppSyntaxLex.cpp"
    Core/LocalCacheManager.test.cpp
    # multi-grid-tabs.md Slice 5 TSan pass — concurrent seed-read (GetAllTickets) +
    # write (SaveTicket) from two std::threads; also in SmatchetTsanTests for TSan.
    Core/LocalCacheConcurrentSeed.test.cpp
    # Multi-grid Slice 1b (ADR-0018) — tickets_v2 backend-key namespacing: one-time copy
    # migration round-trip on a pre-migration fixture DB, idempotence via cache_meta flag,
    # and two-backend disjointness on a colliding numeric id. Links the production LCM TU
    # already pulled in below.
    Core/LocalCacheTicketsV2Migration.test.cpp
    # Slice G Phase 1 (testing-surface.md §6 Gap 6, plan slice-g-db-corruption.md) —
    # characterizes today's corrupt/truncated cache-file-on-open behaviour: the ctor's
    # schema-init runs OUTSIDE the WAL-pragma try/catch, so a non-SQLite/truncated file throws
    # past the ctor (Pillar-3 startup-crash gap), while a zero-byte file opens fresh. Pins the
    # throw so the planned Phase-2 graceful-rebuild lands as a deliberate test diff. File-backed
    # (the throw is on the on-disk header read), so it uses the shared support/TempDbFile.h RAII
    # helper. Links the production LCM TU already pulled in below.
    Core/LocalCacheManagerCorruption.test.cpp
    # Multi-grid Slice 1c (ADR-0018) — pending-queue backend_key: ADD-COLUMN migration
    # idempotence on a pre-1c fixture DB, one-time stamp migration, backend-scoped replay
    # matching (A-rows replay, B-rows stay queued), dead-letter key carry + restore. Links
    # the production LCM + OfflineQueueService TUs already pulled in below.
    Core/OfflineQueueBackendKey.test.cpp
    # Multi-grid Slice 3 pre-staged debt (debt.md 2026-06-07) — replay workers latch STRONG
    # role handles: swap-during-replay completes against the latched backend (create tick)
    # and swap-to-null / context-retirement shape does not crash (field-edit tick).
    Core/OfflineQueueBackendSwap.test.cpp
    # Multi-grid Slice 4 (plan item 20) — TWO live backends (Jira + GitHub panes) over ONE
    # shared queue DB, queuing interleaved while offline then replaying: each backend replays
    # ONLY its own backend_key rows (strict-equality filter under logical concurrency), dead
    # rows under both keys coexist + restore under their own key, and a swap-mid-replay never
    # leaks across keys. Real-thread/TSan concurrency is gated to Slice 5. Links the production
    # LCM + OfflineQueueService TUs already pulled in below.
    Core/OfflineQueueTwoBackendReplay.test.cpp
    # Issue #1081 — backend switch racing in-flight sync workers: publish-under-lock contract
    # (SwapBackendIfTrackerChanged), capture-then-check backend-generation token (offline
    # replay RefreshLocalData drop + create-replay key latch), and the pure
    # PaneSyncKickPolicy.h decision helpers (sync-storm damping + stale-kick drop). Links the
    # TicketSyncService + OfflineQueueService TUs already pulled in below.
    Core/BackendSwitchRace1081.test.cpp
    # ISyncCache contract (ADR-0020): dual-impl spec for the cache seam — identical assertions
    # against the real LocalCacheManager(":memory:") AND FakeSyncCache (the fake-fidelity gate).
    # SQLite-backed by design (real half) — named-exempt from the purity guard below.
    Core/SyncCacheContract.test.cpp
    # The ONE retained service-vs-real-SQLite case (ADR-0020 grill Q3 option 3): per-service-path
    # backstop for fake divergence. SQLite-by-design — named-exempt from the purity guard below.
    Core/OfflineQueueServiceRealCacheSmoke.test.cpp
    # AI chat Claude-Desktop-parity Phase 3 — SQLite-backed chat-history CRUD
    # against the same LCM SQLite path. Production target globs LCM.cpp, so the
    # test rig only needs the test TU itself (no separate cpp link).
    Core/LocalCacheManagerChat.test.cpp
    Core/BackendAuditTrail.test.cpp
    # security #10 regression guard — the offline-queue create audit copy of the draft must be
    # structurally redacted (RedactJson over the parsed payload) while the enqueued/replayed
    # payload retains the real field values. Links IssueDraft.cpp + BackendAuditTrail.cpp (both
    # already in the src list below); no extra .cpp link needed.
    Core/OfflineQueueDraftAuditRedaction.test.cpp
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
    # WS2 / Issue #1068 — runtime TLS-trust seam (ResolveSslConfig + process-global CA-path
    # holder). Pure + cpr-free; links Source/Core/src/Tracker/TrackerHttpPure.cpp below.
    Core/TrackerHttpSslPure.test.cpp
    Core/TrackerDateTimePure.test.cpp
    Core/TicketFieldEditorLongTextPure.test.cpp
    Core/TicketFieldEditorOptionFilterPure.test.cpp
    Core/TicketFieldEditorCommitPolicyPure.test.cpp
    # Security finding #13 — automation count-hook abort policy (shutdown + runaway budget).
    # Header-only pure predicate; no extra production .cpp to link.
    Core/LuaAutomationHookPolicyPure.test.cpp
    # SSRF guard — Lua imgui.image http(s)-URL refusal allowlist. Header-only pure predicate.
    Core/LuaImagePathPolicyPure.test.cpp
    Core/TicketFieldEditorDurationPopupPure.test.cpp
    # Time-tracking sort comparator lifted from TicketGridModel.cpp (TEST_COVERAGE_GAP_MAP.md
    # Tier 1 #1) — dependency-light, so tests link it without the grid model's heavy deps.
    Core/TicketGridDurationSortPure.test.cpp
    # Grid-cell render-model builders lifted from TrackerGridFieldDisplay.cpp (gap map Tier 1
    # #3) — the untrusted tracker-value parse/format layer, ImGui-free after the extraction.
    Core/TrackerGridFieldDisplayPure.test.cpp
    # Gap map Tier 1 #5 — the JQL / Plane query-suggest engines behind their catalog-vector
    # seams (the whole per-keystroke tokenizer/mode/suggestion pipeline; shells forward from
    # AppController). TrackerQuerySuggestCommon.cpp + JqlEscape.cpp already linked below.
    Core/JqlSuggestEnginePure.test.cpp
    Core/PlaneQuerySuggestEnginePure.test.cpp
    # Gap map Tier 1 #6 — merge-watch notify endpoint request-validation core.
    Core/MergeWatchNotifyPure.test.cpp
    Core/IsDescriptionLikeFieldId.test.cpp
    Core/TrackerFieldPayloadPure.test.cpp
    Core/TrackerFieldCatalogPure.test.cpp
    Core/TicketSyncService.test.cpp
    # EditMetaCacheService (AppController god-object decomposition Phase 1) — per-issue +
    # per-issue-type editmeta cache: optimistic-unloaded, status/priority/components carve-outs,
    # sprint/timetracking, loaded-deny, issuetype fallback, Ensure success/failure, Refresh,
    # Prune, and the #975 kick-time component-options warm. Drives a header-only FakeEditMetaDeps
    # (shared_ptr<FakeTrackerClient> + GridContextFieldCatalog) — no AppController/ImGui/cpr/
    # SQLite. Production TU added to the prod-src block below; ProjectResolver.cpp (its
    # ExtractIssueKeyPrefix dep) is already linked in this target.
    Core/EditMetaCacheService.test.cpp
    # FieldEditPipelineService (AppController god-object decomposition Phase 2) — the field-edit
    # network pipeline: regular/sprint/timetracking success, read-only + editmeta-deny blocks,
    # 400-retry editmeta refresh, offline-prepare contract, ApplyFieldEditResult. Drives a
    # header-only FakeFieldEditDeps + FakeEditMetaDeps + real EditMetaCacheService — no
    # AppController/ImGui/cpr/SQLite. Production TU added to the prod-src block below; the audit
    # pair (BackendAuditTrail + FieldEditAuditSource) it links is already in this target.
    Core/FieldEditPipelineService.test.cpp
    # ConnectivityMonitorService (AppController god-object decomposition Phase 3) — the tracker
    # connectivity-probe FSM: the banner-formatter matrix (error-priority / session-note / both-
    # warnings / snapshot-catalog / suffix-dedup / truncation — first-ever banner coverage), the
    # probe -> state mapping, a probe-down -> recovery sequence, two-writer banner reconciliation,
    # and the deferred-success / refetch one-shot latches. Drives a header-only FakeConnectivityDeps
    # + FakeTrackerClient — no AppController/ImGui/cpr/SQLite. Production TU added to the prod-src
    # block below; IsTrackerTransportErrorText (TrackerHttpPure.cpp) already linked in this target.
    Core/ConnectivityMonitorService.test.cpp
    # AttachmentAppUpdateService (AppController god-object decomposition Phase 4) — the attachment-open
    # dispatch + app-version helpers. Covers the deterministic pre-download decision logic: collection-
    # handler dispatch, the no-handler URL fallback, the preview-handler early-rejection branches, and
    # the compile-time version-helper contract. Drives a header-only FakeAttachmentAppUpdateDeps (a
    # plain HostCallbacks struct + recorded OpenUrl/RequestAppQuit) — no AppController/ImGui/SQLite. The
    # production TU pulls <cpr/cpr.h> for the network paths, so it is registered ONLY in this
    # cpr-linked SmatchetTests rig (not the cpr-free TSan / pure-sync rigs); the service has no mutex,
    # so there is no TSan race to cover. Production TU added to the prod-src block below.
    Core/AttachmentAppUpdateService.test.cpp
    # Issue #979 — ITrackerBackendFactory::Create(trackerType, cfg) must receive the
    # caller's LIVE cfg (never a disk re-read racing the debounced prefs save).
    # Exercises the TicketSyncService swap path with a recording factory; no extra
    # production .cpp beyond what TicketSyncService.test.cpp already links.
    Core/TrackerBackendFactoryConfig.test.cpp
    Core/OfflineQueueServiceRuntime.test.cpp
    Core/OfflineFieldEditMerge.test.cpp
    # ADR-0016 — pure scalar offline-conflict classifier (ServerMovedFromBase). Header-only
    # policy (OfflineFieldConflictPolicy.h), zero banned includes; no extra .cpp link.
    Core/OfflineFieldConflictPolicy.test.cpp
    Core/ConfigManager.test.cpp
    # audit H2 — pure POSIX secret-at-rest loose-permission decision (IsLooseConfigFileMode,
    # defined in ConfigManager_PathUtils.cpp which is already linked below). No extra .cpp.
    Core/ConfigSecretFilePerms.test.cpp
    # audit H2 follow-up — AtomicWriteTextFile POSIX hardening (O_NOFOLLOW + fchmod 0600), the
    # filesystem-bound write path (public static in ConfigManager_PathUtils.cpp, already linked
    # below). POSIX assertions run on the Linux ctest jobs; the round-trip runs everywhere. No extra .cpp.
    Core/ConfigAtomicWriteSecurity.test.cpp
    # security backlog 2026-05-17 — pure CR/LF/NUL strip for header-bound config fields
    # (SanitizeConfigStringValue, defined in ConfigManager_PathUtils.cpp linked below). No extra .cpp.
    Core/ConfigStringSanitize.test.cpp
    # vsync-toggle plan — header-only live vsync hub (VsyncControl.h): Set/Get +
    # boot precedence (env > CLI flag > config). Pure C++14, no GLFW, no extra .cpp.
    Core/VsyncControl.test.cpp
    Core/ConfigMigration.test.cpp
    Core/ToolbarConfig.test.cpp
    # Pure helpers extracted from SmatchetToolbarUi::RenderEditor during the
    # function-size decomposition (header-only inline in
    # Source/Core/src/Ui/SmatchetToolbarUi_detail.h). Links the IconCatalog TU
    # below for SmatchetToolbarIconGlyph; no extra source needed.
    Core/ToolbarEditorPure.test.cpp
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
    # PR-12 — per-client cancel + error-body redaction regressions driving the
    # REAL OpenAiClient / AnthropicClient / OllamaClient (CORE sources below) over
    # a real cpr HTTP round-trip at an in-process httplib loopback server
    # (tests/support/AiHttpFixture.h). cpr + httplib bound — these belong in this
    # cpr-bearing rig, NOT the cpr-free TSan / SQLite-free rigs (the AI clients
    # pull <cpr/cpr.h>; the fixture pulls <httplib.h>, both already linked below).
    Core/AiClientCancel.test.cpp
    Core/AiClientErrorRedact.test.cpp
    # PR #991 — pure Ollama stream-error message formatting (transport + non-2xx
    # HTTP) extracted from OllamaClient::SendStreaming. OllamaStreamError.cpp is
    # cpr-free (delegates body redaction to AiErrorRedact), linked in the CORE
    # source block below; this entry adds the doctest TU.
    Core/OllamaStreamError.test.cpp
    Core/AiModelCatalog.test.cpp
    Core/AiModelSignature.test.cpp
    # Pure helpers extracted from AiAssistantController::RunRequest (monoliths
    # decomposition). Header-only — the helpers are `inline` in
    # AiAssistantController.h; no extra .cpp source.
    Core/AiAssistantSystemPrompt.test.cpp
    # Pure agents.md cache-validity decision (AgentsMdCacheStillValid) extracted
    # from AiAssistantController::BuildChatPayload. Regression cover for the #1xxx
    # CodeRabbit finding (auto-discover toggle must invalidate the cache).
    # Header-only — the helper is `inline` in AiAssistantController.h; no .cpp.
    Core/AiAgentsMdCacheValidity.test.cpp
    Core/AiPrefsValidator.test.cpp
    Core/AiEndpointSanitize.test.cpp
    # Gap map Tier 1 #8 — Test-connection pre-network half (credential-slot routing,
    # default-URL fallback, key strip, sanitiser gate).
    Core/AiPrefsTestConnectionPure.test.cpp
    # First-send outbound-context consent gate (security audit 2026-05-17 P2).
    # Header-only pure logic in Source/Core/include/AiOutboundConsent.h — the
    # consent predicate + modal-row builder; no extra .cpp linked (the ImGui
    # modal that consumes the rows stays out of the cpr-free rig).
    Core/AiOutboundConsent.test.cpp
    # Pure rate-limit decision for the ai.prompt Lua glue (security audit H5 /
    # E6). Header-only (Source/Core/include/AiLuaPromptRateLimit.h); no extra
    # .cpp source — the glue calls the helper, the helper is the testable unit.
    Core/AiLuaPromptRateLimit.test.cpp
    Core/CliArgCoercion.test.cpp
    # Source-aware destructive guard (security audit 2026-06-13 #2/#3). Pure
    # trust predicate RequiresExplicitConfirm + IsAutomationSource live inline in
    # Source/Core/include/Commands/Command.h — no extra .cpp linked (the registry
    # TU + handlers stay out of the cpr-free rig).
    Core/CommandSourceTrust.test.cpp
    # command-input-hardening Phase 0 — pure opt-in numeric-range + max-length arg
    # bounds (Source/Core/include/Commands/ParamBoundsPure.h); header-only, no .cpp link.
    Core/ParamBoundsPure.test.cpp
    # json-fwd-swap-by-value-carriers: pins the std::shared_ptr<nlohmann::json>
    # boxing of Command.h's Default/Details/Data members + the readers' null-ptr
    # guards (Success/ToWireJson/ToJson/BuildJsonSchema). Links Command.cpp, which
    # is already pulled into this rig below for the scenario-registry tests.
    Core/CommandResultBoxing.test.cpp
    # Path-confinement leaf (Source/Core/include/Commands/PathConfinement.h) —
    # header-only; pins ConfinePathUnderBase / ConfinePathUnderSubdir traversal
    # rejections + the symlinked-subdir escape regression from the security-audit
    # MCP file-command confinement. No extra .cpp source.
    Core/PathConfinement.test.cpp
    # header-only; pins the screenshot-scenario confinement wrapper's fail-closed
    # escape rejections (absolute / ".." / empty) + scenario-prefixed reject message
    # (#1566 arbitrary-file-write class on the MCP/Lua-reachable screenshot scenarios).
    Core/ScenarioScreenshotPath.test.cpp
    Core/DictationInsertionRouter.test.cpp
    Core/AiAssistantInputSeedDecision.test.cpp
    Core/AiAssistantSendButton.test.cpp
    # Pure layout-arithmetic + picker-resolution helpers extracted from
    # SmatchetAiAssistantUi.cpp during the function-size decomposition. Header-only
    # (Source/Core/include/Ui/SmatchetAiAssistantUi_detail.h); no extra .cpp source.
    Core/AiAssistantUiDetail.test.cpp
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
    # ProbeScenario / ProbeScope — lambda-driven programmatic scenario + the
    # stack-scoped register/unregister RAII. Exercises only the AppController-free
    # surface (Name / IsDone / factory lifecycle); links ProbeScenario.cpp +
    # ScenarioRunner.cpp (already below).
    Core/ProbeScenario.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/ProbeScenario.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/ScenarioCaptureSizing.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/ScenarioRunner.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Command.cpp"
    # PR2 of docs/design/github-tracker-backend.md — pure helpers + label diff.
    Core/LabelEditDiffPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LabelEditDiffPure.cpp"
    Core/GitHubClientHelpers.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubClientHelpers.cpp"
    # linear-tracker-backend.md Slice 1 — pure GraphQL/parse helpers.
    Core/LinearClientHelpers.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearClientHelpers.cpp"
    Core/LinearIssueMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearIssueMappingPure.cpp"
    Core/LinearQueryFromJql.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearQueryFromJql.cpp"
    # linear-tracker-backend.md Slice 3 — pure write-path builders/parsers.
    # Main rig only (not the TSan subset): BuildIssueCreateInput takes an
    # IssueDraft, whose header pulls CachedTicket → SQLite, so the TU is not
    # SQLite-free. No threading surface, so the TSan rig loses nothing.
    Core/LinearMutationPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/LinearMutationPure.cpp"
    # jql-omnibox plan Stream B slice 2c — pure omnibar input classifier
    # (Jql | TicketKey | TitleSearch). Header-only (Source/Core/include/Ui/
    # OmnibarInputClassifier.h); reuses ExtractIssueKeyPrefix (ProjectResolver.cpp)
    # + ParseGitHubIssueKey (GitHubClientHelpers.cpp) — both already linked above,
    # so no new production TU.
    Core/OmnibarInputClassifier.test.cpp
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
    # SECURITY_AUDIT_2026-06-13 LOW — Logger file sink now redacts on the persist
    # path (Logger.cpp + TextRedaction.cpp already linked into this rig below).
    Core/LoggerFileSinkRedaction.test.cpp
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
    # issue-comments PR-A — pure JSON array → TrackerIssueComment mapping, split
    # out of GitHubClient.cpp (which pulls cpr). GitHubCommentMappingPure.cpp
    # depends on ParseIso8601ToUnixSec from GitHubClientHelpers.cpp (already
    # linked above), so no extra dep beyond the mapping TU itself.
    Core/GitHubCommentMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubCommentMappingPure.cpp"
    # issue-comments PR-B — pure Jira comment-node array → TrackerIssueComment
    # mapping, split out of JiraIssueSearch.cpp (which pulls cpr). Reuses
    # ParseCommentAuthor / AdfBodyToPlainText (TrackerFieldValueParser.cpp) +
    # ParseIso8601ToUnixSec (GitHubClientHelpers.cpp) — both already linked above.
    Core/JiraCommentMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraCommentMappingPure.cpp"
    # issue-comments PR-C — pure JSON array → TrackerIssueComment mapping, split
    # out of PlaneClient.cpp (which pulls cpr). PlaneCommentMappingPure.cpp depends
    # on JsonGetStringIfString (TrackerFieldValueParser.cpp) + ParseIso8601ToUnixSec
    # (GitHubClientHelpers.cpp), both already linked into this target above.
    Core/PlaneCommentMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneCommentMappingPure.cpp"
    # Slice 1 of deterministic-jira-test-backend — pure
    # Jira JSON → CachedTicket mapper + field-list builder extracted from
    # JiraIssueSearch.cpp (which pulls cpr). Same pure-seam shape as the
    # Plane and GitHub splits above.
    Core/JiraIssueMappingPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraIssueMappingPure.cpp"
    # PR A6 (function-size-compliance) — pure editmeta field-editability decision
    # extracted from JiraUserAndMeta.cpp::FetchIssueEditMeta (which pulls cpr), to
    # bring that fn under the 30-branch cap. Same pure-seam shape as the mappers.
    Core/JiraEditMetaPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraEditMetaPure.cpp"
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
    # funcsize decomposition — pure list-navigation / completion-resolution
    # helpers split out of TrackerQueryAcp_InputTextCallback (no ImGui dep).
    Core/AutocompleteListNav.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetAutocompleteUi_detail.cpp"
    Core/SelectableTextRun.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SelectableTextRun.cpp"
    # funcsize decomposition — pure geometry / selection-math helpers split out
    # of SelectableTextRun::End (bounding box, point-in-segment, segment-span).
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SelectableTextRun_detail.cpp"
    # Function-size compliance PR A9-A10 — pure byte-level image-dimension parser
    # extracted from SmatchetAttachmentPreviewUi.cpp's ParseImageDimensions. No
    # ImGui / file-I/O dependency; parses untrusted header bytes, so the test
    # pins every signature/bounds/offset/endianness path (PNG/GIF/WEBP/JPEG).
    Core/ParseImageDimensions.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/ImageDimensionsPure.cpp"
    # funcsize decomposition of drawAttachmentPreviewWindow — header-only pure
    # layout/classification helpers (fit-math + card-label/status chains). No
    # ImGui / file-I/O; mirrors the inline branches the monolith shipped.
    Core/AttachmentPreviewLayoutPure.test.cpp
    # funcsize decomposition of SmatchetDrawMcpServerPanel — header-only pure
    # diagnostics-text builder (BuildMcpServerInfoText). No ImGui / AppController;
    # takes TrackerConfig + a nullable McpServerStatus snapshot. ConfigManager.cpp
    # is already linked for the TrackerConfig definition.
    Core/McpServerInfoTextPure.test.cpp
    # Perf Opt 1 + Opt B — BuildPlan / HashContent / PreviewPlan API surface.
    # Pure-logic only (the ImGui-side render is bucket E). Links the same
    # MarkdownPreviewRender.cpp the production targets compile.
    Core/MarkdownPreviewPlan.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/MarkdownPreviewRender.cpp"
    Core/SubprocessCapturePure.test.cpp
    Core/SubprocessCapture.test.cpp
    # Wide-char argv quoting for the p4v/p4vc launcher — canonical
    # CommandLineToArgvW backslash-run doubling (trailing-backslash + embedded-
    # quote injection fix). Header-only pure helper, no production .cpp to link.
    Core/P4vLaunchArgQuotePure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/SubprocessCapturePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/SubprocessCapture.cpp"
    Plugins/Mcp/McpRequestParser.test.cpp
    Plugins/Mcp/McpEnvelope.test.cpp
    Plugins/Mcp/McpToolSchemas.test.cpp
    Plugins/Mcp/McpDispatch.test.cpp
    Plugins/Mcp/McpHostOrigin.test.cpp
    Plugins/LuaConsole/LuaConsoleSelection.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Plugins/Mcp/McpJsonRpcPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/CallstackParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/P4AnnotateParse.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerLabelsPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerDateTimePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/TicketFieldEditorLongTextPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/TicketGridDurationSortPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerGridFieldDisplayPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JqlSuggestEnginePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneQuerySuggestEnginePure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/MergeWatchNotifyPure.cpp"
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
    # Pure half of CompactDateFormat (TryParseJiraDateTime / FormatJiraDateOrDateTimeForApi),
    # split out for the ImGui-free TSan rig; this rig links both halves (CompactDateFormat.cpp
    # above keeps the display path) and TrackerFieldPayloadPure.cpp calls the pure fns.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/CompactDateFormatPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/UiPerfMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/MarkdownConvert.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/LocalCacheManager.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/BackendAuditTrail.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/FieldEditAuditSource.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Logger.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_PathUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Views.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ToolbarConfig.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/ConfigSaveWorker.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetTheme.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/TicketSyncService.cpp"
    # EditMetaCacheService (AppController god-object decomposition Phase 1). Pulls
    # GridLiveContext / ConfigManager / TrackerFieldValueUtils / TrackerFieldValueParser /
    # ProjectResolver (ExtractIssueKeyPrefix) — all already linked into this target. cpr/ImGui/
    # SQLite-free; Core/EditMetaCacheService.test.cpp drives it via header-only fakes.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/EditMetaCacheService.cpp"
    # FieldEditPipelineService (AppController god-object decomposition Phase 2). Pulls
    # EditMetaCacheService (above) + the audit pair (BackendAuditTrail / FieldEditAuditSource,
    # already linked above for OfflineQueueService) + ConfigManager + TrackerFieldValueUtils /
    # TrackerFieldSchema — all already in this target. cpr/ImGui/SQLite-free;
    # Core/FieldEditPipelineService.test.cpp drives it via header-only fakes.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/FieldEditPipelineService.cpp"
    # ConnectivityMonitorService (AppController god-object decomposition Phase 3). Pulls
    # ConfigManager + ITrackerConnectivity (header-only) + IsTrackerTransportErrorText
    # (TrackerHttpPure.cpp / TrackerHttpUtils.cpp, both already linked into this target). cpr/ImGui/
    # SQLite-free; Core/ConnectivityMonitorService.test.cpp drives it via header-only fakes.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/ConnectivityMonitorService.cpp"
    # AttachmentAppUpdateService (AppController god-object decomposition Phase 4). Pulls <cpr/cpr.h>
    # (attachment download + GitHub update query) + ConfigManager + BuildTrackerBasicAuthHeader
    # (TrackerHttpUtils.cpp, linked just below) + AttachmentMimeUtils (header-only) + nlohmann. The
    # cpr dependency is why this TU is linked ONLY into this cpr-linked rig (not the TSan / pure-sync
    # rigs); Core/AttachmentAppUpdateService.test.cpp drives it via the header-only fake.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AttachmentAppUpdateService.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerHttpUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerHttpPure.cpp"
    # Slice D of docs/plans/http-fault-injection.md — the retry+classify engine
    # (TrackerHttpRequestWithRetry / ClassifyTrackerResponse). Wired live into the
    # TrackerXxxLogged helper bodies in TrackerHttpUtils.cpp (D3), so the rig must link
    # it or TrackerHttpUtils.cpp's now-live retry calls are unresolved symbols.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerHttpClient.cpp"
    # Pure unit coverage of the retry+classify engine: ClassifyTrackerResponse status
    # mapping, retry-recovers / exhaustion-count / POST-predicate-stops semantics via
    # stateful requestFn lambdas (no socket). No extra production TU.
    Core/TrackerHttpRetry.test.cpp
    # Integration coverage of the live D3 wiring: drives TrackerGet/Post/PutLogged at a
    # scripted httplib loopback (JiraCatalogHttpFixture) and asserts retry counts prove
    # GET/PUT retry-on-5xx while POST is single-shot (Transport-only). Needs only
    # TrackerHttpUtils.cpp + TrackerHttpClient.cpp + cpr + fixture (all already linked).
    Core/TrackerHttpFaults.test.cpp
    # PR-20 (backlog-pr-roadmap) — tracker-redirect-no-follow-regression-test. SECURITY
    # regression for MakeTrackerRedirectPolicy (TrackerHttpUtils.cpp, H4/E2): asserts the
    # policy object is configured follow=false / cont_send_cred=false AND drives a real
    # tracker GET against an in-process loopback server that 302s to a DIFFERENT host:port,
    # proving the redirect is NOT followed and the Authorization header (API token) never
    # reaches the cross-host target. Needs only TrackerHttpUtils.cpp + TrackerHttpClient.cpp
    # + cpr + httplib (all already linked); the recording loopback server is in-file.
    Core/TrackerRedirectNoFollow.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/NetworkUsageTracker.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/OfflineQueueService.cpp"
    # ticket-change-monitor S1a (docs/plans/active/ticket-change-monitor.md) — three
    # backend-agnostic pure helpers behind the per-pane change monitor, each unit-tested
    # bucket-A with no backend / pane / toast manager. CachedTicketTypes.h-only (no
    # cpr/SQLite/ImGui), so the doctest rig links the two .cpp directly:
    #   * TicketChangeDiffPure — salient-field snapshot differ + toast formatter (has .cpp)
    #   * JqlChangedSincePure  — relative-JQL window wrap + absolute ISO since (has .cpp)
    #   * MembershipDiffPure   — tracked-set add/remove set-diff (header-only, no .cpp)
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/TicketChangeDiffPure.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync/JqlChangedSincePure.cpp"
    Core/TicketChangeDiffPure.test.cpp
    # ticket-change-monitor S1c-2 — ResolveSalientRoster (the item-1 residual: maps the fixed
    # canonical salient roster onto a pane's field catalog by family -> SchemaSystem -> exact-name
    # precedence). Lives in the already-listed TicketChangeDiffPure.cpp; pure (TrackerField only).
    Core/ResolveSalientRoster.test.cpp
    Core/JqlChangedSincePure.test.cpp
    Core/MembershipDiffPure.test.cpp
    # ticket-change-monitor S1b — Jira-private changelog-delta parser (issue `expand=changelog`
    # JSON -> attributed salient TicketChangeSummary). Pure: only nlohmann/json + the header-only
    # TicketChangeDiffPure structs (no JiraClient/cpr), so it links in this pure block.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraChangelogDeltaPure.cpp"
    Core/JiraChangelogDeltaPure.test.cpp
    # ticket-change-monitor S1c-1 — pure poll gate (PaneSyncKickPolicy.h ShouldPollForChanges,
    # header-only) + the tickets.monitor decision helper and its config interval clamp (the
    # latter round-trips through ConfigManager, already linked in this pure block).
    Core/ShouldPollForChanges.test.cpp
    Core/TicketsMonitorCommand.test.cpp
    # ticket-change-monitor S2 — toast-manager history ring (Ui/ToastHistoryPure.h, header-only):
    # bounded oldest-first eviction + RowAction retention, the core of SmatchetToastManager::Push's
    # new history record. Pure STL (no ImGui), so it links in this pure block.
    Core/ToastHistoryPure.test.cpp
    # ticket-change-monitor S3 — Notification Center row formatting (Ui/NotificationCenterPure.h,
    # header-only): severity tag + UTC HH:MM:SS timestamp. Pure STL (no ImGui); the window body
    # that consumes it is bucket-E (tests/ui/notification_center.test.cpp).
    Core/NotificationCenterPure.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/Views.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetToast.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/SmatchetLocalization.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiSseParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiNdjsonParser.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiClientFactory.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiErrorRedact.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiEndpointSanitize.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiPrefsTestConnectionPure.cpp"
    # AiPrefsValidator.cpp now routes base-URL checks through EndpointPolicyForProvider
    # (security audit Item B); its TU lives here, so the doctest link needs it too.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiEndpointPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/OpenAiClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AnthropicClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/OllamaClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/OllamaStreamError.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AgentsMdLoader.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiContextBuilder.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiModelCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiModelSignature.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AiPrefsValidator.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Standalone/CliArgCoercion.cpp"
    # Natural-order key/issuekey sort — exercises CompareIssueKeyNatural
    # (inline in StringUtil.h) which CompareFieldValuesForSort dispatches to.
    Core/TicketGridModelKeySort.test.cpp
    # Legacy `comment` grid-field-id canonicalization (#1291) — exercises
    # CanonicalizeGridFieldId (inline in StringUtil.h) which
    # TicketGridColumnsBuilder::Build folds each saved view field id through.
    Core/GridFieldIdCanonical.test.cpp
    # Function-size decomposition (PR A7-A8) — deterministic dispatch coverage for
    # FormatCompactJiraDateForDisplay (parse-failure passthrough + absolute_iso /
    # absolute_friendly). CompactDateFormat.cpp is already linked above (line ~233).
    Core/CompactDateFormat.test.cpp
    # PR #880 product bug-fix sweep — pure relocations extracted so the coverage-delta
    # gate has meaningful unit deltas. Both are header-only inline pures (no extra
    # .cpp link): EscapeXmlAttr (#826 XML-attr escaping, "AiXmlAttrEscape.h") and
    # MaskEmailForLog (#820 PII log redaction, "EmailMaskForLog.h").
    Core/XmlAttrEscape.test.cpp
    Core/EmailMask.test.cpp
    # build-quality-velocity-hardening #31 — pure CSV/TSV/JSON import+export surface
    # of IssueTableSerializer. No cpr/AppController/ImGui dep — pure stdlib + nlohmann.
    Core/IssueTableSerializer.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/IssueTableSerializer.cpp"
    # Slice 0 WS2 of docs/plans/multi-grid-tabs.md — real catalog-BUILD
    # coverage closing the test.md P2 (2026-06-01): JiraFakeTrackerFixture injects a
    # pre-built catalog and bypasses FetchFieldCatalog assembly, so the PR #672
    # "components rendered as text" bug class was fixture-invisible. The test drives
    # a real JiraClient at an in-process httplib::Server (loopback, scripted JSON)
    # via tests/support/JiraCatalogHttpFixture.h. Instantiating JiraClient needs the
    # full vtable, so all six Jira implementation TUs link here (JiraActivityFeed
    # joined in user-info-window Slice 3 — it defines the ITrackerActivity virtuals);
    # their deps (cpr, TrackerHttpUtils, TrackerFieldCatalogPure, TrackerFieldPayload,
    # OfflineQueue, BackendAuditTrail, ghc_filesystem) are already in the rig.
    Core/TrackerCatalogBuild.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraClient.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraIssueSearch.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraIssueMutation.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraUserAndMeta.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/JiraActivityFeed.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/TrackerFieldCatalog.cpp"
    # build-quality-velocity-hardening #32 — flaky-test quarantine mechanism self-test.
    # Demonstrates SMATCHET_QUARANTINED_TEST_CASE / SMATCHET_QT_CHECK env-flip behaviour.
    # Header-only (QuarantineTestCase.h in tests/support/); no extra .cpp source.
    Core/FlakySelfTest.test.cpp
    # multi-grid-tabs.md Slice 0 WS1 — characterize-existing seam net pinning
    # single-grid behaviour before the multi-pane refactor. ConfigManager_Views.cpp
    # and Ui/Views.cpp are already linked above; no new production TUs.
    Core/ConfigManagerViews.test.cpp
    Core/ViewsClass.test.cpp
    # June 2026 view-create crash fix (Pillar 3) — deferred view-create latch core
    # (ApplyPendingViewCreateCore, inline in SmatchetViewsDashboardUi_detail.h):
    # consume-once apply, unset-latch no-mutation, duplicate-path source survival.
    # Views.cpp / ConfigManager_Views.cpp already linked above; no new production TUs.
    Core/ViewsPendingCreate.test.cpp
    # multi-grid-tabs.md Slice 2 — smatchet_panes.json round-trip + zero-migration
    # default-pane bootstrap (plan item 13) + GridPane per-pane sort-cache isolation
    # (plan items 7/10). ConfigManager_Panes.cpp is the new production TU.
    Core/ConfigManagerPanes.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Panes.cpp"
    # PR #962 review fixes — pure pane-request core (close sweep focus-reassignment
    # report, min-1 invariant, "+" duplicate, cross-backend view self-repair gate).
    # The _detail TU is ImGui-free (mirrors SmatchetGridHeaderUi_detail.cpp).
    Core/GridPaneRequests.test.cpp
    # pane-backend-picker Slice 1 — PaneAddRequest struct + ApplyPaneAddAndCloseRequestsCore
    # extended for cross-backend: ResolveNewPaneView resolution order + cross-backend
    # branch (no snapshot inherit). The _detail TU is already linked above.
    # Issue #1458 — DecidePaneAddRequest arm-only-on-success credential gate. The pure
    # decision lives in PaneCommands_detail.cpp (ImGui/AppController-free); the handler
    # half (PaneCommands.cpp) pulls the UI session + stays OUT of the rig.
    Core/PaneAddRequest.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/PaneCommands_detail.cpp"
    # multi-grid-tabs.md Slice 1a — per-pane GridLiveContext bundle: default state,
    # ADR-0012 Backend slot atomic store/load contract, active-ticket publish path.
    # TicketSyncService.cpp (needed by ~GridLiveContext) is already linked above.
    Core/GridLiveContext.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/GridLiveContext.cpp"
    # user-info-window.md Slices 1+2 — pure VCS-feed core: p4 `changes -u` parse
    # (ParseChangesForUserOutput; P4AnnotateParse.cpp already linked above),
    # email→p4-user, p4-date normalisation, day-window cutoff, GitHub commit-list
    # JSON parse, newest-first two-feed merge. GitHubCommitsParse.cpp is the pure
    # parse TU — the cpr/HTTP fetch half (GitHubCommits.cpp) stays OUT of the rig.
    Core/VcsSubmission.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Vcs/VcsSubmission.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Vcs/GitHubCommitsParse.cpp"
    # user-info-window.md Slice 3 — pure activity-feed core: discovery-JQL build,
    # 8h-workday estimate formatting, changelog-JSON -> TrackerActivityEntry mapping,
    # and the CL / issue-key free-text scans behind "add to query". All production
    # TUs (JiraActivityFeed.cpp, P4AnnotateParse.cpp, ProjectResolver.cpp) are
    # already linked above.
    Core/JiraActivityFeed.test.cpp
    # user-info-window.md Slice 4 — pure Plane/GitHub activity-feed cores. The
    # *Pure.cpp TUs hold only the JSON -> TrackerActivityEntry mapping halves; the
    # client halves (PlaneActivityFeed.cpp / GitHubActivityFeed.cpp) pull cpr +
    # the full client TU sets and stay OUT of the rig. GitHubClientHelpers.cpp
    # (FormatGitHubIssueKey) is already linked above.
    Core/PlaneActivityFeed.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneActivityFeedPure.cpp"
    Core/GitHubActivityFeed.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/GitHubActivityFeedPure.cpp"
    # Coverage ramp (global-floor 65->70) — the pure free-text issue-key scan half of
    # ProjectResolver (FindFirstIssueKeyInText) that ProjectComponentResolution.test.cpp
    # does not exercise. ProjectResolver.cpp is already linked above (line ~301).
    Core/ProjectResolverPure.test.cpp
    # Coverage ramp (global-floor 65->70) — pure Plane structured-query project-scoping
    # helper (SetProjectInQuery). Depends only on nlohmann (already linked); no cpr/sqlite.
    Core/PlaneProjectScope.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker/PlaneProjectScope.cpp"
    # keyboard-shortcuts-rebindable PR1 — pure hotkey grammar (Parse/Stringify/
    # FindShortcutConflict; MatchHotkey is ImGui-IO-bound and excluded -> bucket-E)
    # + the rebindable keybinding table (Defaults() parity, JSON round-trip,
    # malformed-skip). ImGuiHotkey.cpp links against the already-linked ImGuiLib;
    # KeybindingsConfig.cpp pulls only nlohmann (already linked).
    Core/ImGuiHotkey.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/ImGuiHotkey.cpp"
    Core/KeybindingsConfig.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/KeybindingsConfig.cpp"
    # bucket-E diagnosability (PR #1432) -- pure, engine-free formatter for the
    # ui_test.run --spawn child-log outcome dump (KO list + pass/fail summary).
    # The ImGui-dependent extraction stays in UiTestScenario.cpp; this rig links
    # only the engine-free UiTestOutcomeFormat.cpp so it pulls no imgui/cpr/sqlite.
    Core/UiTestOutcomeFormat.test.cpp
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/UiTestOutcomeFormat.cpp"
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
target_include_directories(SmatchetTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source/Plugins/LuaConsole")
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
        # Network-hardening (security audit LOW): host-pin + size-cap policy for the
        # model-download redirect path. Pure decision; no cpr / no I/O.
        Plugins/Whisper/ModelDownloadPolicy.test.cpp
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
        "${CMAKE_SOURCE_DIR}/Source/Plugins/Whisper/ModelDownloadPolicy.cpp"
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
# SQLiteCpp is linked at runtime as of Phase 3 — LocalCacheManager.cpp pulls SQLite/Statement
# objects directly (IssueCreatePipeline.cpp is SQLite-free since the ADR-0020 ISyncCache seam —
# it reaches the cache only through the interface). md4c is linked at runtime so MarkdownConvert.cpp
# (transitively used by TrackerFieldPayload) resolves.
target_link_libraries(SmatchetTests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
    ImGuiLib
    SQLiteCpp
    md4c
    cpr::cpr
    ghc_filesystem
    # Slice 0 WS2 of multi-grid-tabs — JiraCatalogHttpFixture.h serves scripted
    # Jira REST JSON from an in-process loopback server (header-only interface
    # target; same pin the production target links).
    httplib::httplib
)

# ConfigManager.cpp uses CryptProtectData / CryptBinaryToStringA from wincrypt.
# Linked here because the test exe pulls ConfigManager via TrackerFieldValueUtils.
if(WIN32)
    target_link_libraries(SmatchetTests PRIVATE crypt32)
endif()

target_compile_features(SmatchetTests PRIVATE cxx_std_14)

# B8 (/EHsc on SmatchetTests, docs/plans/b8-bucket-e-coverage.md) — force
# C++ exception unwinding ON for the doctest rig, immune to flag clobbering. The
# default /EHsc normally arrives via CMAKE_CXX_FLAGS, but two things can strip it
# for the test build: (1) a partial/stale configure that leaves CMAKE_CXX_FLAGS
# empty (observed locally: the app build dir had `/DWIN32 /D_WINDOWS /EHsc`, the
# test build dir had an EMPTY CMAKE_CXX_FLAGS), and (2) cpr 1.9.2 appending
# `-Wall -Wextra -Wpedantic -Werror` to CMAKE_CXX_FLAGS mid-configure (CMakeLists.txt
# ~L488), which turns the resulting C4530 "unwind semantics not enabled" into a fatal.
# Without /EHsc, doctest's CHECK_THROWS* static_asserts fire C2338. PRIVATE + the
# MSVC generator-expression guard keep Clang/Linux test presets untouched.
target_compile_options(SmatchetTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

# Pillar-3 sanitizer coverage of the doctest rig (infra: asan-doctest-rig-not-instrumented).
# The ninja-msvc-asan preset historically instrumented only the app targets
# (SmatchetStandalone / SmatchetCore_DX12), so heap UAF inside a test TU compiled
# UNINSTRUMENTED and could never trip ASan — every prior "ASan-clean" doctest claim
# was vacuous. SMATCHET_SANITIZE_TESTS opts the doctest rig into ASan; the
# ninja-msvc-asan preset now sets it ON by default (with SMATCHET_BUILD_TESTS=ON),
# so the WS-A cancel repro (Core/UserInfoActivityCancelUaf.test.cpp) and the whole
# rig run GENUINELY instrumented. The historical /RTC1-vs-/fsanitize=address SIGABRT
# (exception-based death tests, e.g. SmatchetResult.test.cpp CHECK_THROWS_AS) is
# resolved upstream: smatchet_strip_msvc_rtc_for_asan() (cmake/Sanitizers.cmake,
# called from the root before any target) strips /RTC* tree-wide under MSVC ASan,
# so the throw/unwind path no longer aborts. The knob stays a knob (default OFF
# globally) so non-asan test presets are unaffected; the asan preset flips it on.
# ImGuiLib stays uninstrumented (linked as-is); MSVC ASan tolerates the mixed link.
# Manual run THROUGH the MSVC env (the clang_rt.asan_dynamic DLL must be on PATH):
#   with-msvc-env.sh bash -c 'ASAN_OPTIONS=abort_on_error=1 ./...SmatchetTests.exe'
option(SMATCHET_SANITIZE_TESTS "Apply SMATCHET_SANITIZER to the SmatchetTests doctest rig" OFF)
if(SMATCHET_SANITIZE_TESTS)
    smatchet_apply_sanitizers(SmatchetTests)
endif()

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

# Glob-vs-list guard (build-quality-velocity-hardening #5 + close-gate-gaps Slice 5):
# the SmatchetTests source list above is hand-maintained — a new *.test.cpp that is
# never added is silently uncompiled, so its assertions never run yet CI stays green
# (false green). Assert at configure time that every test on disk under Core/ AND
# Plugins/ is referenced somewhere in THIS file. Text-match the full tests/-relative
# path (not the SOURCES property) so conditionally-gated tests (the Whisper /
# agent-debug blocks) still count. Slice 5 widened the Core-only original to Plugins/
# (Mcp + LuaConsole carry the same false-green risk). Mirrored by
# agents/scripts/core/check-test-list.sh (--check/--selftest).
file(GLOB _smatchet_unit_test_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/Core/*.test.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/*/*.test.cpp")
file(READ "${CMAKE_CURRENT_LIST_FILE}" _smatchet_tests_cml_text)
set(_smatchet_unlisted_tests "")
foreach(_tf IN LISTS _smatchet_unit_test_files)
    string(FIND "${_smatchet_tests_cml_text}" "${_tf}" _tf_pos)
    if(_tf_pos EQUAL -1)
        list(APPEND _smatchet_unlisted_tests "${_tf}")
    endif()
endforeach()
if(_smatchet_unlisted_tests)
    string(REPLACE ";" "\n  " _smatchet_unlisted_pretty "${_smatchet_unlisted_tests}")
    message(FATAL_ERROR
        "tests/CMakeLists.txt: these tests/{Core,Plugins}/**/*.test.cpp exist on disk but are "
        "referenced nowhere in this file — they would be silently uncompiled (false green):\n  "
        "${_smatchet_unlisted_pretty}\n"
        "Add each to add_executable(SmatchetTests ...) (or the appropriate conditional block).")
endif()

# Sync-cache purity guard (ADR-0020 / ilocalcache-seam PR2): the service-test TUs + the shared
# deps fakes must stay SQLite-free at the CONSTRUCTION + DIRECT-INCLUDE level — they run on
# FakeSyncCache (contract-suite-verified). Transitive `<SQLiteCpp/...>` via the service headers'
# AppController.h include is the known, accepted residual (ADR-0020; debt.md tracks closing it).
# SQLite-by-design TUs (the contract suite's real half, the RealCacheSmoke backstop, the LCM
# impl/migration TUs) are deliberately NOT in this set. NOTE: the regex sees comments too —
# don't spell the construct `LocalCacheManager(` in a comment inside the set (reword instead;
# keeps the guard a simple, dependable text match).
set(_smatchet_pure_sync_test_files
    Core/BackendSwitchRace1081.test.cpp
    Core/OfflineQueueServiceRuntime.test.cpp
    Core/OfflineQueueTwoBackendReplay.test.cpp
    Core/OfflineQueueBackendSwap.test.cpp
    Core/OfflineQueueBackendKey.test.cpp
    Core/TicketSyncService.test.cpp
    Core/TrackerBackendFactoryConfig.test.cpp
    Core/IssueCreatePipelineIntegration.test.cpp
    # EditMetaCacheService (AppController god-object decomposition Phase 1) — the service-test
    # TUs + FakeEditMetaDeps stay SQLite-free (run on header-only fakes; the editmeta cache is
    # plain in-memory containers, no LocalCacheManager).
    Core/EditMetaCacheService.test.cpp
    Core/EditMetaCacheConcurrent.test.cpp
    # FieldEditPipelineService (Phase 2) + its fake stay SQLite-free too (HasCache() is a
    # predicate, not the raw Cache* — ADR-0020 sync-cache purity).
    Core/FieldEditPipelineService.test.cpp
    # ConnectivityMonitorService (Phase 3) + its fake stay SQLite-free — the FSM is plain in-memory
    # state + a std::async probe future; no LocalCacheManager. Replay-timer pushes route through the
    # deps interface (recorded in the fake), never a Cache*.
    Core/ConnectivityMonitorService.test.cpp
    support/FakeEditMetaDeps.h
    support/FakeFieldEditDeps.h
    support/FakeConnectivityDeps.h
    support/FakeOfflineQueueDeps.h
    support/FakeTicketSyncDeps.h
    support/FakeSyncCache.h)
foreach(_pf IN LISTS _smatchet_pure_sync_test_files)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_pf}" _pf_text)
    # Re-configure when a guarded file changes — file(READ) alone registers no dependency, so a
    # violation introduced after the last configure would pass local incremental builds.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_pf}")
    # [>({] covers the construction forms actually in use: make_unique<LocalCacheManager>(...),
    # unnamed brace-init LocalCacheManager{...}, and temporaries LocalCacheManager(...). A NAMED
    # stack variable (`LocalCacheManager mgr(...)`) is not cheaply text-matchable — the
    # direct-include rule below is the backstop for that form. (Review HIGH: the original
    # `[ \t]*\(` pattern matched NONE of the real construction forms — vacuous.)
    if(_pf_text MATCHES "LocalCacheManager[ \t]*[>({]")
        message(FATAL_ERROR
            "tests/${_pf}: constructs LocalCacheManager — this TU is in the SQLite-free service-test "
            "set (ADR-0020 sync-cache purity). Use FakeSyncCache, or move the case to an impl TU "
            "(LocalCacheManager*.test.cpp / LocalCacheTicketsV2Migration.test.cpp).")
    endif()
    if(_pf_text MATCHES "#include[ \t]*\"[^\"]*(LocalCacheManager|SqliteMemFixture)\\.h\"")
        message(FATAL_ERROR
            "tests/${_pf}: direct-includes LocalCacheManager.h/SqliteMemFixture.h — this TU is in "
            "the SQLite-free service-test set (ADR-0020 sync-cache purity). Reach the cache through "
            "ISyncCache/FakeSyncCache.h instead.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Issue #1068 Android OpenSSL fail-fast — behavioral configure-probe (WS5 item 19,
# docs/plans/mobile-mvp-completion.md). Drives the REAL per-ABI static-OpenSSL
# resolve block (cmake/SmatchetThirdParty.cmake smatchet_prepare_cpr) in `cmake -P`
# script mode against committed fixture trees under fixtures/android-openssl/ — no
# NDK, compiler, or cpr fetch needed. These are pure-CMake tests (no SmatchetTests
# link dep) so they run even when the C++ suite is gated off, and the Windows
# test/Coverage CI job exercises them via ctest.
#
# Division of labour: the static text gate (agents/scripts/project/test-mobile-security.sh,
# CI "Android security gate") proves the marker sentence is bound to a
# message(FATAL_ERROR); THIS probe proves the EXISTS-triple guard LOGIC actually
# fails the configure (catches an AND->OR / wrong-filename regression that keeps the
# text). tests/bats/android_openssl_failfast.bats mirrors these cases locally and
# adds whitespace-normalized marker-text assertions (cmake wraps FATAL_ERROR text, so
# ctest gates on the deterministic EXIT CODE here, not a wrap-fragile output regex).
set(_ossl_probe "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android-openssl/probe.cmake")
set(_ossl_thirdparty "${CMAKE_SOURCE_DIR}/cmake/SmatchetThirdParty.cmake")
set(_ossl_fix "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/android-openssl")

# Complete per-ABI tree (libssl.a + libcrypto.a + opensslv.h) resolves: exit 0 + PROBE_OK.
add_test(NAME android_openssl_failfast_complete
    COMMAND "${CMAKE_COMMAND}"
        "-DSMATCHET_THIRDPARTY_CMAKE=${_ossl_thirdparty}"
        "-DSMATCHET_ANDROID_OPENSSL_BASE=${_ossl_fix}/complete"
        -DANDROID_ABI=arm64-v8a -P "${_ossl_probe}")
set_tests_properties(android_openssl_failfast_complete PROPERTIES
    PASS_REGULAR_EXPRESSION "PROBE_OK")

# Missing libcrypto.a -> the EXISTS-triple guard fails the configure (nonzero exit).
add_test(NAME android_openssl_failfast_missing_crypto
    COMMAND "${CMAKE_COMMAND}"
        "-DSMATCHET_THIRDPARTY_CMAKE=${_ossl_thirdparty}"
        "-DSMATCHET_ANDROID_OPENSSL_BASE=${_ossl_fix}/missing-crypto"
        -DANDROID_ABI=arm64-v8a -P "${_ossl_probe}")
# Validate the SPECIFIC #1068 fail-fast, not just a nonzero exit: PASS only when the
# OpenSSL-pinning FATAL_ERROR text appears (whitespace-tolerant — CMake word-wraps the
# message), and FAIL hard if the PROBE_OK success marker ever shows. PASS_REGULAR_EXPRESSION
# ignores the exit code, so it supersedes (and must replace, not pair with) WILL_FAIL.
set_tests_properties(android_openssl_failfast_missing_crypto PROPERTIES
    PASS_REGULAR_EXPRESSION "Android[ \t\r\n]+requires[ \t\r\n]+a[ \t\r\n]+pinned[ \t\r\n]+static[ \t\r\n]+OpenSSL"
    FAIL_REGULAR_EXPRESSION "PROBE_OK")

# Missing public headers -> fail the configure (nonzero exit).
add_test(NAME android_openssl_failfast_missing_header
    COMMAND "${CMAKE_COMMAND}"
        "-DSMATCHET_THIRDPARTY_CMAKE=${_ossl_thirdparty}"
        "-DSMATCHET_ANDROID_OPENSSL_BASE=${_ossl_fix}/missing-header"
        -DANDROID_ABI=arm64-v8a -P "${_ossl_probe}")
set_tests_properties(android_openssl_failfast_missing_header PROPERTIES
    PASS_REGULAR_EXPRESSION "Android[ \t\r\n]+requires[ \t\r\n]+a[ \t\r\n]+pinned[ \t\r\n]+static[ \t\r\n]+OpenSSL"
    FAIL_REGULAR_EXPRESSION "PROBE_OK")

# Complete base but an unbuilt ABI (Gradle drives both ABIs from one base) -> fail.
add_test(NAME android_openssl_failfast_missing_abi
    COMMAND "${CMAKE_COMMAND}"
        "-DSMATCHET_THIRDPARTY_CMAKE=${_ossl_thirdparty}"
        "-DSMATCHET_ANDROID_OPENSSL_BASE=${_ossl_fix}/complete"
        -DANDROID_ABI=mips -P "${_ossl_probe}")
set_tests_properties(android_openssl_failfast_missing_abi PROPERTIES
    PASS_REGULAR_EXPRESSION "Android[ \t\r\n]+requires[ \t\r\n]+a[ \t\r\n]+pinned[ \t\r\n]+static[ \t\r\n]+OpenSSL"
    FAIL_REGULAR_EXPRESSION "PROBE_OK")

# No base supplied -> block is skipped (not fatal): exit 0 + PROBE_OK. The probe
# falls back to ENV{SMATCHET_ANDROID_OPENSSL_BASE} when the CMake var is unset and CI
# exports that var for the real Android build, so pin it empty for a true no-base case.
add_test(NAME android_openssl_failfast_no_base
    COMMAND "${CMAKE_COMMAND}"
        "-DSMATCHET_THIRDPARTY_CMAKE=${_ossl_thirdparty}"
        -DANDROID_ABI=arm64-v8a -P "${_ossl_probe}")
set_tests_properties(android_openssl_failfast_no_base PROPERTIES
    PASS_REGULAR_EXPRESSION "PROBE_OK"
    ENVIRONMENT "SMATCHET_ANDROID_OPENSSL_BASE=")
