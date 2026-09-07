#!/usr/bin/env bats
#
# release_signing_selectors.bats — guards the signing-selector contract in
# scripts/publish/release-github.sh.
#
# System under test: `configure_signing` (which selector wins, which timestamp
# server it defaults to) plus the two argv emitters that carry that choice to
# native signtool — `signtool_argv` (payload + installer) and
# `inno_signtool_definition` (the Inno Setup SignTool hook that signs
# unins000.exe). The two MUST agree: a selector wired into one and not the other
# ships a release whose uninstaller is unsigned, which is exactly the state
# SIGNING.md says never to publish.
#
# Azure Trusted Signing is the fourth selector and the odd one out: there is no
# certificate on disk and no thumbprint. signtool loads a provider DLL (/dlib)
# that mints a short-lived certificate described by a metadata JSON (/dmdf), so
# the emitters must pass BOTH switches and must NOT fall through to /f, /sha1 or
# /n. Its leaf certificate expires in days, so the ACS timestamp server is not
# cosmetic — an untimestamped Trusted Signing signature stops verifying almost
# immediately, and the in-app updater then refuses the installer.
#
# The signing path itself is Windows-only (signtool, Inno). These tests run
# anywhere: they exercise selector arithmetic and argv shape, never a real sign.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    cd "$REPO_ROOT" || return 1
    RELEASE_SH="scripts/publish/release-github.sh"
    [ -f "$RELEASE_SH" ] || {
        echo "missing $RELEASE_SH — test is stale, update it" >&2
        return 1
    }
    # Exported: the function-level harness runs in a child bash and reads it.
    export FIXTURE="$BATS_TEST_TMPDIR/fixture"
    mkdir -p "$FIXTURE"
    : > "$FIXTURE/cert.pfx"
    : > "$FIXTURE/Azure.CodeSigning.Dlib.dll"
    : > "$FIXTURE/metadata.json"
}

# --- Function-level harness ---------------------------------------------------
# release-github.sh is a top-to-bottom program: sourcing it runs a release. To
# unit-test its signing functions, lift them out by name and run them against
# stubs. Extraction is anchored on `name() {` .. a column-0 `}` — the file's
# uniform style — and a miss fails the test loudly rather than silently skipping.

_extract_fn() {
    local fn="$1" body
    body="$(awk -v fn="$fn" '
        BEGIN { re = "^" fn "\\(\\) \\{$" }
        $0 ~ re { p = 1 }
        p { print }
        p && /^\}$/ { exit }
    ' "$RELEASE_SH")"
    [ -n "$body" ] || {
        echo "could not extract $fn() from $RELEASE_SH — test is stale, update it" >&2
        return 1
    }
    printf '%s\n' "$body"
}

# Runs `$1` (shell code) with the signing functions in scope. Stubs stand in for
# the pieces that need a real Windows host or a real filesystem walk; everything
# the assertions look at is the genuine script text.
_with_signing_fns() {
    local prelude harness
    prelude='
set -uo pipefail
die() { echo "release-github: $*" >&2; exit 1; }
abs_path() { printf "%s\n" "$1"; }
winpath() { printf "%s\n" "$1"; }
resolve_signtool() { printf "%s\n" "C:\\sdk\\signtool.exe"; }
SIGN=1
SIGNTOOL_PATH=""
SIGNING_CERT_PATH=""
SIGNING_CERT_PASSWORD=""
SIGNING_CERT_THUMBPRINT=""
SIGNING_CERT_SUBJECT=""
SIGNING_TRUSTED_SIGNING_DLIB=""
SIGNING_TRUSTED_SIGNING_METADATA=""
TIMESTAMP_URL=""
USE_MACHINE_CERT_STORE=0
SIGN_TOOL=""
SIGN_CERT_PATH=""
SIGN_CERT_PASSWORD=""
SIGN_CERT_THUMBPRINT=""
SIGN_CERT_SUBJECT=""
SIGN_TS_DLIB=""
SIGN_TS_METADATA=""
SIGN_TIMESTAMP_URL=""
SIGN_USE_MACHINE_STORE=0
'
    harness="$prelude
$(grep -m1 '^TRUSTED_SIGNING_TIMESTAMP_URL=' "$RELEASE_SH")
$(_extract_fn quote_inno_value)
$(_extract_fn configure_signing)
$(_extract_fn signtool_argv)
$(_extract_fn inno_signtool_definition)
$1"
    printf '%s' "$harness" | bash
}

# --- Selector arity (end-to-end, through the real CLI) ------------------------

@test "two selectors are rejected: PFX and Trusted Signing dlib together" {
    run bash "$RELEASE_SH" --sign \
        --signing-certificate-path "$FIXTURE/cert.pfx" \
        --signing-trusted-signing-dlib "$FIXTURE/Azure.CodeSigning.Dlib.dll"
    [ "$status" -ne 0 ]
    [[ "$output" == *"exactly one certificate selector"* ]]
}

@test "Trusted Signing dlib without metadata is rejected" {
    run bash "$RELEASE_SH" --sign \
        --signing-trusted-signing-dlib "$FIXTURE/Azure.CodeSigning.Dlib.dll"
    [ "$status" -ne 0 ]
    [[ "$output" == *"requires --signing-trusted-signing-metadata"* ]]
}

@test "Trusted Signing metadata alongside a store thumbprint is rejected" {
    run bash "$RELEASE_SH" --sign \
        --signing-certificate-thumbprint DEADBEEF \
        --signing-trusted-signing-metadata "$FIXTURE/metadata.json"
    [ "$status" -ne 0 ]
    [[ "$output" == *"without --signing-trusted-signing-dlib"* ]]
}

