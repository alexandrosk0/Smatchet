# Smatchet Agentic Infrastructure — A Builder's Survey (without-agents pass)

## 1. Executive Summary

Smatchet is a C++ desktop app (Jira/Plane ticket client built on ImGui) that has grown a
surprisingly serious *agent-facing* surface: a single command registry that fans out to five
frontends (CLI, in-app palette, MCP tools, Lua, and an Unreal bridge), a built-in MCP server
served over HTTP/SSE inside the running process, a provider-pluggable streaming AI assistant
(`IAiClient` over OpenAI/Anthropic/Ollama/DeepSeek), and a sandboxed Lua automation layer that
can both *call* the registry and *register new MCP tools*.

**Verdict: yes, there is real inspiration here — concentrated in two patterns.** The
**Unified Command System** (`Source/Core/include/Commands/Command.h`) is the genuinely
copy-worthy idea: a small POD `Command` struct carrying a typed param schema and a handler
lambda, with one validation/coercion/dispatch chokepoint that every entry point funnels
through. It is the textbook "define once, expose everywhere" tool-registry pattern, executed
cleanly in portable C++14, with a stable wire envelope, fuzzy did-you-mean suggestions, a
uniform destructive-confirm gate, dry-run, and source-aware audit logging. The **AI assistant
threading model** (worker→UI hand-off via a `MainThreadDispatcher` with per-turn generation
counters and per-turn cancel atoms) is also production-grade and worth studying for any app
that streams LLM output onto a high-framerate UI thread.

What is *missing* is the actual *agent loop*: this is tool-*plumbing*, not an agent. There is
no tool-call loop, no function-calling integration between the AI assistant and the command
registry, no retries/backoff/rate-limiting on provider calls, no token accounting beyond a
`bytes÷4` CLI estimate, and no eval harness for the LLM behaviour itself. The MCP server lets
*external* agents (Claude Desktop, Cursor) drive the app; the *internal* AI assistant is a
plain streaming chatbot that cannot call the tools the same registry exposes. That gap is the
single biggest "what I'd add."

Overall inspiration value: **7.5/10.**

## 2. Scope & Method

I read the shipped product's code, not the docs alone. Primary files studied in depth:
`Source/Core/include/Commands/Command.h`, `Source/Core/src/Commands/CommandRegistry.cpp`,
`Source/Core/src/Commands/Command.cpp`, `Source/Core/src/Commands/Builtin/BuiltinCommands_Tickets.cpp`,
`Source/Plugins/Mcp/McpPlugin.cpp`, `Source/Core/src/AiAssistantController.cpp`,
`Source/Core/include/IAiClient.h`, `Source/Core/include/AiTypes.h`,
`Source/Core/src/AnthropicClient.cpp`, `Source/Core/src/AiSseParser.cpp`,
`Source/Core/src/AppController_LuaBindings.cpp`, `Source/Core/src/AppController_LuaBindingsCore.cpp`,
`Source/Core/src/AiContextBuilder.cpp`, plus `CLI_GUIDE.md`, `MCP_GUIDE.md`, `LUA_GUIDE.md`,
and the `SMATCHET_WITH_*` gating in `CMakeLists.txt`.

**Per the pass constraint, I deliberately ignored the agentic-governance meta-layer**: root and
per-subsystem `AGENTS.md`, the `agents/` directory, `AI_POLICY.md`, `docs/agent-rules/**`,
`docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`, `.coderabbit.yaml`, and
`.cursor/`. (I did, unavoidably, see `Source/Core/src/Commands/AGENTS.md` listed in a directory
glob, but did not open it.) This report evaluates only the *shipped product's* agentic/AI
capabilities as a developer reading the codebase for reusable patterns. Note: the runtime does
ship an `agents.md` *context-layering* feature (`AgentsMdLoader::LoadLayered`) consumed by the AI
assistant system prompt — that is a *product* feature and is in scope; the *governance* docs of
the same name are not.

## 3. Pattern Catalog

