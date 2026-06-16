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
     2026-06-14 — AUDIT REMEDIATION CAMPAIGN DISPOSITION (single source of truth).
     The 2026-06-13 audit was worked end-to-end. Per-finding outcome below; the
     individual entries further down may still read "Status: open" where a fix
     PR updated only one of the synthesis/re-run DUPLICATE entries — THIS block is
     authoritative. (CodeRabbit / the agents also confirmed 3 audit false
     positives: P4Annotate QuoteWinArgWide line-cite stale; model checksum already
     SHA-256-enforced; MCP thread pool already bounded at 8.)

     FIXED (merged or in-flight PRs):
       H1 AgentsMd path-containment + hard-link guard ......... #1210 (merged)
       H3 JQL AccountId + E1 issue-key (shared escape) ........ #1211 (merged)
       H4/E2 tracker redirect auth-strip ..................... #1212 (merged)
       H5 ai.prompt rate-limit + consent ..................... #1221 (merged)
       ADF unbounded recursion (walkers) .................... #1220 (merged)
       ADF dump-fallback DoS (walkers' sibling, ASan crash) .. #1237
       #6 P4vLaunch QuoteWinArgWide arg-injection ............ #1222 (merged)
       #4 MCP Host/Origin DNS-rebind ........................ #1228 (merged)
       #10 OfflineQueue draft audit redaction ............... #1226 (merged)
       #9 stb decode pre-allocation dimension cap ........... #1225 (merged)
       #11 AI-client redirect auth-strip + #12 tracker error-body redaction #1232 (merged)
       log-redaction gaps (Logger sink / CRLF-ANSI / 36-char UUID) #1230
       #14 SSRF IP-encoding denylist (decimal/octal/hex/IPv6) #1229 (merged)
       #15/#19/#16 subprocess env-scrub / spawn-log race / p4 PATH #1233 (merged)
       #20 MCP SSE bound + Whisper download host-pin/size-cap + http→https #1235
       (gate-fix) CallstackParser ReDoS-sentinel ASan budget . #1215 (merged)

     ACCEPTED — per threat model §1 (same-user code is inside the trust boundary;
     these are LOW/INFO precisely because of that, and coding them adds little on
     a single-user local-first desktop app):
       DPAPI user-scope no-added-entropy (#24); DPAPI plaintext-fallback uniqueness;
       db_path "unsanitised" (the user's own %APPDATA%); SQLite local cache at-rest
       unencrypted; p4/p4vc PATH residual (partially hardened by #1233); legacy
       AiBaseUrl grandfather (cloud-metadata still blocked); attachment-proxy
       user:pass@ userinfo; gradle-wrapper-jar sha (mobile pre-release, tracked).

     DEFERRED — tracked, not coded this campaign (low-value-local or larger scope):
       Crash-handler minidump may include process memory (#17 — minidump scrubbing
       is complex + low-value local); Standalone DLL-search-path full harden (#18);
       MCP attachment-proxy SSRF (#5 — already HTTPS-only + host-allow-listed +
       redirects-disabled; confirm-only, minimal residual); Lua child-coroutine
       hook not inherited (sandbox-completeness; paste-and-run is local).

     STILL OPEN — needs action:
       * #13 Automation-worker hook → shutdown deadlock / UI-thread starvation
         (Pillar 2 MEDIUM) — NOT addressed this campaign (orchestration miss);
         remains a real open MEDIUM.
       * #2/#3 Command-registry / MCP-dispatch lack ctx.Source authz (+ the
         Unreal-console partial #24) — a TRUST-MODEL DESIGN DECISION (deny
         destructive / require-confirm / keep UI-parity), deferred for a human call.
       * The deeper-audit-playbook CANDIDATES (g_ui race, AiAssistant cancel race,
         FormatDateIfIso OOB, Plane/GitHub mapper hardening, JSON→Lua depth bound)
         are a SEPARATE deeper-audit track (not adversarially-confirmed), unchanged.
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

- 2026-06-15 · security-review · [security] · P3 — config.set / Lua direct-write paths bypassed the persist-site header/URL sanitize (defense-in-depth; resolved)
  Details: The persist-site CR/LF/NUL strip (`SanitizeConfigStringValue`, `Source/Core/src/Config/ConfigManager_PathUtils.cpp`) was applied only inside `ConfigManager::Save` (the Preferences-UI path). The MCP `config.set` command (`RunConfigSet` -> `ConfigManager::WriteConfigJson`, `Source/Core/src/Commands/Builtin/BuiltinCommands_Config.cpp:197`) and the Lua layout writer (`Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:39`) write config JSON DIRECTLY via `WriteConfigJson`, bypassing `Save` and therefore the sanitize. The `ConfigSetKeyTable` allowlist already held three URL-bound string fields written this way — `domain` -> `TrackerConfig::Domain` and `planeUrl` -> `TrackerConfig::PlaneUrl` (both spliced into outbound tracker request URLs via NormalizeBaseUrl in JiraClient / PlaneClient), and `planeWorkspaceSlug` -> `TrackerConfig::PlaneWorkspaceSlug` (concatenated RAW into every Plane workspace request path, `.../api/v1/workspaces/<slug>/projects/...`, across PlaneClient / PlaneFieldCatalog / PlaneIssueSearch / PlaneIssueMutation / PlaneActivityFeed, with NO use-site normalization — so the slug was strictly *less* guarded than the base URL, which at least passes through NormalizeBaseUrl). This is exactly the "future allowlist addition reintroduces a header/URL-smuggling bypass" case PR #1284's corrected rationale warned about — already live for these fields, pre-dating #1284 (not introduced by it). NO live vuln: the vector is blunted because the values land in a URL sink parsed by cpr/curl (which rejects/strips raw CR/LF) rather than a direct header value, and the use-site `SanitizeBaseUrlOrLog` in the tracker clients / `AiAssistantController::BuildClientConfig` remains the primary guard. Weaker than the AI-key path (where the value can reach a direct header).
  Resolution: 2026-06-15 (PR `feat/config-ingress-sanitize`) — centralized the sanitize at the write chokepoint. New pure helper `smatchet::config_detail::SanitizeHeaderBoundConfigKeys(nlohmann::json&)` (declared `ConfigManager_Internal.h`, defined `ConfigManager_PathUtils.cpp` next to `SanitizeConfigStringValue`) strips CR/LF/NUL from the header/URL-bound keys {domain, plane_url, plane_workspace_slug, ai_base_url, ai_ollama_base_url, ai_deepseek_base_url}; `ConfigManager::WriteConfigJson` now runs it on a copy before `dump(4)`. Because config.set, the Lua writer, AND Save all funnel through `WriteConfigJson`, every current + future writer of those keys is covered in one place — and any future `ConfigSetKeyTable` addition of a URL/header key in that set is sanitized automatically. The Commands strict zone is untouched (no `ConfigManager_Internal.h` leak, which that header forbids). Secret keys are excluded by design (on-disk DPAPI ciphertext has no CR/LF; POSIX plaintext is already sanitized in Save before encryption). An adversarial multi-lens verification pass over the initial commit surfaced `plane_workspace_slug` as a third config.set URL-bound key in the same sink class the initial key set had missed (and the most exposed of the three — zero use-site guard); it was added before the PR opened. Other new config.set string keys (`git_commit_repos`, `production_group_keyword`) are NOT URL-spliced and stay out; `email` is Base64-wrapped in the Basic-auth header (not an injection sink) and `github_base_url` is not config.set-reachable + has the `IsValidGitHubBaseUrl` allowlist — both correctly excluded. Doctest: `tests/Core/ConfigStringSanitize.test.cpp` gains a config.set-style round-trip stripping CR/LF/NUL from domain / plane_url / plane_workspace_slug / ai_base_url, plus an absent-key / non-string / non-object no-op case. The use-site strip stays primary; this is defense-in-depth parity hardening.
  Status: resolved
  Last-reviewed: 2026-06-15

- 2026-06-14 · build-doctor · [security] · P2 — Android config secrets stored PLAINTEXT at rest (audit H2 remainder; Keystore deferred)
  Details: Audit H2's POSIX half is now fixed (PR `fix/posix-secret-at-rest-perms-h2`: `chmod 0600` on the config file before the atomic rename + a loose-permission LOG_WARN on read, decision logic in the unit-tested `IsLooseConfigFileMode`). The **Android** half is NOT fixed: `ProtectSecretForConfig`'s `#else` branch still returns plaintext, so API tokens (`token`/`plane_api_key`/`github_pat`/`mcp_auth_token`/`ai_*_api_key`/`whisper_api_key`) land as cleartext in the app's filesDir JSON. Filesystem perms are not a reliable owner-isolation boundary on Android the way they are on a multi-user desktop, so the POSIX chmod mitigation does not transfer. The gap is now LOUD (a `LOG_WARN` fires on every Android secret write naming the gap) and Android is marked a known-unsupported platform for secret-at-rest in code + the H2 audit row, but the secret is still recoverable by anyone with filesystem access to the app sandbox (rooted device, ADB backup of a debuggable build, forensic image).
  Concrete next action: Implement Android Keystore-backed encryption-at-rest for the secret fields — generate/resolve an AES key in the AndroidKeyStore provider via JNI, wrap (GCM) the secret value before it reaches the config JSON, unwrap on load; gate behind the same `ProtectSecretForConfig`/`UnprotectSecretFromConfig` seam so the call sites are unchanged. Big JNI effort (host-injected JNIEnv plumbing through `Source/Core`); size L. MUST land before Android ships with real-account secrets (H2 raises P2->P1 the moment POSIX/Android ships).
  Status: open
  Last-reviewed: 2026-06-14

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
  Resolution: 2026-06-15 (PR #1269) — investigation confirmed the OOB read was a false positive on the candidate: `FormatDateIfIso` has carried a `value.size() >= 10` guard before the `value[4]`/`value[7]` reads since its introduction (the playbook's `:352` cite referenced a drifted line + described the function abstractly). `>= 10` strictly subsumes the `>= 8` the action requested (a 10-char floor makes both index reads + the `substr(0, 10)` in-bounds), so no functional change to the guard was needed. The genuine gap was the MISSING regression test the action called for. Hardened: `FormatDateIfIso` exported in `Source/Core/include/Tracker/TrackerFieldValueParser.h` so it is directly unit-testable; a doctest (`tests/Core/TrackerFieldValueParser.extended.test.cpp`, "FormatDateIfIso length-guards the fixed-index ISO sniff") now pins 0-/4-/7-/8-char values (the 7-char case is the regression — `value[7]` would over-read by one) plus the valid 10-char ISO date + datetime-prefix cases; a defense-in-depth comment at the sink marks the guard as a Pillar-3 boundary that must not be weakened to a bare index. UBSan/ASan exercises the doctest via the sanitizer test build.
  Status: resolved
  Last-reviewed: 2026-06-15

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
  Status: resolved
  Resolution: 2026-06-15 (PR #1271) — Confirmed unbounded: the attacker-controlled `decode_json` path (`AppController::Impl::LuaDecodeJsonBind`) called `nlohmann::json::parse`, whose recursive-descent parser builds the DOM by recursing once per nesting level with NO default depth limit — a deeply-nested payload (`[[[[...]]]]`) overflows the C++ stack BEFORE `JsonToLuaImpl` ever runs, and the existing 4 MB byte cap does not bound nesting (~2 M nested arrays fit). Fix: `LuaDecodeJsonBind` now parses through a depth/node-bounded SAX handler (`BoundedDecodeSax`, wrapping nlohmann's `json_sax_dom_parser`) that aborts past depth 256 or 200000 nodes, returning a graceful `(nil, error-string)` to Lua — no C++ throw across the sol2 boundary, no UI-thread block (Pillar 3). Both `JsonToLuaImpl` copies (`AppController_LuaBindings.cpp` + `AppController_LuaBindingsCore.cpp`) additionally gained a node-count budget for defense-in-depth on the internally-produced `commands.invoke` result path. **Follow-up in the same PR #1271 (depth-bomb at MCP/Lua ingress):** a security re-review found the SAME depth-bomb class still LIVE on three attacker-controlled ingress sites left as bare `nlohmann::json::parse(...)` — `McpPlugin::HandleToolsCall` (REST `tools/call`, raw HTTP `req.body`), the JSON-RPC POST handler (raw `req.body`), and `ExecuteLuaMcpTool`'s `paramsJson` (MCP `arguments.dump()`); the detonation happens at the FIRST parse, before `name`/`arguments` are extracted, so the `decode_json` node budget never engages and the MCP 1 MiB payload cap bounds BYTES only (~1 M nested arrays fit). The `BoundedDecodeSax` handler + parse logic were extracted into ONE shared header-only helper `smatchet::json_safe::ParseBounded` (`Source/Core/include/Json/BoundedJsonParse.h`); `decode_json`, the two MCP HTTP/JSON-RPC handlers, the Lua-MCP-params path, AND the `FakeLuaBindingHost` test fake now all route through it (DRY / Pillar 5 — no 4th copy of the SAX handler). On overflow each site degrades protocol-correctly: REST `tools/call` → its structured HTTP-400 `{isError,error}` envelope; JSON-RPC POST → `-32700` parse error; Lua-MCP-params → the path's nil+`outError` contract; each logs a constant-only `LOG_WARN` (never the raw body). The `Source/Plugins/Mcp/` strict lint zone is clean (`test-lint-rules.sh --diff origin/develop` PASS) and the Standalone GL target — which compiles `McpPlugin.cpp` (`SMATCHET_WITH_MCP=ON`) plus all three Lua-binding TUs that gained the new include — builds clean locally (MSVC 14.38). The DX12/Unreal target excludes the Mcp plugin entirely (`SMATCHET_WITH_MCP_UNREAL=OFF`) and compiles the same three Lua-binding TUs with the same `SMATCHET_WITH_LUA_AUTOMATION=ON` already proven in the Standalone build; the header adds only `<cstddef>`/`<string>`/`<nlohmann/json.hpp>` (no GLFW/OpenGL), so it cannot break the OFF/DX12 link — CI is the authoritative dual-target backstop. New pure doctest `tests/Lua/BoundedJsonParse.test.cpp` proves the shared helper rejects a depth-5000 payload + a 250k-node flat array + an oversized body without crashing, distinguishes overflow vs invalid-JSON vs too-large, and leaves valid shallow JSON byte-identical; the existing `tests/Lua/LuaBindings.test.cpp` decode_json depth/node cases still pass (the MCP handler bodies pull httplib/winsock and are not link-clean for the doctest rig, so the depth-bomb rejection is verified at the shared-helper seam they all call). Mirrors the sibling ADF depth-cap fix (`TrackerFieldValueParser.cpp`, PR #1220).
  Last-reviewed: 2026-06-15

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
  Status: resolved
  Last-reviewed: 2026-06-15
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
  Resolution: 2026-06-15 (PR #1225) — stb decode now pre-validates dimensions via stbi_info before the full decode/allocation, so an oversized image is rejected before the large allocation (memory-pressure DoS closed).
  Status: resolved
  Last-reviewed: 2026-06-15

- 2026-06-13 · deep-audit · [security] · P2 — OfflineQueue serialized 'draft' string bypasses audit-trail redaction
  Details: Source/Core/src/Sync/OfflineQueueService.cpp:356,362 serializes the draft to a JSON string before BackendAuditTrail.cpp:124-148 redaction runs, so RedactJson/LooksSensitiveKey never sees nested keys.
  Concrete next action: Redact the draft object structurally pre-serialization or add a value-level pass. Effort S-M.
  Status: resolved
  Last-reviewed: 2026-06-14
  Resolution: 2026-06-14 — QueueCreateOffline builds the audit copy via MakeAuditDraft() (anon ns, OfflineQueueService.cpp): the serialized payload is parsed back into a structured nlohmann::json object and run through BackendAuditTrail::RedactJson BEFORE it reaches the audit trail, so nested `fields` sensitive-keyed values redact (idempotent with AppendEvent's own pass; unparseable payload -> placeholder, never the raw string). The enqueued/replayed payload stays the FULL unredacted draft (replay intact). Only call-sites :356/:362 serialize a draft into the trail (the replay-create audit sites carry ids/errors only). Regression guard: tests/Core/OfflineQueueDraftAuditRedaction.test.cpp.

- 2026-06-13 · deep-audit · [security] · P2 — AI client redirect can forward Anthropic x-api-key cross-host
  Details: AiAssistantController AI-client redirect config can retain the x-api-key header across a redirect to a different host (distinct from the tracker-scoped E2/H4 item).
  Concrete next action: Strip auth headers on cross-origin redirect for all AI clients (cpr::Redirect header-stripping). Effort S.
  Status: applied (2026-06-14 — confirmed the AI provider clients set NO cpr::Redirect at all, so they used cpr's default (FOLLOW), forwarding the caller-set raw provider key (OpenAI/Whisper `Authorization: Bearer`, Anthropic `x-api-key`) on a cross-host 30x — same class as tracker H4 (#1212). Mirrored the H4 fix: added `constexpr bool kAiFollowRedirects = false` to the cpr-free `AiErrorRedact.h` and passed `cpr::Redirect{kAiFollowRedirects, false}` at ALL 8 AI-client cpr sites — OpenAiClient (Get probe + Post stream), AnthropicClient (Head probe + Post stream), WhisperApiClient (multipart Post), OllamaClient (Get probe + Post stream; keyless but covered for defense-in-depth since a user-set OpenAI-compat BaseUrl can differ). cpr 1.9.2 has no same-host-only knob and cont_send_cred=false alone does not strip a raw header, so disabling follow is the only complete fix; the AI REST/SSE verbs respond with 2xx/4xx and never depend on a 30x. Redirect call is cpr-bound (untestable in the cpr-free doctest rig, like H4); the policy constant is pinned by a doctest. Shipped in the ai-client-redirect-and-error-body-redaction PR.)
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P2 — Error/response bodies logged without key-name redaction
  Details: A backend client error-logging site (distinct from E8, which is the SSE/NDJSON parse-fail 200 B site) emits response/error bodies without RedactJson, leaking reflected tokens to logs.
  Concrete next action: Route all body logging through the redaction helper; cap length. Effort S.
  Status: applied (2026-06-14 — located the tracker HTTP clients as the unredacted backend-client body-logging surface: `LogTrackerHttpResult` (TrackerHttpUtils.cpp) logged the FULL response body (up to 64 KB) at Trace with zero redaction, and ~14 `LOG_ERROR/LOG_WARN("...body=%s", TruncateForLog(resp.text, N))` sites across GitHubActivityFeed / JiraIssueMutation / JiraIssueSearch / JiraUserAndMeta / TrackerFieldCatalog logged raw bodies (TruncateForLog truncate-only, NO key/token redaction) — a Jira 401/403 echoing the raw Basic `Authorization` header or a reflected GitHub PAT would land in logs verbatim. Added a single `RedactHttpBodyForLog()` helper to TrackerHttpUtils that delegates to the existing cpr-free `smatchet::ai::pure::RedactProviderErrorBody` (Bearer / api_key / Authorization / x-api-key / sk-/org-/ghp_… heuristics + length cap) — did NOT invent a new redactor per the audit. Routed `LogTrackerHttpResult` + every tracker `body=%s` LOG site through it. The AI provider clients (OpenAI/Anthropic/Ollama/Whisper) already redacted their error bodies via RedactProviderErrorBody, so #12 was tracker-side. The user-facing `outError += TruncateForLog(...)` strings are a separate surface left untouched (this finding is the logging site). Tested: doctest pins the tracker-shaped reflections (Basic-auth echo, ghp_ PAT) get stripped (RedactHttpBodyForLog itself is cpr-bound; its delegated redaction is the tested unit). Shipped in the ai-client-redirect-and-error-body-redaction PR.)
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P2 — Automation worker hook aggravates shutdown deadlock / UI-thread starvation
  Details: The instruction-count worker hook interacts with shutdown so a long automation can hold the process from exiting (verifier raised LOW→MEDIUM). Re-run located it at AppController_LuaBindings.cpp:1257 (LUA_MASKCOUNT, 50000, only checks shuttingDown_) chaining to blocking JiraIssueMutation.cpp:206 — also a UI-thread block (Pillar 2) since the count-hook does not cover blocking C++ glue.
  Concrete next action: Make the hook cooperatively cancellable; bound shutdown wait with timeout/forced-join; keep blocking glue off the UI thread. Effort M.
  Status: open
  Implementation note: 2026-06-15 (PR #1271) — two of three sub-parts landed; finding stays open for the third. (1) Cooperative cancel: the count-hook's pure abort policy (`LuaAutomationHookPolicyPure::DecideAutomationAbort`) already aborts the running script when `automationWorkerShuttingDown_` is set (covered by `tests/Core/LuaAutomationHookPolicyPure.test.cpp`), so a Lua-bound automation is cooperatively cancellable at the next count-hook tick — confirmed, no code change needed. (2) Bounded shutdown join: `~AppController()` no longer issues an unbounded `automationWorker_.join()`. The worker loop now sets a new `automationWorkerExited_` atomic and notifies `automationJobCv_` on exit (`AppController_LuaBindings.cpp` `AutomationWorkerLoop`, `AppControllerImpl.h`); the dtor waits on that CV for a bounded `kAutomationJoinWarnDeadline` (5 s), emits a loud `LOG_WARN` naming the likely blocking-glue cause if the worker has not exited, then joins. Shutdown is now bounded by the in-flight HTTP call's own timeout rather than hanging silently and is diagnosable from the warning (Pillar 2/3). All new state is inside `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` — Lua-OFF build and the bindings/stubs parity are unaffected (these are `AppController` methods, not new Lua glue). (3) DEFERRED — UI-thread block: the blocking synchronous tracker call at `JiraIssueMutation.cpp:206` reached from the hook chain is the Pillar-2 UI-thread-starvation half. Left untouched: moving it off the UI thread is a tracker-backend (not Lua-host) change that risks destabilizing the very shutdown path just bounded, and is a cross-subsystem redesign, not a clean lift. Flagged as a tracker-backend follow-up — re-scope as its own item under the JiraIssueMutation owner.
  Last-reviewed: 2026-06-15

- 2026-06-13 · deep-audit · [security] · P3 — SSRF IP denylist parses dotted-quad literals only
  Details: Source/Core/src/AiEndpointSanitize.cpp:68-96,146-152 ParseIpv4Literal matches only dotted-quad; alternate IP encodings skip the literal denylist (resolution-time block backstops; verifier MEDIUM→LOW).
  Concrete next action: Normalize via inet_pton/getaddrinfo and apply the denylist to resolved addresses. Effort S.
  Status: resolved
  Last-reviewed: 2026-06-15
  Resolution: 2026-06-14 (PR #1229) — replaced the dotted-quad-only ParseIpv4Literal with an overflow-safe CanonicalizeIpv4 that normalises decimal (2852039166), hex (0xA9FEA9FE), octal (0251.0376.0251.0376), dotted-hex, and inet_aton short-forms (169.254.43518) to 4 octets BEFORE the denylist, plus a ClassifyIpv6Literal that handles bracketed IPv6 incl. IPv4-mapped ::ffff:169.254.169.254, link-local fe80::/10, and ULA fc00::/7. Added RejectedPrivateNetwork verdict for RFC1918 (10/8, 172.16/12, 192.168/16) + IPv6 ULA. The integer-form parse is overflow-guarded (>cap rejected) so a denied IP cannot wrap into an allowed one. Doctest coverage in tests/Core/AiEndpointSanitize.test.cpp. Residual: DNS-rebind-to-internal (a hostname that resolves to a denied IP) is still NOT blocked — sanitize-time resolution has its own TOCTOU and the audit scoped this finding to the literal-encoding bypass; tracked separately if pursued.

- 2026-06-13 · deep-audit · [security] · P3 — SubprocessCapture inherits full parent environment
  Details: Source/Core/src/Ui/SubprocessCapture.cpp:106-119,492 — children inherit the full env and a manipulable PATH.
  Concrete next action: Pass a minimal explicit environment; resolve binaries by absolute path. Effort S-M.
  Resolution: 2026-06-14 (PR fix/subprocess-exec-hardening-wave4) — added CaptureOptions::scrubSensitiveEnv + pure SubprocessCapturePure::IsSensitiveEnvName / ScrubSensitiveEnv (drop-sensitive strategy, not a full allow-list: TOKEN/SECRET/PASSWORD/KEY/_PAT/AUTH/SESSION/COOKIE/PRIVATE/PASSPHRASE dropped; PATH/SYSTEMROOT/TEMP/locale/HOME/P4*/GIT* survive so p4+git+file-pickers keep working). Wired on in P4Annotate::P4RunCommand. argv0 already resolved to an absolute path via SearchPathW. Drop-sensitive chosen over allow-list to avoid silently breaking a tool that relies on an unlisted var. Unit tests cover the predicate + filter; end-to-end scrubbed spawn is process-bound (covered by the pure tests + compiled platform merge).
  Status: resolved
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P3 — P4 executable resolved via PATH (binary planting)
  Details: Source/Core/src/Ui/P4vLaunch.cpp resolves p4/p4v via PATH search (verifier MEDIUM→LOW). Re-run also located the SearchPathW resolution at P4Annotate.cpp:49.
  Concrete next action: Resolve the binary by absolute/verified install path before spawn. Effort S.
  Resolution: 2026-06-14 (PR fix/subprocess-exec-hardening-wave4) — both resolvers (SubprocessCapture::ResolveApplicationName, P4vLaunch::ResolveP4VcExecutableWide) ALREADY resolve a bare p4/p4vc name to its absolute SearchPathW result and hand CreateProcessW lpApplicationName / ShellExecuteW the absolute path (not a bare name the loader re-searches). Residual hardening: on a SearchPathW miss the code now LOG_WARNs that it is falling back to a PATH-based launch (binary-planting surface) instead of silently returning the bare name. Proportionate per the same-user threat model (no separate trust-store built).
  Status: resolved
  Last-reviewed: 2026-06-14

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
  Resolution: 2026-06-14 (PR fix/subprocess-exec-hardening-wave4) — ComputeSpawnLogPath now appends 16 hex chars of std::random_device entropy (SpawnLogRandomToken) so the path is unpredictable, and the open is hardened against a pre-planted file/symlink: POSIX open() gains O_CREAT|O_EXCL|O_NOFOLLOW with mode 0600 (was O_TRUNC 0644); Windows CreateFileA uses CREATE_NEW (was CREATE_ALWAYS). O_NOFOLLOW guarded with a #ifndef fallback for the rare host lacking the macro.
  Status: resolved
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit · [security] · P3 — MCP thread pool / SSE parking lacks connection bounds
  Details: Source/Plugins/Mcp/McpPlugin.cpp:848-850,600-620 — no clear cap on concurrent parked SSE connections / pool threads.
  Concrete next action: Bound concurrent connections and idle-park duration. Effort S.
  Status: applied (2026-06-14, PR network-bounds-hardening-wave4) — the httplib worker pool was ALREADY bounded at 8 (#987, StartServerThread). The residual gap was unbounded *concurrent SSE streams*: each SSE stream parks ~2 workers in the heartbeat wait-loop, so ~4 SSE clients exhaust the size-8 pool and the next connection queues forever (the #987 comment itself flagged this). Fix: `McpPlugin::RegisterSseRoute` now reserves a slot via an `std::atomic<int> activeSseConnections` guarded by the pure `CanAcceptSseConnection(currentActive)` decision (cap `kMaxConcurrentSseConnections = 4`, McpJsonRpcPure.h), rejecting the over-cap connect with HTTP 503 + `Retry-After: 5` BEFORE streaming. The slot is released by a `std::shared_ptr<void>` custom-deleter (`sseGuard`) captured into the chunked-content provider, so the decrement fires exactly when httplib destroys the provider (stream close), with no leak on the early-return paths. Pure decision unit-tested (tests/Plugins/Mcp/McpHostOrigin.test.cpp).
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
  Status: applied (2026-06-14, PR network-bounds-hardening-wave4) — SHA-256 verification already gates artefact identity (ModelCatalog, the audit's "no checksum gate" remark is a false positive — see Resolution). Added host-pin + size-cap: new pure helper `Source/Plugins/Whisper/ModelDownloadPolicy.{h,cpp}` (`IsAllowedModelUrl` = https + `*.huggingface.co`/`*.hf.co` exact-suffix allow-list; `ExceedsModelSizeCap`, ceiling 4 GiB > the 1.53 GB largest catalog model). `ModelDownloader::RunDownloadWorker` now (1) rejects a non-allow-listed/non-https initial URL before opening a socket, (2) caps redirect hops at 5 (`cpr::Redirect(5, true, false, POST_ALL)` — note redirects must stay ON: huggingface LFS 30x-bounces the `resolve/main` pointer to a CDN, so the H4 disable-follow approach is not applicable here), (3) aborts mid-stream via the WriteCallback when the running byte count crosses the cap, (4) re-checks the effective post-redirect host against the same allow-list. Pure decisions unit-tested (tests/Plugins/Whisper/ModelDownloadPolicy.test.cpp). cpr-bound redirect wiring itself is integration-only (untestable in the doctest rig).
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit-rerun · [security] · P3 — NormalizeBaseUrl accepts cleartext http:// tracker endpoints
  Details: `Source/Core/src/Tracker/TrackerHttpUtils.cpp:85` `NormalizeBaseUrl` does not require `https://`, so a config (or first-run) `http://` tracker base sends credentials in cleartext and exposes the redirect-forwarding path (H4 / E2). Confirmed LOW. NEW.
  Concrete next action: default-reject `http://` (allow only behind the same explicit insecure-http consent gate the AI endpoint sanitizer uses), or upgrade to `https`. ~S.
  Status: applied (2026-06-14, PR network-bounds-hardening-wave4) — `NormalizeBaseUrl` (TrackerHttpUtils.cpp) now upgrades a cleartext `http://` base to `https://` (with a LOG_WARN) when the host is NON-loopback, so the tracker Basic-auth header never travels in the clear to a public host; loopback `http://` (`localhost`/`127.0.0.0/8`/`::1`, local dev) is left untouched so a loopback dev config is not broken. Upgrade (not hard-reject) keeps the existing string-building contract of all 24 call sites intact. Decision extracted to the pure `TrackerHttpPure::ShouldUpgradeCleartextBase` + `IsLoopbackHost` (the latter requires a real `127.x` dotted-quad — `127.example.com` is correctly treated as a public host, a bug my own test caught). Unit-tested in tests/Core/TrackerHttpSslPure.test.cpp.
  Last-reviewed: 2026-06-14

- 2026-06-13 · deep-audit-rerun · [security] · P3 — Logger file sink writes log lines without RedactLogLine
  Details: `Source/Core/src/Logger.cpp:320` `FileSinkWorker` writes `e.message` verbatim to the on-disk log; `RedactLogLine` (applied on the crash/bug-report paths) is NOT applied at the file sink, so any `LOG_*` that ever carries a secret/PII reaches the log file unredacted. No current `LOG_*` call places a raw credential there, but body-logging at Trace would. Partly-confirmed LOW. NEW.
  Concrete next action: route file-sink writes through `RedactLogLine` (or redact at emit for the body-logging paths). ~S.
  Resolution: 2026-06-14 (fix/log-redaction-gaps-wave4) — `Logger::FileSinkWorker` now writes `smatchet::privacy::RedactLogLine(e.message)` instead of `e.message`, so the on-disk line is scrubbed on the same path the message reaches the sink. `TextRedaction.cpp` linked into the two test targets that link `Logger.cpp` (SmatchetTsanTests, SmatchetLuaTests). doctest `Logger file sink — redacts secret/long-token + strips CR/LF/ANSI on the persisted line` reads the file back and asserts the secret is gone. Status: resolved.
  Status: resolved
  Last-reviewed: 2026-06-15

- 2026-06-13 · deep-audit-rerun · [security] · P3 — CR/LF/ANSI log injection from server-controlled data
  Details: `Source/Core/src/Privacy/TextRedaction.cpp:80` — redaction does not strip CR/LF/ANSI escapes, so server-controlled strings reaching a log line can forge log entries or inject terminal escapes. Confirmed LOW. NEW.
  Concrete next action: strip/encode CR/LF and ANSI CSI sequences in the log-line redactor. ~S.
  Resolution: 2026-06-14 (fix/log-redaction-gaps-wave4) — `RedactLogLine` now runs `StripControlAndAnsi` FIRST (before the secret-shape matchers), replacing CR/LF, lone ESC, ANSI CSI/OSC sequences, and all C0 controls + DEL with a single space. Running it first means a control byte hidden mid-token cannot evade the shape matchers and cannot survive to forge a log line. doctest `RedactLogLine — strips CR/LF/ANSI…` covers CRLF + CSI + bare-ESC/C0. Status: resolved.
  Status: resolved
  Last-reviewed: 2026-06-15

- 2026-06-13 · deep-audit-rerun · [security] · P3 — Redaction LongTokenRe 40-char threshold misses 36-char Plane UUID tokens
  Details: `Source/Core/src/Privacy/TextRedaction.cpp:45` `LongTokenRe` redacts only >=40-char tokens, so a 36-char Plane API UUID (and similarly-sized secrets) is not redacted if it reaches a log. Confirmed LOW. NEW.
  Concrete next action: add a UUID-shaped pattern (8-4-4-4-12) to the redactor, or lower the threshold with a git-hash guard. ~S.
  Resolution: 2026-06-14 (fix/log-redaction-gaps-wave4) — added `UuidRe` matching the 36-char 8-4-4-4-12 hex-with-dashes shape and redacting it in `RedactLogLine` (chose the shape-specific pattern over lowering the 40-char floor, so arbitrary 36-char text is NOT over-redacted). doctest `RedactLogLine — redacts a 36-char UUID token…` asserts a Plane-style UUID is scrubbed AND a benign 36-char dash-free string survives. Status: resolved.
  Status: resolved
  Last-reviewed: 2026-06-15

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
  Status: resolved
  Resolution: 2026-06-15 (PR #1268) — added pure free function `smatchet::config_detail::SanitizeConfigStringValue` in `ConfigManager_PathUtils.cpp` (strips `\r`/`\n`/`\0`, declared in `ConfigManager_Internal.h`) and called it in `ConfigManager::Save` for all five header-bound fields just before the JSON write: `AiBaseUrl` / `AiOllamaBaseUrl` (in `SaveScalarFields`, overwriting the table-driven write) and `McpAuthToken` / `AiApiKey` / `AiAnthropicApiKey` (in `WriteSecretFields` — sanitized before DPAPI-encryption on Windows and before the plaintext write on POSIX; the empty-check that decides whether to drop the legacy plaintext key now keys off the sanitized value). Defense-in-depth behind the use-site strip in `AiAssistantController::BuildClientConfig` (PR #176). NB (rationale corrected): not every config writer funnels through `Save` — `config.set` (`RunConfigSet`) and the Lua layout writer write JSON directly via `WriteConfigJson`, bypassing `Save`. The header-bound fields are safe from those paths because they are *absent from the `config.set` allowlist* (`ConfigSetKeyTable` in `BuiltinCommands_Config.cpp`), not because of a funnel. This matters: adding a header-bound field (e.g. `aiBaseUrl`) to that allowlist later would silently reintroduce a header-smuggling bypass, so any such addition must also route through `SanitizeConfigStringValue`. New doctest `tests/Core/ConfigStringSanitize.test.cpp` (3 cases / 14 assertions: clean values unchanged incl. tabs/spaces, embedded + leading/trailing/run CR/LF/NUL stripped, control-only value sanitizes to empty). Full doctest suite 1868/1868 green; strict-zone lint clean.
  Follow-up: 2026-06-15 — #1268 missed the structurally identical DeepSeek fields. Parity-extend added `SanitizeConfigStringValue` to `AiDeepSeekApiKey` (DPAPI + POSIX branches in `WriteSecretFields`, empty-check now keys off the sanitized value like its siblings) and to `AiDeepSeekBaseUrl` (post-loop overwrite in `SaveScalarFields`); explicit DeepSeek regression case added to `ConfigStringSanitize.test.cpp`. Same correct rationale (allowlist absence, not a funnel) applies.
  Last-reviewed: 2026-06-15

- 2026-05-17 · security-review · [security] · P3 — SSE/NDJSON parse-failure `LOG_WARN` first 200 B unredacted
  Details: When a provider streams malformed JSON, the parse-failure path in `AnthropicClient.cpp:74`, `OpenAiClient.cpp:72`, and `OllamaClient.cpp:133` logs the first 200 bytes of raw `data` / `rawLine`. A misconfigured proxy could echo the request Authorization header in the malformed stream and it would land in logs.
  Concrete next action: route those log-line payloads through `smatchet::ai::pure::RedactProviderErrorBody` before emit. ~15 min, one-line change × 3 files.
  Resolution: 2026-06-15 (PR #1269) — all three SSE/NDJSON parse-failure `LOG_WARN` sites now redact before emit: `AnthropicClient::DispatchAnthropicEvent` (raw `ev.Data`), `OpenAiClient::DispatchOpenAiDataLine` (raw `data`), and `OllamaClient`'s `onParseError` lambda (raw `rawLine`) each compute `smatchet::ai::pure::RedactProviderErrorBody(...)` first, then apply the existing 200-char display cap to the REDACTED string via `%.*s`. `AiErrorRedact.h` was already included in all three files (no new include). Used the existing helper the AI clients already share for error bodies — no new redactor invented, matching the sibling resolutions (#11 redirect-strip / #12 tracker body redaction). The redactor itself is the testable unit (the cpr/stream-bound `LOG_WARN` is integration-only); its pure coverage in `tests/Core/AiErrorRedact.test.cpp` already pins Bearer / api-key / x-api-key / Authorization-echo stripping, and a new case (`Malformed SSE/NDJSON stream chunk redacts a reflected Authorization header before logging`) pins the exact malformed-stream shapes (Bearer + x-api-key echo) this fix guards.
  Status: resolved
  Last-reviewed: 2026-06-15

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

- 2026-06-15 · orchestrator · [security] · P2 — `gitleaks-over-redact-intent-output`: the prompt-intent redactor has only a hand-curated selftest — no independent secret-scanner over its OUTPUT to catch under-redaction regressions
  Details: `agents/scripts/core/redact-intent.py` is the fail-safe scrubber on the `## Intent` capture path (#1260: raw human prompt → redactor → gitignored `.session-intent/<branch>.log` → public PR body). Its only regression coverage is the in-file 58-case selftest + `tests/bats/capture_intent.bats` — both assert against patterns the AUTHOR already enumerated. Under-redaction is the whole threat model (a secret / PII leaking into a public PR body), so a defense-in-depth oracle is warranted: feed a corpus of synthetic prompts carrying planted secrets (AWS keys, bearer tokens, `user:pass@host`, home paths, SIDs) through the redactor and run an INDEPENDENT scanner (`gitleaks detect --no-git` / `gitleaks stdin`) over the redacted OUTPUT — any hit is a redaction escape the curated cases missed. Distinct from the `Install gitleaks + semgrep + flawfinder` entry (tooling.md:498), which only *installs* gitleaks for the agent's general repo scan; this is gitleaks-as-a-redaction-escape-oracle.
  Concrete next action: add `agents/scripts/core/test-redact-intent-gitleaks.sh` — pipe a planted-secret corpus through `redact-intent.py`, run `gitleaks` over the output, fail on any finding; skip cleanly when gitleaks is absent (mirror the bats-wrapper skip pattern). Pairs with the gitleaks-install entry (tooling.md:498). ~1 h incl. corpus. Cross-ref the `intent-capture-pipeline-attack-surface` entry below.
  Status: open
  Last-reviewed: 2026-06-15

- 2026-06-15 · orchestrator · [security] · P2 — `intent-capture-pipeline-attack-surface`: the new prompt→PR `## Intent` pipeline (#1260) is an un-mapped trust boundary — `security-review`'s attack surface should enumerate it
  Details: #1260 added a data flow that crosses a trust boundary into a PUBLIC artifact: the `UserPromptSubmit` hook (`docs/harness/claude-code/hooks/capture-intent.sh`) reads the raw human prompt → `agents/scripts/core/redact-intent.py` → appends to gitignored `.session-intent/<branch>.log` (`.gitignore:81`) → the ship-loop (`docs/agent-rules/ship-loops.md` § Intent capture) fills the PR body `## Intent` → advisory gate `Intent section (advisory)` in `.github/workflows/doc-validation.yml`. The raw prompt may carry secrets / PII / home paths; the PR body is public. `security-review`'s description already claims an "AI-assistant / coding-harness-handoff" surface — this pipeline is exactly that and should be named so future trust-boundary diffs route to it. Surface to map: (a) **under-redaction** → secret/PII into a public PR body (the core risk; pattern-based redactor, residual classes documented — see the `gitleaks-over-redact-intent-output` entry above); (b) **branch-name → file path** — `.session-intent/<branch>.log` is built from the branch name; verify a crafted branch (`../`, absolute) cannot path-traverse out of the capture dir; (c) **stdout discipline** — the hook MUST print nothing to stdout (it is injected into model context; capture-intent.sh:10-11 guards this) — a regression is a context-injection vector; (d) **PR-body injection** — crafted prompt markdown / control bytes flowing into the rendered PR body. (a)+(c) are mitigated today (fail-safe redactor + `exit 0` at every hook step + stdout-silent), but none is enumerated in a security-review checklist.
  Concrete next action: add an "Intent-capture pipeline" surface bullet to `agents/core/security-review.md`'s attack-surface map (the 4 vectors above) so any future diff touching `redact-intent.py` / `capture-intent.sh` / the `## Intent` ship-loop step fires a security-review pass; verify (b) branch-name sanitization explicitly (a quick `redact-intent.py` / hook read). Elevate this entry to P1/P0 if a concrete under-redaction or path-traversal escape is found. ~30 min for the surface-map edit + the (b) check.
  Status: open
  Last-reviewed: 2026-06-15
