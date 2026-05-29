# Agent self-improvement — infra

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-29 · orchestrator · [infra] · P2 — De-Smatchet-ify the portable agentic layer (close the project-literal baseline)
  Details: agentic-layer-project-independence shipped the portable/project STRUCTURE (agents/core vs project, project.config.json seam, docs taxonomy) but the portable files still embed ~157 project literals in prose (`docs/high-integrity/portable-purity-baseline.txt`). `test-portable-purity` baselines this and blocks NEW leakage, but reuse today means copy + adapt the prompts, not copy verbatim.
  Concrete next action: rewrite `agents/core/*` + `docs/agent-rules/*` prose to reference `project.config.json` keys instead of hardcoded `Smatchet`/`Source_Core`/preset literals; shrink the baseline toward zero. Largest chunk is the 15 core-agent prompts. Incremental — drop baselined entries as files are cleaned.
  Status: open
  Last-reviewed: 2026-05-29

- 2026-05-28 · deep-audit · [infra] · P2 — Dead CI job `windows-msvc-no-agentic` tests a removed flag (zero differential coverage, burns a runner/PR)
  Details: `.github/workflows/build-and-test.yml:163-190` runs `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_AGENTIC=OFF`, but no `option(SMATCHET_WITH_AGENTIC ...)` exists — the flag was removed in PR #356 (`tests/ui/agent_proposal_store_sqlite.test.cpp:25` says so outright). CMake silently ignores unknown `-D` cache vars, so the job builds the identical default standalone config as the rest of the matrix and asserts nothing, while consuming a windows-2022 runner on every code PR (paths-ignore only skips docs). Contrast `windows-msvc-no-whisper` which flips the real `SMATCHET_WITH_WHISPER` option. Verified (deep-audit, adversarially confirmed).
  Concrete next action: delete the `windows-msvc-no-agentic` job. If a stripped-feature build is still wanted, point it at a flag that actually exists. ~10 min.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-26 · orchestrator · [infra] · P2 — Advisory CI jobs need step-level non-blocking templates
  Details: PR #460 exposed that job-level `continue-on-error: true` was not enough to keep Bucket-C/Bucket-E soak-window failures from surfacing as red PR checks. Bucket-C and Bucket-E both needed step-level `continue-on-error` on the failing scenario/diff step, with artifact upload keyed off `steps.<id>.outcome` or `always()` so diagnostics still survive.
  Concrete next action: add a shared workflow snippet or documented pattern for advisory jobs: give the risky step an `id`, set `continue-on-error: true` on that step, and upload artifacts using `if: ${{ steps.<id>.outcome == 'failure' }}` or `if: always()` as appropriate. Then audit existing advisory jobs for the same shape. Estimated cost 30 min.
  Status: open
  Last-reviewed: 2026-05-26

