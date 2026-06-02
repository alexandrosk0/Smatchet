# Agent self-improvement — infra

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-06-02 · orchestrator · [infra] · P2 — Bucket-E `--spawn` flake NOT eliminated by the warmup gate — reproduces even on an IDLE machine (re-filed; B8 phase-0 archived it prematurely)
  Details: B8 phase-0 (PR #711) archived the 2026-05-17 spawn-flake entry as stale on the reasoning "deterministic warmup gate shipped (slice 9) + its runner says 'Closes infra.md P2 line 16'." That conflated *a warmup-gate test existing* with *the flake being fixed*. During B8 phase-1 L1 authoring (`command_palette_inline_typing.test.cpp`), the new TU flaked ~2/6 green; isolating the variable by running the existing known-good `Views` columns-reorder TU under the same `--spawn` path showed it ALSO flaked badly. **Initial guess that it was load-amplified is DISPROVEN**: after the session's builds finished, a fresh idle-machine control run was `Views` **0/4 green** + L1 2/6 — i.e. just as broken (worse) idle as under load. So it is a genuine bucket-E spawn/harness break on this host, NOT CPU contention, and the "tolerated-with-retries" assumption is fragile (a 0/4 control TU can't be retried to green). Spawn child-logs (`%LOCALAPPDATA%/Temp/Smatchet-spawn-<pid>-<port>.log`) are **empty** (0 bytes) on failing runs, so the failing `IM_CHECK` is not surfaced — a compounding diagnosability gap that blocks root-causing.
  Concrete next action: (1) make the spawn child-log actually capture the ImGui Test Engine `IM_CHECK` / KO output (currently 0 bytes on failure) so flakes are diagnosable; (2) diagnose the residual race — leading hypotheses: MCP-ready signal vs `ui_test.run` queue dispatch, or the test-engine frame budget under CPU contention; (3) interim: add a retry-once-on-flake wrapper to the `scripts/dev/test-ui-*.sh` runners. The spawn warmup gate stays valuable; this entry tracks the residual it did not close.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-05-31 · orchestrator · [infra] · P3 — merge-watcher autostart bridged by a hand-patched launcher bat until #644 lands
  Details: Restarting the daemon this session surfaced two stacked breakages. (1) The auto-generated launcher bat + two zombie daemons pointed at the pre-migration `scripts/dev/merge-watcher.py` / `merge-gates.sh` paths (moved to `agents/scripts/core/` by `split-scripts-build-vs-agentic`) → every poll `EXIT_127`. (2) After re-pointing, the current installer's inline-cmd task action drops the `Git\bin` PATH prepend the old bat carried, so `_resolve_bin("bash")` resolves the WindowsApps WSL shim (`merge-watcher.py`'s override only caught `System32\bash.exe`) → `EXIT_127` with collapsed `C:\` paths (`/bin/bash: C:DevSmatchet...merge-gates.sh: No such file`). Restored locally with a hand-written `%LOCALAPPDATA%\Smatchet\merge-watch\run-merge-watcher.bat` (correct path + `Git\bin` first on PATH); the `SmatchetMergeWatcher` task now points at it and polls healthy (`BLOCKED`, CI 4/4). PR #644 fixes the daemon-side override to also catch `\windowsapps\`, making PATH order irrelevant. NB: the current daemon uses `clone_path` only as a `gh` cwd and does a server-side REST squash-merge — no local checkout/pull/branch-d — so the 2026-05-30 "HEAD-thrash" entry below appears stale for the merge path.
  Concrete next action: after #644 merges to develop AND the main clone at `C:\Dev\Smatchet` pulls it, re-run `powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-install-autostart.ps1` to restore the canonical inline-cmd task action, then delete the bridge bat. Optionally generalize: have the installer prepend `Git\bin` defensively so a stale daemon checkout can't re-hit the WSL-bash trap. ~10 min.
  Status: open
  Last-reviewed: 2026-05-31

- 2026-05-30 · orchestrator · [infra] · P2 — merge-watcher daemon runs in the orchestrator's main clone → mid-session HEAD thrash + false working-tree reads
  Details: the `SmatchetMergeWatcher` daemon (`scripts/dev/merge-watcher.py daemon`) operates on the **same** working copy the orchestrator edits in. Its git-janitor step (`git checkout develop` + `git pull` + `git branch -d`) runs underfoot whenever a registered sibling PR passes gates. Observed 5× in a single session (2026-05-30): HEAD swapped `fix/...` → `plan/separate-agents-repo` → `plan/reduce-source-comment-bloat` → `develop` between tool calls. Two concrete harms beyond the documented "re-check HEAD before commit" guard (AGENTS.md § Git/p4 discipline): (1) `git checkout <branch>` aborts when the daemon left the tree dirty with its own WIP, blocking the orchestrator; (2) **verification reads off the wrong branch** — a `with-msvc-env.sh` read during a swap showed the toolset pin missing and a backlog entry was filed as a regression that does not exist on develop (the pin is present), retracted only after `git show origin/develop:<file>` cross-check. The guard is a band-aid; the root cause is the shared working tree.
  Concrete next action: run the daemon in a **dedicated clone or git worktree** (e.g. `.merge-watcher-clone/` or a `git worktree add`), so its checkout/pull/branch-delete never touches the orchestrator's HEAD or working tree. Registry `clone_path` already exists — point the daemon's git ops at an isolated path. Alternatively make the janitor step `git -C <isolated>` only and never `checkout` the shared tree. ~2-4 h (daemon git-op audit + isolation + restart-task wiring). Until then, agents MUST `git show origin/<branch>:<file>` (never the working tree) to verify any path-bearing claim mid-session.
  Status: open
  Last-reviewed: 2026-05-30

- 2026-05-29 · orchestrator · [infra] · P2 — De-Smatchet-ify the portable agentic layer (close the project-literal baseline)
  Details: agentic-layer-project-independence shipped the portable/project STRUCTURE (agents/core vs project, project.config.json seam, docs taxonomy) but the portable files still embed ~157 project literals in prose (`docs/high-integrity/portable-purity-baseline.txt`). `test-portable-purity` baselines this and blocks NEW leakage, but reuse today means copy + adapt the prompts, not copy verbatim.
  Concrete next action: rewrite `agents/core/*` + `docs/agent-rules/*` prose to reference `project.config.json` keys instead of hardcoded `Smatchet`/`Source/Core`/preset literals; shrink the baseline toward zero. Largest chunk is the 15 core-agent prompts. Incremental — drop baselined entries as files are cleaned.
  Status: open
  Last-reviewed: 2026-05-29

- 2026-05-26 · orchestrator · [infra] · P2 — Advisory CI jobs need step-level non-blocking templates
  Details: PR #460 exposed that job-level `continue-on-error: true` was not enough to keep Bucket-C/Bucket-E soak-window failures from surfacing as red PR checks. Bucket-C and Bucket-E both needed step-level `continue-on-error` on the failing scenario/diff step, with artifact upload keyed off `steps.<id>.outcome` or `always()` so diagnostics still survive.
  Concrete next action: add a shared workflow snippet or documented pattern for advisory jobs: give the risky step an `id`, set `continue-on-error: true` on that step, and upload artifacts using `if: ${{ steps.<id>.outcome == 'failure' }}` or `if: always()` as appropriate. Then audit existing advisory jobs for the same shape. Estimated cost 30 min.
  Status: open
  Last-reviewed: 2026-05-26

- 2026-05-18 · orchestrator · [infra] · P2 — Whisper Phase H — `SMATCHET_WHISPER_LOCAL_BACKEND` default flip OFF→ON decision (binary size vs. local-by-default UX)
  Details: User's chosen Whisper backend mode is "local default, cloud fallback" (per the dictation plan's locked decisions). Phase C shipped the local backend behind a new sub-option `SMATCHET_WHISPER_LOCAL_BACKEND` that defaults OFF — so default-built users get the plugin compiled in (+10.71 MB binary delta from Phase C) but the actual whisper.cpp link is opt-in. With sub-option OFF: `WhisperLocal::LoadModel` returns "local backend not built" and the mode router falls back to cloud. With sub-option ON: full whisper.cpp + ggml link, binary delta unmeasured (plan's open question #1 flagged a >50 MB risk threshold). Result: the locked "local default" UX does not match the shipped default for users who don't pass `-DSMATCHET_WHISPER_LOCAL_BACKEND=ON`. Three resolution paths: (a) flip default ON, accept binary growth, single-stage user experience matches the plan; (b) refactor whisper.cpp link to a runtime-loaded DLL — plugin compiles in but the whisper code only joins the process on first local-mode transcription, deferring the size hit until needed; (c) keep current state + document the OFF default as the canonical user UX, downgrading the "local default" decision to "cloud default, local available with rebuild". Option (b) is cleanest but a multi-day refactor.
  Concrete next action: measure the binary delta when `SMATCHET_WHISPER_LOCAL_BACKEND=ON` (one `cmake -D... && cmake --build` cycle, compare exe size pre/post). If <50 MB delta — flip default ON. If >50 MB delta — author a separate "whisper-dll-loader" plan doc for option (b) and stay on the current sub-option OFF default in the interim. ~30 min to measure; multi-day if option (b) is chosen.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · test-author · [infra] · P2 — AiClientFactory has no test-injection seam for bucket-E success-path coverage
  Details: `tests/ui/ai_prefs_autosave_flow.test.cpp` Variant 2 (`VerifyOnSave_TestConnection_SetsResult`) falls back to driving libcurl against `http://127.0.0.1:65530` (an unbound loopback port → immediate ECONNREFUSED) to exercise the Failed: branch of the Preferences Test-connection probe. This tests every layer EXCEPT the success-completion branch — `g_ui.assistantPrefsTestResult == "Verified."` + `assistantPrefsTestResultType == 1` is never reached. Adding a static test seam to `AiClientFactory` would let the variant assert the success path against an in-process stub `IAiClient` that returns immediately from `ProbeReachability` + emits a synthetic chat delta on `SendStreaming`.
  Concrete next action: add `AiClientFactory::SetTestOverride(std::unique_ptr<IAiClient>)` + `ClearTestOverride()` statics gated on `#if defined(SMATCHET_BUILD_UI_TESTS)`. `MakeAiClient(...)` consults the override before the provider-kind switch. Stub `IAiClient` class lives under `tests/ui/support/`. Add Variant 4 `VerifyOnSave_TestConnection_StubSuccess_VerifiedLineLands` to the autosave-flow TU. ~3 h (seam + stub + variant + clear-on-test-end semantics).
  Status: applied (2026-05-18, PR feat/ai-client-test-override — `AiClientFactory::SetTestOverride(TestOverrideFn)` lands; `runProbe` extracted to `AiPrefsTestConnection::TriggerProbe` to bypass the host-coupled `Preferences -> Assistant tab -> Test connection` path; V2 + V3 lift to live coverage. Follow-up tracked: 5-line UI rewire in `SmatchetPreferencesUi.cpp` once `whisper-dictation-phase-f` PR #219 merges.)
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [infra] · P3 — Worker-side per-chunk dispatcher post coalescing for AI stream
  Details: `AiAssistantController::RunRequest`'s `onDelta` posts one task to `MainThreadDispatcher` per provider chunk. OpenAI streams 5–10 chunks/s, Anthropic ~30/s, Ollama ~50+/s for fast local models. The dispatcher queue stays drained but the per-frame `assistantStreamBuf.append` cost + redraw cost is proportional to chunk rate. Coalesce on the worker into 50–100 ms windows before posting; one task per window appends the concatenated chunks.
  Concrete next action: add a small per-turn `std::string pendingChunks_` + `std::chrono::steady_clock::time_point lastPostAt_` on the worker side; flush when (a) pendingChunks_.size() > 4 KB OR (b) 80 ms elapsed since `lastPostAt_` OR (c) `isFinal`. Wire through `onDelta`. ~1 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `ImGuiListClipper` in `DrawHistoryArea` for long assistant histories
  Details: `SmatchetAiAssistantUi.cpp:96-101` (now ~110 after PR #176) renders every `AiMessage` via `TextWrapped` per frame. A 10 000-turn session reflows the entire history every frame at 144 Hz. Use `ImGuiListClipper` once messages exceed ~50 to skip rendering off-screen entries. Currently latent (real sessions are short), but cheap to add and future-proofs the panel for "ask Claude to refactor the whole codebase" sessions.
  Concrete next action: wrap the history `for` loop in `ImGui::ListClipper` with `clipper.Begin(history.size())`. Verify `TextWrapped` row-height estimation works (or measure once + cache). ~45 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `AiContextBuilder::BuildActiveTicketBody` O(N) ticket scan
  Details: Linear scan over `tickets` to find the active id at [`AiContextBuilder.cpp:151-153`](../../../Source/Core/src/AiContextBuilder.cpp). For 10 K-ticket views the Send button blocks proportionally. `IdIndex` map already exists elsewhere in the codebase; pass through `Inputs` or accept a pre-resolved `const CachedTicket*`.
  Concrete next action: extend `AiContextBuilder::Inputs` with `const CachedTicket* PreResolvedActiveTicket = nullptr`; populate from the UI side via `IdIndex` lookup. Fall back to the existing scan when null. ~30 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `AgentsMdLoader` reads `maxBytes + 1` even when file is smaller
  Details: [`AgentsMdLoader.cpp:45-46`](../../../Source/Core/src/AgentsMdLoader.cpp) `out.resize(maxBytes + 1)` then reads `maxBytes + 1` bytes regardless of actual file size. Functionally correct (over-cap detection works), but each load reads up to 64 KB+1 even for a 1 KB agents.md. Trivial waste; matters only when invalidation happens on a hot Preferences-change loop.
  Concrete next action: stat the file first; cap the read at `min(maxBytes + 1, file_size)`. ~15 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [infra] · P3 — `SmatchetAiAssistantUi` silently truncates 8 KiB paste
  Details: `InputTextMultiline` is sized to `s_inputCharBuf.size()` (8 KiB). User pasting 9 KiB has the suffix silently dropped with no toast.
  Concrete next action: detect truncation on paste (compare `clipboard.size()` against `kInputBufCap` in an `ImGuiInputTextCallback`); emit a toast naming the dropped suffix length. ~45 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · unreal-bridge · [infra] · P2 — DX12 backbuffer readback for screenshot diff (Phase 7 bucket C)
  Details: Phase 7 (`test-phase-7-screenshot-diff`) ships PPM capture wired into `Source/Standalone/main.cpp:569` via `glReadPixels(GL_FRONT, GL_RGBA, ...)`. The `debug.window.screenshot` flag pair (`UiDrawSession::requestScreenshot{,Path}`) flips on both Standalone and DX12 builds but DX12 never consumes it — Unreal owns the swap chain and has no equivalent backbuffer-readback path wired in `SmatchetCore_DX12`. Bucket-C verification therefore covers Standalone-only.
  Concrete next action: add a DX12-side equivalent in `Source/UnrealPlugins/SmatchetImGuiPlugin/` (or wherever the swap-chain present hook lives) — `ID3D12GraphicsCommandList::CopyResource` from the backbuffer to a readback heap, then memcpy to the same PPM writer used by Standalone. Estimated cost ~4-6 h (DX12 resource-state transitions are fiddly). Until then, Phase 7 gates only run on Standalone — Unreal-shipped builds skip bucket-C without notice.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [infra] · P2 — `IsTrackerTransportErrorText` is cpr-tainted by location; needs cpr-free TU split
  Details: The symbol lives in `Source/Core/src/TrackerHttpUtils.cpp` next to cpr-using HTTP wrappers. Any test target that touches `OfflineQueueService.cpp` (or any other consumer of the symbol) must either (a) link `TrackerHttpUtils.cpp` + cpr (bloats test exe with the entire HTTP layer — what PR E + PR F ended up doing after rebase), or (b) re-define the symbol locally (drift surface — what PR E shipped initially before being forced into option (a) by the rebase cascade). Instance of the Pure-helper TU-split recipe — see AGENTS.md § Orchestrator delegation packet § Pure-helper TU-split recipe.
  Concrete next action: lift the pure error-text classifier into `Source/Core/{include,src}/TrackerHttpErrorText.{h,cpp}` (same pattern as `P4BlameParse`, `TrackerLabelsPure`, `TrackerDateTimePure`). The classifier is <100 LoC, zero `<cpr/*>` includes. `TrackerHttpUtils.cpp` rewires its single call site via using-decl. Estimated cost 30 min refactor + ~5 cases / ~20 assertions doctest coverage.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [infra] · P2 — Shared `tests/support/TestEnvGuard.h` should host the ConfigManager-redirect + audit-redirect RAII guard
  Details: PR E (`OfflineQueueServiceRuntime.test.cpp`) hit `ConfigManager::Load()` returning `ReadOnlyMode=true` on fresh-install when no `smatchet_config.json` exists in cwd (`Source/Core/src/ConfigManager.cpp:713-714` safety branch). Every `QueueCreateOffline` / `QueueFieldEditOffline` call silently returned 0 because the service short-circuits on read-only mode. Fix landed test-side as a private `TestEnvGuard` RAII wrapper. PR F (`TicketSyncService.test.cpp`) hit the same problem and reimplemented its own guard. Phase 4 (Config + schema migration) will hit it again.
  Concrete next action: hoist a shared `tests/support/TestEnvGuard.h` that creates a unique temp dir, points `ConfigManager::SetUserDataDirectory` at it, writes a minimal `smatchet_config.json` with `read_only_mode=false`, and on dtor restores + cleans. Estimated cost 20 min consolidate the two existing copies + replace one in each test file.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [infra] · P2 — Phase 1 deferred 4 tracker units pending production-side TU split
  Details: `tests/Core/{IssueDraft,IssueCreatePipeline,TrackerFieldValueParser.extended,TrackerFieldValueUtils}.test.cpp` shipped (171 new CHECKs; 5/5 high-risk mutation sanity green). Plan-named units NOT shipped because each has pure helpers buried under ImGui / AppController / JiraClient / cpr includes — instance of the Pure-helper TU-split recipe (AGENTS.md § Orchestrator delegation packet § Pure-helper TU-split recipe):
    - `Source/Core/src/TrackerLabelsEditor.cpp` — parse/serialize round-trip + dup-detection lives next to ImGui input handling.
    - `Source/Core/src/TrackerDateTimeFieldEditor.cpp` — ISO-8601 parser lives next to ImGui calendar widget.
    - `Source/Core/src/TrackerFieldPayload.cpp` — payload builder pulls JiraClient → cpr → ConfigManager transitively.
    - `Source/Core/src/TrackerFieldCatalog.cpp` — catalog merge / lookup lives next to JiraClient catalog-fetch surface.
  Concrete next action: per-unit TU split (lift pure helpers to a sibling `*Pure.cpp` + matching header with no ImGui/cpr includes), then add the doctest in a follow-up phase. Estimated cost ~1 h per unit (4 h total). Bonus: `IssueCreatePipeline::ApplyPostIssueSteps` decision logic also deferred — needs `ITrackerClient` mock fixture (Phase 3 HTTP layer). Pick up after Phase 3 ships the HTTP / SQLite fixtures.
  Status: open
  Last-reviewed: 2026-05-17
