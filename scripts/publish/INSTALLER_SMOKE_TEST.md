# Installer Smoke Test

This guide documents the Windows installer smoke test used for Smatchet release validation and provides the exact scripts to run it repeatably.

## What the smoke test covers

- release assets exist
- portable Windows ZIP contains the expected standalone exe
- Windows version metadata is readable from the packaged exe
- installer runs silently
- installed `SmatchetStandalone.exe` exists and launches
- uninstall registration is created
- installed uninstaller exists
- install directory is removed on uninstall
- user data is written under `%LocalAppData%\Smatchet`
- Unreal plugin ZIP contains synced plugin version metadata
- Fab submission bundle contains synced version metadata

## Scripts

- `scripts\publish\test_windows_version_info.ps1`
- `scripts\publish\test_installer_smoke.ps1`
- `scripts\publish\test_release_smoke.ps1`

## Prerequisites

- Windows
- PowerShell
- a built release bundle from `scripts\publish\release_github.ps1`
- if you want signature checks:
  - signed installer and app payload
  - a certificate chain trusted by the machine, or accept that a temporary/self-signed cert will report as present but untrusted

## Typical workflow

Build a release bundle first:

```powershell
.\scripts\publish\release_github.ps1 -AllowDirty
```

Then run the full release smoke test:

```powershell
.\scripts\publish\test_release_smoke.ps1 `
  -ReleaseDir .\out\releases\v0.6.7-local-20260510-152130 `
  -ExpectedVersion 0.6.7
```

If you also want Authenticode checks:

```powershell
.\scripts\publish\test_release_smoke.ps1 `
  -ReleaseDir .\out\releases\v0.6.7-local-20260510-152130 `
  -ExpectedVersion 0.6.7 `
  -CheckInstallerSignatures
```

## Individual checks

Validate one exe's Windows version resource:

```powershell
.\scripts\publish\test_windows_version_info.ps1 `
  -Path .\build\ninja-publish-msys2\SmatchetStandalone.exe `
  -ExpectedVersion 0.6.7
```

Run just the installer smoke test:

```powershell
.\scripts\publish\test_installer_smoke.ps1 `
  -ReleaseDir .\out\releases\v0.6.7-local-20260510-152130 `
  -ExpectedVersion 0.6.7
```

Or point directly at an installer:

```powershell
.\scripts\publish\test_installer_smoke.ps1 `
  -InstallerPath .\out\releases\v0.6.7-local-20260510-152130\assets\Smatchet-v0.6.7-local-20260510-152130-windows-setup.exe `
  -ExpectedVersion 0.6.7
```

## Important options

- `-ClearUserDataBeforeInstall`
  - deletes `%LocalAppData%\Smatchet` before the test so the run starts from a clean user-data state
- `-KeepUserDataAfterTest`
  - leaves `%LocalAppData%\Smatchet` behind for inspection after the run
- `-CheckInstallerSignatures`
  - checks signatures on the installer, installed app exe, and installed uninstaller

## Expected results

Success returns JSON that includes:

- release directory
- portable exe version info
- installed exe version info
- plugin version
- Fab submission version
- user-data files observed after launch/uninstall

The scripts throw immediately on hard failures such as:

- missing assets
- unreadable Windows version info
- missing installed exe
- missing uninstall entry
- install directory still present after uninstall
- version mismatch between expected and packaged artifacts

## Notes

- `test_installer_smoke.ps1` uses silent install and uninstall switches:
  - `/VERYSILENT`
  - `/SUPPRESSMSGBOXES`
  - `/NORESTART`
- user data is expected to remain after uninstall unless you remove it explicitly
- signature checks verify signature presence and report status; a self-signed cert will still show as untrusted
