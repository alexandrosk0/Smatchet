# stub_git + stub_gh — tiny stdlib-only exes that masquerade as `git` and
# `gh` for tests/Source_Core/ClaudeCodeLocalRunner.test.cpp's PR-open
# fallback cases (H6). Built unconditionally when tests are enabled — same
# rationale as stub_claude. The runner test injects their absolute paths
# via Options::gitBinPath / ghBinPath when the fallback branch needs to fire.

add_executable(stub_git stub_git.cpp)
add_executable(stub_gh stub_gh.cpp)
target_compile_features(stub_git PRIVATE cxx_std_14)
target_compile_features(stub_gh PRIVATE cxx_std_14)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(stub_git PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
    target_link_options(stub_gh PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()
