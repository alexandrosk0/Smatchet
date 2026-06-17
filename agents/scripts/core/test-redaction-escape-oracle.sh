#!/usr/bin/env bash
# test-redaction-escape-oracle.sh — wrapper for redaction-escape-oracle.py.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Runs the adversarial redaction-escape oracle (agents/scripts/core/
# redaction-escape-oracle.py) in --selftest mode: proves the oracle can SEE a
# surviving secret, then asserts the real redact-intent.py redacts the whole
# planted-secret corpus. Emits the canonical `Passed: N  Failed: M` line that
# test-all.sh greps for.
#
# Exit codes follow the test-author convention:
#   0 — oracle selftest passed
#   1 — a planted secret survived redaction (a real leak / regression)
#   2 — python interpreter missing (cannot run the oracle)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

# Resolve a python interpreter by PROBE-EXECUTING each candidate — a bare
# `command -v python3` on Windows finds the Microsoft Store alias stub that
# errors on exec, so verify the interpreter actually runs (mirrors the
# resolution in tests/bats/capture_intent.bats). The probe also REQUIRES Python
# 3: the oracle is a `#!/usr/bin/env python3` module, and a bare `python` may be
# a lingering Python 2 where `import sys` still succeeds — letting py2 through
# would run the oracle under the wrong interpreter and mis-report.
PY=""
for cand in python3 python py; do
    if command -v "$cand" >/dev/null 2>&1 \
        && "$cand" -c "import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)" >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done

if [ -z "$PY" ]; then
    echo "test-redaction-escape-oracle: no python interpreter on PATH" >&2
    echo "Passed: 0  Failed: 0  (skipped — python missing)"
    exit 2
fi

ORACLE="agents/scripts/core/redaction-escape-oracle.py"
if [ ! -f "$ORACLE" ]; then
    echo "test-redaction-escape-oracle: $ORACLE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$("$PY" "$ORACLE" --selftest 2>&1)"
RC=$?

echo "$OUT"

# The oracle returns 2 only when it cannot load the redactor module — an
# environment problem, surfaced as a skip rather than a redaction failure.
if [ "$RC" -eq 2 ]; then
    echo "Passed: 0  Failed: 0  (skipped — redactor module unloadable)"
    exit 2
fi

if [ "$RC" -eq 0 ]; then
    echo "Passed: 1  Failed: 0"
    exit 0
fi

echo "Passed: 0  Failed: 1"
exit 1
