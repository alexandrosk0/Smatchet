#!/usr/bin/env bash
# Fixture: rule 8 — a producer piped into an early-break reader FUNCTION under
# pipefail. classify_first reads with `while ... read` and `break`s on the first
# match; the early break closes the pipe → the `git diff` producer takes SIGPIPE
# (141) → under pipefail + set -e the script aborts. This is the #1593 class.
set -euo pipefail

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

result=$(git diff --name-only | classify_first)
echo "$result"
