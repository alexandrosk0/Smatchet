#!/usr/bin/env bash
# Fixture: rule 9 (py-probe) — BOTH picker shapes, neither exec-validated.
#
# Shape (a): candidate-loop picker. Shape (b): literal if/elif chain. On Windows
# `python3` resolves to the Microsoft Store App Execution Alias stub, so either
# shape can select an interpreter that is on PATH but exits non-zero when run.
set -euo pipefail

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

echo "py=$PY alt=$ALT"
