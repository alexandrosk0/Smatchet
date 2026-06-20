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
         (Pillar 2 MEDIUM) — 2 of 3 sub-parts LANDED (PR #1271): cooperative
         cancel (count-hook's pure abort policy observes automationWorkerShuttingDown_)
         + bounded shutdown join (automationWorkerExited_ atomic + automationJobCv_
         wait for kAutomationJoinWarnDeadline 5 s + LOG_WARN before join). RESIDUAL
         still-open slice: part 3 only — the blocking synchronous tracker call at
         JiraIssueMutation.cpp:206 (the UI-thread-starvation half) is DEFERRED to a
         tracker-backend follow-up (cross-subsystem lift, not a clean Lua-host fix).
         The per-finding entry below keeps Status: open for that residual; see its
         Implementation note (2026-06-15) for detail.
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

- 2026-06-13 · deep-audit · [security] · P2 — MCP attachment proxy fetches caller-supplied URLs (SSRF surface)
  Details: Source/Plugins/Mcp/McpPlugin.cpp:275-352 fetches a caller URL; the mcp-lane coverage found it already HTTPS-only + host-allow-listed (tracker domain + api.media.atlassian.com) with redirects disabled, so this is a confirm-it-routes-through-the-shared-AiEndpointSanitize hardening rather than a live SSRF.
  Concrete next action: Route the fetch through the shared sanitizer; deny private/link-local/metadata targets and non-http(s) schemes. Effort S-M.
  Status: deferred — per the 2026-06-14 campaign disposition block (DEFERRED: "MCP attachment-proxy SSRF (#5)"). Not a live SSRF: already HTTPS-only + host-allow-listed (tracker domain + api.media.atlassian.com) + redirects disabled. Shared-sanitizer routing is confirm-only hardening, tracked not coded this campaign.
  Last-reviewed: 2026-06-18

- 2026-06-13 · deep-audit · [security] · P2 — Automation worker hook aggravates shutdown deadlock / UI-thread starvation
  Details: The instruction-count worker hook interacts with shutdown so a long automation can hold the process from exiting (verifier raised LOW→MEDIUM). Re-run located it at AppController_LuaBindings.cpp:1257 (LUA_MASKCOUNT, 50000, only checks shuttingDown_) chaining to blocking JiraIssueMutation.cpp:206 — also a UI-thread block (Pillar 2) since the count-hook does not cover blocking C++ glue.
  Concrete next action: Make the hook cooperatively cancellable; bound shutdown wait with timeout/forced-join; keep blocking glue off the UI thread. Effort M.
  Status: partially applied (2026-06-20 trap-sweep — shipped: parts 1+2 cooperative-cancel + bounded-join PR #1271; remaining: part 3 UI-thread sync tracker call, deferred to tracker-backend follow-up)
  Implementation note: 2026-06-15 (PR #1271) — two of three sub-parts landed; finding stays open for the third. (1) Cooperative cancel: the count-hook's pure abort policy (`LuaAutomationHookPolicyPure::DecideAutomationAbort`) already aborts the running script when `automationWorkerShuttingDown_` is set (covered by `tests/Core/LuaAutomationHookPolicyPure.test.cpp`), so a Lua-bound automation is cooperatively cancellable at the next count-hook tick — confirmed, no code change needed. (2) Bounded shutdown join: `~AppController()` no longer issues an unbounded `automationWorker_.join()`. The worker loop now sets a new `automationWorkerExited_` atomic and notifies `automationJobCv_` on exit (`AppController_LuaBindings.cpp` `AutomationWorkerLoop`, `AppControllerImpl.h`); the dtor waits on that CV for a bounded `kAutomationJoinWarnDeadline` (5 s), emits a loud `LOG_WARN` naming the likely blocking-glue cause if the worker has not exited, then joins. Shutdown is now bounded by the in-flight HTTP call's own timeout rather than hanging silently and is diagnosable from the warning (Pillar 2/3). All new state is inside `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` — Lua-OFF build and the bindings/stubs parity are unaffected (these are `AppController` methods, not new Lua glue). (3) DEFERRED — UI-thread block: the blocking synchronous tracker call at `JiraIssueMutation.cpp:206` reached from the hook chain is the Pillar-2 UI-thread-starvation half. Left untouched: moving it off the UI thread is a tracker-backend (not Lua-host) change that risks destabilizing the very shutdown path just bounded, and is a cross-subsystem redesign, not a clean lift. Flagged as a tracker-backend follow-up — re-scope as its own item under the JiraIssueMutation owner.
  Last-reviewed: 2026-06-20

