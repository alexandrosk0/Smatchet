#!/usr/bin/env bash
# scripts/publish/sync-release-version.sh — point the CMake project version and
# the Unreal plugin manifest at the same release version.
#
# Bash port of the retired sync_release_version.ps1.
#
# With no --version, re-stamps the manifest from whatever CMakeLists.txt already
# declares (the "manifest drifted" repair release-github.sh points at). With
# --version X.Y.Z, moves both files to the new version.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=scripts/common/smatchet-cmake-common.sh
. "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"

VERSION=""

usage() {
    cat <<'EOF'
Usage: bash scripts/publish/sync-release-version.sh [--version X.Y.Z]

  --version <X.Y.Z>  Target version (default: the current CMake project version)
  -h, --help         Show this help
EOF
}

# `shift 2` fails WITHOUT shifting when the option value is missing; under
# `set -e` that aborts with no explanation. Reject it up front with a message.
need_value() {
    [ "$1" -ge 2 ] || { echo "$(basename "$0"): $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version)   need_value $# "$1"; VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)
            echo "sync-release-version: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "sync-release-version: $*" >&2; exit 1; }

PYTHON="$(smatchet_python)" || die "python is required."

if [ -z "$VERSION" ]; then
    VERSION="$(smatchet_project_version "$REPO_ROOT")"
fi
# Rejects a malformed --version before either file is touched.
smatchet_version_parts "$VERSION" >/dev/null

CMAKE_PATH="$REPO_ROOT/CMakeLists.txt"
UPLUGIN_PATH="$(smatchet_plugin_manifest_path "$REPO_ROOT")"

# First match only, UTF-8 without a BOM, existing newlines preserved.
"$PYTHON" - "$CMAKE_PATH" "$VERSION" <<'PY'
import re, sys
path, version = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8", newline="") as fh:
    text = fh.read()
updated, count = re.subn(
    r"project\s*\(\s*SmatchetApp\s+VERSION\s+[0-9]+\.[0-9]+\.[0-9]+",
    "project(SmatchetApp VERSION " + version,
    text,
    count=1,
)
if count == 0 or updated == text:
    sys.exit("Failed to update SmatchetApp VERSION in " + path)
with open(path, "w", encoding="utf-8", newline="") as fh:
    fh.write(updated)
PY

smatchet_set_plugin_manifest_version "$UPLUGIN_PATH" "$VERSION"

echo "Synchronized release version to $VERSION"
echo "  CMake:   $CMAKE_PATH"
echo "  UPlugin: $UPLUGIN_PATH"
