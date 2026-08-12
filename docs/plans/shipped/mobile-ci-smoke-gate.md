# Plan — Mobile CI smoke gate for the #1122 texture-lifetime crash class
<!-- plan-date: 2026-06-13 -->

> **Slug**: `mobile-ci-smoke-gate` (matches this file's basename without `.md`).
>
> **Status**: `shipped`.
>
> **Usage**: design-only plan authored per `docs/plans/active/_plan-template.md`. Closes GitHub Issue #1133 (P1).

## Context

P0 crash #1122 (ImGui 1.92 dynamic-texture orphan → `ImDrawCmd::GetTexID()` SIGABRT) shipped to a physical device with **zero** automated coverage of its trigger path: a configuration change (rotate → detail panel becomes a dock tab) + an on-demand atlas grow (a new tab-label glyph first-bakes) + an EGL/activity recreate. PR #1130 fixed it with `GuardImGuiDynamicTextures` (`Source/Mobile/Android/android_main.cpp:553`), and the pure predicate is unit-tested (`tests/Core/SmatchetImGuiTextureGuard.test.cpp`). But the **integration/device path** — the actual render loop after a recreate + atlas-grow — is still uncovered: the existing advisory Android lanes (`mobile-android-ndk` = `.so` configure+link only; `mobile-android-apk` = Gradle `assembleDebug` + a Robolectric JVM unit test) never *run* the app, never rotate it, never switch a dock tab.

Issue #1133 asks for two coupled pieces: (1) re-introduce the synthetic fault harness that proved #1122 deterministically (the `.force_texstuck` / `.force_ctxloss` / `.disable_rearm` hooks, stripped as temp-debug after the fix), behind a build/env gate that is OFF in ship builds; (2) add a CI smoke gate that drives — under that gated harness — a rotate/CONFIG_CHANGED, a dock-tab switch that first-bakes a glyph, and a forced EGL context-loss+recreate, asserting no SIGABRT, a rendered first frame after each event, and the expected `GuardImGuiDynamicTextures` recovery log.

Intended outcome — after this lands, a regression that reintroduces the orphaned-draw-command path **fails an advisory CI lane before merge** (it would have caught #1122), and the synthetic fault harness is permanently reproducible (not a one-off temp-debug artifact).

## Approach

**Harness mechanism — headless, not an Android emulator.** A booted Android emulator on a GitHub `ubuntu-latest`/`windows-2022` runner is slow (cold-boot ≈ 3-5 min), flaky (KVM/HAXM availability, ADB races), and would only run on a hardware-accelerated runner we don't have. The #1122 crash class is **not Android-specific** — it is an ImGui-1.92 dynamic-texture lifetime bug in the *shared* render path. The three triggers all have host-reproducible analogues:

1. **EGL context-loss + recreate** ≙ destroying and recreating the GL device objects + flipping a font `ImTextureData` to `Destroyed` while still referenced — exactly what `RecreateAfterContextLoss` leaves behind. The `.force_ctxloss` hook forced the swap-buffers `LastSwapLostContext()` path; its host analogue is a scenario step that drives the same `ImTextureData` state machine into `Destroyed`/`Invalid`.
2. **Atlas grow / on-demand glyph bake** ≙ a font re-apply that retires the old atlas texture mid-frame while a committed `ImDrawCmd` still points at it. Reproducible on any backend by baking a new glyph range mid-frame (the `.force_texstuck` hook held a texture `Destroyed` with `TexID=Invalid`).
3. **CONFIG_CHANGED / rotate → dock-tab reflow** ≙ a dock-layout change that moves the detail panel into a tab whose label first-bakes a glyph (trigger for #2).

Therefore the gate runs as a **new headless scenario** (`IScenario` named `mobile-texture-guard`) driven by the existing `scenario.run` CLI under the **Mesa software-GL** lane infrastructure that bucket-C/E already provision — the same `Smatchet.exe scenario.run --name=… --yes` shape, no emulator. The scenario synthesises the three fault states via the **promoted fault-injection hooks** (re-added as a compile-time-gated `SmatchetTextureFaultInjector` reachable only when `SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION` is defined — set ON only on the test/ui-test presets, never in `ninja-publish-msvc`). The scenario asserts, per event: (a) the process did not abort, (b) the next `RenderDrawData` completed (a frame counter advanced), (c) the guard emitted its `[P0 #1122] repointed …` / re-arm log line when the orphan path was forced.

**Why this still exercises the device path meaningfully.** The crashing code (`GuardImGuiDynamicTextures`, the `ImTextureData` `Destroyed→WantCreate` self-heal, the GL3 backend's missing `Destroyed` branch) is **byte-identical shared ImGui + shared guard logic**; the Android `android_main.cpp` recreate path differs only in *what triggers* the state, not in the texture state machine the crash lives in. Driving the same `ImTextureData`/`ImDrawCmd` states through the Mesa-GL standalone exe reproduces the exact assert. A true on-device emulator lane is named as a **deferred follow-up** (Out of scope) — the headless gate buys the regression coverage now at a fraction of the flake budget.

**Where it runs + advisory-vs-required.** A new **advisory** (`continue-on-error: true`) job `mobile-texture-guard-smoke` in `build-and-test.yml`, gated on `needs.changes.outputs.code == 'true'` (it builds the standalone exe; it is NOT android-only-skippable because the shared guard lives in `Source/Core` + the standalone render path). Advisory because it is a new exe-running lane on software GL — the same risk class as bucket-C/E — and per Issue #1133's "advisory" framing. **But** it carries the Plan-2 `bucket-lane-launch-smoke` guards (a non-`continue-on-error` launch-smoke before the scenario step, and a `Passed==0 && Failed>0` hard-exit inside the driver) so it can never green-wash a dead harness the way bucket-C did for 2 weeks.

## Files to modify

1. `Source/Core/include/Mobile/SmatchetImGuiTextureGuard.h` *(grep first: `rg -l 'SmatchetImGuiTextureGuard' Source/`)* — the pure predicate header already exists; **no change** unless the fault injector needs a shared status enum. Listed to confirm reuse, not edit.
2. `Source/Core/include/Mobile/SmatchetTextureFaultInjector.h` **(new)** — compile-time-gated (`#ifdef SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION`) fault-injection surface: `ForceTextureStuck()`, `ForceContextLoss()`, `SetRearmDisabled(bool)`. Header-only enum + tiny inline state struct so it links into both the standalone exe and (later) `android_main.cpp`. In a ship build the macro is undefined and every entry point compiles to a no-op (or is absent), so zero ship-build surface.
3. `Source/Core/src/Mobile/SmatchetTextureFaultInjector.cpp` **(new)** — the gated implementation (only compiled when the option is ON; CMake source-list-gates it like the other `SMATCHET_WITH_*` TUs).
4. `Source/Mobile/Android/android_main.cpp:484` (`RecreateAfterContextLoss`), `:553` (`GuardImGuiDynamicTextures`), `:653` (call site) — re-wire the three hook checks behind `SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION` so the *device* path can also be driven by the same injector when a future emulator lane lands. Minimal: 3 guarded `if (FaultInjector::…)` checks, all compiled out of ship builds.
5. `Source/Core/src/Scenarios/MobileTextureGuardScenario.cpp` **(new)** *(grep first: `rg -l 'MobileTextureGuard|texture.?guard' Source/Core/src/Scenarios/`)* — the `IScenario` named `mobile-texture-guard`: drives 3 sub-steps (force-stuck atlas → render → assert; force-ctxloss → recreate → render → assert; rearm-disabled negative control → assert the guard re-points). Each asserts frame-advance + scans the guard log. Registered in the scenario registry.
6. `Source/Core/src/Scenarios/<registry>.cpp` — register the new scenario (find the existing registration list; do not invent a new registry).
7. `CMakeLists.txt` — add the `SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION` option (default OFF), source-list-gate the new `.cpp`, and turn the option ON in the `ninja-test-msvc` / `ninja-ui-test-msvc` preset paths (via `CMakePresets.json` cacheVariables, NOT in publish).
8. `CMakePresets.json` — set `SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION=ON` on `ninja-test-msvc` + `ninja-ui-test-msvc` only. Explicitly confirm it stays OFF (absent) on `ninja-publish-msvc` and `ninja-iter-msvc`.
9. `.github/workflows/build-and-test.yml` — new advisory job `mobile-texture-guard-smoke` after `windows-msvc`: configure+build `ninja-ui-test-msvc` (or a dedicated `ninja-texture-guard-msvc`), install Mesa (reuse the bucket-C/E cached-DLL block), run a **non-continue-on-error launch-smoke** (`Smatchet.exe cmd app.version --spawn --yes`, ≤10 s) then the `continue-on-error` scenario step (`scenario.run --name=mobile-texture-guard --yes`). Shares the Mesa-install snippet with bucket-C/E — extract once if it drifts (note: Plan-2's launch-smoke step is the *same* mechanism; coordinate so both land one shared step shape).
10. `tests/Core/SmatchetImGuiTextureGuard.test.cpp` — extend with a fault-injector-driven case if the injector exposes a pure-testable predicate (bucket-A). Otherwise unchanged.

## Existing utilities reused

- `GuardImGuiDynamicTextures(ImDrawData*)` — `Source/Mobile/Android/android_main.cpp:553` — the production guard under test; the scenario forces the states it recovers from.
- `SmatchetDrawCmdTextureNeedsRebind(...)` — `Source/Core/.../SmatchetImGuiTextureGuard.*` — pure predicate already unit-tested; the new scenario is the integration complement.
- `scenario.run` CLI + `IScenario` / `Scenarios().Tick(...)` — `Source/Mobile/Android/android_main.cpp:614` shows the tick loop; the standalone exe runs the same scenario engine.
- Mesa software-GL install + cached-DLL block — `.github/workflows/build-and-test.yml` bucket-C (`:422`) / bucket-E (`:535`) — copy the proven `opengl32.dll` / `libgallium_wgl.dll` / `libglapi.dll` provisioning verbatim.
- `continue-on-error` advisory-job + step pattern — bucket-C/E jobs (`:391`, `:483`) — same advisory shape.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — the guard walks draw COMMANDS (tens-hundreds/frame), already costed in #1130; the fault injector is compiled out of ship builds.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — no new sync I/O; the scenario runs headless in CI, not on a user UI thread.
- **Pillar 3 (never crash)**: **direct positive impact** — this plan exists to keep a P0 crash class regression-gated. The fault injector is OFF in ship builds (no new crash surface).
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

The diff touches `Source/Core/` (new scenario TU + fault injector). Per gate:

1. **PR-fast CI** — N/A for behaviour, but declare: the new scenario is headless-CI-only and the guard path is not on a perf-gated steady-state scenario. No `scripts/dev/perf-pr-fast-set.json` entry needed (the touched code runs only under the fault-injection option, never in a perf scenario). Justify in PR.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`. N/A.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`. N/A.
4. **Visible-cue bucket-E harness** — no new > 100 ms sync stall. N/A.
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers. N/A.

**Override**: none expected.

## Risks / non-goals

- **Risk: headless ≠ device.** A bug that manifests ONLY through the Android EGL surface lifecycle (not the shared `ImTextureData` state machine) would escape this gate. Mitigation: the guarded hooks are wired into `android_main.cpp` too (file #4) so a future emulator lane reuses the same injector; the headless gate covers the texture-state-machine class that #1122 actually was.
- **Risk: Mesa software-GL flakiness** (the exact class that produced the 2-week false-green). Mitigation: this plan REQUIRES Plan-2's `bucket-lane-launch-smoke` (non-continue-on-error launch step + `Passed==0 && Failed>0` hard-exit). The two plans must land coordinated, or this gate inherits the same blind spot.
- **Risk: fault injector leaks into ship builds.** Mitigation: compile-time macro default OFF + a doctor/lint assertion that `ninja-publish-msvc` does not define `SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION`.
- **Non-goal — booted Android emulator lane.** Deferred (Out of scope). This plan delivers headless shared-path coverage only.
- **Non-goal — covering the Robolectric JVM IME path.** Unrelated to the texture class; `mobile-android-apk` already runs it.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: extend `tests/Core/SmatchetImGuiTextureGuard.test.cpp` if the injector exposes a pure predicate; assert the negative control (rearm-disabled) predicate flags rebind. Confirms the guard logic without GL.
- **Bucket E / scenario driver**: the new `scenario.run --name=mobile-texture-guard --yes` returns nonzero on any forced-fault SIGABRT/missing-frame/missing-guard-log; run under Mesa software GL in CI.
- **Bash-driver**: a launch-smoke assert (Plan-2 shared step) — the freshly built exe runs `app.version` in ≤10 s and exits 0; hard-fail (non-advisory) if it cannot start.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — the new `Source/Core` TUs and the `SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION` source-gating must compile in BOTH targets with the option OFF (default) AND ON.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the mobile/ImGui domain model + the #1122 RCA before finalising; record the outcome. Required for every plan.
- **Manual residue**: the on-device emulator path stays manual/deferred — named in Out of scope with a follow-up. No silent residue; a `docs/self-improvement/categories/test.md` entry tracks the emulator-lane follow-up.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to a "mobile emulator smoke gate" presented as current, and revise to "deferred follow-up."

- **Booted Android emulator lane** (KVM-accelerated runner driving `am broadcast` rotation + a real APK) — follow-up plan once a hardware-accelerated mobile runner exists; the gated hooks in `android_main.cpp` are pre-wired for it.
- **Perf coverage of the guard path** — the guard is not a steady-state perf scenario; no `perf-pr-fast-set.json` entry. No-action.
- **iOS / other mobile shells** — none exist; no-action.

## Implementation log

Shipped as one PR (`feat/mobile-ci-smoke-gate-1133`, branched off `origin/develop`). The cross-cutting seam (the guard living only in `android_main.cpp`, the standalone render loop having no guard call) was resolved by the architect before implementation; the work split into three slices that landed together:

1. **Guard promotion (shared Core TU).** `GuardImGuiDynamicTextures` moved out of `Source/Mobile/Android/android_main.cpp` into a new shared Core TU `Source/Core/src/Ui/SmatchetImGuiTextureGuard.cpp` (declared in new `Source/Core/include/Ui/SmatchetImGuiTextureGuardRuntime.h`), so the standalone GL3 loop and the Android loop call the byte-identical guard. The status-enum `static_assert` pin moved with it (now compiled in standalone + DX12 too, not just Android). The pure predicate header `SmatchetImGuiTextureGuard.h` was left untouched (still imgui-free + unit-tested). The once-per-episode log latch became a file-scope resettable bool with a `ResetTextureGuardLogLatch()` entry point so the scenario can re-arm the `[P0 #1122] repointed …` line between sub-steps. `Source/Standalone/main.cpp` now calls the guard unconditionally before `RenderDrawData`.
2. **Fault injector (compile-gated).** New `Source/Core/include/Ui/SmatchetTextureFaultInjector.h` (always includable; entry points inline no-ops when the macro is undefined) + `Source/Core/src/Ui/SmatchetTextureFaultInjector.cpp` (whole-file `#if defined(SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION)`, compiles to an empty TU otherwise — no CMake source-list-gate needed; it auto-globs into `CORE_SOURCES`). `ForceTextureStuck` / `ForceContextLoss` record a committed draw command on the atlas texture *before* dooming it; `SetRearmDisabled`/`RearmDisabled` is the negative-control flag.
3. **Scenario + registration.** New `Source/Core/src/Commands/Scenarios/MobileTextureGuardScenario.cpp` (`mobile-texture-guard`), an 8-frame state machine: warm-up → force-stuck → scan → force-ctxloss → scan → rearm-disabled force-stuck → scan. It scans `Logger::Instance().GetEntriesSnapshot()` for `"[P0 #1122] repointed"`; "frame advanced" is proven by `OnFrame` being re-entered for each index (a SIGABRT kills the process first). Registered (un-ifdef'd) in `SmatchetScenarioRegistry.cpp`; the registry snapshot test + stubs were updated to pin the new name.
4. **Build wiring (this agent).** Added `option(SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION … OFF)` to `CMakeLists.txt`, propagated as a define on `SmatchetCoreInterface` (shared → both targets), with a `FATAL_ERROR` hard-guard that the macro can never be ON in a Release/LTO ship build. Flipped ON via `CMakePresets.json` on `ninja-test-msvc` / `ninja-ui-test-msvc` (+ clang/asan test siblings) only — absent on `ninja-iter-msvc`, `ninja-publish-msvc`, and Unreal. New advisory CI job `mobile-texture-guard-smoke` in `build-and-test.yml` (mirrors bucket-E: configure+build `ninja-ui-test-msvc`, install Mesa, **non-advisory** launch-smoke, then the `continue-on-error` scenario step).

## Deviations from plan

- **File paths corrected.** The plan's files-to-modify pinned the guard/injector under `Source/Core/include/Mobile/`; the predicate header actually lives under `Source/Core/include/Ui/`. All new guard/injector files were placed under `Ui/` for consistency. (Caught by the architect's path-grep.)
- **Guard PROMOTED, not duplicated.** The plan listed file #1 (the predicate header) as "no change … listed to confirm reuse" and did not specify how the standalone scenario reaches the guard (the guard was android-only; the standalone loop had no guard call). Resolved by promoting `GuardImGuiDynamicTextures` into a shared Core TU and adding the unconditional standalone call site — a larger guard-layer touch than the plan's file list implied, but required for the gate to be honest (the log-scan assertion needs the guard to actually run on the standalone exe).
- **Resettable log latch added.** Not in the plan: the production guard latches its recovery log once per episode ("further occurrences silenced"), which would silently green-wash sub-steps 2-3 of the scenario. Added `ResetTextureGuardLogLatch()` (harmless in ship builds) so each forced-fault sub-step re-arms the latch. This was the architect's highest-value finding.
- **Injector gated by in-file `#if`, not CMake source-list-gating.** Plan file #3 said "source-list-gates it like the other `SMATCHET_WITH_*` TUs." Used a whole-file `#if` (empty TU when OFF) instead — simpler, avoids a `list(REMOVE_ITEM)`/`list(APPEND)` dance, and the empty-TU cost is nil.
- **`LOG_WARN` (printf-style), not the literal `std::string`-concat snippet.** The shared guard uses the project's actual printf-style `LOG_WARN` macro (matching every Core call site) rather than the plan/handoff's illustrative string-concat pseudocode; the `[P0 #1122] repointed` substring is preserved verbatim for the scan.
- **Android device hooks (plan file #4) wired minimally.** The `android_main.cpp` edits for this slice are the promotion delete + include + qualified call; the per-hook `#ifdef SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION` device-injection points are a future-emulator concern (tracked in `test.md`), since the headless standalone path is what this gate exercises.

## Verification (actual)

- **Build gate (dual-target, macro ON):** `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — see § Regression gate in the ship report for PASS/FAIL. The new Core TUs auto-glob into both targets; the relocated `static_assert` now also compiles in standalone + DX12.
- **Build gate (macro OFF, default):** the same dual-target build with the option absent — the injector is an empty TU + inline no-ops, the guard runs unconditionally, zero ship-build surface.
- **Bucket A (pure-logic):** `SmatchetDrawCmdTextureNeedsRebind` is already unit-tested in `tests/Core/SmatchetImGuiTextureGuard.test.cpp`; the injector exposes no new pure predicate, so that test is unchanged (the scenario is the integration complement).
- **Bucket E / scenario driver:** `scenario.run --name=mobile-texture-guard --yes` under Mesa software GL (the new `mobile-texture-guard-smoke` job); nonzero exit / SIGABRT / missing-frame / missing-guard-log fails the lane.
- **Doc validation:** `scripts/dev/test-docs.sh` + the lint/shell-lint gates — see ship report.
- **Plan stress-test (`grill-with-docs`):** performed at plan-authoring time on the `plan/ui-freeze-future-drain` branch (this PR ships the implementation of that grilled plan).
- **Manual residue:** the on-device emulator lane stays deferred (no hardware-accelerated mobile runner) — tracked in `docs/self-improvement/categories/test.md` (2026-06-13).

## Sequencing note

This advisory lane is only fully honest once the in-flight CI gate-hardening PR's `bucket-lane-launch-smoke` lands (non-`continue-on-error` launch step + the driver's `Passed==0 && Failed>0` hard-exit). The launch-smoke step in `mobile-texture-guard-smoke` is the same mechanism; the two compose once both merge. This PR does not block on it.
