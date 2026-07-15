# Windows Signing

Smatchet's release script signs the standalone payload and the generated Inno Setup installer.

**Signing is mandatory for published releases**: `release_github.ps1 -Publish` refuses to run
without `-Sign` unless you explicitly pass `-AllowUnsignedPublish`. Two things depend on it:
unsigned binaries trigger Windows SmartScreen warnings for every user, and the in-app
auto-updater verifies the downloaded installer's Authenticode signature before launching it —
an unsigned published installer is downloaded and then **rejected** by updating clients.
Local bundles built without `-Publish` may stay unsigned (e.g. CI installer smoke jobs).

What gets signed:

- `Smatchet.exe`
- any staged runtime `.dll` files
- the outer installer `.exe`
- the Inno-generated uninstaller `unins000.exe` through Inno Setup's `SignTool` hook

## Prerequisites

- Windows SDK `signtool.exe`
- one code-signing certificate, selected in exactly one of these ways:
  - PFX file
  - certificate thumbprint from the Windows certificate store
  - certificate subject/common name from the Windows certificate store

## Environment Variables

You can drive signing entirely from environment variables:

```powershell
$env:SMATCHET_SIGN_PFX_PATH = 'C:\secure\smatchet-signing.pfx'
$env:SMATCHET_SIGN_PFX_PASSWORD = 'your-pfx-password'
$env:SMATCHET_SIGN_TIMESTAMP_URL = 'http://timestamp.digicert.com'
```

Or use a certificate already imported into the Windows certificate store:

```powershell
$env:SMATCHET_SIGN_CERT_SHA1 = '0123456789ABCDEF0123456789ABCDEF01234567'
$env:SMATCHET_SIGN_USE_MACHINE_STORE = '1'   # optional
$env:SMATCHET_SIGN_TIMESTAMP_URL = 'http://timestamp.digicert.com'
```

Supported variables:

- `SMATCHET_SIGNTOOL_PATH`
- `SMATCHET_SIGN_PFX_PATH`
- `SMATCHET_SIGN_PFX_PASSWORD`
- `SMATCHET_SIGN_CERT_SHA1`
- `SMATCHET_SIGN_CERT_SUBJECT`
- `SMATCHET_SIGN_USE_MACHINE_STORE`
- `SMATCHET_SIGN_TIMESTAMP_URL`

## Release Usage

Sign a local release bundle:

```powershell
.\scripts\publish\release_github.ps1 -Sign -AllowDirty
```

Sign and publish a tagged release:

```powershell
.\scripts\publish\release_github.ps1 -Tag v0.6.7 -Sign -Publish
```

You can also override settings directly on the command line:

```powershell
.\scripts\publish\release_github.ps1 `
  -Tag v0.6.7 `
  -Sign `
  -SigningCertificatePath 'C:\secure\smatchet-signing.pfx' `
  -SigningCertificatePassword 'your-pfx-password' `
  -TimestampUrl 'http://timestamp.digicert.com'
```

## CI (GitHub Actions)

`.github/workflows/release.yml` runs `release_github.ps1 -Sign -Publish` on
every `v*.*.*` tag push. It reads the certificate from two repository
secrets instead of a file on disk:

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

## Notes

- The release script requires exactly one certificate selector: `-SigningCertificatePath`, `-SigningCertificateThumbprint`, or `-SigningCertificateSubject`.
- ZIP files are not Authenticode-signed; the signed binaries live inside the portable ZIP and installer.
- For production distribution, use a real OV/EV code-signing certificate. A self-signed certificate is fine only for local pipeline validation.
- The in-app updater (`AttachmentAppUpdateService::DownloadAndLaunchInstallerUpdate`) runs
  `WinVerifyTrust` on the downloaded installer and refuses to launch it unless the signature
  chains to a trusted root. When validating the update pipeline with a self-signed certificate,
  either import your test root into the machine's trusted-root store, or set
  `SMATCHET_UPDATE_ALLOW_UNSIGNED=1` in the updating client's environment (forgives everything
  except a bad digest — a tampered file is never launched).
