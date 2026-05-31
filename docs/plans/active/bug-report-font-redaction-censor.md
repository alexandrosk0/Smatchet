# Plan — Bug-report screenshot font-redaction censor (hardening)

> **Slug**: `bug-report-font-redaction-censor` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The bug-report screenshot censor shipped its v1 under the parent feature [`log-a-bug-github.md`](log-a-bug-github.md) (commit `71059e93`, "font-redaction screenshot censor (replaces mosaic)"). Instead of mosaicing rectangles, the capture frame now swaps the whole UI to a `█`-block font (U+2588) so every text codepoint renders as a block — sharp, layout/icon/colour-preserving, no readable text. The mosaic path (`MosaicCensorInPlace`, `bugreport_censor_block`, the `bug.report --censored` param) was removed entirely; downscale-to-1280px was kept.

That v1 commit explicitly deferred three residual leaks to *this* follow-up plan (and the `SmatchetImGuiFonts.h` Redaction-font comment anchors here):

1. **1-frame `█` flash** — the Redaction font is pushed for the single capture frame and popped after, so the live, on-screen UI flashes all-blocks for one frame. Accepted in v1; visible to the user.
2. **Fixed-advance char-count leak** — U+2588 is a fixed-advance glyph, so block-run lengths still reveal text length, word boundaries, and structure even though the characters are unreadable.
3. **Text-in-textures not covered** — text baked into textures/images (not rendered as glyphs) bypasses the font swap and remains readable in the capture.

**Intended outcome**: after this lands, the redacted screenshot leaks neither a visible on-screen flash, nor recoverable text length/structure, nor texture-baked text — while keeping the layout/icon/colour fidelity that motivated the font-redaction approach over mosaic.

> Status: v1 is shipped; hardening is in-flight on branch `feat/bug-report-font-redaction-impl`. This doc back-fills the plan the v1 commit referenced and frames the remaining work.

## Approach

Harden the existing one-frame swap rather than redesign it — the `SmatchetPushRedactionFonts` / `SmatchetPopRedactionFonts` primitive and the Submit+1 capture frame in `main.cpp` stay. Three independent slices, each closing one named leak:

1. **Kill the flash** — render the redacted frame to an offscreen target that is captured but never presented, so the user never sees the `█`-state. The push/pop bracket moves around an offscreen render pass instead of the live present. This is the dual-target-sensitive slice (Standalone GL vs Unreal DX12 capture paths diverge).
2. **Defeat the char-count leak** — normalise block runs so run length no longer maps to text length. The non-obvious trade-off: full per-line normalisation distorts the layout fidelity that is the whole point of font-redaction, so the proposed direction is bounded jitter / quantised run-length buckets (preserve approximate layout, destroy exact length) rather than constant-width bars. Final mechanism is a design call for the owner; if it gets deep it earns an ADR.
3. **Cover texture-baked text** — detect texture-backed widgets in the capture frame (attached images, any text rasterised to a texture) and blur or exclude them, reusing the retained downscale path as the coarsest mitigation.

Slices 2 and 3 are independent of slice 1 and of each other; ship in whatever order de-risks fastest.

## Files to modify

