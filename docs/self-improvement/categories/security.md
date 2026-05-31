# Agent self-improvement — security

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-30 · log-a-bug-github · [security] · P1 — bug-reporter must NOT ship a GitHub token if Smatchet is distributed to external/untrusted users
  Details: The "Log a Bug" feature (`docs/plans/shipped/log-a-bug-github.md`, shipped) files issues to ONE fixed dev GitHub repo. The PAT resolves from env `SMATCHET_BUGREPORT_GITHUB_TOKEN` → opt-in persisted `cfg.BugReportGitHubPat` (DPAPI-encrypted, default off) — `Source/Core/src/Diagnostics/BugReportBody.cpp` `ResolveBugReportTarget`. The tracker `cfg.GitHubPat` is **intentionally never** borrowed (separate personal credential; user directive 2026-05-30). This is correct for **internal/dev** distribution (each user has their own `issues:write` token). It is **unsafe for external distribution**: a fixed dev repo requires every reporter to have write access, so the only way to let untrusted users file is a **shipped token** (credential leak — anyone can extract it and spam/abuse the repo) OR a **server-side relay/bot** that accepts anonymous reports and re-files under a service account. Smatchet must NOT embed a token in the binary. Confirmed ship-blocker for the external-distribution milestone (user decision 2026-05-30: "needs relay").
  Concrete next action: before any external/public release, build a minimal relay endpoint (accepts redacted report JSON + optional screenshot, re-files via a server-held PAT, rate-limited + abuse-guarded) and point the app at it; until then keep the feature internal-only and never bundle a PAT. ~1–2 d.
  Status: **RESOLVED — relay deployed + verified live 2026-05-30.** Relay at `tools/bug-report-relay/` (Cloudflare Worker; holds the token server-side, creates the issue + uploads screenshots/minidumps as Release assets, `x-relay-key` gate, 2 MB cap) is deployed at `smatchet-bug-report-relay.smatchet.workers.dev` and verified end-to-end (issue `alexandrosk0/Smatchet#585`; 401 gate + 404 diagnosis exercised). App relay-mode wired (`bugreport_relay_url`/`bugreport_relay_key`; `ResolveBugReportTarget` → `UseRelay`; no client token). The binary bundles **no** GitHub token. Residual (operational, not a code blocker): add a Cloudflare rate-limit rule on the route, and ensure the relay's `GITHUB_TOKEN` has `contents:write` (inline screenshots) + release perms (crash dumps). Safe to archive once an external build ships pointed at the relay.
  Last-reviewed: 2026-05-30

