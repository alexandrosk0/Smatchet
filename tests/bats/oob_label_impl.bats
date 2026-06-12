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
