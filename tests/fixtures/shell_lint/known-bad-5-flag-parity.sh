#!/usr/bin/env bash
# Fixture: rule 5 — --threshold) takes a value (shift 2) but no --threshold=*) twin.
set -euo pipefail
THRESHOLD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --threshold)
            THRESHOLD="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done
echo "$THRESHOLD"