@test "a missing Trusted Signing dlib is named in the error" {
    run bash "$RELEASE_SH" --sign \
        --signing-trusted-signing-dlib "$FIXTURE/absent.dll" \
        --signing-trusted-signing-metadata "$FIXTURE/metadata.json"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Trusted Signing dlib not found"* ]]
    [[ "$output" == *"absent.dll"* ]]
}

@test "selector arity is reported without a Windows SDK present" {
    # Regression guard for ordering: resolve_signtool() must run AFTER the
    # selector checks, or every misconfiguration on a non-Windows host reports
    # "Unable to locate signtool.exe" and hides the real cause.
    run bash "$RELEASE_SH" --sign
    [ "$status" -ne 0 ]
    [[ "$output" == *"exactly one certificate selector"* ]]
    [[ "$output" != *"Unable to locate signtool.exe"* ]]
}

# --- Environment-variable parity ----------------------------------------------

@test "SMATCHET_SIGN_TRUSTED_SIGNING_* select Trusted Signing without CLI flags" {
    run env \
        SMATCHET_SIGN_TRUSTED_SIGNING_DLIB="$FIXTURE/Azure.CodeSigning.Dlib.dll" \
        SMATCHET_SIGN_TRUSTED_SIGNING_METADATA="$FIXTURE/metadata.json" \
        bash "$RELEASE_SH" --sign
    [ "$status" -ne 0 ]
    # Config is accepted; the run stops at the Windows-only signtool lookup.
    [[ "$output" != *"certificate selector"* ]]
    [[ "$output" == *"Unable to locate signtool.exe"* ]]
}

# --- Emitted signtool argv ----------------------------------------------------

@test "signtool_argv passes /dlib and /dmdf for Trusted Signing, never /f" {
    run _with_signing_fns '
        SIGNING_TRUSTED_SIGNING_DLIB="$FIXTURE/Azure.CodeSigning.Dlib.dll"
        SIGNING_TRUSTED_SIGNING_METADATA="$FIXTURE/metadata.json"
        configure_signing
        signtool_argv "/stage/Smatchet.exe"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"/dlib"* ]]
    [[ "$output" == *"$FIXTURE/Azure.CodeSigning.Dlib.dll"* ]]
    [[ "$output" == *"/dmdf"* ]]
    [[ "$output" == *"$FIXTURE/metadata.json"* ]]
    [ "$(printf '%s\n' "$output" | grep -cx -- '/f')" -eq 0 ]
    [ "$(printf '%s\n' "$output" | grep -cx -- '/sha1')" -eq 0 ]
    # One argument per line: a path that lost its switch would collapse the count.
    [ "$(printf '%s\n' "$output" | grep -cx -- '/dlib')" -eq 1 ]
    [ "$(printf '%s\n' "$output" | grep -cx -- '/dmdf')" -eq 1 ]
    [ "$(printf '%s\n' "$output" | tail -1)" = "/stage/Smatchet.exe" ]
}

@test "the Inno SignTool hook carries the same Trusted Signing switches" {
    run _with_signing_fns '
        SIGNING_TRUSTED_SIGNING_DLIB="$FIXTURE/Azure.CodeSigning.Dlib.dll"
        SIGNING_TRUSTED_SIGNING_METADATA="$FIXTURE/metadata.json"
        configure_signing
        inno_signtool_definition "Smatchet Installer"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"/dlib"* ]]
    [[ "$output" == *"/dmdf"* ]]
    [[ "$output" != *"/f "* ]]
    # Inno substitutes $f with the file it wants signed; without it the hook
    # signs nothing and the uninstaller ships unsigned.
    [[ "$output" == *'$f' ]]
}

@test "Trusted Signing defaults to the ACS timestamp server" {
    run _with_signing_fns '
        SIGNING_TRUSTED_SIGNING_DLIB="$FIXTURE/Azure.CodeSigning.Dlib.dll"
        SIGNING_TRUSTED_SIGNING_METADATA="$FIXTURE/metadata.json"
        configure_signing
        printf "%s\n" "$SIGN_TIMESTAMP_URL"
    '
    [ "$status" -eq 0 ]
    [ "$output" = "http://timestamp.acs.microsoft.com" ]
}

@test "an explicit --timestamp-url still wins under Trusted Signing" {
    run _with_signing_fns '
        SIGNING_TRUSTED_SIGNING_DLIB="$FIXTURE/Azure.CodeSigning.Dlib.dll"
        SIGNING_TRUSTED_SIGNING_METADATA="$FIXTURE/metadata.json"
        TIMESTAMP_URL="http://timestamp.example/rfc3161"
        configure_signing
        printf "%s\n" "$SIGN_TIMESTAMP_URL"
    '
    [ "$status" -eq 0 ]
    [ "$output" = "http://timestamp.example/rfc3161" ]
}

# --- PFX regression: the pre-existing selector is untouched -------------------

@test "PFX signing still emits /f /p and keeps the DigiCert timestamp default" {
    run _with_signing_fns '
        SIGNING_CERT_PATH="$FIXTURE/cert.pfx"
        SIGNING_CERT_PASSWORD="hunter2"
        configure_signing
        printf "%s\n" "$SIGN_TIMESTAMP_URL"
        signtool_argv "/stage/Smatchet.exe"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"http://timestamp.digicert.com"* ]]
    [[ "$output" == *"$FIXTURE/cert.pfx"* ]]
    [[ "$output" == *"hunter2"* ]]
    [[ "$output" != *"/dlib"* ]]
}
