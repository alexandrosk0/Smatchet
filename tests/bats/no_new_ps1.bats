#!/usr/bin/env bats
# tests/bats/no_new_ps1.bats
# ----------------------------------------------------------------------------
# scripts/dev/test-no-new-ps1.sh (rule `no-new-ps1`, tooling 2026-08-05): the
# tracked .ps1 files (host + the agent-layer/ mount) equal the agent layer's
# docs/harness/SETUP.md § Windows-only shims table, each listed one carries its
# marker comment, and every one is ASCII / no BOM / LF. The gate's --selftest
# covers each violation on synthetic trees; these cases pin the real tree green
# and drive the NO_NEW_PS1_ROOT fixture seam end to end on a copy of the real
# SETUP.md, so a table-format change that the parser no longer reads shows up
# here. The fixture is flat (SETUP.md at its root, the shims at their checkout
# paths as plain files), which is the gate's no-mount layout.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/scripts/dev/test-no-new-ps1.sh"
    # SETUP.md is agent-layer content: the populated agent-layer/ mount, else
    # this tree (the gate's own rule).
    LAYER_ROOT="$REPO_ROOT"
    [ -f "$REPO_ROOT/agent-layer/scripts/dev/project-config.sh" ] && LAYER_ROOT="$REPO_ROOT/agent-layer"
    FIX="$(mktemp -d)"
    mkdir -p "$FIX/docs/harness"
    cp "$LAYER_ROOT/docs/harness/SETUP.md" "$FIX/docs/harness/SETUP.md"
    git init -q "$FIX"
    # The real shims, copied byte for byte (the layer's included).
    while IFS= read -r f; do
        mkdir -p "$FIX/$(dirname "$f")"
        cp "$REPO_ROOT/$f" "$FIX/$f"
    done < <(git -C "$REPO_ROOT" ls-files --recurse-submodules '*.ps1')
    git -C "$FIX" add -A
}

teardown() { rm -rf "$FIX" 2>/dev/null || true; }

gate() { NO_NEW_PS1_ROOT="$FIX" bash "$GATE" "$@"; }

@test "--selftest passes" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"--selftest: PASS"* ]]
}

@test "the real tree is green" {
    run bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "a copy of the real tree is green (SETUP.md table parses to every shim)" {
    run gate
    [ "$status" -eq 0 ]
}

@test "a new .ps1 next to the real shims fails and names the rule" {
    printf '# Last remaining PowerShell file\nWrite-Host hi\n' > "$FIX/scripts-new.ps1"
    git -C "$FIX" add -A
    run gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"new PowerShell file: scripts-new.ps1"* ]]
    [[ "$output" == *"no-new-ps1"* ]]
}

@test "an em-dash in a real shim fails the encoding check" {
    f="$(git -C "$FIX" ls-files '*.ps1' | head -n 1)"
    printf 'Write-Host "a \xe2\x80\x94 b"\n' >> "$FIX/$f"
    run gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"$f has non-ASCII bytes"* ]]
}

@test "dropping a shim's table row fails until the file goes too" {
    f="$(git -C "$FIX" ls-files '*.ps1' | head -n 1)"
    name="${f##*/}"
    grep -vF "| \`$name\` |" "$FIX/docs/harness/SETUP.md" > "$FIX/setup.tmp"
    mv "$FIX/setup.tmp" "$FIX/docs/harness/SETUP.md"
    run gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"new PowerShell file: $f"* ]]
    git -C "$FIX" rm -q -f "$f"
    run gate
    [ "$status" -eq 0 ]
}

@test "a root that is not a git work tree is an infra error, not a pass" {
    nogit="$(mktemp -d)"
    mkdir -p "$nogit/docs/harness"
    cp "$FIX/docs/harness/SETUP.md" "$nogit/docs/harness/"
    run env NO_NEW_PS1_ROOT="$nogit" bash "$GATE"
    rm -rf "$nogit"
    [ "$status" -eq 2 ]
    [[ "$output" == *"git ls-files failed"* ]]
}
