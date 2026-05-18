# stub_claude — tiny C++ exe that masquerades as the Claude Code CLI for
# tests/Source_Core/ClaudeCodeLocalRunner.test.cpp. Pure stdlib; no Smatchet
# headers. Built unconditionally when tests are enabled — gating the stub
# on SMATCHET_WITH_AGENTIC would couple test infrastructure to a runtime
# feature flag and complicate the regression matrix (test-all with AGENTIC=OFF
# should still build the stub even though the runner test is gated off).

add_executable(stub_claude stub_claude.cpp)
target_compile_features(stub_claude PRIVATE cxx_std_14)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(stub_claude PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()
