#!/usr/bin/env bash
# Fixture: rule 9 (py-probe) — the flagged shapes. On Windows `python3`
# resolves to the Microsoft Store App Execution Alias stub, so a resolve-only
# probe selects an interpreter that is on PATH but exits non-zero when run.
#
# Shape (a): candidate variable loop, no exec-validation (probe line 14).
# Shape (b): ANY literal python-name probe — the if/elif chain (lines 20/22),
# a single-candidate hard-require guard (line 28), an exec-VALIDATED literal
# chain (line 30), and a SINGLE-QUOTED literal probe (line 34) all flag:
# literal probes must route through lib/resolve-py.sh
# (py-probe-single-candidate-residual).
PY=""
for c in python3 python py; do
    if command -v "$c" >/dev/null 2>&1; then
        PY="$c"
        break
    fi
done

if command -v python3 >/dev/null 2>&1; then
    ALT=python3
elif command -v python >/dev/null 2>&1; then
    ALT=python
else
    ALT=""
fi

command -v python3 >/dev/null 2>&1 || { echo "python3 required" >&2; exit 2; }

if command -v python >/dev/null 2>&1 && python --version >/dev/null 2>&1; then
    VAL=python
fi

command -v 'python3' >/dev/null 2>&1 || { echo "python3 required" >&2; exit 2; }

echo "py=$PY alt=$ALT val=${VAL:-}"
