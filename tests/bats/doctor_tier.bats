#!/usr/bin/env bats
# tests/bats/doctor_tier.bats — scripts/dev/doctor.sh --tier (finding C3 /
# Proposal P5): a required toolchain check whose tool is ABSENT and not in the
# active tier's `expects` list reports [n/a] (informational) instead of a RED
# [FAIL], so an agent knows what it cannot self-validate here rather than
# flat-REDding on a toolchain that is not this environment's job.
#
# Determinism: the contrast tests strip cl.exe / clang-cl from PATH so the
# "compiler absent" condition holds on ANY host (a no-op on Linux where MSVC is
# already absent; on Windows it removes the real compiler dir), then assert the
# tier decides FAIL-vs-[n/a]. Runs the real doctor from the repo root so
# project.config.json § environments is present.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    DOCTOR="$REPO_ROOT/scripts/dev/doctor.sh"
}

# Echo the current PATH with the dirs hosting cl.exe / clang-cl removed, so the
# C++ compiler check deterministically takes its absent path.
_compiler_stripped_path() {
    local sep=':'
    case "$PATH" in *';'*) sep=';' ;; esac
    local strip=()
    local tool tp
    for tool in cl.exe clang-cl; do
        tp="$(command -v "$tool" 2>/dev/null || true)"
        [ -n "$tp" ] && strip+=("$(dirname "$tp")")
    done
    local out="" p d drop
    local IFS="$sep"
    for p in $PATH; do
        drop=0
        for d in "${strip[@]}"; do
            [ "${p%/}" = "${d%/}" ] && drop=1 && break
        done
        [ "$drop" -eq 0 ] && out="${out:+$out$sep}$p"
    done
    printf '%s' "$out"
}

@test "linux-container tier: absent MSVC compiler reports [n/a], not [FAIL], and does not RED" {
    local sp; sp="$(_compiler_stripped_path)"
    run env PATH="$sp" bash "$DOCTOR" --tier linux-container
    [ "$status" -ne 1 ]                             # not RED
    [[ "$output" == *"[n/a]  compiler"* ]]
    [[ "$output" != *"[FAIL] compiler"* ]]
    [[ "$output" == *"Environment: tier 'linux-container'"* ]]
}

@test "windows-dev tier: absent MSVC compiler is a hard [FAIL] (RED)" {
    local sp; sp="$(_compiler_stripped_path)"
    run env PATH="$sp" bash "$DOCTOR" --tier windows-dev
    [ "$status" -eq 1 ]                             # RED
    [[ "$output" == *"[FAIL] compiler"* ]]
    [[ "$output" != *"[n/a]  compiler"* ]]
}

@test "no --tier/env resolves default_tier (windows-dev today): absent compiler still [FAIL]s, not silently skipped" {
    # Locks the no-arg path: omitting --tier must NOT degrade to an all-[n/a]
    # skip. It resolves default_tier, which today expects the compiler, so a
    # stripped compiler is a hard FAIL (RED) exactly as the legacy doctor was.
    local sp; sp="$(_compiler_stripped_path)"
    run env -u SMATCHET_DOCTOR_TIER PATH="$sp" bash "$DOCTOR"
    [ "$status" -eq 1 ]
    [[ "$output" == *"[FAIL] compiler"* ]]
    [[ "$output" == *"Environment: tier 'windows-dev'"* ]]
}

@test "unknown tier is a loud usage error (exit 2), never a silent all-pass" {
    run bash "$DOCTOR" --tier no-such-tier
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown tier 'no-such-tier'"* ]]
}

@test "--tier=<name> equals-form is accepted" {
    local sp; sp="$(_compiler_stripped_path)"
    run env PATH="$sp" bash "$DOCTOR" --tier=linux-container
    [ "$status" -ne 1 ]
    [[ "$output" == *"tier 'linux-container'"* ]]
}

@test "--help prints tier usage and exits 0" {
    run bash "$DOCTOR" --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"--tier"* ]]
    [[ "$output" == *"linux-container"* ]]
}