### 3.1 The typed-command-struct-as-tool-definition (headline)
**What:** `struct Command` (`Command.h:159`) bundles name, category, summary/description,
`std::vector<ParamSpec>`, behavioural flags (`Destructive`, `Idempotent`, `AsyncSafe`,
`DryRunSupported`), aliases, and a `std::function` handler. `ParamSpec` (`Command.h:35`) carries
type, required-ness, description, default (boxed in `shared_ptr<json>`), enum allow-list, numeric
bounds, and a max-byte-length. From one struct, `BuildJsonSchema()` emits an MCP `inputSchema`
and `BuildHelpText()` emits CLI help (`Command.cpp:106`, `:151`).
**Why interesting:** This is the cleanest expression of "one definition, many frontends" I've
seen in a C++ codebase. The schema is *derived*, never hand-maintained per surface.
**Reusability:** Very high — directly portable to any tool-registry design.
**Caveat:** The schema vocabulary is shallow: only `string/integer/boolean/number/object`. There
is no nested-object schema, no array-item typing, no `oneOf`/pattern — a `Json` param becomes a
bare `{"type":"object"}` (`Command.cpp:100`). For richer tool args you'd outgrow it.

### 3.2 Single dispatch chokepoint with layered guards
**What:** `CommandRegistry::Dispatch` (`CommandRegistry.cpp:283`) is the *only* execution path.
It (1) snapshots the command under a mutex so handler reentrancy is safe, (2) returns
fuzzy-suggestion failures on unknown names, (3) validates+coerces+defaults args
(`ValidateAndResolveArgs`, `:221`), (4) applies the destructive-confirm gate, (5) applies the
dry-run-unsupported gate, (6) invokes inside a try/catch that converts any throw into a
structured `HandlerError`, and (7) records recents only on success.
**Why interesting:** Every cross-cutting concern (validation, confirmation, audit, exception
safety) lives in exactly one place, so all five frontends inherit identical semantics for free.
**Reusability:** High — this is the right shape for a tool dispatcher.
**Caveat:** `Dispatch` copies the whole `Command` struct (handler `std::function` included) on
every call to avoid holding the lock during execution (`:289-294`). Correct, but allocates per
dispatch; a `shared_ptr<const Command>` would be cheaper.

### 3.3 Source-aware trust without per-source bypass
**What:** `CommandSource` (`Command.h:94`) tags each call (Cli/Palette/Mcp/Lua/Unreal/Internal).
`RequiresExplicitConfirm` (`Command.h:139`) is a *pure* predicate documenting that **no source
bypasses the confirm gate** — `source` is deliberately `(void)`-cast. Source only changes the
*audit posture*: automation sources (CLI/MCP/Lua, `IsAutomationSource`, `Command.h:121`) get a
`LOG_WARN` audit line on every destructive dispatch (`CommandRegistry.cpp:321`).
**Why interesting:** This is a thoughtful answer to "a token grants *reach*, not *blanket
destructive authority*." Destructive MCP/Lua/CLI calls require an explicit per-call flag
(`__confirm` / `--yes`) that the transport never auto-sets, and `--dry-run` previews mutations.
**Reusability:** High — the "reach ≠ authority" framing transfers to any agent tool surface.
**Caveat:** Confirmation is a single boolean. There's no scoped capability model (e.g. "this
agent may transition but not delete"), no per-tool allow-listing per client.

### 3.4 Canonical wire envelope + stable error taxonomy
**What:** `CommandResult::ToWireJson` (`Command.cpp:75`) always emits `{ok, command, data?}` or
`{ok:false, command, error:{code,message,hint,suggestions,details}}`. `ErrorCode` (`Command.h:51`)
is a stable kebab-case enum mapped by `ErrorCodeString`, and the CLI maps these to stable exit
codes (`CLI_GUIDE.md`: 2=unknown, 5=confirm-required, 6=not-connected, …).
**Why interesting:** Agents get one parseable shape across every transport, with actionable
`hint` and `suggestions` fields. MCP returns HTTP 200 even for logical errors, putting the
envelope in `content[0].text` (`McpPlugin.cpp:537`) so callers parse rather than treat errors as
transport failures.
**Reusability:** High — the envelope+exit-code contract is a clean template.

