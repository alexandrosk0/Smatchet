# Plan — Image-dimension cap in the attachment-preview parser (testing-surface Slice E1)

**Status:** proposed — awaiting review (no code yet)
**Branch:** `harden/image-dim-cap` · worktree `C:\Dev\trees\image-dim-cap`
**Parent:** [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **E1** (§6 P1, Gap 3 narrow; `security.md:59-60`). Part of the approved additive block H→A→D→**E1**.

## Why this is a product change, not a just-do test

The roadmap scoped E1 as "craft PNG byte headers at/above `kMaxGoldenImageDim = 16384`, assert `ParseImageDimensionsFromBytes` rejects." That premise is **false against the current tree**:

- `ParseImageDimensionsFromBytes` (`Source/Core/src/Ui/ImageDimensionsPure.cpp`) enforces **no upper bound** — `TryParsePng` / `TryParseGif` / `TryParseWebp` / `TryParseJpeg` reject only `width == 0 || height == 0`.
- The `kMaxGoldenImageDim = 16384` cap lives **only** in the golden-image *test harness* (`tests/support/GoldenImage.h::LoadImage`), enforced **post-decode** via `stbi_load` — untestable cheaply (a 16384² decode is ~805 MB).
- The product call site (`SmatchetAttachmentPreviewUi.cpp:76-81`) copies `parsed.Width/Height` straight through with no clamp.

So there is no cap to write a rejection test against. To make E1's test meaningful, the cap must first exist in the product parser → a (small) behavioural product change → planned + reviewed per the standing directive.

### Latent bug this also closes

`result.Width = static_cast<int>(width)` casts an untrusted `std::uint32_t` (PNG/WebP dims) to `int`. A crafted width `> INT_MAX` (e.g. `0xFFFFFFFF`) yields a **negative** dimension that today returns `Ok = true` and flows into UI-thread layout + texture allocation — a Pillar-3 (never-crash) smell. A cap of 16384 (`< INT_MAX`) rejects these **before** the lossy cast, closing the bug as a side effect.

## Goal

Add an inclusive max-dimension cap (`kMaxImageDimension = 16384`) to the four byte-header parsers so oversized/overflowing dimensions are rejected with a clear error **before** the `static_cast<int>`, then add crafted-header rejection tests. No call-site or signature changes; `ParsedImageInfo` shape unchanged.

## Design decisions (review these)

1. **Cap value = `16384`.** Matches `kMaxGoldenImageDim` (defense-in-depth parallel), matches the typical GPU max 2D texture dimension (the preview becomes a texture), and is `< INT_MAX` so it also fixes the negative-cast bug. *Alternative considered:* stb's `STBI_MAX_DIMENSIONS = 1<<24` — rejected as too permissive (a 16 M-px header still passes) and `> INT_MAX`-adjacent, so it would not close the cast bug.
2. **Inclusive max:** `dim > kMaxImageDimension` rejects; `dim == 16384` is accepted (boundary pinned by test).
3. **All four formats** get the same check, for uniform behaviour. GIF/JPEG dims are `uint16` (max 65535 — can exceed 16384 but never overflow `int`); PNG/WebP are `uint32` (can overflow). One cap covers both "too big" and "overflow".
4. **DRY:** factor the now-three-step tail (zero-check → cap-check → assign) into one shared helper `FinalizeImageDimensions(result, width, height, formatLabel)` taking `std::uint32_t` (uint16 promotes), called by all four parsers. Removes the existing 4× duplication of the zero-check + assign pattern rather than adding a 5th copy of a cap check. *Reviewer veto point:* if you prefer minimal blast radius, the fallback is a 2-line inline `dim > kMaxImageDimension` check in each parser (no helper) — say which you want.
5. **Error text:** `"<FORMAT> dimensions exceed the maximum supported size."` (e.g. `"PNG dimensions exceed the maximum supported size."`), mirroring the existing `"<FORMAT> dimensions are invalid."` style.
6. **Constant home:** `kMaxImageDimension` in `ImageDimensionsPure.h` (product). The test-harness `kMaxGoldenImageDim` stays separate (different layer); a one-line comment cross-references. No harness refactor in this slice.

## Files to modify

| File | Change |
|---|---|
| `Source/Core/include/Ui/ImageDimensionsPure.h` | add `constexpr int kMaxImageDimension = 16384;` + decl note |
| `Source/Core/src/Ui/ImageDimensionsPure.cpp` | add `FinalizeImageDimensions` helper + route all 4 `TryParse*` tails through it (zero + cap + assign) |
| `tests/Core/ParseImageDimensions.test.cpp` | extend (not new-file) with the cap cases below |

## Test matrix (bucket-A, doctest — extends existing TU)

| Case | Input | Expect |
|---|---|---|
| PNG just over | width `16385`, height `480` | `!Ok`, error "PNG dimensions exceed…" |
| PNG height over | width `640`, height `16385` | `!Ok`, exceed error |
| PNG at cap (boundary) | width `16384`, height `16384` | `Ok`, `Width==16384`, `Height==16384` |
| PNG overflow / negative-cast | width `0xFFFFFFFF` | `!Ok`, exceed error (never `Ok` with negative `Width`) |
| GIF over (uint16) | width `0xFFFF` (65535) | `!Ok`, exceed error |
| WebP over (VP8X uint24) | canvas dim `> 16384` | `!Ok`, exceed error |
| JPEG over (SOF uint16) | width `0xFFFF` | `!Ok`, exceed error |

Existing zero-dim / valid-640×480 / truncated cases stay green (regression).

## Perf-gate section (mandatory — diff touches `Source/Core/`)

Touched zone is `Source/Core/src/Ui/` (**light/ungated**), and the change is two integer comparisons added to a **header-only** parse that runs once per attachment preview — **not** steady-state per-frame UI work. No hot-path, no allocation, no I/O added. Perf impact: negligible; no scenario rerun beyond the standard gate. No `SMATCHET_UI_PERF_SCOPE` needed.

## Verification (to run after approval)

- Build `SmatchetTests` + run `ParseImageDimensions.test.cpp` TU → all cases incl. 7 new pass.
- Dual-target compile is CI-authoritative (pure C++14 integer logic, no GLFW/GL/DX12 surface); local dual-target blocked by the unrelated CMake 4.3.0-rc3 `/EHsc` regression (see Slice D PR #1231 notes).
- `scripts/dev/test-docs.sh` for the plan/doc anchors.

## Implementation log

_(post-ship)_

## Deviations

_(post-ship)_
