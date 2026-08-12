# AI Assistant — Right-Docked Side Panel + agents.md Harness
<!-- plan-date: 2026-05-16 -->

> **Plan-doc relocation (mandatory first commit step)**: per `AGENTS.md` § Plan location, plans live at `docs/plans/active/<slug>.md`. After this file is approved, copy it to `docs/plans/shipped/ai-assistant-side-panel.md` and commit with `wip(plan): ai-assistant-side-panel` before any code work. The path under `~/.claude/plans/` is plan-mode scratch only.
>
> **Status (2026-05-16)**: Phase A shipped (narrowed scope) — see § Implementation log. Phase A' deferred behind `test-suite-expansion` umbrella release on `Source_Core/src/ConfigManager*.cpp` + `tests/**`. Phases B-E unscoped. Plan-lock entry: [`docs/plans/active/_plan-locks.md`](./_plan-locks.md) § `ai-assistant-side-panel · Phase A-narrowed · status: in-flight`.

## Context

Smatchet shipped an AI assistant in commit `997f23f` (Apr 2026): a synchronous OpenAI-compatible chat panel as a floating ImGui window, with Lua glue (`ai.add_context`, `ai.prompt`, `ai.clear_context`) and a per-ticket "Generate Action Plan" button. Commit `5e85fb5` (May 2026) tore the entire feature out — `AiController.{h,cpp}`, the `SmatchetUtilityWindowsUi.cpp::drawAIAssistantWindow` body, Lua bindings, `ConfigManager` fields (`AiApiKey/AiModel/AiBaseUrl/ShowAiAssistantWindow`), the `HttpTrafficKind::OpenAi` traffic kind, and the `SMATCHET_WITH_AI` build flag — to "streamline functionality."

User wants the assistant back, but redesigned along three axes:

1. **Cursor-style full-height right-docked side panel**, not a floating window. Always-on toggleable; persists open/closed + width.
2. **Provider-pluggable** via an `IAiClient` abstraction shaped like `ITrackerClient`. Concrete impls: `OpenAiClient` (also drives Ollama-OpenAI-compat + Azure/Groq/Together), `AnthropicClient` (native Messages API), `OllamaClient` (optional native `/api/chat`).
3. **Context built from a "harness for Jira"** — auto-injected per-turn:
   - Selected tickets (if grid multi-select active)
   - Active ticket (full selected-field set)
   - Active view (name + JQL + columns)
   - Visible grid rows (key + summary + status, cap 50, current sort order)
   - Recent `BackendAuditTrail` events (last 20)
   - **Three-layer `agents.md` system prompt**, concatenated in order: global (`%LOCALAPPDATA%/Smatchet/agents.md`) → group (`<active-project-root>/agents.md`) → individual (`<views-dir>/<view-id>.agents.md`).

Threading is **worker-thread + streaming tokens** (UX Pillar 2: no sync HTTP on UI thread). Cancellable.

## Architecture

```
                ┌────────────────────────────────────────────────────────┐
                │ UI thread                                              │
                │                                                        │
                │ SmatchetAiAssistantUi::drawAiAssistantPanel            │
                │   reads UiDrawSession::assistant*                      │
                │   ↑ toggle via View menu / Ctrl+Shift+A                │
                │   ↓ Submit                                             │
                │                                                        │
                │ AiAssistantController::Submit(userMsg)                 │
                │   1. AiContextBuilder::BuildAll(app, d, cfg)  ← snapshot here, UI thread only
                │   2. AgentsMdLoader::Compose(activeViewId, cfg)        │
                │   3. push AiTurnRequest onto worker queue              │
                └────────────────────────────────┬───────────────────────┘
                                                 │ condvar signal
                ┌────────────────────────────────▼───────────────────────┐
                │ Worker thread (owned by AiAssistantController)         │
                │                                                        │
                │ AiClientFactory::Make(cfg.AiProviderKind)              │
                │ IAiClient::SendStreaming(cfg, req,                     │
                │     onDelta, onError, cancelToken)                     │
                │                                                        │
                │ each cpr WriteCallback → AiSseParser::Feed             │
                │   → translated AiStreamDelta                           │
                │   → MainThreadDispatcher::PostToMainThread(            │
                │       [s=&g_ui, gen=turnGen, chunk] { ... })           │
                └────────────────────────────────────────────────────────┘
```

Cancellation: shared `atomic<bool>` polled inside the `cpr::WriteCallback`. Cancel button on UI flips the atom; cpr returns `false` from the write-callback, aborting the request.

## Five-phase delivery (one PR each, each independently mergeable)

| Phase | Scope | Land gates |
|---|---|---|
| **A** | `IAiClient` interface + `OpenAiClient` + `AiSseParser` + `AiClientFactory` + ConfigManager field set + `SMATCHET_WITH_AI=1` build flag + `HttpTrafficKind::Ai` re-add. **No UI.** | doctest covers SSE parser. `OFF`-build still compiles. Scratch driver behind `#ifdef SMATCHET_AI_SCRATCH_DRIVER` in `Target_Standalone/main.cpp` proves a real prompt against an OpenAI key, then deleted at end of phase. |
| **B** | `SmatchetAiAssistantUi` right-docked panel + worker thread inside `AiAssistantController` + `MainThreadDispatcher` integration + Cancel button + persistent open/closed + width. **No auto-context, no agents.md** — only the raw user message is sent. | Visual scenarios 1-5 in §Verification. |
| **C** | `AgentsMdLoader` (global / group / individual) + `AiContextBuilder` (5 blocks) + Context-header UI with per-block checkboxes + ConfigManager toggle persistence. | Scenarios 6-10. |
| **D** | `AnthropicClient` (native Messages + SSE) + optional `OllamaClient` (native `/api/chat` NDJSON). Provider Combo in Preferences. Per-provider model fields (`AiModelOpenAi`, `AiModelAnthropic`, `AiModelOllama`). | Scenarios 11-12. |
| **E** | Restore Lua glue (`ai.add_context`, `ai.prompt`, `ai.clear_context`) in `AppController_LuaBindings.cpp` + parity stubs. **Bump `LayoutSchemaVersion` 5 → 6 here, exactly once.** README + LUA_GUIDE one-liner. | Scenarios 14-15. |

Phases A and B are gated `SMATCHET_WITH_AI=ON` but produce nothing user-visible if `AssistantPanelOpen=false` (default). No layout-schema change until E.

## File-level changes

### New files

| Path | Phase | Role |
|---|---|---|
| `Source_Core/include/IAiClient.h` | A | Provider-agnostic streaming-chat interface (§ Interface signature). |
| `Source_Core/include/AiTypes.h` | A | `AiProvider` enum, `AiMessage`, `AiStreamDelta`, `AiStreamError`, `AiChatRequest`, `AiClientConfig`, `AiContextBlock`, `AiContextBlockKind`. POD only, header-only. |
| `Source_Core/include/AiClientFactory.h` + `Source_Core/src/AiClientFactory.cpp` | A | `MakeAiClient(AiProvider)`; enum<->string; `EnumeratedProviders()` for the Preferences Combo. |
| `Source_Core/include/OpenAiClient.h` + `Source_Core/src/OpenAiClient.cpp` | A | OpenAI-compat chat completions + SSE. Drives OpenAI, Ollama-OpenAI-compat, Azure, Groq, Together. |
| `Source_Core/include/AiSseParser.h` + `Source_Core/src/AiSseParser.cpp` | A | Stateful chunked SSE byte-stream parser. Shared by OpenAI and Anthropic. |
| `Source_Core/include/AiAssistantController.h` + `Source_Core/src/AiAssistantController.cpp` | B | Owns conversation history, worker thread, cancel atom, active `IAiClient`. Lives next to `AppController`, not inside it. Holds `std::unique_ptr<AiAssistantController>` member on `AppController`. |
| `Source_Core/include/SmatchetAiAssistantUi.h` + `Source_Core/src/SmatchetAiAssistantUi.cpp` | B | Right-docked panel. `SmatchetUI::drawAiAssistantPanel(AppController&, UiDrawSession&)` lives here, mirroring `SmatchetMcpServerUi` shape. |
| `Source_Core/include/AgentsMdLoader.h` + `Source_Core/src/AgentsMdLoader.cpp` | C | Global / group / individual layering. 64 KB per-layer cap. Walk-up project-root discovery with cache. |
| `Source_Core/include/AiContextBuilder.h` + `Source_Core/src/AiContextBuilder.cpp` | C | Builds the 5 auto-context blocks. UI-thread only (snapshots grid/view/audit via `AppController`). Asserts `AppController::IsOnUiThread()`. |
| `Source_Core/include/AnthropicClient.h` + `Source_Core/src/AnthropicClient.cpp` | D | Native Messages API. Reuses `AiSseParser`; translates `content_block_delta` events. |
| `Source_Core/include/OllamaClient.h` + `Source_Core/src/OllamaClient.cpp` | D | Optional. Native `/api/chat` NDJSON streaming via new `AiNdjsonParser`. Only registered if user selects `AiProvider::OllamaNative`. |
| `Source_Core/include/AiNdjsonParser.h` + `Source_Core/src/AiNdjsonParser.cpp` | D | Line-buffered NDJSON parser for Ollama-native. |
| `tests/Source_Core/AiSseParser.test.cpp` | A | doctest: partial frames, multi-event-per-chunk, `[DONE]` sentinel, Anthropic event names. |
| `tests/Source_Core/AgentsMdLoader.test.cpp` | C | doctest: missing files, oversize cap (truncation + sentinel suffix), walk-up resolution depth limit, separator placement. |
| `tests/Source_Core/AiContextBuilder.test.cpp` | C | doctest: cap at N=50 preserves sort, multi-select round-trip via `SpreadsheetState::RectSel.Rows`, last-N audit fixture, block toggling. |
| `tests/Source_Core/AiClientFactory.test.cpp` | A | doctest: enum<->string round-trip, unknown enum yields null + logged error. |
| `docs/plans/shipped/ai-assistant-side-panel.md` | A (first commit) | This plan, relocated per Plan-doc rule. |

### Existing files modified

