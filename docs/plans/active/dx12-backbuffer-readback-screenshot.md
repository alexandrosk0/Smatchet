# Plan — DX12 backbuffer readback screenshot diff (PR-19)

> **Slug**: `dx12-backbuffer-readback-screenshot` (matches this file's basename without `.md`).
>
> **Status**: `active` — from `docs/plans/backlog-pr-roadmap.md` PR-19 (`dx12-backbuffer-readback-screenshot-diff`). Owner: unreal-bridge. **Hardware-gated**: authoring and verification need an Unreal/DX12 build environment (not available in a Linux web session). Related: `docs/plans/dx12-standalone-win-arm64.md`.

## Context

The standalone GLFW build has a screenshot-diff harness (bucket-E / `DockGapSentinelScenario` etc.), but the Unreal-embedded DX12 render path has **no** frame-capture: today only a Slate-backbuffer *log line* exists in the plugin, so a DX12-only rendering regression (dock gaps, clear-color, ImGui-on-Slate compositing) cannot be caught by an automated pixel assertion. This is the DX12 sibling of the standalone pink-clear/dock-gap scan (PR-18, shipped).

Intended outcome: *after this lands, the DX12/Unreal render backend can copy its presented backbuffer into a CPU-readable buffer and write it to an image file, so a pixel-level screenshot diff can gate DX12 rendering the way the standalone scan gates GLFW.*

## Approach

Add a readback path to the DX12 render backend: after the ImGui-on-Slate draw for a frame, `CopyResource` (or `CopyTextureRegion`) the presented backbuffer into a `D3D12_HEAP_TYPE_READBACK` resource, map it, and `memcpy` the rows (respecting `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` row-pitch padding) into a tightly-packed RGBA buffer, then hand it to a minimal PPM writer. Gate the whole path behind a capture-request flag so it is inert in normal runs and only fires when a test/scenario asks for a frame. Trade-off: the readback stalls the frame (GPU→CPU sync), so it is capture-request-only, never on the steady-state present path.

Because this requires the Unreal/DX12 toolchain to compile and a DX12 device (WARP software rasterizer is sufficient — see the ARM64 WARP smoke in `docs/plans/dx12-standalone-win-arm64.md`) to run, both implementation and verification happen in that environment.

## Files to modify

1. `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/SmatchetImGuiRenderBackend_Platform.cpp` — add the backbuffer→readback-heap copy + map + row-pitch-corrected `memcpy` after the frame's ImGui draw; gate behind a capture-request flag.
2. `SmatchetImGuiRenderBackend.h` (same Private dir) — declare the capture-request entry point + result buffer type.
3. A minimal PPM writer helper (new small TU under the same plugin Private dir, or a shared `Source/Core` pure writer if one already exists — grep first) — bytes → `.ppm`.
4. A capture-trigger seam (console command or scenario hook) wired through `SmatchetImGuiConsoleCommands.cpp` so a test can request one frame's capture.

## Existing utilities reused

- The standalone screenshot-diff harness (bucket-E `*Scenario` pixel assertions, e.g. the pink/dock-gap `CountPixels` bash assertion) — the *comparison* half this plan feeds; reuse its diff/threshold logic rather than inventing a new comparator.
- `docs/plans/dx12-standalone-win-arm64.md` § DX12-on-WARP launch smoke — the WARP device pattern for running the readback without a physical GPU.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — adds a readback path + PPM writer; extracts/splits nothing (unless a shared PPM writer already exists, in which case reuse, don't fork).

## UX Pillar callouts

- **Pillar 1 (perf)**: the readback GPU→CPU sync stalls the frame; mitigated by making it capture-request-only (inert on the steady-state present path) — never unconditional.
- **Pillar 2 (UI-thread)**: capture runs on the render path under an explicit request; no ImGui-reachable sync-I/O added to the UI thread.
- **Pillar 3 (never crash)**: map/unmap and resource lifetime are the risk; guard every `HRESULT` and fail the capture (return empty) rather than asserting, so a capture failure never brings down the host.
- **Pillar 4 (accessibility)**: no impact (offscreen capture).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

`N/A` — the diff touches `Source/UnrealPlugins/SmatchetImGuiPlugin/`, not `Source/Core/src/`. The perf-review-system gates key on `Source_Core/`. Note the Pillar-1 stall above is handled by the capture-request gating; the standalone perf baselines are unaffected (they run the GLFW path).

## Risks / non-goals

- **Risk: row-pitch padding corrupts the image.** `D3D12` readback rows are padded to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256 B); mitigation: copy row-by-row using the footprint's `RowPitch`, not a single `memcpy` of the whole mapped range.
- **Risk: format mismatch (backbuffer is BGRA/typeless).** Mitigation: read `GetDesc().Format` and normalise channel order in the PPM writer; assert the format is one of the handled set.
- **Risk: frame-sync deadlock.** Mitigation: signal+wait on a dedicated fence for the copy queue; bounded timeout → fail capture, don't hang.
- **Non-goal: continuous per-frame capture or video.** One-frame-on-request only.
- **Non-goal: standalone GLFW capture.** That path already has PR-18's scan; this is the DX12/Unreal sibling only.

## Verification

- **Bucket A**: the PPM writer's byte-layout logic can have a pure-logic ctest (buffer → expected PPM bytes) if factored as a pure function — do so.
- **Bucket E**: N/A for the standalone rig; the DX12 capture is exercised by the Unreal-side smoke below.
- **Bash-driver scenario / screenshot / sanitizer**: a DX12-on-WARP launch that requests one capture and asserts the written PPM is non-empty and the expected dimensions; wire into the ARM64/x64 WARP smoke leg.
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
