#!/usr/bin/env bash
# Fixture: all 5 rules clean.
set -euo pipefail
command -v curl >/dev/null 2>&1 || { echo "curl required" >&2; exit 2; }
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(git rev-parse --show-toplevel)/agents/scripts/core/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "python required" >&2; exit 2; }

THRESHOLD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --threshold)
            THRESHOLD="$2"
            shift 2
            ;;
        --threshold=*)
            THRESHOLD="${1#--threshold=}"
            shift
            ;;
        *)
            shift
            ;;
    esac
done

input="${THRESHOLD:-0}"
echo "threshold=$input"

curl -fsSL "https://example.com/x.bin" -o /tmp/x.bin
echo "deadbeef  /tmp/x.bin" | sha256sum -c -

"$PY" -c 'print("hi")'
