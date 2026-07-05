# tests/ui — bucket E (ImGui Test Engine) test sources.
#
# Enrolled into SmatchetStandalone (and DX12 stub) at the root CMakeLists.txt
# level when SMATCHET_BUILD_UI_TESTS=ON. Tests link into the running app exe
# rather than a separate runner — the engine drives the live UI loop.

if(NOT SMATCHET_BUILD_UI_TESTS)
    return()
endif()

# Slice 5 of docs/design/pillar-1-2-perf-review-system.md — opt-in flag that
# compiles the synthetic stall hook in tests/ui/sync_stall_visible_cue.test.cpp.
# Off by default even under SMATCHET_BUILD_UI_TESTS=ON so the file becomes a
# no-op shell when the harness isn't wanted; on under the ninja-ui-test
# preset where the bucket-E driver expects it.
option(SMATCHET_DEBUG_VISIBLE_CUE_HARNESS "Compile the synthetic visible-cue stall hook in bucket-E tests" OFF)

set(_SMATCHET_UI_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_tests_registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/jira_deterministic_backend.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/linear_deterministic_backend.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/views_columns_reorder.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/callstack_tooltip_hover.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/help_marker_keyboard_focus.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp_lua_fresh_state_race.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_panel_dock_swap.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_enter_send.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_prefs_autosave_flow.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_preferences_docking.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_preferences_enter_send.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_preferences_validation_banner.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_preferences_save_discard.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_preferences_test_connection.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_preferences_verify_on_save.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai_assistant_model_change_strip.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/description_tooltip_markdown_render.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/spawn_warmup_deterministic_gate.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/annotate_prefs_persist_flow.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/data_dependent_windows_smoke.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/notification_center.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/funcsize_window_render_smoke.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/funcsize_preferences_tabs.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/funcsize_main_ui_smoke.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/funcsize_grid_render.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/grid_pane_windows.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/grid_wheel_route_ownership.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/reset_layout_docking.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/duration_inline_edit_commit.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mobile_views_confirm_modal.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/keybindings_editor_rebind.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/command_palette_inline_typing.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/omnibar_search_apply.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/offline_conflict_modal_panes.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/whisper_ai_assistant_autosend.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sync_stall_visible_cue.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/annotate_before_cl_cue.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/toolbar_append_cache_cue.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/attachment_thumbnail_loading_cue.test.cpp"
    # nightly-monkey-tester Layer 2 — random-input UI monkey (opt-in via SMATCHET_UI_MONKEY=1;
    # inert under `ui_test.run --all`). Compiled here so the Bucket-E build type-checks it.
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_monkey.test.cpp"
)

# Link the test source list into both Standalone (live runner) and the DX12
# stub (compile tripwire — DX12 has no ui_test.run path but compiling the
# tests there ensures no Source/Core leakage into the ImGui engine include
# cascade).
target_sources(SmatchetStandalone PRIVATE ${_SMATCHET_UI_TEST_SOURCES})
target_link_libraries(SmatchetStandalone PRIVATE SmatchetImGuiTestEngine)

# The ImGui Test Engine IM_CHECK_* macros compare against literal expecteds, which MSVC
# (14.44 on the CI bucket-E runner) flags as C4127 "conditional expression is constant" —
# a false positive that the project's /WX turns fatal. Scope /wd4127 to the bucket-E test
# sources only (not product code). MSVC-only flag; the bucket-E build is MSVC.
# TARGET_DIRECTORY is required: SmatchetStandalone / SmatchetCore_DX12 are defined in the
# ROOT CMakeLists, so a bare set_source_files_properties here (tests/ui/ scope) would not
# reach the compile line (CMake source-file properties are directory-scoped).
if(MSVC)
    set_source_files_properties(${_SMATCHET_UI_TEST_SOURCES}
        TARGET_DIRECTORY SmatchetStandalone
        PROPERTIES COMPILE_OPTIONS "/wd4127")
    if(TARGET SmatchetCore_DX12)
        set_source_files_properties(${_SMATCHET_UI_TEST_SOURCES}
            TARGET_DIRECTORY SmatchetCore_DX12
            PROPERTIES COMPILE_OPTIONS "/wd4127")
    endif()
endif()

# Shared bucket-E header-only helpers (plan b8-bucket-e-coverage L3) live in
# tests/ui/_helpers/. Header-only — no source-list entry; just put the dir on
# the include path so TUs can `#include "BucketETooltip.h"`.
target_include_directories(SmatchetStandalone PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/_helpers")
if(TARGET SmatchetCore_DX12)
    target_include_directories(SmatchetCore_DX12 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/_helpers")
endif()

# Slice 3 of deterministic-jira-test-backend — fixture
# support source + include path for the Jira backend injection hook in
# StandaloneAppBootstrap.cpp. Also compiled into DX12 as a no-GL tripwire.
target_sources(SmatchetStandalone PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/support/JiraFakeTrackerFixture.cpp")
target_include_directories(SmatchetStandalone PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/support")

if(TARGET SmatchetCore_DX12 AND TARGET SmatchetImGuiTestEngine_DX12)
    target_sources(SmatchetCore_DX12 PRIVATE ${_SMATCHET_UI_TEST_SOURCES})
    target_link_libraries(SmatchetCore_DX12 PRIVATE SmatchetImGuiTestEngine_DX12)
endif()

if(SMATCHET_DEBUG_VISIBLE_CUE_HARNESS)
    # Target-wide PRIVATE define. Only sync_stall_visible_cue.test.cpp reads
    # the macro (inside a `#ifdef` guard), so other TUs are unaffected. We
    # use target_compile_definitions rather than set_source_files_properties
    # because the source-file property doesn't propagate across the subdir
    # → root-CMakeLists target boundary (the targets are declared in the
    # root CMakeLists, modified from this subdir).
    target_compile_definitions(SmatchetStandalone PRIVATE SMATCHET_DEBUG_VISIBLE_CUE_HARNESS=1)
    if(TARGET SmatchetCore_DX12)
        target_compile_definitions(SmatchetCore_DX12 PRIVATE SMATCHET_DEBUG_VISIBLE_CUE_HARNESS=1)
    endif()
endif()
