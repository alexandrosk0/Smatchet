# Perforce dual-VCS setup runbook

> **Plan**: [`docs/design/git-to-perforce-migration.md`](../design/git-to-perforce-migration.md) Phase 0 + Phase 1.
> **Scope**: Windows dev host. One-time setup. Reproducible from a fresh `p4d` install. Single-user (free-tier license).
> **Time budget**: ~15 minutes after `p4d` is installed.

The Smatchet repo lives in two parallel VCS layers: git/GitHub is canonical (ship-line, PR review, CI), Perforce is an opt-in local layer for agentic-WIP primitives (named shelves, exclusive locks, atomic counters, task streams). This doc walks through the one-time server bring-up + workspace setup so an agent that opts into `SMATCHET_AGENT_VCS=p4` can immediately use the p4 side.

## 0. Prerequisites

- Windows 10/11 with admin rights for the initial `p4d` service install.
- Helix Core download: <https://www.perforce.com/downloads/helix-core-p4d>. Pick the Windows installer; install both `p4d` (server) AND `p4` (CLI) AND P4V (GUI; optional).
- A git checkout of Smatchet at the **canonical client root**. This doc uses `C:\Development\Smatchet` — substitute your actual path everywhere below.

Verify install:

```powershell
Get-Service Perforce*                                # service running?
Get-NetTCPConnection -LocalPort 1666                 # listening on 1666?
& "C:\Program Files\Perforce\p4.exe" -V              # CLI present?
```

If `p4.exe` isn't on PATH, prepend it (or set an alias). All commands below assume `$p4 = "C:\Program Files\Perforce\p4.exe"`.

## 1. Persistent environment (one-time, per Windows user)

```powershell
[System.Environment]::SetEnvironmentVariable("P4PORT",   "localhost:1666",       "User")
[System.Environment]::SetEnvironmentVariable("P4USER",   "alexk",                "User")  # your Windows username
[System.Environment]::SetEnvironmentVariable("P4CLIENT", "smatchet_main_alexk",  "User")
[System.Environment]::SetEnvironmentVariable("P4IGNORE", ".p4ignore",            "User")
```

Restart any open terminals to pick up the new values. Bash sessions (MSYS2 / Git Bash) inherit user-scope env on launch.

## 2. Update the auto-created user (Phase 0 Step 3)

`p4d` auto-creates a `standard` user matching your Windows login on first connect. Update the email + full name so commits attribute correctly:

```powershell
$p4 = "C:\Program Files\Perforce\p4.exe"
$spec = & $p4 user -o $env:P4USER
$spec = $spec -replace 'Email:\s+\S+', "Email:`talexkonstantonis@gmail.com"   # use your git author email
$spec = $spec -replace 'FullName:\s+\S+', "FullName:`tAlexandros Konstantonis" # use your git author name

# UTF-8 no-BOM is required; PS 5.1 native pipe adds a BOM
$tmp = [System.IO.Path]::GetTempFileName()
[System.IO.File]::WriteAllBytes($tmp, [System.Text.Encoding]::UTF8.GetBytes(($spec -join "`r`n")))
cmd.exe /c "`"$p4`" user -i < `"$tmp`""
Remove-Item $tmp
```

Verify: `p4 user -o $env:P4USER | Select-String "^(Email|FullName):"`.

## 3. Create the stream depot + mainline stream (Phase 1 Steps 1-2)

```powershell
function Submit-P4Spec {
    param([string]$Command, [string]$SpecText)
    $p4 = "C:\Program Files\Perforce\p4.exe"
    $tmp = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllBytes($tmp, [System.Text.Encoding]::UTF8.GetBytes(($SpecText -replace "`r?`n", "`r`n")))
    cmd.exe /c "`"$p4`" $Command -i < `"$tmp`""
    Remove-Item $tmp
}

Submit-P4Spec -Command "depot" -SpecText @"
Depot:       smatchet
Owner:       alexk
Type:        stream
Address:     local
Suffix:      .p4s
StreamDepth: //smatchet/1
Map:         smatchet/...
"@

