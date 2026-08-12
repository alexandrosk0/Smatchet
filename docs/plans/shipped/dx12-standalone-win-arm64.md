# Plan — DX12 standalone renderer + Windows ARM64 standalone port
<!-- plan-date: 2026-06-11 -->

> **Slug**: `dx12-standalone-win-arm64`
>
> **Status**: `shipped`

## Context

Two coupled goals, one plan. (1) Port the standalone app to native Windows
ARM64. (2) Give the Windows standalone a Direct3D 12 render path, defaulting to
it, because the current GLFW + OpenGL 3.0 path is the single biggest ARM64
runtime risk: stock Windows-on-ARM ships OpenGL 1.1 software only, so the GL 3.0
context request in `Source/Standalone/main.cpp` fails unless the user manually
installs the Store-only OpenGL Compatibility Pack (GLon12) — an unacceptable
"black window on first launch" failure mode (Pillar 3). D3D12 is native on
Windows-on-ARM, ships in-box, and has WARP (in-box software rasterizer) as a
universal fallback — which also replaces any Mesa bundle for headless CI
rendering.

The DX12 work is sequenced **on x64 first**: the entire render path is provable
on current dev hardware before any ARM64 hardware enters the picture. The
codebase is otherwise ARM64-clean (zero arch-specific first-party code, all 13
deps source-built, Schannel TLS, /MT static CRT); the port cost is concentrated
in toolchain wrappers, presets, CI legs, and packaging.

After this lands: `SmatchetStandalone` runs natively on Windows ARM64 using
D3D12 by default, with the same binary recipe (and DX12 default) on x64; the
OpenGL path remains compiled-in behind a runtime flag for macOS parity debugging
and burn-in escape-hatch.

Origin: user request 2026-06-11 (ARM64 gap analysis → DX12-as-renderer
assessment → "make the plan"). Prior analysis in session
`235e46c5-85e7-4d91-978a-0f3bd65dacaf`.

## Approach

**Phase 0 — emulation telemetry (cheap, independent).** Add
`IsWow64Process2`-based runtime detection so bug reports distinguish "x64 build
under Prism emulation on ARM64 hardware" from native x64.
`BugReportService.cpp` already has the compile-time `_M_ARM64` arch name; this
adds the runtime half. Ships alone, gives fleet data on how many users are on
WoA today.

**Phase 1 — DX12 standalone renderer, x64 (the core of the plan).** Add a
~400-line D3D12 bootstrap (device, swapchain, RTV descriptor heap, fences,
present loop, WARP fallback, back-buffer readback) modelled on Dear ImGui's
`examples/example_win32_directx12/main.cpp`, plus the existing
`imgui_impl_dx12.cpp` backend already vendored for the Unreal target. Window and
input stay on GLFW via `GLFW_NO_API` + `glfwGetWin32Window()` HWND — keeps all
existing input/monitor/DPI code. Renderer-conditional sites are wider than
"context + swap": both render loops (`main.cpp` and
`StandaloneAppBootstrap::RunRenderLoop`), context-current + vsync init,
per-frame viewport/clear, the GL-info startup log, the `glReadPixels` screenshot
path, and the `ImGuiTestEngine_PostSwap` pump — the full anchor inventory is in
§ Files to modify. Runtime selection: `--renderer=dx12|gl` CLI flag, default
`dx12` on Windows, `gl` elsewhere. The existing `SmatchetImGuiHost_DX12` target
is **not** reused for the standalone: it links `SmatchetCore_DX12` which carries
`SMATCHET_EMBEDDED_IN_UNREAL=1` (compiles out local prefs / bug-report / the F11
full-screen toggle, and compiles IN an Unreal-only overlay-close button — wrong
for a standalone exe). Build mechanism: the backend TU joins `ImGuiLib` itself
on WIN32 (vendored code never compiles inside a strict-warnings target);
`SmatchetStandalone` adds the `d3d12 dxgi d3dcompiler` links. Fix the
silent-NULL window-creation failure while in there (Pillar 3).

**Phase 2 — ARM64 toolchain.** Parameterize the three vcvars wrappers
(`vcvars64.bat` hardcoded today) on an arch argument/env
(`SMATCHET_MSVC_ARCH`, default `x64`, accepted `arm64` → `vcvarsamd64_arm64.bat`
cross / `vcvarsarm64.bat` native); add `ninja-iter-msvc-arm64` +
`publish-msvc-arm64` presets (arch flows from the vcvars environment, same as
existing presets — `android-ndk-arm64` is the precedent for arch-suffixed preset
names); make `cmake/Sanitizers.cmake`'s clang ASan runtime lookup arch-suffixed
(`-x86_64` → `-aarch64` when targeting ARM64).

**Phase 3 — ARM64 validation.** Cross-compile the DX12 standalone from x64;
run the full test suite on a `windows-11-arm` GitHub runner (or physical
hardware if runner unavailable). Risk focus: `std::atomic` memory-ordering
(~310 `memory_order` uses, ~50 relaxed, written under x86-TSO; Android arm64
coverage does not cover Windows-only TUs).

**Phase 4 — CI + packaging.** CI: an ARM64 leg — compile-only cross
(`amd64_arm64`) at minimum, `windows-11-arm` runner for binary-executing jobs
where available; WARP serves headless rendering. Packaging: arch token in
artifact names; second Inno Setup arch
(`ArchitecturesAllowed=arm64`/`ArchitecturesInstallIn64BitMode`) — today's
`x64compatible` installer already covers ARM64 via Prism, so the native
installer is an upgrade, not a gap-fix. Perf baselines get a host/arch key —
ARM64 numbers never compare against x64 baselines.

Trade-off note: keeping GL compiled-in on Windows (vs full switch) doubles the
*theoretical* renderer matrix, but CI/goldens gate **DX12 only** on Windows; the
GL flag is an untested-but-present debug aid (it is the only way to exercise the
macOS/Android-shared GL backend on a Windows dev box). Revisit retiring it after
one release cycle.

## Files to modify

Numbered, grouped by phase. Line anchors verified at `071f2473`.

