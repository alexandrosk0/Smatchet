#!/usr/bin/env bash
# test-workflow-job-mask.sh — rule `gate-job-mask-non-advisory`: a CI job whose
# name does NOT contain "advisory" must not carry JOB-level continue-on-error.
# ----------------------------------------------------------------------------
# WHY (tooling backlog 2026-07-05 gate-lane-no-job-level-continue-on-error)
#   The all-gates-blocking flip had three lanes drift between three coupled
#   attributes (name de-advisoried / job mask retained / required-context
#   promoted) — caught by hand in code review. A job-level
#   `continue-on-error: true` green-washes the whole workflow run, the exact
#   anti-pattern the flip removed. Step-level masks (fuzz stochastic, bucket
#   golden diff, bucket-E per-test, cppcheck) stay sanctioned — this rule is
#   JOB-level only.
#
# WHAT IT ASSERTS
#   For every job in .github/workflows/*.yml: if the job's `name:` (falling
#   back to the job id) does not contain "advisory" (case-insensitive), the job
#   must not set a job-level `continue-on-error` other than literal false. An
#   expression value (`${{ … }}`) counts as a mask (cannot be proven false).
#
# MODES
#   (no args) | --check   scan .github/workflows/ under the repo root (or
#                         SMATCHET_JOB_MASK_ROOT); FAIL (1) on any violation.
#   --selftest            fixture dogfood: a masked non-advisory job FAILS; an
#                         advisory-named masked job and a step-level mask PASS.
#
# EXIT  0 clean · 1 violation / selftest failure · 2 infra error (no python/yaml).
# Goes through test-shell-lint.sh + the test-gate-selftests.sh marker check.
# selftest: asserts-failure
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || { echo "test-workflow-job-mask: cannot cd to repo root" >&2; exit 2; }

# Prefer an interpreter that can import yaml (mirrors lib/ci-check-resolve.sh).
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "import yaml" >/dev/null 2>&1; then PY="$_p"; break; fi
done
if [ -z "$PY" ]; then
    echo "test-workflow-job-mask: no python interpreter with PyYAML (pip install pyyaml)" >&2
    exit 2
fi

# scan <workflows-dir> — print one violation line per masked non-advisory job.
# Exit 0 clean · 1 violation(s) · 2 infra (unparseable yaml is a FAILURE, not a
# skip — a workflow this gate cannot read must not silently pass).
scan() {
    "$PY" - "$1" <<'PY'
import glob, os, sys
import yaml

wf_dir = sys.argv[1]
files = sorted(glob.glob(os.path.join(wf_dir, "*.yml")) + glob.glob(os.path.join(wf_dir, "*.yaml")))
if not files:
    print(f"test-workflow-job-mask: no workflow files under {wf_dir}", file=sys.stderr)
    sys.exit(2)
violations = []
for path in files:
    try:
        with open(path, encoding="utf-8") as fh:
            doc = yaml.safe_load(fh)
    except yaml.YAMLError as exc:
        print(f"test-workflow-job-mask: cannot parse {path}: {exc}", file=sys.stderr)
        sys.exit(2)
    jobs = (doc or {}).get("jobs") or {}
    if not isinstance(jobs, dict):
        continue
    for job_id, job in jobs.items():
        if not isinstance(job, dict) or "continue-on-error" not in job:
            continue
        value = job["continue-on-error"]
        if value is False:
            continue  # explicit non-mask
        name = job.get("name", job_id)
        name = name if isinstance(name, str) else str(name)
        if "advisory" in name.lower():
            continue  # sanctioned: advisory-named lane
        violations.append(f"{path} job '{job_id}' (name: '{name}') sets job-level continue-on-error: {value!r}")
if violations:
    print("test-workflow-job-mask: FAIL — gate-job-mask-non-advisory:", file=sys.stderr)
    for v in violations:
        print(f"  - {v}", file=sys.stderr)
    print("  A non-'advisory'-named job must not green-wash its run; mask the flaky STEP or rename the lane '(advisory)'.", file=sys.stderr)
    sys.exit(1)
print(f"test-workflow-job-mask: PASS — no non-advisory job carries a job-level continue-on-error ({len(files)} workflow(s)).")
PY
}

run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    mkdir -p "$tmp/wf"

    # 1. Masked NON-advisory job -> FAIL (the asserted failure case).
    cat > "$tmp/wf/bad.yml" <<'YML'
name: Bad
on: { pull_request: { branches: [develop] } }
jobs:
  gate:
    name: Fuzz smoke (windows-2022)
    runs-on: ubuntu-latest
    continue-on-error: true
    steps:
      - run: echo hi
YML
    if scan "$tmp/wf" >/dev/null 2>&1; then
        echo "test-workflow-job-mask --selftest: FAIL — masked non-advisory job not caught" >&2
        return 1
    fi

    # 2. Advisory-named masked job + step-level mask elsewhere -> PASS.
    cat > "$tmp/wf/bad.yml" <<'YML'
name: Good
on: { pull_request: { branches: [develop] } }
jobs:
  adv:
    name: Mobile — Android NDK (advisory)
    runs-on: ubuntu-latest
    continue-on-error: true
    steps:
      - run: echo hi
  gate:
    name: Fuzz smoke (windows-2022)
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
        continue-on-error: true
YML
    if ! scan "$tmp/wf" >/dev/null 2>&1; then
        echo "test-workflow-job-mask --selftest: FAIL — sanctioned masks were flagged" >&2
        return 1
    fi

    # 3. Expression-valued job-level mask on a non-advisory job -> FAIL.
    cat > "$tmp/wf/bad.yml" <<'YML'
name: Expr
on: { pull_request: { branches: [develop] } }
jobs:
  gate:
    runs-on: ubuntu-latest
    continue-on-error: ${{ github.event_name == 'push' }}
    steps:
      - run: echo hi
YML
    if scan "$tmp/wf" >/dev/null 2>&1; then
        echo "test-workflow-job-mask --selftest: FAIL — expression-valued job mask not caught" >&2
        return 1
    fi

    echo "test-workflow-job-mask --selftest: PASS — catches job-level masks on non-advisory jobs; passes advisory/step-level masks."
    return "$rc"
}

case "${1:-}" in
    --selftest)
        run_selftest
        exit $?
        ;;
    ""|--check)
        scan "${SMATCHET_JOB_MASK_ROOT:-$ROOT}/.github/workflows"
        exit $?
        ;;
    *)
        echo "usage: test-workflow-job-mask.sh [--check|--selftest]" >&2
        exit 2
        ;;
esac
