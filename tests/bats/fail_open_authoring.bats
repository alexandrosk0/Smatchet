#!/usr/bin/env bats
# tests/bats/fail_open_authoring.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/test-fail-open-authoring.sh — the
# authoring-lint for the fail-OPEN gate-shape cluster (PR-5 shapes B zero-match
# grep / C non-recursive scan / D stale committed artefact / E single-toolchain
# literal; Batches 13–17 recurrence shapes F unanchored suppression / G bats
# cleanup tail / H errexit-graceful-path / I zero-tally green / Z zero-run
# driver — see the reopened fail-open-meta-gate-authoring-check entry in
# docs/self-improvement/categories/tooling.md).
#
# selftest: asserts-failure — the @tests drive the gate's own --selftest (a
# synthetic driver that plants one bad snippet per shape and asserts each is
# flagged), plus a real-tree advisory --check that must stay non-blocking (rc 0).
#
# Cleanup lives in teardown() (never as a @test's last command) so a failing
# assertion is what bats scores — the exact shape-G defect this suite tests for.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export FOA="$REPO_ROOT/agents/scripts/core/test-fail-open-authoring.sh"
    tmp=""
}

teardown() {
    [ -n "${tmp:-}" ] && rm -rf "$tmp"
    return 0
}

@test "--selftest passes (synthetic driver flags each shape B/C/D/E/F/G/H/I/Z + escape clears)" {
    run bash "$FOA" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "real-tree --check is advisory and always exits 0 (WARN-first calibration)" {
    run bash "$FOA" --check
    [ "$status" -eq 0 ]
}

@test "--check-strict FAILs on a planted shape-B fail-open in a synthetic tree" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/agents/scripts/core"
    # A grep whose no-match path is a clean signal with no || handler.
    printf '#!/usr/bin/env bash\ngrep -q "X" "$f" && echo clean\n' \
        > "$tmp/agents/scripts/core/bad-gate.sh"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check-strict"
    [ "$status" -eq 1 ]
    [[ "$output" == *"B-zero-match-grep"* ]]
}

@test "--check-strict PASSes once the planted shape-B carries the escape marker" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/agents/scripts/core"
    printf '#!/usr/bin/env bash\ngrep -q "X" "$f" && echo clean # fail-open-ok: literal probe\n' \
        > "$tmp/agents/scripts/core/ok-gate.sh"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check-strict"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "--check-strict FAILs on a planted shape-C non-recursive python glob" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/agents/scripts/core"
    printf "for p in glob.glob('Source/Core/*.cpp'): scan(p)\n" \
        > "$tmp/agents/scripts/core/bad-scan.py"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check-strict"
    [ "$status" -eq 1 ]
    [[ "$output" == *"C-nonrecursive-scan"* ]]
}

@test "--check-strict FAILs on a planted shape-Z FAILED-only driver (no PASSED floor)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/agents/scripts/core"
    printf '#!/usr/bin/env bash\necho "Passed: $PASSED  Failed: $FAILED"\nif [ "$FAILED" != "0" ]; then exit 1; fi\nexit 0\n' \
        > "$tmp/agents/scripts/core/bad-driver.sh"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check-strict"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Z-zero-run-driver"* ]]
}

@test "--check-strict PASSes a driver with a PASSED zero-run floor (shape Z negative)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/agents/scripts/core"
    printf '#!/usr/bin/env bash\nif [ "$FAILED" != "0" ]; then exit 1; fi\nif [ "$PASSED" -eq 0 ]; then echo "FAIL: 0 tests ran"; exit 1; fi\nexit 0\n' \
        > "$tmp/agents/scripts/core/good-driver.sh"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check-strict"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "--check-strict FAILs on a planted shape-G bats cleanup tail (and ignores teardown rm)" {
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/agents/scripts/core" "$tmp/tests/bats"
    printf '#!/usr/bin/env bash\ntrue\n' > "$tmp/agents/scripts/core/placeholder.sh"
    printf '%s\n' 'teardown() {' '    rm -rf "$OK_IN_TEARDOWN"' '}' \
        '@test "x" {' '    [[ "$output" == *"hit"* ]]' '    rm -rf "$tmp"' '}' \
        > "$tmp/tests/bats/bad.bats"
    ( cd "$tmp" && git init -q && git add -A ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check-strict"
    [ "$status" -eq 1 ]
    [[ "$output" == *"G-bats-cleanup-tail"* ]]
    # exactly one G hit: the in-@test tail, not the teardown rm
    [ "$(printf '%s\n' "$output" | grep -c 'G-bats-cleanup-tail')" -eq 1 ]
}

@test "fail-closed: zero gate scripts under the scan dirs is an infra error (exit 2)" {
    tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check"
    [ "$status" -eq 2 ]
}
