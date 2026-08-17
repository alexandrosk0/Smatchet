#!/usr/bin/env bats
# tests/bats/golden_provenance.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/check-golden-provenance.sh — the guard that answers
# "was this golden produced by CI, or by somebody's GPU?" (#2099).
#
# Bucket-C diffs llvmpipe captures against the committed goldens, so the two
# sides must come from the same renderer or the comparison is meaningless. The
# guard's whole value is that it CANNOT be satisfied by a locally-regenerated
# golden — these tests pin that property, plus the WARN-first exit contract that
# keeps it from blocking every PR while the count is still 0/N.
#
# Requires: bash, sha256sum (or shasum), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export GUARD="$REPO_ROOT/scripts/dev/check-golden-provenance.sh"
    WORK="$(mktemp -d)"
    export WORK
    export SMATCHET_GOLDEN_DIR="$WORK/golden"
    mkdir -p "$SMATCHET_GOLDEN_DIR"
    printf 'alpha-pixels' > "$SMATCHET_GOLDEN_DIR/one.png"
    printf 'beta-pixels'  > "$SMATCHET_GOLDEN_DIR/two.png"
}

teardown() {
    rm -rf "$WORK"
}

@test "no manifest: every golden reports non-CI-native" {
    run bash "$GUARD"
    [ "$status" -eq 0 ]                 # WARN-first: reports, never blocks
    [[ "$output" == *"0/2 CI-native"* ]]
    [[ "$output" == *"one.png"* ]]
    [[ "$output" == *"two.png"* ]]
}

@test "--strict turns the same state into a hard failure" {
    run bash "$GUARD" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"0/2 CI-native"* ]]
}

@test "emit then verify: freshly stamped goldens are CI-native" {
    run bash "$GUARD" --emit 4242 24.2.5 1920x1009
    [ "$status" -eq 0 ]
    run bash "$GUARD" --strict
    [ "$status" -eq 0 ]
    [[ "$output" == *"2/2 CI-native"* ]]
}

@test "the manifest records run id, mesa version and geometry per golden" {
    bash "$GUARD" --emit 4242 24.2.5 1920x1009
    run cat "$SMATCHET_GOLDEN_DIR/PROVENANCE.tsv"
    [ "$status" -eq 0 ]
    [[ "$output" == *"4242"* ]]
    [[ "$output" == *"24.2.5"* ]]
    [[ "$output" == *"1920x1009"* ]]
    # One data row per golden, comments excluded.
    n="$(grep -cv '^#' "$SMATCHET_GOLDEN_DIR/PROVENANCE.tsv" || true)"
    [ "$n" -eq 2 ]
}

# The point of the guard: a golden regenerated on a developer GPU changes its
# bytes but gains no manifest row, so it must NOT keep the old blessing. This is
# the exact drift that made bucket-C non-authoritative in the first place.
@test "a locally-regenerated golden loses its CI-native status" {
    bash "$GUARD" --emit 4242 24.2.5 1920x1009
    printf 'locally-rerendered' > "$SMATCHET_GOLDEN_DIR/one.png"
    run bash "$GUARD" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"1/2 CI-native"* ]]
    [[ "$output" == *"one.png"* ]]
    [[ "$output" != *"two.png"* ]]
}

# Renaming cannot launder provenance: the sha must be paired with ITS basename,
# so a golden copied over another scenario's name is not blessed by the row it
# happens to hash-match.
@test "a golden renamed onto another scenario is not CI-native" {
    bash "$GUARD" --emit 4242 24.2.5 1920x1009
    cp "$SMATCHET_GOLDEN_DIR/one.png" "$SMATCHET_GOLDEN_DIR/three.png"
    run bash "$GUARD" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"three.png"* ]]
}

@test "a hand-edited manifest row does not bless mismatched bytes" {
    bash "$GUARD" --emit 4242 24.2.5 1920x1009
    # Forge a row claiming an all-zeros hash for one.png, and drop the real row.
    grep -v $'\tone.png\t' "$SMATCHET_GOLDEN_DIR/PROVENANCE.tsv" > "$WORK/m.tsv"
    printf '%s\tone.png\t4242\t24.2.5\t1920x1009\n' "$(printf '0%.0s' $(seq 64))" >> "$WORK/m.tsv"
    mv "$WORK/m.tsv" "$SMATCHET_GOLDEN_DIR/PROVENANCE.tsv"
    run bash "$GUARD" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"one.png"* ]]
}

@test "--list-missing prints only basenames, for scripted call sites" {
    run bash "$GUARD" --list-missing
    [ "$status" -eq 0 ]
    [[ "$output" == *"one.png"* ]]
    [[ "$output" != *"CI-native"* ]]   # no prose, just names
    bash "$GUARD" --emit 4242 24.2.5 1920x1009
    run bash "$GUARD" --list-missing
    [ "$status" -eq 0 ]
    [ -z "$output" ]                   # all native -> empty, so `-z` gates the --strict flip
}

@test "a missing golden dir is a misconfiguration (2), not a verdict" {
    export SMATCHET_GOLDEN_DIR="$WORK/does-not-exist"
    run bash "$GUARD"
    [ "$status" -eq 2 ]
}

@test "an unknown argument is rejected rather than silently ignored" {
    run bash "$GUARD" --not-a-flag
    [ "$status" -eq 2 ]
}

# The real repo state, so this suite fails loudly the day someone flips the CI
# call to --strict before the goldens are actually CI-native.
@test "the committed goldens are counted against the real manifest" {
    unset SMATCHET_GOLDEN_DIR
    cd "$REPO_ROOT"
    run bash "$GUARD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"CI-native"* ]]
}
