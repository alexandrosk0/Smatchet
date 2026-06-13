# Smatchet — Deeper Security-Audit Playbook

**Status:** planning artifact (not a findings report)
**Authored:** 2026-06-13
**Parent report:** [`SECURITY_AUDIT_2026-06-13.md`](SECURITY_AUDIT_2026-06-13.md) (0 CRITICAL / 5 HIGH / ~14 MED / ~16 LOW / 7 INFO — **static + targeted-read only**)
**Backlog:** [`docs/self-improvement/categories/security.md`](../self-improvement/categories/security.md)

---

## Why this doc exists

The 2026-06-13 audit was explicitly **static-only** (§6: "dynamic/runtime fuzzing was out of scope"). It located *where* bugs likely are by reading code and reasoning about source→sink pairs. It could not **execute** anything: no crash depth was produced, no credential was captured, no data race was observed, no non-Windows build was run, the Unreal/DX12 embed was never compiled, and no dependency was CVE-matched. Every MEDIUM/HIGH is therefore *reasoned*, not *reproduced*.

This playbook maps the **tiered escalation** that converts reasoning into proof (or refutation), grounded in real `file:line` targets discovered by a 5-lane scout sweep over the live tree. It is the blueprint for the next, deeper audit pass — **what to build, in what order, for the highest proof-per-unit-effort.**

It is a *plan*, not a verdict: nothing below is a confirmed bug. The targets are where to point dynamic tooling.

---

## Tier ladder (summary)

| Tier | Dimension | Effort | Gist |
|---|---|---|---|
| 2 | Dynamic execution + sanitizers (ASAN/UBSan/TSan) | high | Run the 4 reasoned crash/UB/race candidates under sanitizers; the only TSan lane excludes MCP/AI/Lua/app entirely |
| 3 | Coverage-naive fuzzing of untrusted-input decoders (libFuzzer) | medium | 6 of 9 decode boundaries are already pure TUs → near-zero-risk harnesses; 3 (incl. the JQL injection sinks) need a lift first |
| 4 | Static taint / dataflow (CodeQL cpp + Semgrep) | high | Completeness (negative-space matrix), second-order sanitizer-bypass, exhaustive sink sweeps |
| 5 | Live red-team PoCs + Unreal/DX12 embed build-exercise | high | 4 runnable exploit harnesses + the never-compiled embed trust surface |
| 6 | Dependency / supply-chain depth + SBOM | medium | curl 7.80.0 (2021) ships; no SBOM, no CVE feed, two-layer gradle gap |
| 7 | Coverage-guided + formal/property proof on the FIX primitives | medium | Lock JqlQuoteLiteral / redaction / path-containment / header-strip against regression |

---

## The 34 concrete targets (all lanes)

Discovered by 5 parallel scouts over `C:/Dev/Smatchet` @ develop. Each is a **deeper-audit move**, not a confirmed defect.

### Lane 1 — Dynamic execution + sanitizers (Tier 2) — 6 targets

1. **ADF walk-time unbounded recursion** — libFuzzer+ASAN over `ParseComments`/`NormalizeTrackerFieldValue`.
   `TrackerFieldValueParser.cpp:290` (`CollectAdfText`), `:309` (`ExtractAdfTextToStream`); public sinks `ParseComments` @ `:415`. Both ADF walkers recurse on `content[]` with **no depth cap**. Build the already-parsed DOM iteratively to isolate walk-time vs nlohmann parse-time overflow. Untrusted source: malicious tracker server JSON (comment bodies + ADF object fields). Effort **low**. Yield: confirms/bounds the stack-overflow + a depth-cap regression test.

2. **stb decode-bomb** — ASAN/RSS harness proving allocation precedes the dimension cap.
   `SmatchetImageTextureCache.cpp:141` (`stbi_load_from_memory` allocates full RGBA), `:149` (>512px cap rejects *after* alloc). A few-KB PNG IHDR claiming 30000×30000 → ~3.6 GB alloc before the cap. Untrusted source: server-supplied avatar/attachment/icon bytes. Effort **low**. Validates the `stbi_info` pre-check fix.

