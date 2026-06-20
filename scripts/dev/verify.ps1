<#.
    Build a Smatchet target, then run the CI delta-lint gates the bare build skips.

    Closes the "build-verify shortcut bypasses the lint gate" gap: a plain
    `cmake --build` (or build_and_run.ps1 -BuildOnly) proves the code compiles but
    does NOT run the comment-noise / function-size / strict-zone delta gates that
    CI enforces. This wrapper builds first, then runs exactly those gates against
    the merge-base with <Base> (default origin/develop) so a comment-blank-run or
    a clang-format-reflowed comment is caught locally, not three minutes later in CI.

    Steps:
      1. scripts/dev/local/build_and_run.ps1 -BuildOnly  (compile only; no app launch)
      2. comment_audit.py --diff <Base>                  (comment-noise delta)
      3. test-lint-rules.sh --diff <Base>                (full delta lint gate)

    Examples:
      .\scripts\dev\verify.ps1
      .\scripts\dev\verify.ps1 -Preset ninja-iter-msvc -Target SmatchetStandalone
      .\scripts\dev\verify.ps1 -Base origin/develop
      .\scripts\dev\verify.ps1 -SkipBuild        # lint gates only (no compile)

    Exit codes: 0 = build + delta-lint clean; non-zero = build or a gate failed.

    NOTE: the delta gates are bash scripts; this wrapper requires `bash` on PATH
    (Git Bash ships it). For the full pre-push gate (clang-format -i, markdown lint,
    doc-validation, review ack) use `bash scripts/dev/pre-ship.sh` instead - this
    wrapper is the build-then-delta-lint shortcut, not a pre-ship replacement.

    NOTE: kept ASCII-only on purpose - Windows PowerShell 5.1 reads a BOM-less .ps1
    as the system codepage, so a non-ASCII char in a code-level string corrupts the
    parse. Do not add em-dashes / smart quotes to executable lines.
#>
param(
    [string]$Preset = "ninja-iter-msvc",
    [string]$Target = "SmatchetStandalone",
    [string]$Base = "origin/develop",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not (Get-Command "bash" -ErrorAction SilentlyContinue)) {
    throw "bash not found on PATH - the delta-lint gates are bash scripts (Git Bash provides bash)."
}

# 1) Build only (no app launch). Reuses the existing configure-skip + MSVC env logic.
if (-not $SkipBuild) {
    $buildScript = Join-Path $repoRoot "scripts\dev\local\build_and_run.ps1"
    Write-Host "verify: building $Target ($Preset) ..."
    & $buildScript -Preset $Preset -Target $Target -BuildOnly
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        throw "verify: build failed (exit $LASTEXITCODE)."
    }
}
else {
    Write-Host "verify: -SkipBuild set - running delta-lint gates only."
}

Push-Location $repoRoot
try {
    # 2) Comment-noise delta gate (exit 1 = violations, >=2 = infra failure).
    Write-Host "verify: comment-noise delta gate vs $Base ..."
    & python "agents/scripts/core/comment_audit.py" --diff $Base
    $commentRc = $LASTEXITCODE
    if ($commentRc -ne 0) {
        throw "verify: comment-noise delta gate found violations (exit $commentRc) - fix before pushing."
    }

    # 3) Full delta lint gate (comment-noise + function-size + strict-zone).
    Write-Host "verify: delta lint gate (test-lint-rules.sh) vs $Base ..."
    & bash "agents/scripts/project/test-lint-rules.sh" --diff $Base
    $lintRc = $LASTEXITCODE
    if ($lintRc -ne 0) {
        throw "verify: delta lint gate failed (exit $lintRc) - fix before pushing."
    }
}
finally {
    Pop-Location
}

Write-Host "verify: PASS - build green + comment-noise + delta lint gate clean."
