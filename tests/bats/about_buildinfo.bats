#!/usr/bin/env bats
#
# about_buildinfo.bats — the CMake half of the Help > About dialog.
#
# Covers the two pieces `tests/Core/AboutInfo.test.cpp` structurally cannot:
# the codegen script (cmake/SmatchetGenerateBuildInfo.cmake) and the credits
# manifest (cmake/SmatchetDepManifest.cmake). The doctest asserts on the parsed
# result; nothing there notices a broken add_custom_target, a git probe that
# stopped resolving, or a dependency pin bumped without the manifest.
#
# Deliberately headless: it drives `cmake -P` straight, never the app. The
# unified CLI dispatches over MCP to a *running* instance, and CI cannot boot
# the exe (root AGENTS.md, Bucket- checks dropped 2026-06-15), so a
# `Smatchet.exe cmd app.version` assertion would be unrunnable exactly where it
# needs to run. See docs/plans/active/about-dialog-help-menu.md § Deviations.
#
# Requires: cmake, git.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT

    if ! command -v cmake >/dev/null 2>&1; then
        skip "cmake not available"
    fi

    SCRIPT="${REPO_ROOT}/cmake/SmatchetGenerateBuildInfo.cmake"
    TEMPLATE="${REPO_ROOT}/cmake/SmatchetBuildInfo.h.in"
    MANIFEST="${REPO_ROOT}/cmake/SmatchetDepManifest.cmake"
    OUT="${BATS_TEST_TMPDIR}/SmatchetBuildInfo.h"
}

# MSYS/Cygwin bash hands out POSIX paths that a native cmake.exe cannot open.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

# Run the generator. $1 = GIT_EXECUTABLE value, $2 = source dir (git workdir).
gen() {
    local git_exe="$1"
    local src_dir="$2"
    cmake \
        "-DSMATCHET_TEMPLATE=$(native_path "${TEMPLATE}")" \
        "-DSMATCHET_OUT=$(native_path "${OUT}")" \
        "-DSMATCHET_SRC_DIR=$(native_path "${src_dir}")" \
        "-DGIT_EXECUTABLE=${git_exe}" \
        -P "$(native_path "${SCRIPT}")"
}

real_git() {
    native_path "$(command -v git)"
}

define_value() {
    # `#define NAME "value"` -> value
    sed -n "s/^#define $1  *\"\\(.*\\)\"\$/\\1/p" "${OUT}"
}

# --------------------------------------------------------------- codegen ---

@test "generator stamps the real HEAD sha" {
    run gen "$(real_git)" "${REPO_ROOT}"
    [ "$status" -eq 0 ]
    [ -f "${OUT}" ]

    local head
    head="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
    [ "$(define_value SMATCHET_GIT_SHA)" = "${head}" ]
    [ "$(define_value SMATCHET_GIT_SHA_SHORT)" = "${head:0:12}" ]
    [ -n "$(define_value SMATCHET_GIT_BRANCH)" ]

    # Booleans are emitted unquoted — they must be literal 0/1, never a string,
    # or AboutInfo.cpp's `SMATCHET_GIT_DIRTY != 0` stops compiling.
    grep -Eq '^#define SMATCHET_GIT_DIRTY +[01]$' "${OUT}"
    grep -Eq '^#define SMATCHET_GIT_SHALLOW +[01]$' "${OUT}"
    grep -q '^#define SMATCHET_BUILDINFO_AVAILABLE 1$' "${OUT}"
}

@test "generator degrades to unknown when git is absent, and never FATALs" {
    # Plan § Verification item 2. A FATAL here would brick the tree for anyone
    # without git and would violate the cmake-local-gate-ci-scope rule.
    run gen "GIT_EXECUTABLE-NOTFOUND" "${REPO_ROOT}"
    [ "$status" -eq 0 ]
    [[ "$output" != *"FATAL"* ]]

    [ "$(define_value SMATCHET_GIT_SHA)" = "unknown" ]
    [ "$(define_value SMATCHET_GIT_SHA_SHORT)" = "unknown" ]
    [ "$(define_value SMATCHET_GIT_BRANCH)" = "unknown" ]
    grep -q '^#define SMATCHET_GIT_DIRTY  *0$' "${OUT}"
    grep -q '^#define SMATCHET_GIT_SHALLOW  *0$' "${OUT}"
}

@test "generator degrades to unknown outside a git repository" {
    local outside="${BATS_TEST_TMPDIR}/not-a-repo"
    mkdir -p "${outside}"

    run gen "$(real_git)" "${outside}"
    [ "$status" -eq 0 ]
    [ "$(define_value SMATCHET_GIT_SHA)" = "unknown" ]
}

@test "generator warns but does not fail on a missing template" {
    run cmake \
        "-DSMATCHET_TEMPLATE=$(native_path "${BATS_TEST_TMPDIR}/nope.h.in")" \
        "-DSMATCHET_OUT=$(native_path "${OUT}")" \
        "-DSMATCHET_SRC_DIR=$(native_path "${REPO_ROOT}")" \
        "-DGIT_EXECUTABLE=$(real_git)" \
        -P "$(native_path "${SCRIPT}")"
    [ "$status" -eq 0 ]
    [ ! -f "${OUT}" ]
}