**Phase 0 — telemetry**
1. [Source/Core/src/Diagnostics/BugReportService.cpp:91](../../../Source/Core/src/Diagnostics/BugReportService.cpp) — `HostArchName()` has compile-time `_M_ARM64`; add `IsWow64Process2`-based "emulated on arm64 host" detection (dynamic `GetProcAddress` — API needs Win10 1709+) surfaced in the bug-report environment block.

**Phase 1 — DX12 renderer**
2. **NEW** `Source/Standalone/Dx12Bootstrap.{h,cpp}` — D3D12 device/swapchain/RTV-heap/fence/present + WARP fallback (`rg -l 'Dx12Bootstrap|D3D12Bootstrap|Dx12Renderer' Source/` → no collision). Whole-file `#ifdef _WIN32` guard mandatory: `STANDALONE_SOURCES` is a `file(GLOB Source/Standalone/*.cpp)` ([CMakeLists.txt:1697](../../../CMakeLists.txt)) compiled on macOS/Linux too; the GLOB lacks `CONFIGURE_DEPENDS`, so adding the TU needs a manual CMake reconfigure. Shape: ImGui `example_win32_directx12` adapted to an externally-created HWND; public surface ≈ `Init(HWND, w, h)`, `BeginFrame(const ImVec4& clearColor)` (clear color sampled per-frame from `ImGuiCol_WindowBg` by the caller — pink-clear contract, see Risks), `RenderDrawData(ImDrawData*)`, `Present(int syncInterval)` (vsync read per frame, mirroring the live `glfwSwapInterval` on-change path), `Resize(w, h)`, `CaptureBackbuffer(std::vector<uint8_t>& rgba, int& w, int& h)` (READBACK-heap staging resource + `Map` → feeds the existing `stbi_write_png` path — DX12 replacement for `glReadPixels`), `Shutdown()`.
3. [Source/Standalone/main.cpp:455-479](../../../Source/Standalone/main.cpp) — renderer selection: GL hint block (455-466) becomes the `gl` branch; `dx12` branch uses `GLFW_NO_API` + `glfwGetWin32Window()`; fix 476-479 so `glfwCreateWindow` NULL produces a logged fatal + OS message box instead of silent exit (Pillar 3, both renderers). Further renderer-conditional anchors in this file: `glfwMakeContextCurrent` (:508), `glfwSwapInterval` boot + live on-change (:512, :598, :933-938 — DX12 maps to `Present(syncInterval)`), GL-info startup log (:556 — DX12 branch logs DXGI adapter description + WARP-vs-hardware), screenshot capture (:652-707 `glReadPixels` → dispatch to `CaptureBackbuffer`), render loop viewport/clear/swap (:785-796), `ImGuiTestEngine_PostSwap` pump (:796-807 — MUST relocate after `Present()` on the dx12 branch or bucket-E screenshot waits hang), `HandleResizeRequest` (:952) + the per-frame `glfwGetFramebufferSize` poll (no framebuffer-size callback exists; DX12 branch compares cached extent → `Resize`).
4. [Source/Standalone/StandaloneAppBootstrap.cpp](../../../Source/Standalone/StandaloneAppBootstrap.cpp) — `--renderer=dx12|gl` CLI flag plumbing (default `dx12` on `_WIN32`, `gl` elsewhere); ImGui backend init split (`imgui_impl_opengl3` vs `imgui_impl_dx12` + shared `imgui_impl_glfw`). Same anchor inventory as main.cpp, second copy: context-current + vsync init (:489-530), `RunRenderLoop` viewport/clear/swap + per-frame framebuffer-size poll (:570-580), `glReadPixels` screenshot (:220-259). Both loops must be converted — bucket-C/E drivers run through this bootstrap, not main.cpp's loop.
5. [CMakeLists.txt:968](../../../CMakeLists.txt) — build mechanism: append `${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp` to `IMGUI_SOURCES` inside a `WIN32` branch (mirrors how :968 adds the GL/GLFW backends) so it compiles inside `ImGuiLib` (lib-level warnings). Explicitly NOT `${IMGUI_DX12_SOURCES}` (:1014 — that GLOB carries *all* imgui core TUs; reusing it on the exe duplicates symbols at link), and NOT a source-append on `SmatchetStandalone` (:1716/:1725-1726 — exe compiles `/W4 /WX`; vendored TUs never compile inside strict-warnings targets, rationale at :1042-1048). `SmatchetStandalone` (:1873) gains `d3d12 dxgi d3dcompiler` links alongside `opengl32` (`imgui_impl_dx12.cpp` calls `D3DCompile`; its `#pragma comment(lib, "d3dcompiler")` self-link is MSVC-only). MinGW/GNU-on-Windows branch (:1876-1906) stays GL-only — `--renderer=dx12` is MSVC-toolchain-only (see Risks). Explicitly NOT linking `SmatchetCore_DX12`/`SmatchetImGuiHost_DX12` (:1550-1571) — `SMATCHET_EMBEDDED_IN_UNREAL=1` (:1375) is wrong for the standalone.
5b. [DockGapSentinelScenario.h:12-15](../../../Source/Core/include/Commands/Scenarios/DockGapSentinelScenario.h) + [ThemeSwitchRoundtripScenario.h:27-29](../../../Source/Core/include/Commands/Scenarios/ThemeSwitchRoundtripScenario.h) — header comments state "the screenshot capture itself is GL-only … DX12 leaves the request flag untriggered"; once `CaptureBackbuffer` lands that's stale — update wording in the same PR.
5c. [.github/workflows/build-and-test.yml](../../../.github/workflows/build-and-test.yml) — bucket-C/E jobs currently scaffold Mesa software-GL for headless rendering. Explicit substrate decision required in the Phase 1 PR: either pin those jobs to `--renderer=gl` (keep Mesa) or switch them to DX12-on-WARP and drop Mesa — never flip the default silently under CI.

