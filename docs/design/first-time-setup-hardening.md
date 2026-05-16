# Plan — First-time setup hardening

## Context

External evaluator ran a clean checkout of `develop` at `C:\Dev\Codex_eval_smatchet`. End-to-end build succeeded: `cmake --preset ninja-test-msys2` → `SmatchetTests` → `ctest` → `SmatchetStandalone` all pass. Baseline is solid; the friction is in the **first-run path** — toolchain prerequisites are verified by trial-and-error, FetchContent timing is undocumented, and one helper script (`scripts/dev/run_cppcheck.py`) silently degrades when the checkout folder isn't named `Smatchet`.

Evaluator's findings (verbatim summary):

1. No "fresh setup verification" command — toolchain detection happens during `cmake --preset`, errors are CMake-y, not actionable.
2. First-configure cost (≈ 5 min) is undocumented — feels like a stall.
3. `scripts/dev/run_cppcheck.py` regex hard-codes `Smatchet` in the path → 0 entries in non-canonical clones ([run_cppcheck.py:49](scripts/dev/run_cppcheck.py:49)).
4. [BUILD.md](BUILD.md) lacks an end-to-end "verify everything" recipe (configure tests → build tests → ctest → build standalone → cppcheck).
5. No CI under `.github/workflows/` — build matrix isn't continuously exercised.
6. Fresh standalone build emits unused-function warnings in [Target_Standalone/main.cpp](Target_Standalone/main.cpp) + [Source_Core/src/SmatchetUI.cpp](Source_Core/src/SmatchetUI.cpp).
7. FetchContent first-run is heavy — no vendoring / cache guidance for restricted-network contributors.

This plan packages the seven items into independently-shippable slices ordered by ROI vs effort.

---

## Approach

### Slice 1 — Fix `run_cppcheck.py` path detection *(small, ship first)*

**Bug**: [run_cppcheck.py:49](scripts/dev/run_cppcheck.py:49) — `rx = re.compile(r"[/\\]Smatchet[/\\](Source_Core|Plugins|Target_Standalone)[/\\]", re.I)`. The `Smatchet` literal is the checkout folder name, not the project marker. Any clone path that doesn't end in `Smatchet` (CI temp dirs, evaluator's `Codex_eval_smatchet`, contributor workspaces) produces an empty filtered DB and a silent no-op.

**Fix**: key the filter off the repo root computed at [line 37](scripts/dev/run_cppcheck.py:37) (`root = Path(__file__).resolve().parents[2]`), not the path basename.

```python
TOP_LEVEL_DIRS = ("Source_Core", "Plugins", "Target_Standalone")

def keep(entry: dict) -> bool:
    abs_path = Path(entry["file"]).resolve()
    try:
        rel = abs_path.relative_to(root)
    except ValueError:
        return False  # outside repo (FetchContent _deps, system headers)
    parts = rel.parts
    if not parts or parts[0] not in TOP_LEVEL_DIRS:
        return False
    return "_deps" not in parts
```

Drop the regex import. Behavior is identical inside `C:\Dev\Smatchet` and now correct everywhere else.

**Verify**: clone repo to a non-`Smatchet`-named scratch dir, run `python scripts/dev/run_cppcheck.py --no-run`, assert non-zero filtered-entry count.

### Slice 2 — `scripts/dev/doctor.ps1` *(medium, highest evaluator-visible win)*

New script. Checks **before** the user runs `cmake --preset` so failures surface in plain English.

Checks (each `[PASS] tool — vX.Y` or `[FAIL] tool — <how to install>`):

- `cmake --version` ≥ 3.24
- `ninja --version` present on `PATH`
- `git --version` present
- MSYS2 UCRT64: `C:\msys64\ucrt64\bin\gcc.exe` exists, `gcc --version` ≥ 13
- `g++ --version` matches gcc
- `lld --version` (or `ld.lld`) present (used by `ninja-iter-msys2`)
- `python --version` ≥ 3.10 (cppcheck driver + setup-harness scripts)
- `cppcheck --version` (warn-only — required for cppcheck workflow, not for build)
- `clang-tidy --version` (warn-only — required for lint hook, not for build)
- `clang-format --version` (warn-only)
- Env vars: `PATH` contains `C:\msys64\ucrt64\bin`; `$env:VCPKG_ROOT` reported if set (not required)
- Disk: ≥ 4 GB free in `$repoRoot/build` (FetchContent + LTO blow up fast)

Exit code: `0` if all required checks pass; `1` if any required fails; `2` if only warn-only checks fail. CI consumes the exit code; humans read the table.

Companion bash port: `scripts/dev/doctor.sh` for MSYS2 / WSL users. Same checks, same exit codes, runs the PowerShell version via `pwsh` when available else inline checks.

**Verify** automated via `scripts/dev/test-doctor.sh` — runs `doctor.ps1` on the current host, asserts exit 0. Bucket-A.

