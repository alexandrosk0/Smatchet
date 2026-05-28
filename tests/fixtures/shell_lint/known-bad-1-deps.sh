#!/usr/bin/env bash
# Fixture: rule 1 deps preflight missing for python.
set -euo pipefail
python -c 'print("hi")'
