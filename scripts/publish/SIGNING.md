# Windows Signing

Smatchet's release script signs the standalone payload and the generated Inno Setup installer.

**Signing is mandatory for published releases**: `release-github.sh --publish` refuses to run
without `--sign` unless you explicitly pass `--allow-unsigned-publish`. Two things depend on it:
unsigned binaries trigger Windows SmartScreen warnings for every user, and the in-app
auto-updater verifies the downloaded installer's Authenticode signature before launching it —
an unsigned published installer is downloaded and then **rejected** by updating clients.
Local bundles built without `--publish` may stay unsigned (e.g. CI installer smoke jobs).

What gets signed:

- `Smatchet.exe`
- any staged runtime `.dll` files
- the outer installer `.exe`
- the Inno-generated uninstaller `unins000.exe` through Inno Setup's `SignTool` hook

## Prerequisites

- Windows SDK `signtool.exe`
- one signing identity, selected in exactly one of these ways:
  - **Azure Trusted Signing** (recommended — see below): the provider DLL + its
    metadata JSON; no certificate material on disk
  - PFX file
  - certificate thumbprint from the Windows certificate store
  - certificate subject/common name from the Windows certificate store

Passing more than one selector is an error, and so is passing none while
`--sign` is set.

## Environment Variables

You can drive signing entirely from environment variables:

```bash
export SMATCHET_SIGN_PFX_PATH='C:/secure/smatchet-signing.pfx'
export SMATCHET_SIGN_PFX_PASSWORD='your-pfx-password'
export SMATCHET_SIGN_TIMESTAMP_URL='http://timestamp.digicert.com'
```

Or use a certificate already imported into the Windows certificate store:

```bash
export SMATCHET_SIGN_CERT_SHA1='0123456789ABCDEF0123456789ABCDEF01234567'
export SMATCHET_SIGN_USE_MACHINE_STORE=1   # optional
export SMATCHET_SIGN_TIMESTAMP_URL='http://timestamp.digicert.com'
```

Supported variables:

- `SMATCHET_SIGNTOOL_PATH`
- `SMATCHET_SIGN_PFX_PATH`
- `SMATCHET_SIGN_PFX_PASSWORD`
- `SMATCHET_SIGN_CERT_SHA1`
- `SMATCHET_SIGN_CERT_SUBJECT`
- `SMATCHET_SIGN_USE_MACHINE_STORE`
- `SMATCHET_SIGN_TIMESTAMP_URL`
- `SMATCHET_SIGN_TRUSTED_SIGNING_DLIB`
- `SMATCHET_SIGN_TRUSTED_SIGNING_METADATA`

## Azure Trusted Signing

Trusted Signing is Microsoft's managed signing service. It is the preferred
identity for published releases:

- the certificate is issued by a Microsoft CA whose **SmartScreen reputation is
  inherited**, so a fresh release does not have to earn its own reputation the
  way a new OV certificate does — this is the fastest route out of the
  "Windows protected your PC" screen short of an EV certificate
- no private key ever exists on a build machine or in a repository secret; the
  signing key lives in the service and access is an Azure RBAC role
  (`Trusted Signing Certificate Profile Signer`) on the certificate profile

Mechanically it is still `signtool.exe`. Instead of `/f cert.pfx`, signtool
loads a provider DLL that mints a short-lived certificate per signature:

```bash
export SMATCHET_SIGN_TRUSTED_SIGNING_DLIB='C:/ts/bin/x64/Azure.CodeSigning.Dlib.dll'
export SMATCHET_SIGN_TRUSTED_SIGNING_METADATA='C:/ts/metadata.json'
bash scripts/publish/release-github.sh --tag v0.6.7 --sign --publish
```

`metadata.json` names the account and certificate profile — no secrets:

```json
{
  "Endpoint": "https://eus.codesigning.azure.net",
  "CodeSigningAccountName": "smatchet-signing",
  "CertificateProfileName": "smatchet-public"
}
```