### Slice 3 — `BUILD.md` "Verify everything" recipe *(small)*

New section after **Common Workflows**, before **Local Overrides**:

```markdown
## First-time verification

Run on a fresh clone to confirm the toolchain is wired correctly. ~5 min first
time (FetchContent populates), ~30 s on a warm clone.

```powershell
# 1. Toolchain pre-flight (instant)
.\scripts\dev\doctor.ps1

# 2. Configure + build the test rig
cmake --preset ninja-test-msys2
cmake --build --preset ninja-test-msys2 --target SmatchetTests

# 3. Run the test rig
ctest --preset ninja-test-msys2 --output-on-failure

# 4. Build the standalone exe
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone

# 5. Optional: static-analysis pass
python .\scripts\dev\run_cppcheck.py
```

Expected first-configure time: **~5 minutes** the very first time as
FetchContent downloads and builds nlohmann/json, cpr, SQLiteCpp, cpp-httplib,
md4c, ImGui, GLFW, Lua, sol2, and ghc::filesystem into `build/<preset>/_deps/`.
Subsequent configures complete in seconds.
```

### Slice 4 — Warning cleanup *(small)*

Build emits `-Wunused-function` (or similar `-Wunused-but-set-variable`) on:

- [Target_Standalone/main.cpp](Target_Standalone/main.cpp) — one of the file-static helpers at lines 96, 101, 109, 148, 160, 172, 183, 194, 203 is not currently referenced after a recent refactor.
- [Source_Core/src/SmatchetUI.cpp](Source_Core/src/SmatchetUI.cpp) — TBD; orchestrator runs `cmake --build --preset ninja-iter-msys2 2>&1 | grep -E 'unused.*function|unused.*variable'` to enumerate.

**Fix path**: delete the dead function outright (preferred — no callers means no contract), or move it under an `#if defined(SMATCHET_…)` gate if it's a near-future hook. **No** `[[maybe_unused]]` or `(void)x` covers — those hide intent. Verify post-fix build is warning-clean with `-Werror=unused-function` temporarily added.

### Slice 5 — GitHub Actions CI *(medium-large)*

New `.github/workflows/build-and-test.yml`:

- Trigger: `push` on `develop` + `pull_request` targeting `develop`.
- Job 1 — **windows-2022 + MSYS2 UCRT64** (canonical toolchain): `doctor.ps1` → configure `ninja-test-msys2` → build `SmatchetTests` → ctest → build `SmatchetStandalone`. ~12 min cold, ~3 min warm with `actions/cache` keyed on `build/<preset>/_deps`.
- Job 2 — **windows-2022 + MSVC** (already supported via VS preset): `cmake --preset ninja-iter-msys2` skipped; use the MSVC preset path. Confirms the dual-compiler invariant.
- Job 3 (deferred to Slice 5b) — lint: `clang-format --dry-run --Werror` + `cppcheck` via `run_cppcheck.py`.
- Job 4 (deferred to Slice 5c) — Unreal `SmatchetCore_DX12` build verification when a self-hosted runner with UE is available; for now, document gap.

`actions/cache` keys: `${{ hashFiles('CMakeLists.txt', '**/CMakeLists.txt') }}` for `_deps/`; cache scope is per-job per-preset.

**Decision deferred to user**: do we accept ~12 min CI cold start? Alternative is a smaller "smoke" job (configure + test rig only, no standalone). Pick at PR review.

### Slice 6 — FetchContent timing + vendoring guidance *(small)*

Append to **First-time verification** in BUILD.md (Slice 3) — covered by the explicit "~5 minutes" callout. Standalone slice for restricted-network contributors:

New `docs/dev/offline-builds.md`:

- How to pre-populate `build/<preset>/_deps/` from a peer / mirror.
- `FETCHCONTENT_SOURCE_DIR_<NAME>` CMake variables for each dependency, with the canonical commit hash from `Source_Core/CMakeLists.txt` (or wherever each `FetchContent_Declare` lives).
- Cache hit semantics: changing the preset re-uses `_deps/` only if same compiler + same generator.
- Mirror option: `FETCHCONTENT_BASE_DIR` override pointing at a network share.

No code change in this slice — pure docs. Defer concrete vendoring scripts until a contributor actually asks; YAGNI.

### Slice 7 — Self-diagnosing `cmake --preset` *(stretch)*

Wire `doctor.ps1` into the preset itself — `cmake -P scripts/dev/PreConfigureCheck.cmake` as a `CMAKE_PROJECT_TOP_LEVEL_INCLUDES` entry, so the first error message a user sees on a missing toolchain is `MSYS2 UCRT64 not found; install with: winget install MSYS2.MSYS2` instead of `The C++ compiler "C:/.../cl.exe" is not able to compile a simple test program.`