@test "copy_if_different leaves an unchanged header's mtime alone" {
    # The whole anti-rebuild-storm mechanism: SmatchetBuildInfoGen runs on every
    # build, so a plain configure_file would recompile AboutInfo.cpp every time.
    gen "$(real_git)" "${REPO_ROOT}"
    touch -t 202001010000 "${OUT}"
    local before
    before="$(ls -l --time-style=+%Y%m%d%H%M "${OUT}" | awk '{print $6}')"

    gen "$(real_git)" "${REPO_ROOT}"
    local after
    after="$(ls -l --time-style=+%Y%m%d%H%M "${OUT}" | awk '{print $6}')"
    [ "$before" = "$after" ]

    # ...but a genuinely different header IS rewritten.
    echo "// clobbered" > "${OUT}"
    gen "$(real_git)" "${REPO_ROOT}"
    grep -q '^#define SMATCHET_BUILDINFO_AVAILABLE 1$' "${OUT}"

    # And the temp file is not left behind next to the real header.
    [ ! -f "${OUT}.tmp" ]
}

# -------------------------------------------------------------- manifest ---

# Collect the manifest under the given feature flags into a newline-per-record
# file. `$1` = output file, remaining args = extra -D flags.
collect_manifest() {
    local out="$1"; shift
    local driver="${BATS_TEST_TMPDIR}/collect.cmake"
    cat > "${driver}" <<'EOF'
cmake_minimum_required(VERSION 3.24)
include("${MANIFEST_MODULE}")
smatchet_collect_dep_manifest(_deps)
file(WRITE "${RECORDS_OUT}" "")
foreach(_rec IN LISTS _deps)
    file(APPEND "${RECORDS_OUT}" "${_rec}\n")
endforeach()
EOF
    cmake \
        "-DMANIFEST_MODULE=$(native_path "${MANIFEST}")" \
        "-DRECORDS_OUT=$(native_path "${out}")" \
        "$@" \
        -P "$(native_path "${driver}")"
}

@test "dep manifest records are well formed" {
    local recs="${BATS_TEST_TMPDIR}/records.txt"
    collect_manifest "${recs}"
    [ -s "${recs}" ]

    # Plan asserts >= 9 records reach the credits box; the unconditional set
    # alone is 10, so this holds even with every feature flag off.
    [ "$(wc -l < "${recs}")" -ge 9 ]

    while IFS= read -r rec; do
        # name|version|license|url|probe — 5 fields, and only the first four
        # reach C++. An empty name or version would render a blank credits row.
        [ "$(awk -F'|' '{print NF}' <<< "${rec}")" -eq 5 ]
        [ -n "$(cut -d'|' -f1 <<< "${rec}")" ]
        [ -n "$(cut -d'|' -f2 <<< "${rec}")" ]
        [ -n "$(cut -d'|' -f3 <<< "${rec}")" ]
        # A quote or backslash would break out of the generated C string literal;
        # smatchet_build_dep_literals drops such a record, silently shrinking the
        # credits box.
        [[ "${rec}" != *'"'* ]]
        [[ "${rec}" != *'\'* ]]
    done < "${recs}"
}

@test "dep manifest gates conditional deps on their feature flags" {
    # The credits box must match the binary the user is actually running.
    local off="${BATS_TEST_TMPDIR}/off.txt"
    collect_manifest "${off}"
    ! grep -q '^whisper.cpp|' "${off}"
    ! grep -q '^Lua|' "${off}"
    ! grep -q '^GLFW|' "${off}"

    local on="${BATS_TEST_TMPDIR}/on.txt"
    collect_manifest "${on}" \
        -DSMATCHET_BUILD_APP=ON -DSMATCHET_WITH_LUA_AUTOMATION=ON -DSMATCHET_WITH_WHISPER=ON
    grep -q '^whisper.cpp|' "${on}"
    grep -q '^Lua|' "${on}"
    grep -q '^GLFW|' "${on}"
    grep -q '^cpr|' "${on}"
    grep -q '^sol2|' "${on}"
}

# Run the drift guard against $2 with every feature flag on, so no pin escapes
# the check. $1 = value for the CI env var ("" = unset).
check_manifest() {
    local ci="$1"
    local root_lists="$2"
    local driver="${BATS_TEST_TMPDIR}/check.cmake"
    cat > "${driver}" <<'EOF'
cmake_minimum_required(VERSION 3.24)
include("${MANIFEST_MODULE}")
smatchet_collect_dep_manifest(_deps)
smatchet_check_dep_manifest("${_deps}" "${ROOT_LISTS}")
EOF
    if [ -n "${ci}" ]; then
        export CI="${ci}"
    else
        unset CI
    fi
    cmake \
        "-DMANIFEST_MODULE=$(native_path "${MANIFEST}")" \
        "-DROOT_LISTS=$(native_path "${root_lists}")" \
        -DSMATCHET_BUILD_APP=ON -DSMATCHET_WITH_LUA_AUTOMATION=ON -DSMATCHET_WITH_WHISPER=ON \
        -P "$(native_path "${driver}")"
}

@test "every manifest version still matches its pin in the root CMakeLists" {
    # The regression this file exists for: bump a GIT_TAG without touching
    # SmatchetDepManifest.cmake and the About credits box starts lying.
    run check_manifest "1" "${REPO_ROOT}/CMakeLists.txt"
    [ "$status" -eq 0 ]
    [[ "$output" != *"stale"* ]]
}

@test "drift guard FATALs under CI but only warns locally" {
    local fake="${BATS_TEST_TMPDIR}/fake-CMakeLists.txt"
    echo "project(Nothing)" > "${fake}"

    run check_manifest "" "${fake}"
    [ "$status" -eq 0 ]
    [[ "$output" == *"stale"* ]]

    run check_manifest "1" "${fake}"
    [ "$status" -ne 0 ]
    [[ "$output" == *"stale"* ]]
}
