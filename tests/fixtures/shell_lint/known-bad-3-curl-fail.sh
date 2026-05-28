#!/usr/bin/env bash
# Fixture: rule 3 — curl invocation without -f / --fail.
set -euo pipefail
command -v curl >/dev/null 2>&1 || exit 2
curl -sSL "https://example.com/healthz" -o /dev/null
