# Smatchet security audit — 2026-06-13

**Scope:** the whole application, all build targets, all configurations and trust
boundaries.
**Method:** two adversarially-verified multi-agent audit fleets (an 11-lane
deep-audit fleet, plus a 5-lane re-run that covered the lanes the first fleet
left stalled), reconciled against the standing security backlog (E1–E11 in
[`docs/self-improvement/categories/security.md`](../self-improvement/categories/security.md)).
**Result:** **0 CRITICAL, 5 HIGH, ~14 MEDIUM, ~16 LOW, 7 INFO** after
adversarial verification. Two findings were refuted outright and six were
downgraded to false-positive/INFO on verification (documented below for
transparency).

The actionable, deduped, per-finding backlog lives at the top of
[`docs/self-improvement/categories/security.md`](../self-improvement/categories/security.md);
this document is the analysis, threat-model framing, dedup map, and coverage
record behind it.

---

## 1. Threat model

Smatchet is a **single-user, local-first** desktop issue-tracker client.
**Windows is the only shipping platform**; the POSIX/Linux and Android branches
are compile-only portability targets today (`SMATCHET_BUILD_POSIX_CORE_CHECK`,
`ninja-tsan-linux`, the mobile CI smoke lane) and not user-facing releases. The
three audited targets are:

1. **Standalone** — GLFW / OpenGL desktop host (the shipping product).
2. **Unreal embed** — DX12, `SMATCHET_EMBEDDED_IN_UNREAL`.
3. **Android mobile** — native host + Gradle/JNI bridge (pre-release).

**Same-user code is inside the trust boundary by design.** Any process already
running as the user can read the user's own `%APPDATA%`, decrypt DPAPI
user-scope secrets, and plant binaries on the user's `PATH`. This is why a
number of findings that would be HIGH on a multi-tenant server (DPAPI no
entropy, PATH-resolved `p4`, world-readable per-user files, inherited
environment) are correctly **LOW/INFO** here. The boundaries that *do* carry
weight are:

- **Config-write attacker** — anything that can edit `smatchet_config.json`
  (the largest real lever: it repoints file-read paths, executable names, and
  command templates that later feed prompts/subprocesses).
- **Malicious tracker server / MITM** — server-controlled JSON and HTTP
  responses reaching parsers, query builders, redirect handlers, and logs.
- **Paste-and-run Lua** — untrusted scripts pasted into the Lua console or
  pushed over MCP `run_lua`.
- **Local MCP clients** — loopback HTTP callers (and rebound browser origins).
- **CI / supply chain** — third-party artifact fetch and action pinning.
- **Non-Windows at-rest** — where the platform crypto shim degrades to
  plaintext (relevant the moment those targets ship).

---

## 2. Methodology

| | Fleet A (deep-audit) | Fleet B (5-lane re-run) |
|---|---|---|
| Lanes | 11 (ai-outbound, ai-clients, mcp, lua, config-secrets, subprocess-vcs, build-ci-supplychain, standalone-target, mobile-target, imaging-parsing, logging-privacy-telemetry) | 5 (lua-sandbox, config-at-rest, subprocess-exec, tracker-transport-parse, logs-audit) |
| Why | Full breadth across targets + trust boundaries | The first fleet's tracker-transport and persistence/SQLite lanes stalled; the re-run gave them dedicated, read-disciplined coverage |
| Verify | Refute-default adversarial skeptic per finding | Same — refute-default verifier per finding, with `adjustedSeverity` |
| Outcome | 41 live findings, 1 refuted (`liveCount=41, refutedCount=1`) | per-finding `confirmed / partly-confirmed / refuted / false-positive` verdicts |

Every finding in this report **survived an adversarial verifier** whose default
verdict was "refuted unless the exact cited lines prove it." Severities here are
the **post-verify** severities, not the auditors' first-pass claims — several
were moved up (`os.date`→INFO refuted, MEDIUM→LOW for PATH/SSRF-literal) or down
on verification. The two fleets are **complementary, not redundant**: Fleet A's
stalled lanes (tracker HTTP transport, persistence/SQLite at-rest) are exactly
what Fleet B's `tracker-transport-parse` and `config-at-rest` lanes covered, so
together they give full coverage with cross-checking on the overlap (JQL,
redirect-forwarding, plaintext-at-rest all independently confirmed by both).