- 2026-05-28 · deep-audit · [security] · P2 — JQL injection via unescaped issue keys in `JiraClient::FetchIssuesForKeys`
  Details: `Source/Core/src/Tracker/JiraIssueSearch.cpp:470-487` interpolates issue keys into double-quoted JQL literals with no escaping of `"` or `\` — single-key path builds `jql = "key = \"" + keys[offset] + "\"";` (:472) and the IN-list appends raw `'"' + keys[offset+i] + '"'` (:479-481). `UrlEncode` (`TrackerHttpUtils.cpp:44-59`) only percent-encodes for transport; the Jira server URL-decodes before JQL parsing, so an embedded `"` reaches the parser intact and breaks out of the quoted literal (e.g. `FOO" OR project=SECRET OR key="BAR`), widening the query beyond the intended key set (cross-project disclosure). Callers: `AppController.cpp:514`, `Source/Core/src/Sync/OfflineQueueService.cpp:714` (offline-queue restore). Keys are mostly server-issued today, so this is defense-in-depth / fragility rather than currently-exploitable. Verified from real code (deep-audit, adversarially confirmed).
  Concrete next action: add a `JqlQuoteLiteral()` helper in `TrackerHttpUtils` (escape `\` → `\\` then `"` → `\"`) OR validate keys against the `[A-Z][A-Z0-9]*-[0-9]+` grammar and skip non-matches before building the IN-list; unit-test it. Audit `PlaneIssueSearch` / GitHub equivalents the same way. ~1 h.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-28 · deep-audit · [security] · P3 — Tracker HTTP clients follow redirects with `Authorization` attached (cross-host credential forwarding)
  Details: All tracker request helpers construct `cpr::Redirect redirect(true, true)` (`Source/Core/src/Tracker/TrackerHttpUtils.cpp:118,131,143,154,242`) while `BuildTrackerHeaders` attaches a Basic `Authorization` header (`BuildTrackerBasicAuthHeader`, :108-110). Because that header is a caller-set raw header (not libcurl `CURLOPT_USERPWD`), libcurl's default `CURLOPT_UNRESTRICTED_AUTH=0` does NOT strip it on cross-host redirects — a 30x from the configured tracker domain to an attacker/MITM host forwards the API token. The MCP attachment proxy already defends this with `cpr::Redirect(false,false)` (`Source/Plugins/Mcp/McpPlugin.cpp:289`); the tracker clients do not. Low severity: base Domain is user-configured (self-targeting trust boundary) and Jira/Plane/GitHub Cloud are HTTPS without cross-host auth redirects — residual risk is a compromised endpoint or an `http://` MITM. Verified (deep-audit, adversarially confirmed).
  Concrete next action: disable redirect-following on the tracker helpers (`cpr::Redirect(false, ...)`) and handle same-host redirects explicitly, OR restrict follow to same host/scheme, OR strip `Authorization` on cross-origin redirects. Mirror the proxy's posture. ~1 h.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-28 · deep-audit · [security] · P3 — Lua source tarball fetched with no integrity hash (only unpinned external fetch)
  Details: `CMakeLists.txt:377` `file(DOWNLOAD https://www.lua.org/ftp/lua-5.3.6.tar.gz "${_lua_tar}")` has no `EXPECTED_HASH` (grep for EXPECTED_HASH/SHA256 across `CMakeLists.txt` + `cmake/` returns nothing). Every other dependency is pinned to an immutable git ref and FontAwesome's TTF is sha256-verified in CI (`build-and-test.yml:88-98`) — Lua is the lone gap. A compromised lua.org mirror or MITM injects unverified C source compiled into both standalone + Unreal targets. Inside `if(SMATCHET_WITH_LUA_AUTOMATION)` + guarded by `if(NOT EXISTS LUA_SRC_DIR)`, so the window is first-fetch / cache-miss CI runs. Mirrors the existing Mesa-archive-integrity entry (2026-05-24). Verified (deep-audit, adversarially confirmed).
  Concrete next action: add `EXPECTED_HASH SHA256=<hash of lua-5.3.6.tar.gz>` to the `file(DOWNLOAD)` call (CMake supports it directly). One line. ~15 min.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-17 · security-review · [security] · P1 — AI-client URL allow-list policy (per-provider host opt-in beyond the IP-literal block shipped in PR #176)
  Details: PR #176 added `AiEndpointSanitize` covering non-http(s) schemes, CR/LF/NUL, 169.254.169.254 / 100.100.100.200 metadata IPs, and 169.254/16 link-local. It does NOT enforce per-provider host allow-lists — a config-write attacker can still repoint OpenAi / Anthropic at any public host (e.g. attacker.example.com) and exfil the API key in the Authorization / x-api-key header. The validator deliberately chose breadth over strictness so Azure OpenAI / LiteLLM / openrouter proxies still work, but the right long-term shape is an explicit "custom endpoint" toggle per provider (default off → host must match `api.openai.com` / `api.anthropic.com`; explicit on → any host accepted with a one-time consent dialog naming the host). **Related (deep-audit 2026-05-28):** the same validator also accepts plain `http://` with no warning — `SanitizeAiEndpointUrl` only rejects non-`{http,https}` schemes (`AiEndpointSanitize.cpp:126`), so a config-set or typo'd `http://` cloud endpoint sends the API key in cleartext (compounds the tracker redirect-forwarding entry above). Plain `http://` is legitimately needed for local Ollama (`localhost:11434`), so the fix is to warn / require consent on `http://` to a NON-loopback host — folds into the per-provider allow-list + consent work below. Verified (deep-audit, adversarially confirmed at AiEndpointSanitize.cpp:126 + BuildClientConfig key-attach).
  Concrete next action: extend `ConfigManager` with `AiAllowCustomEndpointOpenAi` + `AiAllowCustomEndpointAnthropic` bool fields (default false); extend `AiEndpointSanitize` with a new `EndpointVerdict::RejectedNonProviderHost` and a per-provider allow-list parameter; surface a one-time consent dialog in `SmatchetPreferencesUi` when the user enables either toggle, AND gate `http://` to a non-loopback host behind the same consent (loopback / `localhost` stays silent for Ollama). Estimated cost 2-3 h.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-17 · security-review · [security] · P2 — First-send outbound-context consent modal (consent-tracking field + UX)
  Details: Default flip of `AssistantContextBlockAuditTrail` to `false` shipped (`ConfigManager.h:248`); remaining work from the original P1 entry is the one-time first-send consent modal. Modal should list the 5 `AssistantContextBlock*` block names + sample payload sizes + a "what gets sent" expander before the first turn. Drive via a new `cfg.AssistantOutboundConsentShown = false` field. Severity downgraded P1→P2 because the riskiest default (audit-trail PII auto-shipping) is now off.
  Concrete next action: add `cfg.AssistantOutboundConsentShown` (default false); gate `AiAssistantController::RunRequest` on the consent modal first turn; render modal in `SmatchetAiAssistantUi.cpp`. ~3 h UX.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P2 — `AgentsMdLoader` path-traversal: `ProjectAgentsMdPath` / `AgentsMdGlobalPath` accepted verbatim
  Details: A config-write attacker with access to `smatchet_config.json` can repoint either path to any readable file (`C:\Users\<victim>\.ssh\id_rsa`, browser cookies, ssh known_hosts). The first 64 KB are then silently injected into every system prompt sent to the third-party LLM. Loader at [`AgentsMdLoader.cpp:101-117`](../../../Source/Core/src/AgentsMdLoader.cpp) does no validation beyond the 64 KB cap.
  Concrete next action: require the configured path's filename to end in one of `agents.md` / `AGENTS.md` / `.agents.md` (case-insensitive); call `ghc::filesystem::canonical` and reject if the canonical path escapes a small allow-list of roots (`%LOCALAPPDATA%/Smatchet/`, repo root, `%USERPROFILE%`); reject symlinks via `ghc::filesystem::is_symlink`. ~1.5 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P2 — `ai.prompt` Lua glue has no rate limit + no per-session consent toast
  Details: Any Lua script (including one loaded via `Source/Plugins/LuaConsole` paste-and-run) can call `ai.prompt(...)` in a tight loop and burn the user's API quota or leak ticket data to the configured provider. `LuaAutomationHost`'s instruction-count `lua_sethook` doesn't cover the C++-side HTTP call. Sandbox escape with attacker-controlled outbound payload.
  Concrete next action: at the `ai.prompt` C++ glue site in [`AppController_LuaBindings.cpp:776-779`](../../../Source/Core/src/AppController_LuaBindings.cpp), reject calls when an in-flight prompt is already pending OR when the last `ai.prompt` fired less than ~5 s ago. Add a one-time-per-session toast on the first `ai.prompt` call naming the provider host. ~1 h.
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
  Concrete next action: add a fuzz test in `tests/Core/GoldenImage.test.cpp` (new file) covering crafted PNG dims at / above 16384 and verify the cap rejects them. Estimated cost 30 min once a synthetic crafted-PNG fixture is in place.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-24 · coderabbit-triage · [security] · P3 — CI: pin all `uses:` action refs to commit SHAs + enable Dependabot
  Source: CodeRabbit on PR #441 thread `PRRT_kwDORqx0G86EYIXI` (deferred — repo-wide sweep, not slice-6 scope).
  Details: `.github/workflows/build-and-test.yml` has 13 `uses:` sites across 5 jobs using floating `@v*` tags (`actions/checkout@v4` ×3, `msys2/setup-msys2@v2` ×3, `actions/cache@v4` ×3, `actions/upload-artifact@v4` ×3, `actions/download-artifact@v4` ×1). Only ~half are slice-6-introduced; pinning a subset leaves the workflow inconsistent and breaks the zizmor `unpinned-uses` blanket policy.
  Concrete next action: 1 small PR — pin all 9 sites to commit SHAs + add `.github/dependabot.yml` (`package-ecosystem: github-actions`, weekly cadence) so SHAs stay current. Audit any other workflows under `.github/workflows/` for the same pattern in the same PR. Estimated cost ~30 min.
  Status: open
  Last-reviewed: 2026-05-24

- 2026-05-24 · coderabbit-triage · [security] · P3 — CI: workflow-level GITHUB_TOKEN `permissions: {}` + per-job least-privilege
  Source: CodeRabbit on PR #441 thread `PRRT_kwDORqx0G86EYIXJ` (CR thread outdated; deferred to dedicated security PR).
  Details: `.github/workflows/build-and-test.yml` has no `permissions:` block; `GITHUB_TOKEN` inherits the repo default (often `read-write-all` for forks → contents:write). `bucket-c-screenshot-diff` and `bucket-e-ui-tests` upload/download artefacts + curl external binaries — should be locked down.
  Concrete next action: add workflow-root `permissions: contents: read`. Override per-job: `bucket-c-screenshot-diff` and `bucket-e-ui-tests` need `actions: write` (upload-artifact) + `contents: read`. `windows-msys2-ucrt64*` jobs only need `contents: read`. Pair with the action-SHA-pinning entry above — same file, same reviewer concern, batch as one security PR.
  Status: open
  Last-reviewed: 2026-05-24

- 2026-05-24 · coderabbit-triage · [security] · P2 — CI: Mesa archive integrity verification (upstream publishes no checksum)
  Source: CodeRabbit on PR #441 thread `PRRT_kwDORqx0G86EYIXK`. Live in `.github/workflows/build-and-test.yml:302,395` (slice-6 introduction).
  Details: `bucket-c-screenshot-diff` + `bucket-e-ui-tests` jobs `curl` a 72 MB `mesa-3d-*.7z` from the `pal1000/mesa-dist-win` GitHub release with no SHA256 / signature check. Verified via `gh release view 24.2.5 --json assets` that upstream ships zero checksums: `digest: null` on every asset, no `.sha256` companion file, no checksum in the release body. CR's suggested `MESA_SHA256: "<published-sha256>"` literally cannot be filled with a publisher-attested value. Triage-mechanical-fix envelope insufficient.
  Concrete next action: security PR must choose between (a) self-computed TOFU SHA256 pinned in workflow env (mitigates silent upstream tampering, not first-time-trust); (b) mirror the 7z to repo-controlled storage (release asset / LFS / private S3); (c) switch to a Mesa distribution that publishes signed artefacts (cosign-attested builds). Pair with the two entries above as one security PR. **P2** — supply-chain risk on every CI run, but exploit window narrow (public-repo CI, no secrets touched, output is a screenshot diff).
  Status: open
  Last-reviewed: 2026-05-24
