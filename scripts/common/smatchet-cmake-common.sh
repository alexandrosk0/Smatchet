#!/usr/bin/env bash
# scripts/common/smatchet-cmake-common.sh — shared CMake preset + version helpers.
#
# Bash port of the retired scripts/common/SmatchetCMakeCommon.ps1. Source it from
# any dev/publish wrapper:
#
#   . "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"
#
# JSON parsing goes through python (already a hard prerequisite — see
# scripts/dev/check-required-tools.sh); there is no jq dependency here because
# jq is documented as test-harness-only.
#
# Functions (all take explicit args; none read globals):
#   smatchet_python                              -> prints a working python bin
#   smatchet_configure_preset_binary_dir R P     -> binaryDir for configure preset P
#   smatchet_resolve_build_location R P          -> BUILD_DIR=/CONFIGURATION= lines
#   smatchet_project_version R                   -> x.y.z from CMakeLists.txt
#   smatchet_version_parts V                     -> "major minor patch"
#   smatchet_version_tag V                       -> vX.Y.Z
#   smatchet_version_unreal_number V             -> major*10000 + minor*100 + patch
#   smatchet_plugin_manifest_path R              -> path to the .uplugin
#   smatchet_set_plugin_manifest_version P V     -> rewrite Version + VersionName
#   smatchet_plugin_manifest_matches_version P V -> rc 0 when in sync

# Resolve a python interpreter that actually runs. `command -v python3` is not
# enough on Windows: python3 is the Microsoft Store alias stub — on PATH, but
# exits non-zero when executed.
smatchet_python() {
    local cand
    for cand in python python3; do
        if command -v "$cand" >/dev/null 2>&1 && "$cand" -c "" >/dev/null 2>&1; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    echo "smatchet-cmake-common: no working python interpreter on PATH (python or python3)." >&2
    return 1
}

# binaryDir of a configure preset, with ${sourceDir} expanded to <root>.
# Falls back to <root>/build/<preset> when the preset is absent — same contract
# as the PowerShell original.
smatchet_configure_preset_binary_dir() {
    local root="$1" preset="$2" py
    py="$(smatchet_python)" || return 1
    "$py" - "$root" "$preset" <<'PY'
import json, os, sys
root, preset = sys.argv[1], sys.argv[2]
for name in ("CMakePresets.json", "CMakeUserPresets.json"):
    path = os.path.join(root, name)
    if not os.path.isfile(path):
        continue
    with open(path, "r", encoding="utf-8-sig") as fh:
        doc = json.load(fh)
    for p in doc.get("configurePresets") or []:
        # CMake preset names are case-sensitive per the JSON schema.
        if p.get("name") == preset and p.get("binaryDir"):
            print(p["binaryDir"].replace("${sourceDir}", root))
            sys.exit(0)
print(os.path.join(root, "build", preset))
PY
}

