#!/usr/bin/env bash
# test-workflow-yaml.sh — smoke-test every `.github/workflows/*.yml` for
# YAML parse validity. Closes the regression class that hit PR #323 + #324
# where heredoc bodies at column 0 inside a `run: |` block scalar broke the
# whole workflow — visible as `gh run list --workflow=<name>.yml` showing
# completed/failure with zero jobs and no logs. (CodeRabbit PR #323 self-
# improvement note.)
#
# Auto-enrolled into scripts/dev/test-all.sh by the existing `test-*.sh`
# pattern. Exit 1 on any parse failure; exit 0 when all workflows parse.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || { echo "ERROR: cd to $REPO_ROOT failed" >&2; exit 1; }

PY="${PYTHON:-python}"

# Prefer PyYAML when available; fall back to actionlint (which understands
# the GHA schema and indirectly catches the same heredoc class). If neither
# is on PATH, soft-pass with a warning so the developer's first run doesn't
# fail just because they haven't pip-installed yaml — CI workflows already
# have python-yaml installed via the msys2 toolchain step.
PASSED=0
FAILED=0
mapfile -t YAMLS < <(find .github/workflows -maxdepth 1 -name '*.yml' 2>/dev/null | sort)
if [ "${#YAMLS[@]}" -eq 0 ]; then
    echo "[test-workflow-yaml] no .github/workflows/*.yml files — skipping."
    echo "Passed: 0  Failed: 0"
    exit 0
fi

PYYAML_OK=0
$PY -c "import yaml" 2>/dev/null && PYYAML_OK=1
ACTIONLINT_OK=0
command -v actionlint >/dev/null 2>&1 && ACTIONLINT_OK=1

if [ "$PYYAML_OK" -eq 0 ] && [ "$ACTIONLINT_OK" -eq 0 ]; then
    echo "[test-workflow-yaml] WARN: neither PyYAML nor actionlint installed; cannot validate."
    echo "  pip install pyyaml   # or: pacman -S mingw-w64-ucrt-x86_64-python-yaml"
    echo "Passed: 0  Failed: 0"
    exit 0
fi

for f in "${YAMLS[@]}"; do
    if [ "$PYYAML_OK" -eq 1 ]; then
        if $PY -c "import sys, yaml; yaml.safe_load(open('$f'))" 2>/tmp/workflow_yaml.err; then
            echo "[test-workflow-yaml] OK   $f"
            PASSED=$((PASSED + 1))
        else
            echo "[test-workflow-yaml] FAIL $f"
            cat /tmp/workflow_yaml.err >&2
            FAILED=$((FAILED + 1))
        fi
    else
        if actionlint -no-color "$f" > /tmp/workflow_yaml.err 2>&1; then
            echo "[test-workflow-yaml] OK   $f (via actionlint)"
            PASSED=$((PASSED + 1))
        else
            echo "[test-workflow-yaml] FAIL $f (via actionlint)"
            cat /tmp/workflow_yaml.err >&2
            FAILED=$((FAILED + 1))
        fi
    fi
done

echo
echo "Passed: $PASSED  Failed: $FAILED"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
