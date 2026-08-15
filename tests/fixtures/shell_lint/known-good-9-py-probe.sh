#!/usr/bin/env bash
# Fixture: rule 9 (py-probe) negatives — shapes that must stay silent.
#
#   1. Exec-validated candidate VARIABLE loop (the pre-lib canonical form: it
#      validates a DIFFERENT variable than it probes, which is why rule 9
#      searches the window generically rather than anchoring to the loop
#      variable).
#   2. The blessed resolver usage — source lib/resolve-py.sh, resolve once,
#      invoke "$PY" downstream (no literal python-name probe anywhere).
#   3. A non-python single-candidate guard (jq) — rule 9 is python-scoped.
set -euo pipefail

PY=""
for c in python3 python py; do
    p="$(command -v "$c" 2>/dev/null)" || continue
    if "$p" -c "" >/dev/null 2>&1; then
        PY="$p"
        break
    fi
done

# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(git rev-parse --show-toplevel)/agents/scripts/core/lib/resolve-py.sh"
PY2="$(resolve_py)" || { echo "python required" >&2; exit 2; }
"$PY2" -c "" >/dev/null

command -v jq >/dev/null 2>&1 || { echo "jq required" >&2; exit 2; }

echo "py=$PY py2=$PY2"
