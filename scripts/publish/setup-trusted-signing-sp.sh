#!/usr/bin/env bash
# scripts/publish/setup-trusted-signing-sp.sh — create the CI service principal
# for Azure Trusted Signing and grant it the two roles the release pipeline
# needs, then print the repository variables + secrets to configure.
#
# Run once, by a user who can create app registrations in the Entra directory
# AND assign roles on the signing account. Companion to
# scripts/publish/SIGNING.md § Trusted Signing in CI — that doc is the
# reference; this script is the executable form of its setup steps.
#
# What it grants:
#   - "Trusted Signing Certificate Profile Signer" on the CERTIFICATE PROFILE
#     (the role that lets signtool's provider mint a certificate)
#   - "Reader" on the ACCOUNT — the signing client resolves the profile through
#     its account, so a profile-only assignment authenticates and then fails at
#     lookup with an opaque HRESULT
#
# Prerequisites: az CLI, `az login`, and an existing Trusted Signing account +
# certificate profile (identity validation complete, profile type Public Trust).
#
# The client secret it creates is printed ONCE and is not recoverable. Run this
# in a terminal you are watching — do not pipe the output to a shared log.
#
# Exit: 0 created · 1 failure · 2 bad usage / missing prerequisite.

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"

RESOURCE_GROUP=""
ACCOUNT=""
PROFILE=""
SUBSCRIPTION=""
APP_NAME="smatchet-ci-signing"
YEARS="1"
ASSUME_YES=0
FORCE=0

# The RBAC role that authorizes minting a certificate from a profile. Microsoft
# renamed the service to Azure Artifact Signing; if this name stops resolving,
# find the current one with:
#   az role definition list --query "[?contains(roleName,'Signer')].roleName"
# and pass it with --signer-role.
SIGNER_ROLE="Trusted Signing Certificate Profile Signer"

usage() {
    cat <<EOF
Usage: bash scripts/publish/$SCRIPT_NAME --resource-group <rg> --account <name> --profile <name> [options]

  --resource-group <rg>    Resource group holding the signing account (required)
  --account <name>         Trusted Signing account name (required)
  --profile <name>         Certificate profile name (required)
  --subscription <id>      Subscription to operate in (default: current)
  --app-name <name>        App registration display name (default: $APP_NAME)
  --years <n>              Client secret lifetime (default: $YEARS)
  --signer-role <name>     Override the signer role name
  --force                  Create a second app even if the name already exists
  --yes                    Don't prompt when a precondition can't be verified
  -h, --help               Show this help
EOF
}

die() { echo "$SCRIPT_NAME: $*" >&2; exit 1; }
warn() { echo "$SCRIPT_NAME: warning: $*" >&2; }
# `shift 2` fails WITHOUT shifting on a missing value; under `set -e` that
# aborts with no explanation. Reject it up front with a message.
need_value() { [ "$1" -ge 2 ] || { echo "$SCRIPT_NAME: $2 requires a value" >&2; exit 2; }; }

while [ $# -gt 0 ]; do
    case "$1" in
        --resource-group)   need_value $# "$1"; RESOURCE_GROUP="$2"; shift 2 ;;
        --resource-group=*) RESOURCE_GROUP="${1#*=}"; shift ;;
        --account)          need_value $# "$1"; ACCOUNT="$2"; shift 2 ;;
        --account=*)        ACCOUNT="${1#*=}"; shift ;;
        --profile)          need_value $# "$1"; PROFILE="$2"; shift 2 ;;
        --profile=*)        PROFILE="${1#*=}"; shift ;;
        --subscription)     need_value $# "$1"; SUBSCRIPTION="$2"; shift 2 ;;
        --subscription=*)   SUBSCRIPTION="${1#*=}"; shift ;;
        --app-name)         need_value $# "$1"; APP_NAME="$2"; shift 2 ;;
        --app-name=*)       APP_NAME="${1#*=}"; shift ;;
        --years)            need_value $# "$1"; YEARS="$2"; shift 2 ;;
        --years=*)          YEARS="${1#*=}"; shift ;;
        --signer-role)      need_value $# "$1"; SIGNER_ROLE="$2"; shift 2 ;;
        --signer-role=*)    SIGNER_ROLE="${1#*=}"; shift ;;
        --force)            FORCE=1; shift ;;
        --yes)              ASSUME_YES=1; shift ;;
        -h|--help)          usage; exit 0 ;;
        *)                  echo "$SCRIPT_NAME: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v az >/dev/null 2>&1 || { echo "$SCRIPT_NAME: az CLI not found — https://aka.ms/azure-cli" >&2; exit 2; }
