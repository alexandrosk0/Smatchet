#!/usr/bin/env bats
# tests/bats/workflow_job_mask.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/test-workflow-job-mask.sh — rule
# gate-job-mask-non-advisory (a non-"advisory"-named job must not carry a
# job-level continue-on-error). Uses SMATCHET_JOB_MASK_ROOT to point the scan
# at a throwaway fixture tree (no network).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/agents/scripts/core/test-workflow-job-mask.sh"
    FIX="$(mktemp -d)"
    mkdir -p "$FIX/.github/workflows"
}

teardown() {
    rm -rf "$FIX"
}

@test "--selftest passes (asserts the failure case)" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"test-workflow-job-mask --selftest: PASS"* ]]
}

@test "job-level continue-on-error on a non-advisory job -> FAIL with job name" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
name: X
on: { pull_request: { branches: [develop] } }
jobs:
  gate:
    name: Perf PR-fast (windows-2022)
    runs-on: ubuntu-latest
    continue-on-error: true
    steps:
      - run: echo hi
YML
    run env SMATCHET_JOB_MASK_ROOT="$FIX" bash "$GATE" --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"Perf PR-fast (windows-2022)"* ]]
}

@test "advisory-named job-level mask and step-level mask -> PASS" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
name: X
on: { pull_request: { branches: [develop] } }
jobs:
  adv:
    name: Mobile lane (advisory)
    runs-on: ubuntu-latest
    continue-on-error: true
    steps:
      - run: echo hi
  gate:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
        continue-on-error: true
YML
    run env SMATCHET_JOB_MASK_ROOT="$FIX" bash "$GATE" --check
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "job id (no name:) lacking advisory with a job-level mask -> FAIL" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
name: X
on: { pull_request: { branches: [develop] } }
jobs:
  build:
    runs-on: ubuntu-latest
    continue-on-error: true
    steps:
      - run: echo hi
YML
    run env SMATCHET_JOB_MASK_ROOT="$FIX" bash "$GATE" --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"'build'"* ]]
}

@test "explicit continue-on-error: false -> PASS" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
name: X
on: { pull_request: { branches: [develop] } }
jobs:
  gate:
    runs-on: ubuntu-latest
    continue-on-error: false
    steps:
      - run: echo hi
YML
    run env SMATCHET_JOB_MASK_ROOT="$FIX" bash "$GATE" --check
    [ "$status" -eq 0 ]
}

@test "unparseable workflow yaml -> infra error (2), not a silent pass" {
    printf 'jobs: [unclosed\n' > "$FIX/.github/workflows/x.yml"
    run env SMATCHET_JOB_MASK_ROOT="$FIX" bash "$GATE" --check
    [ "$status" -eq 2 ]
}
