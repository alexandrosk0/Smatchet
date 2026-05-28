#!/usr/bin/env bash
# test-doc-anchors.sh — verify every `AGENTS.md § <section>` reference resolves.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
# Implementation in Python next to this shim (bash regex too brittle for the
# heading-extraction + reference-matching shape).

set -uo pipefail

command -v python >/dev/null 2>&1 || { echo "python required" >&2; exit 2; }

cd "$(git rev-parse --show-toplevel)"
exec python scripts/dev/test_doc_anchors.py "$@"