The DLL ships in the `Microsoft.Trusted.Signing.Client` NuGet package
(`bin/x64/Azure.CodeSigning.Dlib.dll` — it must match signtool's architecture).

Credentials come from the environment, via `DefaultAzureCredential`. A service
principal with a client secret is the shape CI uses:

```bash
export AZURE_TENANT_ID='...'
export AZURE_CLIENT_ID='...'
export AZURE_CLIENT_SECRET='...'
```

Two things the script handles for you:

- **Timestamping.** A Trusted Signing certificate is valid for days, so an
  untimestamped signature stops verifying almost immediately — and the in-app
  updater then refuses the installer. When Trusted Signing is selected the
  default timestamp server changes to `http://timestamp.acs.microsoft.com`
  (the ACS chain requires it); `--timestamp-url` still overrides.
- **The uninstaller.** The same `/dlib` + `/dmdf` pair is passed to Inno Setup's
  `SignTool` hook, so `unins000.exe` is signed by the same identity as everything
  else in the release.

## Release Usage

Sign a local release bundle:

```bash
bash scripts/publish/release-github.sh --sign --allow-dirty
```

Sign and publish a tagged release:

```bash
bash scripts/publish/release-github.sh --tag v0.6.7 --sign --publish
```

You can also override settings directly on the command line:

```bash
bash scripts/publish/release-github.sh \
  --tag v0.6.7 \
  --sign \
  --signing-certificate-path 'C:/secure/smatchet-signing.pfx' \
  --signing-certificate-password 'your-pfx-password' \
  --timestamp-url 'http://timestamp.digicert.com'
```

## CI (GitHub Actions)

`.github/workflows/release.yml` runs `release-github.sh --sign --publish` on
every `v*.*.*` tag push.

It picks the signing identity itself: **Azure Trusted Signing** when the
`SMATCHET_TRUSTED_SIGNING_*` repository *variables* are configured, otherwise the
PFX secret. A manual dispatch can force either with the `signing_method` input
(`auto` / `trusted-signing` / `pfx`). A repo with only the PFX secret set keeps
behaving exactly as before.

### Trusted Signing in CI

Repository **variables** (Settings → Secrets and variables → Actions →
Variables) — these are not secrets:

- `SMATCHET_TRUSTED_SIGNING_ENDPOINT` — e.g. `https://eus.codesigning.azure.net`
  (its presence is also what switches `signing_method: auto` over)
- `SMATCHET_TRUSTED_SIGNING_ACCOUNT` — code signing account name
- `SMATCHET_TRUSTED_SIGNING_PROFILE` — certificate profile name

Repository **secrets** — an Entra service principal holding the
`Trusted Signing Certificate Profile Signer` role on that profile:

- `AZURE_TENANT_ID`
- `AZURE_CLIENT_ID`
- `AZURE_CLIENT_SECRET`

The workflow downloads the provider package by pinned version and SHA-256,
writes `metadata.json` into `RUNNER_TEMP`, and exposes the `AZURE_*` credentials
only to the step that builds and signs. Missing configuration fails the run
before the build starts, not at the first signature.

A client secret rather than federated OIDC is deliberate: the release job builds
for up to an hour before it signs anything, and a GitHub OIDC token minted at the
start of the job has long expired by then.

### PFX in CI

The PFX path reads the certificate from two repository secrets instead of a file
on disk:

- `SMATCHET_SIGN_PFX_BASE64` — the signing PFX, base64-encoded:

  ```powershell
  [Convert]::ToBase64String([IO.File]::ReadAllBytes('C:\secure\smatchet-signing.pfx')) |
    Set-Clipboard
  ```

  Paste the clipboard contents as the secret value.

- `SMATCHET_SIGN_PFX_PASSWORD` — the PFX passphrase.

The workflow decodes the secret to a temp file for the duration of the job
and deletes it in an `always()` cleanup step. To dry-run the pipeline
without publishing (e.g. to validate a cert rotation), dispatch the workflow
manually with `publish=false`.

### Bootstrap: publishing before a certificate exists

A tag push always requires signing. If you need to publish a release before
obtaining a code-signing certificate, dispatch the workflow manually with
`sign=false`, `publish=true`, **and** `allow_unsigned_publish=true` — all three
are required, so an unsigned publish can never happen by accident.

Understand what you are shipping:

- users get a Windows SmartScreen warning on every download and install
- **the in-app updater will refuse the unsigned installer** — it runs
  `WinVerifyTrust` and will not launch a build whose signature does not chain to
  a trusted root, so clients cannot auto-update *to* an unsigned release

Treat this as a one-time bootstrap (e.g. to establish a download page) and mark
the release as a prerelease. Sign every subsequent release.

## Notes

- The release script requires exactly one certificate selector: `--signing-certificate-path`, `--signing-certificate-thumbprint`, `--signing-certificate-subject`, or `--signing-trusted-signing-dlib` (which additionally requires `--signing-trusted-signing-metadata`).
- ZIP files are not Authenticode-signed; the signed binaries live inside the portable ZIP and installer.
- For production distribution, use Azure Trusted Signing or a real OV/EV code-signing certificate. A self-signed certificate is fine only for local pipeline validation.
- The in-app updater (`AttachmentAppUpdateService::DownloadAndLaunchInstallerUpdate`) runs
  `WinVerifyTrust` on the downloaded installer and refuses to launch it unless the signature
  chains to a trusted root. When validating the update pipeline with a self-signed certificate,
  either import your test root into the machine's trusted-root store, or set
  `SMATCHET_UPDATE_ALLOW_UNSIGNED=1` in the updating client's environment (forgives everything
  except a bad digest — a tampered file is never launched).
