# Smatchet C++ Security Audit

**Date:** 2026-06-26  
**Branch:** `claude/cpp-security-audit-uw1bns`  
**Scope:** First-party product C++ only — `Source/Core/`, `Source/Standalone/`, `Source/Plugins/`, `Source/UnrealPlugins/.../Source/`. External dependencies (`ThirdParty/`, vendored `nlohmann` / `httplib` / `cpr` / `sol2` / `stb` / ImGui) were excluded, as were tests and tooling.

**Coverage:** 625 first-party C++ files / ~130K LOC, partitioned into 41 LOC-balanced chunks.

> ## ✅ Remediation status — REMEDIATED (verified 2026-07-05)
>
> **All 33 findings are fixed in the current tree.** Remediation landed via **PR #1566** ("C++ security hardening — remediate SECURITY_AUDIT.md", all six slices) plus follow-ups **#1574 / #1578 / #1581** (spawn-path + screenshot-confinement fixups), **#1592 / #1598** (additional untrusted-JSON bounding in the Linear/GitHub/Jira clients), tracked in [`docs/plans/shipped/cpp-security-hardening.md`](../plans/shipped/cpp-security-hardening.md). Verified against current source on 2026-07-05:
> - **Untrusted-ingress DoS class (25 findings):** every cited network/file `nlohmann::json::parse` site now routes through `smatchet::json_safe::ParseBounded`; recursive ADF/field-value walkers carry a depth-256 cap. Slice 6 over-delivered — the `bare-json-parse-untrusted` lint is now **blocking, repo-wide default-deny** (`agents/scripts/project/test-lint-rules.sh`), so regressions are gated in CI.
> - **Path-traversal / arbitrary-write (F2/F3/F19 etc.):** MCP file-touching commands (`whisper.transcribe-once --file`, `perf.dump`, `scenario.run`, `ui_test.run`, screenshots) confined via the shared `ConfinePathUnderSubdir` (`Source/Core/include/Commands/PathConfinement.h`); `localtime`/`strftime` results null-checked.
> - **MCP authorization (F30):** `McpRequireTokenOnLoopback` config knob defaults **ON** — tokenless loopback is denied.
> - **Format-string (F1):** the locale override is specifier-validated against the trusted English source before reaching `vsnprintf`.
> - **Lua→JSON OOM (F20), integer-overflow (F17/F18), null-deref (F14), POSIX config 64 MiB cap (F33):** all guarded.
>
> **The per-finding "Verification (independent re-check)" lines below are the original *audit-time* skeptic re-checks (confirmed / partially-confirmed / refuted), NOT remediation status** — every finding was independently confirmed a real defect at audit time and has since been fixed. No finding remains open.

## Methodology

Agentic deep search executed as a deterministic multi-agent workflow, then independently re-verified:

1. **Audit fan-out** — 41 independent **Opus 4.8 / max-effort** agents, one per code partition, each reading every file in its chunk in full and hunting for memory-safety, injection, deserialization, integer, concurrency, secret-handling, and resource-exhaustion defects with a concrete untrusted-input path.
2. **Triple-check verification** — every one of the 33 findings was then handed to a **separate Opus 4.8 / max-effort skeptic** instructed to *refute* it: re-read the cited source and callers/guards, confirm the unsafe operation is present and unguarded, confirm the untrusted input can actually reach it, and check line-number and severity accuracy.
3. **Orchestrator spot-check** — the distinctive non-DoS findings (path traversal, arbitrary write, format-string, Lua OOM, the merge-watch notify endpoint) were additionally hand-read against source by the orchestrator (marked **✓ hand-verified**).

### Verification outcome

