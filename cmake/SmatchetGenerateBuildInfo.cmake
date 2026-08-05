# ============================================================================
# SmatchetGenerateBuildInfo.cmake — run in SCRIPT MODE (`cmake -P`), not included.
#
# Regenerates ${SMATCHET_OUT} from ${SMATCHET_TEMPLATE}, filling in git
# provenance probed fresh at *build* time (a plain configure_file would freeze
# the SHA at configure time and go stale on the next commit).
#
# Writes via a temp file + `copy_if_different` so an unchanged SHA never moves
# the real header's mtime — that is what keeps the always-run ALL target from
# causing a rebuild storm.
#
# HARD RULE: this script must NEVER `message(FATAL_ERROR ...)`. It runs on every
# local build; a hard gate here would brick the tree for anyone without git, and
# would violate the `cmake-local-gate-ci-scope` rule in AGENTS.md. Every failure
# degrades to "unknown".
#
# Expected -D arguments:
#   SMATCHET_TEMPLATE     path to cmake/SmatchetBuildInfo.h.in
#   SMATCHET_OUT          path to the generated SmatchetBuildInfo.h
#   SMATCHET_STATIC_VARS  path to the configure-time-emitted static var script
#   SMATCHET_SRC_DIR      repo root (git working directory)
#   GIT_EXECUTABLE        git, or GIT_EXECUTABLE-NOTFOUND
# ============================================================================
cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------- defaults ---
set(SMATCHET_GIT_SHA "unknown")
set(SMATCHET_GIT_SHA_SHORT "unknown")
set(SMATCHET_GIT_BRANCH "unknown")
set(SMATCHET_GIT_DIRTY 0)
set(SMATCHET_GIT_SHALLOW 0)
set(SMATCHET_CXX_COMPILER_ID "unknown")
set(SMATCHET_CXX_COMPILER_VERSION "unknown")
set(SMATCHET_CMAKE_VERSION "${CMAKE_VERSION}")
set(SMATCHET_DEPS_LITERALS "")

if(DEFINED SMATCHET_STATIC_VARS AND EXISTS "${SMATCHET_STATIC_VARS}")
    include("${SMATCHET_STATIC_VARS}")
endif()

if(NOT DEFINED SMATCHET_TEMPLATE OR NOT EXISTS "${SMATCHET_TEMPLATE}")
    message(WARNING "SmatchetBuildInfo: template missing (${SMATCHET_TEMPLATE}) — header not generated")
    return()
endif()
if(NOT DEFINED SMATCHET_OUT OR SMATCHET_OUT STREQUAL "")
    message(WARNING "SmatchetBuildInfo: SMATCHET_OUT not set — header not generated")
    return()
endif()

# ------------------------------------------------------------ git probing ---
# Thin wrapper: never fails the build, yields "" on any non-zero exit.
function(_sbi_git_out OUT_VAR)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${SMATCHET_SRC_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _rc EQUAL 0)
        set(_out "")
    endif()
    set(${OUT_VAR} "${_out}" PARENT_SCOPE)
endfunction()

set(_sbi_have_git FALSE)
if(DEFINED GIT_EXECUTABLE AND GIT_EXECUTABLE AND NOT GIT_EXECUTABLE MATCHES "NOTFOUND")
    # `git rev-parse --git-dir` is the ONLY correct "is this a repo" test here:
    # under `git worktree` (scripts/dev/worktree.ps1, used by every agent
    # session) `.git` is a FILE, so `if(IS_DIRECTORY <src>/.git)` would report
    # "not a repo" in most dev trees.
    _sbi_git_out(_sbi_git_dir rev-parse --git-dir)
    if(NOT _sbi_git_dir STREQUAL "")
        set(_sbi_have_git TRUE)
    endif()
endif()

if(_sbi_have_git)
    # NEVER `git describe` — it fails outright on the shallow clones CI uses.
    _sbi_git_out(_sbi_sha rev-parse HEAD)
    if(NOT _sbi_sha STREQUAL "")           # empty => unborn HEAD (fresh `git init`)
        set(SMATCHET_GIT_SHA "${_sbi_sha}")
        string(SUBSTRING "${_sbi_sha}" 0 12 SMATCHET_GIT_SHA_SHORT)
    endif()

    _sbi_git_out(_sbi_branch rev-parse --abbrev-ref HEAD)
    if(_sbi_branch STREQUAL "HEAD")
        # Detached — the usual CI checkout shape. CI hands us the real ref name.
        if(DEFINED ENV{GITHUB_REF_NAME} AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
            set(SMATCHET_GIT_BRANCH "$ENV{GITHUB_REF_NAME}")
        else()
            set(SMATCHET_GIT_BRANCH "(detached)")
        endif()
    elseif(NOT _sbi_branch STREQUAL "")
        set(SMATCHET_GIT_BRANCH "${_sbi_branch}")
    endif()

    # --untracked-files=no is load-bearing: an in-source build dir (or the
    # untracked strays every dev tree accumulates) would otherwise mark every
    # single build dirty.
    _sbi_git_out(_sbi_status status --porcelain=v1 --untracked-files=no)
    if(NOT _sbi_status STREQUAL "")
        set(SMATCHET_GIT_DIRTY 1)
    endif()

    _sbi_git_out(_sbi_shallow rev-parse --is-shallow-repository)
    if(_sbi_shallow STREQUAL "true")
        set(SMATCHET_GIT_SHALLOW 1)
    endif()
endif()

# --------------------------------------------------------------- emission ---
set(_sbi_tmp "${SMATCHET_OUT}.tmp")
configure_file("${SMATCHET_TEMPLATE}" "${_sbi_tmp}" @ONLY)

# copy_if_different is the whole anti-rebuild-storm mechanism — do not collapse
# this into a plain configure_file(... "${SMATCHET_OUT}").
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_sbi_tmp}" "${SMATCHET_OUT}"
    RESULT_VARIABLE _sbi_copy_rc
)
if(NOT _sbi_copy_rc EQUAL 0)
    message(WARNING "SmatchetBuildInfo: could not update ${SMATCHET_OUT}")
endif()
file(REMOVE "${_sbi_tmp}")
