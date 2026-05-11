param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [string]$ExpectedVersion = "",
    [string]$ExpectedCompanyName = "Smatchet",
    [string]$ExpectedProductName = "Smatchet",
    [string]$ExpectedFileDescription = "Smatchet Standalone"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SmatchetVersionNative {
  [DllImport("version.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern int GetFileVersionInfoSizeW(string lptstrFilename, out int lpdwHandle);
}
'@

function Assert-Equal {
    param(
        [string]$Name,
        [string]$Actual,
        [string]$Expected
    )

    if ([string]::IsNullOrWhiteSpace($Expected)) {
        return
    }
    if ($Actual -ne $Expected) {
        throw "$Name mismatch. Expected '$Expected' but found '$Actual'."
    }
}

$resolvedPath = [System.IO.Path]::GetFullPath($Path)
if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
    throw "File not found: $resolvedPath"
}

$dummy = 0
$nativeSize = [SmatchetVersionNative]::GetFileVersionInfoSizeW($resolvedPath, [ref]$dummy)
if ($nativeSize -le 0) {
    throw "GetFileVersionInfoSizeW returned $nativeSize for $resolvedPath"
}

$versionInfo = (Get-Item -LiteralPath $resolvedPath).VersionInfo
if (-not $versionInfo) {
    throw "No VersionInfo available for $resolvedPath"
}

Assert-Equal -Name "FileVersion" -Actual $versionInfo.FileVersion -Expected $ExpectedVersion
Assert-Equal -Name "ProductVersion" -Actual $versionInfo.ProductVersion -Expected $ExpectedVersion
Assert-Equal -Name "CompanyName" -Actual $versionInfo.CompanyName -Expected $ExpectedCompanyName
Assert-Equal -Name "ProductName" -Actual $versionInfo.ProductName -Expected $ExpectedProductName
Assert-Equal -Name "FileDescription" -Actual $versionInfo.FileDescription -Expected $ExpectedFileDescription

[pscustomobject]@{
    Path = $resolvedPath
    NativeVersionInfoSize = $nativeSize
    FileVersion = $versionInfo.FileVersion
    ProductVersion = $versionInfo.ProductVersion
    CompanyName = $versionInfo.CompanyName
    ProductName = $versionInfo.ProductName
    FileDescription = $versionInfo.FileDescription
    Language = $versionInfo.Language
} | ConvertTo-Json -Depth 4
