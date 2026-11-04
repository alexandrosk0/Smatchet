# Plan — DX12 backbuffer readback screenshot diff (PR-19)

> **Slug**: `dx12-backbuffer-readback-screenshot` (matches this file's basename without `.md`).
>
> **Status**: `active` — from `docs/plans/backlog-pr-roadmap.md` PR-19 (`dx12-backbuffer-readback-screenshot-diff`). Owner: unreal-bridge area (`Source/UnrealPlugins/SmatchetImGuiPlugin/README.md`). **Hardware-gated**: authoring and verification need an Unreal/DX12 build environment (not available in a Linux web session). Related: `docs/plans/dx12-standalone-win-arm64.md`.

## Context

The standalone GLFW build has a screenshot-diff harness (bucket-E / `DockGapSentinelScenario` etc.), but the Unreal-embedded DX12 render path has **no** frame-capture: today only a Slate-backbuffer *log line* exists in the plugin, so a DX12-only rendering regression (dock gaps, clear-color, ImGui-on-Slate compositing) cannot be caught by an automated pixel assertion. This is the DX12 sibling of the standalone pink-clear/dock-gap scan (PR-18, shipped).

Intended outcome: *after this lands, the DX12/Unreal render backend can copy its presented backbuffer into a CPU-readable buffer and write it to an image file, so a pixel-level screenshot diff can gate DX12 rendering the way the standalone scan gates GLFW.*

## Approach

Add a readback path to the DX12 render backend: after the ImGui-on-Slate draw for a frame, `CopyResource` (or `CopyTextureRegion`) the presented backbuffer into a `D3D12_HEAP_TYPE_READBACK` resource, map it, and pack the rows (respecting `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` row-pitch padding) into a tightly-packed RGB buffer: the packing step drops each pixel's alpha byte and swizzles a BGRA backbuffer to RGB, so the PNG writer gets the same 3-component layout `WriteCapturePng` writes (`stbi_write_png(..., 3, ...)`; `debug.window.screenshot` always writes PNG). Gate the whole path behind a capture-request flag so it is inert in normal runs and only fires when a test/scenario asks for a frame. Trade-off: the readback stalls the frame (GPU→CPU sync), so it is capture-request-only, never on the steady-state present path.

Because this requires the Unreal/DX12 toolchain to compile and a DX12 device (WARP software rasterizer is sufficient — see the ARM64 WARP smoke in `docs/plans/dx12-standalone-win-arm64.md`) to run, both implementation and verification happen in that environment.

## Files to modify

1. `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/SmatchetImGuiRenderBackend_WinDx12.cpp` (the Windows D3D12 backend, `FWinDx12RenderBackend`; `SmatchetImGuiRenderBackendFactory.cpp` picks it under `PLATFORM_WINDOWS`, while `SmatchetImGuiRenderBackend_Platform.cpp` is the PS5/Xbox backend and is out of scope) — add the backbuffer→readback-heap copy + map + row-by-row pack (RowPitch-aware, BGRA→RGB, alpha dropped, as § Approach describes) after the frame's ImGui draw; gate behind a capture-request flag.
2. `SmatchetImGuiRenderBackend.h` (same Private dir) — declare the capture-request entry point + result buffer type.
3. A PNG writer — bytes → PNG through the vendored `stb_image_write` (`Source/Core/ThirdParty/stb/stb_image_write.h`). The plugin cannot call `boot_detail::WriteCapturePng`: that function lives in `Source/Standalone/StandaloneBoot_detail.cpp`, the one TU that defines `STB_IMAGE_WRITE_IMPLEMENTATION`, and it also sets `g_ui.lastCapture*` and calls the standalone `ReportCaptureSizeContract`; the plugin compiles none of `Source/Standalone` and only links `SmatchetCore_<Backend>` plus the flat header set `Source/UnrealPlugins/CMakeLists.txt` copies into `ThirdParty/Smatchet/include`. Take one of two shapes:
   - **(a, preferred) a Core pure PNG helper extracted from `WriteCapturePng`**: a new `Source/Core` TU that owns the stb implementation block and a pure write (RGB buffer + size + path → success flag, no `g_ui`, no logging policy), with its header added to the packaging copy in `Source/UnrealPlugins/CMakeLists.txt`. `WriteCapturePng` keeps its `g_ui` bookkeeping and contract check and calls the helper, so the standalone and the plugin share one writer. Sized in § Extraction sizing.
   - **(b) the plugin's own small TU** under the same Private dir that defines `STB_IMAGE_WRITE_IMPLEMENTATION` + `STB_IMAGE_WRITE_STATIC` and calls `stbi_write_png` directly, with `stb_image_write.h` added to the packaged include set. Nothing is extracted, at the cost of a second stb implementation block in the tree.
4. A capture-trigger seam (console command or scenario hook) wired through `SmatchetImGuiConsoleCommands.cpp` so a test can request one frame's capture.
5. Shape (a) only — the callers of the moved pack: `Source/Standalone/StandaloneBoot_detail.h` (the `PackRgbDropAlpha` declaration leaves for the Core header), and `Source/Standalone/main.cpp` and `Source/Standalone/StandaloneAppBootstrap.cpp` (each drops its `using boot_detail::PackRgbDropAlpha;` alias and calls the Core pack at two sites: the DX12 path with `flipVertical=false` on the tight RGBA that `Dx12Bootstrap::CaptureBackbuffer` already returns, and the GL `glReadPixels` path with `flipVertical=true`). `Dx12Bootstrap::CaptureBackbuffer` itself is unchanged.

## Existing utilities reused

- The standalone screenshot-diff harness (bucket-E `*Scenario` pixel assertions, e.g. the pink/dock-gap `CountPixels` bash assertion) — the *comparison* half this plan feeds; reuse its diff/threshold logic rather than inventing a new comparator.
- `docs/plans/dx12-standalone-win-arm64.md` § Approach (WARP fallback, back-buffer readback) — the WARP device pattern for running the readback without a physical GPU.
- `Dx12Bootstrap::CaptureBackbuffer` (`Source/Standalone/Dx12Bootstrap.cpp`) — the standalone DX12 readback this mirrors: `GetCopyableFootprints` → `D3D12_HEAP_TYPE_READBACK` resource → barrier → `CopyTextureRegion` → flush → `Map` → per-row copy honouring `RowPitch`. The plugin cannot link it (it lives in `Source/Standalone` and owns its own queue, where Unreal owns the plugin's), so follow its sequence rather than inventing one.
- `boot_detail::PackRgbDropAlpha` (`Source/Standalone/StandaloneBoot_detail.cpp`) — the existing pure RGBA→RGB pack. The plugin's backbuffer is BGRA (the standalone swapchain is RGBA), so the plugin's pack also swizzles. Under shape (a) of item 3 the pack moves into `Source/Core` beside the PNG helper as one pitch- and channel-order-aware function the standalone and the plugin share, and the Bucket A test below covers (sized in § Extraction sizing); under shape (b) the plugin keeps its own pure pack, and § Deviations from plan says why.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

Applies to shape (a) of § Files to modify item 3; shape (b) extracts nothing. Source: `Source/Standalone/StandaloneBoot_detail.cpp` (180 lines; under the 1,200-line `tu-line-ceiling`, so the win is one shared writer, not a cap).

- **EXTRACT → a new `Source/Core` PNG TU + header**: the `stb_image_write` implementation block (the `STB_IMAGE_WRITE_IMPLEMENTATION` / `STB_IMAGE_WRITE_STATIC` defines, the GCC/Clang and MSVC warning guards and the include — about 23 lines) and the pure write inside `WriteCapturePng` (compression level + `stbi_write_png` call + its result, about 5 lines). Projected new TU about 35 lines, header about 15.
- **EXTRACT → the same Core TU**: `PackRgbDropAlpha` (`StandaloneBoot_detail.cpp:130-145`, 16 lines), generalised to `(src, w, h, srcRowPitch, bgra, flipVertical, rgb)`: `srcRowPitch` (bytes per source row; `w * 4` for today's tight callers, the readback `RowPitch` for the plugin) and `bgra` (swap R and B) are new, and `flipVertical` stays, because the GL path flips. About 22 lines in the TU and 3 in the header. Its four standalone call sites pass `w * 4` and `false` (§ Files to modify item 5).
- **STAYS in `WriteCapturePng`**: the failure log, the `g_ui.lastCaptureWidth/Height` writes, the success log and the `ReportCaptureSizeContract` call — about 10 lines, now calling the Core helper.
- **Target**: `StandaloneBoot_detail.cpp` 180 → about 140 lines (the impl block and the pack leave, one call line replaces the write); the new Core TU about 57 lines, its header about 18. The Core impl targets already carry `Source/Core/ThirdParty` on their private include path (`smatchet_configure_core_impl_target` in `Source/Core/CMakeLists.txt`), so `<stb/stb_image_write.h>` resolves there without a build change.

## UX Pillar callouts

- **Pillar 1 (perf)**: the readback GPU→CPU sync stalls the frame; mitigated by making it capture-request-only (inert on the steady-state present path) — never unconditional.
- **Pillar 2 (UI-thread)**: capture runs on the render path under an explicit request; no ImGui-reachable sync-I/O added to the UI thread.
- **Pillar 3 (never crash)**: map/unmap and resource lifetime are the risk; guard every `HRESULT` and fail the capture (return empty) rather than asserting, so a capture failure never brings down the host.
- **Pillar 4 (accessibility)**: no impact (offscreen capture).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

Shape (a) adds a `Source/Core/src` TU (the PNG writer and the RGB pack), so the gates apply; shape (b) touches only `Source/UnrealPlugins/SmatchetImGuiPlugin/` and the standalone, and this section is `N/A` for it.

- **Hot-path exposure**: none. Both functions run only on a capture request (`debug.window.screenshot`, a bucket-C/E screenshot scenario, or the plugin's capture seam), never from per-frame code, so steady-state frame time is unchanged and the Pillar-1 stall stays gated by the request flag.
- **PR-fast scenarios**: no scenario in `scripts/dev/perf-pr-fast-set.json` takes a capture, and the screenshot scenarios are excluded from perf baselines (`screenshotOnly` in `scripts/dev/perf-scenario-map.json`), so the Perf PR-fast lane is expected to report no delta. Run it anyway; a delta is a bug in this change.
- **Pillar 2 scanner**: run `bash scripts/dev/pillar2-scan.sh <the new Core TU>`; it is expected to pass (`stbi_write_png` is not in its `SYNC_IO_REGEX`, and the TU is not UI-reachable by its heuristic). The write stays on the main thread on a capture request only, as it does in the standalone today (`HandleScreenshotRequest` runs from the render loop). Do not mark it `/* PILLAR2_WORKER_ONLY */`, which claims a worker thread; if a reviewer asks for an annotation, use `// TODO(pillar2): <ref>`.
- **Pre-push check**: `scripts/dev/perf-run.sh` on one PR-fast scenario before and after, compared with `scripts/dev/perf-compare.py` (the perf-gatekeeper skill's flow), to confirm the moved code is not linked into a hot path.

## Risks / non-goals

- **Risk: row-pitch padding corrupts the image.** `D3D12` readback rows are padded to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256 B); mitigation: copy row-by-row using the footprint's `RowPitch`, not a single `memcpy` of the whole mapped range.
- **Risk: format mismatch (backbuffer is BGRA/typeless).** Mitigation: read `GetDesc().Format` and normalise channel order before the PNG writer; assert the format is one of the handled set.
- **Risk: frame-sync deadlock.** Mitigation: signal+wait on a dedicated fence for the copy queue; bounded timeout → fail capture, don't hang.
- **Non-goal: continuous per-frame capture or video.** One-frame-on-request only.
- **Non-goal: standalone GLFW capture.** That path already has PR-18's scan; this is the DX12/Unreal sibling only.

## Verification

- **Bucket A**: the readback row-packing logic can have a pure-logic ctest (padded RGBA or BGRA rows → expected tightly-packed RGB bytes, alpha dropped) if factored as a pure function — do so.
- **Bucket E**: N/A for the standalone rig; the DX12 capture is exercised by the Unreal-side smoke below.
- **Bash-driver scenario / screenshot / sanitizer**: a DX12-on-WARP launch that requests one capture and asserts the written PNG is non-empty and the expected dimensions; wire into the ARM64/x64 WARP smoke leg.
- **Build gate**: the Unreal/DX12 build (`vs-unreal-msvc` / `ninja-iter-unreal-msvc` presets) compiles the plugin with the new path; `--target SmatchetCore_DX12` where applicable.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: confirm the row-pitch + format handling against a real `D3D12_PLACED_SUBRESOURCE_FOOTPRINT` before finalising.
- **Manual residue**: verification requires an Unreal/DX12 build environment (WARP acceptable); named here, not silent — cannot be exercised on a Linux/GLFW-only runner.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — no symbols are deferred by this plan.

- **Wiring DX12 screenshots into the required merge-gate** — first land capture + an advisory smoke; promoting the DX12 diff to blocking is a follow-up once the WARP baseline is stable (mirrors the standalone screenshot advisory→blocking lifecycle).

## Implementation log
*(populated post-ship — bullet per shipped commit)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that fills the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
