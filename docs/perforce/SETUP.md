# Perforce — Phase 0 runbook

> Plan: [`docs/design/git-to-perforce-migration.md`](../design/git-to-perforce-migration.md) Phase 0 — Helix Core bring-up.
> Status: **runbook for remote Windows host on LAN**. Smatchet dev box (`Brick`) runs `p4` client only.

## Topology

```
Brick (dev box, this repo)             <REMOTE_HOST> (LAN, Windows)
  ├─ p4.exe + P4V                        ├─ p4d.exe (service, port 1666)
  ├─ C:\Dev\Smatchet (client root)       └─ C:\depot-smatchet\ (server root + db)
  └─ existing p4d on :1666 (Unreal)
```

The dev box already has an unrelated `p4d` on `Brick:1666` serving an Unreal project. That instance stays untouched. Smatchet's depot lives on a separate **remote** Windows host on the same LAN.

## Decisions locked

| Setting | Value | Notes |
|---|---|---|
| Server version | P4D 2025.2 or later | Existing Brick install is 2025.2 — match. |
| Port | `1666` on remote host | Default; no SSL on LAN. |
| Case handling | `sensitive` (`-C0`) | Plan-mandated. **Must** be set at first server start; cannot be flipped later without depot rebuild. |
| Unicode mode | on | `p4d -xi` after init; one-way. |
| Charset | `utf8` | Client sets `P4CHARSET=utf8`. |
| Server root | `C:\depot-smatchet` | Holds `db.*` files + checkpoints. |
| Service name | `p4d_smatchet` | Distinct from any other Perforce service. |

## Pre-flight on the remote host

1. Pick the remote machine's hostname. Verify reachable from Brick:
   ```powershell
   # On Brick:
   Test-NetConnection -ComputerName <REMOTE_HOST> -Port 1666
   ```
   Port 1666 should be **closed** before install (no service yet). Reachability of the host itself is what matters.

2. On the remote host, confirm no existing Perforce server on 1666:
   ```powershell
   Get-Service | Where-Object { $_.Name -like '*p4d*' -or $_.Name -like '*Perforce*' }
   netstat -ano | findstr :1666
   ```
   Expect: empty.

3. Disk space: 1 GB free under `C:\depot-smatchet` is plenty for the agentic-WIP use case.

## Install — on the remote host

Run all commands in an elevated PowerShell unless noted.

### 1. Install Helix Core Server

Download from <https://www.perforce.com/downloads/helix-core-server> — the Windows installer. Match the **2025.2** LTS line.

During install:
- **Install type**: Server (not "client only").
- **Server root**: `C:\depot-smatchet`.
- **Port**: `1666`.
- **Service account**: `LocalSystem` (default) is fine on a single-user LAN box.
- **Do NOT let the installer create a sample workspace** — we'll do that step on Brick.

The installer registers `Perforce` Windows service by default. **Stop and disable it** — we want a custom-named service with the right flags:

```powershell
Stop-Service -Name 'Perforce' -ErrorAction SilentlyContinue
Set-Service -Name 'Perforce' -StartupType Disabled
sc.exe delete Perforce   # removes the installer's default service
```

### 2. Wipe the installer's db files (we re-init with the right flags)

The installer may have started the server once during setup, which writes `db.*` files with case-**insensitive** flag (Windows default). Delete them so the next start locks in case-sensitive:

```powershell
Stop-Service -Name 'Perforce' -ErrorAction SilentlyContinue
Remove-Item C:\depot-smatchet\db.* -Force -ErrorAction SilentlyContinue
Remove-Item C:\depot-smatchet\journal -Force -ErrorAction SilentlyContinue
Remove-Item C:\depot-smatchet\log -Force -ErrorAction SilentlyContinue
```

### 3. Initialize case-sensitive + unicode

```powershell
& 'C:\Program Files\Perforce\Server\p4d.exe' -r C:\depot-smatchet -C0 -xi
```