Submit-P4Spec -Command "stream" -SpecText @"
Stream:     //smatchet/main
Owner:      alexk
Name:       main
Parent:     none
Type:       mainline
Options:    allsubmit unlocked notoparent nofromparent mergedown
ParentView: inherit
Paths:      share ...
"@
```

`ParentView: inherit` is required as of p4d 2025.x — older docs may omit it. The `StreamDepth: //smatchet/1` declares that `//smatchet/<NAME>` is the stream name level (depth 1).

## 4. Create the client workspace rooted at the existing git tree

```powershell
Submit-P4Spec -Command "client" -SpecText @"
Client:        smatchet_main_alexk
Owner:         alexk
Host:          $env:COMPUTERNAME
Description:   Smatchet mainline workspace (dual-VCS).
Root:          C:\Development\Smatchet
Options:       noallwrite noclobber nocompress unlocked nomodtime normdir
SubmitOptions: submitunchanged
LineEnd:       local
Stream:        //smatchet/main
"@
```

Substitute `Root:` with your actual checkout path. The client is **stream-bound** (`Stream:` field) so its view auto-derives from `//smatchet/main`'s paths.

## 5. Configure the typemap — **`+w` modifier is critical** (Phase 0 Step 5)

The plan originally listed bare `text` types in the typemap. That is **wrong** for a dual-VCS workspace: p4 sets Windows `ReadOnly` on every non-`+w` text file post-submit, which breaks the git-side edit-from-either-side contract (next `Edit` fails with `EPERM`). Always use `text+w` for source files and `text+wx` for scripts:

```powershell
Submit-P4Spec -Command "typemap" -SpecText @"
TypeMap:
        text+w   //...
        binary+w //....png
        binary+w //....jpg
        binary+w //....jpeg
        binary+w //....gif
        binary+w //....ico
        binary+w //....ttf
        binary+w //....otf
        binary+w //....dll
        binary+w //....exe
        binary+w //....lib
        binary+w //....pdb
        binary+w //....so
        binary+w //....dylib
        binary+w //....zip
        binary+w //....7z
        binary+w //....tar
        binary+w //....gz
        binary+w //....wav
        binary+w //....mp3
        binary+w //....mp4
        text+wx  //....sh
        text+wx  //....py
        text+wx  //....bat
        text+wx  //....cmd
        text+wx  //....ps1
        text+wx  //....bats
        text+wx  //....bash
"@
```

The `text+w //...` catch-all is the load-bearing line: any extension not enumerated below it falls through to `text+w` (writable text) on `p4 add` — so `.tmpl`, `.mdc`, `.uplugin`, `LICENSE`, `pre-push`, `.clang-format`, etc. all get the safe default.

Per p4 typemap precedence: **the last matching entry wins**. The catch-all comes first; binary + script overrides come after.

## 6. Baseline import (Phase 1 Step 4)

