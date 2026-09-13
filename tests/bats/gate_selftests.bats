#!/usr/bin/env bats
# gate_selftests.bats — regression suite for
# agents/scripts/core/test-gate-selftests.sh (the "gate that gates the gates";
# Gap C / close-gate-gaps Slice 3). The convention-check enforces that every
# first-party script EXPOSING a `--selftest` flag also carries a behaviour-inert
# `# selftest: asserts-failure` marker, so a pass-only selftest cannot ship.
#
# Covers: (1) --check passes on the real tree, (2) its own --selftest dogfoods,
# (3) it FAILS on a synthetic pass-only exposer, (4) it PASSES once the marker is
# added, (5) the `: n/a — <reason>` form satisfies the rule, (6) a mere mention
# (comment / manifest string) is NOT required to carry the marker, (7) the
# SMATCHET_GATE_SELFTEST_FORCE_NONEXEC knob overrides the fs exec bit for
# untracked files in both directions — the mechanism the Windows (MSYS/NTFS)
# --selftest run relies on, where `[ -x ]` is a shebang heuristic.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/agents/scripts/core/test-gate-selftests.sh"
    TMPREPO="$(mktemp -d)"
    # A minimal git repo so the script's `git rev-parse --show-toplevel` resolves
    # to TMPREPO when run with cwd inside it.
    (
        cd "$TMPREPO" || exit 1
        git init -q
        mkdir -p agents/scripts/core agents/scripts/project scripts/dev
    )
}

teardown() {
    rm -rf "$TMPREPO"
}

# write_exposer <path> [marker-line...] — a script that EXPOSES --selftest, plus
# any extra lines (e.g. the marker) appended verbatim.
write_exposer() {
    local f="$TMPREPO/$1"; shift
    {
        printf '#!/usr/bin/env bash\n'
        printf 'case "${1:-}" in\n'
        printf '    --selftest) echo ok; exit 0 ;;\n'
        printf 'esac\n'
        for line in "$@"; do printf '%s\n' "$line"; done
    } > "$f"
}

@test "real-tree --check passes (every exposer carries the marker)" {
    run bash "$SCRIPT" --check
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "the convention-check's own --selftest passes (dogfood)" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "FAILs on a synthetic pass-only exposer (no marker)" {
    write_exposer "agents/scripts/core/fake-gate.sh"
    run bash -c "cd '$TMPREPO' && bash '$SCRIPT' --check"
    [ "$status" -eq 1 ]
    [[ "$output" == *"fake-gate.sh"* ]]
    [[ "$output" == *"asserts no FAILURE"* ]]
}

@test "PASSes once the marker is added" {
    write_exposer "agents/scripts/core/fake-gate.sh" "# selftest: asserts-failure"
    run bash -c "cd '$TMPREPO' && bash '$SCRIPT' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "the n/a marker form satisfies the rule" {
    write_exposer "agents/scripts/project/fake-dispatch.sh" \
        "# selftest: asserts-failure: n/a — pure dispatcher, no failure mode."
    run bash -c "cd '$TMPREPO' && bash '$SCRIPT' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "a mere --selftest mention (comment) is NOT required to carry the marker" {
    # Only mentions --selftest in a comment; no real flag handler -> not an exposer.
    printf '#!/usr/bin/env bash\n# this wrapper runs the lib --selftest via bats\necho hi\n' \
        > "$TMPREPO/scripts/dev/fake-wrapper.sh"
    # Plus a real exposer WITH a marker so the scan is non-empty.
    write_exposer "agents/scripts/core/real-gate.sh" "# selftest: asserts-failure"
    run bash -c "cd '$TMPREPO' && bash '$SCRIPT' --check"
    [ "$status" -eq 0 ]
    [[ "$output" != *"fake-wrapper.sh"* ]]
}

@test "fail-closed: zero exposers under the scan dirs is an infra error (exit 2)" {
    run bash -c "cd '$TMPREPO' && bash '$SCRIPT' --check"
    [ "$status" -eq 2 ]
    [[ "$output" == *"zero"* ]]
}

@test "FORCE_NONEXEC=1 overrides an executable fs bit: untracked raw self-exec is flagged" {
    # The knob run_selftest uses to pin fixture modes on MSYS/NTFS, where
    # `[ -x ]` reports ANY shebang file executable. On Linux this asserts the
    # override beats a REAL +x bit — same code path Windows depends on.
    write_exposer "agents/scripts/core/raw-exec-gate.sh" \
        "# selftest: asserts-failure" \
        '"$_SCRIPT_PATH" --check'
    chmod +x "$TMPREPO/agents/scripts/core/raw-exec-gate.sh"
    run bash -c "cd '$TMPREPO' && SMATCHET_GATE_SELFTEST_FORCE_NONEXEC=1 bash '$SCRIPT' --check"
    [ "$status" -eq 1 ]
    [[ "$output" == *"raw self-exec"* ]]
    [[ "$output" == *"raw-exec-gate.sh"* ]]
}

@test "FORCE_NONEXEC=0 overrides a non-executable fs bit: same untracked raw self-exec passes" {
    write_exposer "agents/scripts/core/raw-exec-gate.sh" \
        "# selftest: asserts-failure" \
        '"$_SCRIPT_PATH" --check'
    chmod -x "$TMPREPO/agents/scripts/core/raw-exec-gate.sh"
    run bash -c "cd '$TMPREPO' && SMATCHET_GATE_SELFTEST_FORCE_NONEXEC=0 bash '$SCRIPT' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}