**Phase 2 — ARM64 toolchain**
6. [scripts/dev/with-msvc-env.sh:122](../../../scripts/dev/with-msvc-env.sh) — `vcvars64.bat` → arch-parameterized (`SMATCHET_MSVC_ARCH`); vswhere `-requires` gains `VC.Tools.ARM64` when arm64 requested.
7. [scripts/dev/with-msvc.ps1:76](../../../scripts/dev/with-msvc.ps1) — same parameterization.
8. [scripts/dev/local/build-msvc-asan.ps1:62](../../../scripts/dev/local/build-msvc-asan.ps1) — same parameterization (ASan-on-ARM64 itself stays blocked on the 14.38 toolset pin — see Risks).
9. [CMakePresets.json:58](../../../CMakePresets.json) — add `ninja-iter-msvc-arm64` + `ninja-publish-msvc-arm64` configure presets + matching build presets (`inheritConfigureEnvironment: true` + explicit `targets`, same as existing build presets); arch comes from the vcvars env like every existing ninja preset (`android-ndk-arm64` at :321 is the naming precedent). No `testPresets` exist in the file — none owed. The publish-arm64 preset omits `SMATCHET_UNREAL_THIRDPARTY_DIR` (Unreal descoped) and sets `SMATCHET_BUILD_TESTS=ON` so the cross-built test binaries ride to the `windows-11-arm` runner. `vs-unreal-msvc` (:58) stays x64-pinned.
10. [cmake/Sanitizers.cmake:79](../../../cmake/Sanitizers.cmake) — BOTH arch-suffixed runtime lookups: `clang_rt.asan_dynamic-x86_64` (:79) AND `clang_rt.asan_dynamic_runtime_thunk-x86_64` (:83) → suffix selected from `CMAKE_SYSTEM_PROCESSOR`/target (`-aarch64` for ARM64 clang builds).

**Phase 4 — CI + packaging**
11. `.github/workflows/` (build/test workflows) — ARM64 leg: compile-only `amd64_arm64` cross on `windows-2022`; binary-executing jobs on `windows-11-arm` where the runner is available.
12. [scripts/publish/installer/SmatchetStandalone.iss](../../../scripts/publish/installer/SmatchetStandalone.iss) — arm64 arch identifiers (second installer or dual-arch single .iss); artifact names gain an arch token.
13. `docs/perf/baselines/` layout + perf-compare host key — arch-qualified baseline key so ARM64 runs bootstrap fresh baselines instead of comparing against x64.
14. [SmatchetImGuiPlugin.Build.cs:105-119](../../../Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs) — defensive guard only: explicit build error if `Target.Architecture` is ARM64 on Win64 (today it would silently link x64 libs). Unreal ARM64 itself is out of scope.

## Existing utilities reused

