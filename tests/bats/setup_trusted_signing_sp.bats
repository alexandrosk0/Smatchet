#!/usr/bin/env bats
#
# setup_trusted_signing_sp.bats — guards the credential parse in
# scripts/publish/setup-trusted-signing-sp.sh.
#
# System under test: the read of `az ad sp create-for-rbac --query
# "[appId,password,tenant]" -o tsv`. az's TSV writer puts each item of a top-level
# list on its own line, so the three values arrive as three lines; a parse that
# expects one tab-separated row reads only the app id, dies, and strands the
# just-created client secret (it is shown nowhere else). A stub `az` on PATH
# replies with that real output shape, so no Azure call is made.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    cd "$REPO_ROOT" || return 1
    SP_SH="scripts/publish/setup-trusted-signing-sp.sh"
    [ -f "$SP_SH" ] || {
        echo "missing $SP_SH — test is stale, update it" >&2
        return 1
    }
    STUB_BIN="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$STUB_BIN"
    # $CREDS_SHAPE picks the create-for-rbac reply: "lines" (what az prints) or "row".
    cat >"$STUB_BIN/az" <<'EOF'
#!/usr/bin/env bash
args="$*"
case "$args" in
    "account show"*) printf 'sub-0000\r\n' ;;
    "resource show"*"accountUri"*) printf 'https://eus.codesigning.azure.net/\r\n' ;;
    "resource show"*) exit 0 ;;
    "ad app list"*) exit 0 ;;
    "ad sp create-for-rbac"*)
        if [ "${CREDS_SHAPE:-lines}" = "row" ]; then
            printf 'app-1111\tsecret-2222\ttenant-3333\r\n'
        else
            printf 'app-1111\r\nsecret-2222\r\ntenant-3333\r\n'
        fi ;;
    "ad sp show"*) printf 'obj-4444\r\n' ;;
    "role assignment create"*) exit 0 ;;
    *) echo "stub az: unexpected call: $args" >&2; exit 9 ;;
esac
EOF
    chmod +x "$STUB_BIN/az"
}

run_sp() {
    run env PATH="$STUB_BIN:$PATH" CREDS_SHAPE="$1" \
        bash "$SP_SH" --resource-group rg --account acct --profile prof --yes
}

@test "the three-line credential reply parses and the secret is printed" {
    run_sp lines
    [ "$status" -eq 0 ]
    [[ "$output" == *"AZURE_CLIENT_ID     = app-1111"* ]]
    [[ "$output" == *"AZURE_CLIENT_SECRET = secret-2222"* ]]
    [[ "$output" == *"AZURE_TENANT_ID     = tenant-3333"* ]]
    [[ "$output" != *"could not parse the credential"* ]]
    # The CR a Windows az writes must not reach the copy-paste lines.
    [[ "$output" != *$'\r'* ]]
}

@test "a one-row tab-separated reply parses too" {
    run_sp row
    [ "$status" -eq 0 ]
    [[ "$output" == *"AZURE_CLIENT_SECRET = secret-2222"* ]]
    [[ "$output" == *"AZURE_TENANT_ID     = tenant-3333"* ]]
}

@test "the endpoint read from the account reaches the gh variable line" {
    run_sp lines
    [ "$status" -eq 0 ]
    [[ "$output" == *"--body 'https://eus.codesigning.azure.net/'"* ]]
}
