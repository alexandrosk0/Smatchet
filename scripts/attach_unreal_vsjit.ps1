param(
    [string]$ProcessName = "UnrealEditor"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-VsWherePath {
    $pf86 = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrWhiteSpace($pf86)) {
        $pf86 = "C:\Program Files (x86)"
    }

    $candidates = @(
        (Join-Path $pf86 "Microsoft Visual Studio\Installer\vswhere.exe"),
        "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    )

    foreach ($path in $candidates) {
        if (Test-Path -Path $path -PathType Leaf) {
            return $path
        }
    }

    return $null
}

function Find-VSJitDebuggerViaVsWhere {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $null
    }

    # Do not pass -requires here: some VS installs omit package IDs expected by strict workloads,
    # but still contain vsjitdebugger.exe when the JIT components are present.
    $found = & $vswhere -latest -products * -prerelease -sort -find "**\vsjitdebugger.exe" 2>$null
    foreach ($line in $found) {
        if (-not [string]::IsNullOrWhiteSpace($line) -and (Test-Path -LiteralPath $line -PathType Leaf)) {
            return $line
        }
    }

    return $null
}

function Find-VSJitDebuggerHeuristic {
    $programFiles = ${env:ProgramFiles}
    if ([string]::IsNullOrWhiteSpace($programFiles)) {
        $programFiles = "C:\Program Files"
    }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrWhiteSpace($programFilesX86)) {
        $programFilesX86 = "C:\Program Files (x86)"
    }

    $roots = @(
        (Join-Path $programFiles "Microsoft Visual Studio"),
        (Join-Path $programFilesX86 "Microsoft Visual Studio")
    )

    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        # Fast path: typical layout for VS2022+
        $years = @("2022", "2026", "2019")
        foreach ($year in $years) {
            $yearRoot = Join-Path $root $year
            if (-not (Test-Path -LiteralPath $yearRoot)) {
                continue
            }

            $editions = @("Community", "Professional", "Enterprise", "Preview", "BuildTools")
            foreach ($edition in $editions) {
                $explicit = Join-Path $yearRoot ($edition + "\Common7\IDE\vsjitdebugger.exe")
                if (Test-Path -LiteralPath $explicit -PathType Leaf) {
                    return $explicit
                }
            }
        }

        # Slower fallback: limited depth search from the VS root (handles nonstandard layouts).
        try {
            $hit = Get-ChildItem -LiteralPath $root -Filter "vsjitdebugger.exe" -File -Recurse -Depth 8 -ErrorAction SilentlyContinue |
                Select-Object -First 1 -ExpandProperty FullName
            if ($hit) {
                return $hit
            }
        } catch {
            # Ignore access-denied / transient enumeration issues.
        }
    }

    return $null
}

function Find-VSJitDebugger {
    $viaVsWhere = Find-VSJitDebuggerViaVsWhere
    if ($viaVsWhere) {
        return $viaVsWhere
    }

    return Find-VSJitDebuggerHeuristic
}

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $process) {
    throw "No running process named '$ProcessName' found. Launch UnrealEditor first (preferably with -WaitForDebugger)."
}

$vsjit = Find-VSJitDebugger
if (-not $vsjit) {
    $msg = @"
Could not find vsjitdebugger.exe.

This tool is optional and may be missing even when Visual Studio is installed.

Fix options:
- Visual Studio Installer -> Modify your VS 2022 instance -> Individual components:
  search for **Just-In-Time debugger** / **Visual Studio Just-In-Time Debugger** and install it,
  or ensure the **Desktop development with C++** workload is fully installed (repair if needed).

Workaround (manual attach):
- Open Visual Studio -> Debug -> Attach to Process... -> select UnrealEditor (PID $($process.Id))

CLI alternatives (if you prefer not to install vsjitdebugger):
- Use WinDbg (`windbg -p $($process.Id)`), or attach from Task Manager -> analyze wait chain -> open debugger if configured.
"@

    throw $msg.Trim()
}

Write-Host "==> Attaching VS JIT debugger"
Write-Host "    Process: $($process.ProcessName) (PID $($process.Id))"
Write-Host "    Debugger: $vsjit"

& $vsjit -p $process.Id
if ($LASTEXITCODE -ne 0) {
    throw "vsjitdebugger returned exit code $LASTEXITCODE"
}
