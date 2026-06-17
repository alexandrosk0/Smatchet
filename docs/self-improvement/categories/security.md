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
       * #2/#3 base gate — RESOLVED 2026-06-14 (PR #1246 "source-aware
         destructive guard + close MCP/Lua confirm hole"). The trust-model call
         was MADE and shipped, not deferred: CommandRegistry now denies/require-
         confirms destructive commands from automation sources
         (CommandRegistry.cpp:302-307 — IsAutomationSource + RequiresExplicitConfirm),
         so MCP/Lua-sourced destructive ops no longer equal UI reach. The two
         per-finding rows below (CommandRegistry no-authz, MCP dispatch un-gated)
         are marked fixed. RESIDUAL still-open slice: only the Unreal-console embed
         entry-point (#24) — its ctx.Source must be tagged = UnrealConsole so the
         now-live gate fires for it (see the target-24 entry below).
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
  Update (2026-06-16): the base per-source trust gate now EXISTS (PR #1246, CommandRegistry.cpp:302-307) — the #2/#3 rows are resolved. This entry NARROWS from "gate absent" to "entry-point not tagged": the Unreal console/Blueprint dispatch must set ctx.Source = UnrealConsole so the now-live gate actually fires for it (an untagged source currently defaults to UI-equivalent reach and bypasses the confirm). Stays OPEN for that embed-entry-point slice only.
  Status: open
  Last-reviewed: 2026-06-16

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

- 2026-06-13 · deep-audit · [security] · P2 — Command registry executes destructive commands with no ctx.Source authorization
  Details: Source/Core/src/Commands/CommandRegistry.cpp:298 gates only on (Destructive && !ConfirmedDestructive); no ctx.Source trust check, so MCP/Lua-sourced commands equal UI-sourced. Basis shared with MCP un-gated dispatch.
  Concrete next action: Add a per-source trust enum; gate destructive/source-restricted commands and require out-of-band confirm for non-UI sources. Effort M.
  Resolution (2026-06-14 · PR #1246 fix(commands): source-aware destructive guard + close MCP/Lua confirm hole): CommandRegistry now carries the ctx.Source trust check. `IsAutomationSource(ctx.Source)` audits a destructive automation-sourced command (CommandRegistry.cpp:302) and `RequiresExplicitConfirm(ctx.Source, snapshot.Destructive, ctx.ConfirmedDestructive, ctx.DryRun)` (CommandRegistry.cpp:307) denies it without an out-of-band confirm — so MCP/Lua-sourced destructive commands no longer equal UI reach. The audit's `CommandRegistry.cpp:298` line-cite is pre-fix; the live gate is at :302-307.
  Status: fixed
  Last-reviewed: 2026-06-16

- 2026-06-13 · deep-audit · [security] · P2 — MCP registry dispatch un-gated after Authorize
  Details: Source/Plugins/Mcp/McpPlugin.cpp:426-445,625-642 — after loopback+token Authorize, dispatch reaches the full command registry with no per-command authorization (token possession == full reach).
  Concrete next action: Apply a command allowlist/capability scope to the MCP surface; route destructive commands through the source-aware gate. Effort M.
  Resolution (2026-06-14 · PR #1246): the destructive-reach half is closed — MCP dispatch now flows through the same source-aware gate as the entry above (RequiresExplicitConfirm denies a destructive MCP/Lua command without an out-of-band confirm), so token possession no longer grants unconfirmed destructive reach. RESIDUAL (optional defense-in-depth, NOT a live hole): a positive capability allowlist scoping even non-destructive commands per MCP token is not implemented; the security-critical destructive-confirm gap is fixed.
  Status: fixed
  Last-reviewed: 2026-06-16

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
  Resolution (2026-06-14 · branch fix/posix-secret-at-rest-perms-h2): POSIX half fixed — `::chmod(tmp, S_IRUSR | S_IWUSR)` (0600) is applied to the temp file before the atomic rename (ConfigManager_PathUtils.cpp:837), and a loose-permission `LOG_WARN` fires on read via the unit-tested `IsLooseConfigFileMode` (ConfigManager_PathUtils.cpp:402). The Android half remains a SEPARATE still-open item (the Android passthrough entry below + the 2026-06-14 P2 Android Keystore entry above). This POSIX entry is closed.
  Status: fixed
  Last-reviewed: 2026-06-16

- 2026-06-13 · deep-audit · [security] · P2 — Android secret passthrough stores credentials in plaintext (re-run: HIGH at-rest exposure; raise to P1 when Android ships)
  Details: Source/Core/src/Config/ConfigManager_PathUtils.cpp:331-334 passes secrets through with no Android Keystore encryption. Same no-op #else branch as the POSIX entry above; confirmed HIGH plaintext-at-rest by the re-run.
  Concrete next action: Back Android secrets with the Keystore or mark the platform unsupported for secret storage. Effort M.
  Status: open
  Last-reviewed: 2026-06-13

- 2026-06-13 · deep-audit · [security] · P2 — Automation worker hook aggravates shutdown deadlock / UI-thread starvation
  Details: The instruction-count worker hook interacts with shutdown so a long automation can hold the process from exiting (verifier raised LOW→MEDIUM). Re-run located it at AppController_LuaBindings.cpp:1257 (LUA_MASKCOUNT, 50000, only checks shuttingDown_) chaining to blocking JiraIssueMutation.cpp:206 — also a UI-thread block (Pillar 2) since the count-hook does not cover blocking C++ glue.
  Concrete next action: Make the hook cooperatively cancellable; bound shutdown wait with timeout/forced-join; keep blocking glue off the UI thread. Effort M.
  Status: open
  Implementation note: 2026-06-15 (PR #1271) — two of three sub-parts landed; finding stays open for the third. (1) Cooperative cancel: the count-hook's pure abort policy (`LuaAutomationHookPolicyPure::DecideAutomationAbort`) already aborts the running script when `automationWorkerShuttingDown_` is set (covered by `tests/Core/LuaAutomationHookPolicyPure.test.cpp`), so a Lua-bound automation is cooperatively cancellable at the next count-hook tick — confirmed, no code change needed. (2) Bounded shutdown join: `~AppController()` no longer issues an unbounded `automationWorker_.join()`. The worker loop now sets a new `automationWorkerExited_` atomic and notifies `automationJobCv_` on exit (`AppController_LuaBindings.cpp` `AutomationWorkerLoop`, `AppControllerImpl.h`); the dtor waits on that CV for a bounded `kAutomationJoinWarnDeadline` (5 s), emits a loud `LOG_WARN` naming the likely blocking-glue cause if the worker has not exited, then joins. Shutdown is now bounded by the in-flight HTTP call's own timeout rather than hanging silently and is diagnosable from the warning (Pillar 2/3). All new state is inside `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` — Lua-OFF build and the bindings/stubs parity are unaffected (these are `AppController` methods, not new Lua glue). (3) DEFERRED — UI-thread block: the blocking synchronous tracker call at `JiraIssueMutation.cpp:206` reached from the hook chain is the Pillar-2 UI-thread-starvation half. Left untouched: moving it off the UI thread is a tracker-backend (not Lua-host) change that risks destabilizing the very shutdown path just bounded, and is a cross-subsystem redesign, not a clean lift. Flagged as a tracker-backend follow-up — re-scope as its own item under the JiraIssueMutation owner.
  Last-reviewed: 2026-06-15

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

- 2026-05-17 · security-review · [security] · P2 — First-send outbound-context consent modal (consent-tracking field + UX)
  Details: Default flip of `AssistantContextBlockAuditTrail` to `false` shipped (`ConfigManager.h:248`); remaining work from the original P1 entry is the one-time first-send consent modal. Modal should list the 5 `AssistantContextBlock*` block names + sample payload sizes + a "what gets sent" expander before the first turn. Drive via a new `cfg.AssistantOutboundConsentShown = false` field. Severity downgraded P1→P2 because the riskiest default (audit-trail PII auto-shipping) is now off.
  Concrete next action: add `cfg.AssistantOutboundConsentShown` (default false); gate `AiAssistantController::RunRequest` on the consent modal first turn; render modal in `SmatchetAiAssistantUi.cpp`. ~3 h UX.
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
