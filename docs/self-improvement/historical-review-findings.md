# Historical code-review findings

> Findings from sweeping **already-merged** PRs with the
> `historical-code-review` skill (`agents/_shared/skills/historical-code-review/`)
> + `agents/scripts/core/historical-review-survivors.sh`. Each PR is
> reviewed for **only the lines it introduced that are still alive and untouched
> at `origin/develop`** — code a newer PR already changed/fixed is excluded by
> construction, so nothing here is already-resolved.
>
> Reviewer model: `code-review` (opus/high). Findings are advisory backlog, not
> auto-fixed. User-visible product defects should be elevated to GitHub Issues
> (ADR-0014); the rest is tech-debt. Newest batch on top.

## Batch 21 — #2032–#1941 (86-PR sweep, 2026-08-16)

Coverage: **86 reviewed — 65 with findings, 14 clean, 7 fully superseded, 0 errored, 0 died.** Net: **1 CRITICAL, 6 HIGH, 67 MEDIUM, 105 LOW** (179 findings, 12 `userVisible`). **The frontier is now #1-#2032 contiguous; the next sweep resumes from #2033.** Survivor-filtered against `origin/develop` @ `3dc6695f` with `--against origin/develop` explicit. Reviewer model `code-review` (opus/high), 86/86 returned; ~5.21M tokens, ~84 min.

Work-list cross-validated against GitHub's merged list before running: 86 merged PRs in (#1940, #2032], and the squash-subject scrape agreed on all 86 (no merge commits in this range), so no constituent expansion was needed here.

**Supersede rate is near zero** — 7 of 86 fully superseded, and most survivors are days old. This range is 8 days of merges, so the blame-survivor filter had almost nothing to subtract and the pass degrades toward an ordinary re-review. That is the same effect the 2026-08-08 targeted pass recorded, and it is the standing argument for choosing the next slice by **age, not count**. Read the 179 findings with that in mind: they are real, but they are not the "survived N later PRs" signal an older slice gives.

#1941, #1952, #1954 and #1962 were re-swept deliberately even though the 2026-08-08 targeted pass covered part of them — that pass read only ~3,000 of their `Source/` survivor lines and its two fixes shipped in #1989, so those specific lines are re-attributed and drop out by construction, while the rest of each PR had never been swept. Excluding them would have reproduced exactly the coverage hole Batch 20 left.

**12 `userVisible` findings are Issue candidates under ADR-0014 and are NOT yet filed.**

### Actionable set — CRITICAL / HIGH / every `userVisible` finding (18)

Full problem + fix. Everything below this block is one-line; the complete structured set, fixes included, is in [`historical-review-findings-2026-08-16.jsonl`](historical-review-findings-2026-08-16.jsonl).


**CRITICAL (1)**
- **#1941 (c7fb2236) · `Source/Core/src/Ui/SmatchetPreferencesUi_General.cpp:247`** · **userVisible** — DrawGeneralStorageSection re-resolves the storage preference every frame: ConfigManager::GetStoragePreference(runtimeAssetDir, kDefaultPref) does a FileExists() stat plus an ifstream open + line-by-line parse of smatchet_storage_mode.txt (ConfigManager_PathUtils.cpp:623-657), and GetUserDataDirectory()/GetStoragePreferenceFlagPath() run again at :300-301. This is synchronous disk I/O on the ImGui render path for every frame the General > Storage section is expanded — AGENTS.md Quality Pillar 2 states 'sync I/O on UI thread = code-review CRITICAL'. The section's own help text advertises the runtime-asset dir may be a network share or source-controlled tree, exactly where the per-frame stat+read stalls the UI thread. **Fix:** Resolve currentPref (and the two displayed paths) once on the section/page open-edge and cache it in UiDrawSession — the same pattern drawPreferencesTrackerTab already uses via RefreshCachedProjectsSnapshotOnOpen — and refresh the cached value only after a successful ConfigManager::SetStoragePreference.

**HIGH (6)**
- **#1952 (2df7bb4e) · `docs/harness/claude-code/hooks/lint-syntax-both.py:199`** — The new bare-filename banner filter plus the widened system-header allow-list turn the dual-target syntax gate into a fail-open: when a compile aborts at the first system header (the documented no-VS-Developer-shell case), every produced line is FP-filtered or is the banner, `real_lines` is empty, no failure is appended, and the hook exits 0 — reporting PASS for a run that never parsed a single first-party token. The PR's own self-improvement entry records that an injected `undeclared_symbol_probe()` returns rc=0. **Fix:** Distinguish "no first-party diagnostics" from "compilation never reached first-party code": if a TU's returncode!=0 and 100% of its lines matched FP patterns/banner, record an inconclusive result and emit a distinct non-zero-or-WARN outcome naming the missing `ninja-iter-clang` preset, instead of silently dropping the target.
- **#1957 (1ec9fb0c) · `scripts/publish/test-installer-smoke.sh:168`** — Bare `"$INSTALLER" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART` (and the identical uninstall call at line 115) invoke a native Windows exe with `/switch` args from Git Bash without `MSYS_NO_PATHCONV=1`. MSYS rewrites `/VERYSILENT` into `C:/Program Files/Git/VERYSILENT`, so Inno drops the silent switches and shows the interactive wizard — the smoke test hangs (or dies with "Installer exited non-zero") on any headless/CI run. This is the exact bug class the same PR introduced `native_exec()` for in release-github.sh, and the later `tests/bats/msys_argv_switches.bats` gate only inspects `$ISCC_EXE`/`$SIGN_TOOL`/`cmd.exe` sites, so these two survive unguarded. Note line 199 already uses `taskkill //F //PID`, showing the escaping was known. **Fix:** Prefix both invocations with `MSYS_NO_PATHCONV=1` (or a local `native_exec` helper), and extend msys_argv_switches.bats to cover `$INSTALLER`/`$uninstaller`/`signtool` call sites in tracked *.sh.
- **#1958 (cf622ad6) · `scripts/dev/worktree.sh:218`** — cmd_resync rewrites EVERY registry entry in the tree to the current branch/sha, with no self-filter. Running `worktree.sh resync` in the shared integration tree silently re-baselines other LIVE sessions' entries, so guard-head-drift.sh stops denying for them — the drift that already happened under those siblings is erased. git-janitor.sh solves the same problem by self-excluding via CLAUDE_SESSION_ID (documented in docs/agent-rules/process-rules.md line 87); resync has no equivalent. **Fix:** Re-baseline only the caller's own entry by default (match CLAUDE_SESSION_ID / SMATCHET_JANITOR_SELF_SESSION against the entry basename), and require an explicit flag (e.g. --all) to touch sibling entries.
- **#1996 (79891e09) · `agents/scripts/core/lib/script-freshness.sh:98`** — script_freshness_fingerprint_one sets SCRIPT_FRESHNESS_INCOMPLETE when EITHER side is missing, so a glob-matched file that exists LOCALLY but not on origin/develop (i.e. a newly added file on the working branch) blanks BOTH fingerprints and forces `unverifiable` — which warn_if_script_stale prints NOTHING for. Reproduced: in a repo declaring `entry.sh` + `rules.d/*.sh`, adding an untracked `rules.d/99-local-only.sh` AND drifting `entry.sh` away from origin/develop yields verdict=unverifiable and zero output, instead of the `stale` WARN. pre-ship.sh declares `agents/scripts/project/lint-rules.d/*.sh`, so any branch that adds one lint rule silently disables the whole freshness caveat — the false-green class this detector exists to prevent, one direction over from the develop-only case the same commit deliberately fixed to report `stale`. **Fix:** Treat a local-only glob match as 'ahead', not 'unknown': in the glob branch, when the develop blob is missing for a locally-matched file, drop that file from BOTH fingerprints (set _matched=true, do not set SCRIPT_FRESHNESS_INCOMPLETE) so the remaining declared files still decide fresh/stale. Keep the INCOMPLETE blanking only for the literal-relpath branch and for a locally-MISSING declared file. Add a bats case mirroring `glob: a file develop has and the checkout LACKS reads stale` for the reverse direction.
- **#2017 (c85b3521) · `agents/scripts/core/merge-gates.d/10-gate-filter.sh:140`** — $pureDocs reuses the is-pure-docs-diff.sh allow-list, which includes `agents/scripts/` — shell/gate code, not docs. It is then used as the ONLY guard on the comment-surface terminal-pass arm of $crreviewskipped (line 181), whose stated purpose is to stop a STALE CodeRabbit skip comment (comments persist across pushes) from fast-passing a fresh CODE push. A PR touching only `agents/scripts/**` (including merge-gates.sh / merge-gates.d/*.sh themselves) is classified pureDocs, so a leftover `## Review skipped` comment terminal-PASSes the CR bucket for an unreviewed gate-script push. Fail-open on the highest-leverage code class in the repo. **Fix:** Do not reuse the build-need allow-list as a review-safety allow-list. Bind a separate predicate for this arm, e.g. `all(test("^(docs/|backlog/|.*[.]md$)"))` (drop `agents/scripts/`), or additionally require the skip comment to be newer than the head commit's pushedDate.
- **#2020 (acccaddb) · `agents/core/code-review.md:37`** — The no-arg review scope is documented backwards. `git diff origin/develop...` is IDENTICAL to `git diff origin/develop...HEAD` (git substitutes HEAD for the omitted right side), so it is merge-base -> HEAD, committed-only. Verified in-repo: with README.md dirty, both forms omit it; only `git diff $(git merge-base origin/develop HEAD)` shows it. The doc asserts the opposite ("merge-base -> working tree, so unstaged hunks are in scope") and calls the HEAD form the one that "silently drops them". A reviewer following this contract reviews a diff that drops every unstaged hunk while being told it cannot — the exact fail-open the sibling `MM` rule at lines 43-48 was added to close. It also breaks the `--intent-to-add` claim on lines 39-40: intent-to-add files appear in worktree diffs, not in a commit-to-commit `A...B` diff. **Fix:** Replace the scope command with the two-dot/working-tree form: `git diff "$(git merge-base origin/develop HEAD)"` (merge-base -> working tree). Drop the false contrast against `origin/develop...HEAD` or restate it as `A...B` == committed-only in both spellings.

**MEDIUM (10)**
- **#1952 (2df7bb4e) · `Source/Core/src/Commands/Scenarios/UserInfoScreenshotScenarios.cpp:168`** · **userVisible** — Deferring `restoreState()` via QueuePostCaptureRestore makes the unwind conditional on another frame being drawn, but restoreState restores PERSISTED config fields (cfg.GitHubPat, GitCommitRepos, GitHubOwner, GitHubRepo, UiMode, VcsFeedLayout, WhisperSetupCompleted), not just session state. ScenarioCaptureQuiesce.h:57-61 asserts the opposite ("every field they restore is session-scoped, which is harmless") and explicitly blesses a process exiting before the drain. An in-process `scenario.run` whose app exits (or saves config) before the next frame persists the cleared GitHub PAT/repos and WhisperSetupCompleted=true to the user's config. **Fix:** Either keep the cfg-field unwind inline in OnFinish (only the g_ui view/identity pins need deferring), or drain the pending queue on shutdown as well; and correct the ScenarioCaptureQuiesce.h contract comment, which currently claims the deferred callbacks touch only session-scoped state.
- **#1962 (a7fa2a32) · `Source/Core/src/Ui/SmatchetUI.cpp:1111`** · **userVisible** — selectDockedTab() is called BEFORE the window's Begin(), and the focus request is consumed on that same frame (g_ui.requestUserInfoFocus cleared at :1117; the Preferences twin at SmatchetPreferencesUi.cpp:194 clears d.requestPreferencesFocus at :202/:211). ImGui only creates the ImGuiWindow inside Begin(), so on the first-ever open of the window in a process FindWindowByName() returns nullptr, selectDockedTab early-returns as a no-op, and the request is gone by the next frame — the exact 'comes up behind its sibling and stays there' failure the PR fixes can still occur on the first open when the sibling tab (Preferences / User Info) was created first. **Fix:** Keep the focus request armed until selectDockedTab actually resolves a window: either clear requestUserInfoFocus/requestPreferencesFocus only when FindWindowByName() succeeded (have selectDockedTab return bool), or re-arm for one extra frame when the window did not yet exist.
- **#1966 (1f42859f) · `Source/Core/src/Ui/SmatchetWindowExpand.cpp:258`** · **userVisible** — The per-frame pin (SetNextWindowDockID(0)/SetNextWindowPos/Size on WorkPos/WorkSize) IS the window's live state, and ImGui's settings writer snapshots Pos/Size/DockId off the live window (omitting DockId= while 0). Quitting the app while a window is expanded therefore persists it to imgui.ini as floating + fullscreen; on relaunch SmatchetUI_Layout's pendingReDockWindows force-redocks it to its DEFAULT slot, silently discarding the user's customised dock placement for that window. No test bucket can observe it (documented in docs/self-improvement/categories/test/2026-08-05-no-bucket-e-shutdown-relaunch-primitive.md). **Fix:** Make the transition ini-safe: either write the pre-expand WindowExpandSaved placement into the window's ImGuiWindowSettings entry when the override is applied, or drop the expansion (replay the saved placement) during shutdown before ImGui::DestroyContext saves, so the persisted state is the home slot.
- **#1984 (e8713063) · `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1496`** · **userVisible** — ApplyAssistantDocking rewrites and persists the user's AssistantPanelOnSecondarySide preference whenever the requested side bar is not live. The block runs every frame and is not gated on assistantPendingSideSwap/needsReDock, so it fires even when no dock write happens (mouse held, or FirstUseEver already satisfied). Since kSecondarySideBar is cut by no DockBuilder call and is absent from the embedded default ini (per the PR's own debt note), an existing config with AssistantPanelOnSecondarySide=true is silently flipped to false on the first frame and ScheduleConfigSaveDetached persists it. The setting is lost durably, and the swap button is BeginDisabled in exactly that state, so the user cannot set it back. **Fix:** Record the fallback side only when a dock write actually lands (inside the targetDockId != 0 branch that consumes the pending flags) and keep it as a session-only override on UiDrawSession instead of persisting, so the stored preference returns once a real secondary node exists. Alternatively cut a real kSecondarySideBar node in the default layout, or delete the constant and the swap feature.
- **#1993 (bcd1e90d) · `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:1196`** · **userVisible** — The keybinding-row match scan was reduced from every-frame to an edge-triggered cache whose invalidation set is incomplete. `prefsKeybindRowsMatchDirty` is set only in the Preferences keybindings editor (SmatchetPreferencesUi_Keybindings.cpp:430), but `d.cfg.Keybindings` is also mutated outside that editor — `quickBind_.Draw(d.cfg)` in SmatchetUI.cpp:1054 (toolbar / command-palette quick-bind) calls only `MarkKeybindingsDirty()`. With Preferences open under an active query, a quick-bind that adds/changes a matching hotkey leaves `prefsKeybindRowsMatchQuery` stale until the user edits the query or the language changes: the Shortcuts section stays hidden (or stays visible with zero matching rows) and the "showing N of M" readout disagrees with the rows. **Fix:** Invalidate at the single source of binding mutation rather than at each UI call site: have `MarkKeybindingsDirty()` (or the quick-bind path in SmatchetUI.cpp:1054) also set `d.prefsKeybindRowsMatchDirty = true`, or key the cache off a monotonically-bumped keybindings revision counter compared each frame in drawPreferencesWindow.
- **#1994 (d665b65e) · `Source/Core/include/Tracker/NewIssueInheritFields.h:25`** · **userVisible** — NewIssueInheritFieldIdsFor() has no kBackendGitHub branch, so a GitHub config falls through to cfg.NewIssueInheritFieldIds (the Jira list). TrackerConfig::NewIssueInheritFieldIdsGitHub is loaded (ConfigManager_Load.cpp:498), saved (ConfigManager_Save.cpp:100) and edited in Preferences (SmatchetPreferencesUi.cpp:243/981), but no draft path reads it — the user's "New issue: inherit fields from last row (GitHub)" setting is accepted, persisted and silently ignored. The PR documents this as a deliberately preserved known gap and pins it in tests, but it remains a live user-visible defect. **Fix:** Add `if (kind == kBackendGitHub) { return cfg.NewIssueInheritFieldIdsGitHub; }` and flip the pinning expectation in tests/Core/NewIssueInheritFields.test.cpp:72-83; or, if the behaviour change needs its own decision, disable/label the GitHub inherit-fields row in Preferences so the UI stops offering a setting nothing consumes.
- **#2013 (8579fd47) · `Source/Core/src/Ui/SmatchetToolbarUi.cpp:279`** · **userVisible** — The toolbar tooltip switched from BoundHotkeyDisplay(bindings, commandId) to BoundHotkeyDisplayAll(bindings, commandId, "{}"). The old call used the command-id-only rule (prefer the "{}" row, otherwise fall back to ANY row for that command); the new call uses semantic args matching against a hardcoded "{}" with no fallback. ToolbarButton carries its own ArgsJson (ToolbarConfig.h:31) and it is ignored here, so a customized toolbar button whose command is bound with non-default args (e.g. view.toggle.performance {"action":"show"}, view.sidebar.primary {"action":"toggle"}) now shows no shortcut at all where it previously showed one. **Fix:** Pass the button's own args: BoundHotkeyDisplayAll(cfg.Keybindings.Bindings, b.CommandId, b.ArgsJson) — matching what DispatchButton actually dispatches.
- **#2024 (964a05d8) · `Source/Core/src/AppController_PaneContexts.cpp:848`** · **userVisible** — ForgetPaneOwnedTicketIds() keys the erase with ctx.CacheBackendKeyCopy() read AT RETIREMENT TIME, but the entry was stored under whatever backend key was live when PublishOwnedTicketIds ran. SwapBackendIfTrackerChanged (TicketSyncService.cpp:665 -> SetCacheBackendKey) re-stamps a pane's key at runtime, so a pane that switched tracker kind leaves an orphan paneOwnedTicketIds_ entry under the OLD key. Nothing else erases it, and CollectTicketIdsRetainedByOtherContexts prefix-scans that namespace with no liveness check, so those ids pin cache rows against every sibling pane's stale sweep permanently (tickets_v2 rows never reclaimed; ghost rows keep resurfacing in siblings' namespace-wide reads). **Fix:** Erase by pane id across all backend keys at retirement (scan paneOwnedTicketIds_ for the '\n'+paneId suffix), or re-key/drop the pane's entry inside GridLiveContext::SetCacheBackendKey when the key changes.
- **#2024 (964a05d8) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:100`** · **userVisible** — Non-atomic read-modify-write of the pane owned-id set: `owned` is snapshotted at line 64, then a full SQLite `Cache->GetAllTickets` read and a mutex acquisition happen before SetPaneOwnedTicketIds writes `owned + admitId` back at line 100. Any PublishOwnedTicketIds that lands in that window (sync-session finalize) is silently clobbered and the pane's recorded set reverts to the pre-sync ids — the next RefreshLocalData then filters the freshly-synced rows out of the grid and stops pinning them against a sibling's stale deletion. AppController.h:1206 explicitly documents this path as worker-invoked, so the write is not UI-thread-serialized by contract. **Fix:** Make the admit an atomic append under paneOwnedIdsMutex_ (e.g. an AddPaneOwnedTicketId(key, paneId, id) that re-reads the map entry inside the lock) instead of writing back a stale copied vector.
- **#2024 (964a05d8) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:65`** · **userVisible** — The pane-scoping filter is skipped whenever the recorded set is empty, falling back to the whole shared namespace. That is not only the true cold-start case: RetireHiddenPaneContexts calls ForgetPaneOwnedTicketIds after the 30 s hidden grace, so a pane the user returns to gets a fresh context with an EMPTY recorded set and the bootstrap refresh repopulates its grid with every sibling pane's tickets until its own sync finishes — exactly the cross-pane row leak F2/F3 was meant to close. **Fix:** Distinguish 'never synced' from 'set forgotten' (e.g. keep a per-pane hasSyncedOnce flag or a tombstone entry) and render empty rather than the sibling union for a revived pane.

**LOW (1)**
- **#1950 (1401376b) · `Source/Core/src/Ui/SmatchetAboutUi.cpp:68`** · **userVisible** — The empty-value placeholder literal "unknown" is emitted straight into the InputText buffer, bypassing TranslateSource — every other chrome string in this TU is localized (and pinned by AboutLocalization.test.cpp), so a French user sees an untranslated "unknown" for any missing fact. **Fix:** Use SmatchetLocalization::TranslateSource("unknown") (and add the row to the localization table) for the placeholder.

### Internal debt — 57 MEDIUM + 104 LOW, indexed by file

All `userVisible:false`. Listed per file rather than per finding — the full problem + fix for every row below is in [`historical-review-findings-2026-08-16.jsonl`](historical-review-findings-2026-08-16.jsonl), keyed by `pr` + `file` + `line`.

- `docs/self-improvement/categories/applied.md` — 3 MEDIUM, 11 LOW — #1947:2279, #1998:3637, #2006:4363, #2010:4747, #2012:5001, #2016:5057, #2016:5115, #2017:5165, #2017:5179, #2018:5196, #2018:5225, #2020:5259, #2025:5496, #2025:5498
- `.github/actions/cr-finding-gate/action.yml` — 2 MEDIUM, 4 LOW — #2004:131, #2004:150, #2004:152, #2004:469, #2004:470, #1977:518
- `docs/self-improvement/postmortems.md` — 6 LOW — #1985:68, #1961:139, #2010:173, #1961:176, #1944:259, #1944:270
- `agents/scripts/core/lib/review-ack.sh` — 2 MEDIUM, 2 LOW — #1970:50, #1964:126, #1964:129, #1964:169
- `docs/self-improvement/categories/process/2026-08-16-watcher-autoregister-bypasses-merge-consent.md` — 1 MEDIUM, 3 LOW — #2031:18, #2031:25, #2031:60, #2031:71
- `scripts/dev/setup-env.sh` — 2 MEDIUM, 2 LOW — #1946:49, #1946:156, #1946:204, #1946:326
- `scripts/dev/worktree.sh` — 3 MEDIUM, 1 LOW — #1958:172, #1958:219, #1958:266, #1958:297
- `tests/bats/msys_argv_switches.bats` — 3 MEDIUM, 1 LOW — #1963:39, #1963:90, #1963:97, #1963:117
- `agents/scripts/core/test-markdown-links.sh` — 1 MEDIUM, 2 LOW — #2028:373, #2028:419, #2028:455
- `agents/scripts/core/test-plan-claim-anchors.sh` — 1 MEDIUM, 2 LOW — #1999:459, #1999:704, #1999:710
- `docs/agent-rules/review-panels.md` — 1 MEDIUM, 2 LOW — #1980:8, #1978:27, #1978:104
- `docs/plans/autonomous-debug-live-evidence.md` — 2 MEDIUM, 1 LOW — #2032:50, #2032:86, #2032:104
- `scripts/common/unreal-batch.sh` — 2 MEDIUM, 1 LOW — #1965:41, #1965:45, #1965:49
- `scripts/dev/pre-ship.sh` — 2 MEDIUM, 1 LOW — #2027:300, #1974:352, #1974:626
- `scripts/dev/test-screenshot-diff.sh` — 1 MEDIUM, 2 LOW — #2023:159, #2023:232, #2023:384
- `.github/workflows/cr-finding-gate.yml` — 1 MEDIUM, 1 LOW — #1977:144, #1977:154
- `Source/Core/src/Ui/ImGuiHotkey.cpp` — 2 LOW — #2013:154, #2013:238
- `agents/scripts/core/postmortem-owed.sh` — 1 MEDIUM, 1 LOW — #1998:726, #2018:982
- `agents/scripts/core/work_item_lint.py` — 1 MEDIUM, 1 LOW — #1979:64, #1979:283
- `docs/plans/agentic-infra-audit-campaign-2026-06.md` — 2 LOW — #1956:267, #1956:276
- `docs/plans/preferences-ia-resegmentation-and-search.md` — 2 LOW — #1941:212, #1941:338
- `docs/self-improvement/categories/debt/2026-08-07-dock-node-id-slot-liveness-followups.md` — 2 LOW — #1984:8, #2020:58
- `docs/self-improvement/categories/tooling.md` — 1 MEDIUM, 1 LOW — #1972:11, #1972:11
- `docs/self-improvement/historical-review-findings.md` — 1 MEDIUM, 1 LOW — #1991:16, #1991:54
- `docs/work/closed/01-spawn-honors-timeout.md` — 2 LOW — #1982:8, #1982:14
- `scripts/dev/local/package-unreal-plugin-msvc.sh` — 1 MEDIUM, 1 LOW — #1959:28, #1959:118
- `scripts/dev/local/test-build-wrapper.sh` — 2 MEDIUM — #1955:101, #1955:353
- `scripts/dev/new-session.sh` — 2 LOW — #1958:32, #1958:57
- `tests/bats/bucket_lane_launch_smoke.bats` — 2 MEDIUM — #2023:200, #2023:255
- `tests/bats/cr_finding_gate.bats` — 2 MEDIUM — #2014:141, #1977:160
- `tests/bats/font_asset_resolve.bats` — 1 MEDIUM, 1 LOW — #1948:48, #1948:104
- `.github/workflows/build-and-test.yml` — 1 LOW — #2023:1146
- `CMakeLists.txt` — 1 LOW — #1954:565
- `Source/Core/include/Commands/CommandPaletteUi.h` — 1 LOW — #1952:40
- `Source/Core/include/Config/KeybindingsConfig.h` — 1 LOW — #2013:23
- `Source/Core/include/Persistence/SmatchetIcoDecode.h` — 1 LOW — #1954:27
- `Source/Core/include/Tracker/TrackerBackendKind.h` — 1 MEDIUM — #1989:15
- `Source/Core/include/Ui/SmatchetWindowExpand.h` — 1 MEDIUM — #1966:42
- `Source/Core/src/Diagnostics/BugReportService.cpp` — 1 LOW — #2005:86
- `Source/Core/src/SmatchetTicketChangeNotifications.cpp` — 1 LOW — #2024:72
- `Source/Core/src/Tracker/GitHubFixtureBackend.cpp` — 1 LOW — #2005:37
- `Source/Core/src/Tracker/LinearFixtureBackend.cpp` — 1 LOW — #2005:52
- `Source/Core/src/Ui/PreferencesFilter.cpp` — 1 LOW — #1989:59
- `Source/Core/src/Ui/SmatchetAboutUi.cpp` — 1 LOW — #1950:70
- `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` — 1 LOW — #1992:220
- `Source/Core/src/Ui/SmatchetHotkeyCapture.cpp` — 1 LOW — #2013:168
- `Source/Standalone/CliDispatch.cpp` — 1 LOW — #2022:255
- `Source/Standalone/CliExitCodes.cpp` — 1 LOW — #2022:43
- `Source/Standalone/CliSpawn.cpp` — 1 MEDIUM — #2022:300
- `agents/_shared/skills/address-review-feedback/SKILL.md` — 1 MEDIUM — #1987:86
- `agents/core/code-review.md` — 1 LOW — #2027:87
- `agents/scripts/core/archive-backlog-entry.sh` — 1 MEDIUM — #1998:393
- `agents/scripts/core/lib/resolve-py.sh` — 1 MEDIUM — #2019:59
- `agents/scripts/core/merge-gates.d/10-gate-filter.sh` — 1 MEDIUM — #2017:175
- `agents/scripts/core/merge-gates.sh` — 1 MEDIUM — #2006:419
- `agents/scripts/core/merge-watcher-install-autostart.ps1` — 1 LOW — #2007:210
- `agents/scripts/core/panel_verdicts.py` — 1 LOW — #1981:16
- `agents/scripts/core/record-review-verdict.sh` — 1 LOW — #2003:57
- `agents/scripts/core/review-ack.sh` — 1 LOW — #1970:158
- `agents/scripts/core/run-review.sh` — 1 LOW — #1979:397
- `agents/scripts/core/test-gate-selftests.sh` — 1 MEDIUM — #2002:60
- `agents/scripts/core/test-shell-lint.sh` — 1 LOW — #2019:483
- `agents/scripts/project/test-cr-finding-gate-bats.sh` — 1 MEDIUM — #1977:46
- `assets/fonts/README.md` — 1 LOW — #1948:28
- `cmake/SmatchetFontAssets.cmake` — 1 LOW — #1948:45
- `docs/CONTEXT.md` — 1 MEDIUM — #2032:122
- `docs/agent-rules/build.md` — 1 LOW — #2021:14
- `docs/agent-rules/cpp-rules.md` — 1 LOW — #2025:86
- `docs/agent-rules/merge-gates.md` — 1 LOW — #1996:152
- `docs/agent-rules/verifier-sidecar.md` — 1 MEDIUM — #1970:63
- `docs/agent-rules/work-items.md` — 1 MEDIUM — #1978:172
- `docs/design/separate-agents-repo.md` — 1 LOW — #1960:100
- `docs/harness/SETUP.md` — 1 LOW — #1960:79
- `docs/plans/pane-stale-delete-collision.md` — 1 LOW — #2024:54
- `docs/plans/cursor-vexp-coexistence.md` — 1 LOW — #1956:11
- `docs/plans/msvc-build-onboarding-hardening.md` — 1 LOW — #1960:77
- `docs/plans/window-expand-button.md` — 1 LOW — #1966:313
- `docs/self-improvement/categories/debt/2026-08-06-scenario-runner-ticks-twice-per-frame.md` — 1 LOW — #1962:1
- `docs/self-improvement/categories/infra/2026-08-05-merge-watcher-liveness-unmonitored.md` — 1 LOW — #2007:103
- `docs/self-improvement/categories/tooling/2026-08-05-dual-target-syntax-hook-vacuous-without-clang-preset.md` — 1 LOW — #1952:9
- `project.config.schema.json` — 1 LOW — #1955:38
- `scripts/dev/local/rebuild-testproject-plugin.sh` — 1 LOW — #1959:17
- `scripts/dev/lockfile.py` — 1 LOW — #1951:122
- `scripts/dev/run-with-procdump.sh` — 1 LOW — #1958:84
- `scripts/dev/test_work_item_lint.py` — 1 MEDIUM — #1979:474
- `scripts/dev/verify.sh` — 1 LOW — #1955:92
- `scripts/dev/with-msvc-env.sh` — 1 MEDIUM — #2021:65
- `scripts/git-hooks/pre-push` — 1 LOW — #2003:332
- `scripts/publish/release-github.sh` — 1 MEDIUM — #1957:218
- `scripts/publish/test-installer-smoke.sh` — 1 MEDIUM — #1957:135
- `tests/bats/archive_backlog_entry.bats` — 1 LOW — #1998:106
- `tests/ui/docked_tab_focus.test.cpp` — 1 LOW — #1962:131
- `tests/ui/prefs_schema_coverage.test.cpp` — 1 MEDIUM — #1941:163

