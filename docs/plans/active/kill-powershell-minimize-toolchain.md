# Kill PowerShell + Minimize External Tools

> **Status**: `active` — not started (no slices merged).

## Context

Smatchet's dev toolchain currently sprawls across **~25 PowerShell scripts under `scripts/`** (point-in-time count as of 2026-06-15; the plan was originally drafted against 20) + a wide external-tool dependency set (`jq`, `flock`, `gh`, `BurntToast`, plus the build core). This couples the project to Windows-specific tooling, doubles the maintenance surface (PS↔bash drift in scripts like `setup-harness.{ps1,sh}`), and makes the iter-loop fragile on first setup (jq + gh PATH bugs already burned the orchestrator mid-merge-gates-poll per the agent-self-improvement backlog).

Goal: **collapse to bash + minimal cross-platform tool set**. Keep PS only where Windows fundamentally requires it (chicken-and-egg + Scheduled Tasks + OS-toast).

User decisions:
- **Tool floor: Iter-speed-optimised** — drop `flock` only; **keep `jq`** (python-port adds +200ms per merge-gates poll, slows hot path); keep `gh`.
- **PS shims: 3 thin shims kept** — `merge-watcher-install-autostart.ps1` + `merge-watcher-uninstall-autostart.ps1` + `smatchet-notify-windows.ps1` (BurntToast). (`bootstrap-msys2.ps1` was removed with the MSYS2 build preset layer in commit 6537dc3.)

## Final tool set after this change

| Tool | Reason kept |
|---|---|
| bash | universal shell |
| python (3.11+) | dev scripts + new flock replacement |
| cmake + ninja + gcc/g++ | build core |
| git | table stakes |
| gh | GitHub CLI — kept (curl+REST rewrite of merge-gates not worth the diff size) |
| jq | **kept** — python port costs +200ms per merge-gates poll on the hot path |
| clang-format + clang-tidy + cppcheck | Pillar-3 enforcement |
| **REMOVED**: flock | → `scripts/dev/lockfile.py` (cross-platform `msvcrt`/`fcntl`) |
| BurntToast (PS module) | **kept** — sole reliable Windows OS-toast channel when Smatchet isn't running |

## Approach

### Phase 1 — Convert PS dev-wrappers to bash (no behaviour change)

Targets — all under `scripts/dev/`:
- `build_and_run.ps1`, `build_and_run_ninja_debug.ps1`, `build_and_run_vs_debug.ps1`, `build_and_run_vs_release.ps1` → single `scripts/dev/build-and-run.sh <preset> [-- args]`
- `build_standalone.ps1` → `scripts/dev/build-standalone.sh` (idempotent CMakeCache.txt skip)
- `run_standalone.ps1` → `scripts/dev/run-standalone.sh`
- `run_clang_tidy.ps1` → `scripts/dev/run-clang-tidy.sh` (reads `compile_commands.json` via python)
- `attach_unreal_vsjit.ps1` → `scripts/dev/attach-unreal-vsjit.sh` (calls `vswhere.exe` via direct path)

All four `build_and_run_*.ps1` files are 10-line shims around `build_and_run.ps1` — collapse to one bash entry-point with `--preset` flag.

Also in scope (added by `dev-onboarding-first-run-quickstart`): the **root `build.ps1`** — a thin dispatcher (preset auto-detect + `with-msvc.ps1` routing, zero build logic) that delegates to `build_and_run.ps1`. It ports to `build.sh` alongside its delegate, and its behaviour is pinned by `scripts/dev/local/test-build-wrapper.ps1` tests 4-7, which port with it.

Existing bash patterns to reuse:
- `scripts/dev/lint-cpp-common.sh` — MSYS2 PATH-prepend idempotency pattern (lines 75-82 of `scripts/dev/check-required-tools.sh`).
- `scripts/dev/perf-run.sh` — argument parsing + cmake invocation idiom.
- `scripts/setup-harness.sh` — Windows-detection pattern (`uname -s` → `MINGW*/MSYS*/CYGWIN*`).

