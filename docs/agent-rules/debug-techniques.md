# Debug techniques (load on-demand)

Trigger: **debugging** a visual / behavioural issue. These project-wide techniques are mandatory whenever they apply. The full behavioural-bug investigation loop is `agents/core/debug-detective.md` + its `debug-instrument` skill; this doc holds the two standalone techniques AGENTS.md used to inline.

## Pink-clear UI gap detection

For "is the background ever visible behind panels?" / "are dock gaps still leaking?" questions, set the clear color to magenta (`glClearColor(1.0f, 0.0f, 1.0f, 1.0f)` on Standalone, equivalent `ClearRenderTargetView` color on DX12). Any visible pink is a guaranteed dock gap or transparent region. Pair with a screenshot + per-pixel pink scan for objective regression tests.

## Exe staleness check

After every rebuild, `ls -la` both the patched output and the most-likely-stale exe paths side-by-side, compare mtimes, and name the **exact** path the user should run. Multiple build outputs (`build/ninja-iter-msvc/`, `build/ninja-debug-msvc/`, `build/ninja-publish-msvc/`, worktree builds) make wrong-exe testing a common time-sink — orchestrator + perf / build agents all enforce this.

## Crash capture (when there is NO WER dump)

A Smatchet crash that "vanishes instantly" with **no Windows Error Reporting dump and no Event-1000** is the signature of a *deliberate* `ExitProcess`, not an unhandled fault: the frame-loop SEH filter (`Source/Standalone/SmatchetCrashHandler.cpp` → `SmatchetCrashSehFilter`) catches the access violation, writes its own marker + minidump under `%LOCALAPPDATA%\Smatchet\crashes\`, then `main.cpp`'s `__except` calls `std::exit()`. WER never sees an "unhandled" exception, so it never dumps. (The app's own `pending_crash.dmp` can also be exception-stream-less if an older build's terminate path clobbered it — fixed, but old archives are still poor.) Two tools recover the real stack; neither needs admin:

- **Capture the death** — run under procdump (`-e` unhandled / `-t` termination / `-ma` full): `pwsh scripts/dev/run-with-procdump.ps1`. For machine-wide auto-capture of *every* future crash, one elevated one-liner: `procdump64.exe -accepteula -ma -i %LOCALAPPDATA%\CrashDumps`. Get procdump from `https://download.sysinternals.com/files/Procdump.zip`.
- **Get the PRIMARY faulting stack** — procdump's `-e`/`-t` fire *after* the app's SEH filter already handled the AV, so they capture the `exit→terminate` tail, not the fault. Break on the **first-chance** AV under `cdb` instead: `cdb -y "<build-dir>;srv*C:\Tools\symcache*https://msdl.microsoft.com/download/symbols" -cf scripts/dev/av-capture.cdb <exe>`. No `cdb` installed? Extract it from the WinDbg MSIX with no install: download `https://aka.ms/windbg/download` (an `.appinstaller` pointing at `windbg.msixbundle`), `Expand-Archive` the bundle → `windbg_win-x64.msix` → `Expand-Archive` again → `amd64/cdb.exe`. Point `-y` symbol path at the build dir for `Smatchet.pdb`.
- **Reproduce hands-free** — for a crash that needs a specific UI interaction (e.g. rapid backend/pane switching), drive it from the per-frame path instead of clicking: a `[temp-debug]` block in `SmatchetUI::drawPerFrameTicksAndHandlers` gated on an env var (e.g. `SMATCHET_AUTOCYCLE_PANES`) that cycles the focused pane every N frames is a deterministic regression harness — it both reproduces the crash and proves the fix (run it for hundreds of cycles). Strip the `[temp-debug]` marker before shipping; `run-with-procdump.ps1 -AutocyclePanes 20` wires the env var through.
