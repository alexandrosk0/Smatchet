#!/usr/bin/env bash
# scripts/common/unreal-batch.sh — run an Unreal .bat (Build.bat, RunUAT.bat, …)
# from Git Bash with arguments that may contain spaces.
#
# Sourced, not executed. Provides: run_unreal_batch <bat> [args...]
#
# Why this exists: none of the obvious invocations survive the MSYS → CreateProcess
# → cmd.exe hop on Windows.
#
#   cmd.exe /c "<bat>" …    MSYS rewrites the lone `/c` into `C:\`, so cmd.exe gets no
#                           command at all — it prints its banner and exits 0, and the
#                           build silently never runs while the script reports success.
#   cmd.exe //c "<bat>" …   `//c` survives, but cmd's quote-stripping then mangles the
#                           space in "C:\Program Files\Epic Games\…".
#   "<bat>" arg "a b"       Runs the .bat, but any argument containing a space loses its
#                           quoting by the time Build.bat forwards %* to UnrealBuildTool,
#                           which fails with `'C:\Program' is not recognized…`. A .uproject
#                           under "…\Documents\Unreal Projects\…" hits this every time.
#
# So we emit a throwaway .bat holding the fully-quoted command line and run that: the
# quoting is authored in cmd's own syntax and never crosses a shell boundary.

# Usage: run_unreal_batch "<bat path>" [args...]
# Paths (the .bat and any path arguments) must already be Windows-form — pass them
# through winpath first. Returns the batch file's exit code.
run_unreal_batch() {
    local bat="$1"
    shift

    local script
    script="$(mktemp -t smatchet-unreal-XXXXXX.bat)" || {
        echo "run_unreal_batch: could not create a temporary batch file." >&2
        return 1
    }

    {
        printf '@echo off\r\n'
        printf 'call "%s"' "$bat"
        local arg
        for arg in "$@"; do
            printf ' "%s"' "$arg"
        done
        printf '\r\n'
        printf 'exit /b %%ERRORLEVEL%%\r\n'
    } > "$script"

    local rc=0
    "$script" || rc=$?
    rm -f "$script"
    return "$rc"
}