**Clean (14, surviving lines read in full, no findings):** #2030, #2015, #2011, #2009, #2008, #2001, #2000, #1995, #1990, #1986, #1975, #1973, #1969, #1945.

**Fully superseded (7, no review surface):** #1997, #1988, #1976, #1971, #1968, #1953, #1949 — every introduced line was changed or removed by a later PR; excluded by construction.

## Batch 20-REDO — #1940–#1878 (60-PR full re-sweep, 2026-08-16) — SUPERSEDES Batch 20

Coverage: **60 PRs / 64 units — 38 with findings, 16 clean, 6 fully superseded, 0 errored, 0 died.** Net: **1 CRITICAL, 9 HIGH, 39 MEDIUM, 47 LOW** (96 findings, 13 `userVisible`). Survivor-filtered against `origin/develop` @ `3dc6695f`, with `--against origin/develop` passed explicitly on every extractor call. Reviewer model `code-review` (opus/high), 64/64 agents returned; ~3.16M tokens, ~38 min.

**This REPLACES Batch 20's result. Batch 20 reported 0 findings over 53 PRs; a full read of the same range found 96 over 60.** Two independent defects in that batch are corrected here.

**1 — Batch 20 was screened, not read.** It swept only the 2,882 surviving product-C++ lines with a 13-pattern hazard scan and left ~10,400 surviving lines (tests, scripts, agents, CI workflows, docs) unreviewed. Its own header said so and called the zero "a weaker claim". It was weaker than that: **the screen also cleared two real defects it had surfaced.** `SmatchetPlanDocViewerUi.cpp:254` was cleared because `future::get()` sits behind a `wait_for(0ms) != ready` guard — but that guard covers *readiness*, never *exceptions*; the call is still made before `loadInFlight` is cleared and without a `try/catch`, and the batch even noted the Lua sibling clears its flag first without drawing the conclusion. `LocalCacheManager.cpp:186` was cleared on "no UI TU calls `LocalCacheManager` — the two `Ui/` files that name it do so only in comments"; the reach is three hops, so grepping `Ui/` for the class name could not see it (`Ui/SmatchetCommentsModalUi.cpp:82` -> `AppController::UpdateCachedCommentCount`, whose own comment reads "Called on the UI thread from the modal's main-thread post-back" -> `UpdateTicket` -> `Cache->SaveTicket`). Both re-verified by hand against HEAD before writing this. The generalizable lesson: **a pattern screen finds sites; clearing a site needs call-chain and exception-path tracing, which a screen does not do.** Five candidates were recorded as "cleared on inspection" — that is positive evidence the method could not actually produce.

**2 — the frontier was never contiguous.** GitHub reports **60** PRs merged to `develop` in #1878-#1940; the `(#N)` squash-subject scrape that built Batch 20's work-list returns **53**. The 7 it cannot see — #1883, #1919, #1920, #1921, #1923, #1927, #1932 — all landed as true merge commits. Recovered here as 11 per-constituent units (blame attributes lines to a merge commit's constituents, never to the merge commit, so the merge sha yields a falsely-clean `FULLY SUPERSEDED`; the Batch 16 #1593 special is the precedent). Yield was small — 6 constituents fully superseded, 4 clean, 2 findings on #1883's `release.yml` — but the release-publishing pipeline had never been survivor-reviewed at all. Filed as [`categories/tooling/2026-08-16-historical-review-worklist-misses-merge-commit-prs.md`](categories/tooling/2026-08-16-historical-review-worklist-misses-merge-commit-prs.md) (P1, recurrence of the Batch 13 #1439/#1577/#1593/#1597 class).

**13 `userVisible` findings are Issue candidates under ADR-0014 and are NOT yet filed** — held pending review rather than opened unreviewed.

### Actionable set — CRITICAL / HIGH / every `userVisible` finding (17)

Full problem + fix. Everything below this block is one-line; the complete structured set, fixes included, is in [`historical-review-findings-2026-08-16.jsonl`](historical-review-findings-2026-08-16.jsonl).


**CRITICAL (1)**
- **#1903 (4c1f7e7d) · `Source/Core/src/TicketFieldEditor.cpp:1067`** · **userVisible** — HandleWorklogSave() is called straight from the ImGui render path (RenderTimeTrackingModal -> HandleWorklogSave, TicketFieldEditor.cpp:1226) and calls app.SubmitWorklog(), which synchronously does ConfigManager::Load() plus a blocking HTTP POST (backend->Collaboration()->AddWorklog). The whole UI freezes for the duration of the Jira round-trip with no spinner/progress cue — a Pillar-1 violation, and on a slow/hung tracker the app appears hung. **Fix:** Move the submit off the render path: kick the SubmitWorklog call into a background task (LaunchBackgroundTask/std::async like the sibling watcher/comment posters), keep an 'in-flight' flag in s_ActiveWorklogState that disables Save and draws a progress cue, and apply the VoidResult (close popup or set ErrorMsg) via PostToMainThread on completion.

**HIGH (9)**
- **#1881 (2966edc7) · `agents/scripts/core/test-agent-contract.sh:440`** — Check 15 fails OPEN on the case it exists to catch: `[[ -z "$hint_model" ]] && continue` skips any agent whose harness-hints claude-code model is missing, and the check never asserts that a top-level `model:` exists at all. Check 5 skips on the same condition (line 186), so an agent file with NO model keys passes every gate and silently inherits the opus-class session model — the exact regression this PR was written to prevent. The check's own header comment (lines 427-431) says "Both must exist and agree". **Fix:** Split the two cases: only `continue` when the agent legitimately has no claude-code hint block at all; when either key is present-but-partial, or when both are absent on a non-README agent, record a mismatch (e.g. `model_mismatch+=("$base: no model: key — agent will inherit the session model")`) instead of skipping.
- **#1885 (6094b058) · `scripts/dev/verifier-produce.py:178`** — extract_scalar scans EVERY character of the response text and returns on the first one that happens to fall in A-T, with no anchoring. Any preamble silently yields a wrong score instead of an error: "Score: B" returns 'S' -> 0.053 (near-worst) rather than B -> 0.947; "The answer" returns 'T' -> 0.0. Scalar mode is exactly the path used for backends prone to a short preamble, so candidates get silently mis-ranked with no failure signal. **Fix:** Anchor the letter parse: only accept the response when the stripped text is a single A-T letter (or the first token after stripping punctuation is one char in _LETTER_SET); otherwise fall through to the numeric parse and raise ProduceError if that fails too.
- **#1891 (0c68611b) · `scripts/dev/verifier-produce.py:269`** — is_degenerate() treats len(dist) < 2 as degenerate, but `dist` is not the raw top_logprobs — extract_logprobs() filters it down to A-T letter tokens only. A healthy, highly confident backend whose top-20 contains just one letter token (remaining slots being whitespace/punctuation/word-piece variants) yields len(dist)==1, so --strict aborts the entire run with "backend returned degenerate logprobs" on a perfectly good response. The DeepSeek recipe added in the same PR puts --strict in the copy-paste command, making this the default path. **Fix:** Only treat a single-letter dist as degenerate when the raw top_logprobs list itself was short/uninformative (e.g. pass the pre-filter token count into is_degenerate), or drop the len<2 rule and rely solely on the placeholder/flat-value checks.
- **#1894 (b472830e) · `Source/Core/src/Persistence/LocalCacheManager.cpp:186`** · **userVisible** — RunWriteTxnWithBusyRetry blocks the calling thread with std::this_thread::sleep_for between attempts, and each of the 5 attempts can additionally sit up to the armed busy_timeout (setBusyTimeout(5000) in ApplyWalPragmas) before SQLite returns BUSY. Worst-case SaveTicket/SaveTickets latency goes from ~5s to ~25s + 375ms of sleeps, with no total-deadline cap. This is not purely a background path: AppController::UpdateCachedCommentCount -> UpdateTicket -> Cache->SaveTicket is documented (AppController_CatalogAndFieldEdit.cpp:852) as "Called on the UI thread from the modal's main-thread post-back", so under real cache contention the retry loop stalls the frame pump with no progress cue. Each attempt also holds stmtMutex_ for its full BEGIN..COMMIT, serializing every other statement on the instance. **Fix:** Bound the whole retry by a wall-clock deadline (e.g. give up after ~500ms total) instead of only an attempt count, and/or lower the per-attempt busy_timeout while retrying; alternatively make the UI-thread caller (UpdateCachedCommentCount) route the cache write off the render thread so no sleep_for ever runs on the main thread.
- **#1902 (8e91dafb) · `.github/workflows/plan-lock-gate.yml:39`** — This Dependabot bump pinned actions/checkout to the full SHA 3d3c42e5aac5ba805825da76410c181273ba90b1 in all 28 other workflows, but here it wrote the mutable tag `uses: actions/checkout@v7.0.1`. Verified repo-wide: this is the ONLY unpinned third-party action in .github/workflows, and no gate enforces SHA pinning (test-workflow-yaml.sh has no pin check), so nothing will catch it. A moved/retagged v7.0.1 executes attacker-controlled code inside the plan-lock hard-net gate job. **Fix:** Replace with `uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1` to match the other 28 workflows; optionally add a SHA-pin assertion to agents/scripts/core/test-workflow-yaml.sh so drift fails CI.
- **#1928 (08cdde44) · `Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:238`** · **userVisible** — StartLoadSelected latches `s.loadInFlight = true;` BEFORE the `std::async` launch on line 239. `std::async` can throw (std::system_error when threads are exhausted, std::bad_alloc), and there is no try/catch — the throw escapes into the ImGui frame and, if caught anywhere upstream, leaves loadInFlight permanently true with an invalid loadFuture, so PollLoadResult early-outs forever and the plan-doc viewer never loads another document for the rest of the session. The sibling site added by this same PR (LuaConsolePlugin::KickScriptLoad, lines 305-326) explicitly documents and avoids exactly this ordering. **Fix:** Mirror KickScriptLoad: build the future into a local inside try/catch, LOG_ERROR + surface a message on failure, and only assign `s.loadFuture` / set `s.loadInFlight = true` after the launch succeeded.
- **#1928 (08cdde44) · `Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:254`** · **userVisible** — PollLoadResult calls `s.loadFuture.get()` (line 254) BEFORE clearing `s.loadInFlight` (line 255) and without a try/catch. A worker exception (bad_alloc on the up-to-1 MiB buffer, an ifstream exception) is rethrown here on the render thread: the future is consumed but loadInFlight stays latched true, permanently wedging the viewer. LuaConsolePlugin::PollScriptLoad (lines 365-385) clears its flag before get() and catches, with a comment naming this precise hazard; the viewer was not given the same treatment. **Fix:** Clear `s.loadInFlight = false;` before `get()`, and wrap the `get()` in `try { ... } catch (const std::exception& ex) { LOG_ERROR(...); s.body = <error text>; return; }`.
- **#1928 (08cdde44) · `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:397`** · **userVisible** — In PollScriptLoad the `if (result.name != selectedScriptName_) return;` bail leaves the editor blank (StartLoadSelectedScriptIntoEditor blanked it and re-baselined diskSnapshot_ onto the blank) with scriptLoadInFlight_ false and scriptLoadRefusedReason_ empty (KickScriptLoad cleared it) — so SaveCurrentScript's two blank-buffer guards (lines 432 and 439) are both open. Reachable: select A -> read kicked; RefreshScriptList/SyncSelectionToList clears the selection because A vanished from disk; result A lands and is dropped here; a later SyncSelectionToList tick auto-selects the first script B; Save then writes the empty buffer over B, truncating a real script. Every other non-applying outcome in this function (TooLarge, worker throw, launch failure) sets scriptLoadRefusedReason_ for exactly this reason. **Fix:** On the name-mismatch bail, set `scriptLoadRefusedReason_` (e.g. "The script never loaded — reselect it before saving.") and clear reloadNoticeName_, so Save stays gated while the editor holds a blank buffer that matches no loaded file.
- **#1935 (ec7321cf) · `Source/Core/src/Tracker/TrackerSetupPure.cpp:71`** · **userVisible** — ApplyVerifiedSaveUnlock is documented as the FIRST-RUN unlock but has no first-run condition — it fires on any session where ReadOnlyMode is true and the pin matches. A veteran (BackendHasBeenReachable already true) who ticks "Read-only mode" (SmatchetPreferencesUi.cpp:289 writes d.cfg directly), presses Test connection (green -> pin set), then Save & Sync has their deliberate write-protection silently cleared, with only a LOG_INFO and no UI cue. The one-shot pin consumption (a4133112) only closed the re-enable-AFTER-save ordering, not enable-then-probe-then-save. **Fix:** Gate on actual first-run: add `|| cfg.BackendHasBeenReachable` to the early-return guard so the unlock can only fire on an install that has never latched reachability; add a bucket-A case for the veteran-enables-read-only path.

**MEDIUM (5)**
- **#1879 (2d347dbf) · `Source/Core/src/TicketGridSortPure.cpp:99`** · **userVisible** — CompareNumericValues' double path accepts NaN: strtod parses "nan"/"NaN" (and "inf") as a whole-string match, so for a "number"-typed column with two NaN cells `da != db` is true and `(da < db)` is false, returning +1 for BOTH Compare(a,b) and Compare(b,a). That comparator is fed straight into std::stable_sort in Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:205, violating strict weak ordering (UB / nondeterministic grid order). **Fix:** In ParseWholeDouble, reject non-finite results (return false unless std::isfinite(out)); in CompareNumericValues decide with `if (da < db) return -1; if (db < da) return 1; return 0;` instead of `da != db`.
- **#1879 (2d347dbf) · `Source/Core/src/TicketGridSortPure.cpp:165`** · **userVisible** — The date-like word heuristic `idLower.find("time") != npos` matches every time-tracking id the same file deliberately special-cases (timespent, timeestimate, timeoriginalestimate, aggregate*), so IsTrackerDateOrDateTimeField returns true for duration columns. Callers then treat them as dates: TicketGridModel.cpp:204 sets IsDateLike, and SmatchetGridUiSupport.cpp:359 routes copy through DisplayValueForTrackerDateField for a raw seconds value. **Fix:** Short-circuit at the top of IsTrackerDateOrDateTimeField with `if (kTimeTrackingFieldIds.count(fieldId)) return false;` (the set is already file-local), mirroring the ordering CompareFieldValuesForSort already uses.
- **#1896 (50352045) · `Source/Core/include/Tracker/CollaborationPreconditionPure.h:58`** · **userVisible** — CollaborationErrorToVoidResult maps a failing TrackerError with an empty Detail to VoidResult::Err("") (the PR's own test at CollaborationPreconditionPure.test.cpp:108 pins this). Downstream the empty message is indistinguishable from success: the watch-self caller in Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp:232-233 encodes success as an empty std::string (`r.has_value() ? std::string() : r.error()`) and the tooltip at line 242 only reports failure when `!watchSelfError.empty()`, so a backend error with no Detail is silently swallowed and the user believes the watch succeeded. **Fix:** In CollaborationErrorToVoidResult, fall back to a non-empty message when err.Detail is empty, e.g. `return VoidResult::Err(err.Detail.empty() ? std::string("Tracker collaboration request failed.") : err.Detail);` (and update the empty-Detail test to assert the fallback text). Same treatment for the read-side CollaborationResultToResult mapping.
- **#1903 (4c1f7e7d) · `Source/Core/src/Commands/Builtin/BuiltinCommands_TicketMutations.cpp:103`** · **userVisible** — ticket.add_worklog does not validate the 'seconds' arg. args.value("seconds", 0) defaults to 0 when the caller omits it, and a negative value is also accepted; the formatter then falls to the final branch and submits timeSpent="0s" (or "-30s") to the tracker, producing an opaque backend rejection instead of a clear argument error. The formatter also silently drops the seconds remainder (e.g. 3630s -> "1h", 90s handled but 3690s -> "1h 1m" loses 30s). **Fix:** Reject non-positive seconds up front with CommandResult::Failure(ErrorCode::InvalidArgument, "seconds must be > 0"), and either append the "%ds" remainder to the h/m string or document the truncation to minute granularity.
- **#1937 (fce0951c) · `Source/Core/src/Ui/SmatchetAboutUi.cpp:456`** · **userVisible** — The dismissal guard calls raw `::ImGui::IsPopupOpen(kAboutPopupId, ...)` while the open/begin calls two lines earlier go through the localized shim (`ImGui` is #defined to SmatchetLocalizedImGui, whose OpenPopup/BeginPopupModal run WindowTitleFromSource). `kAboutPopupId` == "About Smatchet" has a catalog entry ({"window.about", "About Smatchet", "À propos de Smatchet"}), so under a non-English locale the popup is registered under the translated ID and this raw lookup hashes the English literal — it returns false unconditionally. The very protection the adjacent comment describes (do not clear state when a deeper popup owns the frame) is therefore dead in that locale: showAbout is cleared and the cached snapshot released while the modal is still open. **Fix:** Route it through the shim like its siblings — `ImGui::IsPopupOpen(kAboutPopupId, ImGuiPopupFlags_AnyPopupLevel)` (adding an IsPopupOpen forwarder to SmatchetLocalizedImGui.h that applies WindowTitleFromSource), or compare against `::ImGui::GetID(SmatchetLocalization::WindowTitleFromSource(kAboutPopupId))`.

**LOW (2)**
- **#1904 (1af13407) · `Source/Core/src/Ui/AnnotateAnalysisUi_Modals.cpp:271`** · **userVisible** — The Result-unpack here takes `gerr = r.error()` with no empty-message fallback, unlike the two sibling sites this same PR added in SmatchetUserInfoUi.cpp (lines 266/294, which use `r.error().empty() ? "Group lookup failed." : r.error()`). The PR's own seam test (CollaborationPreconditionPure.test.cpp:139) asserts a backend error with an empty Detail still maps to an Err, so gerr can legitimately be empty; the post-back then guards with `if (State().profileGroups.empty() && !gerr.empty())`, so that failure renders as an empty group list with no error shown — a silent failure in the user-profile modal. **Fix:** Mirror the sibling sites: `gerr = r.error().empty() ? "Group lookup failed." : r.error();`
- **#1905 (bc1c5030) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:784`** · **userVisible** — SearchUsersByQuery's backend-absent error is hardcoded to "Jira backend is not initialized." while its two siblings introduced by the same change (FetchIssueWatchers L715, FetchIssueVotes L762) say "Tracker backend is not initialized.". Smatchet supports Linear/GitHub backends, so a non-Jira user hitting this path (users.search command, JQL autocomplete, annotate assign resolve) sees a wrong tracker name. **Fix:** Change the L784 message to "Tracker backend is not initialized." to match the sibling reads (and update any test pinning the literal).

### Internal debt — 34 MEDIUM + 45 LOW, indexed by file

All `userVisible:false`. Listed per file rather than per finding — the full problem + fix for every row below is in [`historical-review-findings-2026-08-16.jsonl`](historical-review-findings-2026-08-16.jsonl), keyed by `pr` + `file` + `line`.

- `docs/plans/refactor-quickwins-parser-lua-service.md` — 4 MEDIUM, 2 LOW — #1898:27, #1898:28, #1898:29, #1898:30, #1898:39, #1898:40
- `scripts/dev/verifier-produce.py` — 2 MEDIUM, 4 LOW — #1885:161, #1885:189, #1885:205, #1891:274, #1891:520, #1887:521
- `scripts/dev/verifier-calibrate.py` — 3 MEDIUM, 2 LOW — #1887:95, #1887:108, #1887:201, #1887:356, #1887:383
- `docs/plans/duration-parser-unification.md` — 3 MEDIUM, 1 LOW — #1913:34, #1913:44, #1913:46, #1913:60
- `docs/self-improvement/postmortems.md` — 1 MEDIUM, 3 LOW — #1929:286, #1929:286, #1931:286, #1931:292
- `.github/workflows/release.yml` — 1 MEDIUM, 2 LOW — #1883:143, #1883:174, #1930:195
- `docs/plans/py-probe-exec-validation-gate.md` — 1 MEDIUM, 2 LOW — #1939:3, #1939:53, #1939:191
- `scripts/dev/verifier-endpoint.py` — 2 MEDIUM, 1 LOW — #1889:177, #1889:186, #1889:286
- `scripts/dev/verifier-sidecar.py` — 1 MEDIUM, 2 LOW — #1884:9, #1884:143, #1884:537
- `tests/ui/ai_prefs_autosave_flow.test.cpp` — 2 MEDIUM, 1 LOW — #1888:270, #1888:304, #1888:318
- `Source/Plugins/Mcp/McpPlugin.cpp` — 1 MEDIUM, 1 LOW — #1893:963, #1893:999
- `agents/scripts/core/small_helper_audit.py` — 1 MEDIUM, 1 LOW — #1914:101, #1914:101
- `agents/scripts/core/test-agent-contract.sh` — 2 MEDIUM — #1938:72, #1881:439
- `agents/scripts/core/test-shell-lint.sh` — 1 MEDIUM, 1 LOW — #1939:462, #1939:465
- `tests/Core/AboutInfo.test.cpp` — 1 MEDIUM, 1 LOW — #1937:141, #1937:172
- `.github/workflows/tsan-linux-nightly.yml` — 1 LOW — #1886:92
- `PRIVACY.md` — 1 MEDIUM — #1916:45
- `QUICKSTART.md` — 1 MEDIUM — #1933:3
- `README.md` — 1 LOW — #1912:30
- `Source/Core/src/AttachmentAppUpdateService.cpp` — 1 LOW — #1911:159
- `Source/Core/src/TicketGridSortPure.cpp` — 1 LOW — #1879:33
- `Source/Core/src/Tracker/TrackerSetupPure.cpp` — 1 LOW — #1935:20
- `agents/_shared/skills/git-cleanup-procedures/SKILL.md` — 1 MEDIUM — #1881:160
- `agents/scripts/core/dead_export_audit.py` — 1 LOW — #1914:151
- `agents/scripts/core/issue-sweep.sh` — 1 MEDIUM — #1936:62
- `agents/scripts/core/test-markdown-links.sh` — 1 LOW — #1914:44
- `agents/scripts/project/migrate-bugs-to-issues.sh` — 1 MEDIUM — #1936:38
- `agents/scripts/project/test-about-buildinfo-bats.sh` — 1 LOW — #1937:45
- `docs/agent-rules/verifier-sidecar.md` — 1 LOW — #1884:3
- `docs/plans/build-quality-velocity-hardening.md` — 1 LOW — #1908:239
- `docs/plans/about-dialog-help-menu.md` — 1 LOW — #1937:42
- `docs/plans/gate-blind-spot-sweep.md` — 1 LOW — #1917:59
- `docs/self-improvement/categories/applied.md` — 1 LOW — #1886:15
- `docs/self-improvement/categories/tooling/2026-08-04-gate-scripts-resolve-only-python-probes.md` — 1 MEDIUM — #1940:26
- `docs/self-improvement/historical-review-findings.md` — 1 MEDIUM — #1878:378
- `scripts/dev/test-mutation-smoke.sh` — 1 LOW — #1886:3
- `tests/bats/capture_intent.bats` — 1 LOW — #1924:375
- `tests/bats/fail_open_gate_cluster.bats` — 1 MEDIUM — #1936:33
- `tests/bats/lint_rules.bats` — 1 LOW — #1936:36
- `tests/bats/verifier_endpoint.bats` — 1 LOW — #1889:67
- `tests/bats/verifier_produce.bats` — 1 LOW — #1885:78
- `tests/ui/mcp_resources.test.cpp` — 1 LOW — #1893:158
- `tests/ui/tracker_first_run_setup.test.cpp` — 1 LOW — #1935:110
- `tools/repo-health/generate.py` — 1 LOW — #1917:90

**Clean (16, surviving lines read in full, no findings):** #1926, #1923, #1921, #1920, #1910, #1909, #1907, #1906, #1901, #1900, #1899, #1895, #1892, #1890, #1882, #1880.

**Fully superseded (6, no review surface):** #1934, #1932, #1927, #1922, #1919, #1918 — every introduced line was changed or removed by a later PR; excluded by construction.

## Batch 20 (SCREENED) — #1940–#1878 (53-PR sweep, 2026-08-08) — ⚠️ SUPERSEDED

> **⚠️ SUPERSEDED 2026-08-16 by § Batch 20-REDO above. Do not cite this batch's zero or its
> coverage claim.** Both were wrong, independently: (a) the pass was a 13-pattern hazard *screen*
> over product-C++ survivors only, and a full agent read of the same range found **96 findings**
> (1 CRITICAL, 9 HIGH) — the screen additionally *cleared* two real defects it had surfaced
> (`SmatchetPlanDocViewerUi.cpp:254`, `LocalCacheManager.cpp:186`), so its "cleared on inspection"
> list is not evidence; (b) the range holds **60** merged PRs, not 53 — the `(#N)` squash-subject
> scrape cannot see a true merge commit, so #1883/#1919/#1920/#1921/#1923/#1927/#1932 never entered
> the work-list and the "contiguous" claim below was false when written. Kept unedited as the record
> of what was claimed; the corrected result is the REDO section.

Coverage: **53 reviewed — 0 with findings, 51 clean, 2 fully superseded, 0 errored.** Net: **0 CRITICAL, 0 HIGH, 0 MEDIUM, 0 LOW.** Closes the gap Batch 19 left: everything merged after #1876 up to #1940, joining this run to the targeted pass above. **The frontier is now #1–#1940 contiguous; the next sweep resumes from #1941.** Survivor-filtered against `origin/develop` @ `c00197d4`, with the extractor's review-ref bug fixed (see the caveat below) and `--against origin/develop` passed explicitly.

