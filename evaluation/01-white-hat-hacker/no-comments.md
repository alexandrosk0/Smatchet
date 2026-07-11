# Smatchet — White-Hat Security Evaluation, COMMENT-STRIPPED Blind Pass

**Assessor:** External white-hat, independent audit — *comments-stripped mirror only*
**Date:** 2026-07-01
**Target:** Smatchet (C++ desktop/mobile ticket-grid app: MCP server, Lua automation, AI assistant, multi-tracker HTTP clients)
**Constraint:** All code read from a mirror with every comment stripped (blank lines preserve line numbers). Orientation docs (README/BUILD/CLI/LUA/MCP guides) allowed; `SECURITY_AUDIT.md` and the agentic meta-layer deliberately not read. Citations are `file:line` (relative to `Source/`), matching the real repo.

---

## 1. Executive Summary + Verdict

Even with 100% of the comments removed, Smatchet reads as a **security-conscious, defense-in-depth codebase**, and I was able to reconstruct essentially the entire threat model from identifiers, control flow, and — crucially — the project's habit of encoding intent in *runtime strings* (log messages, HTTP error bodies, enum-to-description tables). The dangerous primitives are all still legible: a loopback MCP HTTP server with constant-time token compare, Host/Origin DNS-rebind gating, an SSRF allow-listed attachment proxy; a Lua sandbox that strips the escape-hatch globals; an AI-endpoint SSRF sanitizer that canonicalizes decimal/hex/octal IPv4 and IPv6; and a bounded SAX JSON parser that defeats the deep-DOM teardown crash class.

**Verdict: STRONG (unchanged from the code+comments pass).** Comment stripping degraded my *confidence on intent* in a few places (was a passthrough deliberate? is the DNS-only-literal SSRF check an accepted residual?), and made two "why" invariants (constant-time compare, bounded-parse rationale) something I had to *infer* rather than *read*. But it did not hide a single exploitable finding. The code is unusually self-documenting because the authors leaned on descriptive symbol names and, most importantly, **user-facing strings that double as specifications** (e.g. `EndpointVerdictDescription` literally spells out "blocked to prevent SSRF"). Those strings are not comments and survived the strip.

The most material finding remains architectural: tokenless loopback MCP with `mcp_require_token_on_loopback=false` exposes the full command registry (and sandboxed `run_lua` if enabled) to any local process. Secondary: POSIX-desktop cleartext secrets and a scatter of unbounded `nlohmann::json::parse` calls on tracker responses.

---

## 2. Method & Constraint

I read only from `scratchpad/Source-nocomments/**`. I could read: all `.cpp`/`.h` bodies, string literals, log lines, HTTP response bodies, enum names, function/variable names. I could **not** read: any `//` or `/* */` rationale, the `BoundedJsonParse.h` header banner that (in the real tree) explains *why* the SAX cap exists, per-function preconditions, or "we accept this residual risk" notes. Blank lines mark where comments were.

**What survived and carried meaning:** (a) descriptive identifiers (`ConstantTimeStringEquals`, `IsMcpHostOriginAllowed`, `RejectedCloudMetadata`); (b) runtime strings — log warnings (`"MCP: auth denied ... reason=token_required_on_loopback"`), HTTP error bodies (`"Attachment host is not allowlisted."`), and enum description tables (`AiEndpointSanitize.cpp:340-363`). These are *de facto* documentation the strip could not touch.

Files inspected in depth: `Plugins/Mcp/{McpPlugin,McpJsonRpcPure}.cpp`, `Plugins/LuaConsole/LuaConsolePlugin.cpp`, `Core/src/AppController_LuaBindings{,Core}.cpp`, `Core/src/AiEndpoint{Policy,Sanitize}.cpp`, `Core/src/AiNdjsonParser.cpp`, `Core/include/Json/BoundedJsonParse.h`, `Core/src/Config/{ConfigManager,ConfigManager_PathUtils}.cpp`, `Core/src/{P4Annotate,SubprocessCapture}.cpp`, `Core/include/Commands/PathConfinement.h`, `Mobile/Android/SmatchetAndroidSecretBridge.cpp`, and a repo-wide `json::parse` vs `ParseBounded` sweep across `Core/src/Tracker/**`.

**Limitation:** static only. "CONFIRMED" means I traced the path, not that I ran an exploit.

---

## 3. Findings Reachable WITHOUT Comments

