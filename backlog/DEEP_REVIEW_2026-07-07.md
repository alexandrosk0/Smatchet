# Deep code review — untracked findings (2026-07-07 whole-tree pass)

> Companion to [`BACKLOG_CODE_REVIEW.md`](BACKLOG_CODE_REVIEW.md) and
> [`POST_P0_REVIEW.md`](POST_P0_REVIEW.md). This document captures findings from a
> full-tree review (develop tip ~PR #1673) that are **not already tracked** in the two
> companion docs. The prior passes were scoped to `Source/Core/` + `Source/Plugins/` +
> `Source/Standalone/` against a tree ~1500 PRs old; most items below live in code added
> or heavily reworked since — the `Ui/` decomposition, the `Tracker/` backend split, the
> AI client stack, and the `scripts/` CI gates.
>
> Method: 16 parallel reviewer agents, one partition each, every first-party source file
> read in full. Code-only: comments/docs were treated as claims to verify, not evidence.
> Each item carries `file:line` citations and a concrete failure scenario.
>
> Status legend (matches the companion docs):
> - ⏳ **OPEN** — not yet fixed.
> - 🟡 **PARTIAL** — first step shipped; follow-up tracked inline.
> - ✅ **DONE** — landed on `develop`.
>
> Items prefixed **DR** (Deep Review). Where a finding touches an item the companion docs
> mark ✅ RESOLVED, the cross-reference is called out inline — three of these are residual
> holes in a resolved fix, not fresh regressions.

---

## Status snapshot (re-verified 2026-08-16 against develop tip `7da969b`)

| Severity | Count | State |
|----------|-------|-------|
| **P0** — data-loss / security / crash | 17 | 17 ✅ |
| **P1** — significant correctness | 13 | 13 ✅ (DR29 closed 2026-08-18 by PR #2121) |
| **P2** — polish / consistency | 3 | 3 ✅ |

**DR29 is the only item not closed**, and its scope shrank: the ODR violation and three of the
vacuous guards are fixed; three self-referential tests remain, all blocked on the same cause — the
production helper lives in a translation unit the focused test rig cannot link.

Findings landed across three PRs: batch 1 (DR1/DR2/DR14/DR18/DR19/DR26/DR30/DR32/DR33) via
PR #1676; batch 2 (the remaining 24) from parallel fix agents; batch 3 closed the **DR6** residual
(the long-lived Lua/MCP worker `Cache` race — fixed by giving `Cache` the ADR-0012 `shared_ptr` +
`atomic_load`/`atomic_store` treatment, same as `Backend`) and one more **DR29** guard (the
`ai_prefs` self-referential flag). **DR29 is now ✅ RESOLVED** (2026-08-18, PR #2121): the ODR rename plus
four vacuous guards are fixed; the remaining self-referential guards re-implement ImGui/AppController-
coupled production logic not linkable in the focused test rig (fixing them means extracting production
helpers per-symbol — a separate refactor), or would surface a distinct pre-existing latent bug. The
DR6 `SaveFieldCatalogSnapshot` snapshot-under-lock note was a writer-vs-writer polish item, not the
UAF, and is folded into DR6-DONE (the reader-vs-writer race that was the finding is closed).

**Cross-references into resolved companion items:** DR15 (POST_P0 #16), DR16 (BACKLOG B2),
DR17 (BACKLOG A1) — each is a hole the prior fix left open. DR30 overlaps BACKLOG **N12**
(same theme, distinct mechanism). DR13b is distinct from POST_P0 #31.

---

## P0 — data loss / security / crash

### DR1. Three-way merge silently drops edits while reporting "clean" — ✅ DONE (fix pushed on branch)
`Source/Core/src/TextMerge.cpp:189`. The hunk-discard loop advances past any hunk with
`baseEnd <= cursor` **without** setting `isClean=false`. For base `"a"`, mine `"a\nx"`
(queued offline append), theirs `"A"` (server edit): theirs applies, then mine's insertion
is discarded and the result is `{IsClean:true, "A"}`. `OfflineQueueService::ApplyOrRecordMergeResult`
PUTs it as a clean merge — the user's queued edit vanishes with no conflict recorded and no
resolution offered. Symmetric loss in the reverse direction.
- **Fix:** set `isClean=false` in both discard loops (mi/ti) so a consumed-range collision
  degrades to a recorded conflict instead of a silent drop. Add golden cases for the append-vs-replace
  and replace-vs-edit orderings.
- **Related:** `TextMerge.cpp:14` `SplitLines` keeps `\r` and `JoinLines` drops the trailing
  newline, so CRLF-vs-LF inputs conflict spuriously and clean merges lose the final `\n` (P1; fold in here).

### DR2. AI endpoint validator bypassed by URL userinfo + IPv4-mapped IPv6 — ✅ DONE (fix pushed on branch)
`Source/Core/src/AiEndpointSanitize.cpp:38,66`. `ExtractHostPort` returns everything between
`://` and the first `/?#` and never strips the `user:pass@` userinfo (no `@` handling exists
in the file); `StripPort` then splits at the first `:`. So `https://api.openai.com:x@evil.com/`
validates as host `api.openai.com`, passes the provider host-pin, and curl connects to
`evil.com` carrying `Authorization: Bearer <key>`. `https://a:b@169.254.169.254/` validates
host `a` and defeats the cloud-metadata denylist. Separately, `:240`: the IPv4-mapped form
`[::ffff:a9fe:a9fe]` (= 169.254.169.254) is classified "public" because the embedded-IPv4
check requires a dot.
- **Threat model is exactly this validator's purpose** (a config-write attacker). Key exfiltration
  + SSRF to link-local/metadata.
- **Fix:** strip userinfo (reject or drop everything up to the last `@` in the authority) before
  host extraction; canonicalise IPv4-mapped IPv6 (`::ffff:x`) through `CanonicalizeIpv4` regardless
  of dotted form. Add denied-URL unit cases for both.

### DR3. Config load/save can permanently wipe stored credentials — ✅ DONE (batch 2)
`Source/Core/src/Config/ConfigManager.cpp:1541`. `Load` wraps every field loader in one
try/catch, so a single type-mismatched key (`"window_x":"abc"`) aborts secret/list loading,
leaving `cfg` with empty secrets; a later `Save` writes empty `*_enc` and erases the plaintext
fallback → all tokens lost. `:450`: Win32 `WriteSecretFields` unconditionally erases the
plaintext `token`/`plane_api_key` and writes a possibly-empty `token_enc` even when DPAPI
`CryptProtectData` failed, unlike the fallback applied to the other three secrets.
- **Fix:** load each field group in its own try/catch (a bad scalar must not drop secrets);
  in `WriteSecretFields`, keep the existing value / plaintext fallback when encryption returns empty,
  matching the `github_pat`/`linear_api_key`/`mcp_auth_token` paths.

### DR4. Streaming full-sync of zero tickets deletes the whole cache — ✅ DONE (batch 2)
`Source/Core/src/Sync/TicketSyncService.cpp:618`. The streaming stale computation lacks the
empty-full-sync guard that `ApplyIssueFetchPack` has (`kEmptyFullSyncWipeThreshold`). A
`FullSyncCompleted=true` with an empty body (200-with-empty-body glitch, transiently-broken JQL)
makes `workerKeepIds` empty, so every cached id is stale and `DrainStaleDeletionBudget` deletes
the entire SQLite ticket cache and empties `ActiveTickets` — wiping offline-available data.
- **Fix:** port the empty-result threshold guard to the streaming path; treat a zero-ticket full
  sync as suspect and skip mass deletion.

### DR5. Offline-queue replay: stuck latch, null-deref, and infinite resolve loop — ✅ DONE (batch 2)
- `Source/Core/src/Sync/OfflineQueueService.cpp:867`: the field-edit background lambda lacks the
  `ScopeExit` reset the creates path has; any throw (`RefreshLocalData` rethrows SQLite errors,
  `MarkdownToAdf` in the clean-merge path) leaves `offlineFieldEditReplayInFlight_` latched → queued
  edits never replay until restart.
- `:1427`: `TickOfflineCreates` doesn't null-check `deps_.MutationsShared()` before
  `ReplayOneCreate` dereferences it (the sibling `TickOfflineFieldEdits` guards both) → crash when
  the latched backend has no mutations support.
- `Source/Core/src/Persistence/LocalCacheManager.cpp:1039`: `ResolveFieldEditConflict` nulls
  `original_value` but not `has_original_value`, so a resolved scalar conflict keeps a captured-base
  flag over an empty base → `ServerMovedFromCapturedBase` re-suspends the row every tick → infinite
  resolve/re-conflict loop, edit never applied.
- **Fix:** add the `ScopeExit` latch reset; null-check `MutationsShared()` in `TickOfflineCreates`;
  reset `has_original_value=0` alongside `original_value` in `ResolveFieldEditConflict`.

### DR6. AppController Cache-reset UAF + unlocked field-catalog access — ✅ DONE (batch 3)
Cluster of threading/lifetime races (worker threads reach these off the UI thread via MCP/automation):
- `Source/Core/src/AppController.cpp:1286`: `RecreateLocalCacheDatabase` resets `Cache` while
  non-focused panes' streaming-sync `std::thread`s (spawned outside `backgroundWorkers_`), the Lua
  automation worker, and MCP workers still dereference it. `AppController_LuaBindings.cpp:761`
  `LuaGetTicketBind` dereferences `Cache` with no null check.
- `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:314` and
  `Source/Core/src/AppController_IssueCreateOffline.cpp:69,101`: read/mutate `AvailableFields`
  outside `availableFieldsMutex_`, though `GridLiveContext.h` declares that mutex guards those members
  and `SetFieldCatalog` runs on background workers → torn reads / reallocation UAF.
- **Fix:** join *all* Cache-touching workers (or gate them on a generation/lock) before `Cache.reset()`;
  route every `Cache` read through a null-checked accessor; take `availableFieldsMutex_` in every
  `AvailableFields` reader/writer, including the create/draft paths.

### DR7. ScenarioRunner replace-active → std::terminate + stub UAF — ✅ DONE (batch 2)
`Source/Core/src/Commands/Scenarios/ScenarioRunner.cpp:97`. `active_ = std::move(scenario)`
destroys a running scenario without `OnCancel`/`OnFinish`; the streaming scenarios own joinable
`std::thread` members with no joining destructor (`AiAssistantStreaming*Scenario.cpp`,
`AiAssistantSendScenario.cpp:271`) → `~std::thread` on a joinable thread calls `std::terminate`.
The `AiClientFactory::SetTestOverride` stub is also left pointing into the freed scenario.
- **Fix:** on replace, call the outgoing scenario's cancel/finish + join and clear the factory
  override before assigning; give each scenario a joining destructor.

### DR8. Shutdown UAF / null-deref on undrained futures — ✅ DONE (batch 2)
- `Source/Core/src/Ui/SmatchetUI_Layout.cpp:305`: `DrainUiDrawSessionFuturesBeforeAppTeardown`
  never drains `d.appUpdateFuture`, whose `std::async` worker captures `AppController&`
  (`SmatchetUI.cpp:93`, `SmatchetPreferencesUi_Local.cpp:421`) → UAF during shutdown if an update
  check is in flight; the future is only joined in g_ui's static destructor after `main()` returns.
- `Source/Core/src/Ui/AnnotateAnalysisUi.cpp:21`: destructor nulls `s_stateInstance` *before* the
  member destructor joins the worker, so the worker's `State()` derefs null during teardown.
- **Fix:** add `appUpdateFuture` to the teardown drain set; join the annotate worker before clearing
  `s_stateInstance` (or drop the static and thread the state pointer).

### DR9. Unreal plugin shutdown UAF — no render-thread flush — ✅ DONE (batch 2)
`Source/UnrealPlugins/SmatchetImGuiPlugin/.../SmatchetImGuiPluginModule.cpp:199`. `ShutdownModule`
destroys the native host and render backend on the game thread with no `FlushRenderingCommands()`,
while `OnBackBufferReadyToPresent` and already-enqueued RHI lambdas capturing `this`/`Host`
(`:303`) can still run → deref of freed module members and destroyed host on the RHI thread.
- **Fix:** `FlushRenderingCommands()` (or unregister the present callback + flush) before
  `SmatchetHost_Destroy`.

### DR10. Auto-update installer path traversal + attachment redirect SSRF — ✅ DONE (batch 2)
`Source/Core/src/AttachmentAppUpdateService.cpp:656`: the release asset filename is concatenated
into the `%TEMP%` path without the `SanitizeFilename()` applied elsewhere, so an asset named
`..\..\...\Startup\x-windows-setup.exe` (passes the `-windows-setup.exe` substring filter) is
written and `ShellExecute`-launched outside `%TEMP%`. `:149`: `DownloadAttachmentToLocalFile`
follows redirects (`cpr::Redirect(true,false)`) but only allowlists the initial URL, so a tracker
302 to an internal host is fetched to a local file (limited SSRF).
- **Fix:** sanitize the installer filename (strip separators / reject `..`) before building the
  temp path; re-validate the post-redirect host against the allowlist (or disable redirects and
  re-issue against the validated Location).

### DR11. LuaConsole saves editor buffer over the wrong file — ✅ DONE (batch 2)
`Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:269`. The selected script is tracked by integer
index while `RefreshScriptList()` re-sorts every ~0.35 s; a file appearing/disappearing shifts the
index and `SaveCurrentScript` overwrites whichever path now sits at that index — silent data loss
of two files (the edited one and the clobbered one).
- **Fix:** track the selection by path/identity, not list index; re-resolve the index after each refresh.

### DR12. MCP spawn-token lockout + CLI attach sends no token — ✅ DONE (batch 2)
`Source/Plugins/Mcp/McpPlugin.cpp:239`: adopting and scrubbing `SMATCHET_MCP_SPAWN_TOKEN` makes
`AuthTokenMatches` permanently false, so any later `SyncMcpPluginWithConfig` restarts the server
tokenless (`require_token_on_loopback=true`) and every subsequent parent request 401s.
`Source/Standalone/CliCommandRunner.cpp:1653`: the direct `cmd` attach path never sends
`X-Smatchet-Token`, so a tokenless-loopback-deny or operator-token config returns 401 and the
`--spawn` fallback (gated on `!res`) never engages.
- **Fix:** persist/retain the adopted spawn token for restart re-adoption (or don't scrub until after
  config carries it); send `X-Smatchet-Token` from the attach path using `spawnCfg.McpAuthToken`.

### DR13. Persistent-views file clobber + delete-wrong-view — ✅ DONE (batch 2)
- `Source/Core/src/Ui/Views.cpp:107`: `Views::Save` rewrites the whole file from the boot-time
  `Disk` snapshot, reverting the tracker-scope `ToolbarAppend` that `SmatchetToolbarUi`'s save worker
  (`SmatchetToolbarUi.cpp:669`) wrote out-of-band → toolbar buttons vanish on next restart/backend swap.
- **DR13b** `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:199`: right-click *Delete/Rename* on a
  non-active view acts on the **active** view when the editor is dirty (the dirty-guard latches the
  discard-confirm but the delete latch still targets the active view) → permanent loss of the wrong view.
  Distinct from POST_P0 **#31** (which fixed neighbour-selection after deleting the *last* view).
- **Fix:** re-read the file under lock and merge before `Views::Save` (or share the coalescing config
  writer); make delete/rename target the right-clicked view id, and resolve the dirty-editor case before
  latching the delete.

### DR14. CI gates silently pass on real violations — ✅ DONE (fix pushed on branch)
- `scripts/dev/coverage-delta-gate.sh:316`: a block comment closing with code on the same line
  (`/* … */ stmt();`) is classified EXEMPT → untested runtime code passes the required test-delta gate
  on `merge_group`. (Same `/* */ code` class as the single-line fix in #918; the multi-line close was missed.)
- `scripts/dev/pillar2-scan.sh:135`: a sync-I/O line whose stripped text starts with `*`
  (`*out = popen(...)`) is skipped as a comment continuation → bypasses the Pillar-2 CRITICAL gate.
- `scripts/dev/perf-compare.py:190`: `_to_float(...) or DEFAULT` replaces an explicit `0` knob
  (`mean_delta_pct:0`) with the looser default → strictest perf policy silently becomes permissive.
- `scripts/dev/osv-scan.py:90`: `severity_of()` parses only a bare-float `score`, so a CVSS *vector*
  string falls to UNKNOWN → a real HIGH/CRITICAL CVE evades `--fail-on HIGH`.
- **Fix:** in the two shell gates, classify the code tail after a same-line block-comment close /
  pointer-deref line; use an explicit `None` sentinel (not `or`) for the perf knobs; parse CVSS vectors
  in `severity_of`.

### DR15. UI-thread dispatcher can hang shutdown forever — ✅ DONE (batch 2) (hole in POST_P0 #16)
`Source/Core/include/Commands/MainThreadDispatch.h:63`. `RunOnUiThread` blocks in `future.get()`,
but the #16 hardening made `PostToMainThread` silently no-op after `BeginShutdown()` (called *first*
in `~AppController`, before `JoinBackgroundTasks`) and enforce a 4096-entry drop-oldest cap. A worker's
marshalled call posted after `BeginShutdown`, or evicted from a saturated queue, is never executed —
the caller-held promise is never fulfilled, no `broken_promise` fires, and `future.get()` blocks the
join forever. #16 closed a UAF and opened a deadlock.
- **Fix:** on `BeginShutdown` (and on queue eviction), fulfil the dropped task's promise with a
  cancelled/failed status so `RunOnUiThread` unblocks; or make `RunOnUiThread` wait with a deadline and
  return a shutdown error.

### DR16. Non-idempotent POST retried on post-send timeout → duplicate — ✅ DONE (batch 2) (hole in BACKLOG B2)
`Source/Core/src/Tracker/TrackerHttpUtils.cpp:240`. `TrackerPostLogged` retries only on
`TrackerErrorKind::Transport`, and its comment claims Transport = "provably never reached the server."
But `TrackerErrorFromHttpStatus` (`TrackerError.h:101`) maps `status <= 0` → Transport, and cpr uses
status 0 for *post-send* operation-timeouts too. A create/comment the server already committed, that
times out awaiting the response, is reclassified Transport and re-sent → duplicate issue/comment. B2's
"POSTs single-attempt/Transport-only by design" invariant holds only for queued offline creates
(the pending-create latch de-dups); direct online creates via `TrackerPostLogged` are exposed.
- **Fix:** distinguish connect/pre-send timeout from read/post-send timeout before classifying Transport
  (inspect `response.error.code` for `OPERATION_TIMEDOUT` after send), or make POST strictly single-attempt.

### DR17. Config cache re-populated stale during Save — ✅ DONE (batch 2) (hole in BACKLOG A1)
`Source/Core/src/Config/ConfigManager.cpp:611`. `Save` invalidates the cache *before* taking the RMW
lock and writing, so a concurrent `Load` between invalidation and `WriteConfigJson` re-caches the
pre-save config with `hasCached=true`, which then survives after the new file is written → every
subsequent `Load` returns the stale config until the next invalidation.
- **Fix:** hold the RMW/cache lock across invalidate→write, or invalidate (and re-populate) *after* the
  write completes under the lock.

---

## P1 — significant correctness

### DR18. GitHub owner/repo key truncated at first dash — ✅ DONE (fix pushed on branch)
`Source/Core/src/Tracker/IssueDraft.cpp:127`. `FromCachedTicket` derives `ProjectKey` by cutting
`ticket.id` at the first `-`, so `acme/react-native#12` → `acme/react`; `GitHubClient::BuildCreatePayload`
then targets the wrong repo (or 404s).
- **Fix:** parse GitHub ids by the `owner/repo#N` shape, not a first-dash split.

### DR19. DeepSeek provider enum drift → misrouted to OpenAI — ✅ DONE (fix pushed on branch)
`DeepSeek=4` is a fully-supported, user-selectable provider (`ClampProvider`, `ProviderFromConfig`,
prefs UI all handle case 4), but two consumers enumerate only 0–3 and fall through to OpenAI:
`Source/Core/include/Ui/SmatchetAiAssistantUi_detail.h` (`AiResolveProvider`) and
`Source/Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp:556` (`ai.validate-prefs`). The per-turn
model picker offers the OpenAI catalog and the validator reports the wrong provider.
- **Fix:** add `case 4 → AiProvider::DeepSeek` to both; consider a single shared `ResolveProvider`
  so future providers can't drift again.

### DR20. Mid-stream / 2xx provider errors swallowed as success — ✅ DONE (batch 2)
`AnthropicClient.cpp:66` and `OllamaClient.cpp:68` discard SSE `error` events / NDJSON `error` lines,
so a mid-stream failure on an HTTP-200 stream ends with a synthesized successful `eof` final delta —
truncated text committed and persisted as a completed reply. Linear/GitHub read paths route 2xx-non-200
and 200-with-errors responses through `TrackerErrorFromHttpStatus`, which returns `Ok()` for any 2xx
(`LinearClient.cpp:510`, `LinearIssueSearch.cpp:365`, `GitHubClient.cpp:415`) → blank misclassified errors.
- **Related:** BACKLOG **N12** (same theme — inconsistent classification via `IsTrackerTransportErrorText`;
  distinct mechanism). Consider fixing together.
- **Fix:** dispatch stream `error` events to a real failure result; add a 2xx-non-200 → Unknown guard at
  the tracker call sites (or centralise it).

### DR21. ImGui `NewFrame()` called before `DisplaySize`/`DeltaTime` set — ✅ DONE (batch 2)
`Source/Core/src/Ui/SmatchetImGuiHost.cpp:751`. First frame runs `NewFrame` with `DisplaySize=(-1,-1)`
→ `IM_ASSERT` abort in asserts-enabled builds (the ASan CI preset); later frames lay out with the
previous frame's size and delta.
- **Fix:** assign `io.DisplaySize`/`io.DeltaTime` before `ImGui::NewFrame()`.

### DR22. Popup ID-stack mismatch makes modals unreachable — ✅ DONE (batch 2)
`OpenPopup` is called inside a child window while `BeginPopupModal` runs at the parent level, so the
relative IDs never match: `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:410` (and `:723`) — the
assign / quick-comment modal never opens; `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:886` —
"Save view as new" never opens.
- **Fix:** issue `OpenPopup` and `BeginPopupModal` at the same ID-stack level (raise the open to the
  parent scope, or push a matching id), consistent with the working modals.

### DR23. Fixed-buffer edit round-trips truncate-on-commit — ✅ DONE (batch 2)
Editors round stored values through fixed buffers and commit the `min()`-truncated copy on the first
keystroke (no truncation banner, because it happens post-seed): `TicketFieldEditor.cpp:529,540`
(512 B inline), `TicketFieldEditor_Modal.cpp:257` (64 KB long-text), `Ui/SmatchetToolbarUi.cpp:632`
(LuaCode 2048 / ArgsJson 256 / CommandId 128 / Tooltip 128).
- **Fix:** size buffers to the value (or use `std::string`-backed `InputText` callbacks) and gate commit
  on a real dirty check against the untruncated original; surface a truncation banner where a hard cap stays.

### DR24. JQL→native query translation bugs — ✅ DONE (batch 2)
`Source/Core/src/Tracker/GitHubQueryFromJql.cpp`: tokenizer drops `@` so `@me` → literal `me` (`:163`);
`MaybeQuote` wraps whitespace values without escaping embedded `"` (`:234`); `GitHubClient.cpp:670`
`ExtractProjectFromQuery` mis-anchors the project on any query containing `/` (e.g. a `ui/ux` label).
`Source/Core/src/Tracker/LinearQueryFromJql.cpp:105`: `SplitTopLevelAnd`/`DetectOperator` scan without
quote-awareness, so connector/operator substrings inside quoted values corrupt the parse and the text
search is silently dropped.
- **Fix:** make the tokenizers quote-aware and preserve `@`; escape embedded quotes; anchor the project
  extraction on structure, not a bare `/` scan.

### DR25. Plane single-page pagination drops data — ✅ DONE (batch 2)
`Source/Core/src/Tracker/PlaneClient.cpp:90` `ResolvePlaneProject` and `PlaneIssueSearch.cpp:637`
`ListProjects` fetch only the first page of `/projects/` (no cursor loop) → a project past the first 100
can never be resolved and is cached incomplete for 5 min. `PlaneIssueMutation.cpp:548` `FetchIssueComments`
parses only the first comments page (never follows `next_cursor`).
- **Fix:** loop pages via the shared `AppendPagedResults`/cursor pattern used elsewhere.

### DR26. Ollama system prompt silently ignored — ✅ DONE (fix pushed on branch)
`Source/Core/src/OllamaClient.cpp:42`. `BuildChatBody` sends the system prompt as a top-level `"system"`
field, which `/api/chat` ignores (it takes system text only as a `{role:"system"}` message) → agents.md
instructions and context blocks are dropped with no error.
- **Fix:** prepend a `{role:"system"}` message instead of the top-level field.

### DR27. Commands report success but don't affect the running instance — ✅ DONE (batch 2)
`BuiltinCommands_Perf.cpp:205` `perf.toggle_panel` only writes config (UI reads `g_ui.showPerformance`
once at startup); the palette filter latch (`BuiltinCommands_Ui.cpp:101` + `CommandPaletteUi.cpp:239`)
sticks `view.toggle.` for the session and the reopened palette dispatches a stale list;
`BuiltinCommands_Users.cpp:33` `users.search` ignores the backend error and returns `ok:true` empty;
`BuiltinCommands_Debug.cpp:376` `debug.crash --kind=throw` is caught by the dispatcher so the
crash-reporter path is never exercised.
- **Fix:** apply `perf.toggle_panel` to `g_ui` directly; clear the palette filter on consume and
  rebuild `filtered_` on open; surface `users.search`'s error; route `kind=throw` outside the dispatcher
  catch (or document it as untestable).

### DR28. Per-frame icon fetch/parse retry storm — ✅ DONE (batch 2)
`Source/Core/src/Ui/SmatchetFieldIconRender.cpp:402,754`. Failed icon fetches never record a negative
result and the deferred path skips `RememberNegativePriorityResolution`, so an unresolvable icon
re-fetches (3 s cpr each) and re-parses JSON + fs::exists every frame for the whole session.
- **Fix:** memoise negative results (with the `Negative` flag honoured in the lookup) and back off failed fetches.

### DR29. Test suite has vacuous / self-referential regression guards + ODR violation — ✅ RESOLVED (2026-08-18, PR #2121)
Several "regression" tests assert against a local re-implementation of the production logic, so the real
code can regress green: `tests/Core/UserInfoActivityCancelUaf.test.cpp:140,197`, `tests/Lua/LuaTimeout.test.cpp:28`,
`tests/Core/BulkImportAbandonNonBlocking.test.cpp:59`, `tests/Core/MarkdownLanguageDefinition.test.cpp:85`,
`tests/ui/ai_prefs_autosave_flow.test.cpp:308` (clears the flag it then asserts), plus tautologies
(`AgentsMdLoader.test.cpp:336`, `jira_deterministic_backend.test.cpp:168`, `sync_stall_visible_cue.test.cpp:105`)
and a TSan-flaky sleep in `AiAssistantStreamHandoff.test.cpp:85`. **ODR:** two different `TestEnvGuard`
class definitions (`tests/support/OfflineQueueTestEnv.h:32` vs `tests/support/TestEnvGuard.h:46`) are linked
into one binary — link-order-dependent destructor (one skips audit cleanup).
- **Fix:** point these tests at the production symbol; rename one `TestEnvGuard` (or merge the two).
- **Related:** `TEST_COVERAGE_GAP_MAP.md` hygiene notes track adjacent gaps; add these there too.
- **Triage 2026-08-16 (verified against develop tip `7da969b`).** Closed since batch 3:
  - **ODR — fixed.** Only one `TestEnvGuard` class definition remains (`tests/support/TestEnvGuard.h:46`); the offline-queue one is renamed `OfflineQueueTestEnvGuard` (`tests/support/OfflineQueueTestEnv.h:32`). No link-order-dependent destructor.
  - **`ai_prefs_autosave_flow.test.cpp` — fixed** (comment at `:324` records the swap to a production-observable assertion).
  - **`MarkdownLanguageDefinition.test.cpp` — fixed**; the cases now drive the production `LD::Markdown()` token regexes.
  - **`AgentsMdLoader.test.cpp` — fixed**; the tautology is replaced with a determinism pin against the production `AgentsMdLoader::LoadLayered` symbol.
  - **Still self-referential (the documented residual class):** `BulkImportAbandonNonBlocking.test.cpp:48-68` (`FakeBulkSession` + `FakeBulkImportAbandonFutures`, explicitly "byte-for-byte the production shape"), `UserInfoActivityCancelUaf.test.cpp` (`FakeController`/`FakeUserInfoOwner`), and `LuaTimeout.test.cpp` via `tests/support/LuaHostFixture.h:82` (mirrors `LuaHookGuard`, which lives in the src-private `AppController_LuaBindings_detail.h:74`).
  - **Verdict (superseded 2026-08-18 → ✅, see the closing block below):** DR29 stays 🟡 PARTIAL, but the remainder is now a single, well-defined refactor rather than a test-hygiene sweep — **each residual needs its production helper hoisted to a test-linkable header** (`LuaHookGuard`, `BulkImportAbandonFutures`, the user-activity cancel path). Sequence it as one "hoist production helpers for test linkage" change; do not attempt per-test patches, which is what left the residual last time.

- **Closed 2026-08-18 (PR #2121), verified against the merged tree.** The 2026-08-16 verdict called for one
  "hoist production helpers for test linkage" change rather than per-test patches; that is what shipped.

  | production helper | was | now test-linkable at |
  |---|---|---|
  | `BulkImportAbandonFutures` | anon namespace in `Ui/SmatchetBulkTicketsUi.cpp` | `Ui/BulkImportAbandon.h` (`smatchet::ui::`) |
  | the #1150 cancel-before-join handshake | inline in `~SmatchetUserInfoUi` | `Ui/ShutdownCancelGate.h` |
  | `LuaHookGuard` | src-private `AppController_LuaBindings_detail.h` | `Lua/LuaHookGuard.h` (detail header aliases it) |

  Each of the three tests now calls the production symbol: `BulkImportAbandonNonBlocking.test.cpp:101`
  (`FakeBulkImportAbandonFutures` deleted), `UserInfoActivityCancelUaf.test.cpp` holds a real
  `ShutdownCancelGate` as its last member with no destructor of its own — so production's
  `~ShutdownCancelGate` is what unblocks the worker, making the ordering guarantee structural rather
  than a comment — and `tests/support/LuaHostFixture.h:79` constructs the real `LuaHookGuard` instead
  of hand-rolling `lua_sethook`.

  **The guards are no longer vacuous, and that was checked by mutation, not by inspection.** Deleting
  `d.bulkImportCancel.Cancel()` from `BulkImportAbandon.h` and `ioCancel_()` from
  `ShutdownCancelGate::Signal()` turns the suite red (`teardownMs 10000 < 2000`) where the old
  self-referential copies would have stayed green through the same deletion. That is the property
  DR29 existed to obtain.

### DR30. Credentials leaked into logs via unredacted error bodies — ✅ DONE (fix pushed on branch)
Raw HTTP error bodies are spliced into user-facing/log strings, bypassing `RedactHttpBodyForLog`:
`Source/Core/src/Tracker/JiraUserAndMeta.cpp:206` (first 200 B of the watchers response),
`Source/Core/src/Tracker/PlaneIssueSearch.cpp:247` and `PlaneClient.cpp:95` (`response.text.substr(0,300)`
at ERROR level). A 401/403 body reflecting the request's `Authorization`/`x-api-key` lands verbatim in
the persistent log.
- **Fix:** route these through `RedactHttpBodyForLog`/`ExtractJiraErrorMessage` like the sibling paths.

---

## P2 — polish / consistency

### DR31. `SmatchetResult` / `Optional` exception-safety holes — ✅ DONE (batch 2)
`Source/Core/include/SmatchetResult.h:169`: `Result` move-assignment `Destroy()`s the stored object and
sets `ok_` before a placement-new that can throw → on throw, `~Result` double-destroys unconstructed
storage. `:41`: `Optional::operator=` is `noexcept` but its swap placement-new-constructs `T`, so a
throwing move escapes `noexcept` → `std::terminate`. Only bites payload types whose move can throw.
- **Fix:** construct-then-swap (or guard `ok_` so the destructor skips unconstructed storage); drop the
  `noexcept` or constrain to nothrow-movable `T`.

### DR32. Invalid calendar dates submitted to trackers — ✅ DONE (fix pushed on branch)
`Source/Core/src/Tracker/TrackerDateTimePure.cpp:119` `ParseFriendlyDate` accepts any day 1–31 for any
month and the date-picker commit (`TrackerDateTimeFieldEditor.cpp:627`) formats without `ClampDayToMonth`
→ `2026-02-31` is submitted (400 or undefined server normalisation).
- **Fix:** validate day-in-month (or clamp) before formatting for the API.

### DR33. Markdown / text rendering edge cases — ✅ DONE (fix pushed on branch)
`Source/Core/src/Ui/MarkdownConvert.cpp:930` `MatchStoredTaskPrefix` uses `.at("text")` which throws on
an ADF list-item whose first text node lacks a string `text` (uncaught in the offline-queue merge path,
`MineValueToMarkdown`). `Ui/SelectableTextRun.cpp:44` selects on raw byte offsets → Ctrl+C can copy a
partial UTF-8 sequence. `Ui/MarkdownPreviewRender.cpp:826` caches per-word pixel widths with no font/scale
invalidation → stale wrapping after a font-size change. `Ui/CodeColorView.cpp:605` silently drops text past
256 KiB with no indicator.
- **Fix:** guard the `at("text")` read (type-check like the neighbouring `is_required`); snap selection to
  codepoint boundaries; epoch-check the word-width cache on font/scale change; show a truncation indicator.

---

## Sequencing (current — 2026-08-16)

**32 of 33 findings are shipped.** The sequencing plan below is fully executed and kept for provenance.

The single remaining item is **DR29**, and it is one change, not a list: hoist the three production
helpers its residual tests re-implement (`LuaHookGuard`, `BulkImportAbandonFutures`, the user-activity
cancel path) into test-linkable headers, then point `LuaTimeout.test.cpp`,
`BulkImportAbandonNonBlocking.test.cpp`, and `UserInfoActivityCancelUaf.test.cpp` at the real symbols.
Do it as one change — the per-test approach is what left this residual behind last time.

<details>
<summary>Original suggested sequencing (2026-07-07 — all but DR29 shipped)</summary>

**Now (safety, small, high-leverage):** DR1 (merge drop), DR2 (endpoint bypass), DR3 (credential wipe),
DR14 (CI gates — they gate everything else), DR19 (DeepSeek drift, ~2-line each).

**Next (crash/data-loss, medium PRs):** DR4, DR5, DR6, DR7, DR8, DR15, DR16, DR17, DR13.

**Then (correctness):** DR18, DR20, DR21, DR22, DR23, DR24, DR25, DR26, DR29.

**Standing / fold-in when adjacent:** DR9–DR12, DR27, DR28, DR30, DR31, DR32, DR33.

</details>

---

_End of 2026-07-07 deep-review findings._