The `.p4ignore` at repo root (shipped via PR #374) governs which files get reconciled. Pre-flight with a dry run to confirm leakage is zero:

```powershell
Set-Location C:\Development\Smatchet
& $p4 reconcile -n //smatchet/main/... 2>&1 | Measure-Object   # expect ~820 ops
& $p4 reconcile -n //smatchet/main/... 2>&1 | Select-String '\\\.git\\|\\build\\|\\\.claude\\'   # expect 0 matches
```

If `0 matches` for the leakage check, submit:

```powershell
$sha = git rev-parse HEAD
$branch = git rev-parse --abbrev-ref HEAD
& $p4 reconcile //smatchet/main/...
& $p4 submit -d "chore(p4): baseline import from git $branch@$sha"
```

Expected: ~820 files, ~9 MB.

## 7. Verify (Phase 1 Step 5)

```powershell
& $p4 opened                                       # "File(s) not opened on this client."
& $p4 reconcile -n //smatchet/main/... 2>&1        # "no file(s) to reconcile."
git status --porcelain                             # empty (except any pre-existing untracked you have)
```

Edit-driven round-trip:

```powershell
"probe $(Get-Date -Format o)" | Out-File C:\Development\Smatchet\.p4-roundtrip-probe.txt
git status --porcelain | Select-String 'probe'                                    # ?? line
& $p4 reconcile -n //smatchet/main/.p4-roundtrip-probe.txt 2>&1                   # opened for add
Remove-Item C:\Development\Smatchet\.p4-roundtrip-probe.txt
```

Both VCSes must flag the probe; clean-up must restore empty state on both sides.

## 8. Deviations from the plan locked in here

These are deviations from `docs/design/git-to-perforce-migration.md` § Phase 0 that this runbook codifies:

- **`+w` modifier mandatory on every text type** (see § 5). Bare `text` breaks dual-VCS.
- **`case-sensitive`**: kept as the Windows-default `insensitive` (the plan listed an invalid `=2` value). Case-sensitive mode is incompatible with NTFS dual-VCS.
- **`unicode` mode**: skipped (one-way switch, marginal benefit for single-user ASCII-safe-UTF-8 content). Re-enable later via `p4d -r c:\depot -xi` while service is stopped if needed.
- **Daily checkpoint scheduled task** (Phase 0 Step 6): not automated yet. Manual command for now:
  ```powershell
  Stop-Service Perforce; & "C:\Program Files\Perforce\p4d.exe" -r C:\depot -jc; Start-Service Perforce
  ```
  Tracked for automation in `docs/backlog/agent-self-improvement/tooling.md`.

## 9. Troubleshooting

**`Edit` fails with `EPERM`**: a text file in the depot doesn't have `+w`. Fix once per file with `p4 edit -t text+w <file> && p4 submit -d "retype"`, or globally with `attrib -R /S /D C:\Development\Smatchet\*.*` + re-submit typemap with the `text+w //...` catch-all per § 5.

**`Can't clobber writable file <path>` on `p4 delete` / `p4 sync`**: p4 refuses to remove a writable on-disk file. Use `p4 delete -k <path>` (delete depot rev, keep workspace file), or `p4 sync -f <path>` to force overwrite. For deletion, also `Remove-Item <path>` from the workspace afterward.

**`p4 reconcile` picks up a file that should be ignored**: `.p4ignore` follows gitignore syntax but **anchored patterns require a leading `/`**. `*.log` matches `foo.log` but not a bare-name `log`. Use `/log` (root-anchored) for the latter. After updating `.p4ignore`, the change applies to future reconciles — already-imported files need `p4 delete -k` + submit.

**`Missing required field 'ParentView'`** when creating a stream: p4d 2025.x requires `ParentView: inherit` even on mainline streams. Add it to the spec; re-submit.

**`Syntax error in '﻿'` when piping a spec via PowerShell**: PS 5.1's `Out-File -Encoding utf8` adds a BOM. Use the temp-file pattern in `Submit-P4Spec` above (`[System.IO.File]::WriteAllBytes` + `[System.Text.Encoding]::UTF8` produces no-BOM UTF-8) and read via `cmd.exe /c "<exe> -i < <file>"`.

## 10. Daily ops (deferred to Phase 5+)

These are placeholders — none of them are required for the dual-VCS bootstrap to work. They'll get their own scripts as later phases of the plan ship:

- `scripts/dev/p4-task-stream.sh` — allocate a `//smatchet/tasks/<agent-id>` child stream + populated workspace (Phase 2).
- `scripts/dev/p4-task-stream-gc.sh` — periodic prune of stale task streams (Phase 2).
- `scripts/dev/p4-reconcile-check.sh` — `pre-push` hook helper to fail-fast when p4 has uncommitted work git doesn't know about (Phase 1 hook layer).
- Daily `p4d -jc` checkpoint via Windows Scheduled Task (Phase 0 Step 6 deferred).
