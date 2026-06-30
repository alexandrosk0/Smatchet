# Smatchet — External White-Hat Security Evaluation (Pass: *without-agents.md*)

**Assessor role:** Independent offensive-security researcher, unsolicited evaluation, no prior relationship with the team.
**Target:** `/home/user/Smatchet` — Smatchet desktop/CLI ticket-tracker client with an embedded MCP server, Lua automation, AI assistant, and tracker integrations (Jira/Plane/GitHub/Linear/Perforce).
**Date:** 2026-06-30
**Methodology constraint for this pass:** The project's agentic-governance meta-layer was deliberately **ignored** (see Scope & Method). The artifact was evaluated as a stranger would after a clean `git clone`.

---

## 1. Executive Summary

Smatchet is, for a project of this size and feature surface, **a notably well-hardened codebase**. The areas a clone-and-run attacker would reach first — the loopback MCP server, the Lua automation sandbox, untrusted JSON ingress, and the AI/tracker HTTP clients — show evidence of deliberate, defense-in-depth security engineering rather than checkbox compliance. I went in expecting the usual embedded-HTTP-server and embedded-scripting disasters (tokenless localhost RCE, `os.execute` escapes, `VerifySsl{false}`, secrets in logs). I found none of them.

Concretely, the strong points I **confirmed by reading code**:

- The MCP server enforces a layered auth model: DNS-rebinding Host/Origin checks, constant-time token comparison, a *secure-by-default* "require token even on loopback" posture, and a fail-closed localhost-only fallback (`McpPlugin::Authorize`, `McpPlugin.cpp:151-207`).
- The Lua sandbox is a **whitelist**, not a blacklist: `os` is replaced with a 4-function safe subset, and `io`/`package`/`load`/`loadstring`/`dofile`/`require`/`debug`/`setmetatable`/`rawset` are all blocked in a per-run sandbox environment (`AppController_LuaBindingsCore.cpp:230-260`, `AppController_LuaBindings.cpp:241-288`).
- Untrusted JSON ingress on the MCP and AI paths routes through a genuinely correct depth/node-bounded SAX parser (`BoundedJsonParse.h`) that defeats the recursive-DOM-destruction crash class.
- Subprocess execution (Perforce, attachment-open, p4vc) uses **argv arrays with no shell** and scrubs secret-bearing env vars — no command-injection sink exists.
- Dependencies are pinned to **full commit SHAs** via `FetchContent`, and the one HTTP-fetched dep (Lua) is **SHA-256 verified** after download.
- Secrets are DPAPI-sealed on Windows and AndroidKeyStore-sealed (fail-closed) on Android; the POSIX-desktop cleartext limitation is **honestly documented**, not hidden, and mitigated with `O_NOFOLLOW` + `fchmod 0600` atomic writes.

The residual risk is dominated by **denial-of-service on semi-trusted network paths** (tracker HTTP responses still using bare `nlohmann::json::parse`) and one **confirmed arbitrary-file-read** finding I could not fully exonerate. Importantly, **two of the headline findings in the project's own `SECURITY_AUDIT.md` have since been remediated** (perf.dump and whisper.transcribe-once path confinement), which the audit document does not reflect — a positive discrepancy.

**Overall posture verdict: GOOD.** This is software I would be comfortable running on my own machine with default settings, and comfortable recommending after the handful of fixes below. It is meaningfully above the security baseline of comparable open-source developer tools.

---

## 2. Scope & Method

**In scope and read:** `Source/Plugins/Mcp/**`, `Source/Plugins/LuaConsole/**`, `Source/Plugins/Whisper/**`, `Source/Core/src/**` (config/secrets, Lua bindings, tracker/AI HTTP clients, P4, subprocess, JSON ingress), `Source/Mobile/Android*/**`, `CMakeLists.txt`, `SECURITY_AUDIT.md`, `THIRD_PARTY_LICENSES.md`.

**Method:** Direct source reading with file:line citation; targeted `grep` sweeps for known sink classes (`json::parse`, `system(`/`popen`/`exec*`, `VerifySsl`, `CryptProtectData`, `open_libraries`); a parallel sub-investigation of the tracker HTTP layer; and spot-verification of a sample of `SECURITY_AUDIT.md`'s claims against the code rather than trusting the document.