### Phase 2 — Convert PS publish/release scripts to bash

Targets — all under `scripts/publish/`:
- `release_github.ps1` → `scripts/publish/release-github.sh` (uses `gh release create`)
- `install_unreal_plugin.ps1` → `scripts/publish/install-unreal-plugin.sh`
- `sync_release_version.ps1` → `scripts/publish/sync-release-version.sh`
- `test_installer_smoke.ps1`, `test_release_smoke.ps1` → `.sh` equivalents
- `test_windows_version_info.ps1` → `scripts/publish/test-windows-version-info.py` (uses ctypes — PS `Add-Type` doesn't port to bash; python `ctypes.windll.version` does)

Helper currently in PS: `scripts/common/SmatchetCMakeCommon.ps1` → port to `scripts/common/smatchet-cmake-common.sh` (preset-name parsing, version extraction).

### Phase 3 — Convert Unreal packaging PS to bash

Targets:
- `package_unreal_plugin_msvc.ps1` → `scripts/dev/package-unreal-plugin-msvc.sh`
- `build_and_deploy_unreal_plugin.ps1` → `scripts/dev/build-and-deploy-unreal-plugin.sh`
- `build_deploy_and_open_unreal.ps1` → `scripts/dev/build-deploy-and-open-unreal.sh`
- `rebuild_testproject_plugin.ps1` → `scripts/dev/rebuild-testproject-plugin.sh`

These wrap `UnrealBuildTool.exe` + `RunUAT.bat` — both callable from bash on Windows via `cmd.exe /c` (already the pattern in `setup-harness.sh` for `mklink`). Use vswhere via direct path (no PS `Resolve-MSBuild` cmdlet needed).

### Phase 4 — (skipped; `jq` kept for iter-speed)

`jq` stays on the required-tools list. Python port deferred indefinitely — revisit only if jq install becomes a friction point again.

### Phase 5 — Drop `flock`

Sole consumer: `scripts/dev/lint-cpp-drain.sh` (queue serialisation).

Replacement: new `scripts/dev/lockfile.py` — cross-platform exclusive lock using `msvcrt.locking` on Windows + `fcntl.flock` on POSIX. Bash wrapper invokes it as a subprocess that holds the lock for the duration of a `--cmd` invocation.

```bash
# before
flock -x "$lockfile" -- bash drain.sh

# after
python scripts/dev/lockfile.py --lockfile "$lockfile" --cmd 'bash drain.sh'
```

Remove `flock` from `check-required-tools.sh`.

### Phase 6 — Keep BurntToast notify (no change)

`scripts/dev/smatchet-notify-windows.ps1` + BurntToast module stay. Sole reliable Windows OS-toast channel when Smatchet isn't running. `smatchet-notify.sh` channel-2 fallback unchanged.

### Phase 7 — Sweep leftovers

- Delete `scripts/setup-harness.ps1` (bash version already canonical).
- Delete the four `build_and_run_*.ps1` shims.
- Update `.github/workflows/coverage.yml` — replace `shell: powershell` step with `shell: bash` using a curl-based OpenCppCoverage install (or move install to a setup step; choco call wrapped via `cmd.exe /c choco install ...`).
- Grep-sweep `docs/**/*.md` + `agents/**/*.md` for `.ps1` references → update to `.sh` equivalents.
- Update `AGENTS.md` harness-adapter table: note `flock` replacement.
- Update `docs/harness/SETUP.md` for the new tool floor.

### Kept PS (4 files — deliberately)

- ~~`scripts/dev/bootstrap-msys2.ps1`~~ — removed with the MSYS2 build preset layer (commit 6537dc3).
- `scripts/dev/merge-watcher-install-autostart.ps1` — Windows Scheduled Task install (`Register-ScheduledTask` cmdlet has no clean `schtasks.exe` equivalent for the XML config used).
- `scripts/dev/merge-watcher-uninstall-autostart.ps1` — symmetric pair.
- `scripts/dev/smatchet-notify-windows.ps1` — BurntToast OS-toast channel (called by `smatchet-notify.sh`).

Add a comment to each: `# Last remaining PowerShell file — see docs/harness/SETUP.md § Windows-only shims.`

## Files modified

**New (bash + python):**
- `scripts/dev/build-and-run.sh` + ~12 sibling `.sh` files (Phases 1-3)
- `scripts/publish/*.sh` (Phase 2)
- `scripts/common/smatchet-cmake-common.sh` (helper port)
- `scripts/dev/lockfile.py` (flock replacement)
- `scripts/publish/test-windows-version-info.py` (PS Add-Type port)

**Modified:**
- `scripts/dev/check-required-tools.sh` — remove `flock` row (jq stays)
- `scripts/dev/lint-cpp-drain.sh` — use `lockfile.py`
- `.github/workflows/coverage.yml` — shell: bash
- `AGENTS.md` + `docs/harness/SETUP.md` — doc updates

**Deleted:**
- 16 `.ps1` files (20 today → 4 kept)

## Pillar 1-3 callouts

- **Pillar 1 (perf)**: N/A — scripts run outside the UI thread; no frame-budget impact. jq kept specifically to avoid +200ms regression on merge-gates poll cycle.
- **Pillar 2 (UI never freezes)**: N/A — no UI-thread code touched.
- **Pillar 3 (never crash)**: N/A — no C++ touched. `lockfile.py` failure modes (lock held > timeout, file unwritable) surface as non-zero exit + stderr, same contract as `flock`.

## Perf-review-system gates

N/A — no `Source_Core/` files touched. No CI perf gate fires. No bucket-E scenario covers shell scripts.

## Risks

- **PS→bash quoting bugs**: Windows path quoting + spaces in `%USERPROFILE%` paths. Mitigation: every new `.sh` gets a bats test exercising paths-with-spaces (mirrors existing pattern in `tests/bats/`).
- **Unreal packaging breakage**: UBT invocation from bash via `cmd.exe /c` has worked in `setup-harness.sh` but not under heavy build load. Mitigation: keep PS scripts in git for one PR cycle as `.ps1.bak` for fast rollback; delete in the next merge.
- **CI lint hook drift**: PostToolUse hook (`.claude/hooks/lint-cpp.sh`) calls flock. Update in lockstep with `lockfile.py` landing.

## Verification

- `bash scripts/dev/check-required-tools.sh` — confirms reduced tool set (no flock line in TOOLS array; jq still required).
- `bash scripts/dev/build-and-run.sh ninja-iter-msvc` — full build + run from bash only. Matches prior PS exe output.
- `bash scripts/dev/lint-cpp-drain.sh` — concurrent runners (spawn 4 in parallel) — confirms `lockfile.py` serialises.
- `bash scripts/publish/release-github.sh --tag v0.0.0-test --no-publish` — exercises release pipeline without uploading.
- `bash scripts/dev/build-and-deploy-unreal-plugin.sh` — confirms UBT-from-bash works under Windows.
- `bats tests/bats/` — full bats suite passes (covers merge-gates + lockfile interactions).
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target build still clean (no script regression broke a code path).
- Grep sweep: `git grep -nE '\.ps1|powershell|pwsh' -- 'scripts/**' 'docs/**' 'agents/**' '.github/**'` returns only the 4 kept PS shims + the SETUP.md callout.

## Implementation order (suggested PR slices)

1. **Slice A** — `lockfile.py` + drop `flock`. Smallest, decouples from everything else.
2. **Slice B** — Phase 1 dev-wrappers (PS→bash for build/run scripts). Visible to user immediately on hot path.
3. **Slice C** — Phase 2 publish scripts.
4. **Slice D** — Phase 3 Unreal packaging scripts. Highest blast-radius; ships last.
5. **Slice E** — Phase 7 sweep (docs, CI yaml, final PS deletion of 16 scripts). BurntToast notify untouched.

## Implementation log

_To be filled in per shipped commit._

## Deviations from plan

_To be filled in post-implementation._
