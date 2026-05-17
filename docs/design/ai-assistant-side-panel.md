# AI Assistant — Right-Docked Side Panel + agents.md Harness

> **Plan-doc relocation (mandatory first commit step)**: per `AGENTS.md` § Plan location, plans live at `docs/design/<slug>.md`. After this file is approved, copy it to `docs/design/ai-assistant-side-panel.md` and commit with `wip(plan): ai-assistant-side-panel` before any code work. The path under `~/.claude/plans/` is plan-mode scratch only.

> **Status (2026-05-16)**: Phase A shipped (narrowed scope) — see § Implementation log. Phase A' deferred behind `test-suite-expansion` umbrella release on `Source_Core/src/ConfigManager*.cpp` + `tests/**`. Phases B-E unscoped. Plan-lock entry: [`docs/design/_plan-locks.md`](./_plan-locks.md) § `ai-assistant-side-panel · Phase A-narrowed · status: in-flight`.

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
| `docs/design/ai-assistant-side-panel.md` | A (first commit) | This plan, relocated per Plan-doc rule. |

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
| `README.md`, `LUA_GUIDE.md` | E | One bullet each. |

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
- `docs/design/ai-assistant-side-panel.md` *(NEW — this plan, relocated)*

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