**Supersede rate 19%** (~3,200 of 16,442 introduced lines gone), against ~0% for the newest-30 slice in the pass above. These PRs are 3–4 weeks old (2026-07-15 → 08-04), which is the age at which the blame-survivor filter starts doing real work. Worth remembering when choosing the next slice: **age, not count.**

> **Method differs from Batches 13–19 — read this before trusting the zero.** Those batches ran a
> `code-review` agent (opus/high) over every surviving hunk. This one was **screened, not fully
> read**: all 53 digests were extracted, the 2,882 surviving **product-C++** lines were swept with a
> 13-pattern hazard scan (raw `new`, empty `catch(...)`, bare `json::parse`, C string functions,
> `[i±1]` index math, `.size()-1`, unchecked `front()`/`back()`/`->second`, unguarded
> `future::get()`, `detach`, `sleep_for`, unterminated hex escapes, `.at()`), every hit was then
> read in full context, and `TicketGridSortPure.cpp` was read end-to-end as the highest-risk unit.
> The other ~10,400 surviving lines (tests, scripts, agents, docs) were **not** reviewed. So this
> batch's zero means "no defect of the screened classes in product C++", which is a weaker claim
> than Batch 19's. The frontier advances anyway — but a future reader wanting Batch-19-grade
> assurance over #1878–#1940 should re-run the full agent pass rather than read this as equivalent.

**Five candidates surfaced by the scan, all cleared on inspection** (recorded so the next screen does not re-flag them):
- `SmatchetPlanDocViewerUi.cpp:254` and `LuaConsolePlugin.cpp:371` — `future::get()` in UI paths, the Pillar-2 shape. Both are correctly preceded by `wait_for(0ms) != ready → return`; the Lua one additionally clears `scriptLoadInFlight_` **before** `get()` so a worker exception cannot latch the flag and wedge Save for the session.
- `LocalCacheManager.cpp:186` — `sleep_for` backoff inside the SQLite busy-retry. The `lock_guard` scope ends before the sleep (a contending writer never sleeps holding the mutex), and no UI TU calls `LocalCacheManager` — the two `Ui/` files that name it do so only in comments.
- `AboutInfo.cpp:105-106` — `line[line.size() - 1]` guarded by `!line.empty()` on the same condition.
- `AppController_LocalCacheDb.cpp:135` — `raw-new` false positive; the match is the words "new database" inside an error string.

**Clean (51, surviving lines screened, no findings):** #1940, #1939, #1938, #1937, #1936, #1935, #1934, #1933, #1931, #1930, #1929, #1928, #1926, #1924, #1917, #1916, #1914, #1913, #1912, #1911, #1910, #1909, #1908, #1907, #1906, #1905, #1904, #1903, #1902, #1901, #1900, #1899, #1898, #1896, #1895, #1894, #1893, #1892, #1891, #1890, #1889, #1888, #1887, #1886, #1885, #1884, #1882, #1881, #1880, #1879, #1878.

**Fully superseded (2, no review surface):** #1922, #1918 — every introduced line was changed or removed by a later PR; excluded by construction.

## Reconcile + targeted pass (2026-08-08) — NOT a batch; the frontier is unchanged

**Reconcile.** `historical-review-ledger-reconcile.sh --reconcile` over the 18 still-open rows
(~11 unique PRs; several are listed in both a batch section and the sweep-status section):
**0 of 18 flagged as likely superseded.** Unlike the 2026-07-10 pass, the probes found nothing to
drop and no manual re-verification was performed, so every open finding stays open. This section
exists partly to reset the 14-day staleness nudge with an honest dated result.

**Targeted pass — deliberately NOT numbered as a batch.** This session reviewed the surviving
product C++ of **4 PRs only** (#1941, #1952, #1954, #1962 — the code-bearing commits in the newest
~30 on develop), ~3,000 of their 3,614 `Source/` survivor lines. It is not a contiguous sweep and
**does not advance the frontier**: Batch 19 ended at #1876 and the lowest PR touched here is #1941,
so **#1877–#1940 (~60 PRs) remains entirely unswept and the next sweep still resumes from #1877.**
Recording it here so the coverage claim is not overstated by a later reader.

Also worth knowing: the newest-30 slice yielded a **~0% supersede rate** — 30 PRs is about 48 hours
at this repo's merge velocity, so the blame-survivor filter had nothing to subtract and the pass
degraded into an ordinary re-review. Age, not count, is what makes the technique pay.

### MEDIUM (2) — both FIXED, shipped in #1989 (`c00197d4`)
- **#1941 (c7fb2236) · `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:262`** — `TrackerBackendIndexFromBuf` documented itself as a "defensive case-insensitive match" for hand-edited `smatchet_config.json` but matched exactly two spellings per backend (`"GitHub" || "github"`). `"Github"` matched neither and fell through to the Jira default, so Preferences drew the **Jira** credential rows while `DefaultTrackerBackendFactory` — always properly lowercased — built a `GitHubClient` from the same value; the user edits Jira fields against a GitHub backend with nothing on screen saying so. `userVisible:true`. Fixed by moving the mapping to one shared `Tracker/TrackerBackendKind.h` over the canonical `ToLowerAsciiCopy`, so the UI and the factory cannot diverge again.
- **#1941 (c7fb2236) · `Source/Core/src/Ui/PreferencesFilter.cpp:25`** — the search haystack is built from `TranslateSource` output (translated text) but folded with an ASCII-only lowercase, so `É` (`C3 89`) and `é` (`C3 A9`) stayed unequal and typing `PRÉ` never matched `préférences` in the French UI the search exists to serve. `userVisible:true`. Fixed: `ToLowerAscii` → `ToLowerFold`, additionally folding the Latin-1 Supplement letters (flat `+0x20` on the trail byte; `U+00D7` excluded). Stops at Latin-1 on purpose — Latin Extended-A needs real case tables.

### LOW (2) — open, not filed
- **#1941 (c7fb2236) · `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp:284`** — `d.cfg.TrackerType == "Plane"` is the same exact-match-on-hand-editable-config class as the MEDIUM above; a config saying `"plane"` skips the guard and lets the async user-search path run for a backend that does not support it. Wasted async work, not corruption. Left unfixed deliberately: different subsystem, async behaviour not exercisable in the review container.
- **#1954 (1f4a6b74) · `Source/Core/src/Ui/SmatchetAboutIcon.cpp:31`** — `g_aboutIconFailed` latches on *any* failure, justified by "if they fail once they fail every frame". True of the decode; **not** of the upload, which can fail on `!RendererHasTextures()` — an environmental backend flag. One early call latches a glyph-only About icon for the whole session. Unlikely to fire (the flag is set at backend init, long before a user opens About).

**Reviewed clean (surviving lines read, no findings):** `SmatchetIcoDecode.cpp` (binary parser — every bound traced), `SmatchetImageTextureCache.cpp` (a suspected Pillar-3 race was disproved: every caller is a UI-thread draw path and the async icon route hops back via `PostToMainThread` so the upload stays UI-thread-only), `PreferencesSchema.cpp`, `ScenarioCaptureQuiesce.cpp`, `SmatchetPreferencesUi_Templates.cpp` (all six reorder/delete loops correctly bounded), `SmatchetPreferencesUi_Shell.cpp`, `SmatchetPreferencesUi_General.cpp` (`readlink`/`GetModuleFileNameA` correct on all three platforms; the `fork`/`setsid`/`execl` child touches only async-signal-safe calls), `SmatchetAboutUi.cpp`.

**Not reviewed (same slice, ~600 lines):** the draw bodies of `_Local`, `_Assistant`, `_Whisper`, `_QuickCreate`, and two headers — the same filter-gated shape cleared four times above.

### Tooling: the extractor was reviewing against the wrong baseline
Found by *running* the sweep; both fixed in #1987 (`34526b5c`).
`historical-review-survivors.sh` resolved its review ref via `git rev-parse --abbrev-ref origin/HEAD`, which **echoes its unresolvable argument on stdout while exiting 128**. The `origin/develop` fallback was guarded on that string being empty, so it was skipped and `REVIEW_REF` fell through to **local HEAD** — the stale-baseline case the surrounding comment calls CRITICAL, because a line a newer PR already fixed is absent from a behind-by-N tree and **reappears as a false survivor**. The "local HEAD is behind" warning is gated on `REVIEW_REF != HEAD`, so it never printed. Any clone without `origin/HEAD` set (`git remote add` + fetch — how the remote-session clone is built) was affected. Separately, binary files were emitted as reviewable: a golden PNG scored **1173 surviving "lines"** and outranked every source file, and totals could read alive > introduced.

> **Open question for whoever runs the next batch:** if Batches 13–19 were produced in a clone
> without `origin/HEAD` set, their survivor sets may contain false survivors — findings against
> lines a later PR had already fixed. Nothing here proves that happened; the runs may well have had
> `origin/HEAD` (a plain `git clone` sets it). Worth confirming before trusting a spot-check that
> contradicts one of those batches, and worth passing `--against origin/develop` explicitly from now
> on regardless.

## Remediation pass (2026-07-12) — Batch 17's 4 findings, all fixed

The 4 findings from the latest sweep (Batch 17, #1737–#1696) — all `userVisible:false`,
backlog-only until now — are fixed on develop, each with non-vacuous coverage where a
gate/behaviour is involved (reverting the fix fails the new test):

- **#1698** (MEDIUM) `scripts/dev/mutation-smoke.sh` — a zero-scorable run under **`--gate`**
  now **fails closed** (exit 1) instead of green: `--gate` is the explicit opt-in to blocking
  (the nightly TSan lane runs `--gate --floor 80`), so a run where every guard mutant landed in
  a FILE-MISSING/SPEC-ERROR/BUILD_FAIL bucket verified zero assertion strength and must not pass.
  The **non-gate advisory** pilot path stays exit-0 (`fail-open-ok:` retained, slice-F scope).
  Two new bats cases lock both halves (`tests/bats/mutation_smoke.bats`: gate→exit 1, non-gate→exit 0).
- **#1732** (LOW) `Source/Core/src/Ui/AdfToMarkdown.cpp` — `EmitAdfCodeBlock` no longer copies the
  whole accumulated `s.out.str()` per code block (O(blocks × total-output) quadratic); it tracks the
  last emitted char locally (O(1) per block). New doctest covers the multi-node newline-terminated
  branch (`tests/Core/MarkdownConvertAdf.test.cpp`).
- **#1728** (LOW) `docs/plans/shipped/god-file-splits.md` — the 5 broken `../<slug>.md` sibling links
  now point to each target's real home (`appcontroller-clusters-followup.md` moved to `shipped/`
  since the finding → same-dir; `build-quality-velocity-hardening.md` + `testing-surface-roadmap.md`
  are in `active/` → `../active/…`). `test-plan-ref-integrity` green (195/195).
- **#1722** (LOW) `docs/plans/shipped/appcontroller-fan-in-phase6-dissolution.md` — the Phase 5
  successor ref now cites `docs/plans/shipped/appcontroller-fan-in-phase5-facets.md`.

The historical-review **open list is again empty** — every documented finding through Batch 17 is
resolved. (Next sweep still resumes from #1738; the #1738–#1798 range is not yet survivor-swept.)

## Remediation pass (2026-07-10) — the 7 open findings, all fixed

All 7 findings the reconcile pass (below) re-verified STILL OPEN are now fixed on
develop, each with non-vacuous test coverage (reverting the fix fails the new
test/selftest):

- **#1116 + #789** — `pre-ship.sh` now resolves a WORKING python (execution-probed,
  not bare `command -v`) and **fails closed** on strict-zone detection when none is
  found; comment-audit / md-lint route through the resolver. New `--selftest` case
  locks the fail-closed-on-no-python path. → **PR #1733**.
- **#329 + #80 + #77** — the three `test-*.sh` gate wrappers: the perf-marker leak
  gate scans REGENERATED content (not the stale committed doc); theme-syntax gains a
  zero-assertion guard; views-reorder gains a zero-test guard and drops the dead
  `extract()`. Each locked by a new `--selftest` (`test-gate-selftests` now enrolls
  64 scripts). → **PR #1734** (a CodeRabbit finding — a regen failure passing green —
  fixed in the same PR).
- **#784 + #807** — `postmortem-owed.sh` dedup now splits slash-joined PR trailers
  (new bats case, 35 tests); README build-script path corrected to
  `scripts/dev/local/build_and_run.ps1` and the false auto-vcvars claim replaced with
  the accurate `cl.exe`-on-PATH requirement. → **PR #1736**.

The historical-review **open list is now empty** (Batches 1–12 below retain the ~25
MEDIUM + ~60 LOW advisory doc-drift residue — "verify on demand", non-actioned).

## Reconcile / re-verification pass (2026-07-10)

Ran `historical-review-ledger-reconcile.sh --reconcile` (0/11 flagged by the
coarse probes) **then manually re-verified every one of the 11 STILL-OPEN
findings from the 2026-06-20 pass against `origin/develop` by reading the cited
code at HEAD** — because the automated probes are conservative (they never
flagged the 4 below, yet all 4 are genuinely resolved). **Of 11: 4 now DONE, 7
still open.**

**Newly resolved since 2026-06-20 (drop from the open list):**
- **#1138 / #1158 / #1049** (the 3 user-visible defects elevated to GitHub
  Issues) — **Issues #1457 / #1458 / #1459 are all CLOSED (COMPLETED)**. The
  `gridContexts_` map race, the `pane.new` un-credentialed duplicate-spawn, and
  the annotate day→CL re-fire UI freeze are fixed on develop.
- **#919** (HIGH) `merge_watcher.bats` — **DONE**. The broken `case "$2 $3"`
  bash-stub `handle_pass` tests were rewritten to Python monkeypatch
  (`squash_merge_pr` → `ENQUEUED_SENTINEL`); the enqueue + immediate-merge
  queue-safety paths now genuinely run (was Windows-unresolvable/skipped).

**STILL OPEN (NOT DONE) — re-verified alive at develop 2026-07-10** (all HIGH,
internal gate/test/doc debt; no product defect → backlog, not Issues):
_(⚠ ALL 7 now FIXED — see the "Remediation pass (2026-07-10)" section above; PRs
#1733 / #1734 / #1736. Retained here for the audit trail.)_
- **#1116** `scripts/dev/pre-ship.sh:~292` — strict-zone detection uses bare
  `command -v python3` (not the `resolve_python` resolver in
  `agents/scripts/project/lint-rules.d/00-common.sh`) and swallows failure via
  `|| true`, so the Windows `python3` App-Execution-Alias stub (exit-49) leaves
  `$review_strict_zones` empty → a strict-zone diff N/A-passes the review gate.
- **#789** `scripts/dev/pre-ship.sh:~227,~239` — `comment_audit.py` + `md_lint.py`
  still invoked via bare `python3`, ignoring the repo python resolver.
- **#329** `test-perf-marker-inventory.sh:~30` — leak gate greps the committed
  `docs/perf/MARKER_INVENTORY.md`; the `--check` regen output is echoed but never
  compared, so un-regenerated drift is invisible.
- **#80** `test-theme-syntax-colors.sh:~57` — fails only on `FAILED > 0`; a run
  with zero total assertions (vanished suite) exits 0 (green).
- **#77** `test-ui-views-columns-reorder.sh:~63` — no zero-test guard (PASSED=0,
  FAILED=0 passes) **and** a dead `extract()` helper (:~27) never invoked.
- **#784** `agents/scripts/core/postmortem-owed.sh:~221` — `has_entry` dedup regex
  `[,[:space:]]#$1([^0-9]|$)` splits commas/space but not `/` → slash-joined PR
  trailers (`#906/#907/#908`) re-flag every SessionStart.
- **#807** `README.md:~70,~104` — references `scripts/dev/build_and_run.ps1` (the
  script now lives at `scripts/dev/local/build_and_run.ps1`) + overstates
  auto-vcvars bootstrap (`with-msvc.ps1` not invoked by the main path).

## Verification pass (2026-06-20)

Fresh **live-tree re-verification** of every CRITICAL + HIGH finding (plus the
user-visible MEDIUMs and the 2026-06-08 remediation-log "FIXED" claims) against
`origin/develop` — because survivor batches are point-in-time snapshots and a
week of merges silently closed most of the old priority list. Each verdict below
comes from reading the cited code at HEAD, not the batch text. **Of 4 CRITICAL +
22 HIGH: 15 DONE, 10 NOT DONE, 1 PARTIAL.** The 3 still-open **user-visible**
defects were elevated to GitHub Issues per ADR-0014.

**Fixed since logged (no longer actionable) — re-confirmed at develop:**
- **CRITICAL (4/4):** #86 (`test-build-warnings.sh` now greps MSVC `C4101/4189/4505`
  alongside the GCC `[-Wunused-]` tag), #565 (`PersistAnnotateCfg`), #611
  (`LaunchBackgroundTask`), #761 (`std::async` + non-blocking poll).
- **HIGH:** #892 / #767 (snapshot-on-open), #732 (`MarkPrefsDirty`), #854 (rebuild
  via `BuildFieldPayload`), #671 (`PostAppQuitBestEffort`), #948 (migration moved
  post-`InitBackends` with the resolved live key), #430 (`os.walk` recursion),
  #834 (dir-anchored excludes), #918 (strip-and-classify; `--selftest` passes),
  #519 (404-only → fail-closed), #513 (zero-test guard), #452 ×4 drivers (zero-test
  guards added).
- **MEDIUM:** #670 (global two-pass `FindJiraTransitionId`), #975 (kick-time
  context captured by pointer), #524 (full-body actionable-count parse).

**STILL OPEN (NOT DONE) — re-verified alive at develop 2026-06-20:**
_(⚠ SUPERSEDED by the 2026-07-10 reconcile pass above: #1138/#1158/#1049 and
#919 are now DONE; the live open list is the 7 findings in that newer section.)_

_Product / user-visible → filed as GitHub Issues (ADR-0014):_
- **#1138** (HIGH) `AppController_CatalogAndFieldEdit.cpp:2052` — `gridContexts_`
  map-container data race (worker `find` vs UI-thread `erase`; only per-context
  mutexes, no map mutex) → **Issue #1457**.
- **#1158** (HIGH) `PaneCommands.cpp:170` — `pane.new` arms the create latch
  before the creds check and doesn't clear it on the `Failure` return → spawns a
  duplicate pane despite "no credentials" → **Issue #1458**.
- **#1049** (MEDIUM, user-visible) `AnnotateAnalysisUi_Window.cpp:209` — day→CL
  re-fire unguarded; reassigning the in-flight `shared_future` blocks the UI
  thread in its destructor → **Issue #1459**.

_Internal tooling / gate fail-opens / test+doc debt (backlog, no Issue):_
- **#329** (HIGH) `test-perf-marker-inventory.sh:30` — leak gate greps the stale
  committed `MARKER_INVENTORY.md`, not regenerated content.
- **#80** (HIGH) `test-theme-syntax-colors.sh:57` — no zero-assertion guard →
  vanished suite passes green.
- **#77** (HIGH) `test-ui-views-columns-reorder.sh:69` — no zero-test guard (+ dead
  `extract()` helper still at :27).
- **#1116** (HIGH) `pre-ship.sh:292` — strict-zone detection fails open on the
  Windows `python3` stub (swallowed exit-49); review gate N/A-passes a strict-zone
  diff.
- **#789** (HIGH) `pre-ship.sh:239` (+:227) — markdown-lint / comment-audit
  hardcode `python3`, ignoring the repo's `resolve_python` resolver.
- **#919** (HIGH) `merge_watcher.bats:516,554` — `handle_pass` tests still use the
  broken `case "$2 $3"` stub selector → zero working merge-queue-safety coverage.
- **#807** (HIGH) `README.md:70,99,104` — stale `build_and_run.ps1` path (now under
  `local/`) + false "auto-bootstraps vcvars" claim (`with-msvc.ps1` never invoked).
- **#784** (HIGH, ⚠️ PARTIAL) `postmortem-owed.sh:133` — comma-joined PR trailers
  now dedupe, but **slash-joined** (`#906/#907/#908`, the cited case) still
  re-flags every SessionStart.

**Not individually re-verified this pass:** the ~25 MEDIUM + ~60 LOW doc-drift /
stale-line-pin findings across Batches 1–12 (advisory "no-fix"; the 2026-06-08
remediation log already closed a doc-drift batch). Verify on demand before
actioning — most predate many merges and may be stale like the priority list was.

## Remediation log (2026-06-08)

Autonomous fix pass over the safe, deterministic findings (gate-false-pass +
doc-drift); user-visible product correctness/data-loss bugs routed to their own
PRs or GitHub Issues per ADR-0014. Each item verified still-alive at
`origin/develop` before acting; already-fixed ones marked accordingly.

- ✅ **#918** (coverage-delta-gate false-exempt) — FIXED: dropped the `'*'*` /
  `'*/'` bare comment-continuation cases (genuine continuations are consumed by
  the caller's `in_block_comment` state machine, so a `*`-leading line reaching
  the helper is a pointer-deref statement, not a comment) and replaced the broad
  `'/*'*) return 0` with a strip-and-classify-residual so `/* note */ code();`
  no longer exempts real surface. +2 `--selftest` FALLTHROUGH fixtures.
- ✅ **#834** (coverage.sh bare `Ui` token) — ALREADY FIXED on develop
  (`--excluded_sources "Source*Core*src*Ui"`, dir-anchored). No action.
- ✅ **#909 / #855** (build.md `build_and_run.ps1`/`build_standalone.ps1` refs) —
  FIXED → `scripts/dev/local/…`.
- ✅ **#630** (imgui-draw-pattern audit grep) — FIXED → `Source/Core/src/Ui/Smatchet*Ui*.cpp`.
- ✅ **#657** (CONTEXT.md AppController line-pins :574/:595) — FIXED → symbol-only
  (drift-proof).
- ✅ **#722** (CONTEXT.md header paths) — FIXED → `Source/Core/include/Tracker/{LabelEditDiffPure,GitHubClientHelpers}.h`.
- ✅ **#755** (test-rig.md `AppControllerDepsAdapter.cpp`) — FIXED → `GridContextDepsAdapter.cpp`.
- ✅ **#853** (ADR-0016 line-pin `OfflineQueueService.cpp:788`) — FIXED → symbol-only.
- ✅ **#940** (ADR-0018 plan ref) — ALREADY RESOLVED: tier-less
  `docs/plans/multi-grid-tabs.md` resolves via the ref-integrity resolver
  (117/117). No action.
- ✅ **#670** (Jira wrong-status transition, user-visible correctness) — FIXED:
  matcher extracted to the pure, Logger-free unit `smatchet::jira::FindJiraTransitionId`
  (`JiraIssueMappingPure.{h,cpp}`) with GLOBAL two-pass priority (exact status
  id / `to.name` across all transitions BEFORE the transition-name fallback);
  `JiraIssueMutation.cpp` calls it + logs the divergence warn on a name-fallback.
  4 doctest cases incl. the exact #670 shape (name-"Done" transition leading to
  "In Review" must lose to a later transition leading to "Done").
- ⏭ **#854** (offline scalar-edit data-loss), **#611 / #761 / #732 / #767 / #892**
  (sync-I/O on UI render thread → freeze), **#671** (orphaned subprocess),
  **#948** (tickets_v2 migration key) — user-visible → GitHub Issues (ADR-0014),
  not batched here.
- ⏭ **#908** (CMake dead sol2 re-run-cleanup group) — left as-is: editing the
  sol2 patch chain is higher-risk than the harmless dead comment; deferred.

## Sweep status & remaining work (as of 2026-07-10)