**0 of 33 findings were refuted** — every finding is a real defect. 18 confirmed as described; 15 partially-confirmed (real defect, correct fix, but a detail of the *description* was refined — see each finding's **Verification** line). Independent severity matched the auditor on **all 33**. One line-number correction was applied (Jira search finding).

> **Mechanism correction (applies to the whole `unbounded-recursion-DoS` class).** Several findings originally described nlohmann-json as a *recursive-descent parser that overflows the stack during parsing*. Per the codebase's own `Source/Core/include/Json/BoundedJsonParse.h` (lines 6–14), that is **not** how the crash happens: nlohmann's parser is **iterative** (SAX-driven, no per-level parse recursion), so the stack does **not** overflow while parsing. The uncatchable crash instead comes from the resulting **DOM**: `~json` destroys nested containers **recursively**, so tearing down a deeply-nested tree overflows the C++ stack (a `SIGSEGV`, *not* a catchable C++ exception), and building it grows the heap unboundedly. A byte cap alone does not bound depth (~1M nested arrays fit in ~1 MiB). The conclusion and the fix are unchanged: route the parse through `smatchet::json_safe::ParseBounded`, which drives the DOM builder from `sax_parse` and aborts past a depth/node cap *before* a deep DOM is ever constructed.
>
> **Severity framing.** No remotely-exploitable memory-corruption or RCE was found. The dominant class is denial-of-service (Pillar-3 "never crash"), generally requiring a hostile/compromised/MITM'd network peer over TLS or a local IPC/file vector. Severities reflect that.

## Summary

**33 findings** — Medium: 15, Low: 17, Info: 1. Verification: 18 confirmed / 15 partially-confirmed / **0 refuted**.

| Category | Count |
|---|---|
| unbounded-recursion-DoS | 25 |
| path-traversal | 2 |
| integer-overflow | 2 |
| unbounded-allocation-DoS | 1 |
| deserialization | 1 |
| null-deref | 1 |
| format-string | 1 |

### Key themes

- **Bare `nlohmann::json::parse` on untrusted ingress (25 findings).** Tracker HTTP responses (Jira / Plane / Linear / GitHub), AI streaming bodies (Anthropic SSE / OpenAI SSE / Ollama NDJSON), the merge-watch notify HTTP endpoint, and several local cache/config files are parsed without the depth/node-bounded `smatchet::json_safe::ParseBounded` that the MCP server already uses. A deeply-nested payload crashes the process on recursive **DOM destruction** (see the mechanism correction above). The fix is uniform: route these sites through `ParseBounded` (or `sax_parse` with a depth cap), per `Source/Core/include/Json/BoundedJsonParse.h`.
- **MCP command surface lacks per-source authorization (2 findings).** When the MCP server is enabled it binds to loopback and, with no auth token configured, accepts **any local process** unauthenticated (verified in `Source/Plugins/Mcp/McpPlugin.cpp` `Authorize`). `tools/call` dispatches straight into the global command registry, so file-touching commands — `whisper.transcribe-once` (reads an arbitrary path and exfiltrates it to a cloud transcription API) and `perf.dump` (writes an attacker-chosen path) — are reachable with no path confinement or command allow-list.
- **Format-string from locale override files (1 finding).** A translated string loaded from `Locales/`<lang>`.json` is used directly as the `printf` format in `SmatchetLocalization::Format`.
- **Unbounded allocation in Lua→JSON conversion (1 finding).** A sparse Lua array index densifies into billions of `push_back`s (`LuaToJsonImpl` has a depth cap but no node budget, unlike its inverse `JsonToLuaImpl`).

---

## Findings

### 1. [Medium] Format-string vulnerability: locale override strings used as printf format in SmatchetLocalization::Format — **✓ hand-verified**

- **File:** `Source/Core/src/SmatchetLocalization.cpp:1150-1169`
- **Symbol:** `SmatchetLocalization::Format`
- **Category:** format-string | **Auditor confidence:** high
- **Attack vector:** Crafted Locales/<lang>.json override file placed in the runtime asset directory (local file ingress; e.g. shared/source-controlled asset dir, network share, or a malicious locale pack). The override string flows through SetLanguage->LoadOverridesLocked->OverridesRef into T() and becomes the vsnprintf format string.

Format() obtains its format string from `T(key, englishFallbackFmt)`, then passes it straight to vsnprintf with caller-supplied varargs. T() returns an OVERRIDE string first if one exists (see T() lines 1104-1108), and overrides are populated by LoadOverridesLocked() (lines 1007-1031) from an attacker-influenceable JSON file at `<RuntimeAssetDir>/Locales/<lang>.json` — every key->string pair in `root["strings"]` becomes an override, with no validation that the override preserves the original conversion specifiers. Many call sites pass a format string containing specifiers (e.g. keys like "window.views_backend" = "Views - %s", "ai.model" = "Model: %s", "perf.network.intro.help" = "...~%llu B...", "audit.page_count" = "Page %d/%d (%zu rows)"). A crafted override that changes/adds specifiers — e.g. supplying "%s %s %s %n" where the C++ caller only passes one int — makes vsnprintf read…

**Recommendation:** Do not trust override strings as format strings. Either (a) reject/skip any override whose conversion-specifier sequence differs from the built-in English entry's, or (b) render placeholders with a non-printf templating scheme ({0}-style is already used for some keys) and never feed locale-loaded strings to a printf-family function. At minimum, validate…

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Confirmed as described. The only nuance is the precondition: exploitation requires the attacker to control a local Locales/<lang>.json file in the runtime asset directory (local file ingress, not remote). The auditor's attack_vector states this correctly. The %n arbitrary-write claim is theoretically valid on platforms where the libc vsnprintf honors %n (note: many hardened libc builds, e.g. glibc with _FORTIFY_SOURCE on writable format strings, or MSVC by default, disable %n), but even without %n the…

### 2. [Medium] whisper.transcribe-once --file allows arbitrary local file read reachable from the MCP JSON-RPC tool endpoint — **✓ hand-verified**

- **File:** `Source/Plugins/Whisper/WhisperPlugin.cpp:173-200`
- **Symbol:** `ReadWavFile / AcquireTranscribeOnceAudio / RunTranscribeOnce`
- **Category:** path-traversal | **Auditor confidence:** high
- **Attack vector:** MCP JSON-RPC / REST tools/call request -> Commands().Dispatch("whisper.transcribe-once", {file: "/etc/passwd" | "C:/Users/.../config"}) -> AcquireTranscribeOnceAudio -> ReadWavFile opens attacker path -> bytes uploaded to api.openai.com

AcquireTranscribeOnceAudio() reads `args.value("file", ...)` and passes it verbatim to ReadWavFile(), which opens whatever absolute or relative path the caller supplies (no allow-list, no base-dir confinement, no traversal check). The resulting bytes are then POSTed to OpenAI's Whisper endpoint by client.Transcribe(). The whisper.transcribe-once command has AsyncSafe=true and is registered in the global CommandRegistry, which McpPlugin exposes at POST /mcp/tools/call and dispatches via Commands().Dispatch(name, arguments, cctx) (Source/Plugins/Mcp/McpPlugin.cpp:481/499/596). An MCP client therefore controls the `file` argument and can request reads of arbitrary files (e.g. C:\Users\<user>\.ssh\id_rsa, the Smatchet config holding API keys, etc.); the file content is then exfiltrated to a third-party cloud endpoint. There is no size cap on the read either…

**Recommendation:** Confine `--file` reads to an allow-listed directory (e.g. the user-data/whisper dir or an explicit downloads dir), reject absolute paths and any path containing '..', canonicalize and verify the resolved path stays under the base, and cap the read size before resize(). Alternatively gate the file-input path behind a non-MCP-exposed/trusted-only command flag.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Finding is real and accurately described, with one important precondition the auditor understated. The arbitrary-read primitive itself (no allow-list / no `..` check / no base confinement / no canonicalization / unbounded resize) is fully unguarded. But the file is only actually READ + EXFILTRATED on the cloud path, which is gated AFTER a consent check: RunTranscribeOnce (WhisperPlugin.cpp:512-516) calls consent::CanCallCloudApi BEFORE AcquireTranscribeOnceAudio (line 523), and CanCallCloudApi…

### 3. [Medium] perf.dump writes to an unrestricted caller-supplied path (arbitrary file write / path traversal) — **✓ hand-verified**

- **File:** `Source/Core/src/Commands/Builtin/BuiltinCommands_Perf.cpp:125-165`
- **Symbol:** `RegisterPerfDumpCommand`
- **Category:** path-traversal | **Auditor confidence:** medium
- **Attack vector:** MCP JSON-RPC tools.call perf.dump (or scenario.run/ui_test.run) with a crafted outPath when MCP is enabled (and, for off-host, mcpAllowRemote=true); also CLI `cmd perf.dump --outPath=...` and Lua commands.invoke

The perf.dump handler takes an `outPath` string argument straight from the command args and passes it to fs::create_directories(parent_path) + std::fopen(outPath.c_str(), "wb") + fwrite with no validation, normalization, or confinement to the user-data directory. A value like "../../../../home/user/.bashrc" or an absolute path overwrites an arbitrary file the process can write. The written content is perf-row JSON (not attacker-controlled bytes), so impact is limited to clobbering/creating a file with benign JSON content rather than injecting arbitrary data, but it is still an out-of-tree write/DoS primitive. The same unconfined-outPath shape is exposed by scenario.run (outPath/outLog) and ui_test.run. These commands are reachable from CLI, Lua automation, and — when McpEnabled (optionally McpAllowRemote) — the MCP JSON-RPC server, giving a non-local trigger path.

**Recommendation:** Reject absolute paths and any path containing '..' components, and/or resolve outPath against ConfigManager::GetUserDataDirectory() and verify the canonicalized result stays within an allowed output root before opening the file. Apply the same confinement to scenario.run outPath/outLog and ui_test.run outPath.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Finding is accurate as written. Cited lines 125-165 exactly bound RegisterPerfDumpCommand and the unsafe fopen/fwrite. The auditor honestly notes the limited impact (benign JSON content, not attacker bytes), which keeps it at Medium rather than High. I did not independently re-verify the scenario.run/ui_test.run claims, but the perf.dump core claim is fully confirmed. The recommendation (reject absolute/'..' paths or confine under GetUserDataDirectory with canonicalization) is correct.

### 4. [Medium] Unbounded-recursion JSON parse of server-returned rich field value (stack-overflow DoS)

- **File:** `Source/Core/src/Sync/OfflineQueueService.cpp:128-148`
- **Symbol:** `OfflineFieldEditMergeDetail::RichToMarkdown`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Malicious or compromised tracker server (or a man-in-the-middle / SSRF-reachable host configured as the tracker base URL) returns a ticket field whose rich/ADF value is deeply nested JSON. During offline-field-edit conflict resolution the value is re-parsed by RichToMarkdown on a background thread, overflowing the…

RichToMarkdown calls nlohmann::json::parse(rich) at line 134 with the library default recursion limit (effectively unbounded; nlohmann parses the DOM recursively). It is invoked on `theirsRich` in ResolveFieldEditThreeWayMerge (OfflineQueueService.cpp:1063), where `theirsRich = fresh.GetFieldRichValue(fid)` is the rich/ADF content of a ticket field that was fetched from the tracker backend over HTTP (EvaluateFieldEditConflict re-fetches the issue via reader->FetchIssuesForKeys at line 917 and stores the field value verbatim). The surrounding try/catch at lines 133-141 catches nlohmann parse exceptions but does NOT catch a stack overflow: a deeply nested JSON value (e.g. tens of thousands of nested arrays/objects in an ADF field) exhausts the call stack during the recursive parse and crashes the process with SIGSEGV. This runs inside the offline-field-edit replay background task…

**Recommendation:** Parse untrusted (network-origin) JSON with an explicit depth cap, e.g. nlohmann's SAX interface with a nesting-depth limit, or pre-scan the string and reject inputs exceeding a sane bracket-nesting depth before calling parse. Apply the same bound to the conflict-resolution and replay parse sites that can see server data.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Medium** (matches). Real defect class (unbounded network-JSON parse not routed through the existing json_safe::ParseBounded; uncatchable crash), but the finding's mechanism description is wrong: nlohmann's parser is iterative (no parse-time stack overflow, and the catch(...) would catch parse exceptions); the actual crash is the recursive ~json destructor on the deep DOM plus unbounded heap growth — exactly what BoundedJsonParse.h (Source/Core/include/Json/BoundedJsonParse.h, lines 6-14) was written to prevent. Reachability is also…

### 5. [Medium] Unbounded nlohmann::json::parse on Jira HTTP search/comment responses (depth-bomb stack-overflow crash)

- **File:** `Source/Core/src/Tracker/JiraIssueSearch.cpp:41, 88, 116, 166, 429, 458, 493`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** HTTP response from the configured Jira server reaches these parse sites on the sync worker thread. Triggered by a malicious/compromised Jira tenant, a self-hosted/proxy URL the user was tricked into configuring (cfg.Domain), or a network MITM, returning a deeply-nested JSON body.

FetchIssuesStreamed / FetchIssuesForKeys / FetchIssueComments / JiraDiagnoseZeroIssueResult all decode the tracker server's HTTP response body with a plain nlohmann::json::parse(response.text) (e.g. JiraFetchIssueCommentsPages line 41, ProcessJiraSearchPage line 88, FetchIssuesForKeys lines 429/458, /myself line 166). The codebase ships smatchet::json_safe::ParseBounded (Source/Core/include/Json/BoundedJsonParse.h) precisely for attacker-controlled ingress, whose header documents that a deeply-nested payload ([[[[...]]]]) builds a deep DOM that is then destroyed RECURSIVELY by nlohmann's container destructor — overflowing the C++ stack, which is a hard crash, NOT a catchable exception. The surrounding try/catch blocks therefore do NOT mitigate this, and TrackerHttpUtils imposes no response-size cap. ~1M nested arrays fit in well under 1 MiB so a byte cap alone would not help either.

**Recommendation:** Route all tracker-response decoding through smatchet::json_safe::ParseBounded (depth + node caps) instead of bare nlohmann::json::parse; treat ParseBounded's error as a parse failure. Apply uniformly across the Jira/Plane/Linear response parses.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; line correction → Source/Core/src/Tracker/JiraIssueSearch.cpp:41,88,166,429,458 (parse sites). Line 116 is "return outcome;" in the cancel branch — NOT a parse. Line 493 is the call to…; independent severity **Medium** (matches). Description mechanism is correct and matches the codebase's own documented threat model in BoundedJsonParse.h. Two cited line numbers (116, 493) are inaccurate — 116 is a return statement, 493 is a function call. The five genuine parse sites (41, 88, 166, 429, 458) fully substantiate the finding. Recommendation (route through json_safe::ParseBounded) is correct and consistent with existing MCP/Lua usage.

### 6. [Medium] Unbounded nlohmann::json::parse on Jira mutation HTTP responses (depth-bomb stack-overflow crash)

- **File:** `Source/Core/src/Tracker/JiraIssueMutation.cpp:131, 528`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** HTTP response from the configured Jira server during an issue transition/create (automation worker or UI-initiated mutation). A malicious/compromised tracker server or MITM returns a depth-bomb body.

UpdateIssueFieldsViaTransition parses the transitions endpoint body with nlohmann::json::parse(transitionsResp.text) (line 131) and CreateIssue parses the create response with nlohmann::json::parse(response.text) (line 528). As documented in BoundedJsonParse.h, a deeply-nested response triggers recursive DOM teardown → stack overflow, which the try/catch around these calls cannot intercept (it is not a C++ exception). No response-size cap exists upstream.

**Recommendation:** Decode mutation responses via smatchet::json_safe::ParseBounded rather than bare nlohmann::json::parse.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Both cited lines are accurate. JiraIssueMutation.cpp:131 `auto transitionsJson = nlohmann::json::parse(transitionsResp.text);` (inside UpdateIssueFieldsViaTransition) and :528 `auto j = nlohmann::json::parse(response.text);` (inside CreateIssue) both call bare nlohmann::json::parse on full HTTP response bodies. Each is wrapped in try/catch (lines 140 / 543 catch std::exception), but a stack overflow from recursive DOM teardown is NOT a C++ exception and cannot be intercepted — this exact failure mode is documented…

### 7. [Medium] Unbounded nlohmann::json::parse on Jira user/meta HTTP responses (depth-bomb stack-overflow crash)

- **File:** `Source/Core/src/Tracker/JiraUserAndMeta.cpp:47, 116, 167, 232, 281, 349, 404, 458`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** HTTP responses from the configured Jira server reaching these read endpoints (some run on the UI thread via the user-info window). A malicious/compromised tenant or MITM returns a depth-bomb JSON body.

FetchUsers, FetchIssueWatchers, AddIssueWatcher (/myself), FetchIssueEditMeta, FetchIssueVotes, SearchUsersByQuery, FetchUserGroupNames, and FetchGroupMembers each decode the Jira server's HTTP body with a plain nlohmann::json::parse(...). Per BoundedJsonParse.h, a deeply-nested body causes recursive DOM destruction and a stack-overflow crash that the surrounding try/catch cannot catch; no upstream byte/depth cap is applied.

**Recommendation:** Replace bare nlohmann::json::parse with smatchet::json_safe::ParseBounded for all of these response decodes.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Finding is accurate as described; all 8 line numbers point exactly at the bare json::parse calls on server-controlled bodies. The try/catch present at each site does not mitigate, consistent with the auditor's claim. Caveat on exploitability: the JSON source is the user's own configured Jira tenant reached over TLS, so the realistic threat is a compromised/malicious tenant or (harder, TLS) MITM rather than an arbitrary internet attacker — trust level is higher than the MCP/Lua ingress ParseBounded was originally…

### 8. [Medium] Unbounded nlohmann::json::parse on Plane activity-feed HTTP responses (depth-bomb stack-overflow crash)

- **File:** `Source/Core/src/Tracker/PlaneActivityFeed.cpp:71, 156`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** HTTP responses from the configured Plane server (self-hosted PlaneUrl is user-supplied and may be attacker-controlled, or a compromised cloud tenant / MITM) returning a deeply-nested JSON body.

ScanIssueActivities (line 71) and FetchUserActivity discovery loop (line 156) parse the Plane server's per-issue /activities/ and work-item list responses with nlohmann::json::parse(activityResp.text) / (resp.text). The try/catch only catches malformed-JSON exceptions; a deeply-nested payload instead triggers recursive DOM teardown and a stack overflow (see BoundedJsonParse.h), which is an uncatchable remote crash. ResolvePlaneProject (PlaneClient.cpp line 100) uses the non-throwing parse(.., nullptr, false) which still constructs and recursively destroys the deep DOM, so it is equally exposed.

**Recommendation:** Decode all Plane response bodies via smatchet::json_safe::ParseBounded instead of bare nlohmann::json::parse (throwing or non-throwing).

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). All three cited locations confirmed present and unguarded. The description's technical reasoning matches BoundedJsonParse.h's own documentation verbatim (iterative parse, recursive DOM teardown overflow, uncatchable). Recommendation to route through json_safe::ParseBounded is correct and consistent with existing usage in CommandRegistry.cpp, McpPlugin.cpp, and the Lua bindings.

