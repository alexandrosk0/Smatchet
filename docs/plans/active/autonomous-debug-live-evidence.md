<!-- index-summary: close the live-process evidence gaps in the autonomous debug loop — agent-debug preset, log read-back, and on-demand self-minidump for hang diagnosis -->
# Plan — Live-process evidence for the autonomous debug loop

> **Slug**: `autonomous-debug-live-evidence` (matches this file's basename without `.md`).
>
> **Status**: `active` — authored 2026-08-16; `grill-with-docs` pass complete (see § Verification), unstarted.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Origin**: a capability-gap assessment of this repo's agentic debugging harness against Cursor Debug Mode (2026-08-16). The assessment found the harness ahead on *reproduction* and *credential-free running* — what [`docs/adr/0009-autonomous-debugging-no-creds.md`](../../adr/0009-autonomous-debugging-no-creds.md) set out to deliver — and behind on nothing Cursor ships, but with holes in what the loop can **observe in a live process**.

## Context

ADR-0009 gave `debug-detective` a reproducer it can run without a human and without credentials. What it did not give it is a way to observe a process that is already running and misbehaving. Three gaps follow:

1. **The structured agent-debug channel is off in the preset the loop builds from.** `SMATCHET_AGENT_DEBUG` defaults OFF (`CMakeLists.txt:394`) and `ninja-iter-msvc` never mentions it, so `SMATCHET_AGENT_DEBUG_LOG` compiles to `((void)0)` (`tests/_debug/SmatchetAgentDebug.h:370-375`). Observing production-shape behaviour costs a preset switch plus a full rebuild before the first breadcrumb lands.
2. **Nothing reads the runtime log back.** `debug.log` writes into the Logger, but no command returns log content, so an agent must locate and read `%LOCALAPPDATA%\Smatchet\Smatchet-<pid>.log` off disk — fine locally, unavailable to a purely-MCP client (a `--spawn` child, a remote session).
3. **A hang has no capture path at all.** `debug.thread_dump` is a stub (`Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp:103-117`) returning `hardwareConcurrency` and a note. The minidump pipeline only fires on a crash, so a process that wedges without crashing produces no evidence whatsoever.

Gap 3 is not theoretical. `docs/self-improvement/categories/infra/2026-07-05-texture-guard-llvmpipe-spawn-hang.md` is an **open P2**: a *deterministic* deadlock that forced the `Mobile texture-guard smoke` lane to stay advisory, is marked "needs debug-detective", and whose **concrete next action #1 is literally "capture the hung child's stack."** The repo has no tool that can do that. It is not an isolated case either — [`docs/plans/ui-freeze-pillar2-blocking.md`](../shipped/ui-freeze-pillar2-blocking.md) tracks seven issues across two hang mechanisms (blocking `std::future` destructors, sync I/O on the render path), and `docs/self-improvement/categories/security.md:109` carries a still-open UI-thread-starvation finding.

Intended outcome: *after this lands, `debug-detective` can read the log and capture every thread's stack from a live, wedged process over MCP, with no human at the keyboard and no rebuild to turn observability on.*

## Approach

Three slices, cheapest-first, each landing independently.

**The grill pass replaced slice 3's design.** The original plan proposed `StackWalk64` behind a platform-injected seam. Two findings killed it. First, `Source/Core/CMakeLists.txt:30` globs `CORE_SOURCES`, so `BuiltinCommands_Debug.cpp` compiles into **four** targets — `SmatchetStandalone`, `SmatchetCore_DX12` (`Source/Core/CMakeLists.txt:435`), Android `SmatchetMobile` (`Source/Mobile/CMakeLists.txt:39`), and the `SmatchetCore_PosixCheck` portability gate (`:453`) — so `dbghelp` would be needed on three link surfaces including `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/SmatchetImGuiPlugin.Build.cs:101-113`, where a miss surfaces as an **Unreal-editor link failure the CMake CI lane never sees**. Second, the capability already exists: `Source/Standalone/SmatchetCrashHandler.cpp:105` calls `MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), …, MiniDumpNormal, …)` — the app already dumps *itself*, in-process, and a minidump carries every thread's stack.

So slice 3 becomes **`debug.dump_self`**: Core declares a host callback and a command that invokes it; `Source/Standalone` installs the provider next to the existing crash-dump code, where `dbghelp` is already linked by pragma. This follows the split `Source/Core/include/Diagnostics/CrashSink.h:8-9` already codifies ("the OS crash handlers that call it live in `Source/Standalone/SmatchetCrashHandler.cpp`") and the `HostCallbacks` channel (`Source/Core/include/Types/HostCallbacks.h:18-26`) that Standalone, Android and the Unreal host each already install into. Core gains **no** `#ifdef _WIN32`, no target gains a link dependency, and hosts without a provider return today's not-available note. Rationale recorded in [`docs/adr/0024-self-minidump-over-in-process-stack-walk.md`](../../adr/0024-self-minidump-over-in-process-stack-walk.md).

**The hang-diagnosis premise is verified, not assumed.** A grill trace of the MCP `tools/call` path found no lock held by the UI thread during rendering: `CommandRegistry::Dispatch` snapshots the command and **releases `mutex_` before invoking the handler** (`Source/Core/src/Commands/CommandRegistry.cpp:353-356`). And `--spawn` only dispatches after the child answers `app.version` over MCP (`Source/Standalone/CliDispatch.cpp:182`), so a child that then wedges — as the texture-guard child does, failing with a bare `rc=124` rather than the "did not become reachable" envelope — provably still has a live MCP server on a known port. One residual risk remains and is handled in § Risks.

Slice 3 must preserve the handler's **no-marshalling** property. `RunOnUiThreadAsCommandResult` (`Source/Core/include/Commands/MainThreadDispatch.h:101`) blocks with **no timeout** — its own header says so at `:23-27` — and 5 of the 11 `debug.*` commands use it, making them useless during exactly the hang this slice exists to diagnose. `debug.thread_dump` is correctly inline today; the replacement must stay that way.

## Files to modify

**Slice 1 — an agent-debug build preset**

1. `CMakePresets.json` — add a configure preset (e.g. `ninja-iter-agentdebug-msvc`) inheriting `ninja-iter-msvc` (block at `:63-75`) with `SMATCHET_AGENT_DEBUG: "ON"` and its **own `binaryDir`**, plus the matching build preset alongside `:445-452`. Additive: existing presets stay byte-for-byte unchanged, matching the precedent at `CMakeLists.txt:388`. The name must be lowercase-alnum-hyphen — `agents/scripts/core/test-agent-build-facts.sh:37-66` scrapes `ninja-[a-z0-9-]+` out of `agents/**/*.md` and requires each to resolve in `CMakePresets.json`.
2. `BUILD.md` + `docs/agent-rules/build.md` — preset list entries.
3. `agents/_shared/skills/debug-instrument/SKILL.md` — name the preset in § Build so the loop stops paying for a rebuild it can skip.

No wiring change is needed: `Source/Standalone/CMakeLists.txt:55` compiles `${CORE_SOURCES}` directly into the executable, so the `PRIVATE SMATCHET_AGENT_DEBUG=1` define at `:392-395` already reaches `Logger.cpp`'s bridge and every Core TU.

**Slice 2 — `debug.log_tail`**

4. `Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` — new read-only command over `Logger::GetEntriesSnapshot()`. Params via `PInt`/`PString` (`BuiltinCommands_Helpers.cpp:127,136`): `lines` (clamped, adopting the `kMaxLogTailLines = 300` precedent at `Source/Core/src/Diagnostics/EngineContextFormat.cpp:17`), optional `minLevel` (enum), optional `contains`. Returns `{lines[], total, truncated}`. Register the static in the call list at `:405-416`. Set `Description` with returns-shape + examples — `Source/Core/include/Commands/Command.h:173` makes that a stated contract, not a nicety.
5. `tests/monkey/monkey_command_registry.cpp:88-94` — add the name to the hand-curated read-only allow-list (`debug.thread_dump` is already there at `:93`); the list is explicitly never auto-expanded.
6. `docs/guides/cli.md:388-395` — catalogue entry. The § debug section documents 4 of 10 registered `debug.*` commands; do not widen that drift. [`docs/plans/cli-guide-registry-parity-gate.md`](cli-guide-registry-parity-gate.md) is the plan that will gate it (unimplemented — none of its four planned files are on disk), and its planned scanner keys on the literal `MakeCommand("<name>"` spelling, so use that form.

**Slice 3 — `debug.dump_self`**

7. `Source/Core/include/Types/HostCallbacks.h:18-26` — add a `WriteSelfMinidump` callback (path in, success/reason out). Publish-once, host-installed, null-checked by consumers, exactly as the seven existing callbacks are.
8. `Source/Core/include/AppController.h` (near the setters at `:366-381`) + `Source/Core/src/AppController_HostIntegration.cpp` (near `:76-94`) — the `SetWriteSelfMinidumpHandler` setter, mirroring `SetRequestAppQuitHandler`.
9. `Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` — the `debug.dump_self` command: resolve a confined output path, invoke the callback, return `{path, wrote}` or a not-available reason. **Comment the no-marshalling invariant** at the handler. Registration takes `IAppDebug&` like its siblings at `:79`/`:125`/`:167`, so the call-list line at `:405-416` changes shape.
10. `Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp:103-117` — update the `debug.thread_dump` stub's note to point at `debug.dump_self` rather than leaving a dead end. Its `hardwareConcurrency` payload stays.
11. `Source/Standalone/SmatchetCrashHandler.cpp` (near `:90-113`) — extract the dump-writing body so the on-demand path and the SEH path share one implementation. **`MiniDumpNormal` is not negotiable**: the comment at `:98-104` records it as a deliberate secret-hygiene decision (richer scopes sweep heap-resident API tokens into a `.dmp` the bug reporter auto-attaches). The on-demand dump inherits that reasoning verbatim.
12. `Source/Standalone/StandaloneAppBootstrap.cpp:410` — install the provider beside `SetRequestAppQuitHandler`, before `Initialize(...)` at `:447`.
13. `docs/guides/cli.md` — catalogue entries for both commands.
14. `agents/_shared/skills/debug-instrument/SKILL.md` — add `debug.log_tail` + `debug.dump_self` to the § Run command table, and a § Evidence-source catalogue line for hang capture. Include the **marshal-vs-inline table**: during a hang, only the inline `debug.*` commands respond; `dock.dump`, `dock.reset`, `window.resize`, `window.screenshot` and `grid.edit-burst` will block forever.
15. `docs/agent-rules/debug-techniques.md` — a hang-capture entry beside the existing crash-capture one, handing off to the existing `agents/scripts/core/dump-triage.sh` / `cdb -z` tiers.
16. `agents/core/debug-detective.md` — pointer lines only in § 3 (evidence catalogue) and § 8 (crash workflow); the file is at 217 of a hard 250 cap.

## Existing utilities reused

- `MiniDumpWriteDump` + the `MiniDumpNormal` decision — `Source/Standalone/SmatchetCrashHandler.cpp:105` and its rationale at `:98-104`. Slice 3 reuses the call and inherits the secret-hygiene constraint rather than re-deciding it.
- `HostCallbacks` + the publish-once host-install discipline — `Source/Core/include/Types/HostCallbacks.h:18-26`, installed at `Source/Standalone/StandaloneAppBootstrap.cpp:410` (GLFW quit), `Source/Mobile/Android/android_main.cpp:405`, `Source/Core/src/Ui/SmatchetImGuiHost.cpp:629-631` (Unreal). The documented channel for host capabilities — unlike `AiClientFactory::SetTestOverride` and `AnnotateAnalysisConfig::P4RunOverride`, which their own headers mark test-only.
- `agents/scripts/core/dump-triage.sh` + `minidump-triage.py` — the dependency-free reader for whatever slice 3 writes; no new triage tooling is needed. `cdb -z <dump> -c "!analyze -v; q"` is the symbolized tier, already documented in `docs/agent-rules/debug-techniques.md:25-31`.
- `CrashSinkPendingDumpPath` / dump rotation (`Source/Core/include/Diagnostics/CrashSink.h:52`, `keep-5` rotation in `CrashSink.cpp`) — reuse the crash directory conventions and rotation rather than inventing a second dump location.
- `Logger::GetEntriesSnapshot()` — `Source/Core/src/Logger.cpp:189-192`. Slice 2's read-back seam; already has non-UI callers (`Diagnostics/BugReportService.cpp:467`, `Scenarios/MobileTextureGuardScenario.cpp:55`).
- `privacy::RedactLogLine` — applied at ingest in `Logger::Log` (`Logger.cpp:100`), so every ring entry is already redacted. Slice 2 adds no redaction call; it adds a **regression test** that the property still holds.
- `FillLogViewLinesFromEntries` — `Source/Core/src/Ui/SmatchetLog_detail.cpp:29`. Reused so the command and the Runtime Log panel cannot drift.
- `builtin_detail::MakeCommand` + `CommandRegistry::Register` — `BuiltinCommands_Helpers.cpp:118-125`. Registration alone auto-publishes to CLI, Palette, MCP (`Source/Plugins/Mcp/McpPlugin.cpp:526-529`, `:963-972`) and Lua.

## Extraction sizing

Two extractions.

1. **Dump-writing body → shared helper (slice 3).** EXTRACT: the `MiniDumpWriteDump` call plus file-handle setup from `WriteMiniDumpImpl` (`SmatchetCrashHandler.cpp:90-113`, ~24 lines) into a function both the SEH path and the on-demand provider call. STAYS: the SEH-specific exception-record synthesis and the `g_exceptionDumpWritten` arbitration. Not a size fix — a single-implementation fix, so the `MiniDumpNormal` choice cannot drift between the two callers.
2. **Agent-doc cap budget.** `agents/core/debug-detective.md` is at **217 lines against a hard 250** (blocking, `agents/scripts/core/agent_size_audit.py`); `agents/_shared/skills/debug-instrument/SKILL.md` is at **381 against a 400 soft-warn**. All slice-3 mechanics (command table, marshal-vs-inline table, triage handoff) go to the skill; only 2–4 pointer lines land in the agent. Projected: `debug-detective.md` ≈ 220 (clears the hard cap), `debug-instrument/SKILL.md` ≈ 397 (just under the soft-warn — if it crosses, split § Run's command table into its own sink rather than trimming content).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no steady-state impact — every path added is request-only and absent from the render loop. Slice 1 is additive-by-preset precisely so the iter/publish builds keep the zero-overhead contract asserted at `CMakeLists.txt:391-393`.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: slice 2 is a mutex-guarded in-memory copy bounded to ≤ 300 entries — no file I/O. Slice 3 writes a minidump, which **is** a multi-hundred-millisecond stall and is reachable from the Command Palette (an ImGui-thread frontend for the same registry). Accepted and mitigated by scope: it is a diagnostic-only command, `MiniDumpNormal` is the cheapest dump scope, and the stall is by definition acceptable in the situation it exists for. If the static scanner flags it, the fix is a `PILLAR2_WORKER_ONLY` annotation on the *palette* path only — **never** converting the MCP path into a marshalled call, which would destroy the hang-diagnosis property.
- **Pillar 3 (never crash)**: the redesign removes the original risk. There is no `SuspendThread`, no `StackWalk64`, and no allocation-while-suspended hazard; `MiniDumpWriteDump` handles thread suspension internally and is already trusted on the crash path. Residual: dumping a wedged process can itself fail — the provider returns a reason instead of asserting, and never throws across the callback boundary.
- **Pillar 4 (accessibility)**: no impact — no user-facing UI surface is added.

## Perf-review-system gates

Mandatory: the diff touches `Source/Core/`. Per [`docs/plans/pillar-1-2-perf-review-system.md`](../shipped/pillar-1-2-perf-review-system.md).

1. **PR-fast CI** — **fires.** `command-contract-sweep` **auto-enrols** new commands (`Source/Core/src/Commands/Scenarios/CommandContractSweepScenario.cpp`, driven by `scripts/dev/test-command-contract.sh`), so both new registrations must satisfy the error-envelope contract. Pair with `idle` as the baseline floor. Subset declared in `scripts/dev/perf-pr-fast-set.json`.
2. **Pillar 2 static scanner** — **fires** on slice 3 (minidump write reachable from the palette frontend); resolution per the Pillar-2 callout. Slice 2 should not trip it.
3. **Dispatcher drain** — **N/A**; nothing here touches `MainThreadDispatcher::Drain()`. Slice 3 deliberately avoids the dispatcher, which is the point.
4. **Visible-cue bucket-E harness** — **fires** in principle for slice 3 (a new > 100 ms path). Resolved by the diagnostic-only scope above; if a reviewer disagrees, the palette entry gets a visible-cue treatment rather than the command being marshalled.
5. **Marker inventory** — **N/A**; no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run [`docs/guides/perf-workflow.md`](../../guides/perf-workflow.md) § Gate-check vs baseline against `command-contract-sweep` + `idle` before opening each code slice's PR.

## Risks / non-goals

- **Risk: the registry lock defeats the hang premise in one narrow case.** `CommandRegistry::mutex_` is shared with UI-thread reads (`All()`/`Recents()` from `CommandPaletteUi.cpp:79,90`; `HasExact()`/`All()` from the toolbar and keybindings tabs). If the UI thread wedges *inside* one of those, the MCP worker blocks in `Contains()` at `CommandRegistry.cpp:54` before reaching any handler. Those regions are pure in-memory map copies with no I/O and no nested locks, and every recorded hang in this repo is a blocking future, a subprocess, or sync file I/O — all outside the lock. Mitigation: a `try_lock`-with-timeout probe on the dump path so the guarantee is unconditional rather than probabilistic. Cheap; do it in slice 3.
- **Risk: a dump of a wedged process leaks secrets.** Mitigated by inheriting `MiniDumpNormal` (`SmatchetCrashHandler.cpp:98-104`) — the whole point of that scope choice. A bucket-A test asserts the on-demand path passes the same flag; a future contributor "enriching" the dump must break that test to do it.
- **Risk: slice 2 exposes log content over MCP.** Lower than it appears (redaction is at ingest), but now load-bearing for a remote reader — so a bucket-A test seeds a known token through `Logger::Log` and asserts absence from the command's output. That test is the slice's acceptance criterion.
- **Risk: a new preset re-opens the exe-staleness footgun** `docs/agent-rules/debug-techniques.md` exists to prevent. Mitigated by the distinct `binaryDir` plus the absolute-path-and-mtime report the debug loop already performs after every build.
- **Risk: `debug-detective.md` crosses its 250-line hard cap.** Mitigated by the § Extraction sizing budget; `agent_size_audit.py` runs in slice 3's verification.
- **Non-goal: in-process `StackWalk64` / inline thread stacks.** Rejected — see ADR-0024. A minidump plus the existing triage tooling delivers the same evidence at a fraction of the linkage and Pillar-3 cost.
- **Non-goal: out-of-process debugger attach.** Not needed for the evidenced cases: the wedged child keeps a live MCP server, so it can be asked to dump itself. Attach remains the fallback for a process wedged so hard that the MCP thread is stuck too — reachable today via the existing procdump recipes in `docs/guides/crash-capture.md`.
- **Non-goal: flipping `SMATCHET_AGENT_DEBUG` ON by default in `ninja-iter-msvc`.** That preset feeds bucket-C, launch-smoke, CodeQL and the light build; three in-tree contracts state OFF is deliberate; and the `autonomous-debugging-no-creds` V7.6 perf probe was deferred and never run, so no ON-build overhead number exists. Running that probe is the precondition for revisiting, and belongs to that plan.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: slice 2's clamp/filter logic and the **redaction regression test** (seeded token absent from output); slice 3's path-confinement logic, the not-available path when no provider is installed, and the **`MiniDumpNormal` flag assertion**. The Win32 dump write itself is covered below.
- **Bucket E (ImGui Test Engine)**: `N/A` — no UI surface is added. Palette *dispatch* rides the command-contract path, not a UI interaction test.
- **Bash-driver scenario / screenshot / sanitizer**: a driver that `--spawn`s an instance, emits a known `debug.log` breadcrumb and asserts it round-trips through `debug.log_tail`; a `debug.dump_self` call asserting a non-empty `.dmp` is written and that `agents/scripts/core/dump-triage.sh` parses it and attributes the faulting module. Slice 3 additionally exercised under `ninja-msvc-asan`.
- **End-to-end acceptance (slice 3)**: reproduce the open P2 in `docs/self-improvement/categories/infra/2026-07-05-texture-guard-llvmpipe-spawn-hang.md` under Mesa/llvmpipe, call `debug.dump_self` on the wedged child, and triage the dump. Success is that entry's "concrete next action" item 1 being satisfied — a real deadlock's stacks, not a synthetic one. If the dump cannot be obtained, that is a finding about the premise and returns here before slice 3 is called done.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target), plus one configure+build of the new slice-1 preset. Because `CORE_SOURCES` is globbed into four targets, slice 3's Core-side diff must also keep `SmatchetCore_PosixCheck` and the Android NDK lane green — both compile the non-`_WIN32` side, which is why Core gains no `#ifdef` at all.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green — it enumerates the doc-validation steps (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint); defer to the script. A red doc-validation job blocks merge even though non-required. Slice 1 must additionally satisfy `test-agent-build-facts.sh`; slice 3 runs `agent_size_audit.py`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: **DONE 2026-08-16.** Outcomes: (a) the in-process `StackWalk64` design was rejected on linkage cost across four glob-fed targets and replaced by the self-minidump provider seam (ADR-0024); (b) the hang-diagnosis premise was verified against the dispatch path rather than assumed, and the one residual lock risk became a `try_lock` mitigation; (c) the scripted-`cdb` breakpoint tier was cut for lack of evidenced demand (see § Out of scope); (d) a doc-reconciliation slice was cut as unrelated work per [`ship-loops.md`](../../agent-rules/ship-loops.md) § "Unrelated work never shares a PR", and re-filed as self-improvement entries; (e) **Live-process evidence** was pinned as a term in [`docs/CONTEXT.md`](../../CONTEXT.md) § Autonomous debugging.
- **Manual residue**: none expected. If the slice-3 acceptance test cannot run on a CI runner, name it here as a deferred-automation action plus a `docs/self-improvement/categories/tooling/` entry — no silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here — notably any doc describing `debug.thread_dump` as the intended hang tool once slice 3 lands.

- **A scripted-`cdb` breakpoint tier (agent-driven breakpoints / value-at-a-moment capture).** Cut by the grill for lack of evidenced demand: the only evidenced diagnosis gap was hang-stack capture, which slice 3 now serves, and the other stuck investigation (the 2026-05-17 bucket-E spawn flake) is a *race*, which breakpoints perturb rather than reveal. **Revisit trigger**: a real investigation blocked on a value-at-a-moment question that neither `debug.log_tail` nor a dump can answer. Until then this stays unbuilt on purpose — it remains the capability neither Cursor nor Claude Code ships natively, so it is a deliberate deferral, not an oversight.
- **A true MCP↔DAP bridge** (stepping, watch expressions). Strictly larger than the tier above; same trigger, higher bar.
- **Stale CI-lane documentation.** Four docs describe the pre-all-gates-blocking world (bucket-C/E "fully advisory", `bucket-mesa-exe-boot`'s moved entry, the TSan plan header). Real, but unrelated to live-process evidence — filed as `docs/self-improvement/categories/infra/2026-08-16-stale-advisory-lane-docs.md` and `…-tsan-lane-advisory-label-drift.md`.
- **Finishing TSan coverage.** A nightly Linux TSan job already exists (`.github/workflows/tsan-linux-nightly.yml`); the remaining work is slices 2c/3 of [`docs/plans/tsan-imgui-linked-target.md`](tsan-imgui-linked-target.md).
- **Removing the bucket-C / bucket-E step masks.** Belongs to [`docs/plans/testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice B.
- **DX12 / Unreal parity for the new commands.** `debug.dump_self` returns not-available where no provider is installed; wiring the Unreal host is [`docs/plans/dx12-backbuffer-readback-screenshot.md`](dx12-backbuffer-readback-screenshot.md)-adjacent territory, not this plan's.
- **A CPU/allocation profiler or flamegraph integration.** Different subsystem (perf) and owner (`perf-detective`).

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that fills the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
