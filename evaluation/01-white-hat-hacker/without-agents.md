# Smatchet — Independent White-Hat Security Evaluation (without-agents)

**Assessor:** External white-hat, independent clone-and-audit
**Date:** 2026-06-30
**Target:** Smatchet (C++ desktop/mobile ticket-grid app with MCP server, Lua automation, AI assistant, multi-tracker HTTP clients)
**Method:** Source review only (no build/run). Citations are `file:line`.

---

## 1. Executive Summary + Security-Posture Verdict

Smatchet is a large, security-conscious C++ application that exposes several genuinely dangerous primitives — a loopback HTTP/MCP server, a Lua scripting engine reachable over MCP, an AI assistant that ships an API key to configurable endpoints, and HTTP clients that carry tracker credentials. The notable headline is that **the developers have already done most of the hard defensive work, and done it well.** The MCP server has DNS-rebinding defence (Host/Origin checks), a token-required-on-loopback default, constant-time token comparison, an SSRF allow-list and userinfo rejection on its attachment proxy, and concurrent-SSE caps. The Lua sandbox is a careful whitelist that strips `load`/`loadfile`/`dofile`/`require`/`io`/`package`/`debug`, replaces `os` with a safe time-only table, removes `string.dump`, blocks `setmetatable`/`rawset`, and bounds CPU with an instruction-count hook. Attacker-controlled JSON ingress is routed through a bespoke depth/node-bounded SAX parser (`json_safe::ParseBounded`) that defeats the depth-bomb DOM-teardown crash class. Tracker HTTP clients keep TLS verification on, disable redirect-following to stop credential forwarding, and upgrade cleartext bases. File-touching commands reachable over MCP are confined to per-feature subdirectories with a robust symlink-aware path-confinement helper. Dependencies are pinned to full commit SHAs.

**The most material finding is architectural, not a single broken control:** when the MCP server runs tokenless on loopback with `McpRequireTokenOnLoopback=false`, *any* local process can drive the full command registry and (if `McpAllowLuaExecution=true`) execute arbitrary sandboxed Lua. Both are non-default, but they represent a real local-privilege-boundary widening. Secondary findings are mostly residual: cleartext secret-at-rest on POSIX desktop (documented), a handful of unbounded-DOM `json::parse` calls on *tracker-server* responses, and AI clients that do not thread the Android CA bundle into their TLS options.

I also found that **`SECURITY_AUDIT.md` is partly stale**: at least three of its concrete findings (merge-watch unbounded parse, NDJSON unbounded parse, and "perf.dump writes an attacker-chosen path with no confinement") have since been remediated in the code it cites. Trusting the audit at face value would overstate the live risk.

**Posture verdict: STRONG (above-average for an app of this surface area).** The exploitable gaps require non-default config or a semi-trusted/compromised upstream. I would run this on a single-user workstation with default settings.

---

## 2. Scope & Method (and what I ignored)

**In scope and read:** `Source/Plugins/Mcp/*`, `Source/Plugins/LuaConsole/*`, the Lua binding/sandbox in `Source/Core/src/AppController_LuaBindings*.cpp`, AI clients (`AnthropicClient.cpp`, `OpenAiClient.cpp`, `OllamaClient.cpp`, `AiEndpointPolicy.cpp`, `AiEndpointSanitize.cpp`, `AiContextBuilder.cpp`, `AiNdjsonParser.cpp`), tracker HTTP clients (`TrackerHttpUtils.cpp`, `GitHubClient.cpp`, `PlaneClient.cpp`, etc.), config/secret handling (`ConfigManager*.cpp`), JSON ingress (`Json/BoundedJsonParse.h` + a `json::parse` sweep), Perforce/subprocess (`P4Annotate.cpp`, `SubprocessCapture.cpp`), path confinement (`Commands/PathConfinement.h`), the local notify server, `CMakeLists.txt` (FetchContent pinning), and `THIRD_PARTY_LICENSES.md`. I treated `SECURITY_AUDIT.md` as a claim source to verify, not as ground truth.