### 9. [Medium] Non-throwing nlohmann::json::parse on Linear GraphQL responses still exposed to depth-bomb DOM-teardown crash

- **File:** `Source/Core/src/Tracker/LinearIssueSearch.cpp:178`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** GraphQL HTTP response from the configured Linear endpoint (cfg.LinearBaseUrl is user-configurable; default api.linear.app, but a self-hosted/proxy override or MITM can return a depth-bomb body) on the sync/mutation worker threads.

ParseIssuesPage decodes the Linear GraphQL response with nlohmann::json::parse(responseText, nullptr, false). The allow_exceptions=false form prevents an exception on malformed input but still constructs the full DOM and then destroys it recursively; per BoundedJsonParse.h, a deeply-nested response ([[[[...]]]]) overflows the stack during teardown — an uncatchable crash. The same non-throwing-parse pattern is used across LinearClient.cpp (ProbeReachability l169, ListProjects l238, ResolveCatalogTeamId l448, FetchFieldCatalog l496) and LinearIssueMutation.cpp (RunLinearMutation l54, ResolveIssueUuid l82), and ExtractLinearErrorMessage in LinearClientHelpers.cpp l167. No response-size or depth cap is applied upstream.

**Recommendation:** Route Linear response decoding through smatchet::json_safe::ParseBounded so depth/node caps abort before a deep DOM is built or destroyed.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Confirmed as described. Line 178 of LinearIssueSearch.cpp is exactly `nlohmann::json parsed = nlohmann::json::parse(responseText, nullptr, false);`. The codebase's own BoundedJsonParse.h (lines 6-14) documents the precise depth-bomb mechanism: nlohmann's parse is iterative so it does not overflow during parse, but the resulting DOM's `~json` destructor tears down nested containers RECURSIVELY, overflowing the C++ stack as an uncatchable remote crash (not a catchable exception). The `is_discarded()` check at line…

### 10. [Medium] Unbounded JSON parse of network-controlled tracker field values enables remote stack-overflow DoS (depth bomb)

- **File:** `Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp:101-549`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Malicious or compromised tracker server (or MITM) returns an issue whose field value (attachments/worklog/watchers/votes/issuerestrictions/progress, or any string field that gets double-decoded) contains deeply-nested JSON; on grid render the bare nlohmann parser builds the deep DOM and the recursive destructor…

Every attachment/watchers/votes/worklog/issuerestriction/progress render-model builder parses the cell's currentValue via TryParseJsonMaybeDoubleEncoded (JsonParseUtil.h, lines 101, 125, 190, 218, 259, 468, 549), which calls bare nlohmann::json::parse(raw, nullptr, false) with NO depth/node cap, and may parse a second time for the double-encoded string case. These currentValue strings are tracker issue field values originating from the tracker server's JSON response (network ingress; a compromised/malicious tracker host or a MITM with a forged cert chain can return arbitrary field bytes). The project's own BoundedJsonParse.h documents that an unbounded parse of a deeply-nested payload builds the full DOM and then overflows the C++ stack during recursive ~json destruction, a remote crash that is NOT a catchable C++ exception (the surrounding try/catch in…

**Recommendation:** Route these parses through smatchet::json_safe::ParseBounded (or make TryParseJsonMaybeDoubleEncoded itself depth/node-bounded), applying kDefaultMaxDepth/kDefaultMaxNodes, and treat overflow as a non-parsed value rather than crashing.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Description minor inaccuracy: it labels the line numbers as \"JsonParseUtil.h, lines 101, 125, ...\" but those are the call-site lines in TrackerGridFieldDisplay.cpp; the parse helper itself is JsonParseUtil.h:7-22. Substantively correct. Also note the same unbounded-parse class already exists at the network-ingress parse (JiraIssueSearch.cpp:88), so the render path is one of multiple reachable sinks, not the only one.

### 11. [Medium] Unbounded recursion in AdfToMarkdown walker over server-supplied ADF JSON (stack-overflow DoS)