**Deliberately ignored this pass (per constraint):** `AGENTS.md` (root and any `src/*/AGENTS.md`), the entire `agents/` directory, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`, `.coderabbit.yaml`, `.cursor/`. None of these influenced the judgments below.

**Confidence labels:** *CONFIRMED* = I read the unsafe/safe operation and the path that reaches it. *SUSPECTED* = plausible but needs a PoC or a path I could not fully trace.

---

## 3. Attack Surface Map

| Surface | Exposure | Reachable by | Primary control |
|---|---|---|---|
| MCP HTTP server (`/mcp/*`, JSON-RPC) | Loopback by default; `0.0.0.0` if `McpAllowRemote` | Any local process; remote if opted in; malicious MCP client | Token + Host/Origin + loopback gating (`Authorize`) |
| MCP attachment proxy | Same as above | Same | https-only, allow-listed host, userinfo rejected, 10 MiB cap |
| Lua automation (UI, MCP `run_lua`, Lua MCP tools) | Local + MCP (gated on `McpAllowLuaExecution`) | Operator scripts, MCP clients | Whitelist sandbox env, fresh per-run state, instruction-count timeout |
| Command registry via MCP `tools/call` | Loopback/remote MCP | MCP clients | Per-command `Destructive` confirm; **path confinement on file-touching commands** |
| AI assistant (Anthropic/OpenAI/Ollama/DeepSeek) | Outbound HTTPS | Operator config; provider responses | Host pinning, bounded SSE/NDJSON parse, secret redaction |
| Tracker clients (Jira/Plane/GitHub/Linear) | Outbound HTTPS | Operator config; tracker responses | cpr default TLS verify; basic/bearer auth; **bare json::parse on responses** |
| Perforce (`p4`) | Local subprocess | Operator config / ticket paths | argv array, no shell, env scrub |
| Config at rest (`smatchet_config.json`) | Local file | Same-user processes | DPAPI/Keystore seal; POSIX cleartext + 0600 |
| Locale override files (`Locales/<lang>.json`) | Local file | Local attacker / malicious locale pack | **printf format-string risk (audit finding #1)** |
| Android app | Mobile | Device | KeyStore seal, `allowBackup=false`, no WebView/JS bridge |

---

## 4. Findings

### F-1 — `whisper.transcribe-once --file` arbitrary file read (audit #2) — *Severity: Medium (LIKELY REMEDIATED — verify)*

- **Location (read site):** `Source/Plugins/Whisper/WhisperPlugin.cpp:177-204` (`ReadWavFile`), guard at `:445-498` (`AcquireTranscribeOnceAudio`).
- **Description:** The MCP `tools/call` surface dispatches into the global command registry, and `whisper.transcribe-once` accepts a caller-supplied `file` argument whose bytes are POSTed to a cloud transcription endpoint. `SECURITY_AUDIT.md` finding #2 reports this as an unconfined arbitrary-file-read exfiltration primitive. **However**, the current code at `WhisperPlugin.cpp:462` routes `file` through `smatchet::cmd::ConfinePathUnderSubdir(GetUserDataDirectory(), "whisper-import", ...)`, enforces a `.wav` extension (`:473`), requires a regular file (`:478-484`), and imposes a 256 MiB cap (`:495`). I read `ConfinePathUnderSubdir` (`Source/Core/include/Commands/PathConfinement.h:83-152`) and confirmed it canonicalizes the base, rejects `..` and absolute subdirs, rejects symlinked confinement dirs, and re-checks canonical containment.
- **Impact (if the guard were ever bypassed):** Read + cloud-exfiltration of any local file the process can open.
- **PoC sketch:** `POST /mcp/tools/call {"name":"whisper.transcribe-once","arguments":{"file":"../../.ssh/id_rsa"}}` — *currently rejected* by the confinement + `.wav` checks.
- **Why still listed:** The audit document still presents this as an open Medium. The fix is real, but a reviewer should (a) confirm `ConfinePathUnderSubdir`'s candidate-side normalization also collapses `..` *within the candidate* (the helper's tail past line 152 was not fully read in this pass), and (b) update `SECURITY_AUDIT.md` so the stale finding does not mislead. **Treat as a documentation/verification gap rather than a live exploit.**

### F-2 — Tracker HTTP responses parsed with bare `nlohmann::json::parse` (DoS, audit's dominant class) — *Severity: Low–Medium — CONFIRMED*

- **Locations (sample):** `Source/Core/src/Tracker/JiraActivityFeed.cpp:223`; `Source/Core/src/Tracker/TrackerFieldCatalog.cpp:45,87,102,122,145,180,226,257,303`; `Source/Core/src/Tracker/GitHubIssueSearch.cpp:181,341,612`; `Source/Core/src/Tracker/PlaneClient.cpp:100`; `Source/Core/src/Diagnostics/BugReportService.cpp:239-432`.
- **Description:** The MCP/Lua/AI ingress paths correctly use `json_safe::ParseBounded`, but tracker and VCS HTTP **response bodies** are still parsed with raw `nlohmann::json::parse`. Most pass `nullptr, false` (non-throwing), and the throwing ones (e.g. `JiraActivityFeed.cpp:223`) are wrapped in `try/catch`. Crucially, **neither defends against the depth-bomb**: a deeply-nested JSON response builds a deep DOM whose *recursive destructor* overflows the C++ stack — a `SIGSEGV`, not a catchable exception (the mechanism is documented accurately in `BoundedJsonParse.h:6-14`).
- **Impact:** Process crash (DoS) triggered by a malicious or compromised tracker server, or an active MITM if TLS were ever stripped. Not memory corruption / RCE.
- **PoC sketch:** Point the client at a server returning `'[' * 1_000_000 + ']' * 1_000_000` (≈2 MiB) as a Jira/GitHub search response → crash on DOM teardown.
- **Remediation:** Route every tracker/VCS response parse through `json_safe::ParseBounded` (with a response-appropriate `maxBytes`), exactly as the MCP/AI paths already do. This is the audit's #1 theme and the fix is mechanical and uniform.

### F-3 — printf format-string from locale override files (audit #1) — *Severity: Medium — CONFIRMED (local-file precondition)*

- **Location:** `Source/Core/src/SmatchetLocalization.cpp` (`Format`, ~1150-1169; override load ~1007-1031).
- **Description:** `SmatchetLocalization::Format` obtains its format string from `T(key, fallback)`, which returns an **override** string from `<RuntimeAssetDir>/Locales/<lang>.json` when present, then passes it to `vsnprintf` with C++-supplied varargs. A crafted override that adds/changes conversion specifiers (e.g. `%s %s %n` where the caller passes one int) causes out-of-bounds reads, and `%n` an arbitrary write on libc builds that honor it.
- **Impact:** Local DoS / info-leak; potential write on non-hardened `%n` libc. Requires the attacker to plant a locale file in the asset dir (local ingress / malicious locale pack / shared source tree).
- **PoC sketch:** Ship `Locales/xx.json` with `"window.views_backend": "%s %s %s %n"`, select language `xx`.
- **Remediation:** Never feed locale-loaded strings to printf-family functions. Validate that an override's specifier sequence matches the built-in entry, or migrate to `{0}`-style templating (already used for some keys) for all user-facing strings.

### F-4 — Secrets stored as cleartext JSON on POSIX desktop — *Severity: Low (by design, documented) — CONFIRMED*

- **Location:** `Source/Core/src/Config/ConfigManager_PathUtils.cpp:370-405` (`#else` arm of `ProtectSecretForConfig`).
- **Description:** On Linux/macOS there is **no OS-backed encryption**; `ProtectSecretForConfig` returns the plaintext verbatim and it lands in `smatchet_config.json` (tracker API tokens, MCP auth token, AI keys, Whisper key). This is *explicitly documented* as audit H2 and mitigated by `AtomicWriteTextFile` opening the temp file `O_NOFOLLOW` and `fchmod`-ing it `0600` before atomic rename, plus a `LOG_WARN` when an existing config is group/world-readable (`IsLooseConfigFileMode`, `:438-442`).
- **Impact:** Any same-user process (or a careless backup/sync of the home dir) reads the operator's tracker/AI credentials. The 0600 perms bound this to the file owner; it is **not** encryption.
- **Remediation:** Integrate libsecret / macOS Keychain on desktop POSIX to match the Windows DPAPI / Android Keystore arms and reach parity with the documented fail-closed posture.

### F-5 — `McpAllowRemote` binds the command registry to `0.0.0.0` — *Severity: Medium if misconfigured — INFO/CONFIRMED*

- **Location:** `McpPlugin.cpp:218` (bind selection), `:162-172` (Host/Origin check skipped when bound to `0.0.0.0`).
- **Description:** Setting `McpAllowRemote` binds the MCP server to all interfaces and, by design, **skips the DNS-rebinding Host/Origin defense** (a non-loopback Host is then treated as operator intent). Authentication still requires the token (or fails closed to localhost-only when no token is set), so this is not an unauthenticated-RCE, but an operator who sets `McpAllowRemote` *and* a weak/blank token while relying on `McpRequireTokenOnLoopback=false` would expose the full command registry (Lua execution included, if `McpAllowLuaExecution`) to the LAN.
- **Impact:** LAN-reachable tool/command/Lua execution under operator misconfiguration.
- **Remediation:** Refuse to start with `McpAllowRemote=true` unless a non-empty token of sufficient entropy is set; consider a startup banner / hard error. Document that remote exposure + Lua execution is a high-trust combination.

### F-6 — `AllowCustomEndpoint` couples in `AllowInsecureHttp` for AI providers — *Severity: Low — CONFIRMED*

- **Location:** `Source/Core/src/AiEndpointPolicy.cpp:8-24`.
- **Description:** For OpenAI/Anthropic, `p.AllowInsecureHttp = cfg.AiAllowCustomEndpoint<Provider>` — i.e. enabling a custom endpoint *also* permits plain `http://`. An operator pointing at a local proxy may not realize they have simultaneously allowed cleartext transport for API keys.
- **Impact:** API key sent in cleartext if a custom `http://` endpoint is configured.
- **Remediation:** Decouple the two toggles; default custom endpoints to https-required and make insecure-http a separate, explicit opt-in.

### F-8 — GitHub base URL never `https://`-validated on the request path (PAT cleartext exposure) — *Severity: Medium — CONFIRMED*

- **Location:** `Source/Core/src/Tracker/GitHubClientHelpers.cpp:168-202` (`IsValidGitHubBaseUrl`, defined but only used in `Diagnostics/BugReportBody.cpp:114`); request path accepts `cfg.GitHubBaseUrl` verbatim in `GitHubClient.cpp:78,125,403,446,622`, `GitHubActivityFeed.cpp:58-233`, `Vcs/GitHubCommits.cpp:65-78`.
- **Description:** Unlike Jira (which routes through `NormalizeBaseUrl` → `ShouldUpgradeCleartextBase`, `TrackerHttpUtils.cpp:95-113`) and Linear (which rejects `http://` via `IsValidLinearApiUrl`), the GitHub client concatenates the operator-configured base URL into request URLs **without any scheme validation or cleartext upgrade**. The existing `IsValidGitHubBaseUrl` validator is never called on the request path.
- **Impact:** Setting `github_base_url=http://host` causes every request to ship `Authorization: Bearer <PAT>` in cleartext on the wire (MITM-interceptable). An `https://attacker.tld` value is also accepted (host not validated on this path).
- **PoC sketch:** `github_base_url=http://attacker.tld` → `Authorization: Bearer <PAT>` sent plaintext to attacker host.
- **Remediation:** Call `IsValidGitHubBaseUrl` before issuing requests, or route GitHub through the same cleartext-upgrade guard Jira uses.

### F-9 — Plane base URL has no `https://` enforcement / cleartext upgrade (api-key cleartext exposure) — *Severity: Medium — CONFIRMED*

- **Location:** `Source/Core/src/Tracker/PlaneClient.cpp:43-68` (`NormalizePlaneApiBase` trims/rewrites host but never adds scheme or upgrades `http://`); raw base flows into `PlaneClient.cpp:88`, `PlaneIssueSearch.cpp:48/536/658`, `PlaneIssueMutation.cpp:67/318/480`, `PlaneFieldCatalog.cpp:411`, `PlaneActivityFeed.cpp:113` with the `x-api-key` header attached.
- **Description:** Plane is the second client (with GitHub, F-8) missing the cleartext-upgrade protection that Jira and Linear already implement. No https validation exists in config or UI (only example tooltip text, `SmatchetPreferencesUi.cpp:330`).
- **Impact:** `plane_url=http://internal-host` → Plane `x-api-key` sent in cleartext.
- **Remediation:** Apply the same `ShouldUpgradeCleartextBase` guard to `NormalizePlaneApiBase`.

### F-10 — Plane error strings echo raw upstream body (minor leak) — *Severity: Low — CONFIRMED*

- **Location:** `Source/Core/src/Tracker/PlaneClient.cpp:93-94` (`response.text.substr(0, 300)` into a user-facing error).
- **Description:** Unlike the GitHub/Linear paths (which extract only the JSON `message` field) and the centralized redacted log path, the Plane client splices an unredacted upstream error body into its error string. A tracker that reflects request headers in its error JSON could surface them. The Plane api-key is unlikely to appear in its own 401 body, so impact is minor.
- **Remediation:** Extract only the structured `message` field, or run the body through `RedactProviderErrorBody`.

> **Tracker HTTP — confirmed safe (Info):** No `VerifySsl{false}` anywhere (`MakeTrackerSslOptions`, `TrackerHttpUtils.cpp:145-151` — verification stays on); redirects disabled with `cont_send_cred=false` (`:163`) so Authorization is never forwarded cross-host; no raw token logging (logs print `pat_bytes=%zu` / set-booleans only); URLs redacted via `RedactUrlForLog`; request hosts are config-only, never ticket-field-controlled, with strict path-segment validation — **no SSRF-via-ticket-field path exists.**

### F-7 — `p4 annotate` path argument could be read as a flag — *Severity: Low/Info — SUSPECTED*

- **Location:** `Source/Core/src/P4Annotate.cpp:102-107`.
- **Description:** A ticket/user-controlled `depotOrPath` is passed as an argv element to `p4 annotate ... <pathArg>`. There is no shell (no injection), and env is scrubbed, but a `pathArg` beginning with `-` could be interpreted as a `p4` flag (argument injection). `p4 annotate`'s flag set is narrow and read-only, so practical impact is minimal.
- **Remediation:** Prepend `--` or validate that `pathArg` is a depot/local path before spawning. Low priority.

### Non-findings worth recording (things I checked and cleared)

- **No `system()`/`popen()`/shell exec** anywhere in product code; all subprocess use is argv-array (`SubprocessCapture`, `execlp`, `CreateProcessW`) — **no command injection.**
- **No `VerifySsl{false}`** or TLS-verification override anywhere; cpr/libcurl default verification (on) stands. AI and attachment-proxy requests disable cross-host redirects (`cpr::Redirect{...,false}`, `Redirect(false,false)`).
- **MCP run_lua and Lua MCP tools execute in the whitelist sandbox** on a *fresh per-call `sol::state`* (`ExecuteLuaSnippetForMcp`, `AppController_LuaBindings_Draw.cpp:936-984`) with an instruction-count hook timeout — no thread-shared `lua_State`, no `io`/`os.execute` reachable.
- **AI error/SSE logging is redacted** (`RedactProviderErrorBody`) before any provider text reaches a log (`AnthropicClient.cpp:78-86`), closing the "API key echoed by a malicious proxy into logs" channel.
- **MCP attachment proxy** rejects non-https, userinfo URLs, off-allow-list hosts, and caps body at 10 MiB — a credible anti-SSRF design (`McpPlugin.cpp:352-447`, helpers in `McpJsonRpcPure.cpp`).
- **`perf.dump`** (audit-flagged arbitrary write) is **remediated** with `ConfinePathUnderSubdir` (`BuiltinCommands_Perf.cpp:139-150`).

---

## 5. Strengths of the Security Posture

1. **Secure-by-default MCP.** Token-required-on-loopback is the *default*; the tokenless path fails closed to localhost-only; the spawn-token handshake is scrubbed from the env after adoption (`McpPlugin.cpp:229-249`). Constant-time token compare (`ConstantTimeStringEquals`). DNS-rebinding defense via Host+Origin. This is better than most shipped MCP servers.
2. **Whitelist Lua sandbox done right.** The decision to *replace* `os` with a safe subset rather than blacklist dangerous members — with an explicit comment about future Lua versions not silently leaking new dangerous functions — is exactly the right threat model. The `sandbox[name] = false` (vs `nil`) trick to defeat metatable fall-through is a subtle, correct detail (`AppController_LuaBindings.cpp:242-248`).
3. **A genuinely correct bounded JSON parser.** `BoundedJsonParse.h` drives nlohmann's own SAX DOM builder and aborts on depth/node caps *before* a deep DOM exists, with `allow_exceptions=false`. The team also corrected the common-but-wrong "recursive-descent parser overflows during parse" mental model — the real risk (recursive DOM destruction) is documented accurately.
4. **Supply-chain discipline.** Full-SHA pinning for all git FetchContent deps; SHA-256 verification with mirror-fallback for the one HTTP-fetched dependency (Lua); a `THIRD_PARTY_LICENSES.md` present.
5. **Filesystem hardening.** `O_NOFOLLOW` + `fchmod` fd-based perms (no path-based chmod TOCTOU), atomic rename, loose-perm warnings, fail-closed Android Keystore.
6. **Honest, verifiable self-audit.** `SECURITY_AUDIT.md` documents the POSIX cleartext limitation rather than papering over it, and its mechanism-correction note matches the code. The audit's claims I spot-checked were accurate (or conservative — two are now over-stated because the code was since fixed).

---

## 6. Scorecard

| Dimension | Score /10 | Rationale |
|---|---|---|
| Input validation | 8 | MCP/AI ingress bounded + validated; residual bare `json::parse` on tracker responses (F-2) and locale format-string (F-3) hold it back. |
| Secrets / credential handling | 7 | DPAPI/Keystore sealed + redacted logs + env scrub; POSIX-desktop cleartext (F-4) is the gap, though documented and 0600-mitigated. |
| Network / TLS | 7 | Default verification intact, no overrides, cross-host redirects disabled, host pinning; but GitHub + Plane base URLs lack the cleartext-upgrade guard Jira/Linear have (F-8/F-9 — PAT/api-key cleartext exposure), plus the custom-endpoint/insecure-http coupling (F-6). |
| Sandbox isolation | 9 | Whitelist Lua sandbox, fresh per-run state, timeouts, no shell sinks. Among the best parts of the codebase. |
| Dependency hygiene | 9 | Full-SHA pins + SHA-256 verified Lua download + license manifest. |
| Attack-surface minimization | 8 | MCP token-required default, command confinement, attachment-proxy allow-listing; `McpAllowRemote` (F-5) is an operator footgun. |
| Security testing | 8 | Dedicated `tests/Lua/LuaSandbox.test.cpp`, MCP/Lua race tests, a fuzz scenario, and a real multi-agent self-audit with re-verification. |
| **Overall** | **8** | Mature, defense-in-depth posture; residual issues are DoS-class or operator-misconfig, not unauthenticated RCE. |

---

## 7. Prioritized Recommendations

1. **(High value, low effort) Sweep tracker/VCS `json::parse` → `ParseBounded` (F-2).** Closes the audit's dominant DoS class uniformly; the helper already exists. ~25 call sites.
2. **(Medium) Fix the locale format-string (F-3).** Validate specifier parity or move user-facing strings off printf entirely.
3. **(Medium) Desktop POSIX secret encryption (F-4).** libsecret / Keychain to reach Windows/Android parity.
4. **(Medium) Wire `https://` enforcement / cleartext-upgrade into the GitHub and Plane base-URL paths (F-8, F-9)** — the guard already exists for Jira/Linear; GitHub even has an unused validator. Prevents PAT/api-key cleartext exposure.
5. **(Medium) Refuse `McpAllowRemote` without a strong token (F-5);** decouple AI custom-endpoint from insecure-http (F-6).
6. **(Low) Re-verify and re-document F-1.** Confirm candidate-side `..` collapse in `ConfinePathUnderSubdir`, and update `SECURITY_AUDIT.md` to mark perf.dump and whisper.transcribe-once as remediated so the doc doesn't mislead future reviewers.
7. **(Low) Redact the Plane error-body splice (F-10); `--` guard the `p4 annotate` path arg (F-7).**

---

## 8. Bottom Line: Would I trust running this?

**Yes — with default settings, on my own machine, I would run it.** Smatchet clears the bar that most clone-and-run developer tools fail: there is no tokenless-localhost RCE, no scripting-engine escape, no disabled TLS, no secrets-in-logs, and no shell-injection sink. The embedded MCP server and the Lua sandbox — the two surfaces I expected to break — are instead among the strongest parts of the codebase, and the supply chain is pinned and verified.

The honest caveats: (1) if you run on **Linux/macOS**, your tracker and AI credentials sit in a `0600` cleartext JSON file — fine against remote attackers, weak against a hostile same-user process; (2) a **malicious or compromised tracker server** can still crash the client via a JSON depth-bomb until F-2 is swept; and (3) **do not enable `McpAllowRemote` with Lua execution and a weak token** — that combination hands the LAN a scripting engine.

None of these are reasons to withhold trust from a careful operator; they are a punch-list. The team has clearly internalized a "never crash / defense-in-depth" discipline and even self-audited honestly. This is a project I would disclose to cooperatively, expecting fixes — not one I would publicly shame. **Overall: 8/10.**