### F-1. Tokenless loopback MCP → full command registry (+ sandboxed Lua) — **Medium (High if both non-defaults set)**
`Plugins/Mcp/McpPlugin.cpp:173-196` (`Authorize`), `:597-616` (`run_lua`), `:751-872` (JSON-RPC dispatch).
Fully reachable blind. `Authorize()` returns `true` for a loopback caller once `auth_token` is empty **and** `require_token_on_loopback` is false (`:185-196`). After that gate, `HandleToolsCall`/`HandleJsonRpcToolsCall` dispatch any registered command, and `run_lua` runs sandboxed Lua when `allow_lua_execution` is set. The secure default is visible in code: with an empty token the tokenless branch returns **401** (`:185-194`) unless the operator disabled `require_token_on_loopback`. Destructive mutations still require `__confirm` (`arguments.value("__confirm", false)`, `:526`). Impact: unauthenticated *local* command execution across a process trust boundary on a shared host.

### F-2. Unbounded-DOM `json::parse` on tracker/LLM responses — **Medium**
Sample: `Tracker/GitHubClient.cpp:137,414,639`; `Tracker/PlaneIssueMutation.cpp:114,326,350,520`; `Tracker/JiraActivityFeed.cpp:223`; `Tracker/TrackerFieldCatalog.cpp:45,87,102,122,145,180,226,257,303`; `Tracker/GitHubIssueSearch.cpp:181,341,612`.
Reachable blind by contrast: the codebase clearly has *two* parse idioms — `smatchet::json_safe::ParseBounded(...)` (depth/node-capped SAX, `BoundedJsonParse.h`) and bare `nlohmann::json::parse(resp.text)`. The MCP/NDJSON/notify ingress uses the bounded form; many tracker-response sites use the bare form. nlohmann's parse is iterative but the resulting DOM is destroyed **recursively**, so a ~1M-deep nested structure from a hostile/compromised/MITM'd tracker overflows the stack on teardown — an *uncatchable* crash that slips past the surrounding `try/catch`. The `(nullptr,false)` non-throwing variant does not help; it still builds the deep DOM. Medium because the source is the *configured* upstream, and TLS-verify + redirects-off (below) limit MITM.

### F-3. POSIX-desktop secrets stored as cleartext JSON — **Low (platform limitation)**
`Config/ConfigManager_PathUtils.cpp:383-404` (non-Win/non-Android `ProtectSecretForConfig` returns `plainText`); `Config/ConfigManager.cpp:562-593` (`#else WriteSecretFields` writes `token`, `github_pat`, `linear_api_key`, `mcp_auth_token`, `ai_api_key`, `ai_anthropic_api_key`, `ai_deepseek_api_key` as plaintext).
Reachable blind purely from the three-way `#if defined(_WIN32) / #elif __ANDROID__ / #else` split: Windows seals via DPAPI (`CryptProtectData`, `:331-347`), Android via Keystore JNI (fail-closed), and the `#else` desktop branch writes secrets verbatim. Confidentiality then rests entirely on file mode. This is inferable, but *whether it is an accepted design limitation* is exactly the kind of "why" a comment would have stated (see §4).

### F-4. AI custom-endpoint sends provider key to operator-chosen host — **Low**
`AiEndpointPolicy.cpp:8-25`: when `AiAllowCustomEndpoint{OpenAi,Anthropic}` is set, both `AllowCustomHost` **and** `AllowInsecureHttp` are relaxed together. The SSRF/host-pin enforcement lives in `SanitizeAiEndpointUrl` (`AiEndpointSanitize.cpp:264-324`) which appears to run at prefs-validation time, not at each send — so a poisoned/typo'd stored endpoint can exfiltrate the key. Operator-controlled knob, hence Low.

### F-5. P4 / subprocess argument-injection (no shell) — **Low / Info**
`P4Annotate.cpp:107,203` (`{"annotate","-u","-c","-q",pathArg}`); `SubprocessCapture.cpp:525` (`execvp`), `:400` (`CreateProcessW`).
Blind-confirmed: every spawn is argv-vector based — no `/bin/sh -c`, no `cmd.exe`. Classic shell-metachar injection is impossible. A `pathArg` beginning with `-` could be mis-parsed as a `p4` flag (argument injection), but it is a discrete argv element and the env is scrubbed (`opts.scrubSensitiveEnv = true`, `P4Annotate.cpp:58`).

### F-6. MCP remote mode drops Host/Origin rebind defense — **Low (operator intent)**
`McpPlugin.cpp:162-172`: the Host/Origin check runs only when `bind_host == kBindLocalhost`; with `McpAllowRemote` (bind `0.0.0.0`) it is skipped, leaving only the constant-time token check (`:197-206`). Inferable directly from the guard condition.

