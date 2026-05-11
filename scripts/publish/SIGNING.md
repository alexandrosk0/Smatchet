# Windows Signing

Smatchet's release script can sign the standalone payload and the generated Inno Setup installer.

What gets signed:

- `SmatchetStandalone.exe`
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

## Notes

- The release script requires exactly one certificate selector: `-SigningCertificatePath`, `-SigningCertificateThumbprint`, or `-SigningCertificateSubject`.
- ZIP files are not Authenticode-signed; the signed binaries live inside the portable ZIP and installer.
- For production distribution, use a real OV/EV code-signing certificate. A self-signed certificate is fine only for local pipeline validation.
