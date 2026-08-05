# Kill PowerShell + Minimize External Tools

> **Status**: `active` — not started (no slices merged). **Re-audited 2026-08-05** against the live tree; inventory, paths, and phase targets below are the audited state, not the 2026-06 draft.

## Context

Smatchet's dev toolchain sprawls across **34 first-party PowerShell scripts** (audited 2026-08-05 via `git ls-files '*.ps1'` — 27 under `scripts/`, 6 under `agents/scripts/core/`, 1 at the repo root; `tools/bug-report-relay/node_modules/**/*.ps1` are vendored npm shims and out of scope) plus a wide external-tool dependency set (`jq`, `flock`, `gh`, BurntToast, plus the build core). This couples the project to Windows-specific tooling, doubles the maintenance surface (PS↔bash drift — `setup-harness.{ps1,sh}`, `with-msvc.ps1`↔`with-msvc-env.sh`, `set-vcs-mode.{ps1,sh}` are all live twin pairs today), and makes first setup fragile (jq + gh PATH bugs already burned the orchestrator mid-merge-gates-poll).

Goal: **collapse to bash + minimal cross-platform tool set**. Keep PS only where Windows fundamentally requires it (Scheduled Tasks + OS-toast + module install).

### What changed since the original draft (why the re-audit was needed)

