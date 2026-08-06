# Installer Smoke Test

This guide documents the Windows installer smoke test used for Smatchet release validation and provides the exact scripts to run it repeatably.

## What the smoke test covers

- release assets exist
- portable Windows ZIP contains the expected standalone exe
- Windows version metadata is readable from the packaged exe
- installer runs silently
- installed `Smatchet.exe` exists and launches
- uninstall registration is created
- installed uninstaller exists
- install directory is removed on uninstall
- user data is written under `%LocalAppData%\Smatchet`
- Unreal plugin ZIP contains synced plugin version metadata
- Fab submission bundle contains synced version metadata

## Scripts

- `scripts/publish/test-windows-version-info.py`
- `scripts/publish/test-installer-smoke.sh`
- `scripts/publish/test-release-smoke.sh`

## Prerequisites

- Windows
- Git Bash (ships with Git for Windows) and Python 3
- a built release bundle from `scripts/publish/release-github.sh`
- if you want signature checks:
  - signed installer and app payload
  - a certificate chain trusted by the machine, or accept that a temporary/self-signed cert will report as present but untrusted

## Typical workflow

Build a release bundle first:

```bash
bash scripts/publish/release-github.sh --allow-dirty
```

Then run the full release smoke test:

```bash
bash scripts/publish/test-release-smoke.sh --release-dir out/releases/v0.6.7-local-20260510-152130 --expected-version 0.6.7
```

If you also want Authenticode checks:

```bash
bash scripts/publish/test-release-smoke.sh --release-dir out/releases/v0.6.7-local-20260510-152130 --expected-version 0.6.7 --check-installer-signatures
```

## Individual checks

Validate one exe's Windows version resource:

```bash
python scripts/publish/test-windows-version-info.py build/ninja-publish-msvc/Smatchet.exe --expected-version 0.6.7
```

Run just the installer smoke test:

```bash
bash scripts/publish/test-installer-smoke.sh --release-dir out/releases/v0.6.7-local-20260510-152130 --expected-version 0.6.7
```

Or point directly at an installer:

```bash
bash scripts/publish/test-installer-smoke.sh --installer-path out/releases/v0.6.7-local-20260510-152130/assets/Smatchet-v0.6.7-local-20260510-152130-windows-setup.exe --expected-version 0.6.7
```

## Important options

- `--clear-user-data-before-install`
  - deletes `%LocalAppData%\Smatchet` before the test so the run starts from a clean user-data state
- `--keep-user-data-after-test`
  - leaves `%LocalAppData%\Smatchet` behind for inspection after the run
- `--check-installer-signatures` (`--check-signatures` on `test-installer-smoke.sh`)
  - checks signatures on the installer, installed app exe, and installed uninstaller

## Expected results

Success returns JSON that includes:

- release directory
- portable exe version info
- installed exe version info
- plugin version
- Fab submission version
- user-data files observed after launch/uninstall

The scripts exit non-zero immediately on hard failures such as:

- missing assets
- unreadable Windows version info
- missing installed exe
- missing uninstall entry
- install directory still present after uninstall
- version mismatch between expected and packaged artifacts

## Notes

- `test-installer-smoke.sh` uses silent install and uninstall switches:
  - `/VERYSILENT`
  - `/SUPPRESSMSGBOXES`
  - `/NORESTART`
- user data is expected to remain after uninstall unless you remove it explicitly
- signature checks verify signature presence and report status; a self-signed cert will still show as untrusted
