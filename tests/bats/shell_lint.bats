#!/usr/bin/env bats
# tests/bats/shell_lint.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/test-shell-lint.sh.
#
# Plan: docs/design/shell-script-self-review-lint.md. Closes
# docs/self-improvement/categories/process.md 2026-05-28 P1 entry
# "Implementer-side self-review didn't catch real shell-script bugs".
#
# One fixture per rule plus one all-good. Each test asserts the lint fires the
# expected rule id (or doesn't fire at all on known-good). Rule 6 (pipefail
# var=$(...|head) SIGPIPE/truncation) adds a flagged + two negative fixtures.
#
# Requires: bash, bats, shellcheck (`npm install -g shellcheck`).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export LINT="$REPO_ROOT/agents/scripts/core/test-shell-lint.sh"
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

@test "rule 1 (deps): many-line unguarded use does not SIGPIPE the gate (exit 141 regression)" {
    # The deps rule's first-line extraction used to be `printf … | head -1`;
    # with a large $real_use (a tool on many lines) head closed the pipe early,
    # SIGPIPE'd printf, and under `set -euo pipefail` the plain assignment
    # returned 141 → the whole gate aborted with exit 141 (CI-only; msys bash
    # ignores SIGPIPE so it passed locally). Force the default SIGPIPE
    # disposition so this reproduces off-msys, and assert a clean finding, not a
    # crash.
    run bash -c "trap - PIPE; exec bash '$LINT' --target '$FIXTURE_DIR/known-bad-1-deps-manylines.sh'"
    [ "$status" -eq 1 ]   # a finding, NEVER 141
    [[ "$output" == *"SHELL_LINT_DEPS"* ]]
    [[ "$output" == *"jq"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "rule 5 (flag-parity): long ;;-free case body does not SIGPIPE the gate (exit 141 regression)" {
    # The flag-parity rule scanned a case-branch body with
    # `awk … "$script" | head -10`. awk STREAMS the whole file from the flag
    # line until the first `;;`; a branch whose body runs >10 lines (here >64KB,
    # no early `;;`) keeps awk writing after head closes the pipe → awk SIGPIPE →
    # 141 → set -e aborts the gate. This is the SECOND exit-141 trigger (#995
    # fixed only the deps-rule pipe). Force the default SIGPIPE disposition so it
    # reproduces off-msys, and assert a clean finding, not a crash.
    run bash -c "trap - PIPE; exec bash '$LINT' --target '$FIXTURE_DIR/known-bad-5-parity-manylines.sh'"
    [ "$status" -eq 1 ]   # a finding, NEVER 141
    [[ "$output" == *"SHELL_LINT_FLAG_PARITY"* ]]
    [[ "$output" == *"bigflag"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "rule 1 (deps): does NOT fire on graceful '|| fallback' / if-while-condition shapes" {
    # Relaxation lock-in (follow-up to PR #624): tools used with same-line
    # `|| <fallback>` or as an if/while/until condition self-handle a missing
    # tool, so no `command -v` preflight is required and the rule must stay
    # silent. A regression here would re-introduce the 6 false positives that
    # blocked expanding the gate to agents/scripts/.
    run bash "$LINT" --target "$FIXTURE_DIR/known-good-1-deps-graceful.sh"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_DEPS"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
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

# ---------- rule 6: pipefail var=$(...|head) SIGPIPE/truncation ----------

@test "rule 6 (pipefail-head): fires on x=\$(cmd | head -5) under pipefail" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-6-pipefail-head.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_PIPEFAIL_HEAD"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "rule 6 (pipefail-head): does NOT fire when mitigated with '|| true' (nor on non-terminal head)" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-good-6-pipefail-head-mitigated.sh"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_PIPEFAIL_HEAD"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

@test "rule 6 (pipefail-head): does NOT fire when the script does not set pipefail" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-good-6-no-pipefail-head.sh"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_PIPEFAIL_HEAD"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

# ---------- rule 8: producer | early-break reader (SIGPIPE-141 class) ----------

@test "rule 8 (early-break-pipe): fires on producer | early-break-reader-fn under pipefail" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-8-early-break-pipe.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_EARLY_BREAK_PIPE"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "rule 8 (early-break-pipe): silent on full-drain reader + temp-file-fed early-break reader" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-good-8-early-break-pipe.sh"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_EARLY_BREAK_PIPE"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

# ---------- rule 7: NUL-byte guard ----------

@test "rule 7 (nul-byte): fires on a script containing an embedded NUL byte" {
    # Synthesize the NUL-bearing script at runtime (printf '\0') so the repo never
    # commits a binary fixture. A NUL truncates the script at execution under bash;
    # the guard must flag it as corruption.
    tmp="$(mktemp -d)"
    bad="$tmp/known-bad-7-nul.sh"
    printf '#!/usr/bin/env bash\necho before\0echo after\n' > "$bad"
    run bash "$LINT" --target "$bad"
    rm -rf "$tmp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_NUL_BYTE"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "rule 7 (nul-byte): does NOT fire on a clean ASCII script" {
    tmp="$(mktemp -d)"
    good="$tmp/known-good-7-clean.sh"
    printf '#!/usr/bin/env bash\nset -euo pipefail\necho ok\n' > "$good"
    run bash "$LINT" --target "$good"
    rm -rf "$tmp"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_NUL_BYTE"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

# ---------- rule 9: resolve-only python-interpreter picker ----------

@test "rule 9 (py-probe): fires on both picker shapes (candidate loop + literal chain)" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-bad-9-py-probe.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SHELL_LINT_PY_PROBE"* ]]
    # Both shapes must be reported, not just whichever runs first: the loop at
    # line 11 (shape a) and the if/elif chain at line 17 (shape b).
    [[ "$output" == *"known-bad-9-py-probe.sh:11: SHELL_LINT_PY_PROBE"* ]]
    [[ "$output" == *"known-bad-9-py-probe.sh:17: SHELL_LINT_PY_PROBE"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "rule 9 (py-probe): does NOT fire on exec-validated pickers nor a single-candidate guard" {
    run bash "$LINT" --target "$FIXTURE_DIR/known-good-9-py-probe.sh"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_PY_PROBE"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

@test "rule 9 (py-probe): does NOT fire on the canonical in-tree resolver" {
    # assert-code-unchanged.sh validates a DIFFERENT variable than it probes
    # (`_p="$(command -v "$_c")"` … `"$_p" -c ""`). Anchoring the exec-check
    # search to the loop variable would false-positive on the very shape the
    # rule tells everyone to adopt.
    run bash "$LINT" --target "$REPO_ROOT/agents/scripts/core/assert-code-unchanged.sh"
    [ "$status" -eq 0 ]
    [[ "$output" != *"SHELL_LINT_PY_PROBE"* ]]
}

# ---------- known-good: all rules clean ----------

@test "known-good fixture passes all 9 rules" {
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

# ---------- scope coverage: --list-targets includes the new dirs ----------

@test "default target set covers scripts/mobile (recursive) and docs/harness hooks" {
    # Guards the scope extension: a regression that reverts the target
    # derivation back to the flat maxdepth-1 set would silently drop the
    # mobile build helpers (nested under openssl/) and the per-harness guard
    # scripts from the required Shell-lint gate. Path-prefix asserts (not exact
    # filenames) so individual script renames don't break this.
    run bash "$LINT" --list-targets
    [ "$status" -eq 0 ]
    [[ "$output" == *"scripts/mobile/"* ]]
    [[ "$output" == *"docs/harness/"*"/hooks/"* ]]
}

@test "default target set covers agents/scripts/core/lib (sourced libraries)" {
    # ci-check-resolve.sh (the shared name->workflow resolution map) lives one
    # level down in lib/; a regression reverting to the flat maxdepth-1 set would
    # silently drop it from the required Shell-lint gate.
    run bash "$LINT" --list-targets
    [ "$status" -eq 0 ]
    [[ "$output" == *"agents/scripts/core/lib/"* ]]
}

@test "default target set covers agents/scripts/project/lint-rules.d (sourced rule modules)" {
    # test-lint-rules.sh sources its per-rule-family modules from lint-rules.d/;
    # a regression reverting to the flat maxdepth-1 set would silently drop the
    # strict-zone rule scanners from the required Shell-lint gate.
    run bash "$LINT" --list-targets
    [ "$status" -eq 0 ]
    [[ "$output" == *"agents/scripts/project/lint-rules.d/"* ]]
}
