# Agent self-improvement — bug (DEPRECATED)

> **DEPRECATED (ADR-0014, 2026-06-03).** Product bugs now live as **GitHub Issues**
> — see [`../../agent-rules/issue-triage.md`](../../agent-rules/issue-triage.md). This
> file is **frozen**: no new entries. The one-time migration (G) is **done**:
> 9 genuine product bugs became GitHub Issues (#734, #818, #820–#826), 4 tech-debt
> items moved to [`debt.md`](debt.md). The entries **below are the ambiguous residue** —
> they need a human bug-vs-Issue call (per the protocol, ambiguous is never silently
> filed). Resolve each: file an Issue, move to `debt.md`, or close as won't-do.
>
> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug (deprecated) · debt · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Ambiguous residue — pending a human bug-vs-Issue decision (ADR-0014 migration). -->

- 2026-06-01 · code-review · [bug] · P2 — `AiAssistantController` turn uses 3 separate `ConfigManager::Load()` snapshots (provider refresh, model/effort resolve, agents-cfg)
  Details: A mid-turn Preferences edit can refresh `client_`/`clientConfig_` for provider A, resolve `chatReq.Model`/effort against a second reload, then build the payload against a third — a torn config view. Pre-existing (verified vs develop: original RunRequest made 3 loads); surfaced + relocated by the RunRequest phase-split (PR #677, CR finding).
  Concrete next action: take ONE `TrackerConfig` snapshot at turn start, thread it through `RefreshProviderForTurn`/`ResolveModelAndEffort`/`BuildChatPayload` (helpers already take params — now cheap).
  Status: applied (2026-06-20 roadmap campaign — shipped #1515)
  Last-reviewed: 2026-06-20

- 2026-05-20 · orchestrator · [bug] · P3 — Three UI-thread sync-I/O sites not yet moved to workers (Pillar 2 follow-up from Slice 2 migration)
  Details: Slice 2 of `docs/plans/shipped/pillar-1-2-perf-review-system.md` ran `bash scripts/dev/pillar2-scan.sh` against the full first-party tree + migrated the worker-bound false positives by annotating with `/* PILLAR2_WORKER_ONLY */ // est-latency:` markers. Three hits remain that are NOT worker-bound — UI-thread sync reads with bounded sizes today but flagged for migration. Tracked via `// TODO(pillar2): bug-2026-05-20-ui-sync-reads` comments at each site so the scanner reports them as WARN (not CRITICAL — doesn't block the lint gate). Sites: (1) `Source/Core/src/SmatchetAttachmentPreviewUi.cpp:61` `ParseImageDimensions` — reads the entire attachment file into memory on the UI thread to parse the first 24 bytes for PNG/JPEG dimensions. Up to the 50 MB attachment limit. Easy fix: `seekg` + read 64 bytes. (2) `Source/Core/src/SmatchetPlanDocViewerUi.cpp:95` `ReadCapped` — UI-thread read of `docs/plans/active/*.md` / `docs/adr/*.md` on combo-change. 1 MiB cap, local disk, typically sub-ms but legitimately sync on UI. Could move to worker with `MainThreadDispatcher::PostToMainThread` callback. (3) `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:92` `ReadFileAll` — Lua script load on editor-open (UI thread). Small scripts (typically < 100 KB), sub-ms typical. Could move to worker but the load-on-edit flow is a one-time cost.
  Concrete next action: fix in priority order: (1) ParseImageDimensions — high-impact (50 MB hot path), low effort (~30 min — switch to seekg + 64-byte read). (2) ReadCapped — low-impact (1 MiB cap), low effort (~30 min — worker + dispatcher post-back). (3) ReadFileAll — lowest impact (small files, one-time), defer until a real user reports a hitch. After fix, remove the TODO marker so the scanner stops emitting WARN.
  Status: partially applied (2026-06-20 trap-sweep — shipped: site 1 SmatchetAttachmentPreviewUi 64KB-bounded; remaining: sites 2+3 (SmatchetPlanDocViewerUi, LuaConsolePlugin) UI-thread sync reads, site 3 deferred)
  Last-reviewed: 2026-06-20

- 2026-05-18 · debug-detective · [bug] · P2 — `SmatchetAiAssistantUi.cpp` `#define ImGui SmatchetLocalizedImGui` macro is invisible at call sites
  Details: While investigating the whisper splice-no-show (PR #258), the verbose `[temp-debug] a7b2c4 HookDictation REGISTER` log fired for `s_inputCharBuf` even though the AI Assistant TU appeared to call raw `ImGui::InputTextMultiline` (which doesn't go through the wrapper hook). 2 detective rounds were spent grepping for `SmatchetLocalizedImGui::InputTextMultiline` callers (none) before noticing the TU-local `#define ImGui SmatchetLocalizedImGui` at line 21. The macro rewrites every `ImGui::` call in the TU to the wrapper transparently. Greppable indirection (`using namespace`) would have shaved the investigation by half.
  Concrete next action: replace `#define ImGui SmatchetLocalizedImGui` with explicit `using namespace SmatchetLocalizedImGui;` (the wrapper's `using namespace ::ImGui;` inside the namespace handles the fallthrough to underlying ImGui functions). Audit all TUs that do the same macro trick and apply uniformly. ~30 min for the AI Assistant TU + grep-and-sweep across the codebase.
  Status: wont-fix (2026-06-21 decision — keep the macro) — the `#define ImGui SmatchetLocalizedImGui` is a deliberate, documented localization mechanism; replacing it with `using namespace` is unsound (cannot redirect qualified `ImGui::` names → would silently bypass localization across 40 TUs), and a full call-site conversion is a large risky sweep with no functional gain. Decision: keep the macro.
  Last-reviewed: 2026-06-21

- 2026-05-17 · code-review · [bug] · P3 — `AiSseParser::Flush()` synthesises `\n\n` boundary so a final non-terminated chunk delivers as a token
  Details: `AiSseParser.cpp:91` appends `"\n\n"` to the in-progress buffer then re-enters `Feed(nullptr, 0, ...)` to force-emit a final frame. If a malicious or buggy server sends a final non-terminated chunk that happens to parse as a valid SSE frame body, it gets dispatched as a token even though the server never indicated the frame was complete. Low-impact (just a delivered chunk) but the policy "discard residual partial frame on Flush" is safer.
  Concrete next action: change `Flush` to clear `buffer_` without re-feeding (drop the partial frame). Update `AiSseParser.test.cpp` "many small Feeds" or add a new test asserting Flush on `"data:partial"` (no boundary) emits zero events. ~20 min.
  Status: applied (2026-06-21 P3 sweep — shipped #1527)
  Last-reviewed: 2026-06-21
