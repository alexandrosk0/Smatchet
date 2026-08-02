# Smatchet C++ Code Audit

**Date:** 2026-07-01
**Branch:** `claude/cpp-code-audit-woe188`
**Scope:** First-party product C++ only — `Source/Core/`, `Source/Standalone/`, `Source/Plugins/`, `Source/Mobile/`, `Source/UnrealPlugins/.../Source/`. Vendored `ThirdParty/` (nlohmann, cpr, sol2, ImGui, SQLiteCpp, stb, md4c, Lua) and the `tests/` tree were excluded.
**Coverage:** 675 first-party C++ files / ~140K LOC, partitioned into 12 LOC-balanced chunks, each read in full by an independent auditor across ten defect angles.

This is a broad correctness/robustness/security audit, **complementary to** the prior `SECURITY_AUDIT.md` (2026-06-26). That audit focused on the untrusted-JSON DoS class, MCP authorization, the locale format-string, and the Lua→JSON allocation bug. Findings already documented there (and new instances of the same bare-`json::parse` DoS pattern at sites *listed* in that doc) are **not** re-reported here. Several prior findings were independently confirmed **already remediated** in the current tree (see "Remediation confirmations" below).

> ## ✅ Remediation status — REMEDIATED (verified 2026-07-05)
>
> **All 33 findings are resolved: 31 fixed in code and 2 accepted as documented latent risk (#21 dictation shadow-buffer, and the fork-safety half of #26). The last remaining perf sub-item folded under #33 — `CommandPaletteUi::rebuildFiltered` — was fixed 2026-07-05.** Remediation landed via **PR #1593** (Slices 1–4, commits `0aa6dc9`/`8c42543`/`af5536f`/`5276f79`/`8ad4860`) and **PR #1613** (`fefa11f`, the `#33f`/`#33g` missing includes), tracked in [`docs/plans/cpp-code-audit-remediation.md`](../plans/cpp-code-audit-remediation.md). Recurring finding-classes were subsequently turned into standing CI lint gates (PR #1605). Each finding was re-verified against the current source on 2026-07-05 (not against the commit messages).
>
> | Finding | Status | Finding | Status | Finding | Status |
> |---|---|---|---|---|---|
> | #1 long-text truncation | ✅ Fixed | #12 toolbar/keybind ArgsJson | ✅ Fixed | #23 annotate callback deref | ✅ Fixed |
> | #2 GitHub credential routing | ✅ Fixed | #13 audit-jsonl parse | ✅ Fixed | #24 TokenizeCached ref | ✅ Fixed |
> | #3 duration-sort infinite loop | ✅ Fixed | #14 priority iconUrl SSRF | ✅ Fixed | #25 TranslateSource UAF | ✅ Fixed |
> | #4 AI cancel-atom rebind | ✅ Fixed | #15 whisper mic via MCP | ✅ Fixed | #26 fork/exec + zero-timeout | ⚠️ Split: hang fixed / fork-safety accepted-risk |
> | #5 TextMerge LCS OOM | ✅ Fixed | #16 IPv6 fe80::/10 denylist | ✅ Fixed | #27 DrainCapturedPcm unlocked read | ✅ Fixed |
> | #6 replay latch leak | ✅ Fixed | #17 ParseSemanticVersion atoi | ✅ Fixed | #28 injected-font atlas UAF | ✅ Fixed |
> | #7 locale format-string sinks | ✅ Fixed | #18 Linear key int accumulation | ✅ Fixed | #29 ImGui Shutdown without Init | ✅ Fixed |
> | #8 Jira field-catalog parse (10) | ✅ Fixed | #19 duration unit int overflow | ✅ Fixed | #30 p4 future never joined | ✅ Fixed |
> | #9 Plane parse (9) | ✅ Fixed | #20 Lua cyclic-array recursion | ✅ Fixed | #31 DispatchLines unbounded pending | ✅ Fixed |
> | #10 BugReportService parse (8) | ✅ Fixed | #21 dictation shadow-buffer UAF | ⚠️ Accepted-risk (unreachable; documented) | #32 Android IME JNI | ✅ Fixed |
> | #11 views/panes bare parse | ✅ Fixed | #22 icon.Texture null-deref | ✅ Fixed | #33 logic cluster (5 + 4 latent) | ✅ Fixed (incl. the `CommandPaletteUi::rebuildFiltered` perf sub-item, fixed 2026-07-05) |
>
> **Now closed:** finding #33's folded "Also latent" performance sub-item — `CommandPaletteUi::rebuildFiltered` used to call `app.Commands().All()` (which locks, deep-copies, and sorts the whole registry) once per recent inside the loop plus once more (up to 9× per rebuild). Fixed 2026-07-05 by snapshotting the table once at the top of the function and reusing it across both branches. The other 8 components of #33 (year-boundary offset, JQL quoted "order by", single-select combo clear, `PromptAi` turn-gen, `QuoteWinArgWidePure` empty arg, `EnsureAssetsBranch` guard, `<cstdlib>`/`<mach-o/dyld.h>` includes) were already fixed. **No findings remain open.**
>
> **Accepted as documented latent risk (2), not code-fixed:** #21 (all 4 registration sites use process-static buffers — unreachable today; the header documents the invariant) and the fork/exec async-signal-safety **half** of #26 (a prebuilt-argv/envp rewrite of the P4/Git spawn path was judged disproportionate for a Low finding; the *other* half — a zero-timeout `Run()` hang — is fixed with a safety-net timeout). See [`cpp-code-audit-remediation.md`](../plans/cpp-code-audit-remediation.md) § Deviations.

## Methodology

1. **Fan-out** — 12 independent auditors, one per LOC-balanced partition, each reading every file in its partition in full and hunting across ten angles: memory safety, undefined behavior, integer handling, concurrency, RAII/resource management, error handling, security, logic/correctness, performance, and API-misuse/portability. Auditors were permitted to read callers/callees outside their partition to confirm or refute a suspicion.
2. **Dedup gate** — every auditor first read `SECURITY_AUDIT.md` and suppressed already-known findings.
3. **Orchestrator verification** — the four highest-impact findings (long-text data loss, GitHub credential mis-routing, duration-sort infinite loop, and the cancel-atom rebind) were hand-read against source by the orchestrator and are marked **✓ hand-verified**.

## Summary

**33 findings** — High: 1, Medium: 8, Low: 24.

| Category | Count |
|---|---|
| Logic/correctness | 9 |
| Security (SSRF / mic / format-string / SSRF-denylist) | 5 |
| Security/DoS (bare-parse sites not in prior audit) | 5 |
| Concurrency | 6 |
| Resource management / RAII | 4 |
| Integer handling | 3 |
| Error handling | 2 |
| Memory safety | 2 |
| Performance | 1 |
| API misuse / portability | 2 |

> Note: several findings span two angles; the primary is used above.

### Toolchain / build hygiene (context)

The build is well-configured and not a source of findings: `CMAKE_CXX_STANDARD 14` with `STANDARD_REQUIRED ON`; first-party targets carry `/W4` (MSVC) or `-Wall -Wextra -Werror` (Clang); ASAN/UBSan (`ninja-clang-asan`, `ninja-msvc-asan`) and TSan presets exist and are exercised in CI. The dominant residual risk classes are (a) correctness bugs in edit/sort/merge paths that unit warnings can't catch, and (b) untrusted-network-ingress hardening gaps that the prior JSON sweep did not reach.

---

## Findings

### 1. [High][Logic/correctness] Long-text description editor silently truncates values >64 KiB and writes the truncated text back to the tracker on Save — **✓ hand-verified**

- **File:** `Source/Core/src/TicketFieldEditor_Modal.cpp:460-464` (seed copy), `:351-365` (`CommitLongTextEdit`); same truncating copy at `:237-239` and `:492-495`
- **Mechanism:** `OpenLongTextEditor` stores the **full** markdown seed in `OriginalMarkdown` but copies at most `kBufferSize - 1` = 65,535 bytes into the fixed `kBufferSize = 64*1024` edit buffer (`copyLen = std::min(seed.size(), kBufferSize - 1)`). `CommitLongTextEdit` then builds `newValue` from the truncated buffer and diffs it against the **untruncated** seed (`if (newValue != seed) { ...push PendingFieldEdit... }`). For any description whose markdown exceeds 65,535 bytes — GitHub issue bodies go up to exactly 65,536 chars (`GitHubIssueSearchMapping.cpp:19`), and Jira ADF converted to markdown can be far larger — an **unmodified** buffer already differs from the seed, so clicking Save (or Ctrl+Enter) queues an edit that overwrites the server-side field with the truncated text. Silent loss of everything past 64 KiB, no warning banner. Verified: `kBufferSize = 64 * 1024` at line 62; the buffer is a fixed `std::vector` (no ImGui resize-callback growth).
- **Fix:** When `seed.size() > kBufferSize - 1`, grow the buffer to fit or open read-only with a visible "too large to edit" banner; at minimum, diff `newValue` against the *truncated* seed actually shown so a no-op Save never PUTs.
- **Confidence:** high

### 2. [Medium][Logic/correctness] `SMATCHET_TRACKER_TOKEN` / `SMATCHET_TRACKER_BASE_URL` are mis-routed for the GitHub backend — PAT lands in the Jira slot, GitHub requests go out unauthenticated — **✓ hand-verified**

- **File:** `Source/Core/src/Config/ConfigManager.cpp:1407` and `:1423` (`RouteTrackerEnvCredentials`)
- **Mechanism:** The canonical `cfg.TrackerType` for the GitHub backend is the capitalized `"GitHub"` (produced by `NormalizeViewsBackendKey`, persisted by the Preferences UI, listed by `KnownBackendKeys()`). But the routing compares a lowercase literal: `else if (cfg.TrackerType == "github")`. With `TrackerType == "GitHub"` this arm is never taken, so both the token and base-URL blocks fall through to the final `else`, assigning `cfg.ApiToken = envToken` (the **Jira** credential slot) and `cfg.Domain = envBase` instead of `cfg.GitHubPat` / `cfg.GitHubBaseUrl`. Realistic trigger: a user configures GitHub in-app (persisted `"GitHub"`) and injects credentials via `SMATCHET_TRACKER_TOKEN` in CI/container — the PAT lands in the wrong field, GitHub HTTP goes out unauthenticated, and the base-URL override is silently ignored. Plane uses the matching `"Plane"` and Linear routes off a lowercased copy, so only GitHub is affected. The function's own comment acknowledges the "legacy Plane/GitHub arms compare raw casing."
- **Fix:** Route GitHub off `trackerTypeLower == "github"` (as Linear already does), or normalize `cfg.TrackerType` before the branch.
- **Confidence:** high

### 3. [Medium][Logic/correctness] Infinite loop (permanent UI freeze) in the duration sort parser on any non-unit character after a number — **✓ hand-verified**

- **File:** `Source/Core/src/TicketGridModel.cpp:86-88` (`ParseDurationToSecondsForSort`)
- **Mechanism:** In the manual unit loop, when the character at `pos` is neither whitespace, a digit, nor one of `w/d/h/m`, the final `else { total += num; }` branch does **not** advance `pos`. Input like `"2.5h"` or `"3h 30m left"` (any time-tracking cell containing `.`, `s`, parentheses, etc.) reaches this branch and loops forever: `pos` stays fixed, `num` is re-added each iteration. The function runs inside `CompareTimeTrackingValues` → `CompareFieldValuesForSort`, i.e. a sort comparator on the UI thread — one bad tracker-supplied value hangs the whole app the instant the user sorts the `timespent`/`timeestimate`/aggregate column. Pure-integer values escape early via the whole-string `stoll` fast path, so common Jira seconds values don't trigger it, but pretty-printed / backend-variant values do.
- **Fix:** `++pos` (or `break`) in the final `else` branch, matching the unit branches.
- **Confidence:** high

### 4. [Medium][Concurrency] `AiAssistantController::Submit` unconditionally rebinds the shared cancel atom, so Cancel()/the destructor cannot abort an in-flight turn once a newer Submit is queued

- **File:** `Source/Core/src/AiAssistantController.cpp:205-211` (Submit rebind), `:217-222` (Cancel), `:152-164` (destructor), `:229-237` (worker captures `currentCancel_` at pop)
- **Mechanism:** The worker captures `cancel = currentCancel_` when it pops a request and polls only that atom for the whole stream. `Submit()` replaces `currentCancel_` with a fresh atom at enqueue time with no in-flight check. If turn A is streaming (worker holds atom α) and `AppController::PromptAi` (the Lua/automation path, which — unlike the panel's `assistantInFlight`-gated Send — has no in-flight gate) calls `Submit(B)`, `currentCancel_` becomes β. Now the Stop button's `Cancel()` flips β only — turn A streams on, so Stop is a visible no-op. Worse, `~AiAssistantController` flips only β then `worker_.join()`s: the worker is blocked in `SendStreaming` polling α, so shutdown hangs until the stream finishes or the deliberately long `TotalTimeoutMs = 600000` (10 min) fires.
- **Fix:** Give each `Request` its own cancel token; have the worker set `currentCancel_` under `queueMutex_` at pop time, and have `Cancel()`/the destructor flip the in-flight token *and* every pending request's token.
- **Confidence:** high (mechanism); medium overall (requires the ungated `PromptAi` path to race a live turn).

### 5. [Medium][Performance] `TextMerge::ComputeLcs` allocates an O(n·m) int table over server-controlled text — multi-GB allocation / OOM in the offline-replay 3-way merge

- **File:** `Source/Core/src/TextMerge.cpp:39-49` (`ComputeLcs`), reached from `Source/Core/src/Sync/OfflineQueueService.cpp:1072` (`ResolveFieldEditThreeWayMerge`)
- **Mechanism:** `ComputeLcs` builds `std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1))` with no size guard (its own comment admits it's "sufficient for < ~1000 lines"). The inputs include `theirsMd = fresh.GetFieldRichValue(fid)`, re-fetched from the tracker during conflict resolution. Two 30k-line texts (a pasted log, or a hostile/compromised tracker response) allocate ~3.6 GB and O(n·m) time. The resulting `bad_alloc` unwinds the replay task — and via finding #6 the replay subsystem then stalls permanently.
- **Fix:** Bail out of `ThreeWayMerge` (treat as conflict) when `A.size()*B.size()` exceeds a fixed budget (e.g. 4M cells), or use an O(min(n,m))-memory LCS / Myers diff.
- **Confidence:** high

### 6. [Medium][Error handling] Unguarded `cache->SaveTicket` in `IssueCreatePipeline::Run` leaks the offline-replay in-flight latch and can create duplicate issues

- **File:** `Source/Core/src/Tracker/IssueCreatePipeline.cpp:368-370`, interacting with `Source/Core/src/Sync/OfflineQueueService.cpp:1403-1439` and `:839-878`
- **Mechanism:** `RunUpdateExisting` deliberately wraps its post-PUT cache ops in try/catch, but `Run` calls `cache->SaveTicket(...)` bare after a **successful** `CreateIssue`. The cache is SQLiteCpp-backed and throws (every other call site wraps cache ops for this reason). During offline replay a throw here (DB locked / disk full / the TextMerge OOM above) unwinds out of `ReplayOneCreate`; the `LaunchBackgroundTask` firewall catches it, but the lambda tail that resets `offlineReplayInFlight_ = false` never runs. Consequences: (1) the in-flight latch stays `true` forever → offline replay is silently dead until restart; (2) the issue was created server-side but `DeletePendingCreate` never ran → the next replay re-creates it (duplicate issue).
- **Fix:** Wrap the `Run` cache save in try/catch like `RunUpdateExisting`, and reset the in-flight flag via an RAII guard so it can never leak on an exception.
- **Confidence:** high (latch-leak); medium (SaveTicket throw frequency)

### 7. [Medium][Security] Locale-override strings are fed as printf format strings to the ImGui `Text*`/`SliderInt` wrappers via `TranslateSource`, with no specifier validation

- **File:** `Source/Core/include/SmatchetLocalizedImGui.h:188-235` (`Text`, `TextColored`, `TextDisabled`, `TextWrapped`, `BulletText`, `SetTooltip`, `SetItemTooltip`) and `:138-142` (`SliderInt` `format`)
- **Mechanism:** Each wrapper passes `SmatchetLocalization::TranslateSource(fmt)` straight into ImGui's `...V(fmt, args)` printf-family functions. `TranslateSource` returns an attacker-influenceable **override** string verbatim (from `<RuntimeAssetDir>/Locales/<lang>.json`) with no conversion-specifier validation. The specifier-equality guard the prior audit added protects only `SmatchetLocalization::Format` → `vsnprintf`; it is **not** applied on the `TranslateSource` path. `SmatchetLocalizedImGui::Text("Model: %s", name)` with the English source overridden to `"%s %s %n"` makes `TextV` consume varargs never supplied (OOB read → crash / info-leak; `%n` → write on libc builds that honor it). This is the whole localized UI surface (~45 TUs, per-frame) — a distinct set of sinks from the documented finding #1.
- **Fix:** Have `TranslateSource` (or these wrappers) validate that an override's specifier sequence matches the trusted English source before using it as a format — reuse the existing `ConversionSpecifiers` comparison; fall back to the English literal on mismatch. Same local-file ingress precondition as prior finding #1.
- **Confidence:** high (mechanism); precondition is a malicious/shared local locale pack.

### 8. [Medium][Security/DoS] Ten unbounded `nlohmann::json::parse` sites on Jira field-catalog HTTP responses missed by the prior ParseBounded sweep

- **File:** `Source/Core/src/Tracker/TrackerFieldCatalog.cpp:45, 87, 102, 122, 145, 180, 226, 257, 303, 464`
- **Mechanism:** Every phase of `JiraClient::FetchFieldCatalog` decodes the Jira server's raw HTTP body with a bare parse (components, priority/issuetype/status enrichment, createmeta, sprint boards, field list, project components). Per the codebase's own `BoundedJsonParse.h` threat model, a depth-bomb body builds a deep DOM whose recursive `~json` destructor overflows the C++ stack (uncatchable `SIGSEGV`, not an exception the surrounding try/catch can intercept), plus unbounded heap growth. Runs on the catalog-refresh worker with a compromised/MITM'd/hostile-tenant `cfg.Domain` as the vector. Sibling TUs in this same directory were converted by the sweep (`SMATCHET_DEVIATION ... ParseBounded security sweep` markers) — this TU was skipped entirely; none of its 10 sites appear in `SECURITY_AUDIT.md`.
- **Fix:** Replace all ten with `smatchet::json_safe::ParseBounded(response.text, err)`, mapping non-empty `err` to the existing parse-failure path.
- **Confidence:** high

### 9. [Medium][Security/DoS] Nine unbounded `nlohmann::json::parse` sites on Plane HTTP responses missed by the prior ParseBounded sweep

- **File:** `Source/Core/src/Tracker/PlaneIssueSearch.cpp:185, 237, 270, 670`; `PlaneIssueMutation.cpp:114, 326, 350, 520`; `PlaneFieldCatalog.cpp:92, 327`
- **Mechanism:** Same documented depth-bomb class (recursive DOM teardown → uncatchable stack overflow) at network-ingress sites the prior audit did not list (it names only `PlaneActivityFeed.cpp:71/156` — since fixed — and `PlaneClient.cpp:100`). Includes the **primary Plane sync path** (`FetchPlaneIssuePage` main body, `:270`). `cfg.PlaneUrl` is an arbitrary user-configured (often self-hosted) host, so a malicious/compromised Plane server or MITM returns a deep body and crashes the app; the non-throwing `parse(..., nullptr, false)` form and the try/catch both fail to contain the destructor-time overflow.
- **Fix:** Route all nine through `json_safe::ParseBounded` (keeping the `StripUtf8BomCopy` pre-step), mapping errors to the existing invalid-JSON strings.
- **Confidence:** high

---

### Low-severity findings

**10. [Low][Security/DoS] `BugReportService` parses GitHub-API HTTP responses with bare `json::parse` (8 sites).** `Source/Core/src/Diagnostics/BugReportService.cpp:239, 255, 298, 328, 340, 367, 420, 432`. Response bodies bare-parsed inside `catch(std::exception&)` (which cannot catch the destructor-time stack overflow). Bug-report base URL and relay URL are user-configurable → malicious endpoint / MITM vector. Not in prior audit. Fix: `ParseBounded`, non-empty error = failed submit.

**11. [Low][Security/DoS] Views/panes config files parsed with bare `file >> j`, bypassing `LoadJsonFile`'s bounded parse + 64 MiB cap.** `ConfigManager_Views.cpp:311-312`, `ConfigManager_Panes.cpp:64-65`. Sibling loaders missed the hardening the prior audit's finding #33 documents for the main config file; a deeply-nested `smatchet_views.json`/`smatchet_panes.json` → uncatchable stack overflow. Local-file vector. Fix: read via `ConfigManager::LoadJsonFile`.

**12. [Low][Security/DoS] Bare `json::parse` on toolbar/keybinding `ArgsJson` (2 sites).** `SmatchetToolbarUi.cpp:101`, `SmatchetUI.cpp:826`. Config-sourced `ArgsJson` (from `smatchet_config.json`) bare-parsed; deep DOM → uncatchable teardown crash. Same class as prior #24/#28/#29 but these sites are absent from that doc. Fix: `ParseBounded` with empty-object fallback.

**13. [Low][Security/DoS] `BackendAuditTrail::ReadRecentEvents` bare-parses each audit-jsonl line.** `Persistence/BackendAuditTrail.cpp:403`. A depth bomb fits in a single line; app-written file, needs local tamper. Fix: `ParseBounded` per line, skip failures.

**14. [Low][Security] Server-controlled Jira priority `iconUrl` drives an arbitrary-URL fetch (SSRF).** `Source/Core/src/Ui/SmatchetFieldIconRender.cpp:112-127` → `181-200` (`HttpGetBinary`). `iconUrl` read verbatim from tracker priority JSON; any absolute `http(s)` URL is fetched via `cpr::Get` with no host allow-list. A malicious tracker sets `iconUrl` to `http://169.254.169.254/...` → the victim GETs internal/loopback endpoints on every grid render of that priority cell. Fix: confine icon fetches to the configured tracker origin (allow only relative `/path`, or reject hosts ≠ `cfg.Domain`).

**15. [Low][Security] `whisper.simulate-press` / `transcribe-once --seconds` start real WASAPI mic capture from the MCP/CLI command surface.** `Source/Plugins/Whisper/WhisperPlugin.cpp:1002-1029, 516-535`. `simulate-press` is registered `AsyncSafe=true` in the global registry and calls `capture.Start()` from any command source including loopback MCP `tools/call`; `--seconds` records up to 600 s. Consent-gated (`consent::CanCaptureMic`), so it needs Whisper enabled+set-up — then a local MCP client silently triggers the mic with no hotkey/prompt. Does not itself exfiltrate (fills the ring buffer). Fix: gate capture-initiating commands behind a non-MCP `CommandSource` check or per-invocation confirmation.

**16. [Low][Security] SSRF denylist misses the upper half of IPv6 link-local `fe80::/10` (`fe90::`–`febf::`).** `Source/Core/src/AiEndpointSanitize.cpp:240-243` (`ClassifyIpv6Literal`). Only string-matches `"fe80:"`; `fe90::1`/`fea0::1`/`febf::1` fall through to `Allowed`. For an unpinned provider (Ollama/DeepSeek) a config-write attacker could set a base URL to an un-denied link-local literal. Fix: reject the whole /10 (first hextet in `[0xfe80, 0xfebf]`).

**17. [Low][Integer handling] Signed-int overflow (UB) in `ParseSemanticVersion` via `std::atoi` on an unbounded digit run from a GitHub release tag.** `Source/Core/src/AttachmentAppUpdateService.cpp:329`. Version component validated only as all-digits, no length cap; `std::atoi` on `>INT_MAX` (e.g. `v99999999999.0.0`) is UB. TLS-pinned host → needs MITM/compromise. Fix: `std::stoll` + `INT_MAX` range check, mirroring `CallstackParser::ParseLineNumberInRange`.

**18. [Low][Integer handling] Signed-overflow UB in `ParseLinearIssueKey` / `ParseLongOr` digit accumulation on attacker-supplied strings.** `Source/Core/src/Tracker/LinearClientHelpers.cpp:73` (and `:40`). `number = number*10 + (c-'0')` into `int64` with no range guard; `ENG-99999999999999999999` overflows (UB) and the garbage value goes into the GraphQL `number.eq` filter. `ParseLongOr` has the identical pattern on `long` (32-bit on Windows), fed from server-controlled `x-ratelimit-*` / `x-complexity` headers. Fix: cap digit count or checked accumulation.

**19. [Low][Integer handling] Signed overflow in `ParseDurationToSecondsForSort` unit accumulation.** `Source/Core/src/TicketGridModel.cpp:74-88`. `total += num * kSecondsPerWeek` multiplies an unbounded `stoll` result by up to 144,000 with no range check; `"99999999999999w"` overflows `long long` — UB in a sort comparator. Distinct new site of the prior audit's finding-#17 class. Fix: saturate before multiply/add.

**20. [Low][Memory safety] Unbounded self-recursion (stack overflow) on a cyclic dense Lua array in `LuaObjectToIssueFieldString`.** `Source/Core/src/AppController_LuaBindings.cpp:158-176`. The dense-array join recurses with no depth/cycle guard (unlike `LuaToJson`/`JsonToLua`, which cap at 64). `local t={}; t[1]=t; tracker.create_issue({f=t})` → infinite recursion → uncatchable `SIGSEGV`. Trusted-local Lua, so self-inflicted. Fix: thread a depth counter (bail past 64).

**21. [Low][Memory safety] Latent UAF — dictation shadow-buffer fallback can splice into a freed InputText buffer.** `Source/Core/include/DictationInsertionRouter.h:161-164`, used at `DictationInsertionRouter_Whisper.cpp:280-308`. When `entries_` is empty (blur unregistered), a fallback `memcpy`/`memmove` targets a retained raw `char* shadowBuf_`; if a non-static target buffer is freed while a multi-second transcription is in flight, the deferred splice writes freed memory. Unreachable today (all four registration sites use static buffers — the header documents the invariant). Fix: gate the fallback on a registration validity token, or drop it.

**22. [Low][Memory safety] Unchecked `icon.Texture` deref in the explicit-size branch of `DrawImagePathOrUrl`.** `Source/Core/src/Ui/SmatchetFieldIconRender.cpp:587`. The zero-size branch guards `Texture == nullptr` (via `DrawLoadedIconSized`); the explicit-size branch calls `ImGui::Image(icon.Texture->GetTexRef(), ...)` with no null check. If the cache ever reports success with a null `Texture` (failed/zero-dim decode entry) the explicit-size path null-derefs. Fix: mirror the null guard.

**23. [Low][Concurrency] Annotate background-task callbacks dereference `s_stateInstance` that the destructor nulls out.** `Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:303-343, 364-398`. `LaunchBackgroundTask` → `PostToMainThread([...]{ State().x = ...; })`; the destructor doesn't drain the pool or pending callbacks, and `~AnnotateAnalysisUi` sets `s_stateInstance = nullptr`. A queued callback after teardown (or Unreal hot-reload) writes through a null/stale `State()`. Fix: capture a guard token / null-check before `State()`, or drain outstanding tasks first.

**24. [Low][Concurrency] `TokenizeCached` returns a reference into `g_cache` after releasing the lock.** `Source/Core/src/Ui/CodeColorView.cpp:479-499`. Returns `const std::vector<Token>&` into `g_cache`; the lock is released, then the caller iterates unlocked. A later concurrent call's FIFO eviction can `erase` the iterated key → UAF. Latent (draw paths are UI-thread-only today) but the mutex implies multi-thread intent. Fix: return by value / copy under lock.

**25. [Low][Concurrency] `SmatchetLocalization::T`/`TranslateSource` return a pointer into the overrides map that `SetLanguage` can free.** `Source/Core/src/SmatchetLocalization.cpp:1104-1108, 1126-1133` vs `1007-1009`. Returns `overrideIt->second.c_str()` after releasing `LocalizationMutex`; `SetLanguage` → `LoadOverridesLocked` → `OverridesRef().clear()` destroys those strings → dangling `const char*` (UAF/garbage render). Needs an override file + a language switch; English mode is safe (static literals only). Fix: return override strings through the `StoreTempString` ring (copy), like `Format`/`Label`.

**26. [Low][Concurrency] POSIX child does non-async-signal-safe work between `fork()` and `exec()`; parent hangs forever when `timeoutMs == 0`.** `Source/Core/src/SubprocessCapture.cpp:456-527, 612`. In this heavily multithreaded process, the post-`fork` child calls `vector`/`string`/`setenv` (allocator lock) → can deadlock pre-`exec`; the parent pump with `timeoutMs == 0` and no cancel token waits forever → permanent worker hang. Fix: restrict the child to async-signal-safe calls (prebuilt argv/envp + `execve`) or use `posix_spawn`; enforce a non-zero default timeout.

**27. [Low][Concurrency] `WindowsAudioCapture::DrainCapturedPcm` reads `pcmBuffer_.empty()` outside `pcmMutex_`.** `Source/Plugins/Whisper/WindowsAudioCapture.cpp:507-520`. The early-return guard reads `pcmBuffer_.empty()` unlocked while the capture thread mutates it under the mutex; benign only because of an implicit ordering invariant (the worker never writes after clearing `running_`). Fix: take `pcmMutex_` before inspecting, or drop the `.empty()` term.

**28. [Low][Resource management] Injected-font buffer referenced by the atlas with `FontDataOwnedByAtlas=false` can be reassigned, dangling the atlas pointer.** `Source/Core/src/Ui/SmatchetImGuiFonts.cpp:381-385, 406-415`. The atlas holds a raw pointer into `g_InjectedFontBytes` (owned=false); `SmatchetSetInjectedFontBytes` `assign`/`clear` reallocates. Re-injection after the atlas is built (without an intervening `Clear()`) → the next on-demand glyph bake reads freed memory (UAF). Fix: copy into an atlas-owned allocation, or keep the prior buffer alive until the next full rebuild.

**29. [Low][Resource management] ImGui backend `Shutdown()` called without a matching `Init()` on the renderer boot-failure path.** `Source/Standalone/StandaloneAppBootstrap.cpp:464-478`. If `ImGui_ImplGlfw_InitForOpenGL` fails before `ImGui_ImplOpenGL3_Init`, `TeardownPartialBoot` still calls `ImGui_ImplOpenGL3_Shutdown()` (never init'd); with `IM_ASSERT` compiled out in release, that's a null-deref crash instead of the intended clean error return. DX12 path is symmetric. Fix: track which backends actually initialized and only `Shutdown()` those.

**30. [Low][Resource management] Meyers-singleton `std::async` p4 futures in `P4ClPreview`/annotate detach lists are never joined at process exit.** `Source/Core/src/Ui/P4ClPreview.cpp:23-33, 101-119`. A function-local static holds `shared_future`s of `std::async(p4 describe)`; shutdown mid-flight blocks at static destruction or touches a destroyed singleton. Fix: bounded-timeout drain at shutdown, or pooled fetches drained on shutdown.

**31. [Low][Resource management] `SubprocessCapture::DispatchLines` pending-line accumulator is unbounded while stdout capture is byte-capped.** `Source/Core/src/SubprocessCapture.cpp:54-71`. With `onStdoutLine` set, a newline-free stream grows `pending` without a cap → host OOM, defeating the byte cap in the same function. Fix: cap `pending`; drop/flush + flag on overflow.

**32. [Low][Error handling] `SmatchetAndroidImeBridge::Init` dereferences an unchecked `NewGlobalRef` and leaves a pending JNI exception on the class-lookup failure path.** `Source/Mobile/Android/SmatchetAndroidImeBridge.cpp:22-27`. `NewGlobalRef` result used in `GetObjectClass` with no null check (UB on `NULL`); the `clazz == nullptr` path returns without `ExceptionClear()` or `DeleteGlobalRef`, so the next JNI call on the thread aborts under CheckJNI. The sibling `SecretBridge::Init` handles all three cases. OOM-gated. Fix: mirror `SecretBridge`.

**33. [Low][Logic/correctness] Cluster of correctness bugs in field-edit / query paths (5 distinct sites):**
- **Year-boundary UTC-offset inverted** — `TicketFieldEditor.cpp:101-106` (`GetCurrentJiraDateTimeString`): the `tm_yday` comparison adds a day to local instead of UTC around New Year for negative-offset users, producing a nonsense timezone suffix (e.g. `+43:00`) in the worklog `DateStarted` sent to Jira. Fix: compare full civil dates including year.
- **JQL "order by" matched inside quoted strings** — `Sync/JqlChangedSincePure.cpp:44-70` (`FindTrailingOrderBy`): a view JQL like `summary ~ "sort order by date" AND ...` splits inside the string literal → broken changed-since poll → notifications silently stop. The GitHub translator fixed this exact class (`GitHubQueryFromJql.cpp:240-244`); the Jira helper still has it. Fix: quote-aware scan.
- **Single-select combo clears the field for id-less options** — `TicketFieldEditor.cpp:685-698`: the ImGui ID uses the `option.Id.empty() ? option.Value : option.Id` fallback, but the commit paths queue the raw `option.Id`, so selecting an id-less option queues `{""}` (a clear). The multi-select body uses the fallback deliberately. Fix: queue the same fallback in the click + enter-commit paths.
- **`PromptAi` turns always discarded** — `AiAssistantController.cpp:445-447, 505-507`: `PromptAi` gens are seeded at `1<<32` while the UI's `assistantTurnGen` only ever holds small values, so the stale-turn gate is always true → every delta/final/error is dropped after a billable streaming request completes; a `PromptAi` turn after a model-config change even wipes the visible chat. Acknowledged "Phase E" gap; reported because it spends real API quota for a guaranteed-invisible result. Fix: route `PromptAi` through the UI gen counter, or short-circuit with an "unsupported" error.
- **`QuoteWinArgWidePure` drops empty arguments** — `include/Ui/P4vLaunchArgQuotePure.h:24-29`: an empty arg takes the bare-token fast path and returns `""` (nothing) instead of the CommandLineToArgvW-required `""`. Benign at current call sites (trailing positional) but any future non-trailing caller gets silent argument shifting — the exact bug class this pure helper exists to prevent. Fix: `if (arg.empty()) return L"\"\"";`.

**Also latent (build/robustness):**
- **[Low][API misuse] `StringUtil.h` calls `std::strtoll` without `#include <cstdlib>`** (`include/StringUtil.h:74`). Compiles via transitive includes today; a stricter/modular stdlib breaks every TU including this 75+-includer foundation header. Fix: add the include.
- **[Low][API misuse] `CliCommandRunner.cpp` uses `_NSGetExecutablePath` without `#include <mach-o/dyld.h>`** (`Source/Standalone/CliCommandRunner.cpp:227-230`). macOS build with MCP enabled → compile error (`main.cpp` includes it; this TU doesn't). Fix: add the guarded include.
- **[Low][Performance] `CommandPaletteUi::rebuildFiltered` rebuilds the full command table up to 9× per invocation** (`Source/Core/src/Commands/CommandPaletteUi.cpp`). `All()` (locks + deep-copies + sorts the whole table) was called once per recent (≤8) plus once more, where one snapshot suffices. Fix: call `All()` once and cache locally. — **✅ FIXED 2026-07-05** (snapshot hoisted to a single `const std::vector<Command> all = app.Commands().All();` reused by both branches).
- **[Low][Error handling] `EnsureAssetsBranch` does index-then-value on unvalidated network JSON** (`Diagnostics/BugReportService.cpp:255`). `["object"].value(...)` on a non-object throws `type_error`; caught → degrades to local staging (not a crash), but fragile vs guarded siblings. Fix: `contains("object") && is_object()` guard.

> These last four are folded under finding #33's cluster for the count; each is an independent Low.

---

## Remediation confirmations (prior audit)

Auditors independently confirmed the following `SECURITY_AUDIT.md` items are **already fixed** in the current tree and did not re-report them:
- **#1 (locale format-string in `Format`)** — the specifier-match guard is present in `SmatchetLocalization::Format`. (But it does **not** cover the `TranslateSource` wrappers — see finding #7.)
- **#2 (`whisper.transcribe-once --file` arbitrary read)** — mitigated by `ConfinePathUnderSubdir` + extension/regular-file/size/RIFF-magic checks (`WhisperPlugin.cpp:455-513`).
- **#2/#3/#19 (perf.dump / scenario.run / ui_test.run path write, localtime null-deref)** — output paths now routed through `ConfinePathUnderSubdir`; `localtime` results null-checked.
- **#20 (`LuaToJsonImpl` sparse-array allocation)** — a node budget is now threaded through both branches.
- **#23 (`TryParseJsonMaybeDoubleEncoded`)**, **#28 (`SmatchetImGuiHost` top-level args)**, **#29 (offline-queue payload/conflict)**, **#31 (`AttachmentAppUpdateService` GitHub release)** — routed through `json_safe::ParseBounded`.
- Multiple Jira/Plane/GitHub TUs (`JiraIssueSearch`, `JiraIssueMutation`, `JiraUserAndMeta`, `PlaneActivityFeed`, `GitHubActivityFeed`, `FieldCatalogCache`, `TicketFieldEditorLongTextPure`, `CommandRegistry::LoadRecents`, `config.set`) — migrated to `ParseBounded`. The gaps in findings #8/#9/#10–13 are the **sibling** TUs the sweep missed.

## Recommended remediation order

1. **#1 (data loss)** — highest user impact; any GitHub/Jira description over 64 KiB is silently corrupted on edit.
2. **#2 (GitHub credential mis-routing)** — breaks authenticated GitHub in CI/container deployments and mis-files the PAT.
3. **#3 (duration-sort infinite loop)** — trivial one-line fix; a single hostile field value freezes the app.
4. **#8/#9 (Jira-catalog + Plane bare-parse)** — complete the ParseBounded sweep; uniform mechanical fix already proven elsewhere in the tree.
5. **#4/#5/#6 (AI cancel, TextMerge OOM, replay latch)** — the offline-replay/AI reliability cluster.
6. Remaining Low findings as hardening / defense-in-depth.
