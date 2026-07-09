#!/usr/bin/env bats
# tests/bats/oob_label_impl.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/test-oob-label-impl.sh — the documented-vs-
# implemented *-out-of-band label parity gate. Uses SMATCHET_OOB_ROOT to point
# the corpus at a throwaway fixture tree (no network).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/agents/scripts/core/test-oob-label-impl.sh"
    FIX="$(mktemp -d)"
    mkdir -p "$FIX/.github/workflows" "$FIX/agents/scripts/core"
}

teardown() {
    rm -rf "$FIX"
}

@test "documented-only label (comment, no read) -> FAIL" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
# The ghost-out-of-band label downgrades the gate to a warning.
name: X
on: { pull_request: { branches: [develop] } }
jobs:
  g: { runs-on: ubuntu-latest, steps: [ { run: "echo hi" } ] }
YML
    run env SMATCHET_OOB_ROOT="$FIX" bash "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ghost-out-of-band"* ]]
}

@test "implemented label (non-comment read) -> PASS" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
# The ghost-out-of-band label downgrades the gate to a warning.
name: X
on: { pull_request: { branches: [develop] } }
jobs:
  g:
    runs-on: ubuntu-latest
    steps:
      - run: |
          if echo ",$labels," | grep -q ',ghost-out-of-band,'; then o=true; fi
YML
    run env SMATCHET_OOB_ROOT="$FIX" bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "merge-gates.sh impl counts as implementation" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
# The phantom-out-of-band label downgrades CR to WARN.
name: X
on: { pull_request: { branches: [develop] } }
YML
    cat > "$FIX/agents/scripts/core/merge-gates.sh" <<'SH'
#!/usr/bin/env bash
if has_label "phantom-out-of-band"; then downgrade=1; fi
SH
    run env SMATCHET_OOB_ROOT="$FIX" bash "$GATE"
    [ "$status" -eq 0 ]
}

@test "no labels at all -> WARN, exit 0" {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
name: X
on: { pull_request: { branches: [develop] } }
YML
    run env SMATCHET_OOB_ROOT="$FIX" bash "$GATE"
    [ "$status" -eq 0 ]
}

@test "--selftest passes" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

# ---------- poller-vs-admin CI-downgrade parity ----------

# Minimal doc corpus so _run_check stays quiet while parity is exercised.
write_parity_fixtures() {
    cat > "$FIX/.github/workflows/x.yml" <<'YML'
name: X
on: { pull_request: { branches: [develop] } }
YML
    cat > "$FIX/mg.sh" <<'SH'
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "phantom-out-of-band")) as $ph
| ([$failing[] | select(
      ($tests and .name == "Test-delta gate") or
      ($ph and .name == "Phantom gate"))]) as $downgraded
SH
}

@test "parity: label wired in merge-gates but not safe-admin-merge -> FAIL naming both sets" {
    write_parity_fixtures
    cat > "$FIX/sam.sh" <<'SH'
evaluate_rollup() {
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | (($testsOob and $g.name == "Test-delta gate") | not)
}
SH
    run env SMATCHET_OOB_ROOT="$FIX" SMATCHET_OOB_MG_FILE="$FIX/mg.sh" \
            SMATCHET_OOB_SAM_FILE="$FIX/sam.sh" bash "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"DIVERGE"* ]]
    [[ "$output" == *"phantom-out-of-band"* ]]
}

@test "parity: identical downgrade sets -> PASS" {
    write_parity_fixtures
    cat > "$FIX/sam.sh" <<'SH'
evaluate_rollup() {
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | ($labels | any(. == "phantom-out-of-band")) as $phOob
        | (($testsOob and $g.name == "Test-delta gate") | not)
        | (($phOob and $g.name == "Phantom gate") | not)
}
SH
    run env SMATCHET_OOB_ROOT="$FIX" SMATCHET_OOB_MG_FILE="$FIX/mg.sh" \
            SMATCHET_OOB_SAM_FILE="$FIX/sam.sh" bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"same CI-downgrade label set"* ]]
}

@test "parity: bound-but-unused var in evaluate_rollup is not wiring -> FAIL" {
    write_parity_fixtures
    cat > "$FIX/sam.sh" <<'SH'
evaluate_rollup() {
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | ($labels | any(. == "phantom-out-of-band")) as $phOob
        | (($testsOob and $g.name == "Test-delta gate") | not)
}
SH
    run env SMATCHET_OOB_ROOT="$FIX" SMATCHET_OOB_MG_FILE="$FIX/mg.sh" \
            SMATCHET_OOB_SAM_FILE="$FIX/sam.sh" bash "$GATE"
    [ "$status" -eq 1 ]
}

@test "parity: real merge-gates.sh and safe-admin-merge.sh agree" {
    run bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"same CI-downgrade label set"* ]]
    [[ "$output" == *"plan-lock-out-of-band"* ]]
}