- 2026-05-18 · orchestrator · [infra] · P2 — Whisper Phase H — `SMATCHET_WHISPER_LOCAL_BACKEND` default flip OFF→ON decision (binary size vs. local-by-default UX)
  Details: User's chosen Whisper backend mode is "local default, cloud fallback" (per the dictation plan's locked decisions). Phase C shipped the local backend behind a new sub-option `SMATCHET_WHISPER_LOCAL_BACKEND` that defaults OFF — so default-built users get the plugin compiled in (+10.71 MB binary delta from Phase C) but the actual whisper.cpp link is opt-in. With sub-option OFF: `WhisperLocal::LoadModel` returns "local backend not built" and the mode router falls back to cloud. With sub-option ON: full whisper.cpp + ggml link, binary delta unmeasured (plan's open question #1 flagged a >50 MB risk threshold). Result: the locked "local default" UX does not match the shipped default for users who don't pass `-DSMATCHET_WHISPER_LOCAL_BACKEND=ON`. Three resolution paths: (a) flip default ON, accept binary growth, single-stage user experience matches the plan; (b) refactor whisper.cpp link to a runtime-loaded DLL — plugin compiles in but the whisper code only joins the process on first local-mode transcription, deferring the size hit until needed; (c) keep current state + document the OFF default as the canonical user UX, downgrading the "local default" decision to "cloud default, local available with rebuild". Option (b) is cleanest but a multi-day refactor.
  Concrete next action: measure the binary delta when `SMATCHET_WHISPER_LOCAL_BACKEND=ON` (one `cmake -D... && cmake --build` cycle, compare exe size pre/post). If <50 MB delta — flip default ON. If >50 MB delta — author a separate "whisper-dll-loader" plan doc for option (b) and stay on the current sub-option OFF default in the interim. ~30 min to measure; multi-day if option (b) is chosen.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · test-author · [infra] · P2 — Bucket-E `--spawn` ephemeral runner flakes intermittently (pre-existing)
  Details: Empirically observed during `tests/ui/ai_assistant_panel_dock_swap.test.cpp` + `tests/ui/ai_prefs_autosave_flow.test.cpp` development that the existing `scripts/dev/test-ui-*.sh` runners (including the pre-PR `test-ui-views-columns-reorder.sh` and `callstack_tooltip_hover` paths) intermittently report `failed:N passed:0` on otherwise-clean runs that pass on subsequent retries with no source change. Reproducible by running any bucket-E runner ~4× back-to-back on sequential ports — at least one run typically flakes. The `callstack_tooltip_hover` runner has shipped this flake since before PR #210. New AI Assistant TUs in this PR (`ai_assistant_panel_dock_swap`, `ai_assistant_enter_send`, `ai_prefs_autosave_flow` V1) all pass deterministically when run individually + on a clean port; the flake is upstream of the test code, in the spawn/MCP-ready/test-queue lifecycle.
  Concrete next action: diagnose root cause — leading hypotheses are (a) spawn warmup race where the test engine queues tests against an incomplete ImGuiContext, (b) MCP-ready signal racing with `ui_test.run` queue dispatch, (c) port-binding TIME_WAIT state on rapid back-to-back runs. Wire deterministic spawn-warmup gate (e.g. require N frames rendered before accepting queue) or add per-runner retry-once-on-spawn-warmup-failure mitigator. ~4 h diagnosis + fix.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · test-author · [infra] · P2 — AiClientFactory has no test-injection seam for bucket-E success-path coverage
  Details: `tests/ui/ai_prefs_autosave_flow.test.cpp` Variant 2 (`VerifyOnSave_TestConnection_SetsResult`) falls back to driving libcurl against `http://127.0.0.1:65530` (an unbound loopback port → immediate ECONNREFUSED) to exercise the Failed: branch of the Preferences Test-connection probe. This tests every layer EXCEPT the success-completion branch — `g_ui.assistantPrefsTestResult == "Verified."` + `assistantPrefsTestResultType == 1` is never reached. Adding a static test seam to `AiClientFactory` would let the variant assert the success path against an in-process stub `IAiClient` that returns immediately from `ProbeReachability` + emits a synthetic chat delta on `SendStreaming`.
  Concrete next action: add `AiClientFactory::SetTestOverride(std::unique_ptr<IAiClient>)` + `ClearTestOverride()` statics gated on `#if defined(SMATCHET_BUILD_UI_TESTS)`. `MakeAiClient(...)` consults the override before the provider-kind switch. Stub `IAiClient` class lives under `tests/ui/support/`. Add Variant 4 `VerifyOnSave_TestConnection_StubSuccess_VerifiedLineLands` to the autosave-flow TU. ~3 h (seam + stub + variant + clear-on-test-end semantics).
  Status: applied (2026-05-18, PR feat/ai-client-test-override — `AiClientFactory::SetTestOverride(TestOverrideFn)` lands; `runProbe` extracted to `AiPrefsTestConnection::TriggerProbe` to bypass the host-coupled `Preferences -> Assistant tab -> Test connection` path; V2 + V3 lift to live coverage. Follow-up tracked: 5-line UI rewire in `SmatchetPreferencesUi.cpp` once `whisper-dictation-phase-f` PR #219 merges.)
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [infra] · P3 — Worker-side per-chunk dispatcher post coalescing for AI stream
  Details: `AiAssistantController::RunRequest`'s `onDelta` posts one task to `MainThreadDispatcher` per provider chunk. OpenAI streams 5–10 chunks/s, Anthropic ~30/s, Ollama ~50+/s for fast local models. The dispatcher queue stays drained but the per-frame `assistantStreamBuf.append` cost + redraw cost is proportional to chunk rate. Coalesce on the worker into 50–100 ms windows before posting; one task per window appends the concatenated chunks.
  Concrete next action: add a small per-turn `std::string pendingChunks_` + `std::chrono::steady_clock::time_point lastPostAt_` on the worker side; flush when (a) pendingChunks_.size() > 4 KB OR (b) 80 ms elapsed since `lastPostAt_` OR (c) `isFinal`. Wire through `onDelta`. ~1 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `ImGuiListClipper` in `DrawHistoryArea` for long assistant histories
  Details: `SmatchetAiAssistantUi.cpp:96-101` (now ~110 after PR #176) renders every `AiMessage` via `TextWrapped` per frame. A 10 000-turn session reflows the entire history every frame at 144 Hz. Use `ImGuiListClipper` once messages exceed ~50 to skip rendering off-screen entries. Currently latent (real sessions are short), but cheap to add and future-proofs the panel for "ask Claude to refactor the whole codebase" sessions.
  Concrete next action: wrap the history `for` loop in `ImGui::ListClipper` with `clipper.Begin(history.size())`. Verify `TextWrapped` row-height estimation works (or measure once + cache). ~45 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `AiContextBuilder::BuildActiveTicketBody` O(N) ticket scan
  Details: Linear scan over `tickets` to find the active id at [`AiContextBuilder.cpp:151-153`](../../../Source_Core/src/AiContextBuilder.cpp). For 10 K-ticket views the Send button blocks proportionally. `IdIndex` map already exists elsewhere in the codebase; pass through `Inputs` or accept a pre-resolved `const CachedTicket*`.
  Concrete next action: extend `AiContextBuilder::Inputs` with `const CachedTicket* PreResolvedActiveTicket = nullptr`; populate from the UI side via `IdIndex` lookup. Fall back to the existing scan when null. ~30 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `AgentsMdLoader` reads `maxBytes + 1` even when file is smaller
  Details: [`AgentsMdLoader.cpp:45-46`](../../../Source_Core/src/AgentsMdLoader.cpp) `out.resize(maxBytes + 1)` then reads `maxBytes + 1` bytes regardless of actual file size. Functionally correct (over-cap detection works), but each load reads up to 64 KB+1 even for a 1 KB agents.md. Trivial waste; matters only when invalidation happens on a hot Preferences-change loop.
  Concrete next action: stat the file first; cap the read at `min(maxBytes + 1, file_size)`. ~15 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `SmatchetAiAssistantUi` silently truncates 8 KiB paste
  Details: `InputTextMultiline` is sized to `s_inputCharBuf.size()` (8 KiB). User pasting 9 KiB has the suffix silently dropped with no toast.
  Concrete next action: detect truncation on paste (compare `clipboard.size()` against `kInputBufCap` in an `ImGuiInputTextCallback`); emit a toast naming the dropped suffix length. ~45 min.
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
