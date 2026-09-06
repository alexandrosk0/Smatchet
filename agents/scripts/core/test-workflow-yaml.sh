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

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
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
        # encoding="utf-8" is explicit: workflow files carry non-ASCII (em
        # dashes in echo strings), and Python's default text encoding follows
        # the locale, so on a cp1252 Windows shell every such file fails to
        # decode and the gate reports a false FAIL on a valid workflow.
        if $PY -c "import sys, yaml; yaml.safe_load(open('$f', encoding='utf-8'))" 2>/tmp/workflow_yaml.err; then
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

# --- action SHA-pin assertion -------------------------------------------------
# Every THIRD-PARTY `uses:` must name a 40-hex commit, never a mutable tag: a
# retagged `v*` ref executes attacker-controlled code inside the job. Dependabot
# pins on bump, but nothing ENFORCED it, so a hand-written workflow could
# reintroduce a tag silently — `plan-lock-gate.yml` did exactly that and sat as
# the repo's only unpinned ref, inside a hard-net gate job, until a historical
# review found it (finding #1902; nothing else keys on pinning).
#
# SCOPE — real `uses:` STEP keys only, anchored at line start. A naive
# `grep -r 'uses:'` is wrong twice over, and both false-positive classes are
# live in this repo:
#   * `statuses: write` / `statuses: read` CONTAIN the substring `uses:`
#     (stat + uses:) — 4 hits in .github/;
#   * prose in comments ("...the lane uses: host clang/lld") — 4 more.
# A first cut of this check produced 8 false hits over 1 real one. The anchor
# (`^ spaces, optional '- ', spaces, uses:`) rejects both by construction.
#
# Local composite actions (`./.github/actions/...`) are exempt: they are repo
# content, already covered by the pinned checkout that fetched them.
PIN_BAD=0
while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    echo "[test-workflow-yaml] FAIL unpinned action  $hit"
    PIN_BAD=$((PIN_BAD + 1))
done < <(
    grep -rnE '^[[:space:]]*(-[[:space:]]+)?uses:[[:space:]]*[^[:space:]]+' \
         .github/workflows .github/actions 2>/dev/null \
      | grep -vE 'uses:[[:space:]]*\./' \
      | grep -vE 'uses:[[:space:]]*[^[:space:]]+@[0-9a-f]{40}([[:space:]]|$)'
)

if [ "$PIN_BAD" -gt 0 ]; then
    echo "[test-workflow-yaml] $PIN_BAD unpinned third-party action(s)." >&2
    echo "  Pin to the full 40-hex commit and keep the tag as a trailing comment:" >&2
    echo "    uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1" >&2
    echo "  Resolve a tag with: gh api repos/<owner>/<repo>/commits/<tag> --jq .sha" >&2
    FAILED=$((FAILED + PIN_BAD))
else
    echo "[test-workflow-yaml] OK   every third-party action is SHA-pinned"
    PASSED=$((PASSED + 1))
fi

echo
echo "Passed: $PASSED  Failed: $FAILED"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
