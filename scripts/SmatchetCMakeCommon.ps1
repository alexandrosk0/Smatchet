# Shared CMake preset helpers for Smatchet PowerShell scripts.
# Dot-source from repo scripts/: . (Join-Path $PSScriptRoot "SmatchetCMakeCommon.ps1")

function Get-SmatchetConfigurePresetBinaryDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$PresetName
    )
    $files = @(
        (Join-Path $Root "CMakePresets.json"),
        (Join-Path $Root "CMakeUserPresets.json")
    )
    foreach ($path in $files) {
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }
        $doc = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        foreach ($p in $doc.configurePresets) {
            if ((Get-Member -InputObject $p -Name "name" -ErrorAction SilentlyContinue) -and
                ($p.name -ceq $PresetName) -and
                (Get-Member -InputObject $p -Name "binaryDir" -ErrorAction SilentlyContinue) -and
                $p.binaryDir) {
                return ($p.binaryDir -replace '\$\{sourceDir\}', $Root)
            }
        }
    }
    return (Join-Path $Root "build\$PresetName")
}
