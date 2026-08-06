# SmatchetFontAssets.cmake — resolve the gitignored font assets at configure time.
#
# assets/fonts/*.ttf is NOT committed (430 KB binary, SIL OFL 1.1 — see
# assets/fonts/README.md for the drop-in contract; CI fetches it via
# .github/actions/fetch-fontawesome). A linked git worktree created with
# `git worktree add` therefore starts with an EMPTY assets/fonts/, even though
# the main worktree has the TTF sitting right there. Builds from such a
# worktree skipped the POST_BUILD copy, the runtime exe-dir and
# SMATCHET_ASSETS_SOURCE_DIR resolvers in SmatchetImGuiFonts.cpp both missed,
# and every Font Awesome glyph in the toolbar / icon picker / AI-chat hover row
# silently degraded to its text fallback.
#
# smatchet_resolve_font_asset(<outVar> <sourceDir> <fileName>) resolves, in
# order:
#   1. <sourceDir>/assets/fonts/<fileName>       — normal clone / CI fetch
#   2. <mainWorktree>/assets/fonts/<fileName>    — linked-worktree fallback
# where <mainWorktree> is the parent directory of `git rev-parse
# --git-common-dir` (that common dir is the MAIN worktree's .git, shared by
# every linked worktree). Sets <outVar> to the first hit, or "" when neither
# exists — callers degrade, they never fail the build.

function(smatchet_resolve_font_asset outVar sourceDir fileName)
    set(_direct "${sourceDir}/assets/fonts/${fileName}")
    if(EXISTS "${_direct}")
        set(${outVar} "${_direct}" PARENT_SCOPE)
        return()
    endif()

    # Reuse the toolchain's git when a find_package(Git) already ran; otherwise
    # probe once into a cache entry of its own (feeding an empty local variable
    # to find_program would make it skip the search and answer nothing).
    set(_git "${GIT_EXECUTABLE}")
    if(NOT _git)
        find_program(SMATCHET_FONT_ASSET_GIT NAMES git)
        set(_git "${SMATCHET_FONT_ASSET_GIT}")
    endif()
    if(NOT _git)
        set(${outVar} "" PARENT_SCOPE)
        return()
    endif()

    # --path-format=absolute (git >= 2.31) keeps this independent of the
    # working directory; without it --git-common-dir may answer a relative path.
    execute_process(
            COMMAND "${_git}" -C "${sourceDir}" rev-parse --path-format=absolute --git-common-dir
            OUTPUT_VARIABLE _commonDir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _gitRc)
    if(NOT _gitRc EQUAL 0 OR NOT _commonDir)
        set(${outVar} "" PARENT_SCOPE)
        return()
    endif()

    # <main>/.git -> <main>. A bare/worktree-less layout yields something that
    # simply fails the EXISTS check below, which is the correct outcome.
    get_filename_component(_mainTree "${_commonDir}" DIRECTORY)
    set(_fallback "${_mainTree}/assets/fonts/${fileName}")
    if(EXISTS "${_fallback}")
        set(${outVar} "${_fallback}" PARENT_SCOPE)
    else()
        set(${outVar} "" PARENT_SCOPE)
    endif()
endfunction()