1. [`Source/Core/src/Ui/SmatchetImGuiFonts.cpp`](../../../Source/Core/src/Ui/SmatchetImGuiFonts.cpp) — Redaction `ImFont` bake + `Push`/`Pop` repoint; add run-length normalisation hook (slice 2) if done at the font/advance layer.
2. [`Source/Core/include/Ui/SmatchetImGuiFonts.h:38`](../../../Source/Core/include/Ui/SmatchetImGuiFonts.h#L38) — Redaction-font declarations + the comment that anchors this plan (`SmatchetPushRedactionFonts`/`SmatchetPopRedactionFonts`, line 43).
3. [`Source/Standalone/main.cpp`](../../../Source/Standalone/main.cpp) — push-before-`NewFrame` / pop-after-capture wiring; offscreen-capture render pass for the flash fix (slice 1, Standalone/GL side).
4. [`Source/Core/src/Ui/SmatchetBugReportUi.cpp`](../../../Source/Core/src/Ui/SmatchetBugReportUi.cpp) — `requestRedactFontThisFrame` arm path; "Attach screenshot" (always-redacted) flow.
5. [`Source/Core/src/Imaging/ScreenshotCensor.cpp`](../../../Source/Core/src/Imaging/ScreenshotCensor.cpp) / [`.h`](../../../Source/Core/include/Imaging/ScreenshotCensor.h) — retained downscale; candidate home for texture-region blur (slice 3).

## Existing utilities reused

- `SmatchetPushRedactionFonts` / `SmatchetPopRedactionFonts` — `Source/Core/include/Ui/SmatchetImGuiFonts.h:43` — the one-frame whole-UI swap primitive built in v1; all three slices build on it rather than replacing it.
- `SmatchetGetPreviewFonts` — `Source/Core/include/Ui/SmatchetImGuiFonts.h:36` — the preview font set the swap repoints (plus `io.FontDefault`).
- `ScreenshotCensor` downscale-to-1280px — `Source/Core/src/Imaging/ScreenshotCensor.cpp` — retained from v1; the coarse fallback for texture-baked text (slice 3).
- The Submit+1 arm → request-next-frame capture handshake in `main.cpp` (`bugReportShotArmed` / ready signal) — reused; offscreen pass slots into it.

## UX Pillar callouts

Per `AGENTS.md` § UX Pillars.

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no steady-state impact — the swap is a one-shot pointer repoint on a single user-triggered capture frame. The extra Redaction `ImFont` adds one-time atlas-bake cost + atlas memory, not per-frame work. Offscreen capture (slice 1) is one extra render pass on the capture frame only.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new sync I/O on the UI thread; capture/encode already runs off the hot path. The offscreen pass is GPU-bound and one-frame.
- **Pillar 3 (never crash)**: Redaction font falls back to `Regular` when the body font lacks U+2588 (capture degrades to un-redacted only if the fallback also lacks it — guard + WARN). `Push`/`Pop` is **not re-entrant** (one capture at a time) — pairing must survive an exception/early-return between push and capture, else `io.FontDefault` is left dangling at the Redaction font; bracket with RAII-style guaranteed pop.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: largely N/A (diagnostic capture path, not interactive UI). Minor: killing the 1-frame `█` flash (slice 1) also removes a brief full-screen flash — a small photosensitivity win.

## Perf-review-system gates (diff touches `Source/Core/`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`.

1. **PR-fast CI** — **N/A for steady-state**: the changed path is a one-shot, user-triggered capture, not a per-frame scenario in the curated map (`agents/core/perf-gatekeeper.md` § map). No steady-state scenario exercises it; the font-atlas bake is one-time at font-load.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`; capture/encode stays off the UI thread. No `PILLAR2_WORKER_ONLY` annotation needed.
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — adds no new sync-stall code path > 100 ms.
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers; `docs/perf/MARKER_INVENTORY.md` unchanged.

**Pre-push local check**: N/A — no steady-state scenario maps to this path.

**Override**: not anticipated; `perf-out-of-band` available if the offscreen pass surprises a baseline.

## Risks / non-goals

- **Layout fidelity vs char-count leak is a fundamental tension** — normalising run lengths (slice 2) erodes the exact layout that motivated font-redaction over mosaic. Mitigation: bounded jitter / quantised buckets rather than constant-width; accept approximate-layout. Owner decides the exact mechanism (ADR if deep).
- **Dual-target offscreen-capture divergence** — Standalone (GL) and Unreal (DX12) capture paths differ; slice 1 must land in both or be guarded. Risk: GL-only fix leaves the flash on DX12. Mitigation: dual-target build gate + `SMATCHET_EMBEDDED_IN_UNREAL` branch review.
- **Push/Pop non-re-entrancy** — concurrent or nested capture leaves the UI stuck on the Redaction font. Mitigation: single-capture guard + guaranteed pop (Pillar 3).
- **Non-goal**: server/relay-side redaction — the relay never receives a bundled token and is out of scope here.
- **Non-goal**: redacting non-screenshot diagnostic fields (logs, breadcrumbs, crash log-tail) — those are text-redacted by `Privacy/TextRedaction` in the parent feature, not by this font swap.

## Verification

Per `AGENTS.md` § Verification automation.

- **Bucket A (pure-logic ctest, `test-rig`)**: run-length normalisation helper (slice 2) — assert equal-length inputs of differing text map to indistinguishable run-length buckets; Redaction-font selection / U+2588 fallback decision logic.
- **Bucket E (ImGui Test Engine, `ninja-ui-test-msvc`)**: render a frame with redaction pushed → assert the captured target contains no readable glyph runs (all-block); golden screenshot-diff of a known panel; assert the *presented* frame is never the redacted one (slice 1 — flash gone).
- **Bash-driver scenario / screenshot**: `bug.report --dry-run` headless capture → pixel/heuristic scan of the output PNG asserting no non-block text glyphs; verify downscale + texture-region blur (slice 3) on a fixture containing texture-baked text.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — mandatory given slice 1's GL/DX12 divergence).
- **Manual residue**: "no visible flash" may resist bucket-E if the harness can't prove a frame was never presented (vs merely not captured). Deferred-automation plan: assert offscreen-target binding at the capture call-site instead of eyeballing; if still manual, add a `docs/self-improvement/categories/tooling.md` entry rather than leaving silent residue.

## Out of scope (flagged, not designed)

- **Relay-side / server-side redaction** — token never bundled; separate concern in `tools/bug-report-relay/`.
- **Redacting logs / breadcrumbs / crash log-tail** — covered by `Privacy/TextRedaction` under the parent feature, not this font swap.
- **Per-widget opt-out** (let the user un-redact a chosen panel) — no demand; no-action.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit)*

- v1 baseline shipped under the parent feature: `71059e93` · font-redaction screenshot censor replaces mosaic (`SmatchetImGuiFonts` Redaction font + `Push`/`Pop`; modal drops Full/Censored radios; mosaic + `bugreport_censor_block` removed; downscale kept). Hardening (slices 1–3) in-flight on `feat/bug-report-font-redaction-impl`.

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan)*

## Verification (actual)
*(populated post-ship — what was actually tested + result)*