3. **MCP-thread vs UI-thread data race on `g_ui`** — TSan over a real MCP-dispatch + render harness.
   `McpPlugin.cpp:434` → `CommandRegistry.cpp:317` (Handler runs **inline** on the MCP `std::thread`) → handlers write non-atomic `g_ui`: `BuiltinCommands_BugReport.cpp:62-64`, `BuiltinCommands_Debug.cpp:292-294,320-321`. The UI render loop reads those fields every frame. Needs a **new** TSan subtarget (the existing `ninja-tsan-linux` has `WITH_MCP=OFF`). Effort **high**. Single largest dynamic-coverage gap.

4. **Automation-worker shutdown deadlock / UI-thread starvation** — driven scenario under a watchdog.
   Hook `AppController_LuaBindings.cpp:1257` (`lua_sethook LUA_MASKCOUNT,50000` checks `shuttingDown_` only at Lua VM instruction boundaries) → blocking `JiraIssueMutation.cpp:206` (synchronous HTTP PUT); shutdown blocks at `AppController.cpp:898` (`automationWorker_.join()`). A hang-on-connect mock server proves the join hangs. Effort **medium**. Audit synthesis #13.

5. **AI streaming cancel/submit race** — TSan over `AiAssistantController` worker + `PostToMainThread`.
   `AiAssistantController.cpp:146` (worker), `:212/:271` (`currentCancel_` shared_ptr<atomic<bool>>), `:434` (MakeOnDelta posts to main thread), `:346-348` (model-change clears g_ui). The best-written threading surface → primarily *verification* + catching any turn-gen lambda lifetime edge race. Effort **medium**.

6. **UBSan over the tracker JSON parse + numeric-coercion path.**
   `TrackerFieldValueParser.cpp` numeric/date helpers (`NormalizeNumber` ~`:272`, `FormatDateIfIso` `:352` indexes `value[4]`/`value[7]`), `ParseWorkDurationToSeconds` (`:227`); `JqlSuggestEngine.cpp:139-140` (H3 AccountId). The Windows nightly is ASan-only; UBSan exists only in `ninja-clang-asan` and the nightly feeds no hostile parser input. Effort **low**. Catches signed-overflow / OOB-index / narrowing.

### Lane 2 — Fuzzing the untrusted-input decoders (Tier 3) — 9 targets

7. **`AiSseParser::Feed/Flush`** (SSE byte-stream framing) — `AiSseParser.h:33`; impl links standalone (`tests/CMakeLists.txt:522`). Split input into N pseudo-random slices, Feed+Flush. Exercises chunk-boundary reassembly + the 4 MiB cap (`AiSseParser.h:12`). **No refactor.** Effort **low**. Cleanest target in the tree.

8. **`AiNdjsonParser::Feed/Flush`** (NDJSON streaming) — `AiNdjsonParser.h:33`; `tests/CMakeLists.txt:523`. Same split-and-Feed; per-line `nlohmann::json::parse` puts malformed-per-line JSON + 4 MiB line cap in scope. **No refactor.** Effort **low**.

9. **`ParseComments` + ADF recursive text collection** — `TrackerFieldValueParser.h:26`; `CollectAdfText:290`/`ExtractAdfTextToStream:309` (recurse `content[]`, no depth counter). Parse fuzz bytes → `ParseComments(j)`. doctest TU `tests/CMakeLists.txt:495` already links it. **No refactor.** Effort **low**. Confirms the MEDIUM ADF-recursion finding as a real stack-overflow.

10. **`AppendCachedTicketFromJiraSearchIssue`** (Jira /search JSON → CachedTicket) — `JiraIssueMappingPure.h:59`; `tests/CMakeLists.txt:399`. Injectable `fetchIssueComments` callback fed a second buffer slice → HTTP-free; transitively re-exercises ADF recursion via the realistic entry. **No refactor.** Effort **low**.