### 3.5 In-process MCP server with defence-in-depth
**What:** `McpPlugin` (`McpPlugin.cpp`) runs cpp-httplib in a worker thread inside the app,
serving SSE (`/mcp/sse`), JSON-RPC (`/mcp/messages`), and convenience REST. `tools/list` is built
by transforming `app->Commands().All()` into tool entries with derived schemas
(`McpPlugin.cpp:489`, `:924`). `tools/call` tries the registry first (`:592`), then `run_lua`,
then Lua-registered tools, then a structured unknown-command envelope.
**Why interesting:** The security posture is unusually mature for a hobby-scale feature:
loopback-only by default, DNS-rebinding defence via Host/Origin checks (`:162`), constant-time
token compare (`:198`), a spawn-token handshake so a CLI-launched child adopts a random token
from an env var and *scrubs it* (`:234-248`), bounded JSON parsing of attacker-controlled bodies
(`ParseBounded`, `:566`), a concurrent-SSE cap to avoid exhausting the 8-thread pool (`:697`),
and an attachment proxy that rejects userinfo URLs and enforces an allow-list (`:385-395`).
**Reusability:** Medium-high — the protocol handling is bespoke but the hardening checklist is a
great reference. **Caveat:** it implements a *subset* of MCP (no `resources`, `prompts`,
`sampling`, no session IDs — "we use a single global endpoint", `:717`), and the SSE/JSON-RPC
handling is hand-rolled rather than using an MCP SDK.

### 3.6 Provider-pluggable streaming AI client
**What:** `IAiClient` (`IAiClient.h`) is a 3-method interface: `GetProviderName`,
`ProbeReachability`, and `SendStreaming(cfg, req, onDelta, onError, cancel)`. Concrete clients
(`AnthropicClient.cpp`, `OpenAiClient.cpp`, `OllamaClient.cpp`) each translate their wire format
(Anthropic SSE events, OpenAI SSE, Ollama NDJSON) into the same `AiStreamDelta`/`AiStreamError`
callbacks. `AiClientFactory::MakeAiClient` (`AiClientFactory.h:14`) switches on the enum and has a
`SetTestOverride` seam for injecting stub clients in tests.
**Why interesting:** Textbook strategy pattern. One OpenAI client backs three providers (OpenAi,
Ollama-OpenAI-compat, DeepSeek) because they share the wire format. The factory test-override is
exactly the right seam for deterministic provider tests without HTTP.
**Reusability:** High — the interface is small and clean enough to lift wholesale.
**Caveat:** No tool/function-calling in the interface at all — `SendStreaming` is text-in,
text-out. Adding tool-use would require extending `AiChatRequest`/`AiStreamDelta`. Also note
`AiTypes.h:62-67` defaults `TotalTimeoutMs` to 120s but the controller bumps it to 600s for
reasoning models (`AiAssistantController.cpp:120`) — a per-call envelope timeout, not idle.

### 3.7 Worker→UI streaming hand-off with generation counters
**What:** `AiAssistantController` (`AiAssistantController.cpp`) owns a dedicated worker thread, a
`pending_` queue, a condition variable, and a per-turn `currentCancel_` shared `atomic<bool>`.
Deltas are marshalled to the UI via `dispatcher_.PostToMainThread`, and every posted lambda
checks `ui->AssistantTurnGen() != turnGen` to drop stale callbacks from a cancelled/superseded
turn (`:446`, `:506`). Each turn owns its own cancel atom so cancelling turn N can't flip turn
N+1's flag (`:208-211`).
**Why interesting:** This is the genuinely hard part of streaming LLM output onto a 144Hz UI
thread, and it's done right: no synchronous I/O on the UI thread, bounded stream buffer (4 MiB
cap, `:453`), fail-closed provider rebuild between turns (`:300`), and a model-signature change
detector that auto-clears chat history (`:340`).
**Reusability:** High — the generation-counter + per-turn-cancel-atom pattern is reusable in any
GUI that streams async results. **Caveat:** It's a *single-turn* worker; there's no concurrency
across turns and no tool-call interleaving.

### 3.8 Bounded, redacting, fuzzable stream parsers
**What:** `AiSseParser` (`AiSseParser.cpp`) is a byte-fed incremental SSE frame parser with a hard
buffer cap (`overflowed_` poisons the stream past `kAiSseParserMaxBufferBytes`, `:43`), correct
`\n\n` vs `\r\n\r\n` boundary handling, and a deliberate decision to *drop* trailing partial
frames rather than synthesize a truncated final token (`:102-111`). Provider error bodies are run
through `RedactProviderErrorBody` before logging (`AnthropicClient.cpp:83`) to strip leaked
api-key/Bearer shapes. There are dedicated fuzz harnesses (`tests/fuzz/fuzz_ai_sse.cpp`,
`fuzz_ai_ndjson.cpp`) with corpora.
**Why interesting:** Treating the LLM byte stream as *attacker-influenced* (a misconfigured proxy
could echo headers) and fuzzing it is a maturity signal most AI integrations skip.
**Reusability:** High — the bounded-incremental-parser-with-redaction pattern is a good default.