- **File:** `Source/Core/src/Ui/MarkdownConvert.cpp:1043-1053,1079-1098,1122-1165,1204-1209,1935-1951`
- **Symbol:** `EmitAdfBlock / EmitAdfChildren / EmitListItemChildren / EmitAdfBlockquote / AdfToMarkdown`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Compromised or malicious Jira/tracker server (or attacker who can set an issue description / long-text rich field) returns an ADF JSON value with very deep nesting; on rendering/preview the value is parsed and passed to AdfToMarkdown, whose unbounded recursion overflows the call stack and crashes the app.

AdfToMarkdown() recursively walks an ADF (Atlassian Document Format) JSON tree with no depth limit: EmitAdfBlock -> EmitAdfChildren -> EmitAdfBlock, and the list/blockquote handlers (EmitAdfBulletList, EmitAdfOrderedList, EmitAdfListItem->EmitListItemChildren, EmitAdfBlockquote) each re-enter EmitAdfBlock for nested children. The recursion depth equals the JSON nesting depth. The ADF passed in is server-controlled: the immediate caller (Source/Core/src/TicketFieldEditorLongTextPure.cpp:39-40) does a bare nlohmann::json::parse(rich) on a stored rich-text field value (Jira/tracker issue description ADF received from the tracker server) and feeds the result straight into AdfToMarkdown. A maliciously or accidentally deeply-nested ADF document (e.g. thousands of nested blockquote/bulletList/listItem objects) drives the native call stack to overflow. The surrounding try/catch…