Defer to Slice 7 — only worth doing once Slices 1-6 are merged and the doctor script is hardened.

---

## Ordering

| Slice | Effort | ROI | Order |
|---|---|---|---|
| 1. `run_cppcheck.py` fix | XS (10 LOC) | High (silent-bug killer) | **First — standalone PR** |
| 2. `doctor.ps1` | M | High | Second — standalone PR |
| 3. BUILD.md verify recipe | XS | Medium | Bundle with #2 |
| 4. Warning cleanup | S | Low-Medium | Third — own PR |
| 5. CI workflow | M-L | Medium | Fourth — own PR, gated on #1-3 |
| 6. Offline-builds doc | S | Low (niche audience) | Fifth — own PR or fold into #3 |
| 7. Preset pre-flight hook | M | Low (covers 1% of failures) | Backlog item, not this batch |

Each slice is a separate `develop` PR. No cross-slice dependencies in code; #5 depends on #2 existing only at the script level.

---

## Risks

- **Slice 1**: changing the filter predicate might surface latent cppcheck violations in TUs that were previously excluded by the buggy regex. Mitigation: run `python scripts/dev/run_cppcheck.py --no-run` before + after the patch on `C:\Dev\Smatchet`; diff entry counts. Any new violations route through `docs/backlog/CPPCHECK_PLAN.md`.
- **Slice 2**: false negatives in version detection on edge MSYS2 installs (alt prefix, portable installs). Mitigation: every check has a `--verbose` mode that prints the exact command + exit code; doctor.ps1 fails open with clear messages, not silently.
- **Slice 4**: deleting a static function may break an `#if SMATCHET_*` branch we missed. Mitigation: build all four presets (`ninja-iter-msys2`, `ninja-debug-msys2`, `ninja-test-msys2`, `ninja-publish-msys2`) before claiming done.
- **Slice 5**: GitHub Actions minutes cost. Mitigation: cache `_deps/` aggressively; restrict `pull_request` to `develop` (not every branch push).

---

## Verification

Per-slice verification (each shipping PR adds its own `scripts/dev/test-*.sh`):

- **Slice 1**: `scripts/dev/test-cppcheck-path-detection.sh` — clones repo to `$(mktemp -d)/not-named-smatchet`, runs `run_cppcheck.py --no-run`, asserts entry count > 0. Bucket-A.
- **Slice 2**: `scripts/dev/test-doctor.sh` — runs `doctor.ps1` on the current host, asserts exit 0; runs it again with `$env:PATH` stripped of `C:\msys64\ucrt64\bin`, asserts exit 1 and that the error mentions MSYS2. Bucket-A.
- **Slice 3**: prose-only; verified by `markdownlint` + manual read.
- **Slice 4**: `cmake --build --preset ninja-iter-msys2 -- -k 0 2>&1 | grep -c 'unused'` asserts `0`. Add to `scripts/dev/test-all.sh`.
- **Slice 5**: the workflow itself is the test — green build on PR = passed.
- **Slice 6**: prose-only.

All automation goes through `scripts/dev/test-all.sh` per [AGENTS.md § Verification automation](AGENTS.md). No manual residue without a `docs/backlog/AGENT_SELF_IMPROVEMENT.md` entry.

---

## Open questions

- **CI matrix scope (Slice 5)**: full configure+build+ctest, or smoke-only (configure+ctest)? Default to full; downgrade only if minutes burn.
- **Doctor script — bash port (Slice 2)**: ship in same PR as PowerShell, or defer? Default to same PR — single source of truth for the check list.
- **Warning cleanup (Slice 4)**: any chance the file-static helpers are dead-on-`SMATCHET_EMBEDDED_IN_UNREAL` but live on standalone? Verify both targets before deletion.

---

## Implementation log

