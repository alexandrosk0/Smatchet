# Smatchet Context Aggregator for AI Agents
$ProjectRoot = "C:\Dev\Smatchet"

# List the core files an agent needs to understand the architecture
$FilesToCopy = @(
    "CMakeLists.txt",
    "Target_Standalone\main.cpp",
    "Source_Core\include\AppController.h",
    "Source_Core\include\JiraClient.h",
    "Source_Core\include\ConfigManager.h",
    "Source_Core\include\SpreadsheetState.h",
    "Source_Core\src\SmatchetUI.cpp"
)

$AggregatedContext = "Here is the current state of my C++14 Smatchet project:`n`n"

foreach ($File in $FilesToCopy) {
    $FullPath = Join-Path $ProjectRoot $File
    
    if (Test-Path $FullPath) {
        $FileContent = Get-Content $FullPath -Raw
        $AggregatedContext += "=== FILE: $File ===`n"
        $AggregatedContext += "```cpp`n"
        $AggregatedContext += $FileContent
        $AggregatedContext += "`n````n`n"
        Write-Host "Added: $File" -ForegroundColor Cyan
    } else {
        Write-Host "Skipped (Not Found): $File" -ForegroundColor Yellow
    }
}

# Copy the massive string directly to the Windows Clipboard
Set-Clipboard -Value $AggregatedContext

Write-Host "`nSuccess! The project context is now in your clipboard." -ForegroundColor Green
Write-Host "You can hit Ctrl+V in your AI chat right now." -ForegroundColor Green