# Crash capture (when there is NO WER dump)

A Smatchet crash that "vanishes instantly" with **no Windows Error Reporting dump and no Event-1000** is the signature of a *deliberate* `ExitProcess`, not an unhandled fault: the frame-loop SEH filter (`Source/Standalone/SmatchetCrashHandler.cpp` → `SmatchetCrashSehFilter`) catches the access violation, writes its own marker + minidump under `%LOCALAPPDATA%\Smatchet\crashes\`, then `main.cpp`'s `__except` calls `std::exit()`. WER never sees an "unhandled" exception, so it never dumps. (The app's own `pending_crash.dmp` can also be exception-stream-less if an older build's terminate path clobbered it — fixed in #1099, but old archives are still poor.)

Two tools recover the real stack; neither needs admin.

## Capture the death — procdump

Run the target as a child of Sysinternals procdump (`-e` unhandled / `-t` termination / `-ma` full):

```
bash scripts/dev/run-with-procdump.sh
```

For machine-wide auto-capture of *every* future crash, one elevated one-liner:

```
procdump64.exe -accepteula -ma -i %LOCALAPPDATA%\CrashDumps
```

Get procdump from `https://download.sysinternals.com/files/Procdump.zip` (extract `procdump64.exe` to `C:\Tools\Procdump`).

## Get the PRIMARY faulting stack — cdb first-chance

procdump's `-e`/`-t` fire *after* the app's SEH filter already handled the AV, so they capture the `exit → terminate` tail, not the fault. Break on the **first-chance** access violation under `cdb` instead:

```
cdb -y "<build-dir>;srv*C:\Tools\symcache*https://msdl.microsoft.com/download/symbols" \
    -cf scripts/dev/av-capture.cdb <exe>
```

Point `-y` at the build dir for `Smatchet.pdb`. No `cdb` installed? Extract it from the WinDbg MSIX with no install: download `https://aka.ms/windbg/download` (an `.appinstaller` pointing at `windbg.msixbundle`), `Expand-Archive` the bundle → `windbg_win-x64.msix` → `Expand-Archive` again → `amd64/cdb.exe`.

## Reproduce hands-free — per-frame autocycle harness

For a crash that needs a specific UI interaction (e.g. rapid backend/pane switching), drive it from the per-frame path instead of clicking: a `[temp-debug]` block in `SmatchetUI::drawPerFrameTicksAndHandlers` gated on an env var (e.g. `SMATCHET_AUTOCYCLE_PANES`) that cycles the focused pane every N frames is a deterministic regression harness — it both reproduces the crash and proves the fix (run it for hundreds of cycles). Strip the `[temp-debug]` marker before shipping; `run-with-procdump.sh --autocycle-panes 20` wires the env var through.

> Worked example: the #1099 grid-on-backend-switch null-`TrackerField` crash was root-caused with exactly this loop — autocycle + cdb first-chance gave the faulting stack in one run, then the same autocycle (422 switches, zero crashes) validated the fix.