- `imgui_impl_dx12.cpp` backend — already vendored + compiled for the Unreal target ([CMakeLists.txt:1015](../../../CMakeLists.txt)); the standalone reuses the same source file, no new third-party code.
- `SmatchetImGuiHost.cpp` DX12 mechanics ([Source/Core/src/Ui/SmatchetImGuiHost.cpp:249,783,836](../../../Source/Core/src/Ui/SmatchetImGuiHost.cpp)) — reference implementation for `ImGui_ImplDX12_InitInfo` wiring (real `ID3D12CommandQueue` requirement) and `RenderDrawData`; not linked (see Approach), but the proven init order is copied.
- ImGui `examples/example_win32_directx12/main.cpp` (vendored ImGui tree) — canonical bootstrap shape for device/swapchain/fence/present.
- `HostArchName()` `_M_ARM64` branch ([BugReportService.cpp:91](../../../Source/Core/src/Diagnostics/BugReportService.cpp)) — compile-time half of arch telemetry already shipped.
- `android-ndk-arm64` preset ([CMakePresets.json:321](../../../CMakePresets.json)) — naming + structure precedent for arch-suffixed presets.
- `imgui_impl_glfw.cpp` — unchanged; GLFW window/input/DPI path is renderer-agnostic under `GLFW_NO_API`.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: improves the floor — native D3D12 vs GL driver (or GLon12 translation on WoA). Renderer swap re-baselines perf numbers; PR-fast scenarios named below; ARM64 gets its own baseline key (never compared cross-arch).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: device/swapchain creation happens at startup before the first frame (splashless window-open path, same as today's GL context creation). Present-loop fence waits are frame-paced, not unbounded; no new sync I/O reachable from draw code.
- **Pillar 3 (never crash)**: net win — removes the GL-3.0-context-fail black-window mode on WoA; adds WARP fallback when hardware D3D12 is absent (VMs, RDP); fixes the existing silent-NULL `glfwCreateWindow` exit ([main.cpp:476](../../../Source/Standalone/main.cpp)) with a logged fatal + message box. D3D12 device-removed (`DXGI_ERROR_DEVICE_REMOVED`) handled as logged fatal with message box in v1 (graceful degradation, not silent UB).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact — renderer swap below ImGui; no UI semantics, layout, or contrast change. Golden screenshots re-baseline for rasterization deltas only.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Diff touches `Source/Core/` (BugReportService) + the render path under it, so gates declared:

1. **PR-fast CI** — **fires**: `idle` + `priority-grid-scroll` (from `scripts/dev/perf-pr-fast-set.json`) exercise the full present loop directly; renderer swap shows up in every scenario's frame totals. New-baseline bootstrap path applies on first DX12-default run (renderer change = intentional baseline shift, baseline-bump PR queued per override rule if the scanner flags it).
2. **Pillar 2 static scanner** — **N/A**: no new sync-I/O reachable from `ImGui::*` draw code; D3D12 init is pre-first-frame startup code in `Source/Standalone/`.
3. **Dispatcher drain** — **N/A**: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — **N/A**: no new > 100 ms sync-stall path; device creation replaces (not adds to) GL context creation at startup.
5. **Marker inventory** — **N/A**: no new `SMATCHET_UI_PERF_SCOPE` markers planned; if Phase 1 review adds present-loop markers, regen `docs/perf/MARKER_INVENTORY.md` in that same PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `idle` + `priority-grid-scroll` before the Phase 1 PR.

**Override**: `perf-out-of-band` label per `AGENTS.md` § Merge gates if the renderer swap moves baselines intentionally (baseline-bump PR queued).

## Risks / non-goals

- **`std::atomic` weak-ordering on ARM64** — ~310 `memory_order` uses / ~50 relaxed written under x86-TSO; Android arm64 coverage misses Windows-only TUs. Mitigation: targeted audit of relaxed/acquire-release sites + full test suite on `windows-11-arm` runner (Phase 3 gate).
- **MSVC ASan unavailable on ARM64 at pinned toolset** — repo pins 14.38; ARM64 ASan needs 14.50-era. Accepted: sanitizer jobs stay x64 (covers arch-independent memory bugs); revisit at next toolset-pin bump.
- **OpenCppCoverage is x64-only** — accepted: coverage job stays x64 permanently.
- **`windows-11-arm` runner availability** — free for public repos; if this repo's plan tier lacks it, Phase 3/4 binary-executing legs degrade to compile-only cross + manual hardware validation (named manual residue below).
- **Golden re-baseline churn** — D3D12 rasterizes differently from GL. Mitigation: one-time bucket-C/E re-approval per `docs/agent-rules/golden-image-approval.md`; this happens under any WoA GL substitute (GLon12/Mesa) anyway, so it is not a differential cost of DX12.
- **Two renderers on Windows** — matrix risk. Mitigation: CI/goldens gate DX12 only; GL flag is explicitly untested-but-present (debug aid); revisit retiring after one release cycle.
- **WARP perf** — software rasterizer won't hold 6.94 ms on big grids. Accepted: WARP is the no-GPU fallback + headless CI substrate, not a perf-gated config; perf gates run on hardware queues only.
- **Pink-clear contract is code, not just doc** — the clear color is sampled per frame from `ImGuiCol_WindowBg` in both render loops ([main.cpp:788-792](../../../Source/Standalone/main.cpp), [StandaloneAppBootstrap.cpp:574-576](../../../Source/Standalone/StandaloneAppBootstrap.cpp)); the DX12 path must carry it through `BeginFrame(clearColor)` into `ClearRenderTargetView`, or pink-clear UI-gap detection silently dies on the default renderer. `debug-techniques.md:7` wording is already renderer-neutral; verify, update only if needed.
- **MinGW/GNU-on-Windows** (CMakeLists:1876-1906 branch) stays GL-only — `--renderer=dx12` rejected at arg-parse with a logged error on that toolchain. Accepted: MinGW is a non-shipping convenience config.
- **Non-goal**: retiring the OpenGL backend (carries macOS `#version 150` path + Android GLES3; Windows keeps it behind `--renderer=gl`).
- **Non-goal**: multi-viewport support — standalone does not enable `ViewportsEnable` (verified: zero matches in `Source/Standalone` + `Source/Core/src/Ui`); single-window DX12 only.
- **Non-goal**: Metal/macOS renderer work.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: existing suite green on x64 DX12 build (renderer-independent) + on ARM64 in Phase 3 — atomics behavior is exactly what bucket A exercises cross-arch. New unit for `--renderer` arg parsing in `CliArgCoercion` tests if the flag lands there.
- **Bucket E (ImGui Test Engine)**: full bucket-E run under `--renderer=dx12` on x64 (Phase 1 exit gate) — UI behavior must be renderer-invariant; goldens re-approved once per `golden-image-approval.md`. Gate detail: `ImGuiTestEngine_PostSwap` must pump after `Present()` (today after `glfwSwapBuffers`, main.cpp:796-807) or screenshot waits hang; CI substrate decision (Mesa-GL pin vs WARP) per Files row 5c lands in the same PR.
- **Bash-driver scenario / screenshot / sanitizer**: screenshot-diff scenarios re-baselined under DX12 (one-time); ASan/UBSan x64 run on the DX12 path (bootstrap code is new C++ — sanitize it where the toolset allows); WARP-forced run (`DXGI` factory `EnumWarpAdapter`) in headless CI proves the no-GPU path.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) + Phase 2 adds arm64 cross-configure/build of `SmatchetStandalone` as a CI leg.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint per the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: grilled 2026-06-11, two passes. Pass 1 (autonomous, codebase-verified): storage-substrate N/A (no persistence claims); terminology pinned — "renderer" = ImGui rendering backend (`imgui_impl_opengl3` vs `imgui_impl_dx12`), "host" = `SmatchetImGuiHost` (Unreal-embedding wrapper, NOT reused here); `SMATCHET_EMBEDDED_IN_UNREAL=1` confirmed target-level on `SmatchetCore_DX12` (CMakeLists:1375); standalone confirmed to own its ImGui init; `ViewportsEnable` confirmed absent. Pass 2 (4-critic adversarial workflow `wf_a9e37084-bfd`, 35-tool-call budget per critic): 16 code-truth claims re-verified at `071f2473`; 1 CRITICAL adopted (screenshot capture is `glReadPixels`-only in BOTH loops → `CaptureBackbuffer` readback added to the bootstrap surface), 5 MAJOR adopted (ImGuiLib build mechanism vs strict-warnings exe; `IMGUI_DX12_SOURCES` GLOB dup-symbol trap; whole-file `#ifdef _WIN32` for the new TU under the macOS/Linux GLOB; both-render-loops + full renderer-conditional anchor inventory; PostSwap-after-Present + CI substrate decision), MINORs adopted (Sanitizers :83 thunk, `d3dcompiler` link, preset `inheritConfigureEnvironment`, vsync live-on-change anchors, pink-clear-is-code); template conformance clean; `test-docs.sh` green at grill time.
- **Manual residue**: if `windows-11-arm` runners are unavailable for this repo, Phase 3 hardware validation is manual on a physical WoA device — deferred-automation plan: stand up the runner leg the moment availability lands; tracked via a `docs/self-improvement/categories/tooling.md` entry in the Phase 3 PR. No other manual steps.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Unreal plugin on Windows ARM64** — UE 5.7 has no native WoA editor; the x64 plugin works under Prism emulation. No-action beyond the Build.cs defensive guard (Files row 14). Revisit when Epic ships native ARM64 editor.
- **ARM64EC** — not pursued (no ASan support, niche interop value; native ARM64 is the target).
- **Retiring Windows GL** — follow-up decision after one release cycle of DX12-default burn-in.
- **macOS Metal backend** — GL stays the macOS renderer; separate plan if Apple GL deprecation ever bites.
- **Mesa/GLon12 bundling** — explicitly dropped; DX12 + WARP replaces the entire compat-pack/Mesa workstream from the original gap analysis.
- **CMakeLists.txt:311 dead branch** — `if(SMATCHET_EMBEDDED_IN_UNREAL)` tests an undefined CMake *variable* (the real mechanism is the target-level compile *define* at :1375); dead code, not load-bearing. Flag in the Phase 1 PR + `docs/self-improvement/categories/debt.md` entry; not fixed here.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

