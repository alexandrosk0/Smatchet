# Agent self-improvement — security

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-17 · security-review · [security] · P1 — AI-client URL allow-list policy (per-provider host opt-in beyond the IP-literal block shipped in PR #176)
  Details: PR #176 added `AiEndpointSanitize` covering non-http(s) schemes, CR/LF/NUL, 169.254.169.254 / 100.100.100.200 metadata IPs, and 169.254/16 link-local. It does NOT enforce per-provider host allow-lists — a config-write attacker can still repoint OpenAi / Anthropic at any public host (e.g. attacker.example.com) and exfil the API key in the Authorization / x-api-key header. The validator deliberately chose breadth over strictness so Azure OpenAI / LiteLLM / openrouter proxies still work, but the right long-term shape is an explicit "custom endpoint" toggle per provider (default off → host must match `api.openai.com` / `api.anthropic.com`; explicit on → any host accepted with a one-time consent dialog naming the host).
  Concrete next action: extend `ConfigManager` with `AiAllowCustomEndpointOpenAi` + `AiAllowCustomEndpointAnthropic` bool fields (default false); extend `AiEndpointSanitize` with a new `EndpointVerdict::RejectedNonProviderHost` and a per-provider allow-list parameter; surface a one-time consent dialog in `SmatchetPreferencesUi` when the user enables either toggle. Estimated cost 2-3 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P2 — First-send outbound-context consent modal (consent-tracking field + UX)
  Details: Default flip of `AssistantContextBlockAuditTrail` to `false` shipped (`ConfigManager.h:248`); remaining work from the original P1 entry is the one-time first-send consent modal. Modal should list the 5 `AssistantContextBlock*` block names + sample payload sizes + a "what gets sent" expander before the first turn. Drive via a new `cfg.AssistantOutboundConsentShown = false` field. Severity downgraded P1→P2 because the riskiest default (audit-trail PII auto-shipping) is now off.
  Concrete next action: add `cfg.AssistantOutboundConsentShown` (default false); gate `AiAssistantController::RunRequest` on the consent modal first turn; render modal in `SmatchetAiAssistantUi.cpp`. ~3 h UX.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P2 — `AgentsMdLoader` path-traversal: `ProjectAgentsMdPath` / `AgentsMdGlobalPath` accepted verbatim
  Details: A config-write attacker with access to `smatchet_config.json` can repoint either path to any readable file (`C:\Users\<victim>\.ssh\id_rsa`, browser cookies, ssh known_hosts). The first 64 KB are then silently injected into every system prompt sent to the third-party LLM. Loader at [`AgentsMdLoader.cpp:101-117`](../../../Source_Core/src/AgentsMdLoader.cpp) does no validation beyond the 64 KB cap.
  Concrete next action: require the configured path's filename to end in one of `agents.md` / `AGENTS.md` / `.agents.md` (case-insensitive); call `ghc::filesystem::canonical` and reject if the canonical path escapes a small allow-list of roots (`%LOCALAPPDATA%/Smatchet/`, repo root, `%USERPROFILE%`); reject symlinks via `ghc::filesystem::is_symlink`. ~1.5 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P2 — `ai.prompt` Lua glue has no rate limit + no per-session consent toast
  Details: Any Lua script (including one loaded via `Plugins/LuaConsole` paste-and-run) can call `ai.prompt(...)` in a tight loop and burn the user's API quota or leak ticket data to the configured provider. `LuaAutomationHost`'s instruction-count `lua_sethook` doesn't cover the C++-side HTTP call. Sandbox escape with attacker-controlled outbound payload.
  Concrete next action: at the `ai.prompt` C++ glue site in [`AppController_LuaBindings.cpp:776-779`](../../../Source_Core/src/AppController_LuaBindings.cpp), reject calls when an in-flight prompt is already pending OR when the last `ai.prompt` fired less than ~5 s ago. Add a one-time-per-session toast on the first `ai.prompt` call naming the provider host. ~1 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P3 — CR/LF/NUL strip at the config persist site (defense-in-depth)
  Details: PR #176 strips CR/LF/NUL at the use site (`BuildClientConfig` in `AiAssistantController`). For pure defense-in-depth, also strip at the persist site (`ConfigManager::Save`) so a value that round-trips through disk never carries header-smuggling control characters in the first place. Same applies to `MCP config.set` + Lua-config paths that write `AiApiKey` / `AiAnthropicApiKey` / `AiBaseUrl` / `AiOllamaBaseUrl` / `McpAuthToken`.
  Concrete next action: add a single `SanitizeConfigStringValue(...)` helper in `ConfigManager_PathUtils.cpp` (strip `\r`, `\n`, `\0`); call it in `ConfigManager::Save` for every header-bound string field. ~45 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P3 — SSE/NDJSON parse-failure `LOG_WARN` first 200 B unredacted
  Details: When a provider streams malformed JSON, the parse-failure path in `AnthropicClient.cpp:74`, `OpenAiClient.cpp:72`, and `OllamaClient.cpp:133` logs the first 200 bytes of raw `data` / `rawLine`. A misconfigured proxy could echo the request Authorization header in the malformed stream and it would land in logs.
  Concrete next action: route those log-line payloads through `smatchet::ai::pure::RedactProviderErrorBody` before emit. ~15 min, one-line change × 3 files.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [security] · P2 — `tests/support/GoldenImage.h` `std::strtol` parses PPM `w`, `h`, `maxv` without overflow / negative checks (now resolved by PNG migration but the new stb-based reader still warrants a cap)
  Details: Original PPM-P6 parser had no dim caps. The PNG migration (2026-05-17) replaced that reader with stb_image, which has its own `STBI_MAX_DIMENSIONS` (1<<24) plus an additional `kMaxGoldenImageDim = 16384` cap in `GoldenImage.h`. Entry kept open because (a) the cap should also be propagated to the Standalone screenshot writer (currently bounded only by GPU framebuffer size) and (b) a fuzz test against crafted PNG dims is still missing.
  Concrete next action: add a fuzz test in `tests/Source_Core/GoldenImage.test.cpp` (new file) covering crafted PNG dims at / above 16384 and verify the cap rejects them. Estimated cost 30 min once a synthetic crafted-PNG fixture is in place.
  Status: open
  Last-reviewed: 2026-05-17
