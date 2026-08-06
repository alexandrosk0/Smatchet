#!/usr/bin/env bash
# Fixture: rule 9 (py-probe) negatives — three shapes that must stay silent.
#
#   1. Exec-validated candidate loop (the canonical repo form: it validates a
#      DIFFERENT variable than it probes, which is why rule 9 searches the
#      window generically rather than anchoring to the loop variable).
#   2. Exec-validated literal chain.
#   3. Single-candidate hard-require guard — one candidate, so nothing can be
#      mis-selected; it fails loudly rather than silently picking a stub.
set -euo pipefail

PY=""
for c in python3 python py; do
    p="$(command -v "$c" 2>/dev/null)" || continue
    if "$p" -c "" >/dev/null 2>&1; then
        PY="$p"
        break
    fi
done

if command -v python3 >/dev/null 2>&1 && python3 --version >/dev/null 2>&1; then
    ALT=python3
elif command -v python >/dev/null 2>&1 && python --version >/dev/null 2>&1; then
    ALT=python
else
    ALT=""
fi

command -v jq >/dev/null 2>&1 || { echo "jq required" >&2; exit 2; }

echo "py=$PY alt=$ALT"