- `666dfc4` · Plan doc landed (PR [#83](https://github.com/alexandrosk0/Smatchet/pull/83)).
- `ec9e0b9` · Slice 1 — `fix(cppcheck): key TU filter off repo root, not folder name` (PR [#84](https://github.com/alexandrosk0/Smatchet/pull/84)). Replaced the literal-`Smatchet` regex in [scripts/dev/run_cppcheck.py:49](scripts/dev/run_cppcheck.py:49) with a `relative_to(root)` + `TOP_LEVEL_DIRS` predicate; dropped the `import re`. Added [scripts/dev/test-cppcheck-path-detection.sh](scripts/dev/test-cppcheck-path-detection.sh) (bucket-A).
- `a235eb3` · Slices 2+3 — `feat(doctor): toolchain pre-flight + BUILD.md verify recipe` (PR [#85](https://github.com/alexandrosk0/Smatchet/pull/85)). New [scripts/dev/doctor.ps1](scripts/dev/doctor.ps1) (241 LOC) + [scripts/dev/doctor.sh](scripts/dev/doctor.sh) (263 LOC) with parity check list; new [scripts/dev/test-doctor.sh](scripts/dev/test-doctor.sh) (bucket-A) covering both pass and induced-fail paths. [BUILD.md](BUILD.md) gained a 31-line "First-time verification" section with the five-step recipe and the explicit `~5 min` FetchContent first-configure callout.
- `b68bf09` · Slice 4 — `chore(warnings): remove dead file-static helpers in Target_Standalone + Source_Core` (PR [#86](https://github.com/alexandrosk0/Smatchet/pull/86)). Deleted `SmatchetGetStandaloneUserDataDirectory()` from [Target_Standalone/main.cpp](Target_Standalone/main.cpp) and `IsSessionUtilityLayoutKey(const char*)` from [Source_Core/src/SmatchetUI.cpp](Source_Core/src/SmatchetUI.cpp). Both grep-verified dead across `Source_Core/`, `Plugins/`, `UnrealPlugins/`, `Target_Standalone/` before deletion. Added [scripts/dev/test-build-warnings.sh](scripts/dev/test-build-warnings.sh) (bucket-A) — greps iter build log for `-Wunused-*` over the three first-party top-level dirs.

## Deviations from plan

- **Slices 2+3 bundled into one PR (#85)** instead of two — Slice 3's BUILD.md recipe references `doctor.ps1` from Slice 2 by name, so splitting would have shipped a stale reference for the lifetime of the gap. User-approved at dispatch time.
- **Slice 4 test harness pattern**: original plan suggested inline-grep in `test-all.sh`; agent shipped a standalone [scripts/dev/test-build-warnings.sh](scripts/dev/test-build-warnings.sh) instead, auto-enrolled via the existing `test-*.sh` glob — matches the verification-automation convention from AGENTS.md § Verification automation.
- **Slice 4 path-separator regex**: the build-doctor agent caught (via negative-test) that GCC under MinGW emits backslash separators in source paths — `..\..\Target_Standalone\main.cpp`. The shipped regex uses `[\\/]` instead of `/`. Flagged as a candidate AGENTS.md § Debug techniques entry.

## Verification

- **Slice 1**:
  - `python scripts/dev/run_cppcheck.py --no-run` from the canonical `C:\Dev\Smatchet` checkout still writes 205 entries (matches pre-fix baseline) — passed.
  - `python scripts/dev/run_cppcheck.py --no-run` from a non-`Smatchet`-named temp clone: 0 → 3 entries — passed.
  - `bash scripts/dev/test-cppcheck-path-detection.sh` — `Passed: 2  Failed: 0`, exit 0 — passed.
- **Slices 2+3**:
  - `pwsh doctor.ps1` on a host with MSYS2 on PATH — exit 0 GREEN — passed.
  - `pwsh doctor.ps1` with MSYS2 stripped from PATH — exit 1 RED, message names MSYS2 — passed.
  - `bash doctor.sh` matrix (pass + induced-fail) — same result — passed.
  - `bash scripts/dev/test-doctor.sh` — `Passed: 3  Failed: 0`, exit 0 — passed.
  - `markdownlint BUILD.md` — not installed; manual read pass — passed.
- **Slice 4**:
  - `ninja-iter-msys2` Smatchet-owned `-Wunused-*` count: 2 → 0 — passed.
  - `ninja-debug-msys2`: 2 → 0 — passed.
  - `ninja-test-msys2`: 0 → 0 — passed.
  - `ninja-iter-msys2 --target SmatchetCore_DX12`: 0 → 0 (no dual-target regression) — passed.
  - `ctest --preset ninja-test-msys2`: 1/1 — passed.
  - `bash scripts/dev/test-build-warnings.sh` — `Passed: 1  Failed: 0`, plus negative-test (restore dead helper → `Failed: 1` with exact warning line) — passed.
- **Post-merge develop regression gate**:
  - `bash scripts/dev/test-all.sh` on develop @ `b68bf09` — `Aggregate Passed: 89  Failed: 2  Scripts: 7`. The 2 failures are in `test-ui-views-columns-reorder.sh` (pre-existing flake, unrelated to this plan — already flagged by Slice 2+3 build-doctor agent in its smoke report).
- **Open / deferred**:
  - Slices 5 (CI workflow), 6 (offline-builds doc), 7 (cmake preset pre-flight hook) — not shipped, remain on the slice ladder. Slice 5 is the highest-value next step.
  - PS 5.1 `-Command "<multi-line>"` scope-effect drop (build-doctor self-improvement flag) — backlog candidate for `agents/build-doctor.md`.
  - Strict `PATH contains C:\msys64\ucrt64\bin` doctor check produces false-fail on JetBrains-bundled-MinGW hosts (build-doctor self-improvement flag) — backlog candidate to split into "toolchain reachable" (required) + "on PATH at shell time" (warn).
