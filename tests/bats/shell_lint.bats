#!/usr/bin/env bats
# tests/bats/shell_lint.bats
# ----------------------------------------------------------------------------
# Bats coverage for scripts/dev/test-shell-lint.sh.
#
# Plan: docs/design/shell-script-self-review-lint.md. Closes
# docs/self-improvement/categories/process.md 2026-05-28 P1 entry
# "Implementer-side self-review didn't catch real shell-script bugs".
#
# Six fixtures: one per rule plus one all-good. Each test asserts the lint
# fires the expected rule id (or doesn't fire at all on known-good).
#
# Requires: bash, bats, shellcheck (`npm install -g shellcheck`).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export LINT="$REPO_ROOT/scripts/dev/test-shell-lint.sh"
    export FIXTURE_DIR="$REPO_ROOT/tests/fixtures/shell_lint"
}

# ---------- env-gate + missing-binary fallbacks ----------

@test "SMATCHET_SKIP_SHELL_LINT=1 bypasses all checks and exits 0" {
    SMATCHET_SKIP_SHELL_LINT=1 run bash "$LINT" --target "$FIXTURE_DIR/known-bad-1-deps.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 0  Failed: 0"* ]]
    [[ "$output" == *"SMATCHET_SKIP_SHELL_LINT=1"* ]]
}

@test "shellcheck not on PATH -> warn + exit 0 (matches cppcheck/clang-tidy fallback)" {
    # PATH stripped so `command -v shellcheck` returns non-zero. If /usr/bin
    # still has shellcheck (some Linux distros), skip rather than vacuously
    # pass — the assertion only carries weight when the fallback actually fires.
    PATH=/usr/bin:/bin run bash "$LINT" --target "$FIXTURE_DIR/known-bad-1-deps.sh"
    if [[ "$output" != *"shellcheck not on PATH"* ]]; then
        skip "shellcheck reachable via /usr/bin:/bin — fallback path not exercised on this host"
    fi
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 0  Failed: 0"* ]]
}

# ---------- rule 1: dependency preflight ----------

@test "rule 1 (deps): fires on python use without command -v preflight" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-1-deps.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_DEPS"* ]]
    [[ "$output" == *"python"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

# ---------- rule 2: shellcheck clean ----------

@test "rule 2 (shellcheck): fires on SC2086 unquoted expansion" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-2-shellcheck.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_SHELLCHECK"* ]]
}

# ---------- rule 3: curl -f ----------

@test "rule 3 (curl -f): fires on curl invocation without -f / --fail" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-3-curl-fail.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_CURL_FAIL"* ]]
}

# ---------- rule 4: sha256 verify ----------

@test "rule 4 (sha256): fires on curl writing to file without sha256 follow-up" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-4-no-sha.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_SHA256"* ]]
}

# ---------- rule 5: flag parity ----------

@test "rule 5 (flag parity): fires on --threshold) with shift 2 but no --threshold=*)" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-5-flag-parity.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_FLAG_PARITY"* ]]
    [[ "$output" == *"--threshold"* ]]
}

# ---------- known-good: all rules clean ----------

@test "known-good fixture passes all 5 rules" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-good.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

# ---------- self-lint: the lint script lints itself ----------

@test "test-shell-lint.sh lints itself clean (eat your own dogfood)" {
    run bash "$LINT" --target "$LINT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}
