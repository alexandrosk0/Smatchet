# ============================================================================
# SmatchetDepManifest.cmake — third-party credits for the Help > About dialog.
#
# WHY HAND-AUTHORED: every FetchContent_Declare in this tree pins a raw commit
# SHA with the human-readable version living only in a trailing comment
# (`GIT_TAG 329c5a6b...` / `# v1.92.7-docking-39-g329c5a6b3`). There is no
# version variable to read back, and a credits box listing bare 40-char SHAs is
# useless to a user filing a bug. So the versions are written out here and then
# DRIFT-GUARDED against the root CMakeLists.txt by smatchet_check_dep_manifest().
#
# Licenses cross-checked against THIRD_PARTY_LICENSES.md at repo root, which is
# the authoritative notice file. This manifest is intentionally a superset of it
# (it also names the always-linked FetchContent deps that file only lists in
# aggregate); it is a credits list, not a replacement for the notice file.
#
# RECORD FORMAT: "name|version|license|url|probe"
#   - `|` and NOT `;` — CMake list semantics would shred a `;`-joined record.
#   - `probe` is INTERNAL: a substring expected to appear in the root
#     CMakeLists.txt (normally the comment beside the GIT_TAG). `-` = no probe
#     (vendored / drop-in components that have no pin to drift against).
#   - Only the first four fields reach C++.
# ============================================================================

# Append the deps this configuration actually links, so the credits box matches
# the binary the user is running rather than some maximal build.
function(smatchet_collect_dep_manifest OUT_VAR)
    set(_deps "")

    # --- always linked -------------------------------------------------------
    list(APPEND _deps "Dear ImGui|v1.92.7-docking|MIT|https://github.com/ocornut/imgui|v1.92.7-docking")
    list(APPEND _deps "nlohmann/json|v3.11.3|MIT|https://github.com/nlohmann/json|json v3.11.3")
    list(APPEND _deps "SQLiteCpp|3.3.1|MIT|https://github.com/SRombauts/SQLiteCpp|SQLiteCpp 3.3.1")
    list(APPEND _deps "cpp-httplib|v0.49.0|MIT|https://github.com/yhirose/cpp-httplib|cpp-httplib v0.49.0")
    list(APPEND _deps "md4c|0.5.2|MIT|https://github.com/mity/md4c|md4c release-0.5.2")
    list(APPEND _deps "ghc::filesystem|v1.5.14|MIT|https://github.com/gulrak/filesystem|ghc::filesystem v1.5.14")

    # --- vendored in-tree (no pin to drift against) --------------------------
    list(APPEND _deps "IconFontCppHeaders|vendored|zlib|https://github.com/juliettef/IconFontCppHeaders|-")
    list(APPEND _deps "ImGuiColorTextEdit|vendored|MIT|https://github.com/BalazsJako/ImGuiColorTextEdit|-")
    list(APPEND _deps "stb|vendored|MIT / Public Domain|https://github.com/nothings/stb|-")

    # --- shipped runtime asset (drop-in, see assets/fonts/README.md) ---------
    list(APPEND _deps "Font Awesome 6 Free Solid|6.x|SIL OFL 1.1|https://fontawesome.com|-")

    # --- configuration-conditional -------------------------------------------
    if(SMATCHET_BUILD_APP)
        list(APPEND _deps "GLFW|3.3.8|Zlib|https://github.com/glfw/glfw|glfw 3.3.8")
        list(APPEND _deps "cpr|1.9.2|MIT|https://github.com/libcpr/cpr|cpr 1.9.2")
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        list(APPEND _deps "Lua|5.3.6|MIT|https://www.lua.org|lua-5.3.6.tar.gz")
        list(APPEND _deps "sol2|v2.20.6|MIT|https://github.com/ThePhD/sol2|sol2 v2.20.6")
    endif()
    if(SMATCHET_WITH_WHISPER)
        list(APPEND _deps "whisper.cpp|v1.7.4|MIT|https://github.com/ggerganov/whisper.cpp|whisper.cpp v1.7.4")
    endif()

    set(${OUT_VAR} "${_deps}" PARENT_SCOPE)
endfunction()

# Drift guard. Each record's `probe` must still appear in the root CMakeLists.txt;
# if a pin gets bumped without touching this file, the credits box would lie.
#
# Same technique as the false-green guard at tests/CMakeLists.txt, and the same
# escalation rule: WARN locally, FATAL only under CI. A local hard gate here
# would violate the `cmake-local-gate-ci-scope` rule in AGENTS.md — a dev
# mid-pin-bump must never be unable to configure.
function(smatchet_check_dep_manifest DEPS ROOT_LISTS_FILE)
    if(NOT EXISTS "${ROOT_LISTS_FILE}")
        return()
    endif()
    file(READ "${ROOT_LISTS_FILE}" _lists_text)

    set(_stale "")
    foreach(_rec IN LISTS DEPS)
        string(REPLACE "|" ";" _fields "${_rec}")
        list(GET _fields 0 _name)
        list(GET _fields 4 _probe)
        if(_probe STREQUAL "-")
            continue()
        endif()
        string(FIND "${_lists_text}" "${_probe}" _at)
        if(_at EQUAL -1)
            list(APPEND _stale "${_name} (expected \"${_probe}\" in ${ROOT_LISTS_FILE})")
        endif()
    endforeach()

    if(_stale STREQUAL "")
        return()
    endif()
    string(REPLACE ";" "\n  - " _stale_text "${_stale}")
    set(_msg
        "Third-party credits manifest is stale — cmake/SmatchetDepManifest.cmake no longer\n"
        "matches the pins in the root CMakeLists.txt. The Help > About credits box would\n"
        "show wrong versions. Update the manifest record(s):\n  - ${_stale_text}")
    if(DEFINED ENV{CI})
        message(FATAL_ERROR ${_msg})
    else()
        message(WARNING ${_msg})
    endif()
endfunction()

# Render the records as the body of the SMATCHET_DEPS_TEXT macro: one C string
# literal per dep, line-continued, each newline-terminated. One literal per dep
# rather than one giant one — MSVC caps a single string literal at 16380 bytes.
# The template supplies the closing `""`, so EVERY line here ends in ` \`.
function(smatchet_build_dep_literals DEPS OUT_VAR)
    set(_text "")
    foreach(_rec IN LISTS DEPS)
        # A quote or backslash in a field would break out of the C literal.
        # Nothing in the manifest has one today; refuse to emit if that changes.
        if(_rec MATCHES "[\"\\\\]")
            message(WARNING "SmatchetDepManifest: skipping record with quote/backslash: ${_rec}")
            continue()
        endif()
        string(REPLACE "|" ";" _fields "${_rec}")
        list(GET _fields 0 _name)
        list(GET _fields 1 _version)
        list(GET _fields 2 _license)
        list(GET _fields 3 _url)
        # `\\n` here is a literal backslash-n for the C compiler, not a newline.
        string(APPEND _text "    \"${_name}|${_version}|${_license}|${_url}\\n\" \\\n")
    endforeach()
    set(${OUT_VAR} "${_text}" PARENT_SCOPE)
endfunction()