11. **Plane work-item mappers** (`MapPlaneWorkItemsArrayToCachedTickets` / `MapPlaneWorkItemJsonToCachedTicket` / `ExtractKeyFromPlaneQuery`) — `PlaneIssueMappingPure.h:60/:53/:78`; `tests/CMakeLists.txt:420`. The array mapper catches per-item throws, so hunt ASAN/UBSan crashes not exceptions; `ExtractKeyFromPlaneQuery` takes a raw query string. **No refactor.** Effort **low**.

12. **GitHub issue-search mappers + GraphQL→REST rewrite** (`MapIssueOrPullRequestJsonToCachedTicket` / `MapGraphQlNodeToRestShape` / `MapGraphQlNodesToTickets`) — `GitHubIssueSearchMapping.h:38/:81/:96`; `tests/CMakeLists.txt:393`. The GraphQL→REST rewrite is a pure structural transform with many optional-field unwraps. **No refactor.** Effort **low**.

13. **JQL builders `BuildJqlUserInsert` / `BuildKeyInJql`** (injection sinks — **REFACTOR REQUIRED**) — `JqlSuggestEngine.cpp:130-143` (AccountId @ `:139-140` returns `"\""+AccountId+"\""` with no escape, **H3**); `BuildKeyInJql` @ `JiraIssueSearch.cpp` (anon-ns, **E1**). Both static in anonymous namespaces → invisible to any harness. Lift to a `*Pure` free function first, then fuzz with a **property oracle** (output contains no unescaped quote/JQL-metachar breaking the literal). Effort **medium**. The only way to regression-fuzz the highest-severity findings.

14. **JSON→Lua decode** (`JsonToLua` / `LuaDecodeJsonBind`) — sub-step fuzzable, full console path NOT. `AppController_LuaBindingsCore.cpp:64` (`JsonToLuaImpl`, depth guard `kJsonToLuaMaxDepth=64` @ `:62`); `LuaDecodeJsonBind` @ `AppController_LuaBindings.cpp:763` (4 MiB cap, nlohmann parse `:770`); end-to-end `ExecuteLuaConsoleSnippet` @ `AppController_LuaBindings_Draw.cpp:959` binds a `sol::state` member of `AppController::Impl` → **not isolable**. Only the `JsonToLua` sub-step is fuzzable with a throwaway `sol::state`. Effort **medium**. The end-to-end gap is itself the finding.

15. **`DecodeWithStb`** (image bytes → RGBA; cap AFTER allocation — **REFACTOR REQUIRED**) — `SmatchetImageTextureCache.cpp:131` (`stbi_load_from_memory:141` allocates before the cap `:149`); only public caller `GetOrLoadFromMemory:235` registers a GPU texture (un-fuzzable). Split a pure `bytes→RGBA+dims` entry, then a trivial harness under `-rss_limit_mb`. Effort **medium**. stb is the classic high-yield fuzz target.

### Lane 3 — Static taint / dataflow (Tier 4) — 7 targets

16. **H1 — config AgentsMd path → file-read → outbound LLM system prompt** (off-host file exfil). `AgentsMdLoader.cpp:35,38` (`fs::exists`+`ifstream`) → `AiAssistantController.cpp:408,418` (`LoadLayered`→`ComposeSystemPrompt`→outbound). Source: `smatchet_config.json` keys `project_agents_md_path`/`agents_md_global_path` (`ConfigManager.cpp:220-221`, loaded `:791-792`). Two-hop CodeQL cpp path-query; model `fs::exists` as **not** a sanitizer. Effort **medium**.

