# Plan — Dev onboarding, first-run tracker wizard, QUICKSTART

> **Slug**: `dev-onboarding-first-run-quickstart` (matches this file's basename without `.md`).
>
> **Status**: `active` — three independently shippable slices. Slice 3 (QUICKSTART) must land at-or-after slice 1 so it cites the real entry point; slice 2 is independent of both.

## Context

Three onboarding gaps surfaced from a fresh Windows clone/build session plus a UX walkthrough of first launch:

1. **Build friction.** [`docs/plans/msvc-build-onboarding-hardening.md`](../shipped/msvc-build-onboarding-hardening.md) § Approach promised that `build_standalone.ps1` would "bootstrap the Visual Studio compiler environment automatically for `*-msvc` presets", and its § Implementation log line 85 asserts *"`build_standalone.ps1` (plan file 1) already had the MSVC bootstrap from slice 1."* **That is false in the tree**: [`scripts/dev/local/build_standalone.ps1`](../../../scripts/dev/local/build_standalone.ps1) contains zero `vcvars` import — its only `vswhere` use is `Get-VsWherePath` (:77-95) locating MSBuild for the VS-generator branch. Callers must still wrap with [`scripts/dev/with-msvc.ps1`](../../../scripts/dev/with-msvc.ps1) when `cl.exe` is absent from PATH. A shipped plan claiming a delivery it did not make is itself a gate escape — § Out of scope names the postmortem follow-up.
2. **First-run product gap.** Fresh installs already open Preferences and default to read-only ([`ConfigManager.cpp:1478-1483`](../../../Source/Core/src/Config/ConfigManager.cpp:1478) sets `ReadOnlyMode = true` + `ShowPreferencesWindow = true` when no config file existed), but there is **no guided path** to pick a backend, enter credentials, verify them, and save. Worse: the Tracker tab has **no Test-connection control at all** — only the Assistant and Whisper tabs do ([`AiPrefsTestConnection.h`](../../../Source/Core/include/AiPrefsTestConnection.h), [`SmatchetPreferencesUi_Whisper.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp)). The only tracker reachability probe today is the **background poller** in [`AppController_Connectivity.cpp:158-177`](../../../Source/Core/src/AppController_Connectivity.cpp:158), which probes the **already-live focused-context backend** and therefore cannot validate credentials the user has not yet committed. Whisper has a first-run banner ([`SmatchetWhisperSetupBanner.cpp`](../../../Source/Core/src/SmatchetWhisperSetupBanner.cpp)); tracker has none.
3. **Doc friction.** [`BUILD.md`](../../../BUILD.md) is complete but long. New contributors want a ≤35-line "clone → build → run → connect tracker" path at repo root.

**Intended outcome**: after all three slices land, a Windows user runs `.\build.ps1` from an ordinary PowerShell (no VS Developer Prompt), reads `QUICKSTART.md` for the five-step path, and on first launch sees a tracker setup wizard that verifies credentials against the live backend **before** committing them to config.

## Approach

Three PR-sized slices, one concern each, no cross-slice code coupling (slice 3 only cross-links slice 1's entry point).

**Slice 1 — root `build.ps1` + the missing MSVC bootstrap.** Extract the vcvars-discovery/import block from `with-msvc.ps1` into a dot-sourced module `scripts/common/SmatchetMsvcEnv.ps1` exposing `Test-SmatchetMsvcAvailable` + `Import-SmatchetMsvcEnvironment`; have `with-msvc.ps1` (behaviour-preserving refactor) and `build_standalone.ps1` (the actual gap-close) both call it. Add a root `build.ps1` that resolves the repo root from `$PSScriptRoot`, auto-selects a preset, prints the chosen preset **and the reason**, then delegates to `scripts/dev/local/build_and_run.ps1`. `build.ps1` contains no CMake logic — the existing wrappers stay the implementation.

**Slice 2 — first-run tracker setup wizard.** A banner modelled structurally on the Whisper banner (`bool Render(AppController&, TrackerConfig&)` returning "caller should re-save config"), plus the piece that does **not** exist yet and is the real cost of this slice: a **candidate-credential probe**. `ITrackerBackendFactory::Create(trackerType, cfg)` ([`ITrackerBackendFactory.h:30`](../../../Source/Core/include/ITrackerBackendFactory.h:30)) already builds a backend from an arbitrary `TrackerConfig` — the wizard builds a throwaway backend from the *uncommitted* buffer values, calls `Connectivity().ProbeReachability(cfg)` on a worker, and posts the verdict back through `app.mainThreadDispatcher`, with a cancel-atom guard copied from `AiPrefsTestConnection::TriggerProbe`. The trade-off taken: the probe helper lives in its own TU (`TrackerCredentialProbe`), not inside the banner, so a later "Test connection" button on the Preferences Tracker tab reuses it instead of forking it.

**Slice 3 — `QUICKSTART.md`.** Pure docs; leads with `.\build.ps1`, demotes raw preset blocks in `README.md` to an "Advanced" subsection.

## Files to modify

### Slice 1 — build entry point (scripts + docs only)

1. **`build.ps1`** (new, repo root) — blessed wrapper. Resolves repo root from `$PSScriptRoot`; auto-selects preset when `-Preset` omitted; forwards `-Preset` / `-Target` / `-BuildOnly` / `-RunOnly` / `-StandaloneArgs` to `build_and_run.ps1`. **No `-Verify` switch** — `build_and_run.ps1` has no such parameter (its params are `Preset, Target, BuildOnly, RunOnly, StandaloneArgs`, [`build_and_run.ps1:14-24`](../../../scripts/dev/local/build_and_run.ps1:14)); do not invent one. Auto-detect ladder, each branch printing its reason:
   - `cl.exe` on PATH → `ninja-iter-msvc` (reason: "cl.exe already on PATH")
   - else `Test-SmatchetMsvcAvailable` true → `ninja-iter-msvc` + `Import-SmatchetMsvcEnvironment` first (reason: "VS VC-tools install found via vswhere")
   - else `clang-cl.exe` on PATH **and** `Test-SmatchetMsvcAvailable` true → `ninja-iter-clang` (still imports the MSVC env — clang-cl needs the Windows SDK headers/libs)
   - else fail with install hints (`winget install Microsoft.VisualStudio.2022.BuildTools`, `winget install LLVM.LLVM`). Note: without a VS install there is no usable path — a bare LLVM has no Windows SDK, so the clang branch is **not** a no-VS fallback.
2. **`scripts/common/SmatchetMsvcEnv.ps1`** (new) — the extracted vcvars module. Must preserve every behaviour currently in [`with-msvc.ps1:53-139`](../../../scripts/dev/with-msvc.ps1:53): `$env:SMATCHET_MSVC_ARCH` → vcvars-batch selection (`vcvars64` / `vcvarsarm64` / `vcvarsarm64_amd64` / `vcvarsamd64_arm64`), host-arch detection via `PROCESSOR_ARCHITEW6432`, the `build.msvc_toolset_pin` lookup in `project.config.json` with the `-vcvars_ver=` argument, the "enumerate every VC-tools install, prefer the pinned one" loop (**not** `-latest`, which picks a BuildTools whose vcvars breaks the cached `cl.exe` — STL1001), the `cmd /c "...bat && set"` dump-and-apply, and the post-import PATH fix-up that de-prioritises the VS-bundled LLVM so `ninja-clang-*` gets the standalone toolchain. Exit code 2 on "vswhere/vcvars not found" is part of the contract — keep it.
3. **[`scripts/dev/with-msvc.ps1`](../../../scripts/dev/with-msvc.ps1)** — STAYS the orchestration + `-Command` exec wrapper; EXTRACT its discovery/import body to the module. Behaviour-preserving: same exit codes, same stderr strings (`test-build-wrapper.ps1` and agent docs match on them).
4. **[`scripts/dev/local/build_standalone.ps1`](../../../scripts/dev/local/build_standalone.ps1)** — **the actual gap close.** Dot-source the module and call `Import-SmatchetMsvcEnvironment` before `cmake --preset` when the preset matches `*-msvc` or `*-clang` **and** the compiler is not already resolvable. Idempotent (no-op when `cl.exe` is present). Leaves the existing `*-msys2` retirement `throw` (:182) and `Invoke-NativeCommand` stderr handling untouched.
5. **[`scripts/dev/local/test-build-wrapper.ps1`](../../../scripts/dev/local/test-build-wrapper.ps1)** — extend the existing 3-case harness. New cases: (a) preset auto-detect table — stub `Get-Command cl` / `Test-SmatchetMsvcAvailable` and assert the printed preset + reason per branch; (b) `-Preset` explicit override wins over auto-detect; (c) `build_standalone.ps1` is a no-op re-import when `cl.exe` is already on PATH; (d) module missing-VS path exits 2 with the winget hints in the message. All four are string-assertions on script output — no compiler required, so they run in CI.
6. **[`README.md`](../../../README.md)** — Build section leads with `.\build.ps1`; raw preset blocks move under an "Advanced" subsection.
7. **[`BUILD.md`](../../../BUILD.md)** — same lead; add one line that `with-msvc.ps1` remains the wrapper for ad-hoc commands inside the MSVC env.
8. **[`docs/agent-rules/build.md`](../../../docs/agent-rules/build.md)** — update the canonical agent build command to `.\build.ps1` (keeping the explicit-preset form for agents that need a specific preset).

### Slice 2 — first-run tracker wizard

Grep-checked: no existing `TrackerSetupPure`, `TrackerCredentialProbe`, or `SmatchetTrackerSetupBanner` TU under `Source/Core/`.

9. **[`Source/Core/include/Config/ConfigManager.h`](../../../Source/Core/include/Config/ConfigManager.h:381)** — add `bool TrackerSetupCompleted = false;` on `TrackerConfig`, adjacent to `WhisperSetupCompleted` (:381) and `ZenMode` (:431).
10. **[`Source/Core/src/Config/ConfigManager.cpp`](../../../Source/Core/src/Config/ConfigManager.cpp:627)** — serialize `j["tracker_setup_completed"]` at the `whisper_setup_completed` site (:627) and read it at the loader's mirror site (:1027).
11. **[`Source/Core/src/Config/ConfigManager.cpp:1478-1483`](../../../Source/Core/src/Config/ConfigManager.cpp:1478)** — **veteran migration**, in the same `!hasSetupConfig` block that already sets `ReadOnlyMode`/`ShowPreferencesWindow`. When a config file **did** exist and the key is absent, set `TrackerSetupCompleted = true` iff the primary credential for the active `TrackerType` is non-empty (Jira → `ApiToken`, Plane → `PlaneApiKey`, GitHub → `GitHubPat`), so existing users are never nagged. Decision logic lives in `TrackerSetupPure` (item 12) and is called from here — the loader gets no new inline branching.
12. **`Source/Core/src/Tracker/TrackerSetupPure.{h,cpp}`** (new) — pure, ImGui-free, header-includable from the loader:
    - `bool CredentialFieldsComplete(const TrackerConfig&)` — per-backend required-field check.
    - `bool NeedsSetup(const TrackerConfig&)` — `!TrackerSetupCompleted || !CredentialFieldsComplete(cfg)`.
    - `bool ShouldGrandfatherExistingUser(const TrackerConfig&)` — the item-11 migration predicate.
13. **`tests/Core/TrackerSetupPure.test.cpp`** (new) + **[`tests/CMakeLists.txt`](../../../tests/CMakeLists.txt)** — bucket A. The bucket-A source list is **explicit** (`Core/<Unit>.test.cpp` rows), not globbed — the row must be added by hand.
14. **`Source/Core/src/Tracker/TrackerCredentialProbe.{h,cpp}`** (new) — the piece that does not exist today. `void TriggerProbe(AppController& app, const TrackerConfig& candidate, ProbeState& out)`. Builds a throwaway backend via `ITrackerBackendFactory::Create(candidate.TrackerType, candidate)`, runs `backend->Connectivity().ProbeReachability(candidate)` on a detached worker, posts the `TrackerReachabilityProbeResult` back through `app.mainThreadDispatcher`, and honours a `shared_ptr<atomic<bool>>` cancel atom checked inside the posted lambda — the exact shape documented in [`AiPrefsTestConnection.h:12-37`](../../../Source/Core/include/AiPrefsTestConnection.h:12). The backend handle is captured **by value into the lambda** (ADR-0012 lifetime rule, mirrored from [`AppController_Connectivity.cpp:161-170`](../../../Source/Core/src/AppController_Connectivity.cpp:161)); the throwaway backend is never installed on a context, so no `SetBackend` race exists.
15. **[`Source/Core/include/AppController.h:163`](../../../Source/Core/include/AppController.h:163)** — add a public `ITrackerBackendFactory* BackendFactory();` accessor. `backendFactory_` is private (:949) with only a `SetBackendFactory` setter, so the probe has no way to reach the factory today. Returns the lazily-initialised member; null-checked by the caller.
16. **`Source/Core/src/SmatchetTrackerSetupBanner.{h,cpp}`** (new; sits beside `SmatchetWhisperSetupBanner.{h,cpp}` in `Source/Core/src/`, **not** under `Ui/` — matching the Whisper precedent). `bool Render(AppController& app, TrackerConfig& cfg)`; same "return true ⇒ caller saves config" contract, same `SetNextWindowPos` work-area pinning between menu bar and dockspace, same `dismissedThisSession` static + `lastCfgSetupCompleted` sticky-observation pattern. Split into `DrawStepBackend` / `DrawStepCredentials` / `DrawFooter` to stay under the 120-line non-UI cap (the file is not under `Ui/` and the entry point is named `Render`, so **the 200-line ImGui-draw cap does not apply** — plan for 120).
17. **[`Source/Core/src/Ui/SmatchetPreferencesUi.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp:264)** — STAYS the Tracker tab; EXTRACT the per-backend credential-input widgets from `DrawTrackerBackendConfig` (:264-338) into a shared helper the banner also calls, so the field set exists once. The buffer→config copy at :585-608 is currently **inline and unnamed** inside the Save & Sync path; extract it as `SmatchetPreferencesUiDetail::CopyTrackerBuffersToConfig(UiDrawSession&)` and call it from both sites. Keep `TrimTrackerCredentialFields` (:604) inside that helper — the wizard needs the same whitespace trim (issue #979).
18. **[`Source/Core/src/Ui/SmatchetUI.cpp:689`](../../../Source/Core/src/Ui/SmatchetUI.cpp:689)** — render hook immediately after the Whisper banner block (`SMATCHET_UI_PERF_SCOPE("SmatchetWhisperSetupBanner::Render")`), with its own `SMATCHET_UI_PERF_SCOPE("SmatchetTrackerSetupBanner::Render")`. Suppress `ShowPreferencesWindow` on the first frame the wizard renders so the user never faces two surfaces at once.
19. **[`Source/Core/src/SmatchetLocalization.cpp`](../../../Source/Core/src/SmatchetLocalization.cpp:854)** — `tracker.setup.*` keys, en + fr, at the `whisper.preferences.*` block (:854).
20. **`tests/ui/tracker_setup_banner.test.cpp`** (new) + **[`tests/ui/CMakeLists.txt`](../../../tests/ui/CMakeLists.txt)** + **[`tests/ui/ui_tests_registry.cpp`](../../../tests/ui/ui_tests_registry.cpp)** — bucket E. All three edits are required: the UI-test source list is explicit (38 rows), and the registry needs both an `extern "C" void SmatchetRegisterTrackerSetupBannerTests(ImGuiTestEngine*);` declaration and a call. Probe determinism comes from injecting a fixture factory via `AppController::SetBackendFactory` whose `Create` returns [`GitHubFixtureBackend`](../../../Source/Core/src/Tracker/GitHubFixtureBackend.cpp:83) / [`PlaneFixtureBackend`](../../../Source/Core/src/Tracker/PlaneFixtureBackend.cpp:100) — both already return `AuthenticatedReachable`, so no network and no new mock type.
21. **Root [`CMakeLists.txt`](../../../CMakeLists.txt)** — **no edit needed** for the new Core TUs. `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")` (:1060) picks them up automatically; the Whisper banner appears in explicit `list(APPEND …)` lines (:1149-1164) *only* because it is `SMATCHET_WITH_WHISPER`-gated. The tracker wizard is always-on and must **not** be added to any gated list. Row kept here so a reviewer does not re-add it.

### Slice 3 — quickstart doc

22. **`QUICKSTART.md`** (new, repo root, ~30 lines) — clone / prerequisites (CMake 3.24+, Ninja, VS 2022 Build Tools) / `.\build.ps1` / first launch / pointer to `BUILD.md`.
23. **[`README.md`](../../../README.md)** — link at the top of the Build section.
24. **[`BUILD.md`](../../../BUILD.md)** — one-line pointer to `QUICKSTART.md`.

## Existing utilities reused

- `Import-SmatchetMsvcEnvironment` source material — [`scripts/dev/with-msvc.ps1:53-139`](../../../scripts/dev/with-msvc.ps1:53) — vswhere enumeration, toolset pin, vcvars dump-and-apply, LLVM PATH de-prioritisation. Extract, do not rewrite.
- `build_and_run.ps1` — [`scripts/dev/local/build_and_run.ps1:14`](../../../scripts/dev/local/build_and_run.ps1:14) — configure/build/run pipeline; `build.ps1` delegates here.
- `SmatchetCMakeCommon.ps1` — [`scripts/common/SmatchetCMakeCommon.ps1`](../../../scripts/common/SmatchetCMakeCommon.ps1) — preset → binary-dir resolution; already dot-sourced by the wrappers.
- `smatchet::whisper::banner::Render` — [`SmatchetWhisperSetupBanner.h:41`](../../../Source/Core/src/SmatchetWhisperSetupBanner.h:41) — banner signature + "caller saves on true" contract + work-area pinning rule.
- `dismissedThisSession` / `lastCfgSetupCompleted` — [`SmatchetWhisperSetupBanner.cpp:244-287`](../../../Source/Core/src/SmatchetWhisperSetupBanner.cpp:244) — session-dismiss + re-run-from-Preferences edge detection.
- `AiPrefsTestConnection::TriggerProbe` — [`AiPrefsTestConnection.h:12-37`](../../../Source/Core/include/AiPrefsTestConnection.h:12) — cfg-snapshot-by-value, in-flight flag, cancel-atom, dispatcher post-back. The **only** existing user-invoked async probe; copy its threading contract.
- `ITrackerBackendFactory::Create` — [`ITrackerBackendFactory.h:30`](../../../Source/Core/include/ITrackerBackendFactory.h:30) — builds a backend from an arbitrary `TrackerConfig`; this is what makes candidate-credential probing possible without touching the live context. Live-swap precedent: [`TicketSyncService.cpp:501-509`](../../../Source/Core/src/Sync/TicketSyncService.cpp:501).
- `ITrackerConnectivity::ProbeReachability` + `TrackerReachabilityProbeKind` — [`ITrackerConnectivity.h:9-27`](../../../Source/Core/include/ITrackerConnectivity.h:9) — the four-way verdict enum the wizard renders.
- `AppController::MapReachabilityProbeKind` — [`AppController_Connectivity.cpp:43`](../../../Source/Core/src/AppController_Connectivity.cpp:43) — existing kind → `TrackerConnectivityState` mapping; reuse for the wizard's colour/label rather than a second mapping table.
- `GitHubFixtureBackend` / `PlaneFixtureBackend` — [`GitHubFixtureBackend.cpp:83`](../../../Source/Core/src/Tracker/GitHubFixtureBackend.cpp:83), [`PlaneFixtureBackend.cpp:100`](../../../Source/Core/src/Tracker/PlaneFixtureBackend.cpp:100) — return `AuthenticatedReachable`; the bucket-E probe fixture.
- `SmatchetPreferencesUiDetail::TrimTrackerCredentialFields` — [`SmatchetPreferencesUi.cpp:604`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp:604) — issue-#979 whitespace trim; wizard must not bypass it.
- `ConfigManager::NormalizeViewsBackendKey` — [`SmatchetPreferencesUi.cpp:593`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp:593) — canonical PascalCase backend key; wizard writes through it.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: slices 1 + 3 have no runtime impact. Slice 2 adds one small always-on window whose body is skipped by an early `if (cfg.TrackerSetupCompleted || dismissedThisSession) return false;` — after setup, per-frame cost is one bool test, matching the Whisper banner's own early-out (`SmatchetWhisperSetupBanner.cpp:287`). Both banner and probe get `SMATCHET_UI_PERF_SCOPE` markers, so the marker inventory regen fires (see § Perf gates item 5).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: the credential probe is network I/O and **must never** run inline. It goes on a detached worker with a `mainThreadDispatcher` post-back; the button flips to a disabled "Testing…" state for the duration — the same visible-cue shape the Assistant tab already ships. The `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotation is required on the probe body.
- **Pillar 3 (never crash)**: three hazards. (a) Probe supersession — the cancel atom must be re-created per trigger and checked inside the posted lambda, or a late verdict writes into freed wizard state. (b) Backend lifetime — the throwaway backend is captured by value in the worker lambda and never installed on a context (ADR-0012). (c) The veteran migration runs inside `ConfigManager::Load`'s already-`try`-wrapped region and must not throw on a partially-populated config; `ShouldGrandfatherExistingUser` takes a `const&` and performs no allocation beyond string compares.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: wizard uses standard `ImGui::RadioButton` / `InputText` / `Button` widgets, so tab-order and font scaling come free, matching the Whisper banner. WCAG AA contrast on the verdict colours is **not** audited here — the wizard reuses the existing `TrackerConnectivityState` palette, so it inherits whatever that already is. Flagged, not fixed.

## Perf-review-system gates

Slice 2 touches `Source/Core/` → gates apply. Slices 1 + 3 are scripts/docs only → N/A.

1. **PR-fast CI** — `idle` is the scenario that most directly exercises the changed path: the banner render hook sits in the main draw between menu bar and dockspace, so a per-frame regression from the early-out surfaces there. `idle` is already in [`scripts/dev/perf-pr-fast-set.json`](../../../scripts/dev/perf-pr-fast-set.json); no subset change needed.
2. **Pillar 2 static scanner** — **fires**. `TrackerCredentialProbe` introduces new network I/O reachable from an `ImGui::*` callstack (the "Test connection" button handler). Worker-thread plan is item 14; the probe body carries `/* PILLAR2_WORKER_ONLY */ // est-latency: 3000ms` (worst case = the HTTP timeout the backends already use).
3. **Dispatcher drain** — does **not** touch `MainThreadDispatcher::Drain()`; it only posts to it, same as the Assistant probe.
4. **Visible-cue bucket-E harness** — the probe is a new > 100 ms path, so the disabled "Testing…" state is a cue under test. The bucket-E TU (item 20) asserts the button is disabled while in-flight, following the `sync_stall_visible_cue.test.cpp` shape.
5. **Marker inventory** — **fires**. Two new `SMATCHET_UI_PERF_SCOPE` markers (banner render, probe trigger) ⇒ regenerate `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `idle` before opening the slice-2 PR.

**Override**: none expected; `perf-out-of-band` per `AGENTS.md` § Merge gates if a baseline bump is genuinely needed.

## Risks / non-goals

### Risks

- **Collision with [`docs/plans/kill-powershell-minimize-toolchain.md`](kill-powershell-minimize-toolchain.md)** — that active plan's Phase 1 converts `build_and_run.ps1` and `build_standalone.ps1` to bash, and Phase 2 ports `scripts/common/SmatchetCMakeCommon.ps1` to `.sh`. This plan adds a **new** root `build.ps1` and a **new** `scripts/common/SmatchetMsvcEnv.ps1` on top of exactly those files. This is a direct collision, not a parity gap. **Mitigation (decided)**: `build.ps1` is a thin ≤60-line dispatcher with zero build logic, so the kill-PS plan re-points its one delegation line at `build-and-run.sh` and keeps the entry-point name; and `SmatchetMsvcEnv.ps1` is added to the kill-PS plan's Phase-2 port list in the same PR that lands it. Slice 1 must not grow build logic into `build.ps1` — that is what would make the collision expensive. **This mitigation is the open decision this plan is grilled on — see § Grill.**
- **`app-controller-fan-in` gate fires** — the new banner TU and `TrackerCredentialProbe` both need `#include "AppController.h"`, and that gate is a **hard-FAIL ratchet-down-only** cap at the current baseline (115), *not* WARN-first. Mitigation: route both through the existing include if one TU can own the `AppController&`, else add `SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=first-run wizard needs the backend factory + dispatcher; owner=<author>; revisit=<date>)` above the include. Budget one gate-argument round in review.
- **MSVC env import mutates process env** — accepted; scoped to the script process, identical to existing `with-msvc.ps1` behaviour.
- **Preset auto-detect picks the wrong toolchain** — mitigated by printing the chosen preset *and the reason*; explicit `-Preset` always wins; test-build-wrapper asserts the table.
- **Wizard and Preferences drift apart** — mitigated by the item-17 extraction: one field-widget helper, one buffer→config copy, one probe implementation. A second copy of any of the three is a review CRITICAL.
- **Existing users see the wizard** — mitigated by the item-11 grandfather migration; the bucket-A matrix covers "config file existed + token present ⇒ no wizard".
- **Two first-run surfaces** — mitigated by flipping `ShowPreferencesWindow = false` on the wizard's first rendered frame (item 18).

### Non-goals

- Do not auto-clear `ReadOnlyMode` on launch. It clears only after a successful save behind a verified probe — the safety default stands until credentials are proven.
- Not replacing the Preferences Tracker tab. The wizard is first-run only; power users keep the full tab.
- No Whisper / MCP / AI-assistant steps in this wizard. Whisper keeps its own banner.
- No Perforce setup in the wizard — tracker backends only.
- No Linux/macOS build wrapper. Windows-first; `QUICKSTART.md` says so explicitly.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `ctest -R TrackerSetupPure`. Matrix: `NeedsSetup` true/false per backend (Jira/Plane/GitHub) × (flag set/unset) × (primary credential empty/present); `ShouldGrandfatherExistingUser` true for "config existed + token present", false for "no config file", false for "config existed + token empty".
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: `tracker_setup_banner` — (1) banner visible when `TrackerSetupCompleted == false`, absent when true; (2) backend radio switches the credential field set; (3) with a fixture factory injected, "Test connection" flips the save button from disabled to enabled on `AuthenticatedReachable`; (4) the save button stays disabled and the "Testing…" cue is visible while the probe is in flight; (5) "Save & connect" sets `TrackerSetupCompleted = true` and clears `ReadOnlyMode`; (6) "Decide later" leaves both untouched.
- **Bash-driver scenario / screenshot / sanitizer**: `powershell -ExecutionPolicy Bypass -File scripts/dev/local/test-build-wrapper.ps1` green including the four new slice-1 cases. Slice-1 end-to-end from an ordinary PowerShell (no VS Developer Prompt): `.\build.ps1 -BuildOnly` succeeds; `.\build.ps1 -RunOnly` prints exe path + PID; `.\build.ps1 -Preset ninja-debug-msvc -BuildOnly` honours the override.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target). Anchored to the new always-on Core TUs (`TrackerSetupPure`, `TrackerCredentialProbe`, `SmatchetTrackerSetupBanner`) — they are glob-picked, so a DX12-side header-pollution break would only surface here.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Visual-validation exception (`AGENTS.md` § Autonomous ship-loop default, exception 5)**: slice 2 touches `Smatchet*Ui*.cpp` + `SmatchetLocalization.cpp`, which normally forces an orchestrator pause for human verification. The bucket-E TU (item 20) provides the coverage that satisfies the exception — **if it is descoped, the pause is mandatory** and the loop must stop with a launched exe.
- **Manual residue**: one human smoke remains — delete `smatchet_config.json`, launch, complete the wizard against a **real** Jira/Plane/GitHub account. Bucket E covers the fixture path only; no CI runner holds live tracker credentials. Deferred-automation action plan: extend the existing local-cache opt-in pattern from `data_dependent_windows_smoke.test.cpp` to a credentialled opt-in lane gated on a developer-local env var, so the real-credential path is at least reproducible on demand. File a `docs/self-improvement/categories/tooling/<date>-credentialled-wizard-smoke.md` entry in the slice-2 PR.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Linear as a wizard backend.** Linear does **not** exist in the tree — the backend combo is `{"Jira", "Plane", "GitHub"}` ([`SmatchetPreferencesUi.cpp:238`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp:238)) and there is no `Source/Core/src/Tracker/*Linear*`. [`docs/plans/linear-tracker-backend.md`](linear-tracker-backend.md) owns that work. The wizard is written so a fourth radio is one `items[]` entry plus one field block, and that plan adds it.
- **A "Test connection" button on the Preferences Tracker tab.** `TrackerCredentialProbe` (item 14) is deliberately factored to make this a small follow-up, but adding the control to the tab is a separate diff and separate visual verification.
- **Postmortem for the false shipped-plan claim.** [`docs/plans/msvc-build-onboarding-hardening.md`](../shipped/msvc-build-onboarding-hardening.md) § Implementation log asserts a bootstrap that never landed. That is a gate escape (a shipped plan's verification claim not matching the tree) and owes a `gate-escape-postmortem` entry naming a preventing gate — most plausibly a checker that greps a shipped plan's impl-log claims against the files it names. Filed as follow-up, not designed here.
- **Bash parity for `build.ps1`.** Owned by [`docs/plans/kill-powershell-minimize-toolchain.md`](kill-powershell-minimize-toolchain.md); see § Risks for the collision mitigation.
- **Onboarding coachmarks for Views / Command Palette / AI panel.** Follow-up "step 2" after the tracker wizard ships.
- **Grid empty-state copy** ("No issues match…"). Separate UX item.
- **`doctor.ps1` / FetchContent timing docs.** See [`docs/plans/first-time-setup-hardening.md`](../shipped/first-time-setup-hardening.md) deferred slices.

## Suggested ship order

| PR | Slice | Est. effort |
|---|---|---|
| PR-1 | Slice 1 (`build.ps1` + MSVC bootstrap) + Slice 3 (QUICKSTART) | ~4-5 h |
| PR-2 | Slice 2 (tracker setup wizard) | ~2-3 days |

PR-1 bundles slices 1 + 3 — one logical feature (the build entry point and the doc that cites it), per `AGENTS.md` § PR batching. PR-2 is independent; until it merges, `QUICKSTART.md` step 4 reads "Settings → Preferences → Tracker", and the slice-2 PR updates that line.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