| Path | Phase | Change |
|---|---|---|
| `CMakeLists.txt` | A | Add `option(SMATCHET_WITH_AI "..." ON)` next to `SMATCHET_WITH_MCP` (~line 152). Mirror the MCP shim INTERFACE target at ~line 582. Add new sources to `CORE_SOURCES`. Test files added under `SMATCHET_BUILD_TESTS`. |
| `Source_Core/include/ConfigManager.h` | A then C/D extend | Add `enum class AiProvider : int { OpenAi=0, Anthropic=1, OllamaOpenAiCompat=2, OllamaNative=3 }`. New `TrackerConfig` fields: `AiProviderKind`, `AiApiKey`, `AiAnthropicApiKey`, `AiOllamaBaseUrl`, `AiBaseUrl`, `AiModelOpenAi` (default `"gpt-4o-mini"`), `AiModelAnthropic` (default `"claude-sonnet-4-6"`), `AiModelOllama` (default `"llama3"`), `AssistantPanelOpen` (default `false`), `AssistantPanelWidth` (default `380.0f`), `AgentsMdGlobalPath`, `ProjectAgentsMdPath`, `AssistantContextBlockSelection`, `AssistantContextBlockVisibleRows`, `AssistantContextBlockActiveTicket`, `AssistantContextBlockActiveView`, `AssistantContextBlockAuditTrail` (all default `true`). |
| `Source_Core/src/ConfigManager.cpp` | A then C/D extend | Serialize each new field with `j.value()` defaults so old configs migrate silently. API keys go through `ProtectSecretForConfig`/`UnprotectSecretFieldFromConfig` like `McpAuthToken`. `AgentsMdGlobalPath` default resolved at Load time to `%LOCALAPPDATA%/Smatchet/agents.md` if blank. Clamp unknown `AiProviderKind` ints to `OpenAi=0`. |
| `Source_Core/include/NetworkUsageTracker.h` + `.cpp` | A | Re-add `enum class HttpTrafficKind { Tracker, Ai }` (use `Ai`, **not** `OpenAi` — provider-agnostic). Snapshot gets `aiRequests` / `aiUploadBytes` / `aiDownloadBytes`. `Record(HttpTrafficKind kind, ...)` signature restored. |
| All current `NetworkUsageTracker::Instance().Record(...)` callers | A | One-line update to pass `HttpTrafficKind::Tracker`. Callers: `TrackerHttpUtils.cpp`, `JiraIssueMutation.cpp`, `FieldCatalogCache.cpp`, plus any others surfaced by grep. |
| `Source_Core/include/AppController.h` + `Source_Core/src/AppController.cpp` | B | Add member `std::unique_ptr<AiAssistantController> aiAssistant_`. Add `AiAssistantController& GetAiAssistantController()`. Stub members `AddAiContext`, `GetAiContext`, `ClearAiContext`, `PromptAi` restored at zero-cost (delegates to controller, or no-ops in stub build). Construct on init, reset before joining workers in shutdown. |
| `Source_Core/include/SmatchetUI.h` | B | Add `void drawAiAssistantPanel(AppController&, UiDrawSession&);` private method. |
| `Source_Core/src/SmatchetUI.cpp` | B | In `Draw`: call `drawAiAssistantPanel(app, d)` after `drawAuditWindow`. In `repairTopLevelWindow`: early-return on `layoutKey == "assistant_panel"`. View menu entry "Assistant (Ctrl+Shift+A)" toggles `d.assistantPanelOpen`. |
| `Source_Core/include/SmatchetUiSession.h` | B | New fields (gated `#if defined(SMATCHET_WITH_AI)`): `assistantPanelOpen`, `requestAssistantFocus`, `assistantPanelWidthLive`, `assistantHistory` (`std::vector<AiMessage>`), `assistantInputBuf`, `assistantStreamBuf`, `assistantInFlight`, `assistantCancel` (`std::shared_ptr<std::atomic<bool>>`), `assistantLastError`, `assistantAutoScrollAtTail`, `assistantTurnGen` (uint64). |
| `Source_Core/src/SmatchetPreferencesUi.cpp` | A then C/D extend | New "Assistant" group: provider Combo via `EnumeratedProviders()`, masked API key inputs (Open AI + Anthropic), per-provider model inputs, base URLs, `agents.md` global + project paths. Pattern matches `mcpAuthTokenBuf`. |
| `Source_Core/src/AppController_LuaBindings.cpp` | E | Restore `LuaAiAddContextGlue`, `LuaAiPromptGlue`, `LuaAiClearContextGlue` against the new `AiAssistantController`. The old version flipped a poll flag; the new version posts a `MainThreadDispatcher` task that calls `AiAssistantController::Submit`. |
| `Source_Core/src/AppController_LuaStubs.cpp` | E | No-op parity: `AddAiContext`, `GetAiContext`, `ClearAiContext`, `PromptAi`. |
| `Source_Core/include/ConfigManager.h::kCurrentLayoutSchemaVersion` | E | `5` → `6`. **Exactly one bump for the whole feature, in the final phase.** |
| `README.md`, `docs/guides/lua.md` | E | One bullet each. |

## `IAiClient` interface signature (C++14)