17. **H3 — server AccountId → unescaped JQL literal** (autocomplete query injection). `JqlSuggestEngine.cpp:139-140` (no escape); reached via `AppendJqlUserCatalogSuggestions:177`. Source: `TrackerUser.AccountId` from `/users`+`/myself` parse. Semgrep taint: source = json-field read populating AccountId; sink = the JQL string-concat; sanitizer (a `JqlQuoteLiteral`) absent. Same query re-confirms E1 (`BuildKeyInJql`) + the DisplayName branch (`:131`). Effort **medium**.

18. **H4 — tracker API token → cross-host redirect forward** (`cpr::Redirect(true,true)` + caller-set Authorization). `TrackerHttpUtils.cpp:140,152,169,180,268` with `BuildTrackerHeaders:105-108` attaching Basic auth (`:115-117`). An AST conjunction (auth-header + `Redirect(true,true)`), **no build DB needed**. Positive control: `McpPlugin.cpp:314` `Redirect(false,false)` must PASS. Also flags the AI-client x-api-key forward (synthesis #11) + whisper download (`ModelDownloader.cpp:314`). Effort **low**. Highest precision.

19. **Second-order MISS — offline-queue draft serialized to STRING bypasses structural audit redaction.** `OfflineQueueService.cpp:356,362` (`{"draft", IssueDraftHelpers::ToJson(draft)}`) → `IssueDraft.cpp:213` `ToJson` returns `std::string` → `BackendAuditTrail.cpp:144,361` (string-leaf branch). `RedactJsonWithKey` only recurses `is_object()`/`is_array()` (`:128,:138`); a string leaf hits `:144` → `TruncateAuditString` only, never key-redaction. Barrier-aware CodeQL model. Effort **high**. Highest-value "eye missed the mechanism" class.

20. **P4 custom-command template → `ShellExecuteW` argv** (config-template + server `{file}`/`{cl}` → subprocess). `P4vLaunch.cpp:166-195` (`ReplacePlaceholder` `:172-174` → `SplitCommandExecutableAndArgs:48` → `ShellExecuteW:189-190`); `P4Annotate.cpp:52-59` `QuoteWinArg` path; gated by `AnnotateAllowCustomCommands:135-143`. `QuoteWinArgWide:72-86` is a **leaky barrier** (trailing-backslash). Sweep the same sink at `SubprocessCapture` `BuildWindowsCommandLine:125-131`. Effort **medium**.

21. **ADF parser unbounded recursion** (server JSON → stack exhaustion, Pillar 3). `TrackerFieldValueParser.cpp:290-306` / `:309+`, no depth bound. CodeQL: recursive function driven by untrusted-json structure with no depth guard; taint reach from `cpr::Response::text` → `CollectAdfText` arg. Pair with the Lane-1/Lane-2 fuzz harness. Effort **medium**.

22. **Coverage map — config-key SOURCE set with NO current sink** (latent/future-sink guard + negative-space certification). Config table `ConfigManager.cpp:194-221` (`kStringFields`) loaded `:791-792`; env ingress `:1140-1141` (`SMATCHET_DB_PATH`); CLI `:1194-1195`; AiBaseUrl SSRF guard `AiEndpointSanitize.cpp:125-159`; refuted icon-URL `TrackerFieldValueParser.cpp:897-902`. CodeQL global dataflow emits a **source×sink reachability matrix** that reproduces the live findings, certifies the ~30 dormant keys, reproduces the audit's own refutations, and re-checks the IPv4-literal-only AiEndpointSanitize guard (`:68-96` — a DNS name resolving to 169.254.169.254 bypasses, synthesis #14). Effort **high**. Highest strategic value as a standing regression gate.

### Lane 4 — Unreal/DX12 divergence + live red-team (Tier 5) — 7 targets

23. **Unreal embed: prebuilt-lib packaging supply-chain + staleness.** `SmatchetImGuiPlugin.Build.cs`. Copies prebuilt static libs (`SmatchetCore_DX12.lib`, `cpr.lib`, `libcurl.lib`, MCP/Lua) from `ThirdParty/Smatchet/lib/Win64/Development/`; `WarnIfPackagedLibsAreStale` is an mtime check that only **WARNs**. Package a deliberately-stale/swapped lib and confirm the `.uplugin` builds+ships it. Effort **medium**. Embed-only supply-chain finding absent from the report.

24. **Unreal embed: console + Blueprint command-registry bridge reaches full command set.** `SmatchetImGuiConsoleCommands.cpp`. `IsSafeConsoleCommandName` filters only the command NAME charset, not the arg payload; destructive commands gate on `--yes`/`bConfirmedDestructive`. Enumerate which registry entries are reachable from any UE console/exec/Blueprint source (untrusted level data, networked exec, modded content). Effort **medium**.

25. **Unreal embed: ProjectSavedDir config-base divergence + DX12 void* handle C-ABI.** `SmatchetImGuiHost.cpp:454`. (a) Config base derives from `FPaths::ProjectSavedDir()` not exe-dir — confirm where secrets/DB land in a packaged title + whether a world-writable `Saved/` lets another process read the config JSON (compounds H2). (b) DX12 backend passes raw `ID3D12Device*`/`ID3D12CommandQueue*`/heap handles as `void*` across the C-ABI (`SmatchetImGuiHostC.h`) — fuzz the handle-marshalling for type-confusion/lifetime/UAF. Effort **high**. Neither compiled nor reasoned about in the standalone audit.

26. **H4 PROOF: malicious-tracker mock returns 30x cross-host redirect, Authorization leaks.** `TrackerHttpUtils.cpp:140`. Two-instance `httplib::Server` (clone `tests/support/JiraCatalogHttpFixture.h`): origin returns 302/307 to an attacker collector on a different host:port; `cpr::Redirect(true,true)` + default `CURLOPT_UNRESTRICTED_AUTH` replays the `Authorization: Basic base64(Email:ApiToken)` to the foreign host. Assert the collector captured it. Effort **low**. Turns H4 into a captured-credential PoC.

27. **MCP PROOF: DNS-rebinding browser page hits loopback port, no Host/Origin check.** `McpPlugin.cpp:137`. `Authorize` (`:137-161`) checks only `X-Smatchet-Token` + `IsLoopbackAddress(remote_addr)`; never validates Host/Origin, and SSE sets `Access-Control-Allow-Origin: *` (`:591`). A DNS-rebinding page (or curl with spoofed `Host:`) reaches `tools/call` dispatch (`:426-445`). Effort **medium**.

28. **Lua PROOF: paste-and-run `ai.prompt` rate-abuse + count-only automation hook.** `AppController_LuaBindings.cpp:614`. `LuaAiPromptGlue` (`:614-633`, bound `:717`) calls `PromptAi` with **no rate limit** (H5) → `for i=1,N do ai.prompt(...) end` fires N real network calls. The `lua_sethook(LUA_MASKCOUNT,50000)` guard (`:1257`) counts VM instructions, so a job spending wall-time in C++ HTTP/AI never trips it → un-interruptible long-running job. Also probe `LuaCreateIssueBind` (`:778`). Effort **low**.

29. **H2 PROOF: secret-at-rest plaintext dump on a real POSIX/Android build.** `ConfigManager_PathUtils.cpp:332`. The `#else` branch makes `ProtectSecretForConfig` a no-op pass-through; `AtomicWriteTextFile` POSIX branch (`:753-760`) writes via plain ofstream + rename with **no chmod 0600, no O_NOFOLLOW**. Configure creds, persist, then `cat` the config JSON → `token`/`plane_api_key`/`github_pat`/`mcp_auth_token`/`ai_*_api_key` in cleartext; `stat` for world-readable perms; pre-plant a symlink to show the missing O_NOFOLLOW. Effort **medium**.

### Lane 5 — Dependency / supply-chain depth + SBOM (Tier 6) — 5 targets

30. **CVE-match bundled curl 7.80.0** (the headline exposure). Pin: `.fetchcontent-src/cpr-src/CMakeLists.txt:249-250`; confirmed live at `.fetchcontent-src/curl-src/include/curl/curlver.h:33`; ships because `CMakeLists.txt:272` `CPR_FORCE_USE_SYSTEM_CURL=OFF`. 2021-11 release → every advisory ≥7.81.0 applies. Triage redirect/chunked/HTTP2/URL-parser CVEs against the cpr→`TrackerHttpUtils.cpp`/AI-client/`ModelDownloader.cpp` path (CURLOPT_FOLLOWLOCATION on per H4); de-prioritize curl-OpenSSL TLS CVEs (Windows uses Schannel). Deliver a per-CVE reachable verdict + a single bump target (cpr 1.10+/1.11 → curl 8.x). Effort **medium**. Single biggest pin-age exposure.

31. **Generate the missing SBOM + wire a recurring dependency-CVE gate.** Repo has NO SBOM and NO `.github/dependabot.yml`; pins live in `CMakeLists.txt:442-949` + `cmake/ImGuiTestEngine.cmake:17` + `cmake/SmatchetThirdParty.cmake`. Emit a CycloneDX SBOM from the FetchContent pin set; add a weekly scheduled OSV-Scanner CI lane failing on new HIGH CVEs. Dependabot covers neither FetchContent nor the gradle distribution, so SBOM+OSV-Scanner is the **only** continuous mechanism. Effort **medium**.

32. **Close the two-layer gradle distribution + wrapper-jar integrity gap** (INFO synthesis #21, deepened). `Source/Mobile/AndroidApp/gradle/wrapper/gradle-wrapper.properties:3` (`distributionUrl`=gradle-8.7-bin.zip, `distributionSha256Sum` **ABSENT**) + committed `gradle-wrapper.jar` (unverified binary). Add the `distributionSha256Sum`; regenerate the jar via `gradle wrapper --gradle-version 8.7 --distribution-type bin` and diff; add a `gradle-wrapper-validation` CI check. Effort **low**.

33. **CVE-match the second tier of bundled natives never enumerated.** SQLite via SQLiteCpp 3.3.1 (`CMakeLists.txt:500`); cpp-httplib v0.14.1 (`:532` — the MCP loopback server); glfw 3.3.8 (`:644`); nlohmann/json v3.11.3 (`:442`); stb_image v2.30 (`Source/Core/ThirdParty/stb/stb_image.h:1`). Priority by trust-boundary reach: cpp-httplib (request-smuggling/header-parse), SQLite (cached tracker JSON via `LocalCacheManager`), stb_image (attachment/icon decode, ties to synthesis #9). Effort **medium**.

34. **Flag the non-release pin of imgui_test_engine as a reproducible-build / provenance gap.** `cmake/ImGuiTestEngine.cmake:17` (`GIT_TAG 8568767…` — a bare main-branch HEAD, not a tagged release). No version maps to a release artifact → CVE matching impossible; reproducibility depends on the commit still existing upstream. Test-only (no shipping CVE risk). Recommend pinning to the nearest tagged release. Effort **low**.

---

## Top 12 highest-value targets (proof-per-effort)

1. `TrackerHttpUtils.cpp:140` (+152,169,180,268) — **H4** redirect+auth leak. Two-instance httplib PROVES the credential lands cross-host; swept by a no-build-DB Semgrep rule. (control: `McpPlugin.cpp:314`)
2. `JqlSuggestEngine.cpp:139-140` — **H3** unescaped AccountId (+`:131` DisplayName E1). One Semgrep taint rule re-confirms both; the `*Pure` lift unlocks Tier-7 property-fuzzing.
3. `TrackerFieldValueParser.cpp:290`/`:309` — **ADF** no-depth-cap recursion. Cleanest pure stack-overflow fuzz target + cheapest doctest regression.
4. `OfflineQueueService.cpp:356`/`:362` — **second-order redaction bypass** via string-flattened `ToJson(draft)` (`BackendAuditTrail.cpp:144`). Generalizable by a CodeQL barrier model.
5. `SmatchetImageTextureCache.cpp:141` vs `:149` — **stb alloc-before-cap** DoS. ASAN/RSS harness with a 30000×30000 IHDR.
6. `McpPlugin.cpp:137` — **MCP no Host/Origin** (ACAO:* `:591`). DNS-rebinding page reaches `tools/call`.
7. `AppController_LuaBindings.cpp:614` (+1257) — **Lua H5** no rate-limit + count-only hook. 2-line paste-and-run PoC.
8. `ConfigManager_PathUtils.cpp:332` (+`:753-760`) — **H2** POSIX cleartext + no-chmod + no-O_NOFOLLOW. Linux/Android build dumps secrets off disk.
9. `McpPlugin.cpp:434` → `CommandRegistry.cpp:317` → `BuiltinCommands_*` — **MCP→g_ui data race**. New TSan subtarget (current lane excludes all of this).
10. `AppController.cpp:898` + `JiraIssueMutation.cpp:206` — **shutdown deadlock**. Hang-on-connect mock + watchdog.
11. `AgentsMdLoader.cpp:35,38` → `AiAssistantController.cpp:418` — **H1** two-hop off-host file exfil. CodeQL path-query + a containment-helper guard.
12. `.fetchcontent-src/curl-src/include/curl/curlver.h:33` — **curl 7.80.0** ships. OSV-match against the cpr HTTP path; biggest pin-age exposure under the audit's own threat model.

---

## Biggest blind spots (what the static fleet structurally could not see)

1. **Confirmation vs reasoning.** Static-only (§6): could locate *where* but never distinguish a real crash depth / multi-GB RSS spike / data race from a benign one. Every MEDIUM/HIGH is "reasoned", not "repro'd".
2. **The entire cross-thread surface is invisible to the only data-race gate.** `ninja-tsan-linux` builds with `BUILD_APP`/`WITH_MCP`/`WITH_AI`/`WITH_LUA` all OFF and links only `LocalCacheManager` + 2 pure units (`tests/CMakeLists.txt:40-63`). TSan also has no Windows toolchain (`Sanitizers.cmake:14`) — the shipping platform has zero race coverage.
3. **Zero fuzz infrastructure.** No `LLVMFuzzerTestOneInput` in `Source/` (only vendored); the ASAN/UBSan nightly only runs the existing ctest suite which feeds no malicious ADF/image/SSE/NDJSON payloads.
4. **The Unreal/DX12 embed trust surface was never compiled.** `Source/UnrealPlugins/` (prebuilt-lib supply chain, console/Blueprint command bridge, ProjectSavedDir divergence, DX12 void* C-ABI seam) — a whole platform with no static OR dynamic coverage.
5. **Non-Windows behaviour was asserted from source, never demonstrated.** H2's POSIX/Android plaintext-secret `#else`, the no-chmod/no-O_NOFOLLOW write, the symlink-follow — no secret was ever read off disk.
6. **Completeness / negative-space is beyond the eye.** Proved specific config→sink flows exist but cannot certify the *absence* of a sink for the other ~30 `kStringFields` keys, nor mechanically reproduce its own refutations. Only a CodeQL source×sink matrix can.
7. **Second-order / sanitizer-bypass mechanism.** Flagged the draft→audit symptom (synthesis #10) but not the generalizable rule "serialization to a string defeats a structural redactor".
8. **Dependency CVE drift.** Recorded version strings but did not CVE-match → curl 7.80.0 (and un-enumerated cpp-httplib/SQLite/glfw/json/stb) drifted unflagged, with no SBOM, no scheduled feed, no covering Dependabot ecosystem.
9. **Live-exploit proof of the network/authz sinks.** H4 token leak, MCP DNS-rebinding reach, Lua rate-abuse / count-only hook — all reasoned, none driven with a malicious server, a rebinding page, or a paste-and-run script.

---

## Recommended sequence (cheapest-highest-yield first)

1. **Zero-infra wins (no build DB, no new target).** The Tier-4 Semgrep H4 redirect+auth-header structural rule (sweeps all 5 verbs in one pass) **and** the Tier-5 H4 two-instance-httplib captured-credential PoC — the same `TrackerHttpUtils.cpp:140` finding attacked statically and dynamically.
2. **Stand up `SMATCHET_BUILD_FUZZERS`** and fire the 6 no-refactor Tier-3 harnesses (`AiSseParser`/`AiNdjsonParser`/`ParseComments` + the 3 mappers) under ASAN+UBSan. `ParseComments` confirms the ADF stack-overflow + seeds the regression.
3. **In parallel, the two low-effort Tier-5 PoCs needing only a debug build** — Lua H5 rate-abuse + count-only hook, and the H2 POSIX cleartext-dump on a Linux `ConfigManager` build.
4. **The Tier-2 ASAN/UBSan harnesses that reuse the fuzz scaffolding** — stb decode-bomb, UBSan numeric/date coercion.
5. **Build the CodeQL DB; run the Tier-4 taint queries cheapest-first** — H3/E1 single-sink rule → H1 two-hop → second-order redaction barrier → P4 command-injection → finally the high-effort completeness matrix as a standing gate.
6. **Expensive items last (need NEW build infra)** — the Tier-2 MCP/AI/automation TSan subtarget + the shutdown-deadlock mock-server scenario, then the Tier-5 Unreal/DX12 embed build-exercise (a whole UE5 + DX12 toolchain).
7. **Tier 6 SBOM + curl-7.80.0 CVE match runs anytime in parallel** (pin-extraction needs no build); land the curl bump decision + the gradle two-layer fix early — cheap and continuously valuable.
8. **Tier 7 last by definition** — the property/formal oracles presuppose the Tier-3 lifts and the written fixes, locking each fix primitive (JqlQuoteLiteral, object-or-string redaction, path-containment, cross-host header-strip) against regression.

---

## Prerequisites at a glance

- **Tier 2/3:** a `SMATCHET_BUILD_FUZZERS` CMake option + a `-fsanitize=fuzzer,address,undefined` clang target (nearest base `ninja-clang-asan`, lacks `,fuzzer`); a NEW MCP/AI/automation-linked TSan subtarget (current `ninja-tsan-linux` excludes all of it); seed corpora (nested-ADF generator, giant-dimension PNG header, malformed-field-value set); a controllable mock tracker server (hang-on-connect + scripted-SSE) — reuse `tests/support/JiraCatalogHttpFixture.h`.
- **Tier 4:** a CodeQL cpp DB from the Standalone `compile_commands.json`; hand-written models (ConfigManager::Load field-flow, `cpr::Response::text` taint origin, `ifstream`/`ShellExecuteW`/`cpr::Url`/JQL-builder sinks, leaky-barrier models for `RedactJsonWithKey`/`QuoteWinArgWide`/`AiEndpointSanitize`). The H4 Semgrep rule needs none of this.
- **Tier 5:** a configured UE5 project + DX12 RHI + freshly-built `ThirdParty/Smatchet/lib/Win64/Development/` (PART A); the httplib fixture + a `WITH_LUA_AUTOMATION` debug build + a Linux/Android `ConfigManager` build (PART B).
- **Tier 6:** no build for pin-extraction; a hand-authored CycloneDX/SPDX (FetchContent leaves no lockfile) + OSV-Scanner/Grype/Trivy; upstream release SHA256s.
- **Tier 7:** the Tier-3 lifts done + the four fixes written (these are proofs *on* the fix primitives).

---

*Generated from a 5-lane scout sweep (`deeper-security-audit-blueprint`) over the live tree, adversarial-synthesis pass. All `file:line` anchors verified against develop @ the time of authoring; re-verify before acting on any single anchor (line numbers drift).*
