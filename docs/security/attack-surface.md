<!-- tier: project -->
# Smatchet — attack surface

**When to read this file**: a diff touches a trust boundary — MCP, the CLI, Lua bindings or the Lua console, AI assistant clients, p4, tracker HTTP, SQLite, config / cache / attachment paths, image fetches, the merge-watch notify endpoint — or you are running `security-review`, a security audit, or the security leg of a pre-merge review. The `security-review` subagent reaches this file through the review checklist ([`docs/guides/review-checklist.md`](../guides/review-checklist.md) § Security).

The agent layer's own surfaces (the intent-capture pipeline and the merge-watcher notify contract) are in `agent-layer/docs/agent-rules/security-surface.md`. Dated findings and the escalation playbook: [`SECURITY_AUDIT_2026-06-13.md`](SECURITY_AUDIT_2026-06-13.md), [`DEEPER_AUDIT_PLAYBOOK.md`](DEEPER_AUDIT_PLAYBOOK.md).

## Where external input enters

- **MCP server** (`Source/Plugins/Mcp/McpPlugin.cpp`, `SmatchetMcpServerUi`): HTTP-exposed while running; JSON-RPC tool arguments are attacker-controlled. Tool schemas validate their arguments, a missing field defaults to `{}`, and errors return structured envelopes. Loopback bind is the default; `McpAllowRemote` binds `0.0.0.0` as an explicit operator choice. On loopback, `McpPlugin::Authorize` rejects a non-loopback `Host` / `Origin` (the DNS-rebinding defence) before the bearer-token check.
- **CLI** (`Source/Standalone/CliCommandRunner.cpp`): JSON arguments from shells, pipes and `--spawn` scenarios. Command dispatch never crashes on null, a wrong type or a missing key; a new command keeps that defence in depth.
- **Lua** (`scripts/*.lua`, the console in `Source/Plugins/LuaConsole/`): user code in a sandbox bounded by an instruction-count `lua_sethook` (`Lua/LuaHookGuard.h`). The hook does not bound outbound AI HTTP calls; those have their own rate limit (`AiLuaPromptRateLimit.h`).
- **Tracker HTTP** (`TrackerHttpClient` and the Jira, Plane, GitHub and Linear clients): the server may be hostile (a compromised proxy, a malicious self-hosted instance). Parse defensively.
- **P4 CLI** (`P4Annotate`): the depot server may craft data; the CLI runs with the user-configured workspace.
- **Local config and cache** (`ConfigManager`, `LocalCacheManager`, attachment and image-cache dirs): files under user control — the attacker-with-filesystem-access scenario.
- **Image fetches** (`SmatchetImageTextureCache`): URLs come from issue data and hooks; each image is size- and dimension-capped.
- **AI assistant**: the provider clients (`OpenAiClient`, `AnthropicClient`, `OllamaClient`), the streaming parsers (`AiSseParser`, `AiNdjsonParser`), `AgentsMdLoader` (a filesystem read into the prompt), `AiContextBuilder` (an exfiltration channel for ticket, view and audit data) and `AiAssistantController` (worker thread, cancel flag, Lua glue). Per client: the URL allow-list and sanitiser (`AiEndpointSanitize`), error-body redaction so no API key reaches a log (`AiErrorRedact`), buffer caps on streamed responses, `AgentsMdLoader` path validation (no `..` traversal), the Lua `ai.*` rate limit and sandbox, and `AssistantContextBlockAuditTrail` defaulting to `false` (PII is opt-in, never opt-out).
- **Merge-watch notify endpoint** (`SmatchetMergeWatchNotifyServer`, decision logic in `MergeWatchNotifyPure::PlanMergeWatchNotify`, doctested in `tests/Core/MergeWatchNotifyPure.test.cpp`): the receiver for the agent layer's merge watcher. Its contract is in the layer's `security-surface.md` § Merge-watcher notify endpoint.

## Known crash class: JSON DOM teardown

nlohmann's DOM parser is iterative, but destroying a deeply nested tree recurses once per level, so a bare `nlohmann::json::parse` (or a `stream >> json` slurp) of attacker-controlled text overflows the stack on teardown — a `try`/`catch` does not help. Untrusted JSON goes through `smatchet::json_safe::ParseBounded` (`Json/BoundedJsonParse.h`), which caps depth, node count and bytes and returns an error string instead of throwing; the `bare-json-parse-untrusted` lint rule blocks a bare parse. The Lua `decode_json` global already routes through it and returns `nil, err` to Lua; a new binding that accepts a raw string from Lua does the same, never a parse that can throw across the sol2 boundary.

## Checklist

**Injection and command construction**
- `p4 …` in `P4Annotate.cpp`: arguments go as an argv array through `SubprocessCapture::Run`, never concatenated into a shell command. Flag `system()` / `popen` / `&&` chains.
- SQLite in `LocalCacheManager` / `OfflineQueueService`: `?` placeholders only; flag string concatenation into SQL.
- HTTP URLs built from user input (`TrackerHttpClient`): URL-encoded; no host override from user-controlled fields.
- File paths from config (`PathRemaps`, the attachment download dir, the image cache): flag `..` traversal, absolute paths into system dirs, and symlink dereference.

**Input validation at trust boundaries**
- MCP tool arguments: JSON Schema validation present, missing keys handled, size caps on strings.
- CLI arguments: never crash on null, a wrong type or a missing key.
- Lua bindings never accept callables, file paths or raw pointers crossing back into C++.

**Secret handling**
- Auth tokens (Jira API token, Plane PAT, GitHub / Linear tokens, the P4 password) never reach a `LOG_*` call site; run `gitleaks` over the diff.
- Tokens are never serialized into views, scenarios, the audit trail, or error envelopes returned to MCP.
- Config dump commands (`config.get`) redact token-bearing fields.

**Deserialization and parsing**
- Untrusted JSON through `json_safe::ParseBounded` (above); never `.at()` without a presence check, never crash on a missing key.
- Markdown (`md4c`, `MarkdownConvert`) renders as ImGui text, not HTML; verify no path re-injects raw content elsewhere.
- Image decode (`SmatchetImageTextureCache`): the size cap is enforced BEFORE decode, not after.

**Lua sandbox integrity**
- The `lua_sethook` instruction-count protection stays enabled; flag any binding that disables or extends it without justification.
- A new binding exposing filesystem, process or network access is default-deny and needs an explicit rationale.

**MCP server hardening**
- The default listen address stays loopback; flag any change to the default or to the `McpAllowRemote` gate.
- The loopback Host / Origin check stays ahead of authentication.
- No CORS wildcard when cross-origin is enabled.
- A per-request body cap (`set_payload_max_length`) and the concurrent-SSE cap stay in place.
- Errors return structured envelopes: no stack traces, file paths or token-bearing config keys.

**Dependency hygiene**
- A new or bumped FetchContent version (cpr, curl transitively, SQLiteCpp, nlohmann/json, sol2, Lua, cpp-httplib, md4c, stb): check it against known CVEs in the release notes.
