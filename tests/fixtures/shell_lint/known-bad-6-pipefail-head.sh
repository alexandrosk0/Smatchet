#!/usr/bin/env bash
# Fixture: rule 6 — pipefail var=$(... | head) with head as the terminal
# truncating consumer and NO mitigation. head closes the pipe → upstream
# SIGPIPE (141) → under pipefail the assignment carries 141 and set -e aborts.
set -euo pipefail
x=$(printf '%s\n' a b c | head -5)
echo "$x"
