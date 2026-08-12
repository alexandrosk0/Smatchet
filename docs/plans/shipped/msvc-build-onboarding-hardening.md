# Plan - MSVC build onboarding hardening
<!-- plan-date: 2026-05-28 -->

> **Status**: shipped — archived 2026-06-06; post-ship sections populated and cited PRs merged (see § Implementation log).
>
> **Slug**: `msvc-build-onboarding-hardening`

## Context

Recent build onboarding friction came from a mismatch between the current MSVC-first presets and older MSYS2-era instructions, plus wrapper scripts that still assume the caller has already opened a Visual Studio Developer Prompt. After this lands, a Windows user can build and run the standalone app from a normal PowerShell with one documented command, and the repository explicitly says MSYS2 is not required or proposed for building Smatchet.

## Approach

Make `scripts/dev/build_and_run.ps1 -Preset ninja-iter-msvc` the blessed local build/run entry point and teach its build leg to bootstrap the Visual Studio compiler environment automatically for `*-msvc` presets. The script should locate `vcvars64.bat` through `vswhere.exe` first, fall back to known Visual Studio 2022 install roots, import the environment via `cmd /c "...vcvars64.bat && set"`, and then run the existing CMake configure/build path. If the toolchain cannot be found, fail with install hints for Visual Studio Build Tools rather than mentioning MSYS2.

Add friendly preset migration behavior in the repo-owned PowerShell scripts. If a caller passes a retired `*-msys2` preset, fail before CMake with a message such as `ninja-iter-msys2 is retired. Use ninja-iter-msvc for MSVC or ninja-iter-clang for clang-cl.` Do not add replacement MSYS2 presets or aliases; the goal is explicit retirement, not compatibility-through-ambiguity.

Improve runtime feedback by printing the exact executable path and last-write timestamp before launch. For normal interactive app launches with no command-style arguments, launch with `Start-Process -PassThru` and print the PID. Preserve foreground execution for command/CLI-style invocations so `Smatchet.exe cmd ...` still returns output and exit codes to the caller. Also compare the chosen exe against likely stale sibling outputs (`build/ninja-debug-msvc/Smatchet.exe`, `build/ninja-iter-msvc/Smatchet.exe`, `build/ninja-publish-msvc/Smatchet.exe`, and any caller-provided `-BuildDir`) so the stale-exe check in `AGENTS.md` becomes script behavior instead of memory work.

## Files to modify

1. `scripts/dev/build_standalone.ps1`: add MSVC environment bootstrap for `*-msvc` presets; add retired `*-msys2` preset guard before any environment setup; remove the MSYS2 build-environment branch from this app-build wrapper so it cannot propose or activate MSYS2 for building Smatchet.
2. `scripts/dev/build_and_run.ps1`: change the default preset to `ninja-iter-msvc`; make examples and error text match the blessed command.
3. `scripts/dev/run_standalone.ps1`: print resolved exe path, timestamp, and stale sibling comparison; print PID for detached interactive launches while preserving foreground output/exit-code behavior for command-style runs.
4. `BUILD.md`: move the one-command build/run workflow near the top; state MSYS2 is not required for app builds and `ninja-iter-msys2` is retired.
5. `README.md`: mirror the concise build/run command and MSYS2 retirement note in the build section.
6. `AGENTS.md`: keep the canonical build command on `ninja-iter-msvc`; add a short guard that agents must not propose MSYS2 for building the app.
7. `Source_Core/include/SmatchetImConfig.h`: update the stale comment that still names `ninja-iter-msys2` as a default build.
8. `scripts/dev/test-build-wrapper.ps1` or an equivalent new lightweight script test: cover MSYS2 preset rejection, MSVC env discovery failure text, and run-output path formatting without requiring a full rebuild.
9. `scripts/dev/test-build-warnings.sh` or a new build-output helper: add a first-error summary if build logs remain noisy under MSVC.

## Existing utilities reused

