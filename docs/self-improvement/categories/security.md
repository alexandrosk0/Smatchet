# Agent self-improvement — security

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

<!-- ===========================================================================
     2026-06-13 — deep security audit of all targets + configurations.
     Full report: docs/security/SECURITY_AUDIT_2026-06-13.md
     Two adversarially-verified fleets (11-lane deep-audit + 5-lane re-run).
     Post-verify: 0 CRITICAL · 5 HIGH · ~14 MEDIUM · ~16 LOW · 7 INFO.
     Severity-upgrade flags recorded inline on E2 (→P2), E5 (→P1, superseded
     by the AgentsMdLoader entry below), E6 (re-confirmed HIGH).
     =========================================================================== -->

<!-- ===========================================================================
     2026-06-13 — deeper-audit playbook cross-reference (NOT yet confirmed).
     Source: docs/security/DEEPER_AUDIT_PLAYBOOK.md (the 7-tier / 34-target
     follow-up ladder, PR #1191) cross-referenced against this ledger via
     Workflow wf_ccaf45b5-1bc. Of 34 targets: 17 already covered above, 4
     partial, 13 untracked — the 7 finding-bearing rows below are the untracked
     /partial slices that name a concrete code sink. These are CANDIDATES from a
     static cross-reference, NOT adversarially-confirmed bugs (the playbook
     states nothing in it is a confirmed finding). Each names its confirm-step
     (TSan / UBSan / fuzz / review). The 8 audit-METHOD gaps (fuzz lanes, TSan
     CI, CVE/OSV match, SBOM) are filed as one epic in tooling.md
     (`deeper-audit-harness-buildout`), not here — this ledger tracks
     vulnerabilities, not tooling.
     =========================================================================== -->

- 2026-06-13 · deep-audit-xref · [security] · P2 — Candidate MCP/UI cross-thread g_ui data race (playbook target 3)
  Details: MCP dispatch runs off the UI thread — `Source/Plugins/Mcp/McpPlugin.cpp:434` -> `Source/Core/src/Commands/CommandRegistry.cpp:317` -> command handlers that touch UI-owned globals, e.g. `BuiltinCommands_BugReport.cpp:62-64` and `BuiltinCommands_Debug.cpp:292-294` read/write `g_ui` with no synchronization vs the render thread. The existing MCP rows above cover *authorization* (un-gated dispatch, no Host/Origin), NOT this thread-safety gap. Candidate only — needs a TSan run to confirm a real race vs a benign single-writer pattern.
  Concrete next action: Build the MCP/AI TSan lane (see tooling `deeper-audit-harness-buildout`), drive an MCP command that reaches a g_ui handler concurrently with the render loop, confirm/deny the race; if real, marshal handler-side g_ui access onto the UI thread or guard it. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-xref · [security] · P2 — Candidate AiAssistant streaming cancel/submit race (playbook target 5)
  Details: `Source/Core/src/AiAssistantController.cpp` mutates streaming state across the network callback and UI threads: `currentCancel_` set/cleared at `:212`/`:271`, the on-delta closure built at `:434` (`MakeOnDelta`), and a model-change path that clears g_ui state at `:346-348`. A submit/cancel/model-switch interleaving could use-after-clear or double-invoke the cancel token. The existing AiAssistantController row above is the *x-api-key-on-redirect* leak (a different bug). Candidate only — needs TSan + a scripted submit-then-immediately-cancel/switch scenario.
  Concrete next action: Reproduce under the MCP/AI TSan lane with a cancel-during-stream and model-switch-during-stream scenario; if a race is confirmed, make the cancel token + streaming state ownership single-threaded (post to UI thread) or atomic. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-xref · [security] · P2 — Unreal console / Blueprint exec reaches full CommandRegistry without ctx.Source authz (playbook target 24, partial)
  Details: `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/SmatchetImGuiConsoleCommands.cpp` exposes Smatchet commands to the Unreal console / Blueprint exec; `IsSafeConsoleCommandName` filters the command *name* but not arguments, and the dispatch reaches the same registry the `CommandRegistry.cpp:298` row notes has no `ctx.Source` trust gate. So an Unreal-embed console caller gets the same destructive reach as UI — the finding-bearing slice of the partial target-24 coverage (the existing CommandRegistry row tracks the gate, not this embed entry-point). Candidate — needs review of the actual argument-forwarding path + whether ship Unreal builds expose the console.
  Concrete next action: Route the Unreal console/Blueprint entry-point through the per-source trust gate proposed in the `CommandRegistry.cpp:298` row (tag ctx.Source = UnrealConsole, deny destructive without out-of-band confirm). Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-xref · [security] · P3 — Candidate FormatDateIfIso out-of-bounds read (playbook target 6, partial)
  Details: `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:352` `FormatDateIfIso` indexes `value[4]` / `value[7]` to sniff an ISO date separator with no length check on `value` first; a server-supplied field shorter than 8 chars is an OOB read. Finding-bearing slice of the partial target-6 coverage (the existing ADF-recursion row tracks only `:290`/`:309`, the unbounded-nesting parse, not this fixed-index read). Candidate — confirm under UBSan/ASan with a short field value.
  Concrete next action: Length-check `value.size()` before the `[4]`/`[7]` index (or use `.at()` / a `>= 8` guard); add a UBSan-driven unit test with a 0-7 char value. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-xref · [security] · P3 — Plane issue-mapping parsers unhardened against malformed tracker JSON (playbook target 11)
  Details: `Source/Core/include/Tracker/PlaneIssueMappingPure.h` (`:53`/`:60`/`:78`) + `Source/Core/src/Tracker/PlaneIssueMappingPure.cpp` map Plane API JSON into issue structs with no fuzz coverage; only the Jira ADF path has any hardening tracked. A malformed/hostile Plane response is an untested parse surface (type confusion, missing-key deref, deep nesting). Candidate — no confirmed defect, this is an untested-surface gap.
  Concrete next action: Add a libFuzzer/AFL harness over the Plane mappers (part of `deeper-audit-harness-buildout`); fix any crash/over-read it finds. Effort S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-xref · [security] · P3 — GitHub GraphQL->REST issue-mapping parsers unhardened against malformed JSON (playbook target 12)
  Details: `Source/Core/include/Tracker/GitHubIssueSearchMapping.h` (`:38`/`:81`/`:96`) + `Source/Core/src/Tracker/GitHubIssueSearchMapping.cpp` map GitHub search responses with no fuzz coverage, same untested-surface class as the Plane mappers above. Candidate — untested-surface gap, no confirmed defect.
  Concrete next action: Add a fuzz harness over the GitHub mappers (part of `deeper-audit-harness-buildout`); fix any crash/over-read. Effort S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-xref · [security] · P3 — JSON->Lua decode sink lacks explicit depth/size bounds (playbook target 14)
  Details: `Source/Core/src/AppController_LuaBindingsCore.cpp:64` `JsonToLuaImpl` (reached from `AppController_LuaBindings.cpp:763`) recursively converts arbitrary JSON into Lua tables with no explicit depth/size cap shown at the sink. Deeply-nested or huge JSON handed to a Lua binding is a potential stack-exhaustion / memory-amplification surface. The existing Lua rows above cover *different* sinks (ai.prompt rate-limit, count-hook). Candidate — review whether nlohmann's parse-depth limit already bounds this before it reaches the converter.
  Concrete next action: Confirm where the JSON is parsed (nlohmann default max-depth) and whether the converter can be reached with attacker-controlled nesting; if unbounded, add an explicit depth + element-count cap in `JsonToLuaImpl`. Effort S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P1 — Server-supplied AccountId injected unescaped into JQL (JqlSuggestEngine)
  Details: `Source/Core/src/Tracker/JqlSuggestEngine.cpp:139-140` `BuildJqlUserInsert` builds a quoted JQL literal from a tracker-supplied AccountId; the AccountId fallback path does no `"`/`\` escaping, so a malicious/compromised tracker response breaks out of the literal in the user-autocomplete query. Distinct from the issue-key path (E1 / `BuildKeyInJql` `JiraIssueSearch.cpp:199`, partly-confirmed LOW by the re-run). Confirmed HIGH, adversarially verified, NEW.
  Concrete next action: reuse the E1 `JqlQuoteLiteral()` helper (escape `\`→`\\` then `"`→`\"`) at the AccountId insertion site, OR validate AccountId against its grammar before insertion. Unit-test both the key and AccountId paths. ~1 h.
  Status: applied (2026-06-14 — `BuildJqlUserInsert` now routes BOTH the display-name and AccountId paths through the new shared `tracker_jql::QuoteLiteral` (`Source/Core/include/Tracker/JqlEscape.h` + `.cpp`); a display name containing `"` is now escaped, not abandoned to the AccountId fallback. Doctest `tests/Core/JqlEscape.test.cpp` proves break-out payloads are escaped. Shipped with E1 in the JQL-injection PR.)
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P1 — Arbitrary config-specified file read prepended verbatim into outbound LLM system prompt (supersedes E5, raised P2→P1)
  Details: Source/Core/src/AgentsMdLoader.cpp:28-58,122-153 reads a config-specified override path (≤64 KB) guarded only by fs::exists (ConfigManager.cpp:791-792), concatenated into the system prompt at AiAssistantController.cpp:408,418. No canonicalization/allowlist/root-containment. Verified: a local config write → off-host exfil of any readable file to the remote LLM provider. Same gap as E5 below, with sharper exfil framing + exact sink lines.
  Concrete next action: Add a path-containment helper (canonicalize + reject symlinks/out-of-root absolute paths; pin the filename suffix to *agents.md) and treat override content as untrusted in prompt assembly. Effort M (~1 day + tests).
  Resolution: **applied — fix/agentsmd-path-containment**. `AgentsMdLoader.cpp` gained `ContainAgentsMdPath` (anon ns): `fs::canonical`-resolves the override path (resolving symlinks + `..`) and refuses it unless the REAL filename ends in `.md`; `LoadOneCapped` calls it before reading and reads the canonical (symlink-resolved) path (no TOCTOU re-traversal). This blocks a direct repoint to a credential file (id_rsa/cookies/known_hosts — no `.md`) AND a `evil.md`→secret symlink (canonical resolves to the non-`.md` real name → rejected). **Pin is `.md`, NOT the audit-suggested `*agents.md`** — the existing tests + defaults prove the override contract legitimately allows any markdown file (global.md / proj.md / my-instructions.md), and an attacker can't turn id_rsa into a `.md` without already holding its content, so `.md` closes the repoint vector without breaking valid configs. Covers both override entry points (global + project) + auto-discovery uniformly. 2 new doctest cases (non-`.md` file refused via direct + layered entry; `.md`-symlink→non-`.md`-secret refused, gracefully skipped where symlinks need privilege); AgentsMd suite 18/18. Supersedes E5 below (same gap — close both). Cross-ref: AiAssistantController.cpp:408 prompt-assembly sink.
  Status: applied
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P2 — Command registry executes destructive commands with no ctx.Source authorization
  Details: Source/Core/src/Commands/CommandRegistry.cpp:298 gates only on (Destructive && !ConfirmedDestructive); no ctx.Source trust check, so MCP/Lua-sourced commands equal UI-sourced. Basis shared with MCP un-gated dispatch.
  Concrete next action: Add a per-source trust enum; gate destructive/source-restricted commands and require out-of-band confirm for non-UI sources. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — MCP registry dispatch un-gated after Authorize
  Details: Source/Plugins/Mcp/McpPlugin.cpp:426-445,625-642 — after loopback+token Authorize, dispatch reaches the full command registry with no per-command authorization (token possession == full reach).
  Concrete next action: Apply a command allowlist/capability scope to the MCP surface; route destructive commands through the source-aware gate. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — MCP server performs no Host/Origin check (DNS-rebinding exposure)
  Details: Source/Plugins/Mcp/McpPlugin.cpp:137-161 Authorize validates loopback+token but not Host/Origin; a rebound browser origin can reach the loopback port with only the token as barrier.
  Concrete next action: Reject non-loopback Host and remote Origin headers; keep token as defense-in-depth. Effort S.
  Status: open
  Last-reviewed: 2026-06-13
  Resolution: 2026-06-14 · fix/mcp-host-origin-dns-rebind (PR #1228) — McpPlugin::Authorize now applies a fail-closed Host/Origin gate when bound to loopback: rejects any Host that is not a loopback literal (127.0.0.1 / localhost / [::1], port-stripped, case-folded, trailing-dot rejected) and any present Origin that is not empty / "null" / a loopback http(s) origin; 403 + LOG_WARN(reason=bad_host|bad_origin). Decision extracted to pure helpers IsLoopbackHostHeader / IsAllowedMcpOrigin / IsMcpHostOriginAllowed (Source/Plugins/Mcp/McpJsonRpcPure.{h,cpp}) with doctest coverage (tests/Plugins/Mcp/McpHostOrigin.test.cpp). Skipped when McpAllowRemote binds 0.0.0.0 (a non-loopback Host is the operator's explicit intent there). Token check retained as defense-in-depth.

- 2026-06-13 · deep-audit · [security] · P2 — MCP attachment proxy fetches caller-supplied URLs (SSRF surface)
  Details: Source/Plugins/Mcp/McpPlugin.cpp:275-352 fetches a caller URL; the mcp-lane coverage found it already HTTPS-only + host-allow-listed (tracker domain + api.media.atlassian.com) with redirects disabled, so this is a confirm-it-routes-through-the-shared-AiEndpointSanitize hardening rather than a live SSRF.
  Concrete next action: Route the fetch through the shared sanitizer; deny private/link-local/metadata targets and non-http(s) schemes. Effort S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — P4vLaunch argument injection via QuoteWinArgWide trailing-backslash bug
  Details: Source/Core/src/Ui/P4vLaunch.cpp:72-86,149,172,189-190 and P4Annotate.cpp:52-59 compose argv from changelist/file fields; QuoteWinArgWide mishandles trailing backslashes, allowing argument-boundary injection. Re-run confirmed MEDIUM and located the custom-command {file}/{cl} template path at P4vLaunch.cpp:172 (gated by AnnotateAllowCustomCommands).
  Concrete next action: Fix backslash doubling per CommandLineToArgvW; prefer argv-array spawn. Effort S + unit test.
  Resolution (2026-06-14 · p4-annotate · branch fix/p4v-arg-quote-trailing-backslash): QuoteWinArgWide rewritten to the canonical CommandLineToArgvW algorithm (backslash-run doubling before any literal quote AND before the closing wrap quote), extracted to a pure header-only helper Source/Core/include/Ui/P4vLaunchArgQuotePure.h::QuoteWinArgWidePure so it is doctest-unit-tested (tests/Core/P4vLaunchArgQuotePure.test.cpp — trailing-backslash, embedded-quote, and a CommandLineToArgvW round-trip property proving each input parses back to exactly itself). All cited direct-p4vc call sites route through it: the timelapse {file} arg (P4vLaunch.cpp:149) and the change {cl} arg (now quoted; was previously unquoted). The custom-command {file}/{cl} template path (gated by AnnotateAllowCustomCommands) cannot per-arg-requote a user-authored template, so it now rejects {file}/{cl} VALUES containing a double-quote (the injection-enabling case) on top of the existing newline rejection. The audit's P4Annotate.cpp:52-59 cite is stale — that file composes an argv VECTOR passed to SubprocessCapture::Run, which already uses the correct SubprocessCapturePure::QuoteArgvWindows; no manual quoting there to fix.
  Status: fixed
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P2 — POSIX secret writes plaintext with no 0600 mode (re-run: HIGH at-rest exposure; raise to P1 when POSIX ships)
  Details: Source/Core/src/Config/ConfigManager.cpp:473-496 and ConfigManager_PathUtils.cpp:719-761 write secrets as plaintext config with default umask; no chmod 0600. The re-run confirmed the underlying no-op ProtectSecretForConfig #else branch (ConfigManager_PathUtils.cpp:331-334) as HIGH plaintext-at-rest (token/plane_api_key/github_pat/mcp_auth_token/ai_*_api_key/whisper_api_key). Windows (DPAPI) unaffected; this is the non-Windows gap.
  Concrete next action: chmod 0600 on create, verify mode on read; document the platform crypto gap. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — Android secret passthrough stores credentials in plaintext (re-run: HIGH at-rest exposure; raise to P1 when Android ships)
  Details: Source/Core/src/Config/ConfigManager_PathUtils.cpp:331-334 passes secrets through with no Android Keystore encryption. Same no-op #else branch as the POSIX entry above; confirmed HIGH plaintext-at-rest by the re-run.
  Concrete next action: Back Android secrets with the Keystore or mark the platform unsupported for secret storage. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — stb image decode dimension cap applied after allocation
  Details: Source/Core/src/Persistence/SmatchetImageTextureCache.cpp:141,149 checks the max-dimension cap after stb allocation; oversized images allocate before rejection (memory-pressure DoS).
  Concrete next action: Pre-validate via stbi_info before full decode. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — OfflineQueue serialized 'draft' string bypasses audit-trail redaction
  Details: Source/Core/src/Sync/OfflineQueueService.cpp:356,362 serializes the draft to a JSON string before BackendAuditTrail.cpp:124-148 redaction runs, so RedactJson/LooksSensitiveKey never sees nested keys.
  Concrete next action: Redact the draft object structurally pre-serialization or add a value-level pass. Effort S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — AI client redirect can forward Anthropic x-api-key cross-host
  Details: AiAssistantController AI-client redirect config can retain the x-api-key header across a redirect to a different host (distinct from the tracker-scoped E2/H4 item).
  Concrete next action: Strip auth headers on cross-origin redirect for all AI clients (cpr::Redirect header-stripping). Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — Error/response bodies logged without key-name redaction
  Details: A backend client error-logging site (distinct from E8, which is the SSE/NDJSON parse-fail 200 B site) emits response/error bodies without RedactJson, leaking reflected tokens to logs.
  Concrete next action: Route all body logging through the redaction helper; cap length. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — Automation worker hook aggravates shutdown deadlock / UI-thread starvation
  Details: The instruction-count worker hook interacts with shutdown so a long automation can hold the process from exiting (verifier raised LOW→MEDIUM). Re-run located it at AppController_LuaBindings.cpp:1257 (LUA_MASKCOUNT, 50000, only checks shuttingDown_) chaining to blocking JiraIssueMutation.cpp:206 — also a UI-thread block (Pillar 2) since the count-hook does not cover blocking C++ glue.
  Concrete next action: Make the hook cooperatively cancellable; bound shutdown wait with timeout/forced-join; keep blocking glue off the UI thread. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — SSRF IP denylist parses dotted-quad literals only
  Details: Source/Core/src/AiEndpointSanitize.cpp:68-96,146-152 ParseIpv4Literal matches only dotted-quad; alternate IP encodings skip the literal denylist (resolution-time block backstops; verifier MEDIUM→LOW).
  Concrete next action: Normalize via inet_pton/getaddrinfo and apply the denylist to resolved addresses. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — SubprocessCapture inherits full parent environment
  Details: Source/Core/src/Ui/SubprocessCapture.cpp:106-119,492 — children inherit the full env and a manipulable PATH.
  Concrete next action: Pass a minimal explicit environment; resolve binaries by absolute path. Effort S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — P4 executable resolved via PATH (binary planting)
  Details: Source/Core/src/Ui/P4vLaunch.cpp resolves p4/p4v via PATH search (verifier MEDIUM→LOW). Re-run also located the SearchPathW resolution at P4Annotate.cpp:49.
  Concrete next action: Resolve the binary by absolute/verified install path before spawn. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — Crash-handler minidump may include sensitive process memory
  Details: Source/Standalone/SmatchetCrashHandler.cpp:53-55 writes a minidump with flags that can capture broad process memory (in-memory secrets).
  Concrete next action: Use MiniDumpNormal scope; scrub/avoid secret-bearing regions. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — Standalone main does not fully harden DLL search path
  Details: Source/Standalone/main.cpp:1001 — incomplete DLL search-order hardening at startup.
  Concrete next action: Call SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) early. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — CLI spawn log written to predictable /tmp path (symlink race)
  Details: Source/Core/src/Commands/CliCommandRunner.cpp:481-487,538 writes a spawn log to a predictable shared /tmp path without owner-only mode. Re-run confirmed the symlink race: no O_NOFOLLOW and a predictable pid+port name at :481.
  Concrete next action: Use a per-user temp dir with O_EXCL + O_NOFOLLOW + 0600. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — MCP thread pool / SSE parking lacks connection bounds
  Details: Source/Plugins/Mcp/McpPlugin.cpp:848-850,600-620 — no clear cap on concurrent parked SSE connections / pool threads.
  Concrete next action: Bound concurrent connections and idle-park duration. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — Gradle wrapper jar/properties lack sha256 verification
  Details: gradle/wrapper/gradle-wrapper.jar + .properties are not sha256-verified in CI.
  Concrete next action: Enable SHA-pinned gradle wrapper-validation-action; pin the distribution checksum. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — Legacy AiBaseUrl grandfather path narrows SSRF guard
  Details: AiEndpointSanitize legacy AiBaseUrl branch gets looser validation; cloud-metadata payloads still blocked (informational).
  Concrete next action: Fold the legacy path through the shared sanitizer; retire the grandfather branch. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — Attachment proxy accepts URL userinfo component
  Details: Source/Plugins/Mcp/McpPlugin.cpp:275-352 accepts user:pass@ userinfo (verifier LOW→INFO).
  Concrete next action: Reject/strip userinfo before host validation. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P3 — DPAPI secret encryption uses user-scope with no entropy
  Details: Win32 CryptProtectData path uses user scope with no optional entropy; any same-user process can decrypt (same-user is in-scope for a single-user app — informational).
  Concrete next action: Optionally add per-install entropy; document the model. Effort S.
  Status: open
  Last-reviewed: 2026-06-13

<!-- --- 5-lane re-run NEW findings (not in the deep-audit block above) --- -->

- 2026-06-13 · deep-audit-rerun · [security] · P2 — ADF parser unbounded recursion on untrusted tracker JSON (Pillar 3 — Never Crash)
  Details: `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:290` (`CollectAdfText`) and `:309` (`ExtractAdfTextToStream`) recurse over server-supplied Atlassian Document Format nodes with no depth bound; deeply-nested ADF blows the stack → crash / DoS from a malicious or buggy server response. Confirmed MEDIUM, adversarially verified, NEW.
  Concrete next action: add a recursion-depth cap (reject/clamp beyond ~64 levels) to both functions; convert to an explicit work-stack if needed. Unit-test with a deep-nest fixture. ~1 h.
  Resolution: 2026-06-14 — capped BOTH walkers at `kMaxAdfRecursionDepth = 256` (threaded a `depth` param, default 0; on exceeding the cap the walker stops recursing and degrades gracefully — no throw — with a one-shot `LOG_WARN`). Picked 256 (well above any legitimate ADF nesting; real docs are a handful deep) over the ~64 suggested, to leave more headroom for legitimate-but-deep nested lists/tables while still bounding stack growth far short of overflow. Regression guards added in `tests/Core/TrackerFieldValueParser.extended.test.cpp`: two 5000-level deep-nest fixtures (one per walker entry point — `ExtractAdfTextToStream` via `NormalizeTrackerFieldValue`, `CollectAdfText` via the `ParseComments` empty-extraction fallback) parse without stack overflow, plus a shallow-doc no-regression check. Fix PR #1220.
  Status: resolved
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit-rerun · [security] · P3 — Lua child coroutine lua_State does not inherit the instruction-count hook
  Details: `Source/Core/src/AppController_LuaBindings.cpp:315,1257` — the LUA_MASKCOUNT hook is installed on the main lua_State; a `coroutine.create()`'d child State does not inherit it, so a tight loop inside a coroutine runs uncounted (sandbox timeout bypass). Partly-confirmed LOW (needs paste-and-run Lua; same-user boundary), NEW.
  Concrete next action: re-install the count hook on each created coroutine State (sol2 coroutine hook), or refuse coroutine creation in the sandbox. ~1 h.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P3 — SQLite local ticket cache stored unencrypted
  Details: `Source/Core/src/Persistence/LocalCacheManager.cpp:131` opens the local ticket cache DB with no encryption; cached ticket bodies/PII sit in cleartext in the per-user data dir. Confirmed LOW (same-user is in-scope; relevant if the file is synced/backed-up off-host). NEW.
  Concrete next action: document the at-rest model; optionally gate cache-at-rest behind SQLCipher or a no-cache mode for sensitive deployments. ~S-M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P3 — Whisper model download follows redirects with no host-pin and no size cap
  Details: `Source/Plugins/Whisper/ModelDownloader.cpp:314` uses `cpr::Redirect(true,true)` with no host pin and no maximum response size on the ggml model fetch; a redirect to an attacker host serves an unverified blob (and there is no checksum gate on the model). Partly-confirmed LOW. NEW.
  Concrete next action: pin the download host, add a size cap, and sha256-verify the model artifact (mirror the Lua TOFU pin / E3). ~S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P3 — NormalizeBaseUrl accepts cleartext http:// tracker endpoints
  Details: `Source/Core/src/Tracker/TrackerHttpUtils.cpp:85` `NormalizeBaseUrl` does not require `https://`, so a config (or first-run) `http://` tracker base sends credentials in cleartext and exposes the redirect-forwarding path (H4 / E2). Confirmed LOW. NEW.
  Concrete next action: default-reject `http://` (allow only behind the same explicit insecure-http consent gate the AI endpoint sanitizer uses), or upgrade to `https`. ~S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P3 — Logger file sink writes log lines without RedactLogLine
  Details: `Source/Core/src/Logger.cpp:320` `FileSinkWorker` writes `e.message` verbatim to the on-disk log; `RedactLogLine` (applied on the crash/bug-report paths) is NOT applied at the file sink, so any `LOG_*` that ever carries a secret/PII reaches the log file unredacted. No current `LOG_*` call places a raw credential there, but body-logging at Trace would. Partly-confirmed LOW. NEW.
  Concrete next action: route file-sink writes through `RedactLogLine` (or redact at emit for the body-logging paths). ~S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P3 — CR/LF/ANSI log injection from server-controlled data
  Details: `Source/Core/src/Privacy/TextRedaction.cpp:80` — redaction does not strip CR/LF/ANSI escapes, so server-controlled strings reaching a log line can forge log entries or inject terminal escapes. Confirmed LOW. NEW.
  Concrete next action: strip/encode CR/LF and ANSI CSI sequences in the log-line redactor. ~S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit-rerun · [security] · P3 — Redaction LongTokenRe 40-char threshold misses 36-char Plane UUID tokens
  Details: `Source/Core/src/Privacy/TextRedaction.cpp:45` `LongTokenRe` redacts only >=40-char tokens, so a 36-char Plane API UUID (and similarly-sized secrets) is not redacted if it reaches a log. Confirmed LOW. NEW.
  Concrete next action: add a UUID-shaped pattern (8-4-4-4-12) to the redactor, or lower the threshold with a git-hash guard. ~S.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-05-28 · deep-audit · [security] · P2 — JQL injection via unescaped issue keys in `JiraClient::FetchIssuesForKeys`
  Details: `Source/Core/src/Tracker/JiraIssueSearch.cpp:470-487` interpolates issue keys into double-quoted JQL literals with no escaping of `"` or `\` — single-key path builds `jql = "key = \"" + keys[offset] + "\"";` (:472) and the IN-list appends raw `'"' + keys[offset+i] + '"'` (:479-481). `UrlEncode` (`TrackerHttpUtils.cpp:44-59`) only percent-encodes for transport; the Jira server URL-decodes before JQL parsing, so an embedded `"` reaches the parser intact and breaks out of the quoted literal (e.g. `FOO" OR project=SECRET OR key="BAR`), widening the query beyond the intended key set (cross-project disclosure). Callers: `AppController.cpp:514`, `Source/Core/src/Sync/OfflineQueueService.cpp:714` (offline-queue restore). Keys are mostly server-issued today, so this is defense-in-depth / fragility rather than currently-exploitable. Verified from real code (deep-audit, adversarially confirmed).
  Concrete next action: add a `JqlQuoteLiteral()` helper in `TrackerHttpUtils` (escape `\` → `\\` then `"` → `\"`) OR validate keys against the `[A-Z][A-Z0-9]*-[0-9]+` grammar and skip non-matches before building the IN-list; unit-test it. Audit `PlaneIssueSearch` / GitHub equivalents the same way. ~1 h.
  Status: applied (2026-06-14 — `BuildKeyInJql` (`JiraIssueSearch.cpp`) now escapes every key through the shared `tracker_jql::QuoteLiteral` helper for both the single-key `=` and the multi-key `in (...)` paths. Helper lives at `Source/Core/include/Tracker/JqlEscape.h` (one canonical copy; the former file-local `EscapeJqlString` in `JiraActivityFeed.cpp` was promoted into it and all three sites — JqlSuggestEngine, JiraIssueSearch, JiraActivityFeed — now share it). Doctest `tests/Core/JqlEscape.test.cpp` covers the break-out payloads. Plane/GitHub JQL-equivalent audit left as follow-up.)
  Last-reviewed: 2026-06-14

- 2026-05-28 · deep-audit · [security] · P2 — Tracker HTTP clients follow redirects with `Authorization` attached (cross-host credential forwarding) (E2; raised P3→P2 = H4 per the 2026-06-13 audit, both fleets confirmed HIGH)
  Details: All tracker request helpers construct `cpr::Redirect redirect(true, true)` (`Source/Core/src/Tracker/TrackerHttpUtils.cpp:118,131,143,154,242`) while `BuildTrackerHeaders` attaches a Basic `Authorization` header (`BuildTrackerBasicAuthHeader`, :108-110). Because that header is a caller-set raw header (not libcurl `CURLOPT_USERPWD`), libcurl's default `CURLOPT_UNRESTRICTED_AUTH=0` does NOT strip it on cross-host redirects — a 30x from the configured tracker domain to an attacker/MITM host forwards the API token. The MCP attachment proxy already defends this with `cpr::Redirect(false,false)` (`Source/Plugins/Mcp/McpPlugin.cpp:289`); the tracker clients do not. Low severity: base Domain is user-configured (self-targeting trust boundary) and Jira/Plane/GitHub Cloud are HTTPS without cross-host auth redirects — residual risk is a compromised endpoint or an `http://` MITM. Verified (deep-audit, adversarially confirmed).
  Concrete next action: disable redirect-following on the tracker helpers (`cpr::Redirect(false, ...)`) and handle same-host redirects explicitly, OR restrict follow to same host/scheme, OR strip `Authorization` on cross-origin redirects. Mirror the proxy's posture. ~1 h.
  Status: applied (2026-06-14 — all 5 tracker verb helpers (`TrackerGetLogged` x2, `TrackerPostLogged`, `TrackerPutLogged`, `TrackerPatchLogged`) PLUS the previously-uncovered 6th sink — the multipart attachment upload at `JiraIssueMutation.cpp` ~:591 that bypasses the verb helpers — now build their redirect via a single exported `MakeTrackerRedirectPolicy()` returning `cpr::Redirect(false, false)`: redirect-following DISABLED, mirroring the MCP attachment proxy. cpr 1.9.2 exposes no same-host-only knob and `cont_send_cred=false` alone does NOT strip the caller-set RAW `Authorization` header on a cross-host 30x (UNRESTRICTED_AUTH governs only `CURLOPT_USERPWD`), so a blanket disable is the only complete fix; the Jira/Plane/GitHub REST verbs respond directly with 2xx/4xx and never depend on a 30x, so no legitimate same-host redirect is broken. A 30x now surfaces as a non-2xx the callers already handle. Shipped in the tracker-redirect PR. Note the lower-LOW siblings `ModelDownloader.cpp:314` (Whisper) and the AI-client `x-api-key` redirect remain open — distinct sinks.)
  Last-reviewed: 2026-06-14

- 2026-05-28 · deep-audit · [security] · P3 — Lua source tarball fetched with no integrity hash (only unpinned external fetch)
  Details: `CMakeLists.txt:377` `file(DOWNLOAD https://www.lua.org/ftp/lua-5.3.6.tar.gz "${_lua_tar}")` has no `EXPECTED_HASH` (grep for EXPECTED_HASH/SHA256 across `CMakeLists.txt` + `cmake/` returns nothing). Every other dependency is pinned to an immutable git ref and FontAwesome's TTF is sha256-verified in CI (`build-and-test.yml:88-98`) — Lua is the lone gap. A compromised lua.org mirror or MITM injects unverified C source compiled into both standalone + Unreal targets. Inside `if(SMATCHET_WITH_LUA_AUTOMATION)` + guarded by `if(NOT EXISTS LUA_SRC_DIR)`, so the window is first-fetch / cache-miss CI runs. Mirrors the existing Mesa-archive-integrity entry (2026-05-24). Verified (deep-audit, adversarially confirmed).
  Concrete next action: add `EXPECTED_HASH SHA256=<hash of lua-5.3.6.tar.gz>` to the `file(DOWNLOAD)` call (CMake supports it directly). One line. ~15 min.
  Status: applied (2026-06-02 — `CMakeLists.txt` lua `file(DOWNLOAD)` now carries `EXPECTED_HASH SHA256=fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60`, TOFU-pinned from the canonical upstream artifact; cache-hit builds skip the download entirely, fresh fetches are validated)
  Last-reviewed: 2026-06-02

- 2026-05-17 · security-review · [security] · P2 — First-send outbound-context consent modal (consent-tracking field + UX)
  Details: Default flip of `AssistantContextBlockAuditTrail` to `false` shipped (`ConfigManager.h:248`); remaining work from the original P1 entry is the one-time first-send consent modal. Modal should list the 5 `AssistantContextBlock*` block names + sample payload sizes + a "what gets sent" expander before the first turn. Drive via a new `cfg.AssistantOutboundConsentShown = false` field. Severity downgraded P1→P2 because the riskiest default (audit-trail PII auto-shipping) is now off.
  Concrete next action: add `cfg.AssistantOutboundConsentShown` (default false); gate `AiAssistantController::RunRequest` on the consent modal first turn; render modal in `SmatchetAiAssistantUi.cpp`. ~3 h UX.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [security] · P2 — `AgentsMdLoader` path-traversal: `ProjectAgentsMdPath` / `AgentsMdGlobalPath` accepted verbatim
  Details: A config-write attacker with access to `smatchet_config.json` can repoint either path to any readable file (`C:\Users\<victim>\.ssh\id_rsa`, browser cookies, ssh known_hosts). The first 64 KB are then silently injected into every system prompt sent to the third-party LLM. Loader at [`AgentsMdLoader.cpp:101-117`](../../../Source/Core/src/AgentsMdLoader.cpp) does no validation beyond the 64 KB cap.
  Concrete next action: require the configured path's filename to end in one of `agents.md` / `AGENTS.md` / `.agents.md` (case-insensitive); call `ghc::filesystem::canonical` and reject if the canonical path escapes a small allow-list of roots (`%LOCALAPPDATA%/Smatchet/`, repo root, `%USERPROFILE%`); reject symlinks via `ghc::filesystem::is_symlink`. ~1.5 h.
  Resolution: **applied — fix/agentsmd-path-containment** (the P1 entry above, which superseded this one). The shipped guard canonicalizes (resolving symlinks, so no separate `is_symlink` leg needed — a symlink to a secret resolves to the secret's non-`.md` real name and is rejected) and pins the resolved filename to `.md`. The proposed root-allow-list was deliberately NOT used: the override contract allows files outside any small root set (the user's own repos anywhere), so a root-containment would break valid configs while the `.md`-on-canonical pin already defeats the credential-file repoint vector this entry describes.
  Status: applied
  Last-reviewed: 2026-06-14

- 2026-05-17 · security-review · [security] · P2 — `ai.prompt` Lua glue has no rate limit + no per-session consent toast
  Details: Any Lua script (including one loaded via `Source/Plugins/LuaConsole` paste-and-run) can call `ai.prompt(...)` in a tight loop and burn the user's API quota or leak ticket data to the configured provider. `LuaAutomationHost`'s instruction-count `lua_sethook` doesn't cover the C++-side HTTP call. Sandbox escape with attacker-controlled outbound payload.
  Concrete next action: at the `ai.prompt` C++ glue site in [`AppController_LuaBindings.cpp:776-779`](../../../Source/Core/src/AppController_LuaBindings.cpp), reject calls when an in-flight prompt is already pending OR when the last `ai.prompt` fired less than ~5 s ago. Add a one-time-per-session toast on the first `ai.prompt` call naming the provider host. ~1 h.
  Resolution: applied 2026-06-14 (PR fix/ai-prompt-rate-limit-h5). The rate-limit decision was extracted to a pure header `Source/Core/include/AiLuaPromptRateLimit.h` (`DecideAiPromptGate` — re-entrancy checked first, then strict-`<` 5 s spacing via `kAiPromptMinIntervalMs`), unit-tested in `tests/Core/AiLuaPromptRateLimit.test.cpp`. Per-instance state (`aiPromptInFlight_`/`aiPromptLastCallAt_`/`aiPromptEverCalled_`/`aiPromptConsentShown_`, all under `aiPromptGateMutex_`) lives on `AppController::Impl`; the glue `LuaAiPromptGlue` calls `Impl::TryBeginLuaAiPromptTurn` BEFORE any context mutation / `PromptAi` submit and `luaL_error`s (no UI-thread block) on rejection, then `EndLuaAiPromptTurn` after submit. First accepted call fires a one-time `SmatchetToastManager` Warning toast naming `AiAssistantController::GetActiveProviderName()` (guarded `#if SMATCHET_WITH_AI`, falls back to a generic label).
  Status: applied
  Last-reviewed: 2026-06-14

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

- 2026-05-24 · coderabbit-triage · [security] · P2 — CI: Mesa archive integrity verification (upstream publishes no checksum)
  Source: CodeRabbit on PR #441 thread `PRRT_kwDORqx0G86EYIXK`. Live in `.github/workflows/build-and-test.yml:302,395` (slice-6 introduction).
  Details: `bucket-c-screenshot-diff` + `bucket-e-ui-tests` jobs `curl` a 72 MB `mesa-3d-*.7z` from the `pal1000/mesa-dist-win` GitHub release with no SHA256 / signature check. Verified via `gh release view 24.2.5 --json assets` that upstream ships zero checksums: `digest: null` on every asset, no `.sha256` companion file, no checksum in the release body. CR's suggested `MESA_SHA256: "<published-sha256>"` literally cannot be filled with a publisher-attested value. Triage-mechanical-fix envelope insufficient.
  Concrete next action: security PR must choose between (a) self-computed TOFU SHA256 pinned in workflow env (mitigates silent upstream tampering, not first-time-trust); (b) mirror the 7z to repo-controlled storage (release asset / LFS / private S3); (c) switch to a Mesa distribution that publishes signed artefacts (cosign-attested builds). Pair with the two entries above as one security PR. **P2** — supply-chain risk on every CI run, but exploit window narrow (public-repo CI, no secrets touched, output is a screenshot diff).
  Status: open
  Last-reviewed: 2026-05-24
