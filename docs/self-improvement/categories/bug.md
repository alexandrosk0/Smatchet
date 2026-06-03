# Agent self-improvement — bug (DEPRECATED)

> **DEPRECATED (ADR-0014, 2026-06-03).** Product bugs now live as **GitHub Issues**
> — see [`../../agent-rules/issue-triage.md`](../../agent-rules/issue-triage.md). This
> file is **frozen**: no new entries. Existing entries migrate to Issues (genuine
> product bugs) or to [`debt.md`](debt.md) (tech-debt); a bug *in the agentic
> harness/scripts* folds into `tooling`/`infra`. Kept readable until migration (G) completes.
>
> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug (deprecated) · debt · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-06-02 · code-review · [bug] · P3 — C++ syntax highlighter doesn't tokenize the C++14 digit separator (`'`) or uppercase `U` suffix as part of a number
  Details: CodeRabbit (Minor) flagged `Source/Core/src/Ui/CppSyntaxLex.cpp` `IsNumberCont` — it omits `'` (valid C++14 integer-literal digit separator, e.g. `1'000`) and uppercase `U` (unsigned suffix; suffixes are case-insensitive, e.g. `1U`), so those literals split into multiple tokens and lose contiguous number highlighting. Pre-existing: develop's inline `DrawColoredCppLine` number-continuation chain (`CppSyntaxHighlight.cpp:132-136`) accepts exactly `0-9 . x X a-f A-F u l L` — the same set. The CppSyntaxHighlight decomposition (#739) extracted that set byte-for-byte into `IsNumberCont`; adding `'`/`U` would change the rendered token spans vs develop, so it was NOT done in the behaviour-preserving refactor (documented inline at the helper).
  Concrete next action: extend `IsNumberCont` to also accept `'` and `U`; add a bucket-A case for `1'000` and `1U` lexing as single number tokens. ~20min, dedicated highlighter PR.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-02 · code-review · [bug] · P2 — bulk-import: `bulkImportFutures.clear()` can block the UI thread on window-close mid-import
  Details: CodeRabbit (Major) flagged `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp` — `d.bulkImportFutures.clear()` destroys `std::future`s while their async HTTP `CreateIssueAsync` calls may still be in flight; `std::future`'s destructor blocks until the shared state is ready, so closing the bulk-import window mid-import freezes the UI thread until every outstanding request finishes (potentially seconds). Pre-existing on develop — **3 occurrences (develop lines 132/226/311), byte-identical count in the E5 PR head (#733)**; surfaced + relocated by the `drawBulkImportWindow` function-size decomposition (#733). NOT changed in #733 (behaviour-preserving refactor — awaiting/detaching futures alters runtime behaviour). Violates UX Pillar 2 (no UI-thread block > 100 ms).
  Concrete next action: either await all futures with a short timeout before `clear()` and show a "finishing imports…" cue, or move to shared-ownership results + detached futures, or add a cancellation token to `CreateIssueAsync`. ~1.5h, dedicated bulk-import PR.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-02 · code-review · [bug] · P2 — Whisper preferences: 4 pre-existing logic/copy bugs surfaced by the E2 decomposition CR review
  Details: CodeRabbit flagged four issues in `SmatchetPreferencesUi_Whisper.cpp` on the E2 (#729) decomposition — all verified pre-existing (byte-identical to develop; E2 relocated them verbatim, positional-ImGui balance identical). (1) **Auto-mode E2E route prefers cloud-when-key (Major)**: `ResolveE2ERoute`/develop:583-593 uploads the E2E sample to cloud whenever an API key exists even if a local model is installed — contradicts the "Auto (local if present, cloud fallback)" mode description (privacy-relevant). (2) **Fallback model not seeded (Major)**: the model picker shows index 1 (Recommended) when `cfg.WhisperModel` is empty, but `modelPresent`/`dl.Start()` still read the empty/stale id. (3) **Hotkey validation fallback text wrong (Minor)**: rejects no-modifier combos but the message says a non-modifier key is missing (`capturedVk` guarantees the opposite). (4) **E2E hint overstates cloud upload (Minor)**: always says the test uploads audio, but a local transcription path exists — misleading privacy copy in local mode.
  Concrete next action: (1) make the auto-mode E2E route prefer local-when-present (match the mode description) or reword the description; (2) seed `cfg.WhisperModel` to the recommended default when empty; (3) fix the hotkey fallback text; (4) make the E2E hint conditional on the resolved route. ~1-1.5h, dedicated Whisper-prefs PR.
  Status: open
  Last-reviewed: 2026-06-02


- 2026-06-02 · code-review · [bug] · P2 — Assistant preferences: 3 pre-existing Major UI-state bugs surfaced by the E3 decomposition CR review
  Details: CodeRabbit flagged three issues in `SmatchetPreferencesUi_Assistant.cpp` on the E3 (#730) decomposition — all verified pre-existing (E3's diff only hoists static buffers into a struct; no logic change, positional-ImGui balance identical to develop). (1) **Anthropic custom base URL hidden**: Anthropic resolves through `AiBaseUrl` but its section exposes only key/model/consent, so a custom Anthropic endpoint can't be viewed/edited. (2) **Stale probe completions**: the test-connection probe commits its Verified/Failed result even if provider/credentials changed mid-flight (no generation guard) — same class as the AiAssistantController stale-client bug already backlogged from #677. (3) **Implied catalog default not persisted**: the combo shows `catalog[0]` when `modelBuf` is empty but leaves `cfgField` unchanged, so Test-connection can fail with an empty/stale model while the combo looks selected (same class as the E2 Whisper fallback-model finding above).
  Concrete next action: (1) add a base-URL field to the Anthropic section (or document it's shared); (2) gate the probe result-commit on a probe-generation counter captured at launch; (3) persist `cfgField` to `catalog[0]` when empty. ~1.5h, dedicated prefs-tab PR.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-02 · code-review · [bug] · P2 — tracker-config save logs the user's email at INFO (`Email='%s'`) — PII in logs
  Details: `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` Save & Sync logs `LOG_INFO("Updated tracker config (Jira). Domain='%s', Email='%s'", ..., d.cfg.Email.c_str())` — raw user email (PII) into INFO logs. Pre-existing (byte-identical to develop:515-516); surfaced + relocated by the E1 `drawPreferencesWindow` decomposition (#727, CodeRabbit Major). NOT changed in #727 (behaviour-preserving refactor — redacting alters log output). Also flagged: `trackerTypeBuf` can persist non-canonical `"plane"/"github"` on Save (Minor, same PR; develop:175/219-227/440).
  Concrete next action: redact/mask the email in the log (or drop it — Domain is enough for diagnosis); canonicalize `trackerTypeBuf` to the dropdown item on Save. ~20min, dedicated PR.
- 2026-06-02 · code-review · [bug] · P1 — WASAPI capture: UB on silent packets + thread doesn't terminate on GetBuffer failure
  Details: Two genuine pre-existing bugs in `WindowsAudioCapture::CaptureThreadMain` surfaced by the C2-3 decomposition CR review (PR #713), both verified byte-identical to develop (develop:523/513) so NOT changed by the refactor. (1) **Critical UB**: `const BYTE* framePtr = data + (i * frameSize)` is computed unconditionally, but `IAudioCaptureClient::GetBuffer` can return `ppData == NULL` when `AUDCLNT_BUFFERFLAGS_SILENT` is set → NULL pointer arithmetic is UB even though `mono` is then forced to 0. (2) **Major**: on `GetBuffer` failure the code breaks only the inner packet loop; the outer wait loop continues with `running_` still true, so the worker never terminates on a fatal capture error.
  Concrete next action: (1) guard the arithmetic — `const BYTE* framePtr = silent ? nullptr : data + (i * frameSize);` (MixToMonoInt16 is already skipped when silent). (2) on `GetBuffer` failure set `running_.store(false)` (or break the outer loop) before returning, so the thread exits. ~30min, dedicated audio-capture PR.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-02 · code-review · [bug] · P2 — Whisper: stalled-download can't be cancelled + transcribe-once ignores default model
  Details: Two pre-existing Whisper issues surfaced by C2-3 CR review (PR #713), both byte-identical to develop (not refactor regressions). (1) `ModelDownloader::Start` uses `cpr::Timeout{0}` (no overall timeout) and polls `cancelAtom` only inside the `WriteCallback`, so a connection that stalls with zero bytes never runs the callback and can't be cancelled — the worker blocks indefinitely. (2) `ResolveTranscribeOnceMode` (transcribe-once path, develop:196) requires non-empty `cfg.WhisperModel` to use a local model, diverging from the streaming path (WhisperPlugin.cpp:134) which falls back to a downloaded default — so an empty `WhisperModel` makes `auto`/`local` build a bogus `/.bin` path.
  Concrete next action: (1) add `cpr::LowSpeed{1, ConnectTimeout}` (CURLOPT_LOW_SPEED_LIMIT/TIME) so no-byte stalls abort. (2) reconcile the two model-resolution paths — make transcribe-once fall back to the default model like :134. ~1h.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-02 · code-review · [bug] · P2 — field-catalog error path mutates `AvailableFields` unlocked while other threads write it under `availableFieldsMutex_`
  Details: `AppController::SetFieldCatalog`'s entire `if (!error.empty())` branch reads/`std::move`s/clears `AvailableFields` with NO lock held, yet `RefreshFieldCatalog` and the success path write the same vector under `availableFieldsMutex_` from other threads → potential data race / torn read on a fetch-failure concurrent with a refresh. Pre-existing (verified byte-identical vs develop: the unlocked accesses are at develop `AppController_CatalogAndFieldEdit.cpp:214/233/264/277`); surfaced + relocated into `HandleFieldCatalogError` by the B2-B3 size decomposition (PR #704, CodeRabbit Major). NOT fixed in #704 (behaviour-preserving refactor; naively acquiring the mutex risks a deadlock if a caller already holds it).
  Concrete next action: audit the error-branch access pattern; either snapshot under one lock scope or confirm SetFieldCatalog is always called on a single thread. ~1h; do it in a dedicated concurrency PR, not a decomposition.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-02 · code-review · [bug] · P3 — Lua automation with an empty selection processes ZERO tickets (likely should mean "process all")
  Details: `RunAutomationAutoScript`'s loop does `if (selectedSet.empty()) break;` on the first ticket, so an automation job with empty `job.selectedIds` runs `process_ticket` on nothing. CodeRabbit reads the intent as "empty = process all". Pre-existing (byte-identical vs develop loop at `AppController_LuaBindings.cpp:1302-1305`); surfaced by the B4 decomposition (PR #705). NOT changed in #705 (behaviour-preserving refactor; and "empty = nothing" may be an intentional guard against running automation across every ticket unselected).
  Concrete next action: product decision — confirm whether empty selection should process all (then drop the `break`) or stays a deliberate no-op (then add a comment documenting the guard). ~15min once decided.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-06-01 · code-review · [bug] · P2 — `AiAssistantController` turn uses 3 separate `ConfigManager::Load()` snapshots (provider refresh, model/effort resolve, agents-cfg)
  Details: A mid-turn Preferences edit can refresh `client_`/`clientConfig_` for provider A, resolve `chatReq.Model`/effort against a second reload, then build the payload against a third — a torn config view. Pre-existing (verified vs develop: original RunRequest made 3 loads); surfaced + relocated by the RunRequest phase-split (PR #677, CR finding).
  Concrete next action: take ONE `TrackerConfig` snapshot at turn start, thread it through `RefreshProviderForTurn`/`ResolveModelAndEffort`/`BuildChatPayload` (helpers already take params — now cheap).
  Status: open
  Last-reviewed: 2026-06-01

- 2026-06-01 · code-review · [bug] · P2 — `AiAssistantController::RefreshProviderForTurn` returns success on a stale client when rebuild yields null
  Details: If `MakeAiClient` returns null but an old `client_` exists, the turn proceeds through the stale provider instead of failing closed. Pre-existing; behavior preserved by the refactor (PR #677, CR finding).
  Concrete next action: on null rebuild, clear `client_` and return false so the turn fails closed with a clear error.
  Status: open
  Last-reviewed: 2026-06-01

- 2026-06-01 · code-review · [bug] · P3 — `AiAssistantController::ComposeSystemPrompt` writes raw block name into `<smatchet_context block="...">` unescaped
  Details: A `"`/`&`/`<` in a Lua-supplied or future dynamic context-block name corrupts the wrapper. Pre-existing (develop built the same string); now isolated in the `ComposeSystemPrompt` pure helper (PR #677, CR finding) — a clean place to fix.
  Concrete next action: XML/attribute-escape `block.Name`, or assert names match `[A-Za-z0-9_-]`.
  Status: open
  Last-reviewed: 2026-06-01

- 2026-05-28 · deep-audit · [bug] · P2 — `AppController` is a ~1110-line / ~150-method god-object spanning ~9 concerns (code-health)
  Details: `Source/Core/include/AppController.h` is ~1110 lines, ~150 public methods across tracker sync, field-catalog/field-edit + editmeta caching, offline create/field-edit queues, connectivity probing, Lua automation, AI assistant, MCP activity, attachments, app-update, and host callbacks. Owns 10+ `unique_ptr` subsystems + ~10 mutexes; implemented across 10 partial-class `.cpp` files (~7.5k LOC). Highest-coupling node — the natural merge-conflict + reasoning bottleneck. Decomposition is already underway and principled (`OfflineQueueService` / `TicketSyncService` / `LuaAutomationHost` extracted behind ISP `*Deps` interfaces with fakes-for-tests). Supersedes the stale `backlog/BACKLOG_CODE_REVIEW.md` N4 (predates the friend-coupling removal). Verified (deep-audit, adversarially confirmed: header 1110 lines, 10 mutexes, 10 partial TUs).
  Concrete next action: continue the extraction — lift the connectivity-probe FSM and the field-edit/editmeta-cache cluster into their own services behind narrow `*Deps` interfaces; group the optional host-callback setters (OpenUrl / CloseEmbeddedUi / AttachmentViewer / OpenFilePaths / RequestAppQuit) into one `HostCallbacks` struct injected at `Initialize`. Target a thin facade delegating to owned services. Multi-PR.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-28 · deep-audit · [bug] · P3 — Three minor architecture/coupling cleanups (code-health)
  Details: (1) Extracted services still depend on `AppController`'s nested result structs: `Source/Core/include/Sync/OfflineQueueService.h:32` `#include`s the full `AppController.h` and returns `AppController::DeadLetterRestoreSummary` / `*DeleteSummary` (:65-93) — behaviour is inverted via `IOfflineQueueDeps` but the data contract still lives on the god-object, so the service header can't compile standalone. (2) `Source/Core/src/PluginHost.cpp:124` constructs `McpPlugin` by name (`make_unique<McpPlugin>(port)`), the one core spot `IPlugin` isn't fully inverted — build-gated + ADR-0002-governed + dual-target-safe, but Source/Core isn't strictly plugin-agnostic. (3) `Source/Core/src/Tracker/DefaultTrackerBackendFactory.cpp:19-25` hides a synchronous `ConfigManager::Load()` disk-read inside `Create("github")` while the Jira/Plane branches construct arg-less — asymmetric, bakes credentials into the instance. All verified (deep-audit, adversarially confirmed).
  Concrete next action: (1) relocate the queue/dead-letter result structs to `Source/Core/include/Sync/OfflineQueueTypes.h` + re-export aliases on AppController for back-compat; (2) register a `PluginHost::SetMcpPluginFactory(...)` callback wired from bootstrap (mirror `ITrackerBackendFactory`); (3) pass a `TrackerConfig` snapshot into `Create(type, const TrackerConfig&)` or have `GitHubClient` read config lazily like Jira/Plane. Each ~1 h, independent.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-20 · orchestrator · [bug] · P3 — Three UI-thread sync-I/O sites not yet moved to workers (Pillar 2 follow-up from Slice 2 migration)
  Details: Slice 2 of `docs/plans/shipped/pillar-1-2-perf-review-system.md` ran `bash scripts/dev/pillar2-scan.sh` against the full first-party tree + migrated the worker-bound false positives by annotating with `/* PILLAR2_WORKER_ONLY */ // est-latency:` markers. Three hits remain that are NOT worker-bound — UI-thread sync reads with bounded sizes today but flagged for migration. Tracked via `// TODO(pillar2): bug-2026-05-20-ui-sync-reads` comments at each site so the scanner reports them as WARN (not CRITICAL — doesn't block the lint gate). Sites: (1) `Source/Core/src/SmatchetAttachmentPreviewUi.cpp:61` `ParseImageDimensions` — reads the entire attachment file into memory on the UI thread to parse the first 24 bytes for PNG/JPEG dimensions. Up to the 50 MB attachment limit. Easy fix: `seekg` + read 64 bytes. (2) `Source/Core/src/SmatchetPlanDocViewerUi.cpp:95` `ReadCapped` — UI-thread read of `docs/plans/active/*.md` / `docs/adr/*.md` on combo-change. 1 MiB cap, local disk, typically sub-ms but legitimately sync on UI. Could move to worker with `MainThreadDispatcher::PostToMainThread` callback. (3) `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:92` `ReadFileAll` — Lua script load on editor-open (UI thread). Small scripts (typically < 100 KB), sub-ms typical. Could move to worker but the load-on-edit flow is a one-time cost.
  Concrete next action: fix in priority order: (1) ParseImageDimensions — high-impact (50 MB hot path), low effort (~30 min — switch to seekg + 64-byte read). (2) ReadCapped — low-impact (1 MiB cap), low effort (~30 min — worker + dispatcher post-back). (3) ReadFileAll — lowest impact (small files, one-time), defer until a real user reports a hitch. After fix, remove the TODO marker so the scanner stops emitting WARN.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-18 · debug-detective · [bug] · P2 — `SmatchetAiAssistantUi.cpp` `#define ImGui SmatchetLocalizedImGui` macro is invisible at call sites
  Details: While investigating the whisper splice-no-show (PR #258), the verbose `[temp-debug] a7b2c4 HookDictation REGISTER` log fired for `s_inputCharBuf` even though the AI Assistant TU appeared to call raw `ImGui::InputTextMultiline` (which doesn't go through the wrapper hook). 2 detective rounds were spent grepping for `SmatchetLocalizedImGui::InputTextMultiline` callers (none) before noticing the TU-local `#define ImGui SmatchetLocalizedImGui` at line 21. The macro rewrites every `ImGui::` call in the TU to the wrapper transparently. Greppable indirection (`using namespace`) would have shaved the investigation by half.
  Concrete next action: replace `#define ImGui SmatchetLocalizedImGui` with explicit `using namespace SmatchetLocalizedImGui;` (the wrapper's `using namespace ::ImGui;` inside the namespace handles the fallthrough to underlying ImGui functions). Audit all TUs that do the same macro trick and apply uniformly. ~30 min for the AI Assistant TU + grep-and-sweep across the codebase.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-18 · debug-detective · [bug] · P3 — `SmatchetLocalizedImGui::HookDictationOnLastItem` lives inline in a hot header
  Details: Same investigation as the macro entry above. Adding `[temp-debug]` instrumentation to the hook required `#include "Logger.h"` + `<unordered_map>` in `Source/Core/include/SmatchetLocalizedImGui.h` — both contagious to every TU that pulls the wrapper. Long compile churn while iterating on the temp-debug spec; non-trivial cleanup risk (one missed include leaks Logger into hot paths).
  Concrete next action: split `HookDictationOnLastItem` out into a thin `Source/Core/src/SmatchetDictationHook.cpp` with the impl out-of-line behind a forward-declared free function in the header (signature unchanged: `void HookDictationOnLastItem(char*, std::size_t)`). Future debug instrumentation lives in the .cpp without touching every includer. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [bug] · P3 — `AiSseParser::Flush()` synthesises `\n\n` boundary so a final non-terminated chunk delivers as a token
  Details: `AiSseParser.cpp:91` appends `"\n\n"` to the in-progress buffer then re-enters `Feed(nullptr, 0, ...)` to force-emit a final frame. If a malicious or buggy server sends a final non-terminated chunk that happens to parse as a valid SSE frame body, it gets dispatched as a token even though the server never indicated the frame was complete. Low-impact (just a delivered chunk) but the policy "discard residual partial frame on Flush" is safer.
  Concrete next action: change `Flush` to clear `buffer_` without re-feeding (drop the partial frame). Update `AiSseParser.test.cpp` "many small Feeds" or add a new test asserting Flush on `"data:partial"` (no boundary) emits zero events. ~20 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `LuaToJson` / `JsonToLua` + `kJsonToLuaMaxDepth` duplicated verbatim across `AppController_LuaBindingsCore.cpp` ↔ `AppController_LuaBindings.cpp`
  Details: File comment names the duplication as intentional (post-split keeps Core ImGui-free). Drift risk on the next marshalling change.
  Concrete next action: lift to a shared internal header reachable from both TUs. Surfaced by retrospective code-review sweep on PR #144.
  Status: open
  Last-reviewed: 2026-05-17
