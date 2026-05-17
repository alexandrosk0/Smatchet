# tests/ui — bucket E (ImGui Test Engine) test sources.
#
# Enrolled into SmatchetStandalone (and DX12 stub) at the root CMakeLists.txt
# level when SMATCHET_BUILD_UI_TESTS=ON. Tests link into the running app exe
# rather than a separate runner — the engine drives the live UI loop.

if(NOT SMATCHET_BUILD_UI_TESTS)
    return()
endif()

set(_SMATCHET_UI_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_tests_registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/views_columns_reorder.test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/callstack_tooltip_hover.test.cpp"
)

# Link the test source list into both Standalone (live runner) and the DX12
# stub (compile tripwire — DX12 has no ui_test.run path but compiling the
# tests there ensures no Source_Core leakage into the ImGui engine include
# cascade).
target_sources(SmatchetStandalone PRIVATE ${_SMATCHET_UI_TEST_SOURCES})
target_link_libraries(SmatchetStandalone PRIVATE SmatchetImGuiTestEngine)

if(TARGET SmatchetCore_DX12 AND TARGET SmatchetImGuiTestEngine_DX12)
    target_sources(SmatchetCore_DX12 PRIVATE ${_SMATCHET_UI_TEST_SOURCES})
    target_link_libraries(SmatchetCore_DX12 PRIVATE SmatchetImGuiTestEngine_DX12)
endif()