---

## 3. Findings by severity (post-verify)

### CRITICAL — none.

No finding reached CRITICAL after verification. The candidate that looked
CRITICAL on first pass — tracker redirect credential-forwarding — was adjusted
to HIGH because the base domain is user-configured (self-targeting boundary) and
the live cloud endpoints are HTTPS without cross-host auth redirects.

### HIGH (5)

| # | Finding | Location | Why it matters | Fix | Backlog |
|---|---|---|---|---|---|
| H1 | **Arbitrary config-specified file read prepended verbatim into the outbound LLM system prompt** | `AgentsMdLoader.cpp:28-58,122-153` → prompt assembled at `AiAssistantController.cpp:408,418`; path guarded only by `fs::exists` (`ConfigManager.cpp:791-792`) | A config-write attacker repoints `ProjectAgentsMdPath`/`AgentsMdGlobalPath` to any readable file (`id_rsa`, cookies, `known_hosts`); the first 64 KB is silently injected into every system prompt to the third-party LLM → **off-host exfil of arbitrary local files**. No canonicalization / allow-list / root-containment. | Path-containment helper: canonicalize, reject symlinks + out-of-root absolute paths, pin the filename suffix to `*agents.md`; treat override content as untrusted in prompt assembly. | **P1** (supersedes **E5**, raised P2→P1) |
| H2 | **Secrets stored plaintext at rest on every non-Windows target (POSIX + Android)** | `ConfigManager_PathUtils.cpp:331-334` (`#else { return plainText; }`); sink writes `token`/`plane_api_key`/`github_pat`/`mcp_auth_token`/`ai_*_api_key`/`whisper_api_key` raw at `ConfigManager.cpp:473-496` | `ProtectSecretForConfig` is DPAPI only under `_WIN32`; the `#else` branch is a no-op, so all API tokens land as cleartext JSON with default umask, no `chmod 0600`, no Android Keystore. Confirmed **HIGH** by the re-run. Windows (the shipping target) is unaffected. | Per-platform: `chmod 0600` + verify-mode-on-read on POSIX; back Android secrets with the Keystore (or mark the platform unsupported for secret storage). Document the crypto gap. | **P2** today, **raise to P1 the moment POSIX/Android ships** (synthesis #7+#8, re-run upgrade) |
| H3 | **Server-supplied AccountId injected unescaped into JQL** | `JqlSuggestEngine.cpp:139-140` — `BuildJqlUserInsert`, the AccountId fallback path does no quote-escape | A malicious/compromised tracker returns an AccountId containing `"`; it breaks out of the quoted JQL literal in the autocomplete query (distinct from the issue-key path E1/`BuildKeyInJql`). Server-controlled data → query manipulation. Confirmed **HIGH**, NEW. | Quote-escape (`\` → `\\`, `"` → `\"`) or validate AccountId against its grammar before insertion; reuse the `JqlQuoteLiteral()` helper from E1. | **P1** (NEW) |
| H4 | **Tracker HTTP clients follow redirects with `Authorization` attached (cross-host credential forwarding)** | `TrackerHttpUtils.cpp:140,152,169,180,268` — `cpr::Redirect(true,true)` while a raw Basic/Bearer header is caller-set, so libcurl's `CURLOPT_UNRESTRICTED_AUTH=0` default does **not** strip it on cross-host 30x | A 30x from the configured tracker host to an attacker/MITM host forwards the API token. The MCP attachment proxy already defends with `Redirect(false,false)` (`McpPlugin.cpp:289`); the tracker clients do not. Confirmed **HIGH** by **both** fleets (CRITICAL→HIGH on the self-targeting boundary). | Disable redirect-following on the tracker helpers, or restrict to same host+scheme, or strip `Authorization` on cross-origin redirect. Mirror the proxy. | **E2**, raise P3→P2 |
| H5 | **`ai.prompt` Lua glue has no rate limit and no per-session consent** | `AppController_LuaBindings.cpp:776-779` (binding); the instruction-count `lua_sethook` does not cover the C++-side HTTP call | Any paste-and-run Lua can call `ai.prompt(...)` in a tight loop to burn the API quota or stream ticket data to the provider — a sandbox escape with attacker-controlled outbound payload. Confirmed **HIGH** by the re-run (SEC-AUDIT-21 also confirms existing). | Reject re-entrant / <5 s-spaced calls at the C++ glue site; one-time-per-session toast naming the provider host. | **E6**, re-confirmed HIGH (hold P2 — exploit needs local paste-and-run) |

### MEDIUM (deduped)

| Finding | Location | Note | Backlog |
|---|---|---|---|
| **ADF parser unbounded recursion** (Pillar 3 — Never Crash) | `TrackerFieldValueParser.cpp:290` (`CollectAdfText`), `:309` (`ExtractAdfTextToStream`) | Server JSON with deeply-nested ADF blows the stack — untrusted-response DoS / crash. Confirmed MEDIUM, NEW. | NEW P2 |
| **P4vLaunch argument injection** | `P4vLaunch.cpp:72-86,149,172,189-190`; `P4Annotate.cpp:52-59`; custom-command `{file}`/`{cl}` templates gated by `AnnotateAllowCustomCommands` | `QuoteWinArgWide` mishandles trailing backslashes → argument-boundary injection from changelist/file fields. Confirmed MEDIUM. | synthesis #6 |
| **Automation hook → shutdown deadlock / UI-thread starvation** | hook at `AppController_LuaBindings.cpp:1257` (`LUA_MASKCOUNT, 50000`, only checks `shuttingDown_`) chaining to blocking `JiraIssueMutation.cpp:206` | Long automation holds the process from exiting and can block the UI thread via blocking C++ glue the count-hook doesn't cover (Pillar 2). Verifier raised LOW→MEDIUM. | synthesis #13 (re-run sharpened) |
| **Command registry runs destructive commands with no `ctx.Source` authz** | `CommandRegistry.cpp:264,298` — gates only on `Destructive && !ConfirmedDestructive` | MCP/Lua-sourced commands have the same reach as UI-sourced; no per-source trust gate. | synthesis #2 |
| **MCP registry dispatch un-gated after Authorize** | `McpPlugin.cpp:426-445,625-642` | After loopback+token Authorize, dispatch reaches the full command registry — token possession == full reach. | synthesis #3 |
| **MCP server performs no Host/Origin check (DNS-rebinding)** | `McpPlugin.cpp:137-161` | Authorize validates loopback+token but not Host/Origin; a rebound browser origin reaches the loopback port with only the token as barrier. | synthesis #4 |
| **MCP attachment proxy fetches caller-supplied URLs (SSRF surface)** | `McpPlugin.cpp:275-352` | The mcp-lane coverage found it already HTTPS-only + host-allow-listed (tracker domain + `api.media.atlassian.com`) with redirects disabled; this entry is a confirm-it-routes-through-the-shared-sanitizer hardening. | synthesis #5 |
| **stb image decode dimension cap applied *after* allocation** | `SmatchetImageTextureCache.cpp:141,149` | Oversized images allocate before the max-dim check rejects them — memory-pressure DoS. Pre-validate via `stbi_info`. | synthesis #9 |
| **OfflineQueue serialized `draft` string bypasses audit redaction** | `OfflineQueueService.cpp:356,362` serialize to a JSON string before `BackendAuditTrail.cpp:124-148` redaction runs | `RedactJson`/`LooksSensitiveKey` never see the nested keys. Redact the draft structurally pre-serialization. | synthesis #10 |
| **AI client redirect can forward `x-api-key` cross-host** | AI-client redirect config (distinct from the tracker-scoped H4) | Strip auth headers on cross-origin redirect for all AI clients. | synthesis #11 |
| **Error/response bodies logged without key-name redaction** | a backend-client error-logging site (distinct from E8) | Emits response/error bodies without `RedactJson` → reflected tokens to logs. | synthesis #12 |
| **First-send outbound-context consent modal missing** | `AiAssistantController::RunRequest`; drive via `cfg.AssistantOutboundConsentShown` | Re-confirmed by SEC-AUDIT-22. | **E4** |

### LOW (deduped — selected; full set in the backlog)

| Finding | Location | Backlog |
|---|---|---|
| Lua child coroutine `lua_State` does not inherit the instruction-count hook | `AppController_LuaBindings.cpp:315,1257` | NEW P3 |
| SQLite local ticket cache stored unencrypted | `LocalCacheManager.cpp:131` | NEW P3 |
| Whisper model download redirect: no host-pin, no size cap | `ModelDownloader.cpp:314` (`cpr::Redirect(true,true)`) | NEW P3 |
| `NormalizeBaseUrl` accepts cleartext `http://` | `TrackerHttpUtils.cpp:85` | NEW P3 |
| Logger file sink writes lines without `RedactLogLine` | `Logger.cpp:320` (`FileSinkWorker` writes `e.message` verbatim) | NEW P3 |
| CR/LF/ANSI log injection from server-controlled data | `TextRedaction.cpp:80` | NEW P3 |
| `LongTokenRe` 40-char threshold misses 36-char Plane UUID tokens | `TextRedaction.cpp:45` | NEW P3 |
| SSRF IP denylist parses dotted-quad literals only | `AiEndpointSanitize.cpp:68-96,146-152` | synthesis #14 |
| SubprocessCapture inherits the full parent environment | `SubprocessCapture.cpp:106-119,492` | synthesis #15 |
| `p4`/`p4vc` resolved via `PATH` (binary planting) | `P4Annotate.cpp:49` (`SearchPathW`), `P4vLaunch.cpp` | synthesis #16 |
| Crash-handler minidump may include sensitive process memory | `SmatchetCrashHandler.cpp:53-55` | synthesis #17 |
| Standalone main does not fully harden the DLL search path | `main.cpp:1001` | synthesis #18 |
| CLI spawn log written to a predictable shared `/tmp` path (+ symlink race, no `O_NOFOLLOW`) | `CliCommandRunner.cpp:481-487,538` | synthesis #19 |
| MCP thread pool / SSE parking lacks connection bounds | `McpPlugin.cpp:848-850,600-620` | synthesis #20 |

### INFO (7)

Gradle wrapper jar not sha256-verified (synthesis #21 / SEC-AUDIT-26); legacy
`AiBaseUrl` grandfather path narrows the SSRF guard but cloud-metadata is still
blocked (#22 / SEC-AUDIT-27); attachment proxy accepts a `user:pass@` userinfo
component (#23 / SEC-AUDIT-28); DPAPI user-scope encryption with no added
entropy — same-user is in-scope (#24 / SEC-AUDIT-29); `os.date` strftime
n-bound guard holds; `db_path` is the user's own `%APPDATA%`; DPAPI plaintext
fallback for `BugReportPat` shares the common secret pattern (no per-field
uniqueness bug).

---

## 4. Refuted / downgraded on verification (transparency)

The adversarial pass is only credible if its negatives are reported too:

- **Refuted — `HttpGetBinary` icon-URL SSRF** (`SmatchetFieldIconRender.cpp:181-200`).
  The sink is genuinely weaker than the attachment path (no host allow-list, `http://`
  allowed), **but the attacker-controlled source never reaches it**: a Jira `priority`
  object's `iconUrl` is discarded by `NormalizeTrackerObjectValue`
  (`TrackerFieldValueParser.cpp:897-902`, returns only `name`) before it is stored, so
  the live pipeline never delivers an attacker URL to the fetch. Latent/defensive code,
  not a live SSRF primitive. (Worth a cheap HTTPS-pin hardening note in case a future
  mapper forwards raw priority JSON.)
- **Refuted/INFO — `os.date` strftime overflow** — the n-bound guard prevents the overflow.
- **False-positive/INFO — `cfg.JqlQuery` JQL injection** (`JiraIssueSearch.cpp:293`) — the
  query string is the **user's own** config, not a foreign-trust boundary.
- **Refuted/INFO — `db_path` "unsanitised"** (`:790`) — it is the user's own `%APPDATA%` path.
- **Refuted/INFO — `TextRedaction` misses `x-api-key`** (`TextRedaction.cpp:16`) — no
  header-map log path actually emits it.
- **INFO — DPAPI plaintext-fallback "uniqueness" bug** — all secret fields share the same
  pattern; there is no per-field divergence to exploit.

---

## 5. Reconciliation with the standing backlog (E1–E11)

The audit was deduped against the existing 11 entries. The map:

| Existing | Status after 2026-06-13 audit |
|---|---|
| **E1** JQL injection via issue keys (`JiraIssueSearch.cpp:470-487`) | **Re-confirmed** (re-run, `BuildKeyInJql` `:199` partly-confirmed LOW). H3 is the *sibling* AccountId path — fix both with the shared `JqlQuoteLiteral()` helper. |
| **E2** Tracker redirect forwards `Authorization` | **Upgrade flag → raise P3→P2.** Both fleets confirm HIGH (= H4). |
| **E3** Lua tarball hash | **Applied 2026-06-02** — no change. |
| **E4** First-send outbound consent modal | Re-confirmed (SEC-AUDIT-22). No change. |
| **E5** `AgentsMdLoader` path-traversal | **Superseded by H1 → raised P2→P1.** Same gap, sharper exfil framing + exact sink lines. |
| **E6** `ai.prompt` no rate-limit | **Upgrade flag** — re-confirmed HIGH (= H5, SEC-AUDIT-21). Hold P2 (needs local paste-and-run). |
| **E7** CR/LF/NUL strip at persist site | No new signal. No change. |
| **E8** SSE/NDJSON parse-fail logs 200 B unredacted | Distinct from synthesis #12 (a different client error-logging site). Both stand. |
| **E9** GoldenImage PPM strtol (stb-migrated, test-only) | No change. |
| **E10** CI pin `uses:` to SHAs + Dependabot | Re-confirmed (SEC-AUDIT-24/25). No change. |
| **E11** Mesa archive integrity | Re-confirmed. No change. |

**Three severity-upgrade flags** are the headline reconciliation output: **E2 → P2**,
**E5 → P1 (via H1)**, **E6 re-confirmed HIGH**. These are recorded inline in the
backlog entries.

---

## 6. Coverage and gaps

**Covered:** all three targets (Standalone, Unreal-embed, Android-mobile); the AI
outbound path + provider clients; the MCP server + attachment proxy; the Lua
sandbox + paste-and-run + MCP `run_lua`; config/SQLite at-rest + secret
protection; subprocess execution (p4/git/whisper/file-pickers/`--spawn`); tracker
HTTP transport + query construction + untrusted-response parse; CI/build supply
chain (21 workflows + FetchContent + Lua TOFU pin); image decode/cache/attachment;
logging/audit-jsonl/redaction.

**Real mitigations confirmed present** (credited, not findings): the Lua sandbox
closes the classic escapes (no `io`/`os.execute`/`loadstring`/`require`/metatable
pivot); MCP auth is constant-time + fail-closed on empty token + payload-capped;
the attachment proxy is HTTPS-only + host-allow-listed + redirects-disabled; AI
keys are DPAPI-encrypted at rest **on Windows**; header values are CR/LF/NUL
stripped; provider error bodies are redaction-swept before logging; every git
dependency is full-SHA pinned and Font Awesome/OpenSSL/Lua are sha256/TOFU
verified; the Android manifest exports only the launcher activity with
`allowBackup=false` and OS-default cleartext blocking; WIC thumbnail decode
scales down rather than materializing full-res buffers.

**Gaps / not exhaustively covered:** dynamic/runtime fuzzing was out of scope
(static + targeted read only); the DX12/Unreal-embed divergence was reasoned
about via `Source/Core` shared code, not exercised in an Unreal build; third-party
vendored code (curl 7.80.0, cpr 1.9.2, stb, sol2) was read only where Smatchet's
call reaches it, not audited wholesale.

---

## 7. Backlog

The deduped, per-finding, agent-actionable backlog is at the top of
[`docs/self-improvement/categories/security.md`](../self-improvement/categories/security.md),
dated 2026-06-13: the synthesis's 24 entries (1 × P1, 12 × P2, 11 × P3) plus the
re-run's 9 NEW confirmed entries, with the three severity-upgrade flags recorded
inline on E2/E5/E6. Fixing agents are routed by the `area:` of each finding's
location per the delegation table.