---

## 4. Findings that DEGRADED or Became Ambiguous Without Comments

- **DNS-only-literal SSRF (AiEndpointSanitize) — residual, now un-annotated.** `SanitizeAiEndpointUrl` (`:264-324`) canonicalizes and blocks *IP-literal* private/link-local/cloud-metadata targets, but a **hostname** that DNS-resolves to `169.254.169.254` or `10.x` is *not* re-checked after resolution — the classic DNS-rebind / DNS-pinning gap. Blind, I can *see* the literal-only coverage but cannot tell whether this is an accepted residual (curl resolves later; maybe a resolve-time hook exists elsewhere) or an oversight. A comment saying "literal-only; we accept DNS-rebind residual because X" would have resolved it instantly. This is the single most comment-degraded judgement.

- **Constant-time compare — invariant survived, rationale did not.** `ConstantTimeStringEquals` (`McpJsonRpcPure.cpp:268-277`) is *self-evidently* constant-time from structure: full-length XOR accumulation, no early return, length folded into `diff`. The security *invariant* was recoverable without the comment; only the "why (timing side-channel on token compare)" was lost — and the function name recovers even that.

- **Bounded parse — the "why" moved from header banner to inference.** In the real tree, `BoundedJsonParse.h` carries a banner explaining the recursive-DOM-teardown crash class. Stripped, I had to infer the rationale from the SAX subclass overriding `start_object`/`start_array` with `Descend()`/`Count()` guards and the `kDefaultMaxDepth=256` / `kDefaultMaxNodes=200000` constants (`:31-55`). Recoverable, but it took reading the SAX internals rather than one sentence. An auditor in a hurry might have missed *why* the bound matters and under-rated F-2.

- **`IsLoopbackHostLiteral` trailing-dot rejection — subtle, nearly lost.** `McpJsonRpcPure.cpp:221-226` rejects a host whose last char is `.` (`bareHost.back() == '.'`). This defeats `localhost.`-style FQDN-normalization rebind tricks. Without a comment this one-liner is easy to skim past; I only flagged it because I was specifically hunting rebind defenses. Load-bearing check, near-invisible when un-annotated.

- **Spawn-token env scrub — intent inferable, urgency muted.** `McpPlugin.cpp:234-248` adopts `SMATCHET_MCP_SPAWN_TOKEN` then immediately `unsetenv`s it. Blind, the *mechanism* is clear; the *reason* (prevent a later child from inheriting the bearer token) is inferable but was surely a comment.

**Net ambiguity for a real attacker:** the only place stripping would push an attacker toward *dynamic testing* is F-4/the DNS-rebind residual — they'd have to actually point a custom AI endpoint at a hostname resolving to metadata and observe whether the key is sent. Everything else is decidable from source.

---

## 5. Self-Documentation Assessment

**The code stands on its own for a security audit — better than most.** The decisive reason is a stylistic one: Smatchet encodes security intent in **runtime strings and enum-description tables**, which are *not comments* and therefore survived the strip intact:

- `AiEndpointSanitize.cpp:340-363` — `EndpointVerdictDescription()` returns human strings like `"blocked to prevent SSRF"`, `"would send the API key in cleartext"`. This single function is effectively the SSRF threat model, in-band.
- MCP auth denials log `reason=token_required_on_loopback` / `bad_host` / `bad_origin` (`McpPlugin.cpp:167-204`), naming each control.
- Path confinement returns `"path traversal ('..') is not allowed"` / `"absolute paths are not allowed"` (`PathConfinement.h:39-48`), documenting its own contract.
- Function/enum names are specification-grade: `IsAllowedAttachmentHost`, `UrlHasUserinfo`, `RejectedNonProviderHost`, `CanAcceptSseConnection`.

Where the style is *not* string-heavy, comment loss bites: the DNS-literal-only SSRF scope, the trailing-dot rebind trick, and the "is POSIX cleartext accepted?" question all became inference tasks. So the comments were **load-bearing for a handful of "why/residual-risk" judgements**, but **not for locating or understanding any control**. A competent auditor reaches the same finding set; they lose ~15 minutes and a bit of certainty on intent.

---

## 6. DELTA vs the Code+Comments Pass

Prior report: `evaluation/01-white-hat-hacker/without-agents.md` (score **8/10**).

