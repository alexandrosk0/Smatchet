#!/usr/bin/env bash
# Fixture: rule 8 NEGATIVE — two safe shapes rule 8 must stay silent on:
#  1. a full-drain reader (while read with NO early break) fed by a pipe: it
#     consumes the whole stream, so the producer never gets SIGPIPE.
#  2. an early-break reader fed from a temp file (the recommended fix): no pipe,
#     so no SIGPIPE regardless of the break.
set -euo pipefail

drain_all() {
    local n=0
    while IFS= read -r _line; do
        n=$((n + 1))
    done
    printf '%s\n' "$n"
}

classify_first() {
    local verdict="none"
    while IFS= read -r line; do
        if [ -n "$line" ]; then
            verdict="found"
            break
        fi
    done
    printf '%s\n' "$verdict"
}

count=$(git diff --name-only | drain_all)

tmp=$(mktemp)
git diff --name-only >"$tmp"
result=$(classify_first <"$tmp")
rm -f "$tmp"
echo "$count $result"
