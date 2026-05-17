# Agent self-improvement — infra

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-17 · code-review · [infra] · P0 — `tests/golden/*.ppm` 2 × 2.76 MB raw PPMs (5.5 MB / ~191k lines) bloat pack permanently
  Details: PNG equivalents would be ~50-150 KB. Either wire Git LFS for `tests/golden/*.ppm`, or switch writer + reader to PNG via stb_image (already transitive via ImGui).
  Concrete next action: choose LFS vs PNG migration; if PNG, route through stb_image_write + stb_image_read (no new dep). Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · unreal-bridge · [infra] · P2 — DX12 backbuffer readback for screenshot diff (Phase 7 bucket C)
  Details: Phase 7 (`test-phase-7-screenshot-diff`) ships PPM capture wired into `Target_Standalone/main.cpp:569` via `glReadPixels(GL_FRONT, GL_RGBA, ...)`. The `debug.window.screenshot` flag pair (`UiDrawSession::requestScreenshot{,Path}`) flips on both Standalone and DX12 builds but DX12 never consumes it — Unreal owns the swap chain and has no equivalent backbuffer-readback path wired in `SmatchetCore_DX12`. Bucket-C verification therefore covers Standalone-only.
  Concrete next action: add a DX12-side equivalent in `UnrealPlugins/SmatchetImGuiPlugin/` (or wherever the swap-chain present hook lives) — `ID3D12GraphicsCommandList::CopyResource` from the backbuffer to a readback heap, then memcpy to the same PPM writer used by Standalone. Estimated cost ~4-6 h (DX12 resource-state transitions are fiddly). Until then, Phase 7 gates only run on Standalone — Unreal-shipped builds skip bucket-C without notice.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [infra] · P2 — `IsTrackerTransportErrorText` is cpr-tainted by location; needs cpr-free TU split
  Details: The symbol lives in `Source_Core/src/TrackerHttpUtils.cpp` next to cpr-using HTTP wrappers. Any test target that touches `OfflineQueueService.cpp` (or any other consumer of the symbol) must either (a) link `TrackerHttpUtils.cpp` + cpr (bloats test exe with the entire HTTP layer — what PR E + PR F ended up doing after rebase), or (b) re-define the symbol locally (drift surface — what PR E shipped initially before being forced into option (a) by the rebase cascade). Instance of the Pure-helper TU-split recipe — see AGENTS.md § Orchestrator delegation packet § Pure-helper TU-split recipe.
  Concrete next action: lift the pure error-text classifier into `Source_Core/{include,src}/TrackerHttpErrorText.{h,cpp}` (same pattern as `P4BlameParse`, `TrackerLabelsPure`, `TrackerDateTimePure`). The classifier is <100 LoC, zero `<cpr/*>` includes. `TrackerHttpUtils.cpp` rewires its single call site via using-decl. Estimated cost 30 min refactor + ~5 cases / ~20 assertions doctest coverage.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [infra] · P2 — Shared `tests/support/TestEnvGuard.h` should host the ConfigManager-redirect + audit-redirect RAII guard
  Details: PR E (`OfflineQueueServiceRuntime.test.cpp`) hit `ConfigManager::Load()` returning `ReadOnlyMode=true` on fresh-install when no `smatchet_config.json` exists in cwd (`Source_Core/src/ConfigManager.cpp:713-714` safety branch). Every `QueueCreateOffline` / `QueueFieldEditOffline` call silently returned 0 because the service short-circuits on read-only mode. Fix landed test-side as a private `TestEnvGuard` RAII wrapper. PR F (`TicketSyncService.test.cpp`) hit the same problem and reimplemented its own guard. Phase 4 (Config + schema migration) will hit it again.
  Concrete next action: hoist a shared `tests/support/TestEnvGuard.h` that creates a unique temp dir, points `ConfigManager::SetUserDataDirectory` at it, writes a minimal `smatchet_config.json` with `read_only_mode=false`, and on dtor restores + cleans. Estimated cost 20 min consolidate the two existing copies + replace one in each test file.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [infra] · P2 — Phase 1 deferred 4 tracker units pending production-side TU split
  Details: `tests/Source_Core/{IssueDraft,IssueCreatePipeline,TrackerFieldValueParser.extended,TrackerFieldValueUtils}.test.cpp` shipped (171 new CHECKs; 5/5 high-risk mutation sanity green). Plan-named units NOT shipped because each has pure helpers buried under ImGui / AppController / JiraClient / cpr includes — instance of the Pure-helper TU-split recipe (AGENTS.md § Orchestrator delegation packet § Pure-helper TU-split recipe):
    - `Source_Core/src/TrackerLabelsEditor.cpp` — parse/serialize round-trip + dup-detection lives next to ImGui input handling.
    - `Source_Core/src/TrackerDateTimeFieldEditor.cpp` — ISO-8601 parser lives next to ImGui calendar widget.
    - `Source_Core/src/TrackerFieldPayload.cpp` — payload builder pulls JiraClient → cpr → ConfigManager transitively.
    - `Source_Core/src/TrackerFieldCatalog.cpp` — catalog merge / lookup lives next to JiraClient catalog-fetch surface.
  Concrete next action: per-unit TU split (lift pure helpers to a sibling `*Pure.cpp` + matching header with no ImGui/cpr includes), then add the doctest in a follow-up phase. Estimated cost ~1 h per unit (4 h total). Bonus: `IssueCreatePipeline::ApplyPostIssueSteps` decision logic also deferred — needs `ITrackerClient` mock fixture (Phase 3 HTTP layer). Pick up after Phase 3 ships the HTTP / SQLite fixtures.
  Status: open
  Last-reviewed: 2026-05-17