**Reproduced blind (same finding, same severity):**
- F-1 tokenless-loopback MCP + Lua (their F-1) — full reproduction, incl. secure-default 401 path.
- F-2 unbounded `json::parse` on tracker responses (their F-3) — reproduced with the same file:line sample set; the two-idiom contrast made it easy even without the `BoundedJsonParse.h` banner.
- F-3 POSIX cleartext secrets (their F-4) — reproduced from the `#if/#elif/#else` split.
- F-4 AI custom-endpoint key exfil (their F-6) — reproduced.
- F-5 P4/subprocess argv-injection (their F-7) — reproduced.
- F-6 MCP remote-mode rebind drop (their F-8) — reproduced.
- All Strengths (constant-time compare, sandbox global-stripping, SSRF allow-list, bounded ingress, redirects-off TLS, path confinement, SHA-pinned deps, Keystore fail-closed) — independently re-derived from code.

**Could NOT reproduce / lost blind:**
- **Their F-2 (`SECURITY_AUDIT.md` is stale).** By construction I was forbidden from reading `SECURITY_AUDIT.md`, so I cannot assess or reproduce this process finding at all. This is a *scope* loss, not a comment-strip loss — but it means one of their eight findings is simply invisible to this pass.
- **Their F-5 (AI clients don't thread the Android CA bundle).** I found `MakeAiSslOptions` is absent and AI clients pass no `cpr::SslOptions`, but confirming this is a *gap* (vs. deliberate reliance on the system store) leaned partly on the tracker-seam comments in the real tree. Blind, I'd rate it **suspected, unconfirmed** rather than a clean Low — a mild degradation.

**Newly-suspected (sharper without comments than with):**
- The **DNS-rebind residual in `SanitizeAiEndpointUrl`** (literal-IP-only coverage) is called out more explicitly here as a §4 ambiguity, because the missing "accepted residual" comment forced me to treat it as open. In the commented pass this was likely dismissed by an inline note.

**Score change:** The underlying *security posture* is identical — nothing was hidden. My *confidence* dropped slightly on two intent questions (DNS-rebind residual, Android AI CA seam). I therefore keep the **overall at 8/10** (the artifact didn't change) and add a dedicated **auditability-without-comments: 9/10** — docked one point only for the DNS-rebind residual and the CA-seam intent that genuinely needed the "why" a comment would have carried.

---

## 7. Scorecard

| Dimension | Score /10 | Notes |
|---|---|---|
| Input validation | 9 | Bounded SAX on attacker ingress; residual unbounded DOM on tracker responses (F-2). |
| Secrets / credential handling | 7 | DPAPI + Keystore strong; POSIX-desktop cleartext, mode-only (F-3). |
| Network / TLS | 8 | Verify-on, redirects-off, cleartext-http rejected by sanitizer; AI-client CA seam unconfirmed (delta). |
| Sandbox isolation | 9 | Thorough Lua global-strip + `LUA_MASKCOUNT` CPU hook; weakened only by explicit opt-in (F-1). |
| Dependency hygiene | 9 | Full-SHA FetchContent pinning; `THIRD_PARTY_LICENSES.md` present. |
| Attack-surface minimization | 8 | MCP/Lua-exec off by default, confirm-gated mutations; tokenless+remote knobs (F-1/F-6). |
| Security testing | 8 | Bounded-parse/path-confinement units + fuzz scenarios visible in tree. |
| **Code self-documentation / auditability-without-comments** | **9** | Intent survives in identifiers + runtime strings + enum-description tables; only DNS-rebind residual & AI CA-seam intent needed the "why". |
| **Overall** | **8/10** | Mature defense-in-depth; comment loss cost confidence, not findings. |

---

## 8. Bottom Line

Stripping every comment did **not** meaningfully weaken this security audit. I reproduced six of the prior pass's seven *code* findings and all of its strengths purely from identifiers, control flow, and — the real hero — the project's habit of writing its threat model into user-facing strings (`EndpointVerdictDescription`, MCP auth-denial reasons, path-confinement error text). Those are not comments, so they survived, and they make the codebase unusually auditable blind.

**The single biggest thing lost by stripping comments** is the ability to distinguish an *accepted residual risk* from an *oversight* — concretely, whether the AI-endpoint SSRF sanitizer's **IP-literal-only** coverage (no post-DNS-resolution re-check, i.e. no DNS-rebind defense) is a deliberate, documented trade-off or a gap. With comments, one sentence would settle it; without them, only dynamic testing (point a custom endpoint at a hostname resolving to `169.254.169.254` and watch for the key) can. That single "why" is the difference between a confident Low and an open question — and it's why auditability drops from a hypothetical 10 to a 9, while the overall posture holds at **8/10**.