[ -n "${RESOURCE_GROUP//[[:space:]]/}" ] || { usage >&2; exit 2; }
[ -n "${ACCOUNT//[[:space:]]/}" ]        || { usage >&2; exit 2; }
[ -n "${PROFILE//[[:space:]]/}" ]        || { usage >&2; exit 2; }
case "$YEARS" in
    ''|*[!0-9]*) die "--years must be a positive integer (got '$YEARS')" ;;
    0)           die "--years must be at least 1" ;;
esac

# Ask before doing something irreversible on an unverified precondition.
confirm_or_die() {
    local prompt="$1"
    [ "$ASSUME_YES" -eq 1 ] && return 0
    # No TTY (piped/CI): refuse rather than silently proceeding.
    [ -t 0 ] || die "$prompt — re-run with --yes to proceed anyway"
    local reply=""
    read -r -p "$prompt Continue? [y/N] " reply || reply=""
    case "$reply" in
        [yY]|[yY][eE][sS]) return 0 ;;
        *) die "aborted" ;;
    esac
}

if [ -n "$SUBSCRIPTION" ]; then
    az account set --subscription "$SUBSCRIPTION" \
        || die "could not select subscription '$SUBSCRIPTION'"
fi
SUB_ID="$(az account show --query id -o tsv 2>/dev/null)" \
    || die "not logged in — run 'az login' first"
[ -n "$SUB_ID" ] || die "could not resolve the current subscription id"

ACCOUNT_ID="/subscriptions/${SUB_ID}/resourceGroups/${RESOURCE_GROUP}/providers/Microsoft.CodeSigning/codeSigningAccounts/${ACCOUNT}"
PROFILE_ID="${ACCOUNT_ID}/certificateProfiles/${PROFILE}"

# --- Preconditions, checked BEFORE anything is created ------------------------
# A typo'd account or profile would otherwise leave an orphan app registration
# and a live client secret behind. `az resource show` can also fail for reasons
# that are not "absent" (an unregistered provider, an API-version quirk), so an
# inconclusive result asks rather than blocking a correct setup.
verify_resource() {
    local id="$1" label="$2" out=""
    if out="$(az resource show --ids "$id" -o none 2>&1)"; then
        return 0
    fi
    if printf '%s' "$out" | grep -qiE 'not ?found|does not exist|ResourceNotFound'; then
        die "$label not found: $id"
    fi
    warn "could not verify the $label ($id):"
    warn "  ${out%%$'\n'*}"
    confirm_or_die "Unverified $label."
}
verify_resource "$ACCOUNT_ID" "signing account"
verify_resource "$PROFILE_ID" "certificate profile"

# Entra permits duplicate display names, so a re-run would silently create a
# SECOND principal with the same name and its own secret — two live credentials,
# one of them untracked.
EXISTING="$(az ad app list --display-name "$APP_NAME" --query "[].appId" -o tsv 2>/dev/null || true)"
if [ -n "$EXISTING" ] && [ "$FORCE" -eq 0 ]; then
    die "an app registration named '$APP_NAME' already exists (appId: $(printf '%s' "$EXISTING" | tr '\n' ' ')).
Rotate its secret instead:  az ad app credential reset --id <appId> --years $YEARS
Or pass --app-name <other> / --force to create a separate one."
fi

# --- Create -------------------------------------------------------------------
echo "Creating service principal '$APP_NAME'"
echo "  signer role : $SIGNER_ROLE"
echo "  scoped to   : $PROFILE_ID"

# One call makes the app registration, the service principal, the client secret
# and the signer role assignment. -o tsv with an explicit projection keeps the
# parsing dependency-free (no jq/python) and the field order fixed.
CREDS_TSV="$(az ad sp create-for-rbac \
    --name "$APP_NAME" \
    --role "$SIGNER_ROLE" \
    --scopes "$PROFILE_ID" \
    --years "$YEARS" \
    --query "[appId,password,tenant]" -o tsv)" \
    || die "create-for-rbac failed. If it rejected the role name, list the current one with:
  az role definition list --query \"[?contains(roleName,'Signer')].roleName\"
then re-run with --signer-role '<name>'."

IFS=$'\t' read -r CLIENT_ID CLIENT_SECRET TENANT_ID <<<"$CREDS_TSV"
[ -n "${CLIENT_ID:-}" ] && [ -n "${CLIENT_SECRET:-}" ] && [ -n "${TENANT_ID:-}" ] \
    || die "could not parse the credential az returned — check the app registration in the portal before re-running"