- **Post-#1174 frontier re-establishment (2026-07-10): SWEEP COMPLETE #1–#1695.**
  Work-list = all **494** merged-to-develop PRs in **(#1174, #1695]** (authoritative
  GitHub merged list, cross-validated against the develop squash log; 4 PRs needed
  manual sha resolution — #1439/#1577 edited squash subjects, #1593/#1597 true merge
  commits) minus Batch 12's 17 → **477 swept this session** as Batches 13–16
  (#1695–#1175, contiguous, newest first) + the **#1593 per-constituent special**
  (all below). Aggregate: **473 reviewed + #1593 — 56 with findings, 389 clean,
  31 fully superseded, 0 errored, 0 died; 1 CRITICAL, 4 HIGH, 21 MEDIUM, 38 LOW;
  ~16.1M tokens, ~2.7h wall.** The 5 user-visible findings → GitHub Issues
  **#1699** (B13 CRITICAL, AI-input overflow), **#1706** (B14, prefs stale
  buffers), **#1711/#1712/#1713** (B16: env-scrub `_PATH`, p4vc trailing-`\`,
  comments Gen token). Dominant recurring class across all four batches: the
  **zero-test fail-open driver** (`passed=0&&failed=0`→exit-0) — 12+ new sites.
  With the #1–#1174 baseline, **every merged PR #1→#1695 is survivor-reviewed.**
  **Batch 17 (same session) extended the frontier to #1737** — 36 PRs
  (#1696–#1737, incl. #1700 itself), 4 findings (1 MEDIUM fail-open + 3 LOW),
  0 user-visible. **Batch 18 (2026-07-11) extended the frontier to #1795** —
  55 PRs (#1738–#1795), 8 findings (5 MEDIUM incl. 1 user-visible data-loss →
  Issue #1797, + 3 LOW), 45 clean, 2 superseded. **Batch 19 (2026-07-12) extended
  the frontier to #1876** — 72 PRs (#1796–#1876), 5 findings (2 MEDIUM + 3 LOW),
  65 clean, 2 superseded, 0 user-visible. ~~**The next sweep resumes from #1877**~~
  (same recipe; sha-resolved variant recipe in the Batch 13 header for gh-less
  environments).
  **2026-08-16 — frontier now #1–#2032; the next sweep resumes from #2033.**
  Batch 20 (2026-08-08) is **SUPERSEDED**: it screened rather than read, and its
  work-list missed 7 merge-commit PRs, so it never covered #1878–#1940. § Batch
  20-REDO re-swept that range in full (60 PRs / 64 units, 96 findings) and
  § Batch 21 took #1941–#2032 (86 PRs, 179 findings). **Build the next work-list
  from GitHub's merged list, not the `(#N)` squash-subject scrape** — the scrape
  cannot see a true merge commit, and a merge sha must be expanded to its
  constituents before extraction or the digest reads falsely `FULLY SUPERSEDED`
  (`categories/tooling/2026-08-16-historical-review-worklist-misses-merge-commit-prs.md`).
  Full problem + fix for all 275 findings from both batches:
  [`historical-review-findings-2026-08-16.jsonl`](historical-review-findings-2026-08-16.jsonl).
- **Swept:** **#1–#1174** (batches 1–11) — **the entire merged-PR history reviewed.**
  **SWEEP COMPLETE** — Batch 11 (#116–#1, 113 PRs, the final tail incl. the early
  base-`main` PRs #1–#5) added 2026-06-13;
  Batch 10 (#117–#330, 200 PRs) added 2026-06-13;
  Batch 9 (#331–#438, 100 PRs) + Batch 8 (#439–#541, 100 PRs) added 2026-06-13;
  Batch 7 (#1029–#1174, 122 PRs) added the same day. Tooling:
  `agents/scripts/core/historical-review-survivors.sh` + the `historical-code-review`
  skill (shipped PR #968); the persisted workflow shipped PR #1182.
- **Baseline #1–#1174:** **SWEEP COMPLETE** (above). Every merged PR #1→#1174 was
  historically reviewed survivor-only against origin/develop. (#117 has no merge
  commit — open/closed-unmerged, not a develop squash — and was correctly skipped;
  #18/#72/#96 were never merged.)
- **Post-#1174 (incremental — NOT a clean frontier):** **140 PRs merged into develop
  in (#1174, #1322].** **Batch 12** (below, 2026-06-16) survivor-swept **17** of them
  (a sparse subset of #1282–#1318 surfaced as "merged-since unreviewed" this session,
  **not** a contiguous range) — 2 LOW findings (both → tooling.md, PR #1321), 13 clean,
  2 superseded. The other **~123 are NOT yet survivor-swept here.** A subset was
  spot-reviewed by per-session *rolling* backlog sweeps (e.g. #1300 reviewed
  #1261/#1266/#1293; #1302 reviewed #1268/#1274; #1304 reviewed #1301), routing findings
  to `categories/*`, but those were **never laddered into this ledger** — so there is
  **no clean contiguous reviewed frontier above #1174.** To re-establish one, run the
  persisted workflow over the full `(1174, 1322]` work-list (recipe below; ~123 PRs ≈
  6–7M tokens) and append as Batch 13.
- **Resume instructions (for PRs merged after #1174):**
  1. List the new batch — `gh pr list --state merged --base develop --limit 900
     --json number --jq '[.[] | select(.number > 1174) | .number] | sort |
     reverse'` (raise the `> 1174` bound as the marker advances).
  2. Run the persisted workflow, passing the batch as `args`:
     `Workflow({ name: 'historical-review-sweep', args: [<the numbers>] })`.
     Pass a JSON array — but note this harness delivers `args` to the script as a
     **string** even when you pass an array (probe: `argType:'string'`,
     `parsedIsArray:true`), so the workflow `JSON.parse`s it internally. You don't
     stringify it yourself; you just don't rely on it arriving pre-parsed.
     The script is tracked at
     [`agents/project/workflows/historical-review-sweep.js`](../../agents/project/workflows/historical-review-sweep.js)
     — project-scoped (it embeds Smatchet paths, so it can't live in the
     portable, purity-gated `agents/_shared/workflows/`). `setup-harness.sh` links
     it into the gitignored `.claude/workflows/`, so it resolves by name across
     sessions (run `bash agents/scripts/core/setup-harness.sh claude-code` once
     after a fresh clone). **No per-batch script edit** — pass a different `args`
     list each batch; an empty/unparseable list throws loudly, never a silent
     no-op. Hold concurrency to the Opus ≤6 guardrail (the runtime cap is
     min(16,cores-2)=10 on a 12-core box, above ≤6 — use a hand-rolled 6-lane pool,
     not bare `parallel()`). Or, per PR, run `historical-review-survivors.sh --pr <N>`
     and review the survivor digest manually.
  3. Append each batch's findings here (newest on top) + commit/push.
- **Cost guide:** ~100–120 PRs/batch ≈ 4.9–7.0M output tokens, ~25–55 min wall-clock
  (Opus ≤6 pool; ~36 min for the 113-PR Batch 11, ~56 min for the 200-PR Batch 10).
- **Top still-alive findings to act on first** — ⚠️ **SUPERSEDED by the
  § Verification pass (2026-06-20) at the top of this file.** 11 of the 14 items
  in the original list were fixed by later merges (incl. #86, #854, #670,
  #611/#761/#732/#892, #671, #834/#918). The genuinely-open set is now: the 3
  user-visible defects #1138/#1158/#1049 (→ Issues #1457/#1458/#1459) plus
  internal gate/tooling debt #329/#80/#77/#1116/#789/#919/#807/#784. _(Original
  list kept for history: **#86** (CI warning gate blind under MSVC), #854 (offline
  edit data-loss), #670 (wrong Jira status transition), #611/#761/#732/#892 (sync
  I/O on UI render thread → freeze), #671 (orphaned subprocess),
  #834/#918/#329/#80/#77 (blocking gates measuring wrong / false-passing).
  User-visible ones → GitHub Issues per ADR-0014 when actioned.)_

<!-- Batches appended at the top. -->

## Batch 19 — #1876–#1796 (72-PR sweep, 2026-07-12)

Coverage: **72 reviewed — 5 with findings, 65 clean, 2 fully superseded, 0 errored, 0 died.** Net: **0 CRITICAL, 0 HIGH, 2 MEDIUM, 3 LOW.** Incremental on top of the Batch 18 frontier: everything merged after #1795 up to develop @ `b4227fd5`. **The frontier is now #1–#1876 contiguous; the next sweep resumes from #1877.** Survivor-filtered against origin/develop. (Same sha-resolved workflow + reviewer model as Batches 13–18; 72/72 returned, 0 died; ~25 min, ~2.64M tokens.) **All 5 findings are `userVisible:false` → NO GitHub Issues; backlog only per ADR-0014.** Cleanest batch by rate yet (90% clean). Headline is the #1840 Lua-consent TOCTOU — a safety-gate hardening gap (candidate for elevation to an Issue if the local-file-swap threat model is in scope).

### MEDIUM (2)
- **#1840 (7f8f5bad) · `Source/Core/src/AppController_LuaScriptFiles.cpp:263`** — **consent TOCTOU**: `IsLuaScriptConsented` reads the script bytes to fingerprint them then discards them, and callers re-read the file separately via sol `load_file`/`script_file` (`AppController_LuaBindings.cpp:415/568/647`) — two independent disk reads, violating the consent core's documented "hash-then-load on one read" invariant (`LuaScriptConsent.h` L33-35). A file swapped between the consent read and the loader read runs **unapproved content**. Fix: return the hashed bytes from the consent check and hand those exact bytes to the Lua loader (sol load/script from the in-memory buffer) so check and execution operate on the same bytes. _(Adversary-required hardening gap → backlog per the reviewer's userVisible:false; elevate to an Issue if local-FS-race is in the threat model.)_
- **#1815 (b656738d) · `docs/plans/active/n4-trackeractions-interface.md:21`** — broken cross-refs: cites `docs/plans/appcontroller-fan-in-phase5-facets.md` and `…-phase6-dissolution.md`, but both live at `docs/plans/shipped/…` (recurs at :143 and :162). Bare inline code-spans, so the ref-integrity gate doesn't catch them. Fix: add the `shipped/` prefix to all three.

### LOW (3)
- **#1834 (c0fd3ac4) · `Source/Core/src/Diagnostics/EngineContextFormat.cpp:76`** — `AppendPieState` / `AppendSelectedActors` (:100-101) call nlohmann `value()` on nested keys without a type check; a mistyped nested field (`{"pie":{"active":"yes"}}`, actor `label:42`) throws `type_error.302`, contradicting the header's "every helper tolerates mistyped keys by skipping the field" claim. The `LaunchBackgroundTask` firewall contains the throw (no crash) but the entire prefill is dropped rather than the offending field. Fix: `is_boolean()`/`is_string()` guards matching the `StrField` pattern; add a nested-mistype test.
- **#1831 (fa21e318) · `Source/Core/src/AttachmentAppUpdateService.cpp:325`** — comment (:304-308) claims a revoked-but-cached cert still fails, but the code sets `fdwRevocationChecks = WTD_REVOKE_NONE`, disabling ALL revocation checking; `WTD_CACHE_ONLY_URL_RETRIEVAL` is a no-op unless revocation is enabled, so a known-revoked cert chaining to a trusted root would pass — weaker than documented. Fix: set `WTD_REVOKE_WHOLECHAIN` (keeping cache-only for offline) to match intent, or correct the comment.
- **#1868 (1778bd0e) · `docs/plans/deferred/subagent-eval-flywheel.md:5`** — status line cites `docs/plans/subagent-eval-agentic-coverage.md`; the file lives at `docs/plans/active/…` (every other ref uses the `active/` prefix). Fix: add `active/`.

**Clean (65, surviving lines reviewed, no findings):** #1876, #1875, #1873, #1872, #1871, #1869, #1867, #1865, #1864, #1863, #1862, #1861, #1860, #1859, #1858, #1857, #1855, #1853, #1852, #1851, #1849, #1848, #1847, #1846, #1845, #1844, #1843, #1842, #1841, #1839, #1838, #1837, #1836, #1835, #1833, #1832, #1830, #1829, #1828, #1827, #1826, #1825, #1824, #1823, #1822, #1821, #1819, #1818, #1817, #1816, #1814, #1813, #1812, #1811, #1810, #1809, #1807, #1806, #1805, #1804, #1803, #1801, #1800, #1799, #1798.

**Fully superseded (2, no review surface):** #1820, #1796 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 18 — #1795–#1738 (55-PR sweep, 2026-07-11)

Coverage: **55 reviewed — 8 with findings, 45 clean, 2 fully superseded, 0 errored, 0 died.** Net: **0 CRITICAL, 0 HIGH, 5 MEDIUM, 3 LOW.** Incremental on top of the Batch 17 frontier: everything merged after #1737 up to develop @ `1f4b2da4` (includes the fail-open remediation campaign PRs #1768–#1776 and this session's own #1738–#1740). **The frontier is now #1–#1795 contiguous; the next sweep resumes from #1796.** Survivor-filtered against origin/develop. (Same sha-resolved workflow + reviewer model as Batches 13–17; 55/55 returned, 0 died; ~17 min, ~1.76M tokens.) **One MEDIUM (#1742) is `userVisible:true` (data-loss) → GitHub Issue #1797 (ADR-0014); the other 7 findings are internal → backlog only.** Two notable linkages: #1756 is a **live recurrence of Batch 16's #1221** (the Lua ai.prompt in-flight-slot exception-leak — the god-file split relocated the code to `AppController_LuaBindings_Ai.cpp` and #1221's fix was never applied); #1774's `test-all.sh` `CI_SKIP_RE` finding is the anchoring weakness first flagged as Batch 13's #1692, now escape-marked but still latent.

> **Remediation (2026-07-12) — the 3 product-code findings, fixed:** **#1742** (data-loss) →
> `SaveAutomationScriptContent` now routes through `ConfigManager::AtomicWriteTextFile` (temp+rename;
> the trunc-then-unchecked-write can no longer destroy `Automation.lua` on an I/O error) — closes
> **Issue #1797**. **#1756** (Lua lockout) → `LuaAiPromptGlue` releases the in-flight slot via an RAII
> guard on every exit path, not just the happy path (the relocated #1221). **#1785** (false-Ok) →
> `ClassifyRejectedHttpStatus` promoted to the shared `Tracker/TrackerError.h`; the Jira mutation
> branches + Plane project-resolve now share one 2xx-guard (new doctest in `Tracker2xxErrorGuard.test.cpp`).
> Verified: unit tests 50 cases / 165 assertions green; strict-zone lint + dup + include-cycle clean.
> The 5 remaining findings (#1789 vacuous test, #1774 / #1760 / #1752 gate fail-opens, #1795 doc) are
> follow-up PRs.

### MEDIUM (5)
- **#1742 (1461bee3) · `Source/Core/src/AppController_LuaScriptFiles.cpp:170`** — **user-visible data-loss**: `SaveAutomationScriptContent` opens with `std::ios::trunc` (destroying `Automation.lua` up front), writes `ofs << content`, then `return true` with no stream-state check or flush. An I/O error (disk full/quota/device) after open silently loses the script while the caller is told the save succeeded. Fix: `ofs.flush(); if (!ofs.good()) { outError=…; return false; }` — ideally write-temp + atomic-rename. → **Issue #1797**.
- **#1756 (cc9bbad0) · `Source/Core/src/AppController_LuaBindings_Ai.cpp:143`** — `LuaAiPromptGlue` claims the in-flight slot (`TryBeginLuaAiPromptTurn`) and releases it (`EndLuaAiPromptTurn`, :167) only on the success path; an exception from `AddAiContext`/`PromptAi`/the context loop unwinds through the sol2 boundary and skips the release, so `aiPromptInFlight_` stays true forever and every later `ai.prompt` is permanently rejected as re-entrant. **Same defect as #1221 (Batch 16), relocated by the god-file split and still unfixed.** Fix: RAII scope-exit guard around the slot.
- **#1789 (e7fdb36c) · `tests/ui/mobile_view_quick_switcher.test.cpp:160`** — `TabTap_CleanSwitchDoesNotPrompt` asserts only the negative (`!viewsShowDiscardConfirm`) and never confirms the tap actually switched the active view, so a silently-dropped/no-op tap passes vacuously (the bucket-E-vacuous-green class). Fix: capture the active-view id before the tap and `IM_CHECK` it changed afterward, alongside the no-modal check.
- **#1774 (2fbb3922) · `scripts/dev/test-all.sh:155`** — `CI_SKIP_RE` matched unanchored (`[[ $script =~ $CI_SKIP_RE ]]`), so a future suite whose basename embeds a denylist token (`test-docs`, `test-doctor`) is silently skipped in CI while the gate passes (fail-open, shape F). No live collision today; escape-marked as reviewed by #1774 but the anchoring weakness (first flagged Batch 13 #1692) remains. Fix: anchor each token to a full basename (`^(…)$`).
- **#1760 (f1ef7437) · `agents/scripts/core/comment_audit.py:121`** — `_deviation_continuation_lines` closes a wrapped-deviation block only when paren depth `<= 0`; free-form `reason=` prose with an unbalanced `(` leaves depth `> 0` forever, so `in_block` never resets and every subsequent `//`/`*` line in the file is exempted from the comment-noise gate — a fail-open that silently exempts real commented-out code. Fix: bound the span (stop at the first non-comment line, or cap continuation lines) rather than relying on paren balance.

### LOW (3)
- **#1795 (1f4b2da4) · `docs/self-improvement/categories/test.md:46`** — coverage-reconcile claim states `DrawJqlQueryEditorEmbedded` (#767) is covered by "omnibar_search_apply.test.cpp + views_field_selection.test.cpp"; the latter has no JQL reference at all (it covers the Views-editor field-selection reseed). Fix: drop `views_field_selection.test.cpp` from the JQL mapping (conclusion still holds via the single real test).
- **#1785 (9c7956b8) · `Source/Core/src/Tracker/PlaneClient.cpp:112`** — `ResolvePlaneProject`'s non-200 branch classifies via raw `TrackerErrorFromHttpStatus` without the 2xx guard this same PR added elsewhere, so a 201/204/206 maps to `Ok()` on a return-false path. Benign today (every caller re-wraps and `Ok().IsRetryable()` is false). Fix: route through the guarded `ClassifyRejectedHttpStatus` for consistency.
- **#1752 (09d9248c) · `agents/scripts/core/develop-tip-required-green.sh:114`** — the detector relies on "newest last wins" but `gh api …/check-runs --paginate` doesn't guarantee chronological row order for re-run checks; a newest-first response keys off a stale run and can miss a terminal-red or emit a stale red. Non-blocking (advisory nudge). Fix: sort rows by `started_at`/`completed_at` (or select max-per-name explicitly) instead of depending on API order.

**Clean (45, surviving lines reviewed, no findings):** #1794, #1793, #1792, #1791, #1790, #1788, #1787, #1786, #1784, #1783, #1782, #1781, #1780, #1779, #1778, #1776, #1775, #1773, #1772, #1771, #1769, #1768, #1767, #1766, #1765, #1764, #1763, #1762, #1761, #1759, #1758, #1757, #1755, #1754, #1751, #1749, #1748, #1746, #1745, #1744, #1743, #1741, #1740, #1739, #1738.

**Fully superseded (2, no review surface):** #1753, #1750 — every introduced line was changed/removed by a later PR; excluded by construction.

> **Remediation (2026-07-12) — the 4 gate/test/doc findings, fixed:** **#1774** → `test-all.sh`'s
> CI-lane denylist now matches the EXACT basename (`^name$`) instead of an unanchored substring, so a
> future `test-docs-foo.sh` isn't silently skipped by the `test-docs` token (and the pure-logic
> `test-plan-index-robustness-bats.sh`, collateral of the old prefix, now runs — verified 6/6 local +
> self-validated by this PR's `test-all --ci` lane); `fail-open-ok` marker dropped. **#1760** →
> `comment_audit.py`'s `_deviation_continuation_lines` ends the wrapped-deviation span at the first
> non-comment line, so an unbalanced `(` in reason prose no longer leaks the noise-gate exemption onto
> every later comment (new `--selftest` deviation-wrap case). **#1752** → `develop-tip-required-green.sh`
> sorts check-runs by `started_at` so the detector's "newest last" holds regardless of `--paginate`
> order. **#1795** → `test.md` drops the bogus `views_field_selection.test.cpp` co-citation from the
> `DrawJqlQueryEditorEmbedded` JQL mapping (0 JQL refs; `omnibar_search_apply.test.cpp` is the real
> cover). Remaining Batch-18 finding: **#1789** (vacuous bucket-E test) — own follow-up PR.

## Batch 17 — #1737–#1696 (36-PR sweep, 2026-07-10)

Coverage: **36 reviewed — 4 with findings, 32 clean, 0 fully superseded, 0 errored, 0 died.** Net: **0 CRITICAL, 0 HIGH, 1 MEDIUM, 3 LOW.** Same-day incremental on top of the Batches 13–16 frontier: everything merged after #1695 up to develop @ `2877512f` — **the frontier is now #1–#1737 contiguous; the next sweep resumes from #1738.** Survivor-filtered against origin/develop (0 superseded — the PRs are hours old, so essentially every introduced line is alive; includes the sweep's own #1700). (Same sha-resolved workflow + reviewer model as Batches 13–16; 36/36 returned, 0 died; ~13 min, ~1.3M tokens.) **All 4 findings are `userVisible:false` → NO GitHub Issues; backlog only per ADR-0014.** The MEDIUM is another fail-open gate variant — cross-filed onto the REOPENED `fail-open-meta-gate-authoring-check` in [`categories/tooling.md`](categories/tooling.md) as sub-shape (I): a mutation gate whose scored set can silently drain to zero.

### MEDIUM
- **#1698 (e87c5b78) · `scripts/dev/mutation-smoke.sh:219`** — fail-open: scoring counts only `expect=killed` guards; FILE-MISSING/SPEC-ERROR/BUILD_FAIL outcomes silently `continue` without gating. If ALL guard mutants land in those buckets (full corpus rot after a production refactor, or a systemic build break), `scored=0` and the gate exits 0 — a green run that verified zero assertion strength, contradicting the plan's "fails loud, never silently skips-as-pass" claim. Muted today by continue-on-error, but Phase 4 graduates it to blocking. Fix: under `--gate`, exit 1 when `scored==0` (and/or fold the error buckets into a non-zero exit).

### LOW (3)
- **#1732 (2877512f) · `Source/Core/src/Ui/AdfToMarkdown.cpp:505`** — `EmitAdfCodeBlock` materializes the ENTIRE accumulated output (`const std::string current = s.out.str()`) on every code block just to inspect `current.back()` — O(blocks × total-output) quadratic conversion cost. Fix: track a `lastCh` on `AdfWalkState` (or peek the streambuf) instead of copying the growing buffer.
- **#1728 (2204ce60) · `docs/plans/shipped/god-file-splits.md:24`** (was `active/` at the time of the finding; archived to `shipped/` on campaign completion) — sibling-plan cross-refs use `../<slug>.md` (also :92/:133/:152/:153), which resolves to `docs/plans/` — but all three targets lived in `docs/plans/active/`; every link is broken. Fix: same-directory `<slug>.md` targets.
- **#1722 (dc1b693b) · `docs/plans/shipped/appcontroller-fan-in-phase6-dissolution.md:5`** — cites `docs/plans/appcontroller-fan-in-phase5-facets.md`; the Phase 5 plan lives at `docs/plans/shipped/…` (INDEX.md links the shipped/ copy). Fix: add `shipped/`.

**Clean (32, surviving lines reviewed, no findings):** #1737, #1736, #1735, #1734, #1733, #1731, #1730, #1729, #1727, #1726, #1725, #1724, #1723, #1721, #1720, #1719, #1718, #1717, #1716, #1715, #1714, #1710, #1709, #1708, #1707, #1705, #1704, #1702, #1701, #1700, #1697, #1696.

## Batch 16 — #1304–#1175 + the #1593 special (117-PR sweep, 2026-07-10) — FRONTIER COMPLETE

Coverage: **116 reviewed — 14 with findings, 96 clean, 6 fully superseded, 0 errored, 0 died** — plus the **#1593 true-merge special** (below). Net: **0 CRITICAL, 1 HIGH, 9 MEDIUM, 7 LOW.** Final installment of the post-#1174 frontier re-establishment: with Batches 13–16 + Batch 12 + the #1593 special, **every merged PR #1–#1695 is now survivor-reviewed — SWEEP COMPLETE, no gaps.** Survivor-filtered against origin/develop, so every finding is current. (Same sha-resolved workflow + reviewer model as Batches 13–15; 116/116 returned, 0 died; ~44 min, ~3.87M tokens.) **Three findings are `userVisible:true` → GitHub Issues per ADR-0014: the HIGH #1233 → Issue #1711, #1222 → Issue #1712, #1217 → Issue #1713; the other 14 are internal CI/gates/docs → backlog only.** Recurring themes: the zero-test fail-open driver cluster AGAIN (#1181 ×2, #1175, plus the in-workflow #1281 0/0-envelope variant — same class as Batch 15's five), a coverage detect self-gate that fails open on git error (#1206), and two exception-safety/generation-token state bugs in product code (#1221, #1217).

**#1593 special (true merge commit `368e1841`, 9 constituent commits):** reviewed separately — blame attributes its survivors to the 9 branch commits, not the merge sha, so the survivor extractor ran per-constituent. **Clean** — 8 of 9 commits had survivors (~1,167 lines across the CPP_CODE_AUDIT remediation: overflow-checked parsing, SSRF hardening, ParseBounded, per-turn AI cancel tokens, RAII latch resets, editor truncation guards, coverage-delta SIGPIPE fix); `f92255f0` fully superseded; no findings.

### HIGH
- **#1233 (8775fc35) · `Source/Core/src/SubprocessCapturePure.cpp:170`** — **user-visible**: `IsSensitiveEnvName` substring-matches the `"_PAT"` token, which also matches every `*_PATH` name — `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`/`GIT_EXEC_PATH`/`PKG_CONFIG_PATH` are scrubbed from children. `P4Annotate.cpp` sets `scrubSensitiveEnv=true`, so a p4/git child on Linux/macOS can fail to load shared libs — contradicting the documented PATH-family survival guarantee. Tests cover only `PATH`/`GIT_DIR`, so the regression is uncovered. Fix: suffix-match `_PAT` (endsWith), add `CHECK_FALSE` cases for `LD_LIBRARY_PATH`/`GIT_EXEC_PATH`. → **Issue #1711**.

### MEDIUM (9)
- **#1281 (b3f31f18) · `.github/workflows/build-and-test.yml:1139`** — the childlog-extraction `sed 's/.*child stdout\/stderr[^A-Za-z]*//'` also consumes the leading `/` of the absolute POSIX temp path, leaving a relative path that never exists → the per-test ImGui failure reasons are NEVER captured on the Linux bucket-E lane (the :613 twin works only because Windows paths start with a drive letter). Fix: exclude `/` from the consumed class.
- **#1281 (b3f31f18) · `.github/workflows/build-and-test.yml:1177`** — a well-formed envelope with `passed=0 failed=0` stays `status=ok`, breaks the retry loop, and the lane passes green with zero tests executed under `--all`. Fix: treat `passed==0` as `status=broken`.
- **#1223 (facbc2ca) · `docs/agent-rules/build.md:68`** — doc-vs-code drift on a gated preset literal: claims `ninja-msvc-asan` "does not enable tests" (+ manual `-DSMATCHET_BUILD_TESTS=ON` step), but CMakePresets.json:136 bakes `SMATCHET_BUILD_TESTS: ON` into the preset. Fix: rewrite to the preset's actual default.
- **#1222 (6b6ea431) · `Source/Core/src/Ui/P4vLaunch.cpp:183`** — **user-visible**: the custom-command injection guard rejects embedded quotes in `{file}`/`{cl}` but not a TRAILING BACKSLASH — a value ending `\` escapes the template's closing wrap quote, un-terminating the argument and shifting the p4vc boundary (the exact sibling vector this PR fixed in the helper). Fix: reject/sanitize trailing `\` on the raw-template path. → **Issue #1712**.
- **#1221 (85d7e785) · `Source/Core/src/AppController_LuaBindings.cpp:602`** — `EndLuaAiPromptTurn()` (clears `aiPromptInFlight_`) is happy-path-only; an exception from `LuaTableToAiContextBlock`/`AddAiContext`/`PromptAi` unwinds past it, the flag stays set, and every later `ai.prompt` is rejected as re-entrant — permanent feature lockout after one error. Fix: RAII scope guard (or try/catch + release + rethrow).
- **#1217 (3c10e1d1) · `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp:196`** — **user-visible**: `OpenCommentsModal` resets the state struct (Gen→0) then `++Gen`, so the documented monotonic stale-post-back generation token is always 1 — inert. A slow open-fetch landing after a post-triggered re-fetch of the same issue overwrites fresher results, momentarily dropping a just-posted comment. Fix: keep the counter out of the reset (static monotonic counter). → **Issue #1713**.
- **#1206 (f8e746d1) · `.github/workflows/coverage.yml:96`** — the detect self-gate computes `changed` via `git diff … || true`; any git failure yields empty `changed` → `run=false` → the REQUIRED coverage context fake-greens, contradicting the block's own "must NEVER fake-green" comment. Fix: on fetch/diff failure default `run=true`.
- **#1181 (5ba0c5af) · `scripts/dev/test-ui-annotate-before-cl-cue.sh:60`** — zero-test fail-open (the Batch-15 driver cluster again): `passed=0/failed=0` exits 0 green. Fix: `PASSED>=1` floor.
- **#1181 (5ba0c5af) · `scripts/dev/test-ui-toolbar-append-cache-cue.sh:60`** — same fail-open in the sibling driver. Fix: same floor.

### LOW (7)
- **#1261 (e6445a27) · `Source/Core/src/Ui/SmatchetOmnibarUi.cpp:120`** — comment claims the per-frame `ClassifyOmnibarInput` is "pure, allocation-free", but it constructs owning `std::string`s each call (`TrimCopyAsciiWhitespace`, `ToLowerAsciiCopy`) — per-frame copies while the omnibar is visible (bounded/SSO). Fix: fix the wording, or cache the classification on buffer change.
- **#1217 (3c10e1d1) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:853`** — backend-agnostic `FetchIssueComments` returns "Jira backend is not initialized." for any backend (GitHub/Plane); every sibling uses the generic "Tracker backend…". Fix: use the generic string.
- **#1202 (f14e5766) · `docs/guides/testing-surface.md:175`** — § 5.1 backlog line-pins drifted (verified: :175 → debt.md:65 now an unrelated race entry; :181 → applied.md:265 now a postmortem). Fix: re-anchor to headings, not line numbers.
- **#1198 (2ae42828) · `tests/ui/reset_layout_docking.test.cpp:76`** — raw `new` inside a `unique_ptr` ctor; house rule is `make_unique`. Test-only, no leak. Fix: `std::make_unique`.
- **#1186 (14c9c58e) · `tests/Core/UserInfoActivityCancelUaf.test.cpp:197`** — the "cancel signalled before worker starts" TEST_CASE never pre-cancels (its own comment concedes it can't); its assertions pass regardless — false confidence for the named behavior. Fix: drive the guard directly or rename to what it actually exercises.
- **#1183 (67098d12) · `docs/plans/shipped/mobile-ci-smoke-gate.md:103`** — implementation log says "8-frame state machine"/single-frame gap; shipped code is 10-frame with the 2-frame honesty-fix gap. Fix: update the log line.
- **#1175 (61bdf630) · `agents/scripts/core/test-pr-status-watch-bats.sh:48`** — the wrapper's own zero-run fail-open: zero `ok`/`not ok` lines with rc 0 → "Passed: 0 Failed: 0", exit 0. Fix: explicit 0/0 guard.

**Clean (96, surviving lines reviewed, no findings):** #1304, #1302, #1301, #1300, #1299, #1295, #1294, #1293, #1292, #1288, #1280, #1279, #1278, #1277, #1276, #1275, #1274, #1273, #1272, #1271, #1270, #1269, #1268, #1266, #1265, #1264, #1263, #1260, #1259, #1258, #1257, #1256, #1255, #1253, #1252, #1251, #1250, #1248, #1247, #1246, #1245, #1244, #1243, #1242, #1241, #1240, #1239, #1238, #1237, #1235, #1234, #1232, #1231, #1230, #1229, #1228, #1227, #1226, #1225, #1220, #1219, #1218, #1216, #1215, #1214, #1213, #1212, #1211, #1210, #1209, #1208, #1207, #1205, #1204, #1203, #1201, #1200, #1199, #1197, #1196, #1195, #1194, #1193, #1192, #1191, #1190, #1189, #1188, #1187, #1185, #1184, #1182, #1180, #1179, #1178, #1177.

**Fully superseded (6, no review surface):** #1303, #1267, #1262, #1249, #1224, #1176 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 15 — #1435–#1305 (120-PR sweep, 2026-07-10)

Coverage: **120 reviewed — 15 with findings, 90 clean, 15 fully superseded, 0 errored, 0 died.** Net: **0 CRITICAL, 2 HIGH, 6 MEDIUM, 9 LOW.** Third installment of the post-#1174 frontier re-establishment (contiguous #1435→#1305; frontier now **#1305–#1695** on top of the #1–#1174 baseline). Survivor-filtered against origin/develop, so every finding is current. (Same sha-resolved workflow + reviewer model as Batches 13–14; 120/120 returned, 0 died; ~31 min, ~3.55M tokens.) **All 17 findings are `userVisible:false` (internal CI/gates/hooks/docs) → NO GitHub Issues this pass; backlog only per ADR-0014.** Dominant theme, emphatically: the **zero-test fail-open UI-test-driver cluster** — BOTH HIGHs and 3 MEDIUMs are the same `passed=0&&failed=0`→exit-0 shape across five per-feature `test-ui-*.sh` drivers (#1384, #1364, #1405, #1383, #1372), the exact class Batches 10–11 flagged in older drivers — the template keeps being copied. Plus a new sub-shape: `set -e`+`pipefail` aborting a step before its own graceful-degradation branch (#1424 ×2). Cross-filed onto the OPEN P2 `fail-open-meta-gate-authoring-check` in [`categories/tooling.md`](categories/tooling.md).

### HIGH (2)
- **#1384 (d0c80161) · `scripts/dev/test-ui-attachment-thumbnail-loading-cue.sh:62`** — fail-open zero-test gate: after the `SMATCHET_BUILD_UI_TESTS=OFF` and `"?"` guards, the driver exits 0 whenever `FAILED=="0"` — including `passed=0/failed=0` when the `ThumbnailLoadingCue` filter matches nothing (registry entry dropped/renamed). The gate cannot catch regression of its own registration. Fix: fail on `PASSED=0 && FAILED=0` before the exit-0 path.
- **#1364 (addedce1) · `scripts/dev/test-ui-callstack-tooltip-hover.sh:82`** — same class: `passed=0 failed=0` passes the `"?"` check at :77, fails the `FAILED!="0"` check at :83, prints `Passed: 0 Failed: 0`, exits 0 — a green gate that ran no assertions. Fix: zero-run guard before the FAILED check.

### MEDIUM (6)
- **#1424 (0d2744d8) · `.github/workflows/build-and-test.yml:653`** — `result=$(bash scripts/dev/perf-run.sh … | tail -1)` runs under `bash -e` + the step's `set -uo pipefail`; a non-zero perf-run.sh fails the assignment and `set -e` aborts the step BEFORE the graceful `[ -f "$result" ]` handling and the explicitly-anticipated "first-run plumbing" warning at :661-663 — a tolerated first-run hiccup becomes a hard-red ARM64 job. Fix: `|| true` the assignment (or `set +e` bracket) so control reaches the intended `::warning::` path.
- **#1405 (b29620bb) · `scripts/dev/test-ui-omnibar-search-apply.sh:86`** — zero-test fail-open: only `"?"` and `FAILED!=0` are rejected; an "Omnibar" filter matching zero tests exits 0 green. Fix: zero-run floor (or assert `PASSED` == expected 3).
- **#1383 (0772f822) · `scripts/dev/test-ui-command-palette-inline-typing.sh:58`** — same class for the "CommandPalette" filter. Fix: zero-run guard before the `FAILED!=0` check.
- **#1372 (4a06fb20) · `scripts/dev/test-ui-ai-assistant-model-change.sh:69`** — same class for `--name=AssistantModelChange`. Fix: zero-run guard.
- **#1388 (9346fcf1) · `docs/harness/claude-code/hooks/guard-shared-tree.sh:96`** — the detection `gitopts` matches `-c` values with `-c[[:space:]]+[^[:space:]]+`, truncating at the first space inside a quoted value — `git -c user.name="a b" reset --hard` on the shared tree skips the :97 detection grep and the guard fails OPEN (early exit 0). The sibling `guard-head-drift.sh:99` was deliberately made quote-aware for exactly this. Fix: mirror it — `-c[[:space:]]+("[^"]*"|[^[:space:]]+)`.
- **#1348 (9c5a8608) · `agents/scripts/core/merge-watcher-stuck-nudge.sh:83`** — the default `--list` output prints a literal em-dash, contradicting the PR's own ASCII-only deviation note; on a cp1252 Windows console the inline python raises `UnicodeEncodeError`, and being the last command under `set -euo pipefail` the script exits non-zero — violating its "Exit 0 always / degrade silent" contract exactly when a PR is actually stuck. Fix: ASCII ` -- ` like the `--nudge` branch.

### LOW (9)
- **#1424 (0d2744d8) · `.github/workflows/build-and-test.yml:613`** — same errexit sub-shape as the MEDIUM: `childlog="$(… | grep -a 'child stdout/stderr' | …)"` under `set -e`+pipefail; a no-match grep aborts the step before the graceful "no spawned-child log path found" branch — a booted-fine run (rc=0) goes spuriously red. Fix: `|| true` the assignment.
- **#1420 (d0418faa) · `agents/scripts/core/test-shell-lint.sh:308`** — `check_pipefail_head` only recognizes plain/`local`/`export` assignment prefixes; `readonly`/`declare`/`typeset x=$(…|head)` under pipefail is silently not flagged — a miss of the exact class the rule guards. Fix: extend the alternation.
- **#1417 (51facafc) · `agents/scripts/core/workflow-watchdog.sh:121`** — amplification floor uses `-lt 100`, so an explicit `--amplification-pct 100` fires cascade on a healthy fleet where runs == expected. Dormant (non-default). Fix: `-le 100` or `-gt` comparison, or document.
- **#1412 (9438a896) · `docs/adr/0022-intent-gate-promotion.md:3`** — stale line-pin: cites `merge-gates.md:83` for the merge-queue deadlock class; that text is now at :102. Fix: re-pin or cite the section by name.
- **#1395 (8ffe3035) · `.github/workflows/perf-full.yml:113`** — comment cites a `security.md "Mesa archive integrity"` entry that exists nowhere in the repo (duplicated in `perf-pr-fast.yml:228`). Fix: add the entry or repoint both copies.
- **#1388 (9346fcf1) · `docs/harness/claude-code/hooks/guard-shared-tree.sh:97`** — the stash-pop detection branch lacks the `${gitopts}` segment its own :129 enumeration includes, so `git -C <path> stash pop` is never detected — fail-open for that form. Fix: insert `${gitopts}`.
- **#1381 (0abb10cb) · `docs/self-improvement/categories/security.md:69`** — stale line-pin: "blocking synchronous tracker call at `JiraIssueMutation.cpp:206`" now points at a closing brace; the calls are at ~:188/:216. Fix: re-pin or use function names.
- **#1352 (af3e6e24) · `tests/Core/LocalCacheManagerCorruption.test.cpp:5`** — header comment describes the abandoned v1 design (pre-open probe via a `QuarantineIfCorrupt` that exists nowhere; grep: 0 matches); shipped design is catch-in-ctor-body + `RebuildFreshAfterCorruption`/`QuarantinePath`. Fix: reword to the shipped mechanism.
- **#1340 (8458aa1d) · `docs/self-improvement/categories/security.md:77`** — stale line-pin: `CommandRegistry.cpp:302-307` for `IsAutomationSource`/`RequiresExplicitConfirm`; now :332/:337. Fix: re-pin.

**Clean (90, surviving lines reviewed, no findings):** #1435, #1433, #1432, #1430, #1429, #1428, #1427, #1426, #1425, #1423, #1422, #1421, #1416, #1415, #1414, #1413, #1411, #1410, #1409, #1408, #1407, #1406, #1404, #1403, #1401, #1400, #1398, #1397, #1396, #1394, #1393, #1391, #1389, #1387, #1386, #1385, #1382, #1380, #1376, #1375, #1374, #1373, #1371, #1370, #1367, #1366, #1365, #1363, #1361, #1360, #1359, #1358, #1357, #1356, #1355, #1354, #1353, #1350, #1349, #1347, #1346, #1345, #1344, #1343, #1342, #1341, #1339, #1338, #1337, #1336, #1335, #1334, #1333, #1331, #1330, #1329, #1328, #1327, #1326, #1325, #1324, #1323, #1322, #1321, #1320, #1319, #1315, #1314, #1313, #1305.

**Fully superseded (15, no review surface):** #1434, #1431, #1419, #1418, #1402, #1399, #1392, #1390, #1379, #1378, #1377, #1369, #1368, #1362, #1332 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 14 — #1559–#1436 (120-PR sweep, 2026-07-10)

Coverage: **120 reviewed — 13 with findings, 104 clean, 3 fully superseded, 0 errored, 0 died.** Net: **0 CRITICAL, 0 HIGH, 5 MEDIUM, 11 LOW.** Second installment of the post-#1174 frontier re-establishment (contiguous #1559→#1436; frontier now **#1436–#1695** on top of the #1–#1174 baseline). Survivor-filtered against origin/develop, so every finding is current. (Same sha-resolved workflow + reviewer model as Batch 13; 120/120 returned, 0 died; ~47 min, ~4.36M tokens.) **One MEDIUM (#1554) is `userVisible:true` → GitHub Issue #1706 (ADR-0014); the other 15 findings are internal tooling/gates/docs → backlog only.** Dominant theme yet again: the **fail-open / no-op gate cluster** — 4 of 5 MEDIUMs are gates that can't catch (or can be talked out of) the regression they exist for (#1525 unanchored `cr-disposition` attestation, #1511 self-contradictory staleness predicate, #1505 unanchored broken-lane suppression, plus the #1523/#1522/#1520 LOW variants). Cross-filed onto the OPEN P2 `fail-open-meta-gate-authoring-check` in [`categories/tooling.md`](categories/tooling.md).

### MEDIUM (5)
- **#1554 (55eddeb7) · `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:161`** — **user-visible**: window-close reset clears `assistantPrefsWorkingSeeded` (working copy correctly re-seeds from cfg on reopen) but never resets the function-static buffer seed flags nor requests `assistantPrefsForceBufferReseed`. Close-without-Save then reopen → InputText fields still show the DROPPED edit text (present in neither working copy nor cfg, no dirty `*`); re-editing flows the stale text back into the working copy. Fix: in `resetPreferencesWindowState` also set `d.assistantPrefsForceBufferReseed = true`. → **Issue #1706**.
- **#1525 (7df25fa3) · `agents/scripts/core/merge-gates.sh:542`** — the mandatory `cr-disposition:` attestation for a `cr-out-of-band` override is an unanchored, case-insensitive substring test over the whole PR body; a body that merely QUOTES the mechanism (pasted template/docs containing `cr-disposition:<reason>`) satisfies it, so the override bypasses the CR gate with no genuine recorded reason — the exact fail-open PR-3 closed. Fix: anchor per-line (`(?m)^\s*[-*]?\s*cr-disposition:\s*…`) and reject the literal `<reason>` placeholder.
- **#1511 (9c98b7ae) · `agents/scripts/project/test-plan-staleness.sh:63`** — the staleness gate is a near-no-op: `is_stale` needs `cited_prs` non-empty AND all post-ship sections still stubs, but `cited_prs` is read ONLY from § Implementation log, which the stub predicate requires to be empty — the two conditions are mutually near-exclusive, so the "shipped but never written up" drift it was built for can never fire. Fix: source cited PRs from a section that persists (or drop Impl-log from the stub set so a populated log + stub Deviations/Verification flags).
- **#1505 (a8bb69c7) · `agents/scripts/core/postmortem-owed.sh:202`** — `is_broken_lane` matches a configured broken-lane token against red check-names with an unanchored `grep -qiF`; a broad token ("Coverage") also suppresses a genuinely-red sibling ("Coverage-Integration"), laundering a real gate escape into a no-postmortem WARN. Dormant (registry starts empty) but fail-open once configured. Fix: anchored whole-name equality.
- **#1466 (ff1f18e6) · `docs/mobile/CHROMEBOOK_PORT_INVESTIGATION.md:203`** — broken path cross-ref: cites `Source/Core/include/Ui/SmatchetUiModeIds.h`; the header lives at `Source/Core/include/SmatchetUiModeIds.h` (no `Ui/`). Fix: drop the segment.

### LOW (11)
- **#1556 (572a952d) · `scripts/git-hooks/pre-push:221`** — header promises FAIL-OPEN on infra/tool uncertainty, but sections (D)#2 doc-checks and (D)#4 shell-lint treat ANY non-zero exit as a violation (fail-CLOSED) — an internal infra error wedges an unrelated push. Fix: split real-violation rc from infra rc as (D)#1 already does.
- **#1523 (281d16dc) · `scripts/dev/perf-baseline.sh:187`** — orphan-baseline gate fails open: if the (very specific) `Name() const override` regex matches nothing, `registered` is empty and the check exit-0s with a WARN; unparseable baseline JSON is likewise silently skipped. Fix: fail-closed on empty `registered` and on parse failure.
- **#1522 (81de3e07) · `agents/scripts/core/test-shell-lint.sh:330`** — NUL-byte guard uses GNU-only `grep -qaP '\x00'` with stderr swallowed; on BSD grep (macOS is a documented dev platform) the rule silently no-ops. Fix: `tr -d '\000' | cmp -s` probe, or gate on PCRE support; at minimum stop swallowing stderr.
- **#1520 (e807690c) · `agents/scripts/core/test-bats-ascii-names.sh:74`** — same class: `grep -P` failure hidden by `2>/dev/null` → zero matches → green PASS having scanned nothing. Fix: probe `-P` support up front and return 2, or check grep's exit status.
- **#1507 (f50b28be) · `scripts/dev/agent-eval-calibrate.py:301`** — `emit_markdown` re-parses formatted violation strings with `split(" / ")`, mis-parsing the committed label `"cr-dpapi-secret-loss / strong run"`; the per-row verdict prints "ok" for an over-threshold pair (BLOCK banner + exit code stay correct — misleading report only). Fix: return structured (label, dimension) violations instead of re-parsing.
- **#1506 (f5cd3234) · `agents/scripts/core/test-lint-hook-split.sh:229`** — with `nullglob`, an empty `QUEUE_REAL` array makes `cat "${QUEUE_REAL[@]}"` read stdin — potential hang on the failure path when stdin isn't redirected. Fix: guard on array non-emptiness or add `</dev/null`.
- **#1469 (d849b142) · `Source/Core/src/FieldEditPipelineService.cpp:125`** — when `backend->Mutations()` is null, `TryBuildFieldEditPayloadForNetwork` returns false with `outError` never set — callers (e.g. `TryPrepareOfflineFieldEdit`) surface an empty error string. Fix: set a "backend does not support issue mutations" error before the return.
- **#1466 (ff1f18e6) · `docs/mobile/CHROMEBOOK_PORT_INVESTIGATION.md:68`** — stale line-pin: `android:allowBackup="false"` cited at manifest line 7; the A8 touchscreen block shifted it to :19. Fix: re-pin or drop the number.
- **#1463 (f2100d44) · `.github/workflows/lock-cleanup.yml:13`** — comment cites `docs/plans/plan-lock-enforcement.md`, archived to `docs/plans/shipped/` by this very PR (line 12 already uses the shipped/ prefix for the sibling doc). Fix: add `shipped/`.
- **#1463 (f2100d44) · `agents/scripts/core/lock-table-cache.sh:31`** — same stale ref to the pre-archival plan path. Fix: add `shipped/`.
- **#1463 (f2100d44) · `docs/harness/claude-code/hooks/guard-plan-lock.sh:28`** — same stale ref. Fix: add `shipped/`.

**Clean (104, surviving lines reviewed, no findings):** #1559, #1558, #1557, #1555, #1553, #1552, #1551, #1550, #1549, #1548, #1547, #1546, #1545, #1544, #1543, #1542, #1541, #1540, #1539, #1538, #1537, #1536, #1535, #1534, #1533, #1532, #1531, #1530, #1529, #1528, #1527, #1526, #1524, #1521, #1519, #1518, #1517, #1516, #1515, #1514, #1513, #1512, #1510, #1509, #1508, #1504, #1503, #1502, #1501, #1500, #1499, #1498, #1497, #1496, #1495, #1494, #1493, #1492, #1491, #1490, #1489, #1488, #1487, #1486, #1485, #1484, #1483, #1482, #1481, #1480, #1478, #1477, #1476, #1475, #1474, #1473, #1472, #1471, #1470, #1468, #1467, #1465, #1464, #1462, #1461, #1460, #1456, #1455, #1454, #1453, #1452, #1451, #1450, #1446, #1445, #1444, #1443, #1442, #1441, #1440, #1439, #1438, #1437, #1436.

**Fully superseded (3, no review surface):** #1449, #1448, #1447 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 13 — #1695–#1560 (120-PR sweep, 2026-07-10)

Coverage: **120 reviewed — 14 with findings, 99 clean, 7 fully superseded, 0 errored, 0 died.** Net: **1 CRITICAL, 1 HIGH, 1 MEDIUM, 11 LOW.** First installment of the post-#1174 frontier re-establishment (work-list = all 494 merged-to-develop PRs in (#1174, #1695] minus Batch 12's 17; this batch took the newest 120, contiguous #1695→#1560). Survivor-filtered against origin/develop (@ `b69e82f1`), so every finding is current — already-fixed/reverted code excluded by construction. (Reviewer model `code-review` opus/high via a sha-resolved variant of the persisted `historical-review-sweep` workflow — this remote session has no `gh` CLI, so each PR's squash commit was pre-resolved from the develop log and cross-validated against GitHub's merged-PR list, which also caught 4 PRs with edited/non-standard squash subjects the `(#N)`-suffix scrape misses: #1439/#1577/#1593/#1597. Concurrency 2 = the runtime cap min(16, cores−2) on this 4-core box, under the Opus ≤6 guardrail; 120/120 agents returned, 0 died; ~41 min, ~4.35M tokens.) **The CRITICAL (#1601) is `userVisible:true` → GitHub Issue #1699 (ADR-0014); the other 13 findings are internal tooling/docs → backlog only.** Recurring themes: the fail-open gate cluster once more (the HIGH is an unanchored CI denylist token; the MEDIUM is assertion-masking `rm -rf` tails in bats meta-tests), plus the usual stale-line-pin / moved-doc drift tail.

### CRITICAL
- **#1601 (b938ceb5) · `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:113`** — static-buffer overflow on a >8191-byte paste into the AI chat input. `InputBufferResizeCallback` records the dropped-byte count then returns 0 without resizing `Buf` OR resetting `data->BufSize`. But adding `ImGuiInputTextFlags_CallbackResize` (:1147) sets `is_resizable=true` in ImGui, which DISABLES the insert-time capacity clamp in `STB_TEXTEDIT_INSERTCHARS` (the pre-PR behavior the comment relies on) and lets ImGui grow its internal edit buffer to hold the full over-cap paste. At apply time ImGui calls the callback with `BufSize` enlarged to `needed+1`, then does `buf_size = callback_data.BufSize` (still enlarged) and `ImStrncpy(buf, apply_new_text, min(len+1, buf_size))` — copying the full paste into the fixed 8192-byte process-static `s_inputCharBuf`. The comment "ImGui then clamps the insert exactly as before" is false: that clamp only existed because the flag was absent. Memory corruption in exactly the scenario the truncation toast exists to report. Fix: in the `CallbackResize` branch also set `data->BufSize = kInputBufCap` (leave `Buf`/`BufTextLen`); verify against the vendored imgui `InputTextEx` apply-path; add an ASan bucket-E paste regression. → **Issue #1699**.

### HIGH
- **#1692 (681ad70b) · `scripts/dev/test-all.sh:99`** — fail-open CI denylist: `CI_SKIP_RE` uses unanchored substring tokens matched against the full script path (`[[ "$script" =~ $CI_SKIP_RE ]]` at :155). The token `test-plan-index` matches not only the intended `test-plan-index.sh` but also the distinct headless-safe Bucket-A suite `test-plan-index-robustness-bats.sh`, silently denylisting it in CI (`Passed: 0 Failed: 0 Skipped: 1`) — any failure in that suite is masked. Fix: anchor each token to a full basename (`test-plan-index\.sh`, or `(^|/)…\.sh` boundaries) and audit the other tokens for the same over-match.

### MEDIUM
- **#1605 (24fb4e71) · `tests/bats/lint_rules.bats:411`** — assertion-masking cleanup tails: the new `--scan-*` bats tests end with `rm -rf "$tmp"` AFTER their content assertions; bats doesn't run bodies under `set -e`, so the always-zero `rm -rf` becomes the test's exit status and masks a failing `[[ "$output" == … ]]` above it. A regression in the gate's `--root` scan path false-PASSes these meta-tests. Recurs at :322/:334/:346/:372/:423/:444/:458/:476. Fix: make the assertion the last command and use bats' auto-cleaned `$BATS_TEST_TMPDIR` (the `dup_audit.bats` house style), or move cleanup to `teardown()`.

### LOW (11)
- **#1680 (1878db71) · `agents/scripts/core/cost-ceiling-check.py:68`** — `int(row.get("input_tokens",0) or 0)` raises `ValueError` on a non-numeric token value; not caught by the surrounding `except OSError`, so one malformed row crashes the whole sum instead of degrading. Masked by the nudge wrapper's `|| true`, but `--blocking`/direct invocation tracebacks. Fix: wrap the two `int()` accumulations in `try/except (ValueError, TypeError): continue`, matching the existing skip-and-continue idiom.
- **#1674 (4ac9b7aa) · `docs/plans/shipped/appcontroller-fan-in-phase5-facets.md:5`** — status line cites the predecessor plan at `docs/plans/appcontroller-fan-in.md`; it now lives at `docs/plans/shipped/appcontroller-fan-in.md`. Fix: update the path.
- **#1654 (911d1b25) · `AGENTIC_INFRA_AUDIT.md:6`** — surviving audit-doc links still use the flat `docs/plans/<name>.md` layout from before the `active/`/`shipped/` reorg (lines 6, 7, 46, 120, 130, 131) — all 404. Fix: repoint to `active/agentic-infra-audit-campaign-2026-06.md`, `shipped/ai-control-policy.md`, `active/subagent-eval-agentic-coverage.md`, `shipped/mutation-testing-pilot.md`, `active/testing-surface-roadmap.md`.
- **#1640 (ff6c05d7) · `Source/Core/src/Tracker/TrackerHttpClient.cpp:39`** — the `TrackerErrorKind::None` branch hard-codes the diagnostic to `"HTTP 200"`, but the classifier maps any 2xx to `AuthenticatedReachable`; a 201/204 probe reports a misleading string. Fix: `out.Diagnostic = "HTTP " + std::to_string(classified.Status());` like the other branches.
- **#1623 (7f7807d5) · `.github/workflows/build-and-test.yml:1460`** — comment says "the 5-min step cap is the backstop" but the step cap two lines below is `timeout-minutes: 6` (inner bound is `timeout 300`). Fix: reword to 6-min (or align the cap to 5).
- **#1615 (f56694df) · `docs/guides/error-surface-inventory.md:19`** — attributes `RedactHttpBodyForLog` to `TrackerHttpUtils.cpp`, but this same PR moved it to `TrackerHttpPure.cpp` (:209); only `RedactUrlForLog` remains. Fix: update the attribution.
- **#1614 (03acf256) · `Source/Core/src/Ui/SmatchetIconPickerUi.cpp:27`** — comment claims "OpenPopup is not wrapped" to justify the `###` stable-ID, but this same PR added a wrapping `OpenPopup(const char*)` overload and the file's `#define ImGui SmatchetLocalizedImGui` alias routes :29 through it — the premise is false (behavior survives only because `###` hashes just the suffix). Fix: rewrite the comment to describe the actual wrapper + `###` contract.
- **#1594 (839f55a9) · `TEST_COVERAGE_GAP_MAP.md:10`** — stale line-pin: cites `tests/CMakeLists.txt:272` for the "each test TU lists the production sources it exercises" contract; at HEAD that comment is at :306. Fix: re-pin or de-pin to the filename (the quoted text is unique).
- **#1588 (41bc9cda) · `scripts/dev/osv-scan.py:171`** — the OSV scan only queries commit-pinned components (`scannable = [c for c in comps if c["commit"]]`); non-commit deps in the SBOM (lua 5.3.6 — known historical CVEs — and fontawesome) are silently excluded, so under the now-blocking `--fail-on HIGH` gate a HIGH Lua CVE never trips the check. Fix: also query OSV by version/ecosystem (or PURL) for non-commit deps, or at minimum emit a `::warning` per unscanned dep.
- **#1575 (fc8b9945) · `docs/self-improvement/postmortems.md:56`** — the "Filed as" cross-ref points at `categories/infra/2026-06-27-perf-pr-fast-not-required-cancelled-escape.md`, which does not exist anywhere in the repo (the infra dir holds only the 2026-07-05 texture-guard doc). Fix: create the referenced doc or repoint the link.
- **#1567 (aeb3bd52) · `docs/plans/active/tsan-imgui-linked-target.md:15`** — stale line-pin: cites `SmatchetUI.cpp:66` for the `UiDrawSession g_ui;` definition; at develop it is :71. Fix: re-pin or use a symbol reference.

**Clean (99, surviving lines reviewed, no findings):** #1695, #1694, #1691, #1690, #1689, #1688, #1687, #1686, #1685, #1684, #1683, #1682, #1681, #1679, #1677, #1676, #1675, #1673, #1670, #1668, #1665, #1663, #1661, #1660, #1659, #1658, #1657, #1656, #1655, #1653, #1651, #1650, #1649, #1648, #1647, #1646, #1645, #1644, #1643, #1642, #1641, #1639, #1638, #1637, #1636, #1633, #1632, #1631, #1630, #1629, #1628, #1627, #1626, #1625, #1624, #1622, #1620, #1619, #1618, #1617, #1616, #1613, #1612, #1611, #1609, #1608, #1607, #1606, #1604, #1603, #1602, #1600, #1599, #1598, #1597, #1592, #1587, #1586, #1584, #1582, #1581, #1580, #1578, #1577, #1576, #1574, #1573, #1572, #1571, #1570, #1569, #1568, #1566, #1565, #1564, #1563, #1562, #1561, #1560.

**Fully superseded (7, no review surface):** #1671, #1669, #1667, #1664, #1662, #1635, #1634 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 12 — post-#1174 incremental (17-PR session sweep, 2026-06-16)

Coverage: **17 reviewed — 2 with findings, 13 clean, 2 fully superseded, 0 errored, 0 died.** Net: **0 CRITICAL, 0 HIGH, 0 MEDIUM, 2 LOW.** **First post-#1174 installment** — reviewed the merged PRs surfaced as "merged-since unreviewed" this session, a **sparse subset of #1282–#1318** (NOT a contiguous range — see § Sweep status; ~123 other post-#1174 PRs remain unswept-by-survivor here). Survivor-filtered against origin/develop, so every finding is current — already-fixed/reverted code excluded by construction. (Reviewer model `code-review` opus/high via the persisted `historical-review-sweep` workflow; 17/17 agents returned, 0 died, 0 errored.) **Both findings are `userVisible:false` (internal tooling/docs) → NO GitHub Issues; already filed to [`categories/tooling.md`](categories/tooling.md) in PR #1321 (merged).**

### LOW (2)
- **#1296 (eeeaabb7) · `tests/fuzz/README.md:27`** — broken smoke instruction. The "How it builds" block tells Linux users to run `ctest --preset ninja-fuzzer-linux`, but #1296 added `ninja-fuzzer-linux` only as a configure+build preset — there is **no `testPresets` entry** (CMakePresets.json has no testPresets section at all), and CMake test presets don't inherit from configure/build presets, so the command errors with `No such test preset`. CI is unaffected (`fuzz-smoke.yml` uses bare `ctest --output-on-failure` from the build dir; the smoke test IS registered via `add_test`, so the lane works). Fix: add a matching `testPresets` entry, or change README:27 to `ctest --test-dir build/ninja-fuzzer-linux --output-on-failure`. Filed: tooling.md (P3), PR #1321.
- **#1308 (a96b1cb0) · `agents/scripts/core/appcontroller_fan_in_audit.py` (`regression()`)** — fan-in ratchet is COUNT-based, not SET-based. `regression()` short-circuits to pass whenever `len(head_paths) <= len(base_paths)`, so a PR that drops one existing `#include "AppController.h"` includer and adds a different NEW one in the same change (net count flat) is **not** flagged — the new dependency slips through, defeating the gate's stated "block a new includer on sight" contract. The offender-listing loop below already computes the true set-difference `sorted(head_paths - base_paths)`; the count guard pre-empts it for count-neutral swaps. Fail-open. Fix: drop the count early-return, always evaluate the set-diff, FAIL on any new includer, add a same-cardinality selftest (`base={A}`, `head={B}`). Filed: tooling.md (P3), PR #1321. (Symbol-pinned, not line-pinned — verified live on develop: `def regression()` :181, count guard :186, set-diff :189.)

**Clean (13, surviving lines reviewed, no findings):** #1282, #1285, #1289, #1290, #1306, #1307, #1309, #1310, #1311, #1312, #1316, #1317, #1318.

**Fully superseded (2, no review surface):** #1284, #1297 — every introduced line was changed/reverted by a later PR (the config-string sanitize layer both PRs added is gone at HEAD); excluded by construction.

## Batch 11 — #116–#1 (FINAL, 113-PR sweep, 2026-06-13)

Coverage: **113 reviewed — 5 with findings, 16 clean, 92 fully superseded, 0 errored, 0 died.** Net: **1 CRITICAL, 2 HIGH, 0 MEDIUM, 4 LOW.** This is the **final** batch — the work-list ran from #116 all the way to **#1** (the repo's oldest merged PRs), including the early base-`main` PRs #1–#5; every one resolved as an ancestor of develop (main was folded into develop early), so **0 errored** and the sweep genuinely reaches the repo root. Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high, concurrency held to the Opus ≤6 guardrail via a hand-rolled 6-lane pool — run-journal validated max overlap **exactly 6**; 113/113 agents returned, 0 died, all 113 model `claude-opus-4-8`; windowed-read held — max per-agent **59,925** tokens, 0 over 100k; ~36.4 min, 4.88M tokens.) **All 7 findings are `userVisible:false` (internal CI gates / build scripts / docs / archived test-scaffolding) → NO GitHub Issues this pass; backlog only per ADR-0014 + the no-fix directive.** Dominant theme (one final time): the **fail-open gate cluster** — the CRITICAL (#86) is a NEW sub-shape (a required CI warning gate greps the *wrong toolchain's* warning format → always-empty match → always green under MSVC) and both HIGH (#80, #77) are the `passed=0&&failed=0`→exit-0 zero-run family. Cross-filed onto the OPEN P2 `fail-open-meta-gate-authoring-check` in [`categories/tooling.md`](categories/tooling.md).

### CRITICAL
- **#86 (b68bf09a) · `scripts/dev/test-build-warnings.sh:46`** — fail-open warning gate, blind to its own toolchain. The surviving warning grep matches only GCC's `warning: … [-Wunused-…]` tag form, but the default + CI preset is `ninja-iter-msvc` (MSVC), which emits unused-symbol warnings as numeric codes (`warning C4505/C4101/C4189/C4100`, no `[-Wunused-]` tag). So `OWNED_HITS` is always empty under MSVC → the gate always prints `Passed: 1  Failed: 0` and exits 0. `build-and-test.yml` builds `ninja-iter-msvc` then runs this script as a **required** bucket-A check (`SMATCHET_WARN_PRESET` is never set, so there is no GCC path to redeem the regex) — the gate is 100% blind to first-party unused-function/variable warnings on the only toolchain it runs under. The GCC regex was correct when #86 shipped under MinGW; a later commit flipped the default preset to MSVC without updating this surviving line. Fix: make the grep toolchain-aware — also match MSVC's codes, e.g. `grep -E 'warning:.*\[-Wunused-|warning C(4505|4101|4189|4100)'`, update the L43 comment, and add a negative test under the MSVC preset that confirms the gate catches a deliberately-unused symbol.

### HIGH
- **#80 (2e783d61) · `scripts/dev/test-theme-syntax-colors.sh:57`** — fail-open on a vanished suite. If `--test-case='SmatchetTheme*'` (L41) ever matches zero cases (suite renamed/removed/refactored out of the glob), doctest still prints `0 passed | 0 failed`, the L46 emptiness guard passes, `PASSED=0/FAILED=0`, and only `FAILED>0` is checked — so the script exits 0 (green) having run zero assertions. The wrapper exists to PROVE the per-theme syntax-palette round-trip; a disappeared suite must fail, not pass. Classic `passed=0&&failed=0` false-PASS. Fix: after parsing, assert at least one assertion ran — `if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then echo 'ERROR: SmatchetTheme* matched zero assertions — suite missing?'; exit 1; fi` before the `FAILED>0` check.
- **#77 (c104ddd7) · `scripts/dev/test-ui-views-columns-reorder.sh:68`** — fail-open on a zero-test run. If `passed=0` and `failed=0` (e.g. `UI_TEST_FILTER 'ColumnsReorder'` matches nothing after a test rename, or registration is silently dropped), neither value is `'?'`, the L63 guard passes through, `FAILED!="0"` is false, and control reaches `exit 0` — greenlighting a build in which the target test never ran. Fix: add a zero-tests guard before the exit-0 path — `if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then echo 'FAIL: ui_test.run matched/ran 0 tests' >&2; exit 1; fi`.

### LOW (4)
- **#105 (302eb654) · `scripts/dev/archived/test-norton-theme.sh.archived:17`** — revival instructions cite `docs/backlog/BACKLOG_PLANS.md` § 2 as the tracking doc, but that file no longer exists at develop (no successor by that name). A human following the revive steps for this archived bucket-E test hits a dead doc ref. Dormant (archived file is DO-NOT-EXECUTE). Fix: repoint the `Tracked:` line to the live doc tracking the Norton Commander palette-lock revival (a `docs/plans/*` entry), or drop the dead ref.
- **#105 (302eb654) · `tests/ui/archived/norton_commander_theme.test.cpp.archived:20`** — same stale cross-ref: the revival comment cites the removed `docs/backlog/BACKLOG_PLANS.md` § 2. Dormant (archived file is DO-NOT-COMPILE). Fix: repoint to the live tracking doc, or remove the ref.
- **#77 (c104ddd7) · `scripts/dev/test-ui-views-columns-reorder.sh:27`** — dead code: the `extract()` helper (L27-34) is defined but never called — the actual JSON parsing uses the inline `python -c` snippets at L50-52. It also embeds a walrus-assignment list comprehension (`[v := v.get(k) … for k in …]`) that reuses the loop body's assignment target and would be fragile/erroneous if ever invoked. Fix: delete the unused `extract()` function (L27-34).
- **#66 (5b740e92) · `CMakeLists.txt:761`** — the `FATAL_ERROR` message cites a broken doc path: `docs/design/applied/lua-recorded-cmd-list.md § Lua build mode`. That dir does not exist at HEAD; the doc actually lives at `docs/plans/shipped/lua-recorded-cmd-list.md` (which has the `§ Lua build mode` anchor). A contributor who trips this configure-time guard is sent to a dead path. Fix: update the path in the `FATAL_ERROR` string to `docs/plans/shipped/lua-recorded-cmd-list.md § Lua build mode`.

## Batch 10 — #117–#330 (200-PR sweep, 2026-06-13)

Coverage: **200 reviewed — 16 with findings, 61 clean, 122 fully superseded, 1 errored (#117, no merge commit).** Net: **0 CRITICAL, 1 HIGH, 4 MEDIUM, 15 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high, concurrency held to the Opus ≤6 guardrail via a hand-rolled 6-lane pool — run-journal validated max overlap **exactly 6**; 200/200 agents returned, 0 died, all 200 model `claude-opus-4-8`; windowed-read held — max per-agent **70,582** tokens, 0 over 100k; ~55.6 min, 8.95M tokens.) **All 20 findings are `userVisible:false` (internal tooling / gates / docs / test-scaffolding) → NO GitHub Issues this pass; backlog only per ADR-0014 + the no-fix directive.** Dominant theme (again): the **fail-open gate cluster** recurs one batch after #1192's 8-site point-fix — the lone HIGH (#329) is a NEW sub-shape (gate greps a stale checked-in doc, not the regenerated content) and all 4 MEDIUM are the `passed=0&&failed=0`→exit-0 / silent-skip family. Cross-filed onto the OPEN P2 `fail-open-meta-gate-authoring-check` in [`categories/tooling.md`](categories/tooling.md).

### HIGH
- **#329 (783b9946) · `scripts/dev/test-perf-marker-inventory.sh:30`** — fail-open gate. The GATING `perf_temp:*` leak check greps the **checked-in** `docs/perf/MARKER_INVENTORY.md`, not the regenerated inventory. Line 23 runs `perf-marker-inventory.sh --check`, which (`perf-marker-inventory.sh:151-166`) writes a temp file, diffs, then **discards** it — it never updates `MARKER_INVENTORY.md`. So if a dev adds a `perf_temp:*` marker in C++ source but doesn't regenerate the doc, the live tree leaks the marker while the stale committed doc still shows "(none — clean tree)" and the gate PASSES — defeating its sole purpose (blocking `perf_temp:*` from shipping), exactly the no-regen scenario it exists to catch. Fix: gate on the freshly regenerated content — grep the captured `$OUTPUT` (run in full/non-`--check` mode to a temp path and grep that), or have `--check` emit a machine-detectable leak signal and key the exit-1 off it.

### MEDIUM
- **#330 (218b733c) · `scripts/dev/test-ui-sync-stall-visible-cue.sh:62`** — false-PASS on zero-tests-run. The driver exits 0 whenever `FAILED==0`, with no guard that any test actually ran. If the `SyncStallVisibleCue` filter matches nothing (registration regression, rename, harness flag mis-wired), the engine reports `passed=0 failed=0` and the script exits 0 — falsely reporting the Pillar-2 "visible cue before block" invariant verified. Mitigated (not eliminated) by the CI stage being advisory. Fix: fail-closed on zero coverage before the `FAILED != 0` check — `if [ "$PASSED" -lt 2 ]; then echo 'FAIL: expected 2 SyncStallVisibleCue variants, ran '"$PASSED"; exit 1; fi` (Good+Bad).
- **#327 (e3c91847) · `scripts/dev/pillar2-scan.sh:88`** — `is_ui_reachable()` silently skips any UI-thread-reachable TU that is neither named `*Ui*.cpp/.h` / `SmatchetUI*` nor directly `#include`s `<imgui.h>`. A helper invoked from a `Draw*`/`Render*` function (or one that pulls imgui transitively via another header) is never scanned, so sync I/O on the UI path in such a file passes with no CRITICAL — a fail-open. The AST-vs-text trade-off is acknowledged in the header, but this naming/include miss is a genuine silent gap. Fix: also match files that `#include` a known UI header (e.g. `SmatchetUI*.h` / `*Ui*.h`) transitively, or scan callers of `Draw*`/`Render*` symbols; at minimum add a unit test asserting a non-`*Ui*`-named file that includes a UI header is scanned.
- **#214 (83644ca5) · `scripts/dev/test-ui-ai-assistant-enter-send.sh:56`** — fail-open on a zero-test run. When `ui_test.run` returns a valid envelope with `passed=0 failed=0` (filter matched nothing — renamed/typo'd filter, or the Ai tests compiled out while `SMATCHET_BUILD_UI_TESTS` stayed ON), control reaches L56-60 with `PASSED=0/FAILED=0`, prints "Passed: 0  Failed: 0" and exits 0 — a green pass that ran nothing. **Identical surviving logic in `test-ui-ai-assistant-panel-dock-swap.sh` and `test-ui-ai-prefs-autosave-flow.sh`.** Fix: before the final `exit 0`, guard the empty run — `if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then echo 'FAIL: 0 tests matched filter '$FILTER >&2; exit 1; fi` (apply to all three drivers).
- **#134 (3e19f93f) · `scripts/dev/test-config-migration.sh:93`** — gate fails OPEN on zero assertions: the wrapper only checks `[ "$FAILED" -gt 0 ]`, never asserts `PASSED -gt 0`. If the `--test-case='ConfigMigration*'` filter (L77) matches zero cases (renamed/relocated), doctest prints `0 passed | 0 failed`, the script prints "Passed: 0  Failed: 0" and exits 0 — a silent false-PASS that reports green while running no tests. Fix: add a positive-progress guard after parsing — `if [ "${PASSED:-0}" -eq 0 ]; then echo 'ERROR: 0 assertions ran — ConfigMigration filter matched no cases'; exit 1; fi`.

### LOW (15)
- **#330 (218b733c) · `tests/ui/sync_stall_visible_cue.test.cpp:32`** — stale doc-drift in a surviving comment: states "The ninja-ui-test-msys2 preset enables the flag". MSYS2 was retired (AGENTS.md MSYS2-retired); `SMATCHET_DEBUG_VISIBLE_CUE_HARNESS` is enabled via the `ninja-ui-test-msvc` preset (CMakePresets.json + driver/CI). Fix: replace `ninja-ui-test-msys2` with `ninja-ui-test-msvc`.
- **#328 (sha n/a) · `scripts/dev/perf-baseline-bootstrap.py:51`** — `--runner-os` defaults to `"windows-msys2-ucrt64"`, but the only caller (`perf-pr-fast.yml`) builds via MSVC (`ninja-iter-msvc` + `msvc-dev-cmd`) and never overrides it. MSYS2/UCRT64 is retired. Every bootstrapped baseline records a `captureRunnerOs` that misnames the toolchain. Metadata-only (not used in gate comparison), so cosmetic. Fix: default to a MSVC-accurate label (e.g. `"windows-msvc"`) or have the workflow pass `--runner-os` explicitly.
- **#327 (e3c91847) · `scripts/dev/pillar2-scan.sh:14`** — header comment claims the scanner is invoked by "bash scripts/dev/test-all.sh (auto-enrolled scan over changed files)", but `test-all.sh` contains zero references to pillar2. Doc-vs-code drift in a surviving comment: overstates where the gate runs. Fix: wire `pillar2-scan.sh` into `test-all.sh` over changed files, or drop/correct the `test-all.sh` line to reflect the actual paths (pre-commit hook + Claude wrapper + manual + CI).
- **#321 (c90382b3) · `scripts/dev/perf-baseline.sh:127`** — in `capture()`'s embedded Python, L124 guards `raw.get("data")` with `isinstance(raw, dict)`, but the `elif isinstance(raw.get("rows"), list)` on L127 calls `.get` on `raw` unguarded. A top-level array/scalar JSON raises `AttributeError` with an ugly traceback instead of the clean "ERROR: no rows[] payload" on L133. Fails closed (non-zero exit, no baseline written) — purely a worse error message. Fix: guard the elif the same way (`elif isinstance(raw, dict) and isinstance(raw.get("rows"), list):`).
- **#305 (48340053) · `agents/_shared/token-tracking/skill-load-log.py:116`** — `stdin_obj.get("duration_ms")` reads a top-level field Claude Code's PostToolUse payload does not provide (it is `session_id`/`tool_name`/`tool_input`/`tool_response` only). The recorded `duration_ms` is therefore always null; the sibling `agent-token-log.py:390` already hardcodes `"duration_ms": None`. The bundled test feeds a synthetic payload injecting `duration_ms`, so it passes against a shape the runtime never emits (fail-quiet); the L20 docstring compounds it. Fix: drop the field or hardcode `None` like the sibling, and correct the docstring. No functional consumer affected.
- **#304 (1667d145) · `agents/_shared/skills/perf-instrument/SKILL.md:27`** — header-path drift: prose cites `SMATCHET_UI_PERF_SCOPE` from `Source/Core/include/UiPerfMonitor.h`, but at origin/develop it lives at `Source/Core/include/Ui/UiPerfMonitor.h` (note the `Ui/` subdir). The `#include "UiPerfMonitor.h"` directive form is correct (build include path covers `Ui/`), so only the def-site pin is wrong. Same drift mirrored in `agents/core/perf-instrument.md` (in sync, so no V7 break — both wrong). Fix: update the path in both files.
- **#258 (sha n/a) · `tests/ui/whisper_ai_assistant_autosend.test.cpp:29`** — the test replica's drift-warning comment hard-pins production source line ranges (`SmatchetAiAssistantUi.cpp:253-292` at L29, `:393-394` at L140). The absolute pins silently rot whenever the source shifts; nothing gates them. Fix: replace with a stable symbol/function-name anchor (e.g. "the `ConsumePendingReloadItemId` + `ReloadUserBufAndMoveToEnd` block in `SmatchetDrawAiAssistantPanel`").
- **#222 (sha n/a) · `tests/ui/ai_prefs_autosave_flow.test.cpp:210`** — stale line-pin in an explanatory comment: cites `SmatchetPreferencesUi.cpp:197-206` as the cancel-on-close close-handler, but at origin/develop those lines are `CopyStringToBuffer`/inherit-field-buffer code. The pin drifted as the source grew. Fix: drop the brittle line range (keep filename + behavior description) or re-pin.
- **#204 (a390c2bc) · `.github/CODEOWNERS:62`** — `/.claude/CLAUDE.md @alexandrosk0` targets a gitignored/untracked path (`.gitignore:65` `.claude/`). A gitignored file never appears in a PR diff, so this owner mapping is permanently dead — contradicting the L59-60 comment that it protects the Claude auto-load mirror. Fix: drop L62 (`.claude/` is harness-local + gitignored) or repoint at the tracked canonical source; the `/AGENTS.md` entry on L61 already covers the real rules file.
- **#203 (sha n/a) · `.github/workflows/lock-cleanup.yml:84`** — the existence check GETs `repos/.../git/refs/locks/<slug>` (plural `refs`) and the L83 comment asserts "gh api returns non-zero on 404". The current GitHub REST API documents the single-ref GET as the SINGULAR `git/ref/{ref}`; the plural form is legacy/undocumented and its 404-vs-200-array semantics aren't guaranteed. Separately, `>/dev/null 2>&1` masks auth/rate-limit/network as "absent", so a transient error silently no-ops the delete. Fix: use the documented singular endpoint for the probe (ref WITHOUT the `refs/` prefix: `git/ref/locks/${SLUG}`); keep DELETE on the plural path; distinguish 404 from other failures.
- **#146 (d857310e) · `tests/golden/README.md:53`** — the CI-status section says the bucket-C screenshot-diff step is "Advisory (continue-on-error: true) until 2026-05-30", but today is past that date and the CI job (`build-and-test.yml:386-467`) is still `continue-on-error:true`. The doc implies a flip-to-blocking that never happened. Fix: update the date/soak language to the still-advisory reality and note the actual flip criteria.
- **#146 (d857310e) · `scripts/dev/test-screenshot-diff.sh:168`** — auto-bootstrap fail-open: when a golden is missing (not `--bootstrap`), the run writes the capture as the new golden and asserts PASS instead of failing. On a fresh checkout with no committed golden, every scenario passes vacuously. Mitigated only by the CI step being advisory + the behaviour being documented-intentional. Fix: treat a missing committed golden as a hard FAIL in CI while keeping the auto-bootstrap convenience local-only (key the soft-PASS off a dev-only env flag).
- **#145 (d125b364) · `tests/Lua/CMakeLists.txt:46`** — stale precise line-pin: comment cites the production `-mcmodel=large` gate at `CMakeLists.txt:976-989` (and `:976` next line), but at origin/develop that gate lives at ~1500 and ~1824-1844. Doc-vs-code drift on a gated literal. Fix: update the pins (the `NOT SMATCHET_LLD_PROGRAM` `-mcmodel=large` gate at ~1500 / ~1831-1844), or replace the numeric pins with a symbol reference.
- **#145 (d125b364) · `tests/Lua/CMakeLists.txt:93`** — stale precise line-pin: cites the Lua 5.3 static lib (`Smatchet_Lua_Internal`) as built "per CMakeLists.txt:336-343", but at origin/develop `add_library(Smatchet_Lua_Internal …)` is at ~755. Same gated-literal drift. Fix: repoint to ~755 or reference the target name without a line number.
- **#143 (ba1302ec) · `tests/support/LuaHostFixture.h:70`** — fixture comments (L3, L63-64) claim production parity for the `os` whitelist `{time, clock, difftime, date}` (matching `AppController_LuaBindings.cpp:358-359`), but the `osSafe` table only sets `time`/`clock`/`difftime` — `os.date` is omitted. The fixture is silently MORE restrictive than the sandbox it claims to mirror. Security contract still holds (the omission only tightens). Fix: add `osSafe.set_function("date", …)` to match, or amend the parity comments to state `date` is intentionally not mirrored.

**Fully superseded (122, no review surface):** #319, #318, #317, #315, #312, #306, #303, #302, #301, #300, #299, #297, #296, #295, #294, #293, #292, #291, #290, #288, #286, #285, #283, #282, #281, #279, #278, #277, #275, #274, #273, #272, #271, #268, #267, #265, #263, #262, #261, #259, #257, #256, #253, #247, #246, #243, #242, #241, #239, #238, #237, #235, #234, #233, #232, #231, #230, #229, #224, #221, #220, #217, #213, #211, #210, #208, #206, #200, #199, #197, #196, #193, #192, #191, #190, #189, #188, #187, #185, #184, #183, #182, #181, #180, #179, #178, #177, #175, #173, #172, #171, #169, #166, #164, #161, #160, #159, #158, #157, #152, #151, #149, #147, #144, #141, #139, #138, #137, #136, #133, #132, #131, #129, #128, #126, #125, #124, #123, #122, #121, #120, #119 — every introduced line was changed/removed by a later PR; excluded by construction. (#117 errored — no merge commit, not a develop squash.)

## Batch 9 — #331–#438 (100-PR sweep, 2026-06-13)

Coverage: **100 reviewed — 7 with findings, 41 clean, 52 fully superseded.** Net: **0 CRITICAL, 1 HIGH, 0 MEDIUM, 7 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high, concurrency held to the Opus ≤6 guardrail — run-journal validated max overlap **exactly 6**; 100/100 agents returned, 0 died, 0 errored, all 100 model `claude-opus-4-8`; windowed-read held — max per-agent **91,423** tokens, 0 over 100k; ~20.1 min, 4.54M tokens.) **All 8 findings are `userVisible:false` (internal tooling / gates / docs / test-scaffolding) → NO GitHub Issues this pass; backlog only per ADR-0014 + the no-fix directive.** The lone HIGH (#430) is another **fail-open gate** — a non-recursive scan blind to the subdirectory sites it claims to cover — a recurrence of the Batch-8 fail-open-gate cluster (cross-filed P1 in [`categories/tooling.md`](categories/tooling.md)).

### HIGH
- **#430 (sha n/a) · `scripts/dev/test-tooltip-wrapwidth.sh:46`** — the gate scans only the **top level** of `Source/Core/src` via `os.listdir(src_dir)` (non-recursive, root `*.cpp` only), but its header contract claims "every BeginTooltip+MarkdownPreviewRender::Render block in Source/Core/src/" (full-tree). Real markdown-tooltip sites live in subdirs the scan never reaches (`Ui/SmatchetOfflineQueueUi.cpp`, `Ui/SmatchetAiAssistantUi.cpp`, `Ui/SmatchetPlanDocViewerUi.cpp`, `Ui/SmatchetFieldRender.cpp`, `Commands/Scenarios/…`). A new offending site under `Ui/` is silently skipped — `checked` never increments, the script prints "Passed: N  Failed: 0" and exits 0: a fail-open gate that cannot catch the regression it exists to prevent. Fix: walk recursively (`os.walk(src_dir)` over all `*.cpp`), keep the per-file tooltip-block parsing as-is.

### LOW (7)
- **#420 (87b78f34) · `tests/bats/merge_gates.bats:1592`** — broken doc cross-ref: comment cites `docs/evaluation/agentic-infrastructure-2026-05-23.md`, but the doc moved to `docs/reference/` (`docs/evaluation/` no longer exists). Fix: repoint to `docs/reference/agentic-infrastructure-2026-05-23.md` (lines 1630/1674 carry the same stale path outside this PR's survivor set — fix together).
- **#420 (87b78f34) · `tests/bats/merge_gates.bats:1600`** — comment-vs-code drift: the comment describes the guarded mechanism as the defensive `|| echo -1`, but `merge-gates.sh` was refactored to parameter-expansion defaults (`ci_fail="${fields[6]:--1}"`, `cr_open="${fields[12]:--1}"`); the `|| echo -1` form no longer exists. Test assertions remain correct (both verify fail-closed blocking). Fix: reword the comment to the current `${fields[N]:--1}` default form.
- **#415 (2b1119a5) · `docs/perforce/AGENT_FLOWS.md:196`** — stale line-pin: the comment pins "test-p4-dual-vcs.sh scenario 2 (line 149)" but at origin/develop line 149 is a mid-block comment; scenario 2's empty-string `SMATCHET_LOCK_BACKEND=""` contract is at line 153 (block spans 125-168). Fix: repoint to line 153, or drop the line number and reference "scenario 2" by name.
- **#403 (eb0cde08) · `docs/perforce/RUNBOOK.md:86`** — checkpoint-recovery recipe replays journals via `Get-ChildItem … | Sort-Object Name` (lexicographic), so once rotation reaches double digits the order is wrong (`journal.10.gz` sorts before `journal.2.gz`) → out-of-sequence replay during disaster recovery. Bounded (non-canonical depot, rotation rarely double-digit) but the documented recipe is subtly incorrect. Fix: sort numerically by the rotation index (`Sort-Object { [int]($_.Name -replace '\D','') }`).
- **#398 (sha n/a) · `tests/bats/merge_gates.bats:757`** — the secondary assertion `[[ … *"2/2"* || … *"1/2"* ]]` is too loose: the test exists to prove a CheckRun "build" and a StatusContext "build" are NOT deduped to one, but the OR-branch accepts `1/2` — exactly the deduped-to-one outcome it claims to reject. Primary asserts (`status -eq 1`, `1 fail`) still verify the FAILURE blocks merge, so not fully fail-open, but the count assertion can't distinguish the collision bug. Fix: drop the `|| *"1/2"*` branch, assert only `*"2/2"*`.
- **#391 (a249cf5e) · `docs/CONTEXT.md:53`** — stale forward-reference: pins the scripts at `scripts/dev/p4-task-stream*.sh`, but they landed at `agents/scripts/project/p4-task-stream*.sh` (no `scripts/dev/` copy exists); lines 55/57 of the same section already use the correct path → internally inconsistent. Fix: update line 53 to `agents/scripts/project/p4-task-stream*.sh`, or drop the now-stale forward-reference note (PRs #380/#382 merged).
- **#364 (sha n/a) · `tests/bats/merge_watcher.bats:341`** — loose disjunction: the "handle_pass on PR-already-merged → merge_failed" test asserts `merge_failed` OR `skipped`, but the stub makes `gh repo view` succeed and only `gh pr merge` fail, so only `merge_failed` can fire; the `|| skipped` weakens the guard — a regression that early-returns to `skipped` (never attempts the merge) would still pass green. Fix: drop the `|| skipped` alternative, assert only `merge_action: merge_failed`.

**Fully superseded (52, no review surface):** #438, #437, #435, #425, #423, #422, #419, #416, #414, #413, #412, #408, #406, #402, #400, #399, #396, #395, #394, #392, #389, #388, #386, #385, #383, #382, #380, #379, #378, #376, #371, #370, #369, #367, #362, #359, #358, #356, #355, #354, #353, #351, #350, #349, #346, #345, #340, #339, #338, #336, #335, #333 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 8 — #439–#541 (100-PR sweep, 2026-06-13)

Coverage: **100 reviewed — 12 with findings, 48 clean, 40 fully superseded.** Net: **0 CRITICAL, 6 HIGH, 6 MEDIUM, 7 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high, concurrency held to the Opus ≤6 guardrail cap; 100/100 agents returned, 0 died, 0 errored; ~17.8 min, 4.64M tokens.) **All 19 findings are `userVisible:false` (internal tooling / gates / docs / test-scaffolding) → NO GitHub Issues this pass; backlog only per ADR-0014 + the no-fix directive.** Dominant theme: a **fail-open gate cluster** (6 HIGH) where a probe/test driver returns green on a transient error or a zero-match filter — cross-filed as P1 in [`categories/tooling.md`](categories/tooling.md).

### HIGH
- **#519 (9aaba5c7) · `.github/actions/cr-finding-gate/action.yml:69`** — the CR-installed probe fails **OPEN** on transient API errors: it collapses every non-zero `gh` exit (genuine 404 *and* auth/network/500) to `cr_installed=false`, so the required gate posts "CR not installed", exits 0, and waves a PR through with its CodeRabbit findings un-reviewed. This is exactly the pre-H12 bug `merge-gates.sh` was hardened against (its lines 194-222 separate a real 404→absent from a transient failure). Fix: set `cr_installed=false` only on a confirmed HTTP 404; treat any non-404 as installed (fail safe / closed).
- **#513 (ce58faf1) · `scripts/dev/test-ui-mcp-lua-fresh-state-race.sh:99`** — fail-open: the driver fails only when `FAILED != 0`; if the FreshState filter matches **zero** tests, `passed=0 failed=0` exits 0 green and the cross-thread `lua_State` race this guard exists to catch could re-land undetected. Fix: also fail when zero tests executed (`if [ "$PASSED" -lt 1 ]; then …; exit 1`).
- **#452 (27419f5b) · `scripts/dev/test-ui-agent-proposal-store-sqlite.sh:65`** — fail-open: no `passed=0 && failed=0` guard between the run (L60) and the final green echo, so a zero-match AgentProposalStore filter passes with zero coverage. Sibling drivers guard this. Fix: add a zero-test guard before the final echo.
- **#452 (27419f5b) · `scripts/dev/test-ui-ai-assistant-preferences.sh:65`** — same fail-open on the AiPrefsTab filter; a renamed/zero-match filter exits green. Fix: add the zero-test guard.
- **#452 (27419f5b) · `scripts/dev/test-ui-description-tooltip-markdown-render.sh:65`** — same fail-open on the DescriptionTooltip filter (defensive cover for the be2b1d9 `wrapWidth` regression); zero matches → green, regression undetected. Fix: add the zero-test guard.
- **#452 (27419f5b) · `scripts/dev/test-ui-spawn-warmup-deterministic-gate.sh:64`** — same fail-open on the SpawnWarmup filter (the infra.md P2-line-16 deterministic gate); zero matches → green. Fix: add the zero-test guard.

### MEDIUM
- **#524 (808fde79) · `.github/actions/cr-finding-gate/action.yml:167`** — the actionable-finding count is parsed only from the **first** body line (`split("\n")[0]`); a CR banner/walkthrough preamble before "Actionable comments posted: N" makes the header invisible → real findings missed, gate fails open. (`merge-gates.sh` keeps `cr_actionable=-1` on a parse-miss → fails closed — opposite direction.) Fix: scan the whole body for the header, or fail closed when the header is absent but `n_reviews > 0`.
- **#518 (a0d2b97a) · `.github/workflows/pillar2-scan.yml:57`** — `git diff … 2>/dev/null … || true` silences all errors, so a failed fetch / unresolvable base / all-zero SHAs yields zero files and exits 0 "no first-party C++ changed" **without scanning** — a Pillar-2 escape. Fix: distinguish a git error from a genuinely empty diff (capture rc, `rev-parse --verify` the base, exit 1 on error).
- **#509 (35fc99a8) · `docs/harness/claude-code/hooks/autoregister-pr.sh:28`** — the PR number is grepped as the first `pull/[0-9]+` across the **entire** payload then `head -1`, so `gh pr create --body "supersedes pull/123"` lets the body's number win → the **wrong** PR is registered with the merge-watcher → an unintended auto-merge target. Fix: parse only the created-URL (jq on `tool_response`), or take the **last** `pull/N`.
- **#502 (1b9e607e) · `docs/CONTEXT.md:35`** — stale gated literal: the Pillar-1 row says "Perf PR-fast (windows-2022) **NOT** required on develop", but that promotion already shipped — it **is** required. Fix: update the row to "Required on develop".
- **#498 (sha n/a) · `tests/bats/markdown_links.bats:49`** — the detection/exclusion bats cases re-run **handwritten Python copies** of the lint regex instead of invoking `$LINT`: the inline `LINK_RE` omits the title-suffix branch and the inline `is_active_md` hardcodes only the `docs/plans/shipped` exclusion vs the real lint's two. A real regression in the lint would still pass the test. Fix: drive every case through the real `$LINT`.
- **#471 (5701646a) · `docs/harness/claude-code/hooks/lint-catch-all.py:59`** — the body-capture loop only appends when `j > body_start`, so a single-line `} catch (...) { return false; }` yields an empty body → misclassified as an empty catch (false CRITICAL) even though it has content. Fix: seed `body_text` with the post-`{` remainder of the opening line.

### LOW (7)
- **#502 (1b9e607e) · `docs/CONTEXT.md:37`** — count drift: claims "3 MSVC variants" but branch protection lists 2. Fix: change to 2.
- **#502 (1b9e607e) · `docs/CONTEXT.md:35`** — scenario-count drift: "all 15 scenarios" but the canonical count is 14. Fix: reconcile to 14 (or "all registered scenarios").
- **#471 (5701646a) · `docs/harness/claude-code/hooks/lint-catch-all.py:48`** — the brace scan is bounded to a 100-line window; a `catch` body extending beyond it is silently skipped (fail-open). Fix: scan to EOF, or emit a diagnostic on window-exhaustion.
- **#471 (5701646a) · `docs/harness/claude-code/hooks/lint-catch-all.py:49`** — brace depth counts braces inside string literals / comments, so `LOG_ERROR("… }")` miscounts and can misclassify the catch. Fix: skip braces inside string literals and `//` comments.
- **#447 (57081811) · `tests/support/FakePlaneFixture.h:88`** — raw `new FakeTrackerClient("Plane")`; `no-raw-new` is an absolute (0-grandfathering) rule. Fix: `std::make_unique<FakeTrackerClient>("Plane")`.
- **#446 (sha n/a) · `tests/support/FakeGitHubFixture.h:84`** — raw `new FakeTrackerClient(...)`; test scaffolding, no leak, but the same `no-raw-new` style deviation. Fix: `make_unique`.
- **#445 (4407adcd) · `tests/_debug/SmatchetAgentDebug.h:358`** — `SMATCHET_AGENT_DEBUG_FSYNC=true` only increments `fsync_count`; it does **not** fsync (no-op), yet is advertised as "semantically wired" → a latent footgun for anyone relying on it for durability testing. Fix: back it with an explicit fd + real fsync, or change the comment to "no-op (count only)".

**Fully superseded (40, no review surface):** #536, #532, #529, #528, #521, #516, #515, #512, #508, #506, #504, #503, #501, #499, #496, #494, #493, #492, #491, #489, #483, #481, #480, #479, #474, #470, #469, #468, #467, #466, #465, #464, #463, #455, #453, #451, #450, #449, #448, #440 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 7 — newer #1029–#1174 (122-PR sweep, 2026-06-13)

Coverage: **122 reviewed — 16 with findings, 99 clean, 7 fully superseded.** Net: **0 CRITICAL, 3 HIGH, 5 MEDIUM, 10 LOW.** Survivor-filtered against origin/develop, so every finding is current — already-fixed code excluded by construction. (Reviewer model `code-review` opus/high; 122/122 agents returned, 0 died, 0 errored.) **3 user-visible findings → GitHub-Issue candidates (ADR-0014): #1158, #1138, #1049** — logged here, **NOT** filed and **NOT** fixed this pass per the no-fix directive.

### HIGH
- **#1158 (928693ae) · `Source/Core/src/Commands/PaneCommands.cpp:176`** — `pane.new` arms the deferred-create latch (`d.paneAddRequest.sourceId = focusedPane().id`, :169) **before** the `BackendCredentialsPresent` check. On absent creds the handler returns `Failure` (:176) but leaves `sourceId` set with `targetBackendKey` cleared; the host's `ApplyPaneAddAndCloseRequestsCore` keys only on `!sourceId.empty()` (no credential re-check) and creates a same-backend **duplicate pane** next frame. So `pane.new {backend:"Plane"}` with no Plane creds reports "no credentials configured" yet still spawns an unwanted pane — command reports failure but mutates state. **→ Issue candidate (user-visible).** Fix: validate creds + resolve backend/view into locals first, arm `d.paneAddRequest` only on the success path; or clear the latch (`d.paneAddRequest = PaneAddRequest{};`) before the `Failure` return.
- **#1138 (49932ed8) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1952`** — data race on the `gridContexts_` `std::map` **container itself**. `FetchPaneGroupMembers` runs on a `std::async` worker (`SmatchetUserInfoUi::launchMembersFetch`) and, after a multi-second blocking HTTP fetch, calls `gridContexts_.find(paneId)` for the roster write-back. Concurrently the UI thread's `TickAllContexts → retireExpiredHiddenContexts_` (AppController.cpp:797-841) **erases** from the same bare `std::map` (no guarding mutex; AppController.h:999). ADR-0012's latch/graveyard keeps the retired `GridLiveContext` object alive but does **not** serialize map-node mutation — an erase rebalances/deletes tree nodes a concurrent find traverses → UB/crash. The User-Info window can outlive a retired/hidden source pane, so the wide post-HTTP window is reachable. **→ Issue candidate (user-visible crash).** Fix: guard all `gridContexts_` structural access (find here + emplace at :487 + erase at :841) with a dedicated mutex, or snapshot the `unique_ptr<GridLiveContext>*` under that mutex before the fetch and skip the post-fetch find.
- **#1116 (17db90b3) · `scripts/dev/pre-ship.sh:283`** — strict-zone detection fails **OPEN** on Windows where `python3` resolves to the App-execution-alias stub. `command -v python3` succeeds for the stub, so the WARN fallback (:289) never fires; the actual `python3 -c …` (:285) then fails (exit 49 "Python was not found"), swallowed by `2>/dev/null … || true`, leaving `review_strict_zones` empty with no warning. A sub-60-line strict-zone diff (e.g. 30-line edit to `Source/Core/src/Sync/`) is then classified non-substantive and the review gate **N/A-passes** it (:338) — exactly the high-risk code the gate exists to force a review on. Empirically confirmed: `bash scripts/dev/pre-ship.sh --selftest` exits 1 here ("review gate passed a strict-zone diff with NO ack"). Internal-tooling (gate fail-open), not user-visible. Fix: probe a real `"$PY" -c 'import json'` invocation (not file existence), or fall back to a pure-shell `project.config.json` parse; emit WARN whenever zero zones AND cpp changed; wire the selftest into CI/SessionStart.

### MEDIUM
- **#1164 (12a5444f) · `scripts/dev/worktree-prune.sh:99`** — dirty-gate checks `git diff --quiet` + `--cached --quiet` but **not untracked files**. A merged worktree holding only untracked scratch is classed clean → shown as "would-reap" in dry-run and routed to REAP on `--apply`. No data loss (non-force `git worktree remove` at :105 refuses it, rc=128 → reap reports FAILED), but the dry-run candidate list misleads and `--apply` spuriously fails; the bats dirty-skip test only covers the staged path. Fix: treat non-empty `git -C "$path" ls-files --others --exclude-standard` as dirty=1; add a bats untracked-only case.
- **#1161 (b1cf9c0f) · `docs/harness/claude-code/hooks/guard-shared-tree.sh:79`** — linked-worktree exemption extracts one command-global `-C <path>` (`grep -oE … | tail -n1`) and exempts the **whole** command if it resolves to a foreign worktree. The :69 trigger matches if ANY mutating git op appears in a compound command, so `git -C <other-wt> merge && git reset --hard origin/develop` extracts only the worktree `-C`, resolves foreign, and exempts (:84) — letting the bare `reset --hard` in the **shared** tree escape the sibling-HEAD rug-pull guard. `tail -n1` also lets the last `-C` win. Mitigated: secondary/advisory guard (`guard-head-drift.sh` is the hard net; compound form uncommon). Fix: only exempt when EVERY mutating git op carries a `-C` to a non-integration worktree — split on `;`/`&&`/`||`/`|` and evaluate each segment's git op independently.
- **#1057 (d82b2b9f) · `tests/CMakeLists.txt:808`** — surviving comment claims the glob-vs-list guard (widened to `Plugins/` here at :810-812) is "Mirrored by `agents/scripts/core/check-test-list.sh`", but the bash mirror only scans `tests/Core/*.test.cpp` (:55 + :26 glob Core-only). No `Plugins/` coverage → an unreferenced `tests/Plugins/**/*.test.cpp` passes the local check and only trips at CMake configure. Doc-vs-code drift on a gate claim. Fix: widen `check-test-list.sh` to also scan `tests/Plugins/*/*.test.cpp`, or soften the comment to "mirror covers Core/ only".
- **#1053 (ecf911df) · `agents/scripts/core/test-gate-selftests.sh:55`** — `exposers()` scans scan-dirs **non-recursively** (`for f in "$root/$d"/*` + `[ -f "$f" ]` skips dirs), so any `--selftest`-exposing script in a subdirectory is silently excluded. `scripts/dev/local/` already holds 6 scripts. Latent fail-open in the meta-gate whose stated job is "a future gate physically cannot ship a pass-only selftest" — a selftest exposer one level deeper escapes entirely (none currently expose `--selftest`, so not yet triggered). Fix: recurse via `find "$root/$d" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.bash' \)` (preserve unreadable-fail-closed + no-match suppression), or assert the top-level-only scope.
- **#1049 (15ac159d) · `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:209`** — the day→CL resolve launch (:196-218) is gated only on date-picker confirm + valid parse, **not** on `State().beforeClResolving`. Confirming a second date while the first server-wide `p4 changes -r -m 1 -s submitted //...` scan is in flight reassigns `State().beforeClFut` (:209), dropping the last reference to the unready `std::async` shared state — whose destruction **BLOCKS the UI thread** until the in-flight p4 scan finishes (the Pillar-2 freeze this code claims to fix). The mirror `P4ClPreview.cpp:45-56` explicitly detaches the pending future into `DetachedHoverFuts` before overwrite; this re-fire path omits the safeguard. **→ Issue candidate (user-visible UI freeze).** Fix: gate the launch on `!State().beforeClResolving`, or adopt the `P4ClPreview` detach-and-reap pattern.

### LOW (10)
- **#1168 (d147a685) · `agents/scripts/project/test-lint-rules.sh:503`** — `cmake-local-gate-ci-scope` is a greedy 80-line backward-window heuristic: any `ENV{CI}`/`ENV{GITHUB_ACTIONS}` token in the window sets `ci=1` and suppresses the finding even if it belongs to an unrelated earlier `if()` block → an unrelated CI-gated stanza upstream of a genuinely-unguarded knob-keyed `FATAL_ERROR` fails open. The `*'message(FATAL_ERROR'*` match (:498) also misses `message( FATAL_ERROR` with whitespace. Accepted cheap-heuristic per the plan's Risks; if precision matters later, scope `ci=` to the enclosing `if()/endif()` block and tolerate whitespace after `message(`.
- **#1160 · `agents/scripts/core/test-oob-label-impl.sh:68`** — `_label_is_implemented` treats any non-comment line that merely **mentions** the label as "implemented" — no check it is actually READ (no `$labels`/`gh pr view --json labels`/`has_label` context). A `*-out-of-band` label on a non-comment-but-non-reading line (a step `- name:`, an `echo`) falsely PASSES, weakening the prose-promise class the gate exists to catch; the `--selftest` only exercises a genuine reading line. Fix: require the label co-occur with a label-reading construct, or add a non-reading-line selftest fixture.
- **#1154 · `Source/Core/src/TicketFieldEditor.cpp:222`** — `LoadDurationSuggestions()` runs **every frame** the suggestions popup is open, on the ImGui render path: `ConfigManager::Load()` (memory-cached, no disk I/O, but takes a mutex + returns the full `TrackerConfig` by value — many string/vector members) then copies out the `DurationSuggestions` vector — per-frame heap-churn while the dropdown shows. Latent perf, scoped to popup-open frames; an existing backlog item already tracks `ConfigManager::Load()` render-path overhead. Fix: cache the list once on popup-open (per-widget `ImGuiStorage` / static refreshed on config-invalidate).
- **#1116 (17db90b3) · `scripts/dev/pre-ship.sh:289`** (twin of the HIGH) — the WARN "strict-zone detection skipped (the line threshold still applies)" only prints when `command -v python3` fails outright; on the common Windows-stub case (command -v succeeds, invocation fails) the operator gets **no** warning, so a silently-disabled strict-zone half looks identical to a genuinely non-substantive diff in the "gate N/A" output (:338). Fix: trigger the warning on the observable condition (`review_strict_zones` empty while `review_changed_cpp` non-empty), not on the `command -v` guard.
- **#1110 (4918b5e9) · `.github/scripts/mobile-emulator-smoke.sh:3` (+8 others)** — surviving code-comments reference the plan at `docs/plans/mobile-mvp-completion.md`, but this same PR archived it to `docs/plans/shipped/mobile-mvp-completion.md`. Stale path recurs in 8 more surviving comments: `build-and-test.yml:1214`, `mobile-emulator-smoke.yml:3`, `Tracker/TrackerHttpPure.h:7`, `AndroidApp/app/build.gradle:89`, `SmatchetActivityImeTest.java:15`, `robolectric.properties:2`, `test-android-openssl-failfast-bats.sh:13`, `tests/CMakeLists.txt:876`. Free-text comments (not gated links), so no gate breaks — misdirect a reader only. Fix: repoint each to the `shipped/` location.
- **#1101 (d806f863) · `agents/scripts/core/postmortem-owed.sh:400`** — the Core-cpp de-noise gate drops a flagged PR when `! pr_touches_core_cpp`; `pr_touches_core_cpp` runs `gh pr view … 2>/dev/null | grep -qE`, so on a transient gh/API/auth failure the pipeline emits nothing, grep returns false, the negation is true, and a **genuine** Core-cpp escape carrying an allow-listed trigger is silently suppressed (false negative — the direction the file's own :375/:381 comments call worse). Latent: advisory SessionStart nudge (never blocks merge), upstream `gh pr list` would usually fail in the same outage, and the fail-silent pattern matches sibling helpers. Optional hardening: capture `files=$(gh pr view …)`, treat empty as "no Core cpp" only when `$?`==0, else keep the PR.
- **#1085 (ecdc9332) · `agents/scripts/project/test-mobile-security.sh:130`** — `check_cmake` uses `if ! grep -Pzq …` for the `FATAL_ERROR` control-flow check; grep exits 2 on error (e.g. a `-P`/PCRE-less grep build), `! 2` is false, the block is skipped, and a missing fail-fast marker is silently treated as PASS — fail-open on a non-GNU grep. Guarded in practice (the workflow runs `--selftest` first). Fix: capture `rc=$?`, treat `rc>=2` as infra error (return 2); same for `OSSL_WARN_RE`/`grep -qF`.
- **#1085 (ecdc9332) · `agents/scripts/project/test-mobile-security.sh:32`** — documented self-disable hole (:32-37): the merge-gate poller enforces absent-check presence only for **required** contexts (`$reqAbsent` over `$reqNames`, merge-gates.sh:350); an absent non-required allow-listed check passes. A PR that deletes/renames `mobile-security.yml` in its own diff makes "Android security gate" absent, so the poller no longer blocks the #1067/#1068 regressions. Acknowledged + backlogged out-of-WS1-scope, visible in diff review. Fix: poller-wide present-assertion for allow-listed meant-to-block checks (already tracked).
- **#1060 (703261fc) · `agents/scripts/core/test-markdown-links.sh:80`** — the `--selftest` re-invokes the real diff-scope scan and asserts only a non-zero overall exit: if any other changed/untracked markdown already has a dangling link the selftest passes for the wrong reason; conversely the :87 "clean untracked file must PASS" assertion false-FAILs whenever the ambient tree has a pre-existing dangling link. Not hermetic — depends on working-tree cleanliness; errs toward false-FAIL not false-PASS of the production gate (tooling robustness only). Fix: scope the re-invocation to a temp dir / synthetic fixture (drive the python scanner over fixture files, or env-gated `TARGETS` override).
- **#1037 (5eefb9c2) · `docs/plans/build-quality-velocity-hardening.md:83`** — ~~broken markdown cross-ref~~ **RESOLVED (false positive, 2026-06-16)**: the cited link `[tracker-result-migration.md](docs/plans/tracker-result-migration.md)` uses the tier-LESS move-proof form, which is canonical per PR #890 (`test-plan-ref-integrity.sh` resolves `docs/plans/<slug>.md` against any tier) — it was never dangling. `tracker-result-migration` was archived to the `shipped/` tier in the 2026-06-16 plan-archival sweep and the tier-less link still resolves. No fix needed.

**Fully superseded (7, no review surface):** #1153, #1090, #1077, #1051, #1042, #1041, #1039 — every introduced line was changed/removed by a later PR; excluded by construction.

## Batch 6 — newer #952–#1028 (64) + older #601–#542 (50) (115-PR sweep, 2026-06-08)

Coverage: **115 reviewed — 20 with findings, 82 clean, 13 fully superseded.** Net: **1 CRITICAL, 6 MEDIUM, 13 LOW.** Survivor-filtered against origin/develop, so every finding is current (already-fixed code excluded by construction — see the Remediation log above for prior-batch fixes).

### CRITICAL
- **#565 (1a9db3b0) · `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp:212`** — the `colorRow` lambda in `DrawAnnotateCacheAndColors` (render path) calls `ConfigManager::SaveAnnotateAnalysis(cfg)` **synchronously on each color-edit commit** — takes the shared config RMW mutex, re-reads merged config, JSON-encodes, atomically rewrites `smatchet_config.json` on disk, all on the UI thread. The rest of the file deliberately routes this off-thread via `PersistAnnotateCfg`/`ScheduleAnnotateConfigSaveDetached`; line 212 is the survivor on the sync path. **→ Issue candidate (joins the #611/#761 sync-I/O cluster).** Fix: `PersistAnnotateCfg("edit_color")`.

### MEDIUM
- **#975 (d304eab3) · `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:681`** — `EnsureProjectComponentsLoaded` inserts the `projectComponentsInFlight_` marker into the kick-time focused context but the worker re-resolves `fieldCatalog()` at completion (L697) and erases/writes into the *completion-time* focused context. A pane focus-switch mid-fetch **permanently leaks the in-flight marker** in the original pane → that project stuck "Loading components…" forever until restart (NOT self-healing, contra the debt note). Fix: capture the kick-time `GridLiveContext*` and write through it.
- **#967 (c742847d) · `docs/harness/claude-code/hooks/guard-head-drift.sh:107`** — NEW fail-open: `all_git_ops_target_safe_worktree`'s `grep -oE` trailing boundary `(\$|[;&|)[:space:]])` is *consuming*, so `git -C /safe-wt commit;git commit` (single separator, no space) — grep eats the `;`, the second bare `git commit` loses its leading boundary and isn't extracted → exemption validates only the safe op → deny skipped → **bare commit to develop slips through**. Distinct from the shapes in tooling.md:54. Fix: zero-width trailing boundary (`grep -oP` lookahead) or normalize separators to whitespace first; add a bats case.
- **#978 (d68f0f32) · `docs/plans/active/subagent-eval-agentic-coverage.md:72`** — Phase-3 schema design uses a JSON-schema `else` clause but `validate_schema.py` only resolves `if`→`then` inside `allOf` (no `else`); implemented verbatim, the single-shot branch is never evaluated → `expectedFindingCount` silently un-enforced (false-pass), contradicting the plan's own "grill-verified" guarantee. Fix: express as two `allOf` if/then branches, or extend the validator.
- **#976 (cdc7b8dd) · `docs/agent-rules/process-rules.md:172`** — the Claude Code row prescribes `autoCompactEnabled: true` + numeric `autoCompactWindow` in `~/.claude/settings.json`; these keys appear nowhere in the repo/harness docs and don't match Claude Code's known settings schema (auto-compact isn't a numeric knob) → an operator adds a no-op key (~75% confidence). Fix: verify vs the real schema; if absent, describe the built-in auto-compact as advisory (like the Codex/Cursor rows).
- **#572 (d4a31a75) · `tests/ui/annotate_prefs_persist_flow.test.cpp:240`** — host config restore not failure-safe: the test writes a seed into the REAL config file then restores via `EndConfigSnapshot` only after all asserts; `IM_CHECK` expands to `if(!res) return;`, so a failing check skips the restore → leaves the host's real config polluted (violates the file's "byte-identical" guarantee; compounds across variants). Fix: RAII scope-guard restore on every return path.
- **#548 (e9688342) · `docs/adr/0003-github-as-itrackerclient.md:9`** — #548's link-retarget points two Context links at `../plans/active/agentic-flow-implementation.md` + `agentic-triage-flow.md`, both hard-deleted by the agentic ripout 8 days prior → 404 inside an Accepted ADR. (+2 LOW same pattern in ADR-0004.) Fix: repoint to `docs/plans/shipped/github-tracker-backend.md`.

### LOW (13)
- **#968 (d01ca584) — in the historical-review tool itself:** `historical-review-survivors.sh:157` hardcodes 40-hex SHA-1 in the blame-porcelain parser → on a SHA-256 repo every file reads as FULLY SUPERSEDED (false-clean; latent); `:173` `--context N>0` range-merge vs ±N pad can overlap → context printed twice + backwards `@@` numbers (cosmetic). Fix: accept 40-or-64-hex; merge when `gap ≤ 2*ctx+1`.
- **#1017 (508277ba)** `SmatchetResult.h:142` — `Result<T,E>` not exception-safe like sibling `Optional<T>`: assignment ops set `ok_` before a throwing placement-new + the private default ctor leaves no member; a throw → `Destroy()` runs a dtor on unconstructed storage (silent UB). Narrow (current types have noexcept moves) but a foundational reusable primitive. Fix: construct-then-commit + a constructed-member guard.
- **#962 (472c2de3)** `GridPane.h:13,72` — header still states the Slice-2 model ("exactly ONE GridLiveContext is live … until Slice 3"); Slice 3 shipped (#986) making every visible pane live; sibling headers updated, this central data-structure header missed → misstates the liveness/thread-safety model. Doc-only.
- **#578 (6eab3dbc)** `SmatchetUI.cpp:459` — `ParseImGuiHotkey` re-tokenizes the config string into a heap `vector<string>` every frame on the render path (Pillar-1 per-frame alloc); negligible but trivially memoizable.
- Doc-drift / broken-ref / stale-status cluster (all LOW, doc-only): **#1010** plan "4 PRs" miscount; **#980** `merge_gates.bats:516` ordering-coverage gap (fixture has "too many files" so the arm-order invariant is never exercised); **#952** golden-image `SMATCHET_TEST_DEFAULT_IMGUI_THEME` knob documented but unread in source; **#590** DeepSeek buffer-shape comment contradicts the sizes; **#563** `p4-annotate.md:18` duplicate `- annotate` trigger from the rename; **#555** `test-doctor.sh:91` strip-dir only removes first PATH copy; **#549** `smatchet-merge-watcher.md:90` `../guides/` link + `STRUCTURE.md:68` stale "157" (now 192) purity count; **#545×3** stale `docs/plans/active/applied/` refs in coverage-gate.yml + 2 Lua test comments; **#542** `PORTABILITY.md:34,41` stale `p4-blame` (→ `p4-annotate`).

### Prior-findings re-verification (this request's "what's been fixed")
The **Remediation log (top of file)** already records the prior-batch fixes done in the 2026-06-08 pass — confirmed independently here: **#918, #834, #630, #657, #722, #755, #853, #909/#855, #940, #670 are FIXED** at origin/develop; **#854, #611, #761, #732, #767, #892, #671, #948** are deferred to GitHub Issues (user-visible), still alive. Batch 6's survivor filter excludes all of those fixed lines automatically, so no batch-6 finding re-reports a remediated one. (Note: the new **#565** CRITICAL is the same sync-I/O class as the deferred #611/#761 — fold it into that Issue.)

## Batch 5 — PRs #602–707 (100-PR workflow sweep, 2026-06-07) — FINAL BATCH

Coverage: **100 reviewed — 13 with findings, 73 clean, 14 fully superseded.** Net: **1 CRITICAL, 1 HIGH, 3 MEDIUM, 8 LOW.** Sweep stopped here per user.

### CRITICAL
- **#611 (246b5238) · `Source/Core/src/Ui/SmatchetToolbarUi.cpp:124`** — `RefreshTrackerAppendCache()` calls `ConfigManager::LoadPersistentViewsFromDisk()` (sync ifstream + JSON parse under `GetIoMutexRef()` + OS `ScopedFileLock`) from `RenderBar()` (`:142`) on the ImGui render path → Pillar-2 CRITICAL. Memoized (backend-change/startup/post-save), but those frames block + can stall under contention with a concurrent `SavePersistentViewsToDisk`. **→ Issue candidate.** Fix: source the per-tracker append from in-memory config, or hoist off-thread (`std::async` + per-frame poll) on backend-change.

### HIGH
- **#671 (2533b9ab) · `Source/Standalone/CliCommandRunner.cpp:915`** — `SpawnAndRunHandleAsync`'s invalid-JSON `catch(...)` returns `kExitHandler` **without** sending `app.quit`, breaking its own documented invariant (`:845`) that the caller relies on. Result: when the scenario result file exists but isn't valid JSON, the spawned ephemeral instance is never told to quit → **orphaned subprocess holding a TCP port**. The other two failure branches do send the best-effort quit. Fix: POST the same `app.quit` before the `return` (mirror `:887-890`).

### MEDIUM
- **#670 (333a133f) · `Source/Core/src/Tracker/JiraIssueMutation.cpp:81`** — `FindTransitionIdInArray` applies its match priority **per-transition** instead of globally (contradicting its own doc: id → status-name → transition-name). An earlier transition whose *name* matches the requested status but whose `to.name` differs is returned ahead of a later transition that actually leads to the requested status → **the issue moves to the WRONG status in the user's Jira** (WARN logged, but wrong transition still executes). **→ Issue candidate (user-visible external mutation).** Fix: two-pass — exact id/status-name match across all transitions first, transition-name fallback only if none.
- **#630 (8a824ef7) · `docs/guides/imgui-draw-pattern.md:91`** — Rule 4's audit command greps `Source/Core/src/Smatchet*Ui*.cpp` but all UI sources are under `Source/Core/src/Ui/` → matches **zero** files, silently implying "no `static` locals to extract", defeating the rule. Fix: `Source/Core/src/Ui/Smatchet*Ui*.cpp`.
- **#620 (ac7dcb58) · `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md:77`** (+85,112-114) — instructs agents to run `test-backlog-counts.sh --fix` to sync a § Index count column; the script was rewritten 2026-06-03: no `--fix` flag, the count column was deliberately removed, and the gate now **FAILS** if a count column is re-added. Following the doc trips the guard (inverse of documented effect). Fix: drop `--fix` + count-sync; state counts are on-demand via `--list` and a count column must never be re-added.

### LOW (8)
- **#664 (9765cf4a)** `SmatchetAiAssistantUi.cpp:232` — hash-collision recovery branch uses `emplace` (no-op on existing key) → returns stale plan + duplicate insertion-order key + byte-gauge drift. Unreachable (64-bit FNV collision) but the guard is a no-op. Use `[]=` / erase-then-insert.
- **#663 (57769145)** `test-ui-jira-deterministic-backend.sh:75` — `passed=0 failed=0` (filter matches nothing / all skipped) exits 0 → silent false-pass bucket-E gate. Add a positive-test floor.
- **#657 (bf921a6b)** `docs/CONTEXT.md:77` stale line refs `AppController.cpp:574/:595` (now 826/880). Use symbol-only refs.
- **#653 (9ae9c24e)** `tests/agent-eval/code-review/cr-dpapi-secret-loss.json:37` — fixture shows 3 unguarded data-loss sites but `expectedFindingCount=1`; a more-thorough prompt scoring 3 would regress to 0.0 (false gate fail). Trim to 1 site or set count=3.
- **#643 (feb9d903)** `TrackerFieldCatalogPure.cpp:195` dead `is_number_unsigned()` branch (unreachable after `is_number_integer()`); latent narrowing via `get<long long>()`. Reorder or drop.
- **#640 (bdd2644a)** `subagent-eval-flywheel.md:9` + **#623 (0772d4be)** `bug-report-font-redaction-censor.md:5,9,96` + earlier batch's #747 — same broken AGENTS.md `§ Plan *` cross-links (restructured to `§ Process rules § Plan-doc family`) + a stale in-flight-branch claim + a `log-a-bug-github.md` link now under `shipped/`.
- **#610 (f938ac5b)** `verify-cr-reply.sh:20` stale usage-example path `scripts/dev/verify-cr-reply.sh` (moved to `agents/scripts/core/`).

**Cluster (confirms batches 3–4):** Pillar-2 **sync-I/O-on-render survived decompositions** — now #611 (CRITICAL) joins #761/#732/#767/#892. Strong candidate for a single targeted audit + the accepted off-thread/`MarkPrefsDirty`/snapshot-on-open patterns. Plus the **bucket-E/gate false-pass-on-0-tests** recurrence (#663 + batch-4 #719). #670 is the one genuinely user-facing *correctness* bug (wrong Jira status).

## Batch 4 — PRs #708–808 (100-PR workflow sweep, 2026-06-07)

Coverage: **100 reviewed — 14 with findings, 75 clean, 11 fully superseded.** Net: **1 CRITICAL, 4 HIGH, 5 MEDIUM, 12 LOW.**

### CRITICAL
- **#761 (8b5a39f1) · `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:191`** — `DrawCallstackProcessControls` (on the render path) synchronously runs `p4 changes -r -m 1 -s submitted //...@start,end` via the blocking `P4RunCommand` subprocess on the UI thread, no cue, when the "or day" date picker is confirmed. Server-wide depot range round-trip can exceed 100 ms → freeze (Pillar-2; checklist explicitly names p4 as must-be-off-thread). Lone on-thread blocking call in a file that offloads everything else; relocated verbatim by #761's decomposition, still alive. **→ Issue candidate.** Fix: `LaunchBackgroundTask` + `PostToMainThread`, "Resolving CL…" status.

### HIGH
- **#732 (02eb69c8) · `SmatchetPreferencesUi_Templates.cpp:78`** (+90,102,127,167,179,191,216) — duration-suggestion & work-log-template sub-tabs call `SaveDurationSuggestions`/`SaveCommentTemplates` → `ConfigManager::Save` (full read-modify-write + DPAPI encrypt + disk write under 2 mutexes) **synchronously on every reorder/delete/add click** on the render thread. Sibling sub-tabs already use deferred `MarkPrefsDirty(d)`; these two are the survivors on the sync path. Fix: mutate `d.cfg.*` + `MarkPrefsDirty(d)` (or `ConfigSaveWorker`).
- **#784 (0c62b21c) · `agents/scripts/core/postmortem-owed.sh:54`** — `has_entry()` dedup regex requires the `PR ` prefix, so it matches only the FIRST PR in a multi-PR ledger entry (`## … PR #N, #M …`); comma-joined trailers (`#906/#907/#908`, `#774/#776/#778` …) are re-flagged "postmortem owed" every SessionStart despite an existing entry. **This is the source of the recurring postmortem-owed nudges.** Fix: match a bare `#N` token regardless of `PR ` prefix.
- **#789 (6987b7d5) · `scripts/dev/pre-ship.sh:126`** — markdown-lint step hardcodes `python3` (bypasses the repo's `resolve_python()`), so on Windows local the store-stub `python3` (exit 49) makes `pre-ship` print "FAIL — fix the markdown findings" even when docs are clean. Defeats the local half of the gate. Fix: resolve a working interpreter (`python3`/`python`/`py`) and fail loudly only if none runs.
- **#807 (fe06fa23) · `README.md:62,91,96`** — onboarding "one-command build" `scripts/dev/build_and_run.ps1` was relocated to `scripts/dev/local/` (file not found at origin/develop) AND the README claims it auto-bootstraps vcvars ("no Developer Prompt needed") but the chain never invokes `with-msvc.ps1`/vcvars → `cl.exe not found` from a plain shell. Fix: correct the path + the bootstrap claim (or wire the bootstrap). (2 MEDIUM, grouped.)

### MEDIUM
- **#767 (802402c3) · `SmatchetViewsDashboardUi_widgets.cpp:276`** — `ListCachedProjects()` (ifstream + JSON parse + v3 migrate + sort under global mutex) called **every frame** the project-pill popup is open. Sub-frame today (16-entry cap) + matches the accepted sibling convention (`SmatchetProjectPicker`, `SmatchetPreferencesUi` — see #892), so MEDIUM. Fix: snapshot on popup-open into `UiDrawSession`.
- **#746 (4d166612) · `scripts/dev/pre-ship.sh:93`** — comment claims "(staged, unstaged, committed)" but the bare `git diff` captures unstaged only; a staged-never-committed-no-further-edit file is clang-format/lint-skipped (silent false-pass). Fix: `git diff HEAD` or add a `--cached` pass.
- **#719 (78e19958) · `scripts/dev/test-ui-funcsize-window-render-smoke.sh:69`** — a run with `passed=0 failed=0` (filter matches nothing after a rename) exits 0 green; only `FAILED!=0` is checked. Zero-coverage green on a no-visual-validation gate. Fix: require positive count.

### LOW (12)
- **#788 (19779297)** `test-backlog-counts.sh:53` redundant `|| echo 0` double-emits for empty categories; `:61` regression-guard regex omits `debt`; `AGENT_SELF_IMPROVEMENT.md:149` § Index table omits the `debt` row.
- **#789** `md_lint.py:38` MD028 no fence tracking (latent false-positive on a fenced blockquote example); `:27` `git ls-files` returncode unchecked → silent clean from a non-repo dir.
- **#759 (0268a29b)** `with-msvc.ps1:85` when the pinned toolset isn't installed, falls back to first install but still forces `-vcvars_ver=<pin>` → vcvars fails silently, build runs in non-MSVC env (opaque `cl.exe not found`); contradicts the "exit 2" contract. (Related to the toolset-pin friction in this very session.)
- **#755 (1d88d25c)** `test-rig.md:55` names `AppControllerDepsAdapter.cpp` (renamed to `GridContextDepsAdapter.cpp` by #945).
- **#747 (ec0d1770)** `agent-kit-productization.md:5` cross-links 5 AGENTS.md subsections that no longer exist (restructured to nav-only).
- **#722 (6990b8bf)** `docs/CONTEXT.md:13,14` broken header paths — missing `Tracker/` subdir (`LabelEditDiffPure.h`, `GitHubClientHelpers.h`).
- **#709 (777dc48d)** `SubprocessCapture.cpp:475` iterator-pair `std::string(char*, const char*)` is a hard compile error on the non-glibc/BSD fallback path (unreachable on Win/Linux glibc matrix, but malformed C++); `:540` POSIX pump FD_SETs an EOF'd fd forever → 100% CPU spin if a child closes one pipe but keeps running (test-only POSIX path).

**Cluster:** Pillar-2 **sync-I/O-on-render-thread survived decompositions** — #761 (p4, CRITICAL), #732 (config save, HIGH), #767 + batch-3 #892 (cache read, MEDIUM). Worth a targeted audit + the accepted `MarkPrefsDirty`/`LaunchBackgroundTask`/snapshot-on-open patterns. Also a **gate-fails-open / false-pass** recurrence (#719/#746/#789/#788) consistent with batch 3's #834/#918.

## Batch 3 — PRs #809–925 (100-PR workflow sweep, 2026-06-07)

Ran via the `historical-review-sweep` workflow (100 code-review agents, concurrency-capped, structured output). Coverage: **100 reviewed — 20 with findings, 65 clean, 15 fully superseded.** Net: **5 HIGH, 4 MEDIUM, 14 LOW.**

### HIGH
- **#854 (7d7b2f01) · `Source/Core/src/Sync/OfflineQueueService.cpp:617`** — scalar conflict-resolve writes the chosen DISPLAY string verbatim into the payload key, but every ID/object/array-valued field (single/multi-select, status, priority, issuetype, user, component, cascading, labels) stores an object/array payload (`{"id":"3"}` …). Resolve clobbers `{"priority":{"id":"3"}}` → `{"priority":"Low"}`; `ReplayOneFieldEdit` PUTs it → Jira 400 → retries → dead-letter → **user's resolved offline edit silently lost** (despite "edit re-queued" toast). "Use Mine" equally broken. Test uses a bare-string payload so CI never sees it. **→ Issue candidate (data-loss).** Fix: route scalar "Use Mine" through the unchanged-payload path; for "Use Theirs"/"Save" rebuild via `BuildFieldPayload/BuildValue`; only overwrite with a bare string when the existing value is itself a string. Add an object-payload test.
- **#892 (1e085193) · `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:324`** — `DrawTrackerRecentProjects` calls `FieldCatalogCache::ListCachedProjects()` **every frame** the Prefs→Tracker tab is open → synchronous `ifstream` read + full JSON parse + schema migration + sort on the UI render thread (Pillar-2 violation). Slow/locked/large cache file stalls the window. (Relocated by the refactor, but line 324 is the surviving call.) **→ Issue candidate (UI freeze).** Fix: read+filter once on tab-open, cache in `UiDrawSession`, refetch only on backend change / Forget; or load on a worker.
- **#834 (2fd9ef33) · `scripts/dev/coverage.sh:168`** — OCC exclude patterns use the bare token `Ui` as an unanchored case-insensitive substring → excludes far more than `Ui/`: the whole strict-zone `Source/Core/src/Commands/Builtin/` (`Builtin`), `CommandPaletteUi.cpp`, `Commands/Scenarios/UiTestScenario.cpp`, `AiContextBuilder.cpp` (`Builder`). The **blocking** coverage gate's 67% baseline is computed on a too-small surface; strict-zone code uncounted; tests there can't move the gate. Fix: anchor to a dir boundary (`Source*Core*src\Ui\`), re-verify the captured file list, re-baseline.
- **#918 (c2287c02) · `scripts/dev/coverage-delta-gate.sh:79`** — classifier `'*'*) return 0` (meant for block-comment continuations, already handled by the state machine) instead only fires on real statements starting with deref/indirection: `*out = compute();`, `*it = next();` → classified no-runtime-surface → a diff of output-pointer writes is falsely auto-EXEMPTED from the required test-delta gate (false PASS, unsafe direction; violates the file's own CONSERVATIVE contract). Fix: drop the redundant comment-continuation cases or tighten `'*'*` to a true continuation shape.
- **#919 (1d83a108) · `tests/bats/merge_watcher.bats:354`** — the two new `handle_pass` tests (enqueue-on-queue, merge-when-no-queue — the entire point of #919) FAIL on Windows: (1) the gh stub selector `case "$2 $3"` no longer matches now that #919 moved the discriminator to `$1/$2` (`gh pr merge`/`gh repo view`), falls through to `exit 0`; (2) the `PATH=$STUB_BIN gh` trick is documented non-functional on Windows (native `shutil.which` skips extensionless stubs → real `gh.exe` runs). So merge-queue-safety logic has **zero working coverage**; CI misses it (no bats on the Windows runner). Fix: drive the 3 tests through the `mw.squash_merge_pr`/`_gh_owner_repo` monkeypatch seams; if keeping a stub, name it `gh.cmd` and use `case "$1 $2"`.

### MEDIUM
- **#918 · `coverage-delta-gate.sh:77`** — `'/*'*) return 0` exempts open-and-close comment lines with trailing code (`/* note */ launchTask();`); mirror hole at `:185` (`*/` + code → `continue` drops the statement). Both silently exempt real surface. Fix: strip the comment span, classify the residual code.
- **#814 (c0e4f4e5) · `agents/scripts/project/migrate-bugs-to-issues.sh:12`** — usage header claims `--apply` "create Issues + move debt + prune bug.md" but apply only creates Issues (debt/bug.md reconciliation is manual by design). Fix: correct the header.
- **#813 (f3652350) · `project.config.json:146`** — `governance.loop_mode` is a dead knob: `_doc` (+ AI_POLICY.md, ship-loops.md) claims it's the SessionStart default, but the only consumer `clear-session-context.sh` reads only `SMATCHET_LOOP_MODE` env and hardcodes `in` else. Harmless only because both equal `in`. Fix: read `governance.loop_mode` as the unset-env fallback, or reword the docs to "advisory/unconsumed".
- **#810 (1ec969d5) · `docs/agent-rules/issue-triage.md:68`** — documents the `[issue-propose]` line as `(P<k>, area:X)` but `issue-sweep.sh:111` emits priority only `(P0)`/`(P1)`, no area. Fix: drop `area:X` from the doc or append the area label in the emitter.

### LOW (14)
- **#917** `test-lua-mirror-smoke.sh:58` `mapfile` breaks on macOS bash 3.2 (no summary line → silent gate degrade); not on CI/Win targets.
- **#915** `check-test-list.sh:29` + `tests/CMakeLists.txt:608` — substring (not path-boundary) match → a basename that's a suffix of a referenced file is falsely "referenced" → uncompiled test (the exact false-green this guard prevents).
- **#914** `infra.md:31` `test-delta-test-light-exemption` still `open` but shipped (#918) → mark applied/archive; leave sibling `pr-burst-guard` open.
- **#913** `guard-shared-tree.sh:51` doesn't exempt `-C <worktree>`-targeted git ops (false-deny) unlike the parity helper in `guard-head-drift.sh`; `SMATCHET_ALLOW_SHARED_SWITCH=1` overrides.
- **#911** `test-config-globs.sh:72` process-substitution helper crash isn't caught by `set -e` → loop reads 0 globs → PASS (fails OPEN vs documented fail-CLOSED). Capture into a var + check status.
- **#909** `build.md:31` broken ref `scripts/dev/build_and_run.ps1` → `scripts/dev/local/build_and_run.ps1`.
- **#908** `CMakeLists.txt:664` sol2 RE-RUN CLEANUP group is dead (PRIMARY patch FATALs first on a double-appended tree); harmless (SHA-pin re-fetches fresh) but comment is wrong. Delete the dead group or reorder + fix comment.
- **#888** `docs/harness/pi/README.md:57` overstates "read-only enforced by tool scoping" — read-only agents with `shell` get `bash` (can write). Soften wording.
- **#887** `agents/_shared/workflows/pre-merge-review.js:106` `filter(Boolean)` destroys positional identity → on partial reviewer failure the judge mis-attributes code-review vs security-review punch lists. Reference `reviews[0]/[1]` directly.
- **#872** `followup-due-nudge.sh:186` unguarded `"${warns[@]}"` under `set -u` (bash<4.4) in the due-path; mirror line 195's `:-` guard. Dormant on supported toolchains.
- **#855** `build.md:19` broken refs `scripts/dev/build_and_run.ps1`/`build_standalone.ps1` → under `scripts/dev/local/`.
- **#853** `docs/adr/0016-…md:30` stale line-pin `OfflineQueueService.cpp:788` (write moved to :1008). Reference the symbol instead.
- **#814** `issue-sweep.sh:75` relabel verdict fires on missing-priority but apply only adds `bug` → perpetual no-op RELABEL inflating the acted count; `:7` header lists verdicts (`mirror-then-close`/`flag-stale`) the script never emits.
- (Plus the #810/#813/#814 MEDIUMs above each had adjacent doc-vs-code drift.)

**Recurring classes:** (a) gate scripts failing OPEN / false-exempting (#834/#918/#911/#915) — highest-value, undermines blocking gates; (b) stale doc line-pins + moved-script refs (#909/#855/#853/#914/#810/#813); (c) `set -u`/portability latent shell bugs (#872/#917). The gate-false-pass cluster (#834/#918) is worth prioritising — they let untested/uncovered code merge green.

## Batch 2 — PRs #926–946 (swept 2026-06-07)

20 PRs (#946,945,944,942,941,940,939,938,937,936,935,934,933,932,931,930,929,928,927,926). Net: **0 HIGH, 2 MEDIUM, doc-drift LOW cluster.** Fully superseded (nothing alive): #941, #936, #929. Clean: #946, #945 (faithful behaviour-identical extraction — destruction order + ADR-0012 atomics verified), #944, #942, #939, #933, #927, #934.

### MEDIUM
- **#935 (e3996308)** — `docs/agent-rules/merge-gates.md:10` quotes the meant-to-block allow-list regex as `Coverage|Sanitizer|Bucket-`, but live `merge-gates.sh:365` is `Coverage|Sanitizer|Bucket-|Perf PR-fast` (the 4th pattern added later by #942). A reader concludes a red `Perf PR-fast` is non-blocking when it actually blocks — dangerous doc-vs-code drift on a merge-gate. Same omission (LOW) at `merge-gates.sh:9` header comment and `AGENTS.md:49`. **Fix:** append `|Perf PR-fast` (or soften to "e.g.") at all three sites; consider a selftest asserting the regex literal mirrors into the docs.
- **#928 (394746a8)** — `Source/Core/src/Tracker/CONTEXT.md` documents as *live data-model facts* terms that exist nowhere in `Source/` on develop: `:43-47` `TrackerActivityEntry` + `GroupMemberCache`/"group roster" (Slice 2 never landed, gated behind unshipped multi-grid), and `:26` claims `ITrackerCollaboration` "handles ... per-user activity" but the interface has no `FetchUserActivity` (the PR's own plan says no activity endpoint exists). Canonical leaf-doc describes vaporware → readers chase non-existent symbols. **Fix:** mark both as planned/forthcoming or revert the CONTEXT.md additions until the backend slice ships.

### LOW (notable)
- **#940 (af465eb8)** — `docs/adr/0018-multi-grid-pane-contexts.md:6` broken ref `docs/plans/multi-grid-tabs.md` → should be `docs/plans/shipped/multi-grid-tabs.md`; `:3` status still `proposed` though Slice 1 shipped; stale `AppController.h` line citations in the design addendum.
- **#937 (b5716262)** — `scripts/dev/perf-run.sh:152` inline JSON validator checks `data.rows` then top-level `rows`, inverted vs `perf-compare.py extract_rows()` (top-level first); on a malformed file carrying both, perf-run.sh PASSes while perf-compare reads `[]` → false PASS. Match the order.
- **#931 (27e9e428)** — `postmortems.md:269` "### Filed as … (P1, decision-pending)" stale (resolved option B, #933, per RESOLVED note above it); stale `merge-gates.sh:345` line citation.
- **#932 (5f2dd18b)** — `build-quality-velocity-hardening.md:196` status block lists #8/#13 "Parked" but impl-log (:175) records "UN-PARKED → GATE ARMED"; impl-log missing bullets for #920/#925/#926.
- **#930 (7531a53c)** — `session-guard-agnostic.md:75-78` nested-backtick markdown breaks the Perf-gates `N/A` block rendering.
- **#938 (09f4c791)** — `TicketSyncService.test.cpp:427` mislocated comment; `SpinUntil` 400 ms cap is a latent flake-watch (5 new dependents).
- **#926 (c9f0c9dc)** — `data_dependent_windows_smoke.test.cpp`: stale Views-Dashboard probe comment, `"SMAT-1"` vs `"SMAT-TEST-1"` comment mismatch, and `app==nullptr` logs SKIP but records PASS (vacuous-green if harness fails to boot) — `IM_CHECK(app!=nullptr)` would fail loudly.
- **#942 (3e381a8c)** — informational only: armed relative gate inert on 3/4 scenarios (calls<min_baseline_calls) + warmup-contaminated `emaAvgMs` baked into approved baselines (unused by current gate). Calibration-phase state, not a defect.

### Note on doc-drift recurrence
Several findings (#935, #931, #932, #934, #940) are the same class: a later PR changed code/status and left mirror docs (allow-list, postmortem status, plan status, ADR status) stale. Candidate standing gate: mirror-literal selftests + a "doc status vs shipped" check. (#934 itself flagged the #942-induced AGENTS.md staleness — same root as #935.)

## Batch 1 — PRs #947–951 (swept 2026-06-07)

Survivor coverage (alive/introduced at origin/develop): #951 610/610 · #950 1/1 ·
#949 ~305/306 · #948 690/690 · #947 281/284. Net: **1 HIGH, 0 other.**

### PR #948 (2e9a7fbf) — multi-grid Slice 1b: tickets_v2 namespacing migration
- **HIGH · still alive** — `Source/Core/src/AppController.cpp` `InitConfig` migration block (~L1214-1220): the one-time tickets_v2 copy migration stamps legacy rows with `NormalizeViewsBackendKey(backendType)` (the `Initialize` **param**), but the live read/write key is re-derived in `InitBackends` (`AppController.cpp:1344`) from `ConfigManager::Load().TrackerType` — which ignores the ephemeral `--backend-type`/`-b` CLI override (`StandaloneAppBootstrap.cpp`) and the embedder's `options.BackendType` (`SmatchetImGuiHost.cpp:636`). Launch e.g. `Smatchet -b Plane` once on a pre-1b DB whose persisted tracker is Jira → migration copies Jira rows into `tickets_v2` under `"Plane"`, **consumes the `cache_meta` flag permanently**, while the live path reads under `"Jira"` (empty): legacy cache stranded + wrong namespace polluted, never re-migrated.
  **Fix:** run the migration with the resolved live key *after* `InitBackends` re-stamps it (`focusedContext().CacheBackendKeyCopy()`), exactly as `RecreateLocalCacheDatabase` already does (`AppController.cpp:2132`). The 1218-1219 comment's "fixtures use fresh DBs" rationale covers only the env-factory case, not this CLI/embedder one.
  **Route:** user-visible (cached tickets vanish on upgrade for non-default backends) → GitHub Issue candidate (ADR-0014). Originally surfaced as CR-948-1 in the live PR review; this confirms it is **still unfixed on origin/develop**.

### PR #947 (7835dba3) — guard-head-drift worktree git -C + PowerShell
- Clean (no NEW bug in the surviving set). CR-947-1 (trailing-boundary regex) already **fixed by #956** → correctly excluded. CR-947-2 (space-in-`-C`-path false-block, fail-safe) already backlogged in `tooling.md`.

### PR #949 (f7411db7) — perf: per-scope p99Ms in snapshots
- Clean. Percentile math (`ceil(0.99n)` rank, in-bounds for all n), ring wraparound/bounds, and cold-path locking all verified correct on the surviving lines. (Some #949 perf lines were re-attributed to the #963 100 Hz change and excluded.)

### PR #950 (fef198e4) — config strict=false
- Clean. Sole survivor `project.config.json` `branch_protection.strict: false` — valid JSON, matches the documented merge-throughput decision; required-contexts lists unchanged.

### PR #951 (258116f2) — multi-grid Slice 1c: pending-queue BackendKey + replay
- Clean. All SQLite column↔bind index orderings verified after the inserted `backend_key`; migration idempotent/transactional/empty-key-guarded; replay filter never replays against a wrong backend nor drops rows. (Earlier live-review CR-951-1 was a *verify* item about the dead-letter restore UI path; the surviving cache path `RestoreDeadPendingCreate` re-queues under the original key correctly.)