- `Get-SmatchetConfigurePresetBinaryDir` in `scripts/common/SmatchetCMakeCommon.ps1`: resolves preset binary directories; reuse it rather than duplicating preset JSON parsing in the build script.
- `Get-VsWherePath` in `scripts/dev/build_standalone.ps1`: already locates `vswhere.exe`; extend this pattern to find `vcvars64.bat`.
- `Invoke-NativeCommand` in `scripts/dev/build_standalone.ps1`: keep the native-command wrapper so stderr handling remains compatible with Windows PowerShell 5.1.
- `Resolve-StandaloneBuildLocation` in `scripts/dev/run_standalone.ps1`: already resolves the executable location from CMake presets; use it as the source of truth for timestamp/PID output.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no app runtime impact; changes are build scripts and docs.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no app UI-thread impact; no runtime code path is added.
- **Pillar 3 (never crash)**: improves developer safety by reducing wrong-toolchain and wrong-exe launches; no shipped crash surface changes.
- **Pillar 4 (accessibility - keyboard nav / font scaling / WCAG AA)**: no UI impact.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A - docs/scripts only except stale comment`)

1. **PR-fast CI** - N/A; no runtime `Source_Core/src` behavior changes.
2. **Pillar 2 static scanner** - N/A; no ImGui sync-I/O path changes.
3. **Dispatcher drain** - N/A; no dispatcher changes.
4. **Visible-cue bucket-E harness** - N/A; no new sync-stall app path.
5. **Marker inventory** - N/A; no perf markers added.

**Pre-push local check**: N/A because the only `Source_Core` touch is a comment update. If implementation expands into runtime code, rerun the relevant perf gate per `docs/guides/perf-workflow.md`.

## Risks / non-goals

- Risk: importing `vcvars64.bat` mutates the caller process environment in surprising ways. Mitigation: scope the import to the PowerShell process running the script and print the selected Visual Studio path.
- Risk: Visual Studio installations vary by SKU. Mitigation: use `vswhere` with `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, then known 2022 BuildTools/Community/Professional/Enterprise paths as fallback.
- Risk: stale-exe comparison becomes noisy. Mitigation: print a compact table only for existing sibling exe paths and clearly mark the selected path.
- Non-goal: restoring `ninja-iter-msys2` or adding MSYS2 compatibility aliases.
- Non-goal: removing every historical MSYS2 reference from applied design docs, CI history, or perf baselines; only current build guidance and live scripts are in scope.
- Non-goal: changing the supported C++ toolchains beyond MSVC and clang-cl.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: not required for script/docs-only behavior unless implementation touches compiled code beyond comments.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A; no visual UI behavior.
- **PowerShell wrapper scenario / screenshot / sanitizer**: run the new wrapper test for retired preset messages and output formatting. Run `powershell -ExecutionPolicy Bypass -File scripts/dev/build_and_run.ps1 -Preset ninja-iter-msys2 -BuildOnly` and verify it fails with the migration hint.
- **Build gate**: `powershell -ExecutionPolicy Bypass -File scripts/dev/build_and_run.ps1 -Preset ninja-iter-msvc -BuildOnly` from a normal PowerShell, not a VS Developer Prompt. Then `powershell -ExecutionPolicy Bypass -File scripts/dev/build_and_run.ps1 -Preset ninja-iter-msvc -RunOnly` and verify exact exe path/timestamp/PID output for an interactive launch. Also run one command-style invocation if available and verify stdout/exit-code behavior is preserved.
- **Manual residue**: none expected. If a local machine lacks Visual Studio, the script test should validate error text without requiring a compiler install.

## Out of scope (flagged, not designed)

- Full CI migration away from any historical MSYS2 jobs. That needs a separate CI plan if still present in workflows.
- Rewriting old `docs/plans/shipped/*` implementation logs that truthfully record past MSYS2 builds.
- Replacing PowerShell wrappers with the proposed future bash-only build script from `docs/plans/kill-powershell-minimize-toolchain.md` (since shipped).
- Adding a full C++ lint rule for `ghc::filesystem::directory_iterator` range-for. This is worthwhile, but should be a separate code-quality plan because it touches source-pattern policy rather than build onboarding.

## Implementation log
- Previous slices (5015147c): `scripts/dev/with-msvc-env.sh` bash wrapper + `build_standalone.ps1` MSYS2-guard landed.
- This slice: `build_and_run.ps1` default preset → `ninja-iter-msvc`; `SmatchetImConfig.h` stale comment updated; `BUILD.md` MSYS2 section header updated with retirement notice; `run_standalone.ps1` exe path/timestamp/stale-sibling/PID output; `build_standalone.ps1` `*-msys2` preset now throws retirement error instead of activating MSYS2 env; `scripts/dev/test-build-wrapper.ps1` new lightweight test wrapper.
- Docs slice (2026-06-03): closed the previously-deferred plan files 5 + 6. `README.md` build section gains the blessed one-command (`build_and_run.ps1 -Preset ninja-iter-msvc`, no Developer Prompt needed) + the MSYS2-retirement note. `AGENTS.md` § Build rule gains the "never propose MSYS2 for building" guard (the canonical `ninja-iter-msvc` command was already present). **Remaining: only the optional/conditional file 9** (`test-build-warnings.sh` first-error summary — gated on "if build logs remain noisy under MSVC", not observed; effectively droppable).

## Deviations from plan
- `build_standalone.ps1` (plan file 1) already had the MSVC bootstrap from slice 1. This slice only added the retirement `throw` for `*-msys2` presets (replacing the `Use-Msys2Ucrt64Environment` call) — the overall function was not removed to preserve historical context in `Use-Msys2Ucrt64Environment` for the function body itself (it is now unreachable but documents what it did).
- `README.md` and `AGENTS.md` updates (plan files 5, 6) were deferred from the first slice — **now shipped** in the 2026-06-03 docs slice (see Implementation log). Only the conditional file 9 (`test-build-warnings.sh` first-error summary) remains, and it is gated on a noisy-log condition not observed under MSVC, so it is effectively droppable — this plan is functionally complete.

## Verification (actual)
- `scripts/dev/test-build-wrapper.ps1` run: 3/3 tests passed. Verified: (1) `ninja-iter-msys2` passed to `build_standalone.ps1` exits non-zero with "retired" + "ninja-iter-msvc" in output; (2) `run_standalone.ps1` prints `Exe :` path and `Time:` timestamp; (3) stale-sibling comparison table printed with `<<< selected` marker.
- **Docs slice (2026-06-03):** `md_lint --all` clean on `README.md` / `AGENTS.md` / this plan; `test-agent-contract` 25/0 (AGENTS.md structural gate intact after the anti-MSYS2 guard addition).
- **Scope-reduction final-check for the dropped file 9** (AGENTS.md § Process rules § Scope-reduction grep): grepped `**/CONTEXT*.md`, `docs/adr/`, `agents/`, `docs/self-improvement/categories/` for the file-9 symbols (`first-error summary`, `build-output helper`, the `test-build-warnings.sh` first-error behaviour) → **zero hits**. File 9 was never implemented and is referenced nowhere, so dropping it requires no revision/deletion elsewhere — the deferral is clean. File 9 stays a no-op "open if build logs ever get noisy under MSVC" note, not tracked work.