**Deliberately ignored (per evaluation rule — evaluate the shipped artifact, not its governance meta-layer):** all `AGENTS.md` files, `agents/`, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`, `.coderabbit.yaml`, and `.cursor/`. These do not change the runtime attack surface a stranger who clones the repo would face.

**Limitation:** static review only. I did not compile or fuzz; "CONFIRMED" below means I traced the code path, not that I ran an exploit.

---

## 3. Attack Surface Map

| Surface | Entry point | Trust boundary | Default exposure |
|---|---|---|---|
| MCP REST + JSON-RPC + SSE | `McpPlugin.cpp` routes on `127.0.0.1:<port>` | Any local process; LAN if `McpAllowRemote` | Off by default; loopback + token-required when on |
| MCP attachment proxy | `GET /mcp/attachment_proxy?url=` | Outbound fetch carrying tracker Basic-auth | Allow-listed to tracker domain |
| `run_lua` over MCP | `tools/call` → `ExecuteLuaSnippetForMcp` | Remote-driven local code exec (sandboxed) | Gated by `McpAllowLuaExecution` (off) |
| Lua automation | console / hooks / background worker | Local user scripts | Local-trust |
| AI assistant | `AnthropicClient`/`OpenAiClient`/`OllamaClient` | API key → configurable endpoint; LLM stream parsing | Pinned host unless custom-endpoint opt-in |
| Tracker clients | Jira/Plane/GitHub via cpr | Tracker credential on wire; response parsing | TLS-verified, redirects disabled |
| Config at rest | `smatchet_config.json` | Local FS | DPAPI (Win) / 0600 cleartext (POSIX) / Keystore (Android) |
| Local notify server | `SmatchetMergeWatchNotifyServer` POST | Any local process | 127.0.0.1 only, bounded parse |
| Perforce / subprocess | `P4Annotate.cpp`, `SubprocessCapture.cpp` | argv exec, no shell | env-scrubbed |
| Supply chain | `CMakeLists.txt` FetchContent | Build-time deps | Full-SHA pinned |

---

## 4. Findings

### F-1. Tokenless loopback MCP + Lua exec exposes full command registry to any local process — **Medium** (High if both non-defaults set)
**File:** `Source/Plugins/Mcp/McpPlugin.cpp:173-196` (Authorize), `:597-616` (run_lua), `Source/Core/include/Commands/Command.h:139-146` (confirm gate).
**Impact:** With `mcp_auth_token` empty AND `mcp_require_token_on_loopback=false`, `Authorize()` returns `true` for any loopback caller (`:195`). Every registered command then becomes invokable by any local process/container sharing the loopback interface — including, if `McpAllowLuaExecution=true`, arbitrary (sandboxed) Lua via `run_lua`. On a shared/multi-user host this crosses a process trust boundary. Destructive mutations still require `__confirm` (`Command.h:139`), and file commands are path-confined (see Strengths), so this is not arbitrary FS read/write — but it is unauthenticated local command execution.
**PoC sketch:** `curl -s 127.0.0.1:<port>/mcp/tools/call -d '{"name":"run_lua","arguments":{"mode":"snippet","code":"return 1"}}'` succeeds from any local process when both toggles are set.
**Mitigating reality:** Secure-by-default — `require_token_on_loopback` defaults ON (`:185-194` returns 401), and Lua exec defaults off. The operator must actively weaken two settings.
**Remediation:** Keep the secure defaults; consider a per-`CommandSource::Mcp` allow-list so even an authenticated MCP caller cannot reach every command, and warn loudly in the UI when `require_token_on_loopback` is disabled.

### F-2. `SECURITY_AUDIT.md` is stale — several cited vulnerabilities are already fixed — **Info / process risk**
**Files:** `SECURITY_AUDIT.md:192,218` vs `Source/Core/src/SmatchetMergeWatchNotifyServer.cpp:82` and `Source/Core/src/AiNdjsonParser.cpp:23`; `SECURITY_AUDIT.md:42,470` vs `Source/Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp:139-150`.
**Impact:** The audit asserts the merge-watch notify handler and the NDJSON parser call `nlohmann::json::parse` unbounded; both now call `smatchet::json_safe::ParseBounded` (notify `:82`, NDJSON `:23`). The audit asserts `perf.dump` writes an attacker-chosen path "with no path confinement"; the code confines it via `ConfinePathUnderSubdir(...,"perf",...)` (`:145`). A reader trusting the audit would over-state live risk and possibly "re-fix" already-fixed code.
**Remediation:** Re-date the audit, mark remediated findings RESOLVED with the fixing commit, and add a verification timestamp.

### F-3. Unbounded-DOM `json::parse` on tracker/LLM HTTP responses — **Medium**
**Files (sample):** `Source/Core/src/Tracker/GitHubClient.cpp:137,639`; `Source/Core/src/Tracker/PlaneIssueMutation.cpp:114,326,350`; `Source/Core/src/Tracker/JiraActivityFeed.cpp:223`; `Source/Core/src/Diagnostics/BugReportService.cpp:239,298,420`; `Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp:147,223`.
**Impact:** These parse `resp.text` from the configured tracker (and GitHub) with a bare `json::parse`. nlohmann's parse is iterative (no parse-time recursion), but the resulting DOM is **destroyed recursively** — a ~1M-deep nested array (which fits in well under the response size) overflows the C++ stack on teardown, an **uncatchable** crash that bypasses the surrounding `try/catch` (the project's own `BoundedJsonParse.h:7-14` documents exactly this class). Threat model: a malicious/compromised tracker, a MITM on a misconfigured `http://` base, or an SSRF-reachable host set as the tracker base. Severity is Medium (not High) because the source is the *configured* upstream, not an arbitrary attacker, and TLS/redirect hardening reduces MITM exposure.
**PoC sketch:** Compromised tracker returns `{"x":` + `[`×1_500_000 + `]`×1_500_000; client crashes on DOM teardown after `json::parse`.
**Remediation:** Route every `resp.text` parse through `json_safe::ParseBounded` (the project already proved the pattern at the MCP/Lua/notify ingress sites). The `(nullptr,false)` non-throwing variant does **not** help — it still builds the deep DOM.

