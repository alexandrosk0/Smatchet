#!/usr/bin/env bats
# tests/bats/verify_fontawesome.bats
# ----------------------------------------------------------------------------
# Bats coverage for scripts/dev/verify-fontawesome.sh — the network-free sha256
# check the `.github/actions/fetch-fontawesome` composite action runs against the
# COMMITTED assets/fonts/fa-solid-900.ttf (it replaced a per-job curl from a
# single third-party host; backlog tooling/2026-08-17 + infra/2026-08-18).
#
# Cases: the real committed file verifies against the action's own pin (so the
# pin and the bytes cannot drift apart unnoticed); a missing / empty file and a
# hash mismatch fail closed with an actionable annotation; a malformed pin is a
# usage error; the action stays network-free and keeps its call-site interface.
#
# Requires: bash, bats, sha256sum (or shasum).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    VERIFY="$REPO_ROOT/scripts/dev/verify-fontawesome.sh"
    ACTION="$REPO_ROOT/.github/actions/fetch-fontawesome/action.yml"
    FONT="$REPO_ROOT/assets/fonts/fa-solid-900.ttf"
    # The pin as the action declares it — the single source of truth.
    PIN_SHA="$(sed -n 's/^[[:space:]]*FA_SHA256:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$ACTION")"
    PIN_TAG="$(sed -n 's/^[[:space:]]*FA_TAG:[[:space:]]*"\([^"]*\)".*/\1/p' "$ACTION")"
    export REPO_ROOT VERIFY ACTION FONT PIN_SHA PIN_TAG
}

@test "action.yml declares a well-formed FA_TAG + FA_SHA256 pin" {
    [[ "$PIN_SHA" =~ ^[0-9a-f]{64}$ ]]
    [ -n "$PIN_TAG" ]
}

@test "committed TTF verifies against the action's own pin (exit 0)" {
    [ -f "$FONT" ]
    run env FA_TAG="$PIN_TAG" FA_SHA256="$PIN_SHA" bash "$VERIFY"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verify-fontawesome: OK"* ]]
}

@test "missing file fails closed with a FONT-ASSET-MISSING annotation" {
    run env FA_TAG="$PIN_TAG" FA_SHA256="$PIN_SHA" FA_FILE="$BATS_TEST_TMPDIR/absent.ttf" bash "$VERIFY"
    [ "$status" -eq 1 ]
    [[ "$output" == *"::error title=FONT-ASSET-MISSING::"* ]]
    [[ "$output" == *"git checkout --"* ]]
}

@test "empty file fails closed as missing (no bytes to trust)" {
    : > "$BATS_TEST_TMPDIR/empty.ttf"
    run env FA_TAG="$PIN_TAG" FA_SHA256="$PIN_SHA" FA_FILE="$BATS_TEST_TMPDIR/empty.ttf" bash "$VERIFY"
    [ "$status" -eq 1 ]
    [[ "$output" == *"FONT-ASSET-MISSING"* ]]
}

@test "mismatched bytes fail closed and name both hashes" {
    printf 'not the pinned font\n' > "$BATS_TEST_TMPDIR/wrong.ttf"
    run env FA_TAG="$PIN_TAG" FA_SHA256="$PIN_SHA" FA_FILE="$BATS_TEST_TMPDIR/wrong.ttf" bash "$VERIFY"
    [ "$status" -eq 1 ]
    [[ "$output" == *"::error title=FONT-ASSET-MISMATCH::"* ]]
    [[ "$output" == *"$PIN_SHA"* ]]
    [[ "$output" == *"FA_TAG + FA_SHA256"* ]]
}

@test "malformed pin is a usage error (exit 2), not a silent pass" {
    run env FA_SHA256="deadbeef" bash "$VERIFY"
    [ "$status" -eq 2 ]
    [[ "$output" == *"FONT-PIN-INVALID"* ]]
    run env -u FA_SHA256 bash "$VERIFY"
    [ "$status" -eq 2 ]
}

@test "action is network-free and delegates to the verify script" {
    # Non-comment lines only — the action's comment records the retired fetch.
    run bash -c "grep -vE '^[[:space:]]*#' '$ACTION' | grep -nE '(^|[^A-Za-z])(curl|wget)([^A-Za-z]|\$)|https?://'"
    [ "$status" -eq 1 ]
    run grep -nF 'bash scripts/dev/verify-fontawesome.sh' "$ACTION"
    [ "$status" -eq 0 ]
}

@test "action keeps its call-site interface (no inputs, no outputs)" {
    run grep -nE '^(inputs|outputs):' "$ACTION"
    [ "$status" -eq 1 ]
}
