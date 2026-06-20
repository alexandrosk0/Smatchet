#!/usr/bin/env bats
# tests/bats/fail_open_authoring.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/test-fail-open-authoring.sh (PR-5) — the
# authoring-lint for the fail-OPEN gate-shape cluster (B zero-match grep / C
# non-recursive scan / D stale committed artefact / E single-toolchain literal).
#
# selftest: asserts-failure — the @tests drive the gate's own --selftest (a
# synthetic driver that plants one bad snippet per shape and asserts each is
# flagged), plus a real-tree advisory --check that must stay non-blocking (rc 0).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export FOA="$REPO_ROOT/agents/scripts/core/test-fail-open-authoring.sh"
}

@test "--selftest passes (synthetic driver flags each shape B/C/D/E + escape clears)" {
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
    rm -rf "$tmp"
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
    rm -rf "$tmp"
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
    rm -rf "$tmp"
}

@test "fail-closed: zero gate scripts under the scan dirs is an infra error (exit 2)" {
    tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q ) >/dev/null 2>&1
    run bash -c "cd '$tmp' && bash '$FOA' --check"
    [ "$status" -eq 2 ]
    rm -rf "$tmp"
}