**Recommendation:** Add an explicit recursion-depth counter to AdfWalkState (and a matching cap, e.g. 64-128) checked at the top of EmitAdfBlock/EmitAdfChildren; on exceeding it, record a dropped-node entry and stop descending instead of recursing. Equivalently, rewrite the walk to an explicit work-stack. Also bound nesting at parse time upstream (e.g. nlohmann…

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Medium** (matches). Finding is accurate as described. Note the upstream nlohmann::json::parse at TicketFieldEditorLongTextPure.cpp:39 (and the discarded-mode parse at line 21 in ClassifyRichValue) would themselves stack-overflow during parse on the same deeply-nested input — in practice the parse-time recursion may overflow before the walker is even reached, but both share the same root cause and both must be bounded (recommendation correctly calls for parse-time depth limiting as defense in depth). Recommendation to add a depth…

### 12. [Medium] Unbounded-recursion stack overflow: bare nlohmann::json::parse on NDJSON stream lines (Ollama native) — **✓ hand-verified**

- **File:** `Source/Core/src/AiNdjsonParser.cpp:16-25`
- **Symbol:** `AiNdjsonParser::emitOneLine`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** LLM streaming HTTP response body. The Ollama-native base URL is user-configurable and unpinned (no host pin, loopback/private allowed), so a malicious or compromised local/remote model server — or a MITM on a plain-http endpoint — returns a deeply-nested NDJSON line that the worker thread parses and crashes on.

Each NDJSON line received from the LLM provider's streaming response is parsed with a bare `nlohmann::json::parse(line)` (line 21). nlohmann's default parser is recursive-descent; deeply-nested JSON (e.g. `[[[[[...` repeated tens of thousands of times) exhausts the call stack and SIGSEGVs the process. A single line may be up to the 4 MiB buffer cap (kAiNdjsonParserMaxBufferBytes), which is far more than enough nesting to overflow any thread stack. The surrounding try/catch only catches std::exception (parse_error) — a stack overflow is NOT a C++ exception, so it is not contained and crashes the whole app. The codebase already ships `smatchet::json_safe::ParseBounded` (Source/Core/include/Json/BoundedJsonParse.h) precisely for this, and AGENTS.md documents a lint rule (`bare-json-parse-untrusted`, issues #1271/#1287) stating a bare parse on HTTP/network ingress 'stack-overflows the…

**Recommendation:** Route the line parse through smatchet::json_safe::ParseBounded(line, err, /*maxBytes=*/...), which enforces a nesting-depth cap before the recursive parser is entered, and treat a rejection like the existing onError path. Do not rely on the try/catch — depth-overflow is not catchable.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Medium** (matches). Partially-confirmed: real bug, correct severity, correct fix. The description's mechanism is inaccurate — per the project's own BoundedJsonParse.h the recursion that overflows is the json DOM destructor / deep-tree teardown after the iterative sax parse builds it, not a recursive-descent parser. Also the actual nlohmann::json::parse call is on line 20; line 21 is the catch. These do not change the exploitability or the remediation.

### 13. [Medium] Unbounded-recursion stack overflow: bare nlohmann::json::parse on Anthropic SSE event data

- **File:** `Source/Core/src/AnthropicClient.cpp:71-87`
- **Symbol:** `DispatchAnthropicEvent`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** Anthropic /v1/messages SSE response stream. Although the Anthropic host is pinned to api.anthropic.com over HTTPS by default, the AiAllowCustomEndpointAnthropic config flag lets the user repoint the base URL at an arbitrary host (or a self-hosted Anthropic-compatible gateway), and a MITM/compromised gateway then sends…

Each decoded SSE event's data payload is parsed with a bare `nlohmann::json::parse(ev.Data)` (line 73) on the streaming worker thread. ev.Data is the accumulated `data:` lines of one SSE frame; the SSE parser caps only the total buffer at 4 MiB (kAiSseParserMaxBufferBytes), so a single frame's JSON can be large and arbitrarily nested. nlohmann's recursive-descent parser stack-overflows on deep nesting (`{"a":{"a":{...` etc.) well before 4 MiB, SIGSEGVing the app. The try/catch (lines 72-87) catches only std::exception/parse_error and redacts the message for logging; it does NOT and cannot contain a stack-overflow crash. Same documented bug class as #1271/#1287; the canonical fix helper smatchet::json_safe::ParseBounded already exists in the tree.

**Recommendation:** Parse ev.Data via smatchet::json_safe::ParseBounded with a depth/size cap and feed a rejection into the existing malformed-data warning path instead of nlohmann::json::parse directly.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Medium** (matches). Real bug, but the described crash MECHANISM is wrong. The finding says "nlohmann's recursive-descent parser stack-overflows on deep nesting... well before 4 MiB." Per the codebase's own authoritative doc in Source/Core/include/Json/BoundedJsonParse.h lines 6-14, nlohmann's parser is ITERATIVE (drives the DOM builder from sax_parse, not per-level recursion), so the parse does NOT stack-overflow. The actual crash is (a) the RECURSIVE ~json destructor tearing down the deep DOM, and/or (b) unbounded heap growth (~1M…

### 14. [Medium] Unbounded nlohmann::json::parse on network ingress in merge-watch notify endpoint (recursion-DoS) — **✓ hand-verified**

- **File:** `Source/Core/src/SmatchetMergeWatchNotifyServer.cpp:78-84`
- **Symbol:** `SmatchetMergeWatchNotifyServer::RegisterRoutes (POST /merge-watch/notify handler)`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** HTTP POST to 127.0.0.1:<Mcp/notify port> /merge-watch/notify with a deeply nested JSON body from any local process. The body reaches nlohmann::json::parse unbounded.

The POST /merge-watch/notify handler calls `nlohmann::json::parse(req.body)` directly on the raw HTTP request body with no nesting/size bound. nlohmann's default parser is recursive-descent; a deeply nested payload such as thousands of `[` characters drives unbounded recursion and overflows the worker-thread stack, crashing the process (the try/catch only catches parse_error, not the stack overflow). This is the exact bare-parse-on-network-ingress class flagged for this codebase. The server binds 127.0.0.1 only, so the attack surface is any local process able to reach the loopback port (the merge-watcher daemon, other local users/containers, or attacker-influenced data relayed by the daemon), but it still turns a malformed local request into a hard crash, violating Pillar-3.

**Recommendation:** Parse with a bounded/iterative configuration: cap req.body size before parsing and use nlohmann::json::parse with a SAX/callback parser that enforces a maximum nesting depth (reject beyond e.g. 32), or pre-scan and reject excessive bracket depth. Return 400 on over-limit input instead of recursing.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Medium** (matches). SmatchetMergeWatchNotifyServer.cpp:77-84 is exactly the cited code: `nlohmann::json j; try { j = nlohmann::json::parse(req.body); } catch (const nlohmann::json::parse_error& e) {...}`. This is the raw unbounded DOM parse on attacker-supplied `req.body` — no depth/node cap, and it does NOT route through the codebase's own json_safe::ParseBounded (Source/Core/include/Json/BoundedJsonParse.h), which the project built specifically for attacker-controlled ingress. All validation (KnownStates allow-list at line 98,…

### 15. [Medium] Unbounded recursive JSON parse of network-sourced ADF rich values (stack-overflow DoS)

- **File:** `Source/Core/src/TicketFieldEditorLongTextPure.cpp:20-24, 39`
- **Symbol:** `ClassifyRichValue / ComputeLongTextSeed`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Tracker HTTP response -> CachedTicket rich value cache -> GetFieldRichValue -> RichValueToTooltipMarkdown/OpenLongTextEditor -> ClassifyRichValue/ComputeLongTextSeed -> nlohmann::json::parse on attacker-influenced (compromised/MITM tracker or hostile ticket author) deeply-nested ADF

Both ClassifyRichValue (line 21, no-throw form nlohmann::json::parse(rich, nullptr, false)) and ComputeLongTextSeed (line 39, throwing form inside a try/catch) parse the field 'rich value' with the stock nlohmann recursive-descent parser and no depth cap. The `rich` string is the ticket's stored rich/ADF value returned by CachedTicket::GetFieldRichValue, i.e. the description/environment/custom-textarea blob fetched from the tracker server (Jira ADF JSON, Plane, etc.). A maliciously deep or pathologically nested JSON document (thousands of nested arrays/objects) overflows the stack inside the parser BEFORE any nlohmann exception is constructed; the try/catch at lines 38-43 cannot recover from a stack-overflow fault, and the no-throw form at line 21 still recurses identically. This path runs every time a description-like cell tooltip is hovered (TicketFieldEditor.cpp DrawTextCellTooltip /…

**Recommendation:** Parse rich values through a depth-bounded SAX/callback parser (nlohmann::json::sax_parse with a max-depth limiting handler) or pre-scan the byte string for a nesting-depth cap before calling json::parse. A try/catch is insufficient because deep recursion faults the stack rather than throwing; enforce the depth limit during parsing.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Medium** (matches). Real defect, but the auditor's crash MECHANISM is wrong. The finding claims the stack overflows "inside the parser BEFORE any nlohmann exception is constructed" and that "the no-throw form at line 21 still recurses identically." That is incorrect for nlohmann v3.11.3 (the version this repo pins — CMakeLists.txt:511). The project's OWN authoritative analysis in Source/Core/include/Json/BoundedJsonParse.h lines 6-14 states it plainly: nlohmann's parser is iterative (DOM built via sax_parse, NOT per-level recursion),…

### 16. [Low] Unbounded nlohmann::json::parse on the Whisper HTTP response body (no depth/size cap)

- **File:** `Source/Plugins/Whisper/WhisperApiClient.cpp:75-81`
- **Symbol:** `pure::ParseWhisperResponse`
- **Category:** deserialization | **Auditor confidence:** medium
- **Attack vector:** Network response from the transcription endpoint (MITM / malicious proxy / DNS-poisoned api.openai.com) -> r.text -> nlohmann::json::parse with no bounds

ParseWhisperResponse calls bare nlohmann::json::parse(jsonBody) on the raw HTTP response body returned from the transcription endpoint, with no depth limit or input size cap. nlohmann's default parser recurses per nesting level, so a deeply-nested JSON body can drive stack growth / DoS. The endpoint is the fixed https://api.openai.com host and redirects are limited (kAiFollowRedirects), so an attacker must control the response (TLS MITM, a malicious proxy, or a compromised/redirected host) to reach this; the body is also bounded by the 60s transfer timeout. This is the bare-json-parse-on-network-ingress class the audit flags, mitigated by the fixed-host and TLS, hence Low.

**Recommendation:** Parse with a bounded/iterative configuration or pre-check nesting depth and a maximum body size before parsing, mirroring the bounded-parser pattern used for other untrusted-ingress JSON in the codebase.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real defect of the flagged class, severity and vector correct, but the description's MECHANISM is technically wrong and contradicts the codebase's own documentation. The finding says nlohmann's "default parser recurses per nesting level, so a deeply-nested JSON body can drive stack growth" during parse. Per Source/Core/include/Json/BoundedJsonParse.h lines 6-14, nlohmann's parse is ITERATIVE (sax-driven), so the parse itself does NOT overflow the stack. The actual depth-bomb danger is (a) the RECURSIVE ~json DOM…

### 17. [Low] Signed integer overflow in ParseWorkDurationToSeconds unit accumulation

- **File:** `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:787-798`
- **Category:** integer-overflow | **Auditor confidence:** medium
- **Attack vector:** Tracker server returns a worklog/time-tracking/changelog string with an extremely large numeric magnitude before a unit suffix (w/d/h/m); parsed and multiplied during grid display formatting.

After std::stoll parses a token (which only guarantees the value fits in long long), the unit multipliers compute number*5*8*60*60 (weeks => *144000), number*8*60*60, etc., and add into 'total'. A large but in-range number such as '99999999999999w' yields number ~1e14 which, multiplied by 144000, overflows signed long long (UB). The same applies to FormatChangelogTimeValue -> stoll then *? not, but ParseWorkDurationToSeconds is the overflow site. The result only feeds a duration display string, so impact is limited to undefined-behavior / garbage output, not memory corruption, but the value originates from server-supplied changelog/time-tracking strings.

**Recommendation:** Use checked/saturating arithmetic (e.g. compare against (LLONG_MAX - total)/multiplier before multiplying/adding) and clamp on overflow, or compute in unsigned/__int128 and saturate.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Confirmed as described. The description's parenthetical about "FormatChangelogTimeValue -> stoll then *? not" is garbled and can be ignored; the primary claim (overflow in ParseWorkDurationToSeconds unit accumulation) is fully accurate. Recommendation (checked/saturating arithmetic against (LLONG_MAX - total)/multiplier, or __int128 saturation) is sound.

### 18. [Low] Signed integer overflow (UB) in progress fast-path number scanner

- **File:** `Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp:531-536`
- **Symbol:** `BuildProgressRenderModel::parse_num`
- **Category:** integer-overflow | **Auditor confidence:** high
- **Attack vector:** Tracker server returns a progress field with an oversized numeric value; parsed on grid render.

The zero-allocation fast path accumulates digits with val = val * 10 + (trimmed[i] - '0') into a plain int with no digit-count or overflow guard. A tracker-supplied progress field like {"progress":99999999999,"total":1} overflows signed int, which is undefined behavior. The result only feeds a float fraction used for a progress bar (clamped visually by ImGui), so there is no memory-safety or control-flow impact, but it is UB on attacker-controlled input.

**Recommendation:** Accumulate into a wider unsigned type with a length cap, or clamp once it exceeds a sane bound.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Finding is accurate as described. The fast-path scanner is a lambda inside BuildProgressRenderModel (not a separate member parse_num, but the symbol naming is close enough). currentValue is a tracker-supplied grid cell value, so the input is attacker-controlled. Impact is genuinely limited: the overflowed int only feeds a float fraction (p/t) clamped visually by ImGui::ProgressBar, with no memory-safety or control-flow consequence. Note also that even the JSON slow-path (ParseJsonIntFieldLoose, lines 556-557) uses…

### 19. [Low] std::localtime() return value passed to strftime without null check

- **File:** `Source/Core/src/Commands/Scenarios/ScenarioRunner.cpp:55-58`
- **Symbol:** `ScenarioRunner::Start`
- **Category:** null-deref | **Auditor confidence:** high
- **Attack vector:** local-only (no external input path; time source is std::time(nullptr), the outPath default branch is only hit by the trusted local CLI/MCP caller omitting outPath)

When no outPath arg is supplied, the runner builds a timestamped output path via `std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t))`. std::localtime can return nullptr (it returns NULL on conversion failure, e.g. a time_t value the implementation cannot map to a struct tm), and strftime would then dereference a null pointer. Here `t` comes from std::time(nullptr) (current wall-clock), so on any sane system it is in range and localtime will not fail — the path is therefore not reachable with attacker influence and only a corrupted/extreme system clock could trigger it. Still a latent unchecked-return crash on a 'never crash the UI' codebase.

**Recommendation:** Null-check the std::localtime result before passing it to strftime; fall back to a fixed string (or std::gmtime / localtime_r) when it returns nullptr.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Finding is accurate and self-consistent: it explicitly acknowledges the non-exploitability (no attacker influence over the time source). Confirmed as a real but local-only latent defect; not externally exploitable. Severity Low is correct.

### 20. [Low] Unbounded array allocation from Lua table key in LuaToJsonImpl — **✓ hand-verified**

- **File:** `Source/Core/include/Json/LuaJsonConvert.h:102-107`
- **Symbol:** `smatchet::lua_json_detail::LuaToJsonImpl`
- **Category:** unbounded-allocation-DoS | **Auditor confidence:** high
- **Attack vector:** Local Lua automation script: a user-authored automation that returns or passes a sparse-indexed table to a binding which calls LuaToJson (e.g. building MCP tool params or command results). No remote/network path — Lua scripts are run by the trusted local user, so this is a local-only OOM/crash, not remote-reachable.

When converting a Lua table to JSON, the array branch computes max_idx as the maximum integer key seen in the table, then loops for (size_t i = 1; i <= max_idx; ++i) and push_backs an element for every index. The table only needs a single sparse integer key (e.g. {[2000000000]=true} or {[1e15]=1}) for max_idx to become astronomically large. The loop then attempts to build a nlohmann::json array with billions of elements (each iteration calling t[i] and push_back), driving the heap to exhaustion and crashing the app. Unlike JsonToLuaImpl (which carries both a depth cap of 64 and a 200000 node budget) and ParseBounded (depth+node bounded), LuaToJsonImpl has only a depth cap (depth>64) and no node/element budget, so a flat table with one huge index is unbounded. This violates the never-crash pillar.

**Recommendation:** Cap the materialised array size: reject or truncate when max_idx exceeds a sane bound (e.g. a few hundred thousand) and/or thread a shared node budget through LuaToJsonImpl like JsonToLuaImpl already does, returning early once exhausted. Also guard against max_idx far exceeding the actual element count (treat as object/sparse) rather than densifying.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Finding is accurate as described. Severity Low is correct given the local-only attack surface (Lua scripts run by the trusted local user; no network/remote path). The recommendation (cap max_idx, treat far-exceeding-element-count tables as sparse/object, or thread a node budget like JsonToLuaImpl) is sound. Existing test file tests/Lua/LuaJsonConvert.test.cpp only covers the depth cap (line 142), not the array-size densification, confirming this gap is real and untested.

### 21. [Low] Unbounded nlohmann::json::parse on MCP HTTP response bodies (deep-nesting stack-exhaustion not catchable by try/catch)

- **File:** `Source/Standalone/CliCommandRunner.cpp:625-630, 1469`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Local user runs `Smatchet cmd <name>` with --mcp-host/--mcp-port (or env / instance.json) pointed at an endpoint that returns a deeply-nested JSON body; the response is parsed unbounded in the CLI client.

RunCmdAttachDispatch (line 1469) and TryAppendLiveCatalogToHelp (lines 625, 630) call bare nlohmann::json::parse() on res->body, the raw HTTP response from the MCP endpoint. The double-parse at line 630 also parses an inner JSON string (content[0].text) drawn from that body. nlohmann's parser is a recursive-descent parser: a deeply-nested document (e.g. tens of thousands of nested '[' or '{') exhausts the native call stack and aborts the process via SIGSEGV BEFORE any parse exception is thrown — the surrounding try/catch cannot intercept a stack overflow. The MCP host/port the CLI connects to is user/env controllable (--mcp-host, SMATCHET_MCP_HOST, --mcp-port, SMATCHET_MCP_PORT, or a planted instance.json port in the user-data dir), so a local user or a malicious endpoint the CLI is pointed at can return a crafted body that crashes the CLI process (violates the never-crash pillar). No…

**Recommendation:** Parse network-sourced bodies through a depth-bounded SAX parser (nlohmann::json::sax_parse with a custom handler that caps nesting depth) or pre-scan/reject bodies exceeding a sane nesting/size cap before the recursive parse, rather than relying on try/catch which cannot catch stack exhaustion.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real defect, correct fix (route through json_safe::ParseBounded), but the finding's mechanism is wrong: nlohmann's parser is iterative and does NOT stack-overflow during parsing — the non-catchable crash comes from the recursive ~json DOM destructor on a deep tree plus unbounded heap growth (per BoundedJsonParse.h:6-14), not from a recursive-descent parser exhausting the stack mid-parse. Outcome and recommendation remain valid.

### 22. [Low] Unbounded nlohmann::json::parse on spawn/scenario result files and instance.json

- **File:** `Source/Standalone/CliCommandRunner.cpp:147, 961, 1169`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Local file: a crafted/corrupted scenario result file (path taken from MCP response data.outPath) or instance.json in the user-data directory is read and parsed unbounded by the CLI.

The scenario result file is read into `content` and parsed with bare nlohmann::json::parse at line 961 (--spawn async path) and line 1169 (in-process async path); instance.json is parsed via SafeParseJson (bare parse at line 147). As with the HTTP body parses, a deeply-nested JSON document triggers recursive-descent stack exhaustion that is not catchable by the wrapping try/catch, crashing the CLI. These files live in the user-data dir / a path supplied by the MCP server response, so the vector is local (a malicious or corrupted result file / instance.json). Lower severity than the network path because the writer is normally the trusted spawned instance.

**Recommendation:** Use a depth-bounded parse for these file reads as well, and cap the file size read into memory before parsing.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Lines are accurate: 147 = `out = nlohmann::json::parse(text)` in SafeParseJson; 961 = `nlohmann::json::parse(content)` (--spawn async result file); 1169 = `nlohmann::json::parse(content)` (in-process scenario result file). All three are bare parses NOT routed through json_safe::ParseBounded. The file reads (954-958, 1159-1164, 1335-1339) load the entire file with no byte cap, and instance.json is read from ConfigManager::GetUserDataDirectory() (1325). The codebase's own BoundedJsonParse.h documents the exact…

### 23. [Low] Unbounded recursive nlohmann JSON parse in TryParseJsonMaybeDoubleEncoded (no depth cap on tracker-sourced field values)

- **File:** `Source/Core/include/JsonParseUtil.h:7-22`
- **Symbol:** `TryParseJsonMaybeDoubleEncoded`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** low
- **Attack vector:** Malicious/compromised (or MITM'd) tracker server returns a field value containing deeply nested JSON; the value is cached into CachedTicket and later parsed by the grid field-display / field-icon render path through this helper on the UI thread.

The helper calls nlohmann::json::parse(raw, nullptr, false) (and a second nested parse when the first result is a string) with no recursion/depth limit. nlohmann-json's parser is recursive-descent for nested arrays/objects, so a sufficiently deeply nested payload (e.g. '[[[[[...]]]]]' or '"[[[...]]]"' for the double-encoded branch) drives unbounded native stack recursion and can stack-overflow the process (a hard crash, not a recoverable exception — allow_exceptions=false only suppresses parse-error throws, not the recursion). The callers feed it CachedTicket field values (Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp lines 101/125/190/218/259/468/549 and SmatchetFieldIconRender.cpp), which are populated from tracker HTTP responses (Jira ADF / Plane / GitHub) — so the bytes originate off-host. Practical reachability requires a malicious or compromised/MITM'd tracker backend…

**Recommendation:** Parse through a depth-bounded SAX callback (nlohmann::json::sax_parse with a parser_callback that rejects beyond a fixed nesting depth, e.g. 64) or pre-scan the raw string for an excessive run of leading nesting tokens before calling parse, mirroring the bounded-parser discipline already applied to the SSE/NDJSON stream parsers. Apply the same cap to the…

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real defect, but the finding's stated crash MECHANISM is wrong. The finding claims nlohmann's parser is "recursive-descent" and that a deep payload "drives unbounded native stack recursion and can stack-overflow the process" DURING the parse. The codebase's own BoundedJsonParse.h (lines 6-14) explicitly refutes this: nlohmann's parser is ITERATIVE (driven from sax_parse, no per-level recursion), so the stack does NOT overflow while parsing. The actual crash vectors are (a) the RECURSIVE ~json destructor tearing…

### 24. [Low] Unbounded nlohmann::json::parse on cmd_recents.json (depth-bomb crash class)

- **File:** `Source/Core/src/Commands/CommandRegistry.cpp:408-433`
- **Symbol:** `CommandRegistry::LoadRecents`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** local file: a crafted cmd_recents.json in the user-data directory (e.g. pre-seeded by another local process or restored from a shared/synced profile) is parsed unbounded at startup; deep nesting overflows the stack on DOM teardown

LoadRecents reads <userData>/cmd_recents.json into a std::string and calls bare `nlohmann::json::parse(content)` (line 420). The codebase explicitly documents this exact construct as a known crash class in Json/BoundedJsonParse.h: an attacker-supplied deeply-nested JSON ([[[[...]]]]) builds a deep DOM whose RECURSIVE destructor (~json) overflows the C++ stack — an uncatchable crash, NOT a catchable exception, so the surrounding try/catch (line 419) does not protect against it. Every other untrusted-ingress JSON site in this partition (config.set, ParamType::Json coercion) routes through json_safe::ParseBounded; this local-file loader was missed. The vector is a local file normally written by the app itself, so exploitation requires the ability to write the user-data directory (another process running as the user, a synced/shared profile, or a malicious tarball that pre-seeds the file).…

**Recommendation:** Replace `nlohmann::json::parse(content)` with `json_safe::ParseBounded(content, err)` (already included transitively via the registry) and bail when err is non-empty, matching the rest of the codebase's ingress policy.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Confirmed as described. One precision worth noting: the crash is purely on DOM teardown (~json), not during parse — the description's parenthetical "builds a deep DOM whose RECURSIVE destructor overflows" is correct, and it correctly states the try/catch does not protect. The fix (ParseBounded + bail on non-empty err) is sound and low-risk since the header is already included and the helper already used in this same translation unit.

### 25. [Low] Unbounded-recursion JSON parse on GitHub HTTP responses (stack-overflow DoS)

- **File:** `Source/Core/src/Tracker/GitHubActivityFeed.cpp:77,135,196,229`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** HTTP response body from the configured tracker server reaches the parser. Reachable via a malicious/compromised GitHub Enterprise endpoint (the Base URL is an arbitrary user-supplied https host, only loosely validated in IsValidGitHubBaseUrl), a man-in-the-middle on the TLS channel, or any backend that can return a…

FetchUserActivity / FetchUserGroupNames / FetchGroupMembers parse the raw HTTP response body from the GitHub REST API with bare nlohmann::json::parse(resp.text) (line 77 wrapped only in try/catch; lines 135, 196, 229 likewise). nlohmann::json's default parser is recursive-descent with no depth limit and the codebase has no bounded-parse helper. A deeply-nested JSON array/object (e.g. tens of thousands of nested '[') in the response overflows the C++ stack — a SIGSEGV that try/catch cannot intercept (stack overflow is not a C++ exception). This crashes the whole app, violating the 'never crash' pillar. The same pattern recurs throughout this partition: GitHubClient.cpp ProbeReachability (137), CreateIssue (639); GitHubClientHelpers.cpp ExtractGitHubErrorMessage (271); JiraClient.cpp ListProjects (125); JiraActivityFeed.cpp (223); IssueDraft.cpp FromJson (244); IssueTableSerializer.cpp…

**Recommendation:** Parse untrusted HTTP/file JSON through a single shared helper that caps nesting depth, e.g. nlohmann::json::parse with a SAX callback that aborts past a fixed depth (or json::sax_parse with a depth-counting handler), and reject/raise a TrackerErrorParse instead of recursing unboundedly. Apply uniformly to every tracker JSON ingress in this partition.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real bug, but the finding's stated mechanism contains two factual errors that should be corrected. (1) It claims nlohmann's default parser is recursive-descent and overflows the stack DURING the parse. That is wrong: nlohmann's parser is iterative (it drives the DOM builder from sax_parse, not per-level recursion), so the parse itself does NOT overflow. The codebase's own authoritative header Source/Core/include/Json/BoundedJsonParse.h:6-14 documents this precisely — the uncatchable stack overflow occurs at DOM…

### 26. [Low] Recursive option deserialization over locally-cached JSON (defense-in-depth)

- **File:** `Source/Core/src/Tracker/FieldCatalogCache.cpp:95-115,354-371`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** local-only: smatchet_field_catalog_cache.json in the user data directory. Only reachable by something that can write that file (the trusted local user, or another process with filesystem access). No network/IPC path.

LoadAndMigrateRootLocked parses the on-disk field-catalog cache file with bare nlohmann::json::parse(text), and OptionFromJson (lines 95-115) then recurses over the nested 'children' arrays of each TrackerFieldOption. Both the parse step and the OptionFromJson recursion are unbounded in depth. A deeply nested cache JSON would overflow the stack at parse time (the recursion in OptionFromJson would only matter if parse succeeded on a shallower-but-wide structure). This is local-only (the cache file lives under the user data directory and is normally written by the app itself), so impact is limited to a crash if the file is corrupted or tampered with.

**Recommendation:** Use the same depth-bounded parse helper recommended for HTTP ingress, and add an explicit recursion-depth cap to OptionFromJson so a pathological cache cannot exhaust the stack.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real but mis-described mechanism. nlohmann parse is iterative (per the repo's own BoundedJsonParse.h:6-14), so the "stack overflow at parse time" claim is incorrect; the genuine uncatchable overflow comes from the recursive DOM destructor and/or the unbounded OptionFromJson recursion (lines 107-112). Attack surface is local-file-tampering only — no network/IPC path feeds smatchet_field_catalog_cache.json. Recommendation (route through ParseBounded + add a depth cap to OptionFromJson) is valid defense-in-depth. Low…

### 27. [Low] Unbounded recursion in NormalizeTrackerFieldValue on server-controlled nested JSON arrays/objects

- **File:** `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:988-1023`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Malicious or compromised Jira/tracker HTTP server returns an issue whose field value is a deeply nested JSON array/object; the issue-fetch path parses the body and calls NormalizeTrackerFieldValue on each field value.

NormalizeTrackerFieldValue() recurses through nested JSON without any depth cap: the array branch (lines 1012-1021) calls itself on each element, and the object branch dispatches to NormalizeTrackerObjectValue which can re-enter (e.g. child option resolution / array-of-objects). The input is a Jira/tracker server issue field value (called from JiraIssueMappingPure::MapIssue -> NormalizeTrackerFieldValue on issueFields[...] at JiraIssueMappingPure.cpp:160/222/228/315), i.e. a foreign trust boundary. The same TU explicitly caps its ADF walkers (CollectAdfText / ExtractAdfTextToStream) and the dump path (SafeJsonDump/JsonExceedsDepth) at kMaxAdfRecursionDepth=256 precisely because 'a malicious or buggy tracker can return JSON nested thousands of levels deep, blowing the C++ stack (Pillar 3 - Never crash)', but this generic normalizer was left uncapped. A deeply-nested array value (e.g.…

**Recommendation:** Thread a depth parameter (default 0) through NormalizeTrackerFieldValue and NormalizeTrackerObjectValue, bail/degrade once it reaches kMaxAdfRecursionDepth (return SafeJsonDump or an empty/marker string), mirroring the existing ADF-walker cap in this file.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real but partly mischaracterized. Only the ARRAY branch is a genuine unbounded-recursion vector. The object branch (line 1010 -> NormalizeTrackerObjectValue) does NOT re-enter unboundedly: its recursive helpers are bounded. BuildTrackerOptionDisplayValue (called once at line 957 for value["child"]) does not itself recurse into nested child objects (lines 61-98: it only reads primitives/preferred-label keys and returns), and ParseComments / ADF text use the capped kMaxAdfRecursionDepth=256 walkers…

### 28. [Low] Unbounded nlohmann::json::parse on host command args (depth-bomb DoS, not catchable by surrounding try/catch)

- **File:** `Source/Core/src/Ui/SmatchetImGuiHost.cpp:1053-1083`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** C-ABI SmatchetHost_EnqueueCommand(argsJsonUtf8) from the Unreal/embedding host -> EnqueueCommand -> PendingCommands -> DrainCommandQueue; args JSON may carry values forwarded from an automation/agent caller

DrainCommandQueue() calls `nlohmann::json::parse(req.ArgsJson)` (line 1056) directly on the args string supplied via the C-ABI entry point SmatchetHost_EnqueueCommand -> EnqueueCommand. The parse is wrapped in try/catch (line 1075/1079), but per the project's own BoundedJsonParse.h threat model the danger of a deeply-nested payload (e.g. "[[[[...]]]]") is NOT a catchable exception: nlohmann builds the full DOM, and the recursive ~json destructor overflows the C++ stack when the deep tree is torn down — a crash that bypasses try/catch entirely (and unbounded heap growth before that). Every documented ingress in this codebase (MCP POST handlers, Lua decode_json, CommandRegistry string args at CommandRegistry.cpp:203) routes through smatchet::json_safe::ParseBounded specifically to prevent this; this host-layer top-level parse does not, so it is the unbounded boundary (the args object is…

**Recommendation:** Replace the bare nlohmann::json::parse(req.ArgsJson) with smatchet::json_safe::ParseBounded(req.ArgsJson, err) and surface a ValidationError CommandResult on err, mirroring CommandRegistry.cpp:203. Do not rely on the try/catch — a depth-bomb destructor crash is not a C++ exception.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Finding is accurate as described. The top-level args structure is parsed unbounded at the host layer; the registry's ParseBounded only covers nested string-typed Json fields after Dispatch receives the already-parsed object, so it does not protect the top-level nesting depth. Severity Low is appropriate: the ingress is the Unreal/embedding C-ABI host bridge, and attacker control is conditional on the host forwarding untrusted/agent-originated args (CommandSource::Unreal), not a direct unauthenticated network…

### 29. [Low] Bare nlohmann::json::parse on offline-queue payload in UI-thread tooltip

- **File:** `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp:962`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** low
- **Attack vector:** local SQLite offline-queue DB payload/conflict_context_json rendered on the UI thread; structure app-generated, server influences only leaf string values

DrawOfflineRowPayloadTooltip() runs `nlohmann::json::parse(row.payload)` on the UI thread inside a try/catch. row.payload is the serialized create/field-edit payload stored in the local SQLite offline queue. As with the host-command case, a sufficiently deeply-nested JSON value would overflow the stack in the recursive ~json destructor — a crash the try/catch (line 979) cannot intercept. The JSON *structure* here is app-generated (IssueDraft serialization), so an attacker would need to influence nesting depth of stored payload values, which is not clearly reachable; this is primarily a defense-in-depth / Pillar-3 hardening note. ParseConflictModalCtx() (line 1115) uses allow_exceptions=false but is still the unbounded DOM-build path and shares the same destructor-depth exposure for conflict_context_json (which embeds server-derived 'theirs'/'base' values).

**Recommendation:** Route both row.payload (line 962) and conflict_context_json (line 1115) through smatchet::json_safe::ParseBounded for consistency with the rest of the codebase's hardened ingress sites.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). Confirmed as a Low/defense-in-depth finding exactly as described, including the auditor's explicit caveat that the nesting structure is app-generated and not clearly attacker-reachable. The unsafe pattern (bare parse vs ParseBounded) and the recursive-destructor crash mechanism are real and accurately characterized; the recommendation matches the codebase's existing hardened-ingress convention. Practical exploitability is essentially nil because server influence is limited to leaf string values within a flat…

### 30. [Low] Unbounded recursion in ParseGitHubCommitListJson via bare nlohmann::json::parse on HTTP response

- **File:** `Source/Core/src/Vcs/GitHubCommitsParse.cpp:24`
- **Symbol:** `ParseGitHubCommitListJson`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** GitHub commits HTTP response body (resp.text) parsed without a depth limit; reachable when cfg.GitHubBaseUrl points to an attacker-controlled or MITM'd endpoint returning deeply nested JSON.

ParseGitHubCommitListJson calls nlohmann::json::parse(body, nullptr, false) with no recursion-depth cap. nlohmann's recursive-descent parser recurses one stack frame per nesting level of arrays/objects, so a deeply nested JSON payload (e.g. tens of thousands of '[' characters) will exhaust the call stack and crash the process before parse() returns. The `body` is `resp.text` from the GitHub commits HTTP endpoint (GitHubCommits.cpp line 86), and the target host is derived from the user-configurable cfg.GitHubBaseUrl. A malicious/compromised endpoint or a network MITM on the (configurable, possibly non-TLS-pinned) base URL can return a pathological body that crashes the app, violating the never-crash pillar. The error-mode-false flag only affects how syntax errors are reported (is_discarded), not the recursion depth, so it does not mitigate this.

**Recommendation:** Use a bounded SAX parse or pass a maximum nesting depth / size cap before parsing (reject bodies that exceed a sane size, and use a callback parser that aborts past a fixed depth). Mirror whatever bounded-parse helper the Tracker subsystem uses for other network ingress.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real defect: bare nlohmann::json::parse on attacker-influenced body bypasses the project's own json_safe::ParseBounded depth/node/byte cap. Description's crash mechanism is wrong (parser is iterative, not recursive-descent; crash is via recursive ~json DOM teardown + heap growth, per BoundedJsonParse.h:6-14), but the bug, severity, reachability, and fix are correct. Recommend routing line 24 through smatchet::json_safe::ParseBounded.

### 31. [Low] Unbounded nlohmann::json::parse on GitHub update-check HTTP response (deep-nesting DOM destruction crash / unbounded allocation)

- **File:** `Source/Core/src/AttachmentAppUpdateService.cpp:530-540`
- **Symbol:** `AttachmentAppUpdateService::CheckForAppUpdate`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** HTTP response body from api.github.com (TLS-pinned trusted host; only reachable via TLS-MITM or compromised endpoint); repo path is local-config-controlled.

The update-check response body is parsed with a bare `nlohmann::json::parse(response.text)` (line 532) wrapped only in a try/catch. This is exactly the class the codebase explicitly hardens everywhere else by routing through `smatchet::json_safe::ParseBounded` (see AppController_LuaBindings.cpp LuaDecodeJsonBind/ParseMcpToolDef and AppController_LuaBindings_Draw.cpp ExecuteLuaMcpTool, all in this same partition, which document that nlohmann builds the DOM iteratively but a deeply-nested payload (`[[[[...]]]]`) overflows the C++ stack when the resulting deep tree is destroyed in `~json` — an overflow a try/catch around parse cannot trap). Additionally, the GET here uses no cpr::WriteCallback size cap (unlike DownloadAttachmentToLocalFile's 50 MiB cap), so `response.text` is buffered unbounded before parsing. Reachability is limited: the request targets a fixed…

**Recommendation:** Route the response through smatchet::json_safe::ParseBounded (depth/node/byte caps) like the other ingress sites in this codebase, and add a cpr::WriteCallback byte cap on the update-check GET so an oversized body is rejected before buffering/parsing.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Low** (matches). All claims verified. The bare parse at line 532 bypasses the codebase's own ParseBounded hardening (BoundedJsonParse.h), and the secondary claim that the update-check GET lacks a WriteCallback byte cap (unlike DownloadAttachmentToLocalFile's 50 MiB cap at lines 147-152) is also correct. Reachability is genuinely limited to TLS-MITM / compromised GitHub endpoint, so Low / defense-in-depth is the right severity.

### 32. [Low] Unbounded per-line nlohmann::json::parse on AI SSE stream

- **File:** `Source/Core/src/OpenAiClient.cpp:87-103`
- **Symbol:** `DispatchOpenAiDataLine`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** medium
- **Attack vector:** Attacker-controlled or proxied OpenAI-compatible streaming endpoint (cfg.BaseUrl) returns a single SSE data line containing deeply nested JSON; reaches nlohmann::json::parse during SendStreaming.

Each SSE `data:` line from the AI provider is parsed with bare `nlohmann::json::parse(data)` (recursive descent, no depth bound). A malicious or compromised endpoint (or MITM proxy when a custom/plain-http BaseUrl is configured) could send a deeply nested JSON line to overflow the worker-thread stack and crash the app. Lower severity than the merge-watch case because the endpoint is user-configured and the per-line size is implicitly limited by the SSE framing, but the depth is not bounded and the parse exception handler does not catch a stack overflow.

**Recommendation:** Use a depth-bounded SAX parse for streamed provider lines (cap nesting depth and line length) consistent with hardening the other ingress parsers.

> **Verification (independent Opus/max re-check):** **partially-confirmed**; cited lines accurate; independent severity **Low** (matches). Real defect (unbounded parse of attacker-influenced SSE lines; ParseBounded exists and is the intended fix), but the auditor's described overflow mechanism is inaccurate: the crash is via the recursive json DESTRUCTOR / heap growth on a deep DOM, not "recursive descent during parse." The catch-block argument and the recommendation (route through depth-bounded SAX parse) remain correct.

### 33. [Info] POSIX config-file read has no size cap while the Win32 sibling caps at 64 MiB

- **File:** `Source/Core/src/Config/ConfigManager_PathUtils.cpp:771-791`
- **Symbol:** `ConfigManager::LoadJsonFile`
- **Category:** unbounded-recursion-DoS | **Auditor confidence:** high
- **Attack vector:** local-only (config file is owner-only / chmod 0600; the only writer with arbitrary nested content is the trusted local user — MCP config.set / Lua writes go through WriteConfigJson and emit controlled scalar values, not arbitrary attacker-nested JSON)

On Win32 LoadJsonFile reads the config file through CreateFileW/ReadFile and explicitly rejects files larger than 64 MiB (li.QuadPart <= 64*1024*1024) before parsing. The POSIX/non-Windows branch instead slurps the entire file via `ss << file.rdbuf(); raw = ss.str();` with no size limit, then hands `raw` to `nlohmann::json::parse(raw)` with the library's default (unbounded) recursion depth. A pathologically large or deeply-nested config/views/panes file would be read fully into memory and parsed with default depth. The asymmetry shows the 64 MiB cap on Win32 was a deliberate guard the POSIX path is missing. Impact is bounded because the file is owner-only (AtomicWriteTextFile chmods 0600 and LoadJsonFile warns on a group/world-readable config), so the only principal who can plant such a file is the trusted local user; nlohmann throws on excessive nesting and the parse is wrapped in…

**Recommendation:** Mirror the Win32 64 MiB size cap in the POSIX branch (stat the file or check raw.size() before parse) and consider passing a bounded max-depth / using a depth-limited SAX parse, to match the deliberate cap already present on Win32.

> **Verification (independent Opus/max re-check):** **confirmed**; cited lines accurate; independent severity **Info** (matches). Finding is accurate and honestly self-rated. The unbounded read + default-depth parse is genuinely present and asymmetric vs Win32, but the attack surface is limited to the trusted local user (0600 owner-only config), so it is correctly an Info-level hardening note, not an exploitable vulnerability. Recommendation (mirror the 64 MiB cap / use a depth-bounded parse) is reasonable.

---

## Remediation priorities

1. **Single highest-leverage fix:** sweep every bare `nlohmann::json::parse(` on network / IPC / file ingress to `smatchet::json_safe::ParseBounded`. This closes 25 of 33 findings at once. A delta-gated lint (`bare-json-parse-untrusted`) already exists in WARN mode — promoting it to blocking and clearing the backlog would prevent regressions.
2. **Confine MCP file-touching commands:** add a per-`CommandSource` capability check (or path allow-list / base-dir confinement) so `whisper.transcribe-once`, `perf.dump`, and similar cannot read/write arbitrary paths when invoked over MCP; require the auth token even on loopback.
3. **Format-string:** change `SmatchetLocalization::Format` to never use a runtime-loaded translation as the format argument (fixed `"%s"` + substitution, or validate the override's specifier list against the source string).
4. **Bound the Lua→JSON array path** with a node budget mirroring `JsonToLuaImpl`.
5. The integer-overflow / null-deref / size-cap items are low-severity hardening; fix opportunistically.

## Caveats

- This was a static, read-only review; no exploits were built or run. Reachability for the DoS class assumes an attacker who controls or can MITM the configured tracker/AI endpoint, or (for the notify/MCP servers and local-file parses) a local process. Hosts pinned to TLS (api.anthropic.com, api.github.com) require a TLS-MITM or endpoint compromise.
- Findings were produced by LLM agents and then independently re-verified by separate LLM agents against source (0 refuted, severities unanimous). The distinctive non-DoS items were additionally orchestrator-hand-verified. Line numbers should still be confirmed at edit time, as code drifts.