### F-4. POSIX-desktop secrets stored as cleartext JSON (file-mode only) — **Low** (by design, documented)
**File:** `Source/Core/src/Config/ConfigManager_PathUtils.cpp:369-382` (passthrough `ProtectSecretForConfig`).
**Impact:** On Linux/macOS desktop there is no OS-backed sealing; tracker API tokens, AI API keys, GitHub PATs, and the MCP auth token land as cleartext in `smatchet_config.json`. Confidentiality rests entirely on `0600` file mode (set via `O_NOFOLLOW` + `fchmod` before the atomic rename, `:925-930`), and `LoadJsonFile` warns on group/world-readable files (`:760-769`). Any process running as the same user, a backup that ignores mode, or a misconfigured share leaks every secret. Windows (DPAPI, `:331-347`) and Android (Keystore, fail-closed, `:383-399`) are protected.
**Remediation:** Integrate libsecret/Keychain on POSIX desktop; until then keep the loud permission warning and document the exposure prominently.

### F-5. AI clients do not apply the configured CA bundle (Android TLS seam gap) — **Low**
**File:** `Source/Core/src/AnthropicClient.cpp:141-142,191-193`; `OpenAiClient.cpp:155`; vs `Source/Core/src/Tracker/TrackerHttpUtils.cpp:145-151` (`MakeTrackerSslOptions`).
**Impact:** Tracker verbs thread `MakeTrackerSslOptions()` (which on Android injects the private-dir `cacert.pem`) into every request; the AI clients pass **no** `cpr::SslOptions`, so they rely solely on libcurl's default CA store. On desktop this is correct (system store, verification on). On Android — where the whole point of the CA seam is that the default store may be unavailable — AI TLS connections are not guaranteed to use the bundled CA, risking either failure or, worst case, weaker trust resolution than the tracker path. No evidence verification is *disabled*; this is an inconsistency, not a confirmed bypass.
**Remediation:** Add a `MakeAiSslOptions()` mirroring the tracker seam and pass it to every AI `cpr::Get/Head/Post`.