Expected output:
```
Perforce db files in 'C:\depot-smatchet' will be created if missing...
Server switched to Unicode mode.
```

The `-C0` arg on first run locks the server into case-sensitive mode. The `-xi` flag flips unicode mode.

### 4. Register Windows service `p4d_smatchet`

Use `p4s.exe` (the Windows service wrapper that ships with Helix Core 2025.x), **not** `p4d.exe` directly. `p4d.exe` is the command-line server and has no SCM dispatcher — registering it as a Windows service produces a `[SC] StartService failed` error on start. `p4s.exe` reads its config from per-service Perforce env vars (`P4ROOT` / `P4PORT` / `P4LOG` / `P4JOURNAL`) set via `p4 set -S <service>`.

```powershell
# Create the service first (it WILL fail to start until the per-service env vars are set below).
New-Service -Name 'p4d_smatchet' `
  -BinaryPathName '"C:\Program Files\Perforce\Server\p4s.exe"' `
  -DisplayName 'Perforce Server (Smatchet)' `
  -Description 'Helix Core server hosting //smatchet depot for the Smatchet project.' `
  -StartupType Automatic

# Set per-service env vars (the service uses these to locate the depot, port, and logs).
& 'C:\Program Files\Perforce\p4.exe' set -S p4d_smatchet P4ROOT=C:\depot-smatchet
& 'C:\Program Files\Perforce\p4.exe' set -S p4d_smatchet P4PORT=1666
& 'C:\Program Files\Perforce\p4.exe' set -S p4d_smatchet P4LOG=C:\depot-smatchet\p4d.log
& 'C:\Program Files\Perforce\p4.exe' set -S p4d_smatchet P4JOURNAL=C:\depot-smatchet\journal

Start-Service -Name 'p4d_smatchet'
```

Verify:
```powershell
Get-Service p4d_smatchet                  # Status: Running
Test-NetConnection -ComputerName localhost -Port 1666
& 'C:\Program Files\Perforce\p4.exe' set -S p4d_smatchet   # should list P4ROOT/P4PORT/P4LOG/P4JOURNAL
```

If `Start-Service` fails with "Cannot start service", double-check `p4 set -S p4d_smatchet` shows all four env vars. Per-service config lives in the registry under `HKLM\SOFTWARE\[Wow6432Node\]Perforce` and is **bound to the service name** — deleting the service drops its per-service config. If you need to rename the service later, re-add the four `p4 set -S <new-name> ...` lines before starting.

### 5. Configure user `alexk`

From the remote host (or from Brick once firewall allows):

```powershell
& 'C:\Program Files\Perforce\p4.exe' -p localhost:1666 -C utf8 user -f -i <<'EOF'
User: alexk
Email: alexkonstantonis@gmail.com
FullName: Alex Konstantonis
EOF
```

Verify:
```powershell
& 'C:\Program Files\Perforce\p4.exe' -p localhost:1666 -C utf8 users
# alexk <alexkonstantonis@gmail.com> (Alex Konstantonis) accessed <date>
```

### 6. Install typemap

```powershell
$typemap = @'
TypeMap:
	binary+w //....png
	binary+w //....jpg
	binary+w //....jpeg
	binary+w //....dll
	binary+w //....exe
	binary+w //....lib
	binary+w //....pdb
	binary+w //....ico
	binary+w //....ttf
	binary+w //....otf
	text+x //....sh
	text+x //....py
	text+x //....ps1
	unicode //....json
	unicode //....md
	unicode //....yml
	unicode //....yaml
	text+w //....cpp
	text+w //....h
	text+w //....hpp
	text+w //....c
	text+w //....cc
	text+w //....cmake
	text+w //....lua
	text+w //....txt
'@
$typemap | & 'C:\Program Files\Perforce\p4.exe' -p localhost:1666 -C utf8 typemap -i
```