### 3.9 Structured LLM context assembly
**What:** `AiContextBuilder` wraps each context block in
`<smatchet_context block="...">...</smatchet_context>` tags (`AiContextBuilder.cpp:67`), composed
after an `agents.md` layered prefix (`ComposeSystemPrompt`). Block kinds (`AiTypes.h:70`):
active ticket, active view, multi-selected tickets, visible grid rows, and audit trail. The
expensive audit-trail block uses a clever **deferred-fetch sentinel**: the UI emits a block whose
body is the literal `"__SMATCHET_DEFERRED__"`, and the worker thread rewrites it with the real
SQLite/filesystem read off the UI frame (`AiAssistantController.cpp:361-393`). The `agents.md`
blob is cached and invalidated on path/preference change (`:402-424`).
**Why interesting:** XML-tagged context blocks + deferred heavy-I/O assembly + a per-layer 64 KB
cap is a tidy, bounded context-injection design.
**Reusability:** Medium-high — the deferred-sentinel trick is the standout idea.

### 3.10 Lua as a self-extending agent surface
**What:** `commands.invoke("name", {args})` (`AppController_LuaBindingsCore.cpp:188`) lets scripts
call any registry command and get back `{ok, data, error}` — Lua is just another `CommandSource`,
subject to the same confirm gate (`__confirm`, `:208`). Scripts can also `mcp.register_tool(...)`
to publish *new* MCP tools (`MCP_GUIDE.md` §4), and `ai.prompt(...)` to invoke the assistant —
gated by a re-entrancy lock, a 5s rate limit, and a one-time-per-session consent toast naming the
outbound provider (`AppController_LuaBindings.cpp:664-705`). The sandbox blocks `io`, `os.execute`,
`load`/`loadstring`, `require`, `debug`, metatable escapes, and `string.dump`
(`CreateSandboxEnvironment`, `:241-280`), with a 100k-instruction cap on MCP tool callbacks.
**Why interesting:** A scripting layer that can both consume *and extend* the tool registry is a
powerful, dynamic agent surface — and the sandbox is whitelist-oriented (new dangerous `os.*`
won't silently leak in).
**Reusability:** Medium — sol2-specific, but the "scripts are an automation source with explicit
consent + rate limit + sandbox" model is broadly applicable.

## 4. The Unified Command Registry — Deep Dive

This is the architectural centrepiece and deserves the closest look. The design contract is
stated right in the header (`Command.h:4-13`): one `Command` struct feeds five frontends, and the
header is held to strict C++14 (no `string_view`/`optional`/`variant`) *because it compiles into
both the MinGW standalone and an MSVC-under-Unreal core*. That portability constraint is load-
bearing — it's why the schema carriers are boxed behind `shared_ptr<json>` and the header pulls
`json_fwd.hpp` rather than the full `json.hpp` (`:26`).

**Registration ergonomics.** Commands are registered in small `BuiltinCommands_*.cpp` files via a
`MakeCommand(name, summary, handler)` helper plus param helpers `PInt`/`PString`
(`BuiltinCommands_Tickets.cpp:29-58`). A handler is a lambda capturing `AppController& app` by
reference, reading args with `args.value("limit", 50)`, and returning
`CommandResult::Success(json)`. Read-only list commands route through a shared
`PaginateJsonArray` so every list returns `{items, total, limit, offset, hasMore}` — pagination
is a convention, not per-command boilerplate. Back-compat is handled by an alias table
(`tickets.list_active` aliases the legacy MCP name `list_active_tickets`,
`BuiltinCommands_Tickets.cpp:57`), with first-writer-wins collision handling
(`CommandRegistry.cpp:33-41`).

**The fan-out.** Each frontend is a thin adapter over `Dispatch`:
- **MCP** builds tool entries from `Commands().All()` and dispatches with
  `Source::Mcp`, lifting `__confirm`/`__dry_run`/`__timeout_ms` out of the arguments object into
  the `CommandContext` (`McpPlugin.cpp:521-540`).
- **Lua** does the identical lift from the args table (`AppController_LuaBindingsCore.cpp:198-210`).
- **CLI** (per `CLI_GUIDE.md`) maps flags to context and the envelope to exit codes, and is itself
  just an MCP client over HTTP — there is no separate server process; the CLI attaches to the
  running app via `instance.json` discovery (`McpPlugin.cpp:265-296`).
- **Palette** and **Unreal** are interactive UIs that set `ConfirmedDestructive` themselves.

**Discovery is a first-class feature.** `commands.list/help/search/recents` are themselves
commands (the `commands.*` category), so an agent bootstraps entirely from inside the tool
surface: list the catalog, fetch a JSON schema per command, fuzzy-search by intent. `--tokens`
pre-estimates output size before pulling a large result into context. This is genuinely
agent-ergonomic.

**Where it stops short.** The schema vocabulary (§3.1 caveat) is the main ceiling. There's also
no notion of a *tool result schema* — `Description` is expected to document the return shape in
prose (`BuiltinCommands_Tickets.cpp:52`), but it's not machine-readable, so an agent can't
validate or plan over outputs. And `AsyncSafe`/`PendingAsyncResult` exist for spawn-mode waits but
there's no streaming-tool or progress-event concept. For a UI-automation registry these are fine;
for a rich agent platform they're the next things you'd build.

## 5. AI Assistant & MCP Architecture Critique

**Strengths.** The provider abstraction (§3.6) and threading model (§3.7) are the strongest parts.
The endpoint sanitisation is notably careful: every base URL — *including built-in defaults* —
passes through `SanitizeAiEndpointUrl` against a per-provider `EndpointPolicy`
(`AiAssistantController.cpp:79-112`), header values are stripped of CR/LF/NUL to prevent header
smuggling (`:56-62`), and a null client rebuild fails the turn *closed* rather than silently
routing through the previous provider (`:300-310`). Error bodies are redacted before logging.
This is security hygiene that most LLM integrations simply don't have.

**The central gap: the internal AI assistant cannot use the tools.** The MCP server exposes the
whole command registry to *external* agents, but the *in-app* `AiAssistantController` is a plain
text-streaming chatbot. `IAiClient::SendStreaming` has no tool-call channel; `AiChatRequest` has
no `tools` field; `AiStreamDelta` has no tool-call delta. So the assistant can be *told about*
the active ticket/grid/audit via injected context blocks, but it cannot *act* — it can't call
`ticket.set_field` or `view.activate` even though those tools exist three layers down in the same
process. The architecture has all the pieces for an agent loop (a tool registry with JSON
schemas, a streaming client, a dispatcher) and wires exactly none of them together internally.
Closing that loop — feed `BuildJsonSchema()` output as the provider `tools` array, parse tool-call
deltas, dispatch through `Commands().Dispatch`, loop — is the obvious high-value next step and
would turn this from "chatbot + remote-controllable app" into "embedded agent."

**MCP is a hand-rolled subset.** It speaks enough JSON-RPC to satisfy Claude Desktop/Cursor
(`initialize`, `tools/list`, `tools/call`) but omits `resources`, `prompts`, and `sampling`, uses
a single global SSE endpoint with no session management (`McpPlugin.cpp:717`), and hard-codes
`protocolVersion`/`serverInfo` (`:919-921`). For an internal automation bridge that's pragmatic;
as a reference MCP implementation it's incomplete.

**No reliability layer on provider calls.** There is a per-call total timeout and a user-driven
cancel atom, but **no retries, no exponential backoff, no rate-limit handling (429), no circuit
breaker.** A transient provider blip surfaces as a terminal error strip. The Lua `ai.prompt` path
has a crude 5s client-side rate limit (`:677`), but the provider clients themselves have none.

## 6. Gaps / What a Serious Agentic Platform Would Add

1. **Internal tool-call loop** — connect the AI assistant to the command registry via provider
   function-calling (the single biggest gap; §5).
2. **Machine-readable result schemas** — so agents can validate and plan over tool outputs, not
   just inputs.
3. **Retries/backoff/rate-limit handling** on `IAiClient` calls; honour `Retry-After`, handle 429.
4. **Observability** — there's audit *logging* (`AppendMcpActivity`, destructive `LOG_WARN`s) but
   no metrics, no per-tool latency/error counters, no token-usage accounting beyond the CLI's
   `bytes÷4` estimate. Provider responses carry `usage` blocks that are currently discarded.
5. **An eval harness for LLM behaviour.** There are excellent *deterministic* tests (SSE/NDJSON
   fuzzers, AI scenario tests under `tests/ui/`, the `AiClientFactory` stub seam, golden command-
   palette images) and an `tests/agent-eval/` dir — but nothing that scores actual model
   *outputs* against a rubric. The plumbing is well-tested; the agent behaviour is not.
6. **Scoped capabilities** — confirmation is one boolean. A real platform wants per-client tool
   allow-lists and capability scopes ("read-only token" vs "mutating token").
7. **Streaming/progress tool results** — long-running tools (`sync.full`, `scenario.run`) return
   only a final envelope; no progress events.
8. **Conversation/tool-call persistence with provenance** — chat history persists to SQLite
   (`SmatchetChatPersistWorker`), but there's no record of *which tool calls* produced a result.

## 7. Scorecard

| Dimension | Score | Notes |
|---|---:|---|
| Command-registry design | 9/10 | Define-once/expose-everywhere done cleanly; portable C++14; one dispatch chokepoint. Loses a point for shallow schema vocabulary + no result schemas. |
| MCP implementation | 7/10 | Excellent hardening (DNS-rebind, constant-time token, spawn-handshake, bounded parse, SSE cap) but a hand-rolled protocol *subset*; no resources/prompts/sessions. |
| AI provider abstraction | 8/10 | Small clean `IAiClient`, one client backing three wire-compatible providers, test-override seam. No tool-calling channel. |
| Streaming / threading | 9/10 | Generation counters + per-turn cancel atoms + dispatcher hand-off + bounded buffers + fail-closed rebuild. Production-grade. |
| Scriptability / extensibility | 8/10 | Lua both consumes and *extends* the registry/MCP; whitelist sandbox; consent + rate limit. sol2-specific. |
| Reusability of patterns | 8/10 | The registry, the `IAiClient`/streaming pair, and the bounded parsers are directly liftable. |
| **Overall inspiration value** | **7.5/10** | Outstanding tool *plumbing*; no actual agent *loop*, reliability layer, or eval harness. |

## 8. What I'd Steal vs What I'd Skip

**Steal:**
- The **`Command` struct + single `Dispatch` chokepoint + derived schema/help** pattern, almost
  verbatim. It's the cleanest "one tool definition, many frontends" I've read in C++.
- The **canonical wire envelope + stable kebab-case `ErrorCode` enum + stable exit codes**, with
  `hint`/`suggestions` fields for agent self-correction.
- The **source-aware "reach ≠ authority" confirm model**: explicit per-call `__confirm`/`--yes`
  the transport never auto-sets, plus a uniform `--dry-run` preview, plus automation-source audit
  logging — a single chokepoint, no per-source bypass (`Command.h:121-146`).
- The **`IAiClient` strategy interface + factory test-override seam**, and **one client backing
  multiple wire-compatible providers**.
- The **generation-counter + per-turn-cancel-atom worker→UI hand-off** (`AiAssistantController`),
  and the **deferred-fetch context sentinel** (`"__SMATCHET_DEFERRED__"`).
- The **bounded, redacting, fuzzed stream parsers** and the discipline of treating LLM bytes as
  attacker-influenced.
- The **MCP hardening checklist** (DNS-rebind Host/Origin, constant-time token compare,
  spawn-token handshake with env scrub, bounded JSON ingress, concurrent-SSE cap) as a reference.

**Skip / do differently:**
- **Hand-rolling MCP** — I'd use an MCP SDK and implement `resources`/`prompts`/`sampling` +
  sessions rather than a single global SSE endpoint.
- **The text-only `IAiClient`** — I'd extend it with a tool-call channel from day one and **close
  the internal agent loop** (provider function-calling → `Commands().Dispatch` → loop), which is
  the one missing piece that would make this an embedded agent rather than a chatbot beside a
  remote-controllable app.
- **Prose-only result documentation** — I'd add machine-readable result schemas.
- **Zero provider-call resilience** — I'd add retries/backoff/429-handling and a circuit breaker.
- **No behavioural eval** — I'd add an eval harness scoring model outputs, not just the
  deterministic plumbing (which is already well-tested).
- The **per-dispatch full-struct copy** — I'd store/dispatch `shared_ptr<const Command>`.