### F-6. AI custom-endpoint sends the provider API key to an operator-chosen host; policy enforced only at prefs-validation time — **Low**
**File:** `Source/Core/src/AiEndpointPolicy.cpp:8-24`; key attached in `AnthropicClient.cpp:163-164`, `OpenAiClient.cpp` send path.
**Impact:** When `AiAllowCustomEndpoint{Anthropic,OpenAi}=true`, the host pin (`api.anthropic.com`/`api.openai.com`) is relaxed and the `x-api-key` is sent to whatever `ResolveBaseUrl(cfg)` yields. Endpoint policy/SSRF evaluation (`AiEndpointSanitize.cpp`, `AiPrefsValidator.cpp`) runs at preferences-validation time, not at each send; the send path trusts the stored config. This is an operator-controlled config knob (not remote-reachable), so Low — but a poisoned config or a typo'd endpoint silently exfiltrates the key. Loopback/Ollama allowances are gated by `EndpointVerdict` consent in the sanitizer.
**Remediation:** Re-assert the endpoint verdict at send time, not just at config save.

### F-7. P4 / subprocess argument-injection surface (no shell, but leading-dash args) — **Low / Info**
**File:** `Source/Core/src/P4Annotate.cpp:102-118,189-237`; `Source/Core/src/SubprocessCapture.cpp:114-118,400`.
**Impact:** All process spawns use argv-vector exec (`CreateProcessW` / `execv`), so classic shell metacharacter injection is **not** possible. A depot/file path argument is passed as a discrete argv element; a value beginning with `-` could be interpreted as a `p4` flag (argument injection), but the inputs are user/tracker-derived, not a clean attacker channel, and the parent env is scrubbed of secrets before spawn (`P4Annotate.cpp:56-59`). `SubprocessCapture` also resolves bare exe names to absolute paths to defeat PATH planting (`:114-118`). Mostly informational.
**Remediation:** Prefix path args with `--` where the tool supports end-of-options, or validate that path args do not start with `-`.

### F-8. MCP `0.0.0.0` mode drops the Host/Origin DNS-rebind defence — **Low** (operator intent)
**File:** `Source/Plugins/Mcp/McpPlugin.cpp:162-172`.
**Impact:** The Host/Origin DNS-rebinding check only runs when bound to loopback; with `McpAllowRemote=true` (bind `0.0.0.0`) it is skipped, leaving only the bearer token. This is documented as deliberate (a non-loopback Host is "the operator's explicit intent"), and the token check (`:197-205`, constant-time) still applies. The residual risk is operator-driven LAN exposure.
**Remediation:** Even in remote mode, require a non-empty token (already true) and consider a configurable Host allow-list.

---

## 5. Strengths (the defensive engineering is the headline)

- **MCP DNS-rebinding + auth hardening:** loopback Host/Origin validation (`McpPlugin.cpp:162-172`, `McpJsonRpcPure.cpp:230-266`), constant-time token compare (`:268-277`), token-required-on-loopback secure default (`:185-194`), ephemeral spawn-token handshake that is scrubbed from the env after adoption (`McpPlugin.cpp:234-248`).
- **Attachment-proxy SSRF defence:** https-only, userinfo rejected (`McpPlugin.cpp:385-389`), host allow-list keyed to the tracker domain plus `api.media.atlassian.com` (`McpJsonRpcPure.cpp:279-292`), redirects disabled, 10 MiB cap, 8 KiB URL cap.
- **Lua sandbox:** `os` replaced by a safe whitelist (`AppController_LuaBindingsCore.cpp:239-260`), and `load`/`loadfile`/`dofile`/`loadstring`/`require`/`io`/`package`/`debug`/`setmetatable`/`rawset`/`string.dump` all blocked (`AppController_LuaBindings.cpp:241-288`); fresh per-call state on worker threads; instruction-count hook bounds CPU (`AppController_LuaBindings_detail.h:69-80`); script path traversal blocked (`AppController.cpp:2416-2425`).
- **Bounded JSON ingress:** `json_safe::ParseBounded` drives nlohmann's SAX builder and aborts on depth/node caps *before* a deep DOM exists — defeats the recursive-teardown crash class for the genuinely attacker-controlled paths (MCP REST/JSON-RPC, `decode_json`, notify server, NDJSON). (`BoundedJsonParse.h`).
- **Tracker TLS/credential hygiene:** verification always on, redirect-following disabled to prevent Authorization forwarding on 30x (`TrackerHttpUtils.cpp:153-163`), cleartext-base upgrade with warning (`:104-108`).
- **Path confinement:** symlink-aware, canonical-containment, `..`/absolute rejection for MCP-reachable file writes (`PathConfinement.h`).
- **Supply chain:** all FetchContent deps pinned to full commit SHAs (`CMakeLists.txt:511-1018`), with `THIRD_PARTY_LICENSES.md` present.
- **Secret-at-rest:** DPAPI (Windows) and Android Keystore with fail-closed drop-rather-than-leak behaviour.