# Resolve a build/configure preset name to the directory holding the executable.
# Prints `BUILD_DIR=<abs>` and `CONFIGURATION=<name-or-empty>`; exits non-zero
# with a message on stderr when the preset needs judgement the caller must make
# explicitly (unknown preset, inheritance cycle, `condition` block, or a CMake
# macro other than ${sourceDir}).
smatchet_resolve_build_location() {
    local root="$1" preset="$2" py
    py="$(smatchet_python)" || return 1
    "$py" - "$root" "$preset" <<'PY'
import json, os, re, sys

root, preset = sys.argv[1], sys.argv[2]

docs = []
for name in ("CMakePresets.json", "CMakeUserPresets.json"):
    path = os.path.join(root, name)
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8-sig") as fh:
            docs.append(json.load(fh))


def find(collection, name):
    for doc in docs:
        for item in doc.get(collection) or []:
            if item.get("name") == name:
                return item
    return None


def die(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(1)


def resolve_configure(name, stack):
    if name in stack:
        die("CMake configure preset inheritance cycle detected: "
            + " -> ".join(list(stack) + [name]))
    item = find("configurePresets", name)
    if item is None:
        die("CMake configure preset not found: " + name)
    merged = {}
    inherits = item.get("inherits") or []
    if isinstance(inherits, str):
        inherits = [inherits]
    for parent in inherits:
        merged.update(resolve_configure(parent, stack + [name]))
    merged.update(item)
    return merged


configure_name = preset
configuration = ""
build_preset = find("buildPresets", preset)
if build_preset is not None:
    configure_name = build_preset.get("configurePreset") or preset
    configuration = build_preset.get("configuration") or ""

configure = resolve_configure(configure_name, [])

# `condition` gates whether CMake enables the preset on this host. We do not
# evaluate it; force the caller to be explicit so we never run a build dir CMake
# would have skipped.
if "condition" in configure:
    die("Configure preset '%s' has a 'condition' block this script does not "
        "evaluate. Pass --build-dir explicitly." % configure_name)

binary_dir = configure.get("binaryDir")
if not binary_dir:
    die("CMake configure preset '%s' does not define binaryDir." % configure_name)

resolved = binary_dir.replace("${sourceDir}", root)
# Only ${sourceDir} is supported; any other macro would silently yield a wrong
# path, so reject it rather than guess.
if re.search(r"\$(\{[^}]+\}|env\{[^}]+\}|penv\{[^}]+\})", resolved):
    die("binaryDir '%s' on preset '%s' contains a CMake preset macro this script "
        "does not resolve (only ${sourceDir} is supported). Pass --build-dir "
        "explicitly to bypass." % (binary_dir, configure_name))

print("BUILD_DIR=" + resolved)
print("CONFIGURATION=" + configuration)
PY
}

# project(SmatchetApp VERSION x.y.z) out of the root CMakeLists.txt.
smatchet_project_version() {
    local root="$1" cmake_path version
    cmake_path="$root/CMakeLists.txt"
    if [ ! -f "$cmake_path" ]; then
        echo "smatchet-cmake-common: CMakeLists.txt not found: $cmake_path" >&2
        return 1
    fi
    version="$(grep -oE 'project[[:space:]]*\([[:space:]]*SmatchetApp[[:space:]]+VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$cmake_path" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
    if [ -z "$version" ]; then
        echo "smatchet-cmake-common: unable to locate project(SmatchetApp VERSION x.y.z) in $cmake_path" >&2
        return 1
    fi
    printf '%s\n' "$version"
}

# "major minor patch" — also the strict semver validator the other version
# helpers lean on.
smatchet_version_parts() {
    local version="$1"
    if [[ ! "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
        echo "smatchet-cmake-common: version must be semantic major.minor.patch: $version" >&2
        return 1
    fi
    printf '%s %s %s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"
}

smatchet_version_tag() {
    smatchet_version_parts "$1" >/dev/null || return 1
    printf 'v%s\n' "$1"
}

smatchet_version_unreal_number() {
    local parts
    parts="$(smatchet_version_parts "$1")" || return 1
    # shellcheck disable=SC2086  # deliberate word-split of the "maj min patch" triple
    set -- $parts
    printf '%s\n' "$(( $1 * 10000 + $2 * 100 + $3 ))"
}

smatchet_plugin_manifest_path() {
    printf '%s\n' "$1/Source/UnrealPlugins/SmatchetImGuiPlugin/SmatchetImGuiPlugin.uplugin"
}

# Rewrite Version (int) + VersionName (string) in the .uplugin. UTF-8 without a
# BOM plus a trailing newline — UnrealBuildTool rejects a BOM'd manifest.
smatchet_set_plugin_manifest_version() {
    local path="$1" version="$2" number py
    number="$(smatchet_version_unreal_number "$version")" || return 1
    py="$(smatchet_python)" || return 1
    if [ ! -f "$path" ]; then
        echo "smatchet-cmake-common: plugin manifest not found: $path" >&2
        return 1
    fi
    "$py" - "$path" "$version" "$number" <<'PY'
import json, sys
path, version, number = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(path, "r", encoding="utf-8-sig") as fh:
    manifest = json.load(fh)
manifest["Version"] = number
manifest["VersionName"] = version
with open(path, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(manifest, fh, indent=4)
    fh.write("\n")
PY
}

# rc 0 when the manifest's Version + VersionName both match; rc 1 otherwise.
smatchet_plugin_manifest_matches_version() {
    local path="$1" version="$2" number py
    number="$(smatchet_version_unreal_number "$version")" || return 1
    py="$(smatchet_python)" || return 1
    if [ ! -f "$path" ]; then
        echo "smatchet-cmake-common: plugin manifest not found: $path" >&2
        return 1
    fi
    "$py" - "$path" "$version" "$number" <<'PY'
import json, sys
path, version, number = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(path, "r", encoding="utf-8-sig") as fh:
    manifest = json.load(fh)
ok = str(manifest.get("VersionName")) == version and int(manifest.get("Version", -1)) == number
sys.exit(0 if ok else 1)
PY
}