# EVERYTHING PAST THIS POINT IS NON-FATAL. The client secret exists now and is
# displayed nowhere else; aborting here would strand it (the app would need a
# credential reset). So print it first, then report later failures as warnings
# with the exact command to finish by hand.
set +e

cat <<EOF

  AZURE_TENANT_ID     = ${TENANT_ID}
  AZURE_CLIENT_ID     = ${CLIENT_ID}
  AZURE_CLIENT_SECRET = ${CLIENT_SECRET}

^ The secret is shown ONCE and cannot be retrieved again. Save it now.

EOF

# Reader on the ACCOUNT, not the profile — see the header. Role assignment can
# race Entra replication of the just-created principal, so retry briefly.
echo "Granting Reader on the signing account..."
reader_ok=0
for attempt in 1 2 3 4 5; do
    if az role assignment create \
        --assignee-object-id "$(az ad sp show --id "$CLIENT_ID" --query id -o tsv 2>/dev/null)" \
        --assignee-principal-type ServicePrincipal \
        --role Reader \
        --scope "$ACCOUNT_ID" -o none 2>/dev/null; then
        reader_ok=1
        break
    fi
    [ "$attempt" -lt 5 ] && sleep $((attempt * 3))
done
if [ "$reader_ok" -ne 1 ]; then
    warn "could not assign Reader on the account. Signing will fail at profile lookup until you run:"
    warn "  az role assignment create --assignee $CLIENT_ID --role Reader --scope '$ACCOUNT_ID'"
fi

# The account knows its own regional endpoint; don't rebuild it from a guessed
# region slug. An empty result means the property name moved, not that the
# account is broken — say where to find it rather than printing a blank.
ENDPOINT="$(az resource show --ids "$ACCOUNT_ID" --query properties.accountUri -o tsv 2>/dev/null)"
endpoint_ok=1
if [ -z "${ENDPOINT:-}" ]; then
    # A literal placeholder, not prose: this value is echoed into a copy-paste
    # `gh variable set` line below, and shell metacharacters there would either
    # break the paste or set a bogus endpoint that silently keeps the pipeline
    # on the PFX path.
    ENDPOINT="PASTE_ACCOUNT_URI_HERE"
    endpoint_ok=0
    warn "could not read the account endpoint automatically; copy it from the portal Overview blade."
fi

cat <<EOF
Set these in GitHub -> Settings -> Secrets and variables -> Actions.

  Variables (not secret):
    SMATCHET_TRUSTED_SIGNING_ENDPOINT = ${ENDPOINT}
    SMATCHET_TRUSTED_SIGNING_ACCOUNT  = ${ACCOUNT}
    SMATCHET_TRUSTED_SIGNING_PROFILE  = ${PROFILE}

  Secrets: AZURE_TENANT_ID / AZURE_CLIENT_ID / AZURE_CLIENT_SECRET (above).

Or with the gh CLI, from a clone of this repo:

  gh variable set SMATCHET_TRUSTED_SIGNING_ENDPOINT --body '${ENDPOINT}'
  gh variable set SMATCHET_TRUSTED_SIGNING_ACCOUNT  --body '${ACCOUNT}'
  gh variable set SMATCHET_TRUSTED_SIGNING_PROFILE  --body '${PROFILE}'
  gh secret   set AZURE_TENANT_ID     --body '${TENANT_ID}'
  gh secret   set AZURE_CLIENT_ID     --body '${CLIENT_ID}'
  gh secret   set AZURE_CLIENT_SECRET   # prompts, keeps it out of shell history

Setting the ENDPOINT variable is what switches the release workflow's
signing_method=auto over to Trusted Signing.

The secret expires in ${YEARS} year(s). Rotate with:
  az ad app credential reset --id ${CLIENT_ID} --years ${YEARS}

Then dry-run the pipeline: dispatch the Release workflow with
publish=false, sign=true, signing_method=trusted-signing.
EOF

# A partial setup must not report success — but the credentials above are real
# and already saved by the operator, so say so alongside the non-zero exit.
if [ "$reader_ok" -ne 1 ] || [ "$endpoint_ok" -ne 1 ]; then
    echo
    warn "setup is INCOMPLETE — see the warnings above. The credentials printed above are valid; finish the remaining step(s) by hand."
    exit 1
fi
exit 0