- **`scripts/` was split** by [`split-scripts-build-vs-agentic.md`](../shipped/split-scripts-build-vs-agentic.md) (PRs #609/#610): 97 scripts relocated. Every PS path in the old plan is stale — the dev wrappers now live under **`scripts/dev/local/`** (human-run, CI-irrelevant, allow-listed to skip the MSVC build) and the watcher/notify shims under **`agents/scripts/core/`**.
- **Eight PS files exist that the old plan never listed**: `scripts/dev/{new-session,worktree,run-with-procdump,with-msvc,verify}.ps1`, `scripts/dev/local/{build-msvc-asan,test-build-wrapper}.ps1`, plus `agents/scripts/core/merge-watcher-{install-prune-task,notify-setup}.ps1` — and the repo-root **`build.ps1`** landed later still (`dev-onboarding-first-run-quickstart`). Net PS count went **up**: 20 drafted → 25 counted 2026-06-15 → **34** today.
- **`jq`'s role shrank**: `check-required-tools.sh` now documents it as **test-harness-only** (`merge_gates.bats` mocks `gh` via jq); the poller + watcher parse through **gh's bundled jq** (`gh api --jq`). The old "+200ms per poll" argument for keeping it no longer applies to the hot path — but it stays anyway (bats suite dependency, zero-cost to keep).
- **`flock` is still required and still un-replaced** — no `scripts/dev/lockfile.py` exists; `docs/harness/claude-code/hooks/lint-cpp-drain.sh` still calls it. Phase 5 is unchanged and still the cheapest slice.
- **New consumer of the tool list**: `scripts/dev/setup-env.sh` (PR #1946) *installs* missing tools from its own package map, which carries a `flock` row (line 138) — dropping `flock` now means editing **two** files, not one.
- **CI PS coupling grew**: `build-and-test.yml` has three `shell: pwsh` steps around the **ARM64 installer** that invoke `scripts/publish/release_github.ps1` directly (lines ~672–738). The old plan only knew about `coverage.yml`'s `shell: powershell` OpenCppCoverage step (still there, line 130).

## Current inventory (audited 2026-08-05)

| Location | Count | Files |
|---|---|---|
| `scripts/dev/local/` | 14 | `build_and_run.ps1` + 3 `build_and_run_*` shims, `build_standalone`, `run_standalone`, `run_clang_tidy`, `attach_unreal_vsjit`, `build-msvc-asan`, `test-build-wrapper`, `package_unreal_plugin_msvc`, `build_and_deploy_unreal_plugin`, `build_deploy_and_open_unreal`, `rebuild_testproject_plugin` |
| `scripts/dev/` | 6 | `worktree`, `new-session`, `with-msvc`, `set-vcs-mode`, `run-with-procdump`, `verify` |
| repo root | 1 | `build.ps1` (thin dispatcher → `scripts/dev/local/build_and_run.ps1`) |
| `scripts/publish/` | 6 | `release_github`, `install_unreal_plugin`, `sync_release_version`, `test_installer_smoke`, `test_release_smoke`, `test_windows_version_info` |
| `scripts/common/` | 1 | `SmatchetCMakeCommon.ps1` |
| `agents/scripts/core/` | 6 | `setup-harness`, `smatchet-notify-windows`, `merge-watcher-install-autostart`, `merge-watcher-uninstall-autostart`, `merge-watcher-install-prune-task`, `merge-watcher-notify-setup` |
| **total** | **34** | → target **5 kept**, **29 deleted** |

## Final tool set after this change

| Tool | Reason kept |
|---|---|
| bash | universal shell (Git Bash on Windows — already a hard prerequisite) |
| python (3.11+) | dev scripts + new flock replacement |
| cmake + ninja + gcc/g++ | build core |
| git | table stakes |
| gh | GitHub CLI — kept (curl+REST rewrite of merge-gates not worth the diff size); its **bundled jq** is what the poller actually uses |
| jq | **kept** — now test-harness-only (`merge_gates.bats` mocks `gh` through it); cheap to keep, and dropping it would mean rewriting the bats mocks |
| clang-format + clang-tidy + cppcheck | Pillar-3 enforcement |
| **REMOVED**: flock | → `scripts/dev/lockfile.py` (cross-platform `msvcrt`/`fcntl`) |
| BurntToast (PS module) | **kept** — sole reliable Windows OS-toast channel when Smatchet isn't running |

## Approach

### Phase 1 — Convert `scripts/dev/local/` build wrappers to bash (no behaviour change)

- `build_and_run.ps1` + `build_and_run_ninja_debug.ps1` + `build_and_run_vs_debug.ps1` + `build_and_run_vs_release.ps1` → single **`scripts/dev/local/build-and-run.sh --preset <preset> [-- args]`** (the three `build_and_run_*` files are thin shims over the first).
- `build_standalone.ps1` → `build-standalone.sh` (idempotent `CMakeCache.txt` skip).
- `run_standalone.ps1` → `run-standalone.sh`.
- `run_clang_tidy.ps1` → `run-clang-tidy.sh` (reads `compile_commands.json` via python).
- `build-msvc-asan.ps1` → `build-msvc-asan.sh`.
- `attach_unreal_vsjit.ps1` → `attach-unreal-vsjit.sh` (calls `vswhere.exe` by direct path).
- `test-build-wrapper.ps1` → `test-build-wrapper.sh`, retargeted at the new `.sh` wrappers, and picked up automatically by `test-all.sh`'s `.sh` glob (today it is invisible to the harness because it is `.ps1`).

MSVC-env sourcing: **do not re-derive** the `vswhere`→`vcvars64` dance — call `scripts/dev/with-msvc-env.sh`, which already exists and is the documented bash wrapper ([`build.md`](../../agent-rules/build.md) § MSVC toolset env). Reuse `scripts/dev/perf-run.sh` for arg-parsing idiom, `agents/scripts/core/setup-harness.sh` for the `uname -s` → `MINGW*/MSYS*/CYGWIN*` Windows detection, and `scripts/dev/check-required-tools.sh` (lines 75-82) for the idempotent MSYS2 PATH-prepend.

Also in scope, and **must move in the same slice** as `build_and_run` (both delegate to it):

- the repo-root **`build.ps1`** (added by [`dev-onboarding-first-run-quickstart`](../shipped/dev-onboarding-first-run-quickstart.md)) — a thin dispatcher (preset auto-detect + `with-msvc.ps1` routing, zero build logic) → **`build.sh`**. Its behaviour is pinned by `test-build-wrapper.ps1` tests 4-8 (five cases on top of the three pre-existing ones), which port with it. Root `build.ps1` is cited from `README.md` / `QUICKSTART.md` / `BUILD.md` — user-facing, so keep a one-line PS shim *or* update all three in the same slice.
- **`scripts/dev/verify.ps1`** → **`verify.sh`**: build (`build_and_run -BuildOnly`) then `comment_audit.py --diff` + `test-lint-rules.sh --diff`. Steps 2-3 are already bash, so the port is mostly dropping the wrapper; it is cited from `BUILD.md`, `docs/agent-rules/{build,process-rules}.md`, and five agent prompts (`build-doctor`, `debug-detective`, `mechanic`, `test-rig`, `offline-sync`) — grep-sweep all of them.

**Doc coupling**: [`build.md`](../../agent-rules/build.md) § MSYS2 retired + § Dual-target verify + § MSVC toolset env name these `.ps1` paths explicitly — update in the same slice.

### Phase 2 — Convert `scripts/publish/` to bash + fix the CI ARM64 path

- `release_github.ps1` → `scripts/publish/release-github.sh` (uses `gh release create`; must keep the Inno Setup `ISCC.exe` discovery — fixed install dirs + uninstall-registry probe — which the CI step's warning text depends on).
- `install_unreal_plugin.ps1` → `install-unreal-plugin.sh`; `sync_release_version.ps1` → `sync-release-version.sh`; `test_installer_smoke.ps1` / `test_release_smoke.ps1` → `.sh`.
- `test_windows_version_info.ps1` → `scripts/publish/test-windows-version-info.py` (PS `Add-Type` has no bash analogue; python `ctypes.windll.version` does).
- `scripts/common/SmatchetCMakeCommon.ps1` → `scripts/common/smatchet-cmake-common.sh` (preset-name parsing, version extraction) — shared by Phases 1–3, so land it **with Phase 1**.
- **`.github/workflows/build-and-test.yml`**: the ARM64 installer job's three `shell: pwsh` steps (~672–738) call `release_github.ps1 -Arch arm64`. Convert to `shell: bash` + `release-github.sh --arch arm64`. This is the only *CI-blocking* PS in the whole plan — every other target is human-run.

### Phase 3 — Convert Unreal packaging PS to bash

`package_unreal_plugin_msvc.ps1`, `build_and_deploy_unreal_plugin.ps1`, `build_deploy_and_open_unreal.ps1`, `rebuild_testproject_plugin.ps1` → `.sh` under `scripts/dev/local/`. These wrap `UnrealBuildTool.exe` + `RunUAT.bat`, both callable from bash on Windows via `cmd.exe /c` (already the pattern in `setup-harness.sh` for `mklink`). vswhere by direct path — no `Resolve-MSBuild` cmdlet needed.

### Phase 4 — Session/worktree launchers + drift twins

**Drift twins — delete the PS, bash is already canonical:**
- `agents/scripts/core/setup-harness.ps1` (twin of `setup-harness.sh`; [`SETUP.md`](../../harness/SETUP.md) line 19 offers it as a Windows substitute and line 159 tells contributors to edit **both** — that instruction is the drift).
- `scripts/dev/with-msvc.ps1` (twin of `with-msvc-env.sh`).
- `scripts/dev/set-vcs-mode.ps1` (twin of `set-vcs-mode.sh`).

**Ports (no PS-only capability involved):**
- `scripts/dev/worktree.ps1` (284 lines — worktree lifecycle: `git worktree`, `.claude/` provisioning, junctions via `mklink`) → `scripts/dev/worktree.sh`.
- `scripts/dev/new-session.ps1` (66 lines — `nsc` launcher) → `scripts/dev/new-session.sh`.
- `scripts/dev/run-with-procdump.ps1` (66 lines — procdump child-process wrapper) → `scripts/dev/run-with-procdump.sh`.

⚠ **User-visible workflow change**: `nsc` is a PowerShell-profile alias and `pwsh scripts/dev/worktree.ps1 new <slug>` is quoted in the SessionStart shared-tree banner, [`process-rules.md`](../../agent-rules/process-rules.md) § Concurrent interactive sessions, `SETUP.md`, and the `guard-shared-tree.sh` / `guard-head-drift.sh` hooks. The alias must be re-pointed at `bash …/new-session.sh` and **all** hook/banner strings updated in the same slice — a half-migration here silently breaks the worktree discipline that keeps concurrent sessions from corrupting each other. This is the highest-coordination slice despite being small.

### Phase 5 — Drop `flock` (unchanged; still the cheapest slice)

Sole consumer: `docs/harness/claude-code/hooks/lint-cpp-drain.sh` (queue serialisation).

Replacement: new `scripts/dev/lockfile.py` — cross-platform exclusive lock via `msvcrt.locking` (Windows) / `fcntl.flock` (POSIX). Bash wrapper invokes it as a subprocess holding the lock for a `--cmd` invocation.

```bash
# before
flock -x "$lockfile" -- bash drain.sh

# after
python scripts/dev/lockfile.py --lockfile "$lockfile" --cmd 'bash drain.sh'
```

Then remove the `flock` row from **both** `scripts/dev/check-required-tools.sh` (line 65) **and** `scripts/dev/setup-env.sh` (package-map line 138 + the hint case at line 148), and the `flock` row in [`SETUP.md`](../../harness/SETUP.md) (line 52).

### Phase 6 — Keep the Windows-native shims (no change)

The 5 kept files all live under `agents/scripts/core/` and all use cmdlets with no clean `.exe` equivalent:

| File | Why PS is mandatory |
|---|---|
| `smatchet-notify-windows.ps1` | BurntToast toast — sole reliable OS-toast channel when Smatchet isn't running |
| `merge-watcher-notify-setup.ps1` | `Install-Module BurntToast` opt-in (the notify path deliberately never auto-installs) |
| `merge-watcher-install-autostart.ps1` | `Register-ScheduledTask` with XML config — no clean `schtasks.exe` equivalent |
| `merge-watcher-uninstall-autostart.ps1` | symmetric pair |
| `merge-watcher-install-prune-task.ps1` | `Register-ScheduledTask` (daily `merge-watch prune` self-heal) |

Add to each: `# Last remaining PowerShell file — see docs/harness/SETUP.md § Windows-only shims.`

### Phase 7 — Sweep leftovers

- `.github/workflows/coverage.yml` line 130 — replace `shell: powershell` (OpenCppCoverage install) with `shell: bash` + choco via `cmd.exe /c choco install …`, or move the install to a setup step.
- Grep-sweep `docs/**/*.md` + `agents/**/*.md` + `BUILD.md` + the `.claude` hook scripts for `.ps1` / `pwsh` / `powershell` → point at the `.sh` equivalents. Known hits beyond those already named: `BUILD.md`, `README.md`, `QUICKSTART.md`, `docs/plans/shipped/dev-onboarding-first-run-quickstart.md`, `docs/agent-rules/cpp-rules.md`, `docs/plans/INDEX.md`.
- Update `AGENTS.md` harness-adapter table: note the `flock` → `lockfile.py` replacement.
- Update `docs/harness/SETUP.md` for the new tool floor + a **§ Windows-only shims** section naming the 5 kept files.
- Delete the 29 ported `.ps1` files.

## Files modified

**New (bash + python):** ~14 `.sh` under `scripts/dev/local/` (Phases 1+3) · 5 `.sh` + 1 `.py` under `scripts/publish/` (Phase 2) · `scripts/common/smatchet-cmake-common.sh` · `scripts/dev/{worktree,new-session,run-with-procdump}.sh` (Phase 4) · `scripts/dev/lockfile.py` (Phase 5).

**Modified:** `scripts/dev/check-required-tools.sh` + `scripts/dev/setup-env.sh` (drop `flock`) · `docs/harness/claude-code/hooks/lint-cpp-drain.sh` · `.github/workflows/build-and-test.yml` (ARM64 installer → bash) + `coverage.yml` (→ bash) · `AGENTS.md` + `docs/harness/SETUP.md` + `docs/agent-rules/build.md` + `docs/agent-rules/process-rules.md` + `BUILD.md` · `guard-shared-tree.sh` / `guard-head-drift.sh` banner strings.

**Deleted:** 29 `.ps1` files (34 today → 5 kept).

## Pillar 1-3 callouts

- **Pillar 1 (perf)**: N/A — scripts run outside the UI thread; no frame-budget impact. (The old "+200ms per merge-gates poll" jq argument is obsolete — the poller uses gh's bundled jq.)
- **Pillar 2 (UI never freezes)**: N/A — no UI-thread code touched.
- **Pillar 3 (never crash)**: N/A — no C++ touched. `lockfile.py` failure modes (lock held > timeout, file unwritable) surface as non-zero exit + stderr, same contract as `flock`.

## Perf-review-system gates

N/A — no `Source/Core/` files touched. No CI perf gate fires. No bucket-E scenario covers shell scripts.

## Risks

- **Worktree-launcher half-migration (new, highest)** — `nsc` / `worktree.ps1` is load-bearing for concurrent-session isolation and is referenced from hooks + the SessionStart banner + three docs. Mitigation: Phase 4 lands launcher + every reference in **one** PR; keep `worktree.ps1` as a 3-line pass-through to the `.sh` for one release cycle before deleting.
- **ARM64 installer CI break (new)** — the only PS in a *required* CI path. Mitigation: land Phase 2's workflow edit alone, on a PR that actually exercises the installer job, before deleting `release_github.ps1`.
- **PS→bash quoting bugs** — Windows path quoting + spaces in `%USERPROFILE%`. Mitigation: every new `.sh` gets a bats test exercising paths-with-spaces (existing pattern in `tests/bats/`).
- **Unreal packaging breakage** — UBT from bash via `cmd.exe /c` works in `setup-harness.sh` but is untested under heavy build load. Mitigation: keep the PS in git one PR cycle as `.ps1.bak`, delete in the next.
- **Lint-hook drift** — `lint-cpp-drain.sh` calls `flock`; update in lockstep with `lockfile.py` landing.
- **`setup-env.sh` skew (new)** — the installer's package map duplicates the tool list; any tool-floor change must edit it too or a fresh clone re-installs a dropped tool.

## Verification

- `bash scripts/dev/check-required-tools.sh` — reduced tool set (no `flock` row; jq still required).
- `bash scripts/dev/setup-env.sh --dry-run` — no `flock` row in the plan output.
- `bash scripts/dev/local/build-and-run.sh --preset ninja-iter-msvc` — full build + run from bash only; matches prior PS exe output.
- `bash scripts/dev/local/test-build-wrapper.sh` — wrapper smoke test, now inside `test-all.sh`'s glob.
- 4 parallel `bash docs/harness/claude-code/hooks/lint-cpp-drain.sh` — confirms `lockfile.py` serialises.
- `bash scripts/publish/release-github.sh --tag v0.0.0-test --no-publish` — release pipeline without upload; plus a green ARM64 installer job on the Phase-2 PR.
- `bash scripts/dev/local/build-and-deploy-unreal-plugin.sh` — UBT-from-bash under Windows.
- `bash scripts/dev/new-session.sh <slug>` from the integration tree — creates the worktree, provisions `.claude/`, launches; guard hooks still fire.
- `bats tests/bats/` green; `bash scripts/dev/test-all.sh` green; `bash scripts/dev/test-docs.sh` green (doc-link sweep after the `.ps1` → `.sh` repath).
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target still clean.
- Grep sweep: `git grep -nE '\.ps1|powershell|pwsh' -- scripts docs agents .github BUILD.md AGENTS.md` returns only the 5 kept shims + the SETUP.md § Windows-only shims callout.

## Implementation order (suggested PR slices)

1. **Slice A** — `lockfile.py` + drop `flock` (both `check-required-tools.sh` and `setup-env.sh`). Smallest, fully decoupled.
2. **Slice B** — `smatchet-cmake-common.sh` + Phase 1 build wrappers, **including root `build.ps1` → `build.sh` and `verify.ps1` → `verify.sh`** (both delegate to `build_and_run`, so they cannot be split off) plus the README/QUICKSTART/BUILD + agent-prompt reference sweep. Hot path, visible immediately.
3. **Slice C** — Phase 4 drift-twin deletions (`setup-harness.ps1`, `with-msvc.ps1`, `set-vcs-mode.ps1`). Pure deletion, no port.
4. **Slice D** — Phase 2 publish scripts **+ the ARM64 CI workflow edit** (must ship together).
5. **Slice E** — Phase 4 worktree/new-session/procdump launchers + every hook/doc/banner reference.
6. **Slice F** — Phase 3 Unreal packaging. Highest blast radius.
7. **Slice G** — Phase 7 sweep (coverage.yml, docs, final deletions). Kept shims untouched.

## Implementation log

_To be filled in per shipped commit._

## Deviations from plan

- **2026-08-05 — re-audit, no code shipped.** Plan rewritten against the live tree after `split-scripts-build-vs-agentic` invalidated every path. Inventory 20 → **34** (`git ls-files '*.ps1'`, vendored `node_modules` excluded — the first pass globbed only `scripts/**` + `agents/**` and so missed root `build.ps1` and `scripts/dev/verify.ps1`); kept-PS set 4 → **5** (all `agents/scripts/core/`); Phase 4 added (drift twins + worktree launchers, previously unlisted); Phase 2 gained the ARM64 CI workflow; Phase 5 gained `setup-env.sh`; the jq rationale was replaced (the +200ms poll argument no longer holds — the poller uses gh's bundled jq).