**Phase 0 — telemetry** (PR #1248):
- `af44ffae` · bug-report emulation telemetry — `DetectHostMachine()` (`IsWow64Process2`, dynamic `GetProcAddress` from kernel32) in BugReportService.cpp surfaces `emulated` + `host_arch` env keys; BugReportBody.cpp annotates the OS/Arch markdown row; +2 BuildMarkdownBody doctest cases.

**Phase 1 — standalone DX12 renderer (PR #1255):**
- `4d7f3b41` · DX12 standalone renderer backend — `Dx12Bootstrap.{h,cpp}` pImpl D3D12 device/swapchain/RTV/fence + WARP fallback + SRV allocator + top-left backbuffer capture; renderer-conditional `StandaloneAppBootstrap`/`main.cpp` (init/render-loop/shutdown/screenshot); `Initialize` decomposed into `InitRendererBackend` + `TeardownPartialBoot`; CMake links `d3d12/dxgi/d3dcompiler` + compiles `imgui_impl_dx12.cpp` under `WIN32 AND MSVC`; `ResolveStandaloneRenderer` precedence `--renderer` > `SMATCHET_RENDERER` env > platform default; CI pins `SMATCHET_RENDERER=gl` workflow-wide.

**Phase 2 — ARM64 toolchain + Release/installer (PR #1389):**
- (this PR) · Arch-parameterized the three MSVC env wrappers (`with-msvc-env.sh`, `with-msvc.ps1`, `local/build-msvc-asan.ps1`) on `SMATCHET_MSVC_ARCH` (x64 default; arm64 → `vcvarsamd64_arm64.bat` cross from an x64 host / `vcvarsarm64.bat` native, host detected via `PROCESSOR_ARCHITEW6432`→`PROCESSOR_ARCHITECTURE`) with arch-selected vswhere `-requires` (`VC.Tools.ARM64`). Added `ninja-iter-msvc-arm64` + `ninja-publish-msvc-arm64` configure+build presets (publish omits `SMATCHET_UNREAL_THIRDPARTY_DIR`, sets `SMATCHET_BUILD_TESTS=ON`, builds `SmatchetStandalone`+`SmatchetTests`; arch flows from the sourced vcvars env). Arch-suffixed the clang-cl ASan runtime lookup in `cmake/Sanitizers.cmake` (`-x86_64`→`-aarch64` from `CMAKE_SYSTEM_PROCESSOR`, both names). Parameterized the Inno installer (`MyArchitecturesAllowed`, default `x64compatible`) and added `-Arch arm64` to `release_github.ps1` (selects the arm64 publish preset, `-arm64` artifact token, `arm64` Inno arch; auto-skips Unreal + light). Merged via PR #1389. Post-review hardening: lowercase-normalize `SMATCHET_MSVC_ARCH` in `with-msvc-env.sh`; `-Arch`↔preset-suffix cross-check in `release_github.ps1`; `with-msvc.ps1` uses `[Console]::Error.WriteLine` so `exit 2` is reached under `ErrorActionPreference=Stop`.

**Phase 3 — ARM64 validation (PR #1409):**
- `36b5b297` · dormant `windows-arm64-native` CI leg — native `windows-11-arm` build of `ninja-publish-msvc-arm64` (`SmatchetStandalone`+`SmatchetTests`) then `ctest` (bucket-A doctest rig = the atomics surface); advisory (`continue-on-error`), gated behind repo variable `ENABLE_WIN_ARM64_RUNNER` so it never queues on a tier without the runner. `tooling.md` residue entry updated to "leg wired (dormant)".
- `ad0b34a1` (#1409) · `std::atomic` weak-memory audit — 316 explicit `memory_order` sites (73 relaxed / 99 acquire / 124 release / 20 acq_rel), 46 `memory_order` files + 66 declaring `std::atomic`. **Verdict CLEAN**: every relaxed site is a standalone counter/flag with no cross-variable invariant; every publish/gate of non-atomic data already uses paired acquire/release or a `std::mutex`; the new Windows-only `Dx12Bootstrap.cpp` carries **zero atomics** (fence + mutex only). One contract-fidelity fix applied: the AI streaming-cancel flag was *stored* with `release` (`AiAssistantController.cpp:159/222`) but *read* with `relaxed` in the three SSE clients — six loads `relaxed→acquire` (`AnthropicClient`/`OpenAiClient`/`OllamaClient`), pairing them with the release store and matching the already-correct acquire read at `AiAssistantController.cpp:241`.
- `3f013e05` (#1413) · **first live run** of the `windows-11-arm` leg — added a host-arch assertion (`PROCESSOR_ARCHITEW6432`→`PROCESSOR_ARCHITECTURE`, mirroring `with-msvc.ps1`) so the job hard-fails if the runner is ever silently x64, then flipped the leg live once the user set `ENABLE_WIN_ARM64_RUNNER=true`. Native ARM64 build of `ninja-publish-msvc-arm64` + `ctest` bucket-A doctest rig ran **GREEN on real Windows-on-ARM hardware** — the atomics surface is now runtime-validated on ARM64, not just statically audited.
- `0d2744d8` (#1424) · DX12-on-WARP launch smoke + perf-baseline bootstrap on the live leg — the job now launches `Smatchet.exe` with `SMATCHET_RENDERER=dx12` and presents frames through the DX12 backend on the in-box WARP rasterizer (no GPU on the hosted ARM image), then runs `perf-baseline-bootstrap.py --host ci-windows-arm64` and uploads the seeded baselines as the `perf-arm64-baselines-<run_id>` artifact. **GREEN** — proves the DX12 default path works natively on WoA.
- `368e7d18` (#1426) · native ARM64 Inno installer smoke — `windows-arm64-installer` job: `choco install innosetup` (iscc is x86, runs emulated on ARM64), `release_github.ps1 -Arch arm64 …`, silent install, then a DX12/WARP launch smoke of the installed exe under a 60s `Start-Job`/`Wait-Job` watchdog. **GREEN** after two latent bug fixes (below). Proves the end-to-end ARM64 Release→install→launch path on WoA.

**Phase 4 — CI + packaging (PR #1398):**
- (this PR) · ARM64 advisory CI leg + Unreal ARM64 guard. `build-and-test.yml` gains `windows-msvc-arm64`: a compile-only `amd64_arm64` cross job (`msvc-dev-cmd` `arch: amd64_arm64` sources the cross vcvars → `ninja-iter-msvc-arm64` builds `SmatchetStandalone`), `continue-on-error` advisory, no `ctest` (the cross-built ARM64 binary can't execute on the x64 runner). `SmatchetImGuiPlugin.Build.cs` throws a `BuildException` when a Win64 target's architecture is ARM64 (packaged Win64 libs are x64-only). Row 12 (installer) already shipped in Phase 2; row 13 (perf arch key) deferred — see Deviations.

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

**Phase 0**:
- Plan row 1 named only BugReportService.cpp; the markdown OS/Arch row is hard-coded in **BugReportBody.cpp** (the pure half), so the arch-cell composition + its 2 unit tests landed there too — keeps the rendering unit-testable in the doctest rig (BugReportService.cpp links cpr/host symbols and is excluded from the rig). No behavioural change vs the plan's intent ("surfaced in the bug-report environment block").

**Phase 1:**
- **Renderer precedence gained an env layer** — plan specified `--renderer` flag > platform default; shipped as flag > `SMATCHET_RENDERER` env > platform default. The env knob lets CI pin every standalone launch (incl. the `scenario.run` buried inside `scripts/dev/test-screenshot-diff.sh`) to gl via one workflow-level var instead of threading `--renderer=gl` through every launch site.
- **CI substrate pinned by env, not per-launch flag** (plan row 5c left the mechanism open) — workflow-level `SMATCHET_RENDERER: gl` in `build-and-test.yml`; env inherits to every child exe, so bucket-C/E + mobile-texture-guard + perf-headless stay Mesa-GL byte-stable. A future DX12 CI job overrides via explicit `--renderer=dx12` (flag > env).
- **CMake guard is `WIN32 AND MSVC`, not the plan's literal "WIN32 branch"** — MinGW lacks `_MSC_VER` + the DX12 SDK import libs, so it must stay GL-only; matches the `SMATCHET_STANDALONE_HAS_DX12` compile predicate.
- **`Initialize` decomposed** into file-local `InitRendererBackend` + `TeardownPartialBoot` (not in plan) — the renderer-conditional branch pushed `Initialize` past the 120-line `function-too-long` gate (142L); the shared teardown helper also removes the three-way duplicated failure-unwind.
- **DRY WARNs accepted (calibration phase, non-blocking)** — SRV allocator mirrors `SmatchetImGuiHost.cpp`; `PackRgbDropAlpha`/`CaptureFrameRgb` screenshot helpers duplicated across `StandaloneAppBootstrap.cpp` ↔ `main.cpp` (two standalone loops, one shape). Comment-ratio WARNs on the two scenario headers accepted (renderer-agnostic doc text).
- **Plan kept `active` through Phases 0–4, archived at Phase 3 close** — multi-phase work meant the `## Archive` `git mv` to `shipped/` was deferred from the Phase 1 PR to the final-phase PR. Executed now: all phases shipped and the ARM64 runtime half validated GREEN on real WoA hardware (#1413/#1424/#1426), so Status flips to `shipped` and the file moves to `docs/plans/shipped/` in this PR.
- **Flagged debt** — `CMakeLists.txt` dead `if(SMATCHET_EMBEDDED_IN_UNREAL)` *variable* branch (plan § Out-of-scope) filed to `docs/self-improvement/categories/debt.md` this PR.

**Phase 2:**
- **Phase 4 row 12 (installer) + the release-script arch wiring pulled forward into the Phase 2 PR** — the request was to make Release + installation work for ARM64, not just the dev toolchain, so the `.iss` `MyArchitecturesAllowed` define + `release_github.ps1 -Arch arm64` ship here instead of a later Phase 4 PR. Phase 4 rows 11 (CI legs), 13 (perf-baseline arch key), 14 (Build.cs guard) and all of Phase 3 (hardware validation) still remain.
- **x64 path is byte-identical** — the x64 branch still sources `vcvars64.bat` and emits `x64compatible` with no artifact arch token; the host-arm64→x64 cross batch (`vcvarsarm64_amd64.bat`) is wired but only reached on an ARM64 host, so existing x64 dev/CI/release behaviour is unchanged.
- **arm64 publish build preset targets `SmatchetStandalone` + `SmatchetTests` only** (not `SmatchetLuaTests`) — the preset sets `SMATCHET_BUILD_TESTS=ON` but not `SMATCHET_BUILD_LUA_TESTS`, so the Lua test target isn't defined; dropping it avoids a missing-target build error. The doctest rig is the bucket-A atomics surface Phase 3 cares about.
- **`-Arch arm64` auto-skips Unreal + light standalone** — neither has an arm64 preset this phase (Unreal descoped; no arm64 light preset), so the release run skips them to stay green end-to-end rather than erroring on a missing preset.

**Phase 3:**
- **ARM64 execution shipped dormant in #1409, then activated + run live in #1413** — at #1409 no `windows-11-arm` runner was confirmed, so `windows-arm64-native` shipped gated behind the `ENABLE_WIN_ARM64_RUNNER` repo variable (off by default). Resolution: GitHub's `windows-11-arm` hosted runner is **free on public repos**, the user set `ENABLE_WIN_ARM64_RUNNER=true`, and #1413 added a host-arch assertion and flipped the leg live — the native ARM64 build + `ctest` ran **GREEN on real WoA hardware**. The gate stays in place so the leg is a one-variable toggle if the runner tier ever changes.
- **The audit applied one fix beyond pure validation** — Phase 3 was scoped as validation, but the audit found a genuinely mis-paired acquire/release (the AI-client streaming-cancel flag: stored `release`, read `relaxed`) and corrected it in-scope (six `relaxed→acquire` loads). Strictly safer, cross-platform, zero behavioural change on x64; the rest of the ~310-site surface needed no change.
- **DX12-on-WARP + `-Arch arm64` installer smokes landed on the live leg (not deferred)** — once the runner was live, both follow-ons that #1409 had documented as future work were authored and run GREEN: the DX12-on-WARP launch smoke + perf bootstrap (#1424) and the ARM64 Inno installer smoke (#1426). The plan's biggest stated ARM64 risk — "black window on first launch" from the GL 3.0 path — is now closed by a real DX12-on-WARP present on hosted WoA, with no GPU present.
- **The installer smoke surfaced two latent ARM64-Release bugs, fixed in #1426** — (1) `release_github.ps1` built the `iscc` arg array with inline `$(if …)` expressions that left empty `""` elements on an unsigned build, so `iscc` aborted with "Unknown option:"; rewritten to append to an `$isccArgs` array only when signing is enabled. (2) `SmatchetStandalone.iss` `SetupIconFile` still pointed at the pre-#552 `Source\Standalone\smatchet.ico` *root-relative* path that no longer resolved from the installer dir, so `iscc` failed on line 49 "cannot find the path"; corrected to the real `..\..\..\Source\Standalone\smatchet.ico`. Both were dormant since their respective changes and only an actual ARM64 packaging run exercised them.
- **Perf baselines bootstrapped in CI; seed-file commit intentionally left for a human-reviewed bootstrap PR** — #1424 runs `perf-baseline-bootstrap.py --host ci-windows-arm64` and uploads the six `docs/perf/baselines/*.ci-windows-arm64.json` files as a run artifact. Committing them is deferred **by design**: the Phase 4 row-13 plan already specifies the first ARM64 baselines bootstrap fresh and land via a human-reviewed bootstrap PR (a single-sample CI seed should be eyeballed before it becomes a regression gate), and the repo owner confirmed 2026-06-19 to leave them for human review. (Fetching the artifact from this sandbox is egress-blocked regardless — the allowlist rejects `*.blob.core.windows.net`, HTTP 403 — so it would be a human commit either way.) The baselines exist and are correct (the job is green); the ARM64 perf path is proven. Tracked in `docs/self-improvement/categories/tooling.md`.

**Phase 4:**
- **Row 13 (perf-baseline arch key) deferred to Phase 3** — `perf-baseline.sh` already keys files `<scenario>.<host>.json` with a free-form `--host` label (`dev`, `ci-windows-latest`), so an arch-qualified key is a *naming* convention, not a code change: the future ARM64 perf job passes `--host=ci-windows-arm64` and bootstraps fresh baselines. No ARM64 perf run exists until a `windows-11-arm` runner lands (Phase 3), so wiring it now would be speculative + untestable. Documented; implemented alongside the first ARM64 perf run.
- **Row 11 leg is compile-only + advisory** — plan said "compile-only cross (`amd64_arm64`) at minimum"; shipped exactly that (`continue-on-error: true`, no `ctest`) because cross-built ARM64 binaries can't execute on the x64 `windows-2022` runner. The binary-executing `windows-11-arm` leg + the atomics test run remain Phase 3 (gated on runner availability; tracked in `docs/self-improvement/categories/tooling.md`).
- **Row 14 detects arch via `Target.Architecture.ToString()`** — works whether UBT exposes `Architecture` as an `UnrealArch` (UE 5.2+) or the legacy string, and an empty/x64 value is a no-op, so the guard is version-robust and never fires on the supported x64 path.

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

**Phase 0** (at `af44ffae`, rebased onto current `origin/develop`):
- **Build** — PASS: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12 SmatchetTests` green pre- and post-rebase (dual-target verified: standalone compiles the Win32 `DetectHostMachine` path; `SmatchetCore_DX12` links).
- **Tests** — PASS: full doctest rig 1704 cases / 15765 assertions, 0 failed; the 2 new BuildMarkdownBody emulation cases pass.
- **Lint** — PASS: `test-lint-rules.sh --diff origin/develop` clean (strict-zone, comment-noise, no-raw-new/deviation-overdue/no-detach, GLFW-in-Core-headers, CMake-FATAL all green; narrowing-conversions skipped — opt-in).
- **Not run** — perf gates (Phase 0 adds no draw-code path; `DetectHostMachine` is a one-shot bug-report-time call, not a per-frame path); ASan/UBSan (deferred to CI).

**Phase 1 (commit `4d7f3b41`, worktree `C:\Dev\trees\dx12-phase1`):**

| Gate | Command | Result |
|---|---|---|
| Standalone build (GL + DX12 backend linked) | `with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` | **PASS** — `Smatchet.exe` linked, EXIT=0 |
| DX12 dual-target compile | `… --target SmatchetCore_DX12` | **PASS** — `SmatchetCore_DX12.lib`, EXIT=0 |
| Unit + Lua tests | `ctest --test-dir build/ninja-test-msvc` (built `SmatchetTests` + `SmatchetLuaTests`) | **PASS** — 7/7 100% |
| Lint (delta vs `origin/develop`) | `agents/scripts/project/test-lint-rules.sh --diff origin/develop` | **PASS** EXIT=0 — WARN-only (4 DRY copy-paste, 2 comment-ratio, 1 soft func-size; all non-blocking, see Deviations) |
| Docs | `scripts/dev/test-docs.sh` | **PASS** — 13/0 |

**NOT-RUN / deferred (not a Phase-1 regression — tracked to later phases):**
- **DX12 runtime render smoke on physical Windows-on-ARM hardware** — Phase 3 residue; no WoA runner / device available this session. The build links DX12 + WARP fallback but no frame has been presented through the DX12 backend on real hardware.
- **DX12 visual parity under bucket-C/E** — CI pins `SMATCHET_RENDERER=gl` to keep Mesa goldens byte-stable, so the bucket goldens never exercise the DX12 backbuffer-capture path. DX12 pixel output is therefore **unverified in CI**. Local manual smoke (`Smatchet.exe --renderer=dx12`) on x64 Windows is the only available check this session and is offered in the post-ship menu.

**Phase 2 (PR #1389) — authored on Linux; the change is Windows/MSVC-only (vcvars, clang-cl ASan, Inno Setup), so no compile/link happened that session — validated live in Phase 3 (#1413/#1426):**

| Gate | Command | Result |
|---|---|---|
| Shell syntax | `bash -n scripts/dev/with-msvc-env.sh` | **PASS** |
| Presets parse + new presets present | `python3 -c "json.load(...)"` (4 new presets asserted) | **PASS** |
| Docs | `scripts/dev/test-docs.sh` | **PASS** |

**NOT-RUN / deferred (needs a Windows / Windows-on-ARM toolchain — Phase 3):**
- Cross-configure+build `ninja-publish-msvc-arm64` from an x64 host (`SMATCHET_MSVC_ARCH=arm64`); run `SmatchetTests` on a `windows-11-arm` runner (atomics audit); build the ARM64 Inno installer (`-Arch arm64`) and smoke-install on WoA.
- PowerShell wrappers (`with-msvc.ps1`, `build-msvc-asan.ps1`, `release_github.ps1`) are unparsed here — no `pwsh` on the Linux host; syntax to be confirmed on Windows.

**Phase 3 — the atomics audit is static analysis (PR #1409); the ARM64 *runtime* half ran live + GREEN on a `windows-11-arm` hosted runner (#1413/#1424/#1426):**

| Gate | Command | Result |
|---|---|---|
| `std::atomic` weak-memory audit (full tree) | read of all 46 `memory_order` files + 66 `std::atomic` decls | **PASS — CLEAN**: no x86-TSO-dependent relaxed publish; `Dx12Bootstrap.cpp` has zero atomics; one `relaxed→acquire` fix applied (AI-client cancel reads) |
| Native ARM64 build + `ctest` on real WoA hardware (bucket-A doctest rig = the atomics surface) | `windows-arm64-native` job on `windows-11-arm`, `ENABLE_WIN_ARM64_RUNNER=true` (#1413) | **PASS** — host-arch assertion confirms a genuine ARM64 runner; build of `ninja-publish-msvc-arm64` + `ctest` green. The atomics surface is now runtime-validated on ARM64. |
| DX12-on-WARP launch smoke (DX12 default path presents a frame on the in-box software rasterizer, no GPU) | `SMATCHET_RENDERER=dx12` launch in `windows-arm64-native` (#1424) | **PASS** — closes the plan's headline "black window on first launch" ARM64 risk on real WoA. |
| ARM64 perf-baseline bootstrap | `perf-baseline-bootstrap.py --host ci-windows-arm64`, uploaded as `perf-arm64-baselines-<run_id>` artifact (#1424) | **PASS (run)** — six baselines seeded green in CI; *seed-file commit intentionally left for a human-reviewed bootstrap PR* (owner decision 2026-06-19), see Deviations. |
| End-to-end ARM64 Release → install → launch | `windows-arm64-installer` job: `choco install innosetup` → `release_github.ps1 -Arch arm64` → silent install → DX12/WARP launch smoke under a 60s watchdog (#1426) | **PASS** — after fixing two latent ARM64-Release bugs (iscc empty-args + stale icon path), see Deviations. |
| Workflow YAML parses + jobs well-formed (`runs-on: windows-11-arm`, gate, `continue-on-error`) | `python3 -c "yaml.safe_load(...)"` | **PASS** |
| Lint (delta vs `origin/develop`) | `agents/scripts/project/test-lint-rules.sh --diff origin/develop` | **PASS** |
| Doc validation | `scripts/dev/test-docs.sh` | **PASS** — 13/0 |

**Deferred by design (one residual — process, not code):**
- **Commit of the bootstrapped `ci-windows-arm64` perf baselines** — the six `docs/perf/baselines/*.ci-windows-arm64.json` files exist as the green #1424 run artifact; their commit is **intentionally left for a human-reviewed bootstrap PR** (Phase 4 row-13 design — a single-sample CI seed gets eyeballed before it becomes a regression gate; repo-owner decision 2026-06-19). Fetching the artifact from this sandbox is egress-blocked regardless (`*.blob.core.windows.net`, HTTP 403), so it is a human commit either way. The ARM64 perf path itself is proven by the green bootstrap run; only the human-reviewed seed-file commit remains. Tracked in `docs/self-improvement/categories/tooling.md`.

**Phase 4 (PR #1398) — authored on Linux; the `amd64_arm64` cross leg has since run GREEN in CI on every PR:**

| Gate | Command | Result |
|---|---|---|
| Workflow YAML parses + `windows-msvc-arm64` job well-formed (`arch: amd64_arm64`, `continue-on-error`) | `python3 -c "yaml.safe_load(...)"` | **PASS** |
| `amd64_arm64` cross configure+build of `SmatchetStandalone` | `windows-msvc-arm64` advisory job in CI | **PASS** — green since #1398 merged; the ARM64 toolchain wiring compiles the standalone from an x64 host. |
| Doc validation | `scripts/dev/test-docs.sh` | **PASS** |
| Lint (delta vs `origin/develop`) | `agents/scripts/project/test-lint-rules.sh --diff origin/develop` | **PASS** |

**NOT-RUN / deferred (one residual — no UBT in CI):**
- `SmatchetImGuiPlugin.Build.cs` ARM64 guard is uncompiled (no UBT / UE build on these runners); it is a defensive throw on an out-of-scope path (x64 unaffected), and UE on WoA is explicitly Out-of-scope (no native ARM64 editor). The binary-executing `windows-11-arm` leg + ARM64 perf baselines that this block previously deferred are **now done in Phase 3** (#1413/#1424/#1426).