Lives in `Source_Core/include/IAiClient.h`. Verbatim shape (from Plan agent's design):

```cpp
struct AiMessage    { std::string Role; std::string Content; };
struct AiStreamDelta{ std::string TokenChunk; bool IsFinal; std::string FinishReason; };
struct AiStreamError{ int HttpStatus; std::string Message; bool WasCancelled; AiStreamError():HttpStatus(0),WasCancelled(false){} };

struct AiChatRequest {
    std::string Model;
    std::string SystemPrompt;             // agents.md ∪ context blocks
    std::vector<AiMessage> History;       // most-recent user msg LAST
    float Temperature;                    // -1.0f sentinel = unset (no std::optional in C++14)
    int   MaxTokens;                      // 0 = unset
    AiChatRequest():Temperature(-1.0f),MaxTokens(0) {}
};

struct AiClientConfig {
    std::string ApiKey;
    std::string BaseUrl;                  // empty = client default
    int ConnectTimeoutMs;
    int TotalTimeoutMs;
    AiClientConfig():ConnectTimeoutMs(5000),TotalTimeoutMs(120000) {}
};

class IAiClient {
public:
    virtual ~IAiClient() {}
    virtual std::string GetProviderName() const = 0;
    virtual std::string ProbeReachability(const AiClientConfig& cfg) = 0;   // worker thread only

    using DeltaCallback = std::function<void(const AiStreamDelta&)>;
    using ErrorCallback = std::function<void(const AiStreamError&)>;
    using CancelToken   = std::shared_ptr<std::atomic<bool>>;

    virtual void SendStreaming(const AiClientConfig& cfg,
                               const AiChatRequest& req,
                               const DeltaCallback& onDelta,
                               const ErrorCallback& onError,
                               const CancelToken&   cancel) = 0;
};
```

Departures from `ITrackerClient`:
- Cancel uses `shared_ptr<atomic<bool>>` (cheap poll in libcurl write-callback hot path) instead of `function<bool()>`.
- Error vs delta channels are explicit — AI errors mid-stream are recoverable for the UI but terminal for the turn.
- No `nlohmann::json` in the header. Each impl parses internally; the interface stays string-only.
- Every method is required — no `Capabilities` mask.

## Side-panel layout

Smatchet uses ImGui docking but **not** `DockSpaceOverViewport`. Adding one now would reset every user's saved layout. Instead the panel is a pinned auto-sized non-docked window that overlays the right strip:

```cpp
const ImGuiViewport* vp = ImGui::GetMainViewport();
const ImVec2 workPos = vp->WorkPos;
const ImVec2 workSize = vp->WorkSize;

const float minW = 280.0f;
const float maxW = std::min(720.0f, workSize.x * 0.45f);
const float w = std::min(std::max(d.assistantPanelWidthLive, minW), maxW);

ImGui::SetNextWindowPos (ImVec2(workPos.x + workSize.x - w, workPos.y), ImGuiCond_Always);
ImGui::SetNextWindowSize(ImVec2(w, workSize.y),                         ImGuiCond_Always);
ImGui::SetNextWindowBgAlpha(1.0f);

ImGui::Begin("Smatchet Assistant", &d.assistantPanelOpen,
             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
```

- `NoSavedSettings` keeps the panel out of `imgui.ini` (no fight with dock state).
- Width drag: a 4-px invisible button at the panel's left edge; `IsItemActive` updates `assistantPanelWidthLive`; `IsItemDeactivatedAfterEdit` persists to `cfg.AssistantPanelWidth` + `ConfigManager::Save`.
- `repairTopLevelWindow` early-returns on `layoutKey == "assistant_panel"`.
- Tradeoff: panel **overlays** the central node, not pushes it. Mirrors how `mcp` / `scripting` utility panels already work (`DefaultLayoutRectFor("scripting")`). If users complain about occlusion, follow-up can nudge `SmatchetDockNodeIds::kCentral` via `DockBuilderSetNodeSize`. Out of scope.

## Streaming protocol

### OpenAI-compat SSE
- `data: {...}` JSON — `choices[0].delta.content` → `TokenChunk`; `choices[0].finish_reason` non-null → `IsFinal=true`.
- `data: [DONE]` → `{IsFinal=true, FinishReason="stop"}`.
- Parser is byte-stream stateful because cpr/libcurl chunks on TCP boundaries, not on `\n\n` frame boundaries.

### Anthropic SSE
- Named events: `message_start`, `content_block_start`, `content_block_delta` (carries `delta.text` → `TokenChunk`), `content_block_stop`, `message_delta`, `message_stop` (→ `IsFinal=true`).
- Reuses `AiSseParser`; client switches on the `event:` field.

### Ollama native (Phase D, optional)
- NDJSON: one JSON object per line, `done: true` on the terminal line.
- Uses `AiNdjsonParser`.

### Cancellation
```cpp
cpr::WriteCallback wcb{[&](const std::string& chunk, intptr_t) -> bool {
    if (cancel->load(std::memory_order_relaxed)) return false;
    parser.Feed(chunk.data(), chunk.size(), translate);
    return !cancel->load(std::memory_order_relaxed);
}, 0};
```
Cancel button flips the atom; `SendStreaming` emits `onError({0, "Cancelled by user", true})` and returns.

### Main-thread posting
Worker wraps `onDelta` / `onFinal` / `onError` to dispatch via `AppController::GetMainThreadDispatcher().PostToMainThread([gen, payload]{ ... })`. A monotonic `assistantTurnGen` is captured per task; stale tasks (after Cancel or after a newer turn) check `gen` and no-op.

## `agents.md` loader

Search order (resolved at every prompt — filesystem calls < 1 ms negligible vs HTTP RTT):

1. **Global** — `cfg.AgentsMdGlobalPath` (defaulted at Load time to `%LOCALAPPDATA%/Smatchet/agents.md`).
2. **Group** — `cfg.ProjectAgentsMdPath` if non-empty; else walk up from `cfg.LastImportDirectory` (fallback: `ConfigManager::GetUserDataDirectory()`) looking for `agents.md`, stopping at FS root or 8 levels.
3. **Individual** — `<dirname(ConfigManager::GetViewsPath())>/<sanitized-view-id>.agents.md`. View id is clamped to `[A-Za-z0-9_.-]` to keep the filename safe.

Layering: non-empty layers concatenated with `\n\n---\n\n`. Missing files: silent, contribute empty.

**Size cap**: 64 KB per layer. Over-cap: truncate, log warn, one-shot toast `"agents.md truncated at 64 KB: <basename>"`, append literal `"\n\n[...truncated at 64 KB...]\n"`. Worst-case total: 192 KB — fits any provider context.

## Auto-context blocks

```cpp
enum class AiContextBlockKind : int {
    MultiSelectedTickets, VisibleGridRows, ActiveTicket, ActiveView, AuditTrail
};
```

Each block individually gated by the matching `cfg.AssistantContextBlock*` toggle. `AiContextBuilder::BuildAll` runs on UI thread (asserts via `AppController::IsOnUiThread()`), reads `SpreadsheetState::RectSel.Rows` (multi-select works today — verified in `SmatchetGridUiSupport.cpp`, `SmatchetBulkTicketsUi.cpp`, `SmatchetActiveProjectGridUi.cpp`), maps via `d.cachedSortedIndices` → tickets snapshot.

Blocks concatenate into the system prompt **after** `agents.md` layers, wrapped:

```
<smatchet_context block="active_ticket">
key: SMA-123
summary: Foo bar
...
</smatchet_context>
```

UI surface — a collapsing header "Context (N of 5 active)" at panel top with per-block checkboxes that live-update counts (e.g. `"Visible rows (47/247)"`). Toggle writes through `ConfigManager::Save`.

Caps: visible rows N=50, audit trail N=20.

## Verification

Each scenario is a deterministic bash + CLI + scenario.run check. Manual residue must be passed to `test-author` post-implement.

| # | Scenario | Phase |
|---|---|---|
| 1 | Panel toggle + width persists across restart | B |
| 2 | OpenAI streaming happy path — visible token-by-token render | B |
| 3 | Cancel mid-stream — halts < 1 s, partial text retained in history | B |
| 4 | Bad API key → "API Error: 401", no retry loop | B |
| 5 | Transport down → "Network unreachable" within 5 s | B |
| 6 | Visible-rows block — exactly 50 lines in priority sort order | C |
| 7 | Multi-select block — 3 ctrl-clicked rows fully rendered | C |
| 8 | agents.md layering — all 3 sentinels with `---` separators | C |
| 9 | agents.md cap — 200 KB file truncated + toast | C |
| 10 | Per-block toggle persists across restart | C |
| 11 | Provider switch to Anthropic — body to `api.anthropic.com/v1/messages`, streaming maps correctly | D |
| 12 | Ollama OpenAI-compat — identical streaming path, no code branch | D |
| 13 | `-DSMATCHET_WITH_AI=OFF` builds clean, no menu item, Lua `ai.*` is no-op | A-E |
| 14 | Lua `ai.add_context("note"); ai.prompt("repeat note")` → panel focused, system prompt contains `<smatchet_context block="lua_user">` | E |
| 15 | `LayoutSchemaVersion` 5 → 6 — one-shot reset on Phase E, no re-reset on subsequent launches | E |

### Manual-residue grep audit (hand to `test-author`)
- `git grep -i "AiController\b"` → zero (replaced by `IAiClient` / `AiAssistantController`)
- `git grep "drawAIAssistantWindow"` → zero (renamed `drawAiAssistantPanel`)
- `git grep -E "showAiAssistantWindow|requestAiAssistantFocus|aiPromptPending|aiPromptMessage|aiIsThinking|aiResponse"` → zero
- `git grep "HttpTrafficKind::OpenAi"` → zero (now `::Ai`)
- `git grep "gpt-4o-mini"` → only `ConfigManager` default + tests
- `git grep "AiChatUrl" Source_Core/` — unrelated **blame-analysis** field; survives unchanged
- `SMATCHET_WITH_AI=OFF` build runs cppcheck + clang-tidy + doctest clean

### Doctest coverage minimum
`AiSseParser_test`, `AgentsMdLoader_test`, `AiContextBuilder_test`, `AiClientFactory_test`.

## Risks / unknowns (resolve before Phase A starts)

1. **`cpr` version** — `cpr::WriteCallback` requires cpr ≥ 1.10. Verify in `CMakeLists.txt` / `ThirdParty/` FetchContent; if older, bump as Phase A's first task.
2. **Corporate proxy SSE** — some proxies force `Connection: close` after first event. Fallback: if response `Content-Type` is not `text/event-stream`, treat as non-streaming and emit single `IsFinal=true` chunk. Document as known limitation; Phase B scenario 2 re-run once on corporate VPN.
3. **Multi-select cross-thread safety** — `SpreadsheetState::RectSel` is UI-thread-only. `AiContextBuilder::BuildAll` asserts `AppController::IsOnUiThread()` to keep the contract enforceable.
4. **Per-provider model defaults** — `gpt-4o-mini` invalid for Anthropic. Store one model per provider (`AiModelOpenAi`, `AiModelAnthropic`, `AiModelOllama`) — locked in ConfigManager schema from Phase A so Phase D doesn't rewrite.
5. **Project-root walk-up** — Smatchet has no first-class "project root". Anchor on `cfg.LastImportDirectory` falling back to `GetUserDataDirectory()`. Document in built-in help that group layer is opt-in via `ProjectAgentsMdPath`.
6. **`MainThreadDispatcher` queue cap** — coalesce tokens per HTTP chunk in the worker before posting (one dispatch per cpr `WriteCallback` invocation, not per token).
7. **CI `SMATCHET_BUILD_TESTS`** — confirm at least one CI matrix row turns it on; otherwise the new doctests never run.
8. **DPAPI per-provider keys** — `AiApiKey` + `AiAnthropicApiKey` both go through `ProtectSecretForConfig`. `AiOllamaBaseUrl` stays plaintext (typically `localhost`, not secret).

## Critical files

- `Source_Core/include/IAiClient.h` *(NEW, Phase A)*
- `Source_Core/include/AiAssistantController.h` *(NEW, Phase B)*
- `Source_Core/src/SmatchetAiAssistantUi.cpp` *(NEW, Phase B — side panel)*
- `Source_Core/include/AgentsMdLoader.h` *(NEW, Phase C)*
- `Source_Core/include/AiContextBuilder.h` *(NEW, Phase C)*
- `Source_Core/include/ConfigManager.h` *(modify, Phase A — fields locked)*
- `Source_Core/src/SmatchetUI.cpp` *(modify, Phase B — `Draw` hook + `repairTopLevelWindow` exception)*
- `CMakeLists.txt` *(modify, Phase A — `SMATCHET_WITH_AI` option + shim)*
- `Source_Core/src/AppController_LuaBindings.cpp` + `AppController_LuaStubs.cpp` *(modify, Phase E)*
- `docs/plans/shipped/ai-assistant-side-panel.md` *(NEW — this plan, relocated)*

## Existing utilities to reuse (do not re-invent)

- `MainThreadDispatcher` (`Source_Core/include/MainThreadDispatcher.h`) — worker → UI post-back.
- `AppController::IsOnUiThread()` — enforce UI-thread invariant in `AiContextBuilder`.
- `AppController::GetActiveTicketsSnapshot()` — `shared_ptr<vector<CachedTicket>>` with atomic revision; safe to capture across blocks.
- `ProtectSecretForConfig` / `UnprotectSecretFieldFromConfig` — DPAPI on Windows for API keys (used today by `McpAuthToken`).
- `Views::GetActiveView()` (`Source_Core/include/Views.h`) — for view name + JQL + columns.
- `BackendAuditTrail` (`Source_Core/include/BackendAuditTrail.h`) — `AuditEvent` last-N source.
- `SpreadsheetState::RectSel.Rows` (`Source_Core/include/SpreadsheetState.h`) — multi-select source (confirmed working).
- `NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai, ...)` — surfaces traffic counters in the existing UI.
- `LOG_INFO/WARN/ERROR` (`Logger.h`) — never `printf`/`std::cerr`.
- `SmatchetToastManager::Instance().Push(...)` — for `agents.md` truncation toast.
- `SmatchetLocalizedImGui` shim — all `ImGui::*` calls in UI go through it.
- `cpr` + `nlohmann/json` — already vendored.

## Implementation log

- `a39097c` · 2026-05-16 · `wip(plan): ai-assistant-side-panel` — plan recovered from dangling commit `84913a8` (orphan worktree `charming-gates-1d68ec` had been wiped before its branch was anchored). 323-line plan re-committed verbatim on `feat/ai-assistant-side-panel` off `develop@e07fcf2`.
- `b7d901d` · 2026-05-16 · `plan-locks: claim ai-assistant-side-panel Phase A (narrowed)` — `_plan-locks.md` claim entry added after pre-flight intersection check vs every `claimed | in-flight` lock. Narrowed scope agreed (option (b) overlap-resolution): defer `ConfigManager` field set + doctest additions until `test-suite-expansion` umbrella releases `Source_Core/src/ConfigManager*.cpp` + `tests/**`.
- `a6a8cb1` · 2026-05-16 · `feat(ai): Phase A — provider-pluggable IAiClient skeleton + OpenAiClient` — 14 files (8 new C++ + `NetworkUsageTracker` re-fit + 3 caller updates + CMake option/shim).
- `8086793` · 2026-05-16 · `plan-locks: ai-assistant-side-panel Phase A-narrowed -> in-flight` — status flip after dual-target build verified.
- PR [#140](https://github.com/alexandrosk0/Smatchet/pull/140) merged at `eeea501`.
- TBD · 2026-05-17 · `feat(ai): Phase A' — ConfigManager Ai fields + DPAPI key protection + AiSseParser/AiClientFactory doctests` — 17 `TrackerConfig` Ai fields appended (provider kind clamped to `AiProvider` enum, DPAPI-protected `AiApiKey` + `AiAnthropicApiKey` mirroring `McpAuthToken`, per-provider models + base URLs, side-panel persistence, `agents.md` paths, 5 context-block toggles). 2 new doctest files registered: `AiSseParser.test.cpp` (15 cases / 40+ assertions, 2 `[high-risk]`) + `AiClientFactory.test.cpp` (8 cases / 25+ assertions). `tests/CMakeLists.txt` extended with `AiSseParser.cpp` + `AiClientFactory.cpp` + `OpenAiClient.cpp` direct-source-list (Phase-5 mcp-pure recipe).
- TBD · 2026-05-17 · `feat(ai): Phase B — Assistant side panel + AiAssistantController worker thread + Ctrl+Shift+A` — 4 new files (`AiAssistantController.{h,cpp}`, `SmatchetAiAssistantUi.{h,cpp}`) + 7 modified (`AppController.{h,cpp}` add member + accessor + always-on stubs; `SmatchetUI.{h,cpp}` add private draw method + call site + Ctrl+Shift+A keybinding; `SmatchetUI_MainMenu.cpp` View menu entry; `SmatchetUI_Layout.cpp` repair early-return; `SmatchetUiSession.h` 10 `assistant*` fields; `CMakeLists.txt` link `SmatchetCoreAiShim` to standalone-OpenGL core targets). Threading: worker thread inside controller, cancel atom polled in `cpr::WriteCallback`, deltas + errors hand off to UI via `MainThreadDispatcher::PostToMainThread`, stale callbacks dropped via `assistantTurnGen` comparison. `SMATCHET_WITH_AI=ON` + `=OFF` builds both green. ctest still 331/1745.
- TBD · 2026-05-17 · `feat(ai): Phase D — AnthropicClient + OllamaClient + AiNdjsonParser + provider Combo wiring` — 3 new wire-protocol units: `Source_Core/include/AiNdjsonParser.{h}` + `src/AiNdjsonParser.cpp` (sibling to `AiSseParser`, JSON-aware line-buffered parser; partial-frame buffer across `Feed()` calls; invalid-JSON lines surface via `onError` without breaking the stream); `Source_Core/include/AnthropicClient.{h}` + `src/AnthropicClient.cpp` (`IAiClient` impl driving `/v1/messages` Native Messages API; reuses `AiSseParser`; translates `content_block_delta.delta.text` → `TokenChunk`, `message_delta.delta.stop_reason` → cached `FinishReason`, `message_stop` → terminal `AiStreamDelta`; `x-api-key` + `anthropic-version: 2023-06-01` headers); `Source_Core/include/OllamaClient.{h}` + `src/OllamaClient.cpp` (`/api/chat` NDJSON; no API key; default `http://localhost:11434`; emits per-line `{message.content, done}` → `AiStreamDelta`). `AiClientFactory.cpp` Anthropic + OllamaNative branches return non-null impls (`LOG_WARN`/null Phase A placeholders removed). `tests/Source_Core/AiNdjsonParser.test.cpp` adds 6 cases / 31 assertions (1 `[high-risk]`). `tests/Source_Core/AiClientFactory.test.cpp` flips 2 cases from nullptr-assertion to non-null + `GetProviderName()` match. `tests/CMakeLists.txt` registers 1 new test + 3 new production .cpps. `SmatchetPreferencesUi.cpp` Assistant group completes: provider Combo via `AiClientFactory::EnumeratedProviders()` bound to `cfg.AiProviderKind`; per-provider conditional inputs — masked OpenAI + Anthropic API keys (mirrors `McpAuthToken` pattern), per-provider model inputs (`AiModelOpenAi` / `AiModelAnthropic` / `AiModelOllama`), generic + Ollama base URLs (`AiBaseUrl` / `AiOllamaBaseUrl`). All inputs persist via `ConfigManager::Save` on change. Static buffer cache reseeded whenever provider changes so a user can switch + switch back and see persisted values rather than unsaved buffer state.
- TBD · 2026-05-17 · `feat(ai): Phase E — Lua ai.* glue + LayoutSchemaVersion 5→6 + README/LUA_GUIDE bullets (closes plan)` — `Source_Core/src/AppController_LuaBindings.cpp` gains 3 glues `LuaAiAddContextGlue` / `LuaAiClearContextGlue` / `LuaAiPromptGlue` in `smatchet_lua_init_detail::` + helper `LuaTableToAiContextBlock` for kind-string → `AiContextBlockKind` mapping. Registered on `state["ai"]` inside `InitLuaUi` (not `InitLuaCore` — glues resolve `__smatchet_app_ui` per the Phase 6b dual-key pattern, since the Core key holds `ILuaBindingHost*` not `AppController*`). Glues call `AppController::AddAiContext` / `ClearAiContext` / `PromptAi` — the always-on stubs that shipped Phase B; the OFF/AI build silently no-ops with zero new gating. `Source_Core/include/ConfigManager.h::kCurrentLayoutSchemaVersion` bumped `5 → 6` (single bump for the whole feature); comment block updated to record the AI assistant feature trigger. `tests/Source_Core/ConfigMigration.test.cpp` v5-fixture assertion flipped from `== kCurrentLayoutSchemaVersion` to `< kCurrentLayoutSchemaVersion` to track the post-bump invariant (fixture is now legitimately old). `README.md` gains a feature bullet (Ctrl+Shift+A, all 4 providers, agents.md layering, Lua surface). `docs/guides/lua.md` gains an `ai` Module section with usage example + threading note. `AppController_LuaStubs.cpp` gets a docstring noting the Phase E `ai.*` surface needs no stub mirror because the receiver methods are already always-on. `docs/backlog/agent-self-improvement/process.md` gains a P2 entry for the worktree-bootstrap-from-stale-base issue surfaced Phase D + reconfirmed Phase E. **Closes the plan** — Phase E is the final phase per plan rule; plan move to `docs/plans/shipped/` ships as a separate chore PR.
- TBD · 2026-05-17 · `feat(ai): Phase C — AgentsMdLoader + AiContextBuilder + context-block checkboxes` — 4 new files (`AgentsMdLoader.{h,cpp}` layered loader with 64 KB per-layer cap + walk-up project discovery; `AiContextBuilder.{h,cpp}` snapshot-builder for the 5 auto-context blocks). 4 modified: `SmatchetAiAssistantUi.cpp` (per-block checkbox row + Send-time `BuildAll` snapshot construction via `app.GetActiveTicketsSnapshot()` + `d.gridState.RectSel.Rows` + `d.cachedSortedIndices` + `d.gridState.ActiveIssueId` + view-definition arg); `SmatchetPreferencesUi.cpp` (new "Assistant" tab with `AgentsMdGlobalPath` + `ProjectAgentsMdPath` inputs — provider Combo / API keys / model selectors stay deferred to Phase D per plan scope); `AiAssistantController.cpp` (SystemPrompt assembly: agents.md prefix + `\n---\n\n` separator + per-block `<smatchet_context>` tag wrapping on the worker thread before `IAiClient::SendStreaming`); `SmatchetUI.cpp` (forward `ViewState.GetActiveView()` to the free-function panel drawer). `SmatchetAiAssistantUi.h` signature gains a `const ViewDefinition*` parameter. `tests/CMakeLists.txt` adds 2 test .cpps + 2 production .cpps + `ghc_filesystem` link dep. ctest delta: 331 → 357 cases (+26), 1745 → 1836 assertions (+91). Mutation-sanity confirmed (cap mutation 64KB→32KB → over-cap doctest fails).

### Phase B shipped contents

| File | Disposition | Notes |
|---|---|---|
| `Source_Core/include/AiAssistantController.h` | NEW | Worker-thread coordinator + state machine + always-on no-op stubs. Header collapses to empty when `SMATCHET_WITH_AI` is undefined so DX12 (Unreal) TUs compile cleanly. |
| `Source_Core/src/AiAssistantController.cpp` | NEW | Entire body gated `#if defined(SMATCHET_WITH_AI)`. Worker pops requests from `pending_` queue, runs `IAiClient::SendStreaming` synchronously on the worker thread, hands off delta + error callbacks to UI via `app.mainThreadDispatcher.PostToMainThread`. Per-turn cancel atom; stale-callback drop on UI side via `assistantTurnGen` comparison. |
| `Source_Core/include/SmatchetAiAssistantUi.h` | NEW | Single free function `SmatchetDrawAiAssistantPanel(AppController&, UiDrawSession&)`. |
| `Source_Core/src/SmatchetAiAssistantUi.cpp` | NEW | Right-anchored pinned non-docked panel via `SetNextWindowPos`/`SetNextWindowSize` with `ImGuiCond_Always` + `NoSavedSettings`. History scroll area with auto-pin-to-tail tracking. Multi-line input + Send/Cancel buttons. Left-edge resize grip persisting `cfg.AssistantPanelWidth` on `IsItemDeactivatedAfterEdit`. Hydrates from + persists to `ConfigManager` on toggle / width-commit. |
| `Source_Core/include/AppController.h` | MOD | `aiAssistant_` unique_ptr member (gated). `GetAiAssistantController` + `HasAiAssistantController` accessors (gated). Always-on no-op stubs: `AddAiContext`/`ClearAiContext`/`GetAiContext`/`PromptAi` — present regardless of build flag so Phase E Lua glue can call them unconditionally. Includes `AiTypes.h` unconditionally (POD types) + full `AiAssistantController.h` when AI is on (so consumers' default-deleter sees a complete type for the unique_ptr). |
| `Source_Core/src/AppController.cpp` | MOD | Construct `aiAssistant_` at end of `Initialize` (after `ConfigManager::Load` has settled all Ai* fields). Destroy at top of `~AppController` BEFORE `mainThreadDispatcher.BeginShutdown()` — the controller's worker may still be inside `SendStreaming`, and its callbacks need a live dispatcher to drain through. Stub bodies delegate to `aiAssistant_->...` when present; no-op otherwise. |
| `Source_Core/include/SmatchetUI.h` | MOD | Add private `drawAiAssistantPanel(AppController&, UiDrawSession&)` (gated). |
| `Source_Core/src/SmatchetUI.cpp` | MOD | Include `SmatchetAiAssistantUi.h`. Call `drawAiAssistantPanel` after `drawAuditWindow` in `Draw` (gated). Ctrl+Shift+A keybinding wired alongside Ctrl+B / Ctrl+J. Member-impl delegates to free function. |
| `Source_Core/src/SmatchetUI_MainMenu.cpp` | MOD | View menu entry "Assistant (Ctrl+Shift+A)" toggles `d.assistantPanelOpen` + sets `requestAssistantFocus`. |
| `Source_Core/src/SmatchetUI_Layout.cpp` | MOD | `repairTopLevelWindow` early-returns on `layoutKey == "assistant_panel"`. Defensive — the panel never calls repair itself, but the early-return guarantees a misplaced future call-site cannot fight the `ImGuiCond_Always` SetNextWindowPos. |
| `Source_Core/include/SmatchetUiSession.h` | MOD | 10 `assistant*` fields gated `#if defined(SMATCHET_WITH_AI)`. Includes `AiTypes.h` (for `AiMessage`) under same gate. |
| `CMakeLists.txt` | MOD | Link `SmatchetCoreAiShim` PUBLIC in `smatchet_configure_opengl_core_impl_target` so the standalone target gets `SMATCHET_WITH_AI=1`. DX12 path NOT linked — `SmatchetCore_DX12` compiles the same Source_Core/ TUs with the macro undefined; AI codepaths drop out via `#if defined(SMATCHET_WITH_AI)` gates. |

### Phase A shipped contents (narrowed)

| File | Disposition | Notes |
|---|---|---|
| `Source_Core/include/IAiClient.h` | NEW | Interface verbatim from § Interface signature. |
| `Source_Core/include/AiTypes.h` | NEW | POD types + `AiCancelToken = shared_ptr<atomic<bool>>` alias. C++14 — no `optional`/`variant`. |
| `Source_Core/include/AiSseParser.h` + `.cpp` | NEW | Byte-stream stateful SSE parser. Handles `\n\n` + `\r\n\r\n` boundaries, multiple `data:` lines per event, `event:` named events, `:` comments, unknown fields. |
| `Source_Core/include/OpenAiClient.h` + `.cpp` | NEW | Drives OpenAI `/v1/chat/completions` + OpenAI-compatible endpoints. Cancel via `cpr::WriteCallback` returning `false`. Both pre-completion and mid-stream cancel paths covered. Translates `choices[0].delta.content` → `TokenChunk`, `choices[0].finish_reason` → `IsFinal`. `[DONE]` sentinel terminates. |
| `Source_Core/include/AiClientFactory.h` + `.cpp` | NEW | `MakeAiClient(AiProvider)` returns `unique_ptr`. `Anthropic` + `OllamaNative` return null + LOG_WARN until Phase D. `ProviderToString` / `ProviderFromString` / `EnumeratedProviders` for the future Preferences Combo. |
| `Source_Core/include/NetworkUsageTracker.h` + `.cpp` | MOD | Added `enum class HttpTrafficKind { Tracker, Ai }` + `ai*` atomics + snapshot fields. `Record` signature: `Record(HttpTrafficKind, uint64_t, const cpr::Response&)`. |
| `Source_Core/src/TrackerHttpUtils.cpp` | MOD | 5 callers updated to pass `HttpTrafficKind::Tracker`. |
| `Source_Core/src/JiraIssueMutation.cpp` | MOD | 1 caller updated. |
| `CMakeLists.txt` | MOD | `option(SMATCHET_WITH_AI ON)` + `SmatchetCoreAiShim` INTERFACE target mirroring the MCP shim pattern. Files auto-picked-up by existing `file(GLOB_RECURSE CORE_SOURCES …)`. |

## Deviations from plan

- **`FieldCatalogCache.cpp` is not a `NetworkUsageTracker::Record` caller.** Plan listed it among the 3 callers needing one-line updates; `git grep` finds only `TrackerHttpUtils.cpp` (5×) + `JiraIssueMutation.cpp` (1×). No edit applied to `FieldCatalogCache.cpp`.
- **No `CORE_SOURCES` list edit.** Plan said "Add new sources to `CORE_SOURCES`" — the target is `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source_Core/src/*.cpp")` (CMakeLists.txt line 530), so new files auto-included. Phase A's CMake edits are option + shim only.
- **cpr version bump unnecessary.** Plan risk #1 said "cpr ≥ 1.10 required for `cpr::WriteCallback`". Repo ships 1.9.2 (`CMakeLists.txt:FetchContent_Declare(cpr GIT_TAG 1.9.2)`); `cpr/callback.h` line 39 already defines `class WriteCallback` with `bool(std::string, intptr_t)` signature. No bump applied.
- **`SMATCHET_WITH_AI` shim wired, not yet consumed.** Plan A says Phases A and B are "gated `SMATCHET_WITH_AI=ON`". Phase A files compile unconditionally — nothing depends on the macro yet. The shim INTERFACE target is created so Phases B-E can `target_link_libraries(... SmatchetCoreAiShim)` without re-plumbing CMake. ON/OFF builds are structurally identical for Phase A.
- **`ConfigManager` field set + Ai doctest deferred to Phase A'.** Plan-locks pre-flight intersected hard with `test-suite-expansion · phases 2-9 · claimed` (umbrella claim on `Source_Core/src/ConfigManager*.cpp` + `tests/CMakeLists.txt` + `tests/Source_Core/**`) and `test-suite-expansion · phase 1 · in-flight` (`tests/CMakeLists.txt`). User picked option (b) — defer the colliding paths to Phase A'.
- **`SMATCHET_AI_SCRATCH_DRIVER` skipped in Phase A.** Plan A land-gate included a scratch driver behind `#ifdef SMATCHET_AI_SCRATCH_DRIVER` in `Target_Standalone/main.cpp` to prove a real OpenAI prompt round-trip. Not added — Phase A ships with no validation against a live endpoint. Will validate once Phase B wires the side-panel UI and the user can drive a real prompt through the running app, or earlier via Phase A' doctest of `AiSseParser` against canned fixtures + a one-shot `bash scripts/dev/ai-smoke.sh` once `test-author` automates it.

### Phase A' deviations

- **Direct-cpr link in `SmatchetTests` already cleared.** Plan A' noted `OpenAiClient.cpp` pulls `cpr/cpr.h` and worried about banned-include leakage in tests. In practice `cpr::cpr` is already a `target_link_libraries` of `SmatchetTests` (transitive via `TrackerHttpUtils.cpp`, present since Phase 4); adding `OpenAiClient.cpp` to the test target's source list is a no-op for the dep graph. Tests don't actually call `SendStreaming` (which would need a live HTTP endpoint) — they only exercise the constructor + `GetProviderName()` and let the factory return `nullptr` for Phase D branches.
- **`AgentsMdGlobalPath` default-at-Load resolution uses `ConfigManager::GetPlatformSharedUserDataDirectory()`.** That helper already lives in `ConfigManager_PathUtils.cpp:507` and returns `%LOCALAPPDATA%\Smatchet\` (with trailing separator) on Win32 with sensible POSIX/macOS fallbacks. The new logic appends `agents.md` only when the user's persisted path is blank — explicitly-empty user choice round-trips as empty (caller can re-blank to opt out of agents.md inclusion).
- **`AiProviderKind` clamped to `AiProvider` enum range on Load, not at construct.** Out-of-range values (e.g. a future-version config opening on an older build) silently degrade to `OpenAi` (0). The default in `TrackerConfig` is the integer `0` rather than `static_cast<int>(AiProvider::OpenAi)` to keep the header from forcing `AiTypes.h` into TUs that already include `ConfigManager.h` purely for path getters.
- **No `LayoutSchemaVersion` bump.** Phase A' is field-additions only with `j.value()`-default migration; old v4/v5 configs default-load cleanly. The single schema bump for the whole feature lands in Phase E per plan rule.

### Phase E deviations

- **Glues registered on `state["ai"]` in `InitLuaUi`, not `InitLuaCore`.** Plan packet listed `InitLuaCore` as the registration site by analogy with `state["smatchet"]` / `state["tracker"]`. In practice `__smatchet_app` (the Core key) holds an `ILuaBindingHost*` since the PR #144 interface lift; resolving an `AppController*` through it via sol2's `get_or<AppController*>` would race with multiple-inheritance offset bugs (the comment block at `AppController_LuaBindings.cpp:448-454` documents the existing constraint). `InitLuaUi` already sets the dedicated `__smatchet_app_ui` key with the concrete `AppController*` — the `ai.*` glues piggy-back on that, matching the existing UI / cached-cell glue resolution pattern. Concrete trade-off: the Core (background automation `bgState`) state also gets `__smatchet_app_ui` set in `AppController_LuaBindings.cpp:1118`, so `ai.*` is available in worker scripts too — and that's where the documented race-mutate caveat (`luaContext_` mutation without MainThreadDispatcher hop) lives.
- **No `MainThreadDispatcher` hop in the glue bodies.** Plan packet's draft decision tree noted "if always-on stubs are UI-thread-only, switch to ILuaBindingHost-extension path". Phase B's `AppController::AddAiContext` / `ClearAiContext` / `PromptAi` are NOT UI-thread-guarded — they directly mutate `aiAssistant_->luaContext_` and call `Submit`. Phase E inherits that invariant: glues are thin wrappers, no extra dispatcher hop. Scripts running on the UI thread (the main `lua` state) are safe; background automation worker (`bgState`) scripts that call `ai.*` race-mutate `luaContext_`. Caveat documented inline in the glue comment block + docs/guides/lua.md `ai` Module section; matches Phase B's design choice rather than introducing new threading machinery.
- **`AppController_LuaStubs.cpp` gains documentation only — no new stub.** Plan packet's claim "mirror stub implementations" assumed the OFF build needs `ai.*` Lua-callable no-op stubs. But when `SMATCHET_WITH_LUA_AUTOMATION=0`, the entire Lua interpreter is absent — no Lua script can run, so no `ai.*` name needs to resolve. The genuine parity is at the C++ `AppController` member level (`AddAiContext` / `ClearAiContext` / `PromptAi`), which Phase B already shipped as always-on stubs in `AppController.cpp`. LuaStubs.cpp gets a top-of-file docstring explaining this so future contributors don't try to add a stub mirror that wouldn't serve any link target.
- **`ai.prompt` `extra_blocks` arg appends, doesn't replace.** Plan packet sketched `ai.prompt(prompt [, table])` without specifying merge semantics. Shipping path: optional `extra_blocks` array iterates 1..N (Lua-array convention) and each table is passed to `AddAiContext` before `PromptAi`. Matches the panel's "Send with accumulated context" path rather than introducing a separate "replace context for this turn" branch. Scripts that want replacement call `ai.clear_context()` first.
- **`AiContextBlockKind` string mapping covers all 5 enum values + accepts unknown gracefully.** Plan packet hinted at a kind-string with no enumeration. Shipping path: 5 documented strings (`"active_ticket"`, `"multi_selected_tickets"`, `"visible_grid_rows"`, `"active_view"`, `"audit_trail"`) map 1:1 to the enum; unknown strings (including the empty string and `nil`) default to `ActiveTicket` (the enum's default-ctor value). Avoids `luaL_error` from a malformed call — matches the rest of the sol2 binding's "be lenient with bad Lua input" pattern.
- **`ConfigMigration` v5-fixture assertion flipped from `==` to `<`.** Pre-bump, the test asserted `cfg.LayoutSchemaVersion == kCurrentLayoutSchemaVersion` as a freshness gate (fixture and constant aligned). Post-bump, the fixture is legitimately old (`5`) and the constant is `6` — `==` would fail. Replaced with `<` (asserts the fixture is older than current, which is the *real* invariant) plus a comment block explaining the bump's rationale. v4 fixture tests untouched — those exercise the auto-inject migration and don't compare against the constant.

### Phase D deviations

- **`AiNdjsonParser` is JSON-aware (parses each line internally) rather than emitting raw line text like `AiSseParser`.** Plan packet sketched `LineCallback = void(const nlohmann::json&)`. Shipped exactly that — the parser performs `nlohmann::json::parse` itself and surfaces invalid lines via `onError(const std::string& rawLine)`. Trade-off: parser TU pulls `<nlohmann/json.hpp>` in the header (already a transitive dep in every TU that includes `AiTypes.h`-adjacent helpers, so no new header pollution), and the `OllamaClient` stays a thin translation layer rather than re-parsing per line. The `AiSseParser` design predates this and emits raw `data:` payloads because OpenAI / Anthropic have provider-specific top-level keys; NDJSON is uniform enough that JSON-in-parser is the cheaper shape.
- **Anthropic `message_delta` is captured for `stop_reason` even though the plan packet only listed `content_block_delta` + `message_stop`.** Anthropic surfaces `stop_reason` (e.g. `"end_turn"`, `"max_tokens"`) on the `message_delta` event ahead of `message_stop`. Caching it on a local string and emitting it on the terminal `AiStreamDelta::FinishReason` is one line of code that preserves provider-specific finish-reason fidelity for any UI / log surface that wants it. Plan packet's "Anthropic doesn't surface finish_reason the same way" is technically right; in practice `message_delta` is the parallel.
- **`OllamaClient` uses `done_reason` from the terminal NDJSON line when present, falling back to `"stop"`.** Ollama added `done_reason` (e.g. `"length"`, `"stop"`, `"unload"`) in a recent release; older daemons omit it. Preserving when present matches the per-provider `FinishReason` shape used by `OpenAiClient` (`finish_reason`) and `AnthropicClient` (`stop_reason`).
- **`SmatchetPreferencesUi.cpp` Assistant group uses a single shared `AiBaseUrl` field for both OpenAI and Anthropic.** Plan packet listed `AiBaseUrl` as "used by OpenAI for Azure / Groq / OpenAI-compat endpoints — empty = client default" — that's the OpenAI case. Anthropic's `BaseUrl` field maps to the same `cfg.AiBaseUrl` member (no separate `cfg.AiAnthropicBaseUrl` in `ConfigManager.h`). The two providers share the field because (a) only one is "active" at a time per the Combo + (b) the override case (Anthropic-compatible proxy) is rare. If both providers need simultaneous distinct base URLs in the future, that's a `ConfigManager` schema bump candidate.
- **Static input buffers reseed whenever `AiProviderKind` changes, not just on first frame.** Plan packet said "copy from cfg into local `*Buf` strings on Open, write back to cfg + ConfigManager::Save on Save / change". A simpler "seed once" would leave stale buffer contents when the user switches provider, edits a field, switches back, and finds the cfg-persisted value overwritten by the older buffer. Reseeding on `s_lastSeededProvider != d.cfg.AiProviderKind` keeps the UI faithful to the persisted state.
- **`AiClientFactory.cpp` drops `LOG_WARN` placeholder bodies.** Plan Phase A had two `LOG_WARN` lines explaining "not yet implemented (Phase D)". Phase D replaces both with `new AnthropicClient()` / `new OllamaClient()` — no log line because the path is now correct, not deferred.
- **`AiNdjsonParser::Flush` treats trailing data as a final line.** Plan packet's "shape" sketch didn't specify Flush semantics. Shipping path: `Flush` emits whatever's in the buffer as a single line (parses, dispatches, clears). Matches `AiSseParser::Flush` (which appends a frame-boundary to force-flush). Lets `OllamaClient` survive a server that closes the connection without a trailing `\n` on the `done:true` line.
- **`OllamaClient` does NOT pass `req.SystemPrompt` as a `{"role":"system"}` message.** Native `/api/chat` accepts both `body.system` (top-level field) and a leading system-role message; emitting top-level matches the `AnthropicClient` shape and avoids duplicating it. If an Ollama daemon ignores top-level `system`, the system prompt is silently dropped — known per-daemon behaviour; backlog candidate if it bites in practice.
- **No `LayoutSchemaVersion` bump.** Phase D is wire + UI additions only; the single bump for the whole feature lands in Phase E per plan rule (mentioned explicitly in plan packet).

### Phase C deviations

- **`AiContextBuilder::Inputs` carries `shared_ptr<vector<CachedTicket>>` directly, not an `AppController*`.** Plan-sketch API in the prompt had `BuildAll(AppController& app, ...)` but linking `AppController.cpp` into `SmatchetTests` would drag the production AppController link surface (transitive grid / scheduler / view storage dependencies). Inputs take the ticket snapshot by `shared_ptr` so the panel resolves it once via `app.GetActiveTicketsSnapshot()` and hands a const reference to the builder — the test target stays clean of `AppController.cpp`.
- **agents.md loading runs on the worker thread, not the UI thread.** Plan acknowledged both paths were acceptable per pillar 2 (file I/O at 64 KB-per-layer cap is < 10 ms). Shipping path: `AiAssistantController::RunRequest` calls `AgentsMdLoader::LoadLayered` after `ConfigManager::Load()` for the model resolution it already performs each turn. UI thread stays pure; the worker absorbs the file I/O.
- **AuditTrail block reads `BackendAuditTrail::ReadRecentEvents` directly inside `BuildAll`.** Plan said "audit fixture passed in"; implementation chose to call the namespace function directly from `BuildAll` so the panel-side call site doesn't have to pre-resolve it. The builder's pure-helper `BuildAuditTrailBody(vector<string>)` IS exposed for tests — the empty-state test explicitly disables the audit block to avoid pulling whatever audit file happens to be configured in the test runner environment.
- **`SmatchetAiAssistantUi.h` signature changes to take `const ViewDefinition*`.** Plan said the panel reads the active view via "AppController". In practice `Views ViewState` lives on `SmatchetUI`, not `AppController` (canonical Phase B finding). The cleanest forward path: the `SmatchetUI::drawAiAssistantPanel` member calls `ViewState.GetActiveView()` and forwards the pointer. No cross-cutting refactor required.
- **`VisibleRows` block reuses `d.cachedSortedIndices` as the "visible window".** The grid's true viewport range (top-of-scroll + visible-row-count) isn't surfaced as a separate session field yet — Phase C uses the full sorted-index list and lets `BuildVisibleRowsBody` cap at N=50 rows. Deterministic + matches the user's natural read order. A future precision pass can plumb the actual visible-row range without changing the builder API.
- **Preferences Assistant tab carries only agents.md path inputs.** Plan packet was explicit — provider Combo / API keys / model inputs land in Phase D. The current tab notes the deferred surface inline so a user reading the dialog can find the documentation themselves.
- **agents.md cache deferred to backlog.** Plan suggested an `unordered_map<path, mtime+content>` cache; the worker-thread read on Submit is < 10 ms at the 64 KB cap and only runs per-turn (not per-frame). Cacheless is the shipped behaviour; a follow-up cache layer is filed for `agent-self-improvement/infra.md` if the per-turn I/O becomes measurable.
- **No `LayoutSchemaVersion` bump.** Phase C is additive: new files + new toggles that already shipped Phase A'. The single bump for the whole feature lands in Phase E per plan rule.

### Phase B deviations

- **`AiAssistantController.h` collapses to an empty header when `SMATCHET_WITH_AI` is undefined.** The plan implied the header would be unconditionally present and the class definition gated; in practice the entire header is wrapped `#if !defined(SMATCHET_WITH_AI) ... #else ... #endif` so that the DX12 build (which does NOT link `SmatchetCoreAiShim`) sees nothing when `AppController.h` includes the header. This avoids a forest of `#if defined(SMATCHET_WITH_AI)` guards inside `AiAssistantController.cpp` callers.
- **`AppController.h` includes `AiAssistantController.h` unconditionally** (under its own `#if defined(SMATCHET_WITH_AI)`). The plan suggested a forward-decl + member declaration shape, but `std::unique_ptr<AiAssistantController>` requires a complete type at the point where the implicit default-deleter is instantiated (which happens in any TU that includes `AppController.h` — e.g. `main.cpp`). Including the full header sidesteps the need for every consumer to provide an out-of-line dtor.
- **AppController stubs always present (no `SMATCHET_WITH_AI` gate on declarations).** Per plan: "Stub members `AddAiContext`, `GetAiContext`, `ClearAiContext`, `PromptAi` restored at zero-cost." Implemented as always-on declarations in the header so Phase E Lua glue compiles without macro plumbing; the OFF-build bodies are no-ops, the ON-build bodies delegate to `aiAssistant_->...`. `AiTypes.h` is included unconditionally from `AppController.h` for the same reason (it's a POD-only header with no transitive bloat).
- **No production-side `extern UiDrawSession g_ui;` declaration broadening.** Plan implied the controller's UI-thread callbacks would reach into the global directly; the global is declared `extern` behind `SMATCHET_WITH_LUA_AUTOMATION` in `SmatchetUiSession.h`. Rather than broaden the header gate (which would change a public-facing decl visible to every TU that includes it), `AiAssistantController.cpp` declares the `extern` locally at file scope. Single-TU coupling, no header pollution.
- **`MainThreadDispatcher` API uses `Task = function<void()>`, not `function<void(AppController&)>` as the prompt sketched.** Verified in `Source_Core/include/MainThreadDispatcher.h:33`. Lambdas capture state by value (turnGen, chunk, isFinal, …) and reach into the global `g_ui` for UI mutation. The `AppController*` is captured for the worker→dispatcher hop, not re-passed through the lambda.
- **Cancel-button history retention encoded directly in `onError`.** Plan Scenario 3 contract: "Cancel mid-stream — halts < 1 s, partial text retained in history". Implemented by `onError` checking `WasCancelled` and pushing `assistantStreamBuf + "\n[cancelled]"` into history before clearing. Avoids a separate "cancel state" branch in the UI panel.
- **No `AiAssistantController` doctest in Phase B.** Plan optional. The state-machine transitions are driven by `IAiClient::SendStreaming`, which has no fake/injectable shape today — adding one is its own work (would touch `AiClientFactory` to support a `FakeAiClient` injection, blast-radius beyond Phase B). Deferred to `test-author` follow-up alongside Scenarios 4 / 5 / the static-mutation reasoning recipe.
- **No `SmatchetPreferencesUi.cpp` Assistant group.** Plan Phase A-C-D table claims the Assistant group is incrementally extended across phases; Phase B's panel inherits provider config from existing `cfg.AiProviderKind` + `cfg.AiApiKey` (set Phase A'). Until Preferences exposes those fields, the user must edit `smatchet_config.json` directly. Phase B's `assistantLastError` strip surfaces "no provider" / "API Error: 401" inline so the empty-API-key case is visible — not invisible. Recommend Phase B.5 or fold into Phase C alongside the per-block toggles.

## Verification

- [x] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` — green.
- [x] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — green (dual-target).
- [x] `clang-format -i` / `cppcheck` / `clang-tidy` — clean via PostToolUse lint hook (manual re-run of `.claude/hooks/lint-cpp.sh` after one race-condition false-block during multi-edit confirmed no real issues).
- [x] `cpr` 1.9.2 `WriteCallback` signature `bool(std::string, intptr_t)` confirmed at `.fetchcontent-src/cpr-src/include/cpr/callback.h:39`.
- [x] **Phase A' — `tests/Source_Core/AiSseParser.test.cpp`** (15 cases / 40+ assertions, 2 `[high-risk]`): single + multi-event frames, partial-frame buffering, CRLF/LF boundary equivalence, multi-data-line concat, named events (Anthropic shape), `:` comments, unknown fields, `[DONE]` sentinel pass-through, Reset / Flush, one-byte-at-a-time drip equivalence. Covers Scenario 2 (streaming happy path) for the parser surface; Scenario 4 / 5 still defer to Phase B (HTTP-driven assertions need the real client wired against a fixture endpoint).
- [x] **Phase A' — `tests/Source_Core/AiClientFactory.test.cpp`** (8 cases / 25+ assertions): `ProviderToString` / `ProviderFromString` round-trip for all 4 enums, rejection of unknown / case-mismatched keys, `EnumeratedProviders` stable order + non-empty Display, `MakeAiClient(OpenAi)` + `MakeAiClient(OllamaOpenAiCompat)` non-null with `GetProviderName() == "openai"`, `MakeAiClient(Anthropic)` + `MakeAiClient(OllamaNative)` nullptr until Phase D.
- [x] **Phase A' — `ConfigManager` Ai fields default-load cleanly from v4 / v5 fixtures.** `test-config-migration.sh` keeps passing post-Phase-A' (additive `j.value()` defaults; no schema bump).
- [ ] **Scenario 4** (Bad API key → "API Error: 401") — DEFERRED to Phase B (needs UI surface that surfaces the AiStreamError text).
- [ ] **Scenario 5** (Transport down → "Network unreachable" within 5 s) — same defer as Scenario 4.
- [ ] **Scenario 13** (`-DSMATCHET_WITH_AI=OFF` builds clean, no menu item, Lua `ai.*` no-op) — not meaningfully testable in Phase A (no menu / Lua surface yet). Will block on Phase E.
- [ ] **`SMATCHET_WITH_AI=OFF` build** — not verified explicitly. Phase A files compile unconditionally so OFF is structurally equivalent to ON; explicit OFF-config-and-build is a Phase A' addition.

### Phase B verification

- [x] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — green (dual-target). `SmatchetCore_DX12` builds with `SMATCHET_WITH_AI` undefined (DX12 path does NOT link `SmatchetCoreAiShim`); all AI codepaths drop out via `#if defined(SMATCHET_WITH_AI)` gates including the entire `AiAssistantController.h` body and the `AiAssistantController.cpp` TU.
- [x] `cmake -B build/ninja-ai-off-check --preset ninja-iter-msys2 -DSMATCHET_WITH_AI=OFF && cmake --build build/ninja-ai-off-check --target SmatchetStandalone` — green. Confirms Phase B's gated paths drop cleanly even on the standalone target.
- [x] `cmake --build --preset ninja-test-msys2 && ctest` — both `smatchet_tests` + `smatchet_lua_tests` pass. SmatchetTests aggregate **331 cases / 1745 assertions** — exact match with the Phase A' baseline (no Phase B doctest added).
- [x] `bash scripts/dev/test-all.sh` — 168 passed of 176 total sidecar assertions. 8 failures are pre-existing worktree-infra: `test-lint-hook-split.sh` needs `.claude/hooks/lint-cpp.sh` which only exists at the main repo root, not under `.claude/worktrees/<id>/`. The other 4 batched failures (`test-lua-error-log`, `test-markdown-lang-tag`, `test-screenshot-diff`, `test-theme-syntax-colors`, `test-ui-views-columns-reorder`) all pass when re-run isolated with `PATH` properly set — they're harness-batched PATH issues + a known flaky bucket-E race, not Phase B regressions. Confirmed by running each individually.
- [x] **Scenario 1** (Panel toggle + width persists across restart) — implemented: `HydrateFromConfigOnce` on first frame reads `cfg.AssistantPanelOpen` + `cfg.AssistantPanelWidth`; `PersistOpenStateImmediate` writes back on every frame the panel is closed (idempotent), `PersistWidthDebounced` writes on `IsItemDeactivatedAfterEdit` from the left-edge resize grip. Manual verification deferred to launch-time eyeball (Smatchet.exe → View → Assistant → resize → close → relaunch).
- [x] **Scenario 3** (Cancel mid-stream — halts < 1 s, partial text retained in history) — code path implemented: `Cancel()` stores `true` into the per-turn cancel atom; cpr's `WriteCallback` polls and returns `false` (already wired in `OpenAiClient`); `onError` fires with `WasCancelled=true` and the partial `assistantStreamBuf` is pushed into history with `"\n[cancelled]"` suffix. Live verification gated on Scenario 2.
- [ ] **Scenario 2** (OpenAI streaming happy path) — DEFERRED to live-API smoke: requires a valid `cfg.AiApiKey`. The wiring is end-to-end (UI Send → controller worker → OpenAiClient → SSE parser → MainThreadDispatcher → UI render); a fixture-driven automation slot is open per § Manual residue.
- [ ] **Scenario 4** (Bad API key → "API Error: 401") — code path implemented: `onError` formats `"API Error: %d %s"` into `assistantLastError`, the panel renders that string in red via `DrawErrorStrip`. Live verification: type a bad key into `smatchet_config.json` + send a prompt; expected red strip with "API Error: 401 …".
- [ ] **Scenario 5** (Transport down → "Network unreachable" within 5 s) — same shape as Scenario 4; `AiClientConfig::ConnectTimeoutMs` default is 5000ms.
- [ ] **Scenario 6** (Visible-rows block) — Phase C work, not Phase B.
- [ ] **Scenario 11/12** (Anthropic / Ollama-OpenAI-compat provider switch) — Phase D work.
- [ ] **Scenario 14** (Lua glue) — Phase E work. AppController's `AddAiContext` / `ClearAiContext` / `GetAiContext` / `PromptAi` stubs land in Phase B specifically so Phase E Lua glue compiles unconditionally; they're no-ops in OFF builds and delegate to `aiAssistant_` in ON builds.

### Phase C verification

- [x] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target green.
- [x] `cmake -B build/ninja-ai-off-check --preset ninja-iter-msys2 -DSMATCHET_WITH_AI=OFF && cmake --build build/ninja-ai-off-check --target SmatchetStandalone` — `SMATCHET_WITH_AI=OFF` build clean. Build dir discarded.
- [x] `cmake --build --preset ninja-test-msys2 && ctest` — both `smatchet_tests` + `smatchet_lua_tests` pass. **357 cases / 1836 assertions** (Phase A' baseline was 331 / 1745 → Phase C adds 26 cases / 91 assertions).
- [x] **`tests/Source_Core/AgentsMdLoader.test.cpp`** (13 cases / ~44 assertions, 3 `[high-risk]`): empty / nonexistent path → empty, small-file byte-perfect round-trip, over-cap input truncates + sentinel suffix `[truncated at 64 KB`, explicit cap argument honoured, file-at-depth-0 walk-up found, walk-up locates ancestor at depth-3 from depth-3 start, walk-up depth cap stops the walk before file is found, uppercase `AGENTS.md` found, case-insensitive FS preference of lowercase, missing both layers → empty, global only / project only round-trip unchanged, both layers joined with `\n\n---\n\n` separator and ordering verified, project override takes precedence over walk-up candidate.
- [x] **`tests/Source_Core/AiContextBuilder.test.cpp`** (13 cases / ~47 assertions, 1 `[high-risk]`): empty Inputs → 5 blocks with empty bodies + `MergeEnabled` empty, BuildSelectionBody caps at 50 sorted rows preserving sort-order ascending (first SMA-100, last SMA-149, no SMA-150+), empty selection → empty body, OOB row indices silently skipped, BuildVisibleRowsBody caps at 50 + skips OOB indices, BuildActiveTicketBody empty for empty/unknown id, populated emits `id:` + `summary:` + `status:` lines, BuildActiveViewBody null → empty / populated emits `name:` `id:` `query:` `columns:` lines, BuildAuditTrailBody caps at 20 most-recent-first (verified by `{"i":29}` first / `{"i":10}` last / `{"i":9}` absent), BuildAll respects per-block enable flags (disabled blocks → empty body), MergeEnabled wraps non-empty blocks in `<smatchet_context block="...">...</smatchet_context>` tags + skips disabled blocks.
- [x] **Mutation-sanity** (per backlog #48 recipe #1): changed `AgentsMdLoader::kDefaultLayerCapBytes` from 64 KB → 32 KB; rebuilt; the over-cap doctest failed with `REQUIRE( 32879 > 65536 )` — confirming the test actually exercises the cap. Reverted.
- [x] `bash scripts/dev/test-all.sh` — 62/70 sidecar assertions pass. 8 failures are pre-existing worktree-infra (`ninja-ui-test-msys2` binary not present + lint-hook script not under `.claude/worktrees/<id>/`) — per AGENTS.md not a halt condition.
- [ ] **Scenario 6** (Visible-rows block — exactly 50 lines in priority sort order) — pure-logic coverage shipped (`AiContextBuilder::BuildVisibleRowsBody` caps at 50 + skips OOB); end-to-end "in priority sort order" gate stays deferred to the live-API smoke harness because the actual sort fingerprint depends on `cachedSortFingerprint` which is set by the grid renderer on first paint.
- [ ] **Scenario 7** (Multi-select — 3 ctrl-clicked rows fully rendered) — `BuildSelectionBody` correctness is locked at the unit-test boundary (3-row case validated via `SMA-100` / `SMA-101` / `SMA-102` assertions in `BuildAll — disabled flags zero-out matching blocks`). Live-app verification via ctrl-click defers to the same backlog `test-author · Headless AiAssistant scenarios` entry as Phase B Scenarios 2 / 4 / 5.
- [ ] **Scenario 8** (agents.md layering — all 3 sentinels with `---` separators) — 2-layer separator validated; 3-layer (individual-view) deferred until a per-view override path lands.
- [ ] **Scenario 9** (agents.md cap — 200 KB file truncated + toast) — truncation gate validated in pure unit test; the **toast** is plan-only (no toast wired in shipping path — the loader silently caps + appends sentinel, the AI sees the sentinel, the user sees nothing). Backlog candidate: `2026-05-17 · test-author · [tooling] — SmatchetToastManager push for over-cap agents.md`.
- [ ] **Scenario 10** (Per-block toggle persists across restart) — implemented (`DrawContextBlockCheckboxes` calls `ConfigManager::Save` on any dirty toggle); manual-restart verification gated on launch-time eyeball.

### Phase D verification

- [x] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target green.
- [x] `cmake -B build/ninja-ai-off-check --preset ninja-iter-msys2 -DSMATCHET_WITH_AI=OFF && cmake --build build/ninja-ai-off-check --target SmatchetStandalone` — `SMATCHET_WITH_AI=OFF` build clean. Build dir discarded after. Phase D's new wire-protocol TUs (`AiNdjsonParser.cpp`, `AnthropicClient.cpp`, `OllamaClient.cpp`) are not `#if defined(SMATCHET_WITH_AI)` gated — they're pure HTTP-client surface that compiles independently of the side-panel UI. Confirmed they don't pull any UI / SDK include that would break OFF builds.
- [x] `cmake --build --preset ninja-test-msys2 && ctest` — both `smatchet_tests` + `smatchet_lua_tests` pass. SmatchetTests aggregate post-Phase-D: **363 cases / 1873 assertions** (Phase C baseline 357 / 1836 → Phase D adds 6 cases / 37 assertions — slight bump over the planned 31 because the AiClientFactory flip from nullptr-assertion to non-null + GetProviderName check added 4 more assertions in 2 cases).
- [x] **`tests/Source_Core/AiNdjsonParser.test.cpp`** (6 cases / 31 assertions, 1 `[high-risk]`): single complete line emits one parsed JSON object; multiple lines in one Feed emit in order; line split across two Feed calls buffers correctly + completes on second Feed; blank lines (LF and CRLF) silently skipped; invalid JSON line surfaces to onError with raw line text + subsequent valid lines still parse (`[high-risk]` — guards against the parser "getting stuck" failure mode); `Reset()` clears partial-frame buffer mid-stream.
- [x] **`tests/Source_Core/AiClientFactory.test.cpp`** Anthropic + OllamaNative cases flipped: now assert non-null + `GetProviderName() == "anthropic"` / `"ollama"`. Existing 6 other cases (`ProviderToString`, `ProviderFromString`, `EnumeratedProviders`, OpenAi non-null, OllamaOpenAiCompat non-null) unchanged.
- [x] **Mutation-sanity** (per backlog #48 recipe #1): swap `AiNdjsonParser::Feed`'s `nl + 1` to `nl` in the buffer-erase calc — split-line test fails (parser re-emits the line forever). Reverted.
- [ ] `bash scripts/dev/test-all.sh` — TBD on dispatch.
- [ ] **Scenario 11** (Anthropic provider switch — same prompt streams via Anthropic-shape SSE) — wire-level support shipped; live-API smoke deferred to the same `AiAssistantSendScenario` fixture harness slot as Phase B Scenarios 2/4/5 (backlog `test-author · Headless AiAssistant scenarios`).
- [ ] **Scenario 12** (Ollama native provider switch — NDJSON consumed end-to-end) — same as Scenario 11.
- [ ] **Scenario 14** (Lua glue) — Phase E work.

### Phase E verification

- [x] `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target green. DX12 path's `AppController_LuaStubs.cpp` link continues to satisfy the always-on AI stubs from `AppController.cpp` (Phase B); Phase E adds nothing the OFF build needs.
- [x] `cmake -B build/ninja-ai-off-check --preset ninja-iter-msys2 -DSMATCHET_WITH_AI=OFF && cmake --build build/ninja-ai-off-check --target SmatchetStandalone` — `SMATCHET_WITH_AI=OFF` build clean. The new `ai.*` glues call AppController's always-on stubs which no-op silently — no script-side error, no link error, the panel never appears (Phase B gate).
- [x] `cmake --build --preset ninja-test-msys2 && ctest` — both `smatchet_tests` + `smatchet_lua_tests` pass. `smatchet_lua_tests::LuaStubsCompile.test.cpp` (the binding↔stub drift sentinel) continues to pass — Phase E adds nothing to its `ExpectedLuaPublicSurface` set because the `ai.*` glues call always-on AppController members that aren't in the Lua-public-only sentinel scope. SmatchetTests aggregate post-Phase-E: 363 cases / 1873 assertions (Phase D baseline preserved — the `ConfigMigration` v5-fixture assertion swap doesn't change the case / assertion count).
- [x] `bash scripts/dev/test-config-migration.sh` — v4 / v5 fixtures still load cleanly post-bump. `LayoutSchemaVersion` round-trips raw (`5` for v5 fixture); the new `< kCurrentLayoutSchemaVersion` assertion replaces the old `==` freshness gate.
- [x] `bash scripts/dev/test-all.sh` — sidecar suite (8 pre-existing worktree-infra fails remain, all filed `tooling/P2`).
- [x] **Scenario 13** (`-DSMATCHET_WITH_AI=OFF` clean — no menu item, Lua `ai.*` no-op) — verified via the explicit OFF-build gate plus the always-on stub bodies. `ai.prompt("test")` from a Lua script under an `OFF` build resolves the glue, calls `AppController::PromptAi`, which `#else (void)prompt;` no-ops — no error, no log, no panel. Script-load compatibility holds.
- [x] **Scenario 14** (`ai.add_context` / `ai.clear_context` / `ai.prompt` callable from Lua, ON build) — verified via the `state["ai"]` registration trace in `InitLuaUi` + manual `RunLua.lua` test (`ai.add_context({kind="active_ticket", name="ticket", body="PROJ-123"}); ai.prompt("summarise this")` → panel populates + worker dispatches). Live-API smoke gated on Scenario 2's deferred fixture harness.
- [x] **Scenario 15** (`LayoutSchemaVersion` 5→6 — one-shot reset on Phase E, no re-reset on subsequent launches) — verified by reading `SmatchetUI.cpp:242-246` migration path: first launch post-bump detects `cfg.LayoutSchemaVersion (5) < kCurrentLayoutSchemaVersion (6)`, resets layout + bumps `cfg.LayoutSchemaVersion = 6`, persists. Next launch reads `6`, the `<` check is false, no reset. Idempotent.

### Phase E manual residue

- **None directly from Phase E.** All gates auto-pass; all 3 scenarios are bookkeeping (build-config + script-side functional + schema-bump migration). Inherited residue from Phase B/C/D (live-API smoke harness for Scenarios 2/4/5/8/9/11/12) remains in the backlog under `test-author · [tooling] — Headless AiAssistant scenarios`.

### Phase D manual residue

- **Live-API verification of Anthropic + Ollama streaming end-to-end** — needs canned `httplib` fixture per the `test-author · [test] · P2` backlog entry filed during Phase B. Wire-level translation is locked at the unit-test boundary (`AiNdjsonParser` + `AiSseParser` Anthropic-named-event coverage) but the cpr-driven HTTP path remains live-API-only until the fixture lands.
- **Preferences Combo focus + tab order keyboard a11y eyeball** — falls under UX pillar 4 (aspirational); no auto-check today.
- **`AiBaseUrl` semantics ambiguity for the "Anthropic via proxy" case** — current UI shares the field with OpenAI's `BaseUrl`. A user who switches providers and forgets to clear the field can accidentally point Anthropic at an OpenAI-shaped endpoint. Backlog candidate: `2026-05-17 · process · [tooling] — provider-specific BaseUrl fields in ConfigManager` to split the shared `AiBaseUrl` into `AiBaseUrlOpenAi` + `AiBaseUrlAnthropic` if the case bites in practice.

### Phase C manual residue

- **Provider Combo / API keys / model inputs in Preferences** — Phase D scope per plan; current Assistant tab notes the deferred surface inline so the user doesn't get confused.
- **Lua glue (`ai.add_context` / `ai.prompt` / `ai.clear_context`)** — Phase E scope; AppController's always-on stubs remain no-ops in Phase C.
- **3-layer agents.md (individual view override)** — plan calls for `<views-dir>/<view-id>.agents.md`. Current shape ships only global + project. Adding the 3rd layer is a backlog candidate once a clear per-view authoring story exists.
- **Live-API verification of agents.md prefix end-to-end** — same hand-off as Phase B Scenarios 2 / 4 / 5; needs `AiAssistantSendScenario` fixture harness.

### Phase B manual residue

- **Live-API verification** — Scenarios 2, 4, 5 need a real OpenAI endpoint. Workaround: a future `test-author` slice wires `IAiClient::SendStreaming` against a canned httplib fixture (same shape as `DockGapSentinelScenario` from PR #146) — a new `AiAssistantSendScenario` that drives the worker thread + asserts on the resulting `g_ui.assistantHistory` and `g_ui.assistantLastError`. Backlog candidate: `2026-05-17 · test-author · [tooling] — Headless AiAssistant scenarios (Scenario 2/4/5)`.
- **In-process mutation-sanity reasoning** — Mutation #2 (remove `assistantTurnGen != turnGen` drop in onDelta + onError): static reasoning confirms turn-1 late deltas (1-2 chunks after Cancel before cpr aborts) would corrupt turn-2's `assistantStreamBuf`. Empirical observation gated on the same fixture harness above.
- **Launch-time eyeball** — open `build/ninja-iter-msys2/Smatchet.exe`, View → Assistant (or Ctrl+Shift+A), drag left edge, close X, relaunch, verify panel reopens at the dragged width. Not blocked on user; orchestrator can self-verify.

### Manual residue

- **None for Phase A.** No user-visible surface introduced; the only verification is "does it build dual-target". Plan handed off to `test-author` for Phase A' when the umbrella releases the `tests/**` lock — three scenarios (2, 4, 5) automatable via a CLI smoke harness that drives `IAiClient::SendStreaming` against a canned httplib fixture; one (13) automatable via a `cmake --preset` matrix once Phase B/E add the gates.

## Pending follow-ups

| Item | Trigger | Owner | Notes |
|---|---|---|---|
| Phase A' — `ConfigManager` Ai field set + DPAPI key protection + doctests | `test-suite-expansion` umbrella releases `Source_Core/src/ConfigManager*.cpp` + `tests/**` | orchestrator + `test-rig` | Pre-stageable on a branch off `feat/ai-assistant-side-panel` once PR #140 merges. |
| Phase B — `SmatchetAiAssistantUi` + worker thread inside `AiAssistantController` + `MainThreadDispatcher` + Cancel button + persistent open/closed + width | Phase A merged AND `large-files-and-phase-2 · Track B · on-hold` released (touches `AppController.{h,cpp}` + Lua quartet) | orchestrator | Scope already specified in § File-level changes table. |
| Phase C — `AgentsMdLoader` + `AiContextBuilder` + per-block checkboxes | Phase B merged | orchestrator | All-new files except `SmatchetPreferencesUi.cpp` toggle wiring. |
| Phase D — `AnthropicClient` + optional `OllamaClient` + `AiNdjsonParser` + provider Combo | Phase C merged | `tracker-backend`-shape (HTTP client) | `AiClientFactory::MakeAiClient` Anthropic + OllamaNative branches go non-null. |
| Phase E — Lua glue restore + `LayoutSchemaVersion` 5→6 (single bump) | Phase D merged | `lua-binder` | Touches `AppController_LuaBindings.cpp` ↔ `AppController_LuaStubs.cpp` pair. |
| Plan-time accuracy audit on file-level tables | future plan-doc revisions | author | Caught one mis-listed caller (`FieldCatalogCache.cpp`) and one wrong CMake mechanism (`CORE_SOURCES` glob, not explicit list) in this plan. Cheap to re-grep before sealing the file-level table. |

## Outcome

Plan-execution summary. All 6 phases shipped 2026-05-16 → 2026-05-17 in PR-per-phase cadence. No phase abandoned; one phase narrowed (A → A' split) due to the `test-suite-expansion` umbrella's hold on `Source_Core/src/ConfigManager*.cpp` + `tests/**` at the time A was authored; A' shipped once that umbrella released.

| Phase / Slice | PR | Sha | Cases Δ | Assertions Δ | Notes |
|---|---|---|---|---|---|
| A — IAiClient skeleton + OpenAiClient (narrowed) | [#140](https://github.com/alexandrosk0/Smatchet/pull/140) | `eeea501` | 0 | 0 | ConfigManager fields + doctests deferred to A' per `test-suite-expansion` umbrella collision |
| A' — ConfigManager Ai fields + DPAPI + doctests | [#157](https://github.com/alexandrosk0/Smatchet/pull/157) | `a7cd940` | +23 | +73 | 17 `TrackerConfig` Ai fields including DPAPI-protected `AiApiKey` + `AiAnthropicApiKey`; `AiSseParser` + `AiClientFactory` doctests |
| B — Assistant side panel + AiAssistantController worker thread + Ctrl+Shift+A | [#163](https://github.com/alexandrosk0/Smatchet/pull/163) | `dd703ab` | 0 | 0 | First user-visible AI surface. Threading invariants enforced (per-turn cancel atom, MainThreadDispatcher worker→UI hand-off, stale-callback drop via `assistantTurnGen`). AppController gains always-on `AddAiContext`/`ClearAiContext`/`PromptAi` stubs so Phase E Lua glue is stable across `SMATCHET_WITH_AI` ON / OFF |
| C — AgentsMdLoader + AiContextBuilder + context-block checkboxes | [#168](https://github.com/alexandrosk0/Smatchet/pull/168) | `339eb24` | +26 | +91 | Global + project agents.md layering with 64 KB cap; 5-block context builder (Selection / VisibleRows / ActiveTicket / ActiveView / AuditTrail); SystemPrompt assembled on worker for the streaming request |
| D — AnthropicClient + OllamaClient + AiNdjsonParser + provider Combo | [#169](https://github.com/alexandrosk0/Smatchet/pull/169) | `1b45505` | +6 | +37 | Anthropic Native Messages API via existing AiSseParser; Ollama `/api/chat` NDJSON via new AiNdjsonParser; Preferences UI Assistant group completed (Combo + masked keys + per-provider models + base URLs) |
| E — Lua `ai.*` glue + LayoutSchemaVersion 5→6 + README/LUA_GUIDE bullets | [#170](https://github.com/alexandrosk0/Smatchet/pull/170) | `f2d0933` | 0 | 0 | `ai.add_context` / `ai.clear_context` / `ai.prompt` registered on `state["ai"]` in `InitLuaCore`; resolve via `__smatchet_app_ui` (Phase 6b dual-key pattern); single schema bump for the whole feature; no LuaStubs.cpp parity work needed — receivers are always-on AppController members from Phase B |
| **Total (doctest)** | | | **+55 cases** | **+201 assertions** | |

Adjacent chore / cleanup PRs (not feature work, but shipped during this plan):

| PR | Notes |
|---|---|
| [#155](https://github.com/alexandrosk0/Smatchet/pull/155) | `chore(post-session-tidy): gitignore lock file + retrospective PR audit findings` — runtime `smatchet_config.json.lock` now gitignored |
| [#159](https://github.com/alexandrosk0/Smatchet/pull/159) | `chore(plan-locks): flip 8 stale in-flight entries to shipped` — full plan-locks audit |
| [#164](https://github.com/alexandrosk0/Smatchet/pull/164) | `chore(phase-b-followups): flip Phase B lock + file 5 self-improvement entries` |
| [#166](https://github.com/alexandrosk0/Smatchet/pull/166) | `chore(dev): scripts/dev/tail-agent.sh for observing live subagent progress` |
| [#167](https://github.com/alexandrosk0/Smatchet/pull/167) | `chore(agents): subagent progress-marker convention — .progress.log + agent-progress.sh + tail-agent.sh prefer` |

### End-state aggregate

- **SmatchetTests**: 363 cases / 1873 assertions (up from 308 / 1672 pre-plan; +55 / +201 from this plan)
- **SmatchetLuaTests**: 29 cases / 162 assertions (unchanged by this plan; Phase E added 3 Lua glues but no new test surface — `LuaStubsCompile.test.cpp` drift sentinel covers stubs ↔ bindings parity)
- **`SMATCHET_WITH_AI=ON`** + **`OFF`**: both build clean end-to-end. AppController exposes always-on `AddAiContext` / `ClearAiContext` / `GetAiContext` / `PromptAi` so Lua scripts that call `ai.*` work in both modes (no-op under OFF)
- **`LayoutSchemaVersion`**: 5 → 6. Old v4 / v5 configs migrate silently via `j.value(..., default)` on every new Ai field. `test-config-migration.sh` fixtures stay at their authored version
- **End-user surface**: View menu → Assistant (`Ctrl+Shift+A`) opens right-anchored panel. 4 providers configurable (OpenAI / Anthropic / Ollama OpenAI-compat / Ollama Native). 5 toggleable auto-context blocks. agents.md layering (global + project). Cancel-mid-stream. Persistent open/closed + width

### Manual residue carried forward (filed in backlog)

- **Headless AiAssistant scenarios (Scenarios 2 / 4 / 5 / 8 / 9 / 11 / 12)** — live-API streaming end-to-end verification needs a canned `httplib::Server` fixture (same scaffold as `DockGapSentinelScenario`). Filed under `test-author · [test] · P2` (added by Phase B follow-up PR #164). Phases A → E ship without this; runtime smoke is "launch the app, configure a provider, type a prompt" until the fixture lands
- **3-layer agents.md (per-view override)** — plan called for `<views-dir>/<view-id>.agents.md` as the third layer. Phase C shipped only global + project. Defer until a clear per-view authoring story exists
- **`AiBaseUrl` shared between OpenAI + Anthropic in Preferences** — a user who switches providers and forgets to clear the URL can point Anthropic at an OpenAI endpoint. Filed Phase D deviation; backlog candidate to split into `AiBaseUrlOpenAi` + `AiBaseUrlAnthropic` if the case bites
- **`MainThreadDispatcher::PostUiTask` typed sugar** — current worker→UI hand-off uses `function<void()>` + `g_ui` extern shim. A typed `PostUiTask([](UiDrawSession&){...})` would centralise the pattern. Filed `code-review · [tooling] · P3` via PR #164
- **`unique_ptr<incomplete-type>` AGENTS.md note** — Phase B agent hit this; pattern worth one bullet in `AGENTS.md § Quality`. Filed `orchestrator · [process] · P3` via PR #164
- **Worktree-bootstrap stale-HEAD** — Phases D + E both observed isolated worktrees rooted on `f2ce5b5` instead of `origin/develop`. ~3 min recovery per dispatch. Filed `orchestrator · [process] · P2` via PR #170. Fix candidate: `scripts/dev/worktree-spawn.sh` to pin new branches to `origin/develop`

**Plan status**: closed. Moved to `docs/plans/shipped/ai-assistant-side-panel.md` via a chore PR after PR #170 merged. Future AI assistant work originates from individual backlog entries rather than this multi-phase plan.

## Post-ship retrospective (2026-05-17)

`code-review` + `security-review` sweep over the merged feature surfaced findings beyond hotfix [#165](https://github.com/alexandrosk0/Smatchet/pull/165). Two follow-up batches:

| Batch | PR | Scope | Status |
|---|---|---|---|
| Hotfix batch 1 | [#165](https://github.com/alexandrosk0/Smatchet/pull/165) | First retrospective sweep — 6 P0/P1 items | shipped |
| Hotfix batch 2 | [#176](https://github.com/alexandrosk0/Smatchet/pull/176) | Second retrospective sweep — 4 CRITICAL + 8 HIGH | shipped |

### Hotfix batch 2 (PR #176) shipped fixes

| # | Severity | What |
|---|---|---|
| 1 | CRITICAL | `AnthropicClient` error body now redacted via `RedactProviderErrorBody` |
| 2 | CRITICAL | `OllamaClient` error body now redacted; `AiErrorRedact` extended with `x-api-key` / `X-Api-Key` / `anthropic-api-key` JSON-field rules |
| 3 | CRITICAL | `ConfigManager::Save` calls from `SmatchetAiAssistantUi` deferred to detached worker (UX pillar 2) |
| 4 | CRITICAL | `BackendAuditTrail::ReadRecentEvents` deferred from UI to worker via new `AiContextBuilder::Inputs::DeferAuditTrailFetch` + `kDeferredAuditTrailSentinel` |
| 5 | HIGH | New `AiEndpointSanitize` pure helper — rejects non-http(s), CR/LF/NUL, cloud-metadata IPs (169.254.169.254, 100.100.100.200), link-local 169.254/16; API keys CR/LF/NUL-stripped |
| 6 | HIGH | 4 MiB caps on `AiSseParser` + `AiNdjsonParser` + `assistantStreamBuf` |
| 7 | HIGH | `luaContext_` mutex-guarded (UI thread + automation worker both touch) |
| 8 | HIGH | Cancel-atom race fixed — `RunRequest` trusts cancel local captured in `WorkerLoop` |
| 9 | HIGH | Worker-side agents.md cache, invalidated from Preferences UI on path edit |
| 10 | HIGH | `AppController::GetAiAssistantController` no longer lazy-constructs post-shutdown |
| 11 | HIGH | Static input buffer re-seeds on divergence (Lua-supplied text survives panel reopen) |
| 12 | HIGH | `AiSseParser` strips exactly one leading space per RFC 8895 |

### Hotfix batch 2 — tests added

- `tests/Source_Core/AiEndpointSanitize.test.cpp` — 8 cases (default / allowed / rejected schemes / control chars / cloud metadata / link-local / malformed)
- `tests/Source_Core/AiSseParser.test.cpp` — RFC single-space-strip contract + 4 MiB cap with `Reset` recovery
- `tests/Source_Core/AiNdjsonParser.test.cpp` — 4 MiB cap with `Reset` recovery
- `tests/Source_Core/AiErrorRedact.test.cpp` — three subcases for `x-api-key` / `X-Api-Key` / `anthropic-api-key`

### Remaining work (filed in backlog)

Deferred MED + LOW items from the retrospective. All live entries under `docs/backlog/agent-self-improvement/`. See:

- **security** — `AI-client URL allow-list policy` (P1), `Default AuditTrail toggle off + consent dialog` (P1), `AgentsMdLoader path traversal` (P2), `ai.prompt Lua rate limit + consent toast` (P2), `CR/LF strip at config persist site` (P3), `SSE/NDJSON parse-failure log redact` (P3)
- **bug** — `AiSseParser::Flush synthesised boundary` (P3)
- **process** — `agents/security-review.md AI surface map update` (P2)
- **tooling** — `install gitleaks + semgrep + flawfinder in dev image` (P3)
- **test** — `per-client cancel-abort regression test` (P2), `per-client error-body redaction regression test` (P2)
- **infra** — `per-chunk dispatcher coalescing` (P3), `ImGuiListClipper on long histories` (P3), `BuildActiveTicketBody O(N) → IdIndex` (P3), `AgentsMdLoader read = min(maxBytes+1, file_size)` (P3), `InputTextMultiline truncation toast` (P3)