Expected: `Typemap saved.`

### 7. Open the Windows Firewall on port 1666 (LAN only)

```powershell
New-NetFirewallRule -DisplayName 'Perforce p4d_smatchet (LAN)' `
  -Direction Inbound -Protocol TCP -LocalPort 1666 -Action Allow `
  -Profile Private,Domain
```

`Public` profile excluded — server must not be reachable from non-LAN networks.

### 8. Daily checkpoint Scheduled Task

```powershell
$action = New-ScheduledTaskAction -Execute 'C:\Program Files\Perforce\Server\p4d.exe' `
  -Argument '-r C:\depot-smatchet -jc'
$trigger = New-ScheduledTaskTrigger -Daily -At 3am
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
Register-ScheduledTask -TaskName 'p4d_smatchet_checkpoint' `
  -Action $action -Trigger $trigger -Principal $principal `
  -Description 'Daily Smatchet Perforce depot checkpoint.'
```

Verify next-run:
```powershell
Get-ScheduledTask p4d_smatchet_checkpoint | Get-ScheduledTaskInfo
```

Offsite checkpoint copy (OneDrive / NAS) is **recommended but not blocking** — plan rationale: git stays canonical.

## Smoke test from Brick

After the remote install completes, from `C:\Dev\Smatchet` on Brick:

```bash
P4PORT=<REMOTE_HOST>:1666 P4USER=alexk P4CHARSET=utf8 \
  "/c/Program Files/Perforce/p4.exe" info
```

Expected:
```
Server address: <REMOTE_HOST>:1666
Server root: C:\depot-smatchet
Server version: P4D/NTX64/2025.2/...
Case Handling: sensitive
```

If `Case Handling` reads `insensitive` — STOP, the `-C0` flag did not take. Wipe `C:\depot-smatchet\db.*` on remote and redo step 3.

## Phase 0 exit gate — throwaway CL roundtrip

Phase 0 closes when a throwaway client can submit + re-sync a file. This step gets executed in **Phase 1** as part of the baseline import — `//smatchet/main` does not exist yet at Phase 0 close. Phase 0 closes with the smoke test above.

## Deviations from plan

- **Two p4d hosts** instead of one — the dev box's existing `Brick:1666` (Unreal) is left untouched; Smatchet's depot lives on a remote LAN host. Plan § Approach assumed one combined `p4d`; the split is operationally cleaner and matches user preference.
- **Case sensitivity**: server initialized case-sensitive per plan. Windows default would have been insensitive.
- **Server version**: 2025.2 (latest LTS available 2026-05-21) instead of plan's `r24.2`.
- **Service wrapper**: original runbook prescribed `sc.exe create … binPath= "…\p4d.exe -r … -p … -L …"` (direct `p4d.exe` invocation). On Helix Core 2025.2 that registration succeeds but `Start-Service` fails — `p4d.exe` has no SCM dispatcher. The runbook above is the corrected version using `p4s.exe` + per-service env vars (`p4 set -S p4d_smatchet …`).
- **Existing Mainbot install (2026-05-22)**: actual server root is `C:\depot\` (Helix installer default) instead of the runbook's `C:\depot-smatchet\`, and case handling is **insensitive** (the `-C0` initialization step was skipped on first start, and the depot has accumulated data since — cannot be flipped to sensitive without a full backup-wipe-replay rebuild). Service was renamed from the installer-default `Perforce` to `p4d_smatchet` in-place; per-service env vars point at `C:\depot\`. Doc keeps the original `C:\depot-smatchet\` path as the canonical fresh-install target.

## Open items (post-Phase-0)

- Remote `<REMOTE_HOST>` to be supplied by user post-install.
- Brick-side `P4PORT` default — set via `p4 set -s P4PORT=<REMOTE_HOST>:1666` (system-wide) after smoke test passes.
- Phase 1 stream depot `//smatchet` creation depends on Phase 0 close.