---

## 6. Scorecard

| Dimension | Score /10 | Notes |
|---|---|---|
| Input validation | 9 | Bounded SAX parse on attacker ingress; residual unbounded DOM on tracker responses (F-3). |
| Secrets / credential handling | 7 | DPAPI + Keystore strong; POSIX-desktop cleartext (mode-only) drags it down (F-4). |
| Network / TLS | 8 | Verify-on, redirects-off, cleartext upgrade; AI clients miss the Android CA seam (F-5). |
| Sandbox isolation | 9 | Thorough Lua whitelist + CPU bound; only weakened by an explicit config opt-in (F-1). |
| Dependency hygiene | 9 | Full-SHA pinning; no obvious vendored-and-forgotten CVE surface found in review. |
| Attack-surface minimization | 8 | MCP/Lua-exec off by default, confirm-gated mutations; tokenless+remote knobs exist (F-1/F-8). |
| Security testing | 8 | Dedicated bounded-parse/path-confinement tests, fuzz dir; audit doc is stale (F-2). |
| **Overall** | **8/10** | Mature, defense-in-depth posture; remaining gaps need non-default config or a compromised upstream. |

---

## 7. Prioritized Recommendations

1. **(F-3, Medium)** Route all `resp.text` JSON parses in `Source/Core/src/Tracker/**` and `Diagnostics/BugReportService.cpp` through `json_safe::ParseBounded`. Highest payoff-to-effort: closes the last remote-crash class against a hostile/MITM'd upstream using a pattern already in the tree.
2. **(F-1, Medium)** Add a per-`CommandSource::Mcp` command allow-list (or capability tags) so authenticated MCP callers cannot reach the *entire* registry; surface a prominent warning when `mcp_require_token_on_loopback=false`.
3. **(F-2, Info)** Refresh `SECURITY_AUDIT.md`: mark merge-watch, NDJSON, and perf.dump findings RESOLVED with fixing commits; add re-verification dates so the doc stops over-reporting risk.
4. **(F-5, Low)** Thread a `MakeAiSslOptions()` (mirroring the tracker CA seam) into every AI client request for Android parity.
5. **(F-4, Low)** Add libsecret/Keychain backing for POSIX-desktop secrets; until then keep the world-readable warning loud.
6. **(F-6, Low)** Re-evaluate the AI endpoint verdict at send time, not only at preferences save.

---

## 8. Bottom Line — would I trust running this?

**Yes, on a single-user workstation with default settings — with reservations on shared/multi-user hosts.** Smatchet demonstrates security engineering well above the norm for an application of this surface area: the dangerous primitives (loopback server, embedded scripting, credential-bearing HTTP, AI key handling) are each wrapped in deliberate, layered controls, and the defaults are safe (MCP off; token required on loopback; Lua exec off; mutations confirm-gated; TLS verified; redirects disabled; deps SHA-pinned). The exploitable paths I found require either an explicit weakening of two config toggles (F-1), a compromised/MITM'd tracker upstream (F-3), or local same-user access (F-4) — none are a default-config remote compromise.

My two cautions: (1) **the residual tracker-response `json::parse` calls (F-3) are the one live remote-crash class still open** and should be closed with the project's own bounded parser; (2) **do not treat `SECURITY_AUDIT.md` as current** — it materially overstates risk that the code has already fixed. Net: a codebase I would trust to run, and one whose security posture I would rate **8/10**.
