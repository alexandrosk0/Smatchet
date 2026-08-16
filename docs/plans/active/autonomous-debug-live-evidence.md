<!-- index-summary: close the live-process evidence gaps in the autonomous debug loop — log read-back, real thread dumps, a scripted debugger tier, and an agent-debug build preset -->
# Plan — Live-process evidence for the autonomous debug loop

> **Slug**: `autonomous-debug-live-evidence` (matches this file's basename without `.md`).
>
> **Status**: `active` — authored 2026-08-16, unstarted.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.
>
> **Origin**: a capability-gap assessment of this repo's agentic debugging harness against Cursor Debug Mode (2026-08-16). The assessment found the harness ahead on *reproduction* and *credential-free running* — what [`docs/adr/0009-autonomous-debugging-no-creds.md`](../../adr/0009-autonomous-debugging-no-creds.md) set out to deliver — and behind on nothing Cursor ships, but with four concrete holes in what the loop can **observe in a live process**. This plan closes those four.

## Context

ADR-0009 gave `debug-detective` a reproducer it can run without a human and without credentials. What it did not give it is a way to observe a process that is already running and misbehaving. Four gaps follow, and each forces the loop into a slower path — an extra instrument-build-run cycle, an out-of-band file read, or a dead end:

1. **The structured agent-debug channel is off in the preset the loop builds from.** `SMATCHET_AGENT_DEBUG` defaults OFF (`CMakeLists.txt:394`) and `ninja-iter-msvc` never mentions it, so `SMATCHET_AGENT_DEBUG_LOG` compiles to `((void)0)` (`tests/_debug/SmatchetAgentDebug.h:370-375`). Observing production-shape behaviour costs a preset switch plus a full rebuild before the first breadcrumb lands.
2. **Nothing reads the runtime log back.** `debug.log` writes into the Logger, but no command returns log content, so an agent must locate and read `%LOCALAPPDATA%\Smatchet\Smatchet-<pid>.log` off disk — fine locally, unavailable to a purely-MCP client (a `--spawn` child, a remote session).
3. **`debug.thread_dump` is a stub** (`Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp:103-117`) returning `hardwareConcurrency` and a note. A hang or deadlock — the one crash-class the minidump pipeline never sees, because nothing crashes — therefore has **no capture path at all**.
4. **There is no evidence tier between "read the logs" and "add more logs."** Every hypothesis needing a variable's value at a moment costs a full instrument → build → run → read cycle. Nothing in the repo spawns a debugger: `scripts/dev/av-capture.cdb` is a passive command file a human feeds to `cdb -cf`, and `grep -rln cdb` over `scripts/ agents/ docs/` returns 7 files, none of which drive a session.

Intended outcome: *after this lands, `debug-detective` can read the log, dump every thread's stack (including while the UI thread is hung), and take breakpoint evidence off a running scenario — through the unified command surface or a checked-in script, with no human at the keyboard.*

## Approach

Five slices, cheapest-first, each landing independently. Slice 0 is a bounded doc-truth fix the rest of this plan depends on; slices 1–3 extend the in-app surface; slice 4 adds the one new external tool.

Two design decisions are load-bearing.

**Slice 2 is far cheaper than it looks, because the read-back source already exists.** `Logger` keeps a revision-tracked in-memory ring of the last 1000 entries (`Source/Core/include/Logger.h:92-99`), exposed thread-safely by `GetEntriesSnapshot()` (`Source/Core/src/Logger.cpp:189-192`) and **redacted at ingest** by `privacy::RedactLogLine` (`Logger.cpp:100`). The Runtime Log panel is a *consumer* of that ring, not its owner. So the command is a registry entry over an existing accessor — no file I/O, no new redaction path, and no UI-thread marshalling.

**Slice 3's constraint is linkage, not portability.** The stub's stated reason ("no OS-specific code in `Source/Core`") is over-conservative — 54 files under `Source/Core/` already carry `_WIN32` guards. The real blocker is that `dbghelp` is linked **only** by `#pragma comment(lib, "dbghelp.lib")` inside a `Source/Standalone` TU (`SmatchetCrashHandler.cpp:10-15`); no `CMakeLists.txt` or `*.cmake` in the repo mentions it, and `StackWalk64`/`SymInitialize` appear nowhere. `Source/Core` must also compile under Unreal MSVC and Android/POSIX. So the implementation goes behind a **platform-injected provider seam** — `Source/Core` declares the interface and returns today's note when no provider is installed; `Source/Standalone` (which already compiles the dbghelp pragma) installs the Win32 walker. This mirrors the `P4RunOverride` / `AiClientFactory::SetTestOverride` seams ADR-0009 already uses, and confines the dbghelp link surface to the target that has it. The rejected alternative — `#ifdef _WIN32` in place plus `target_link_libraries(dbghelp)` on both `SmatchetStandalone` and `SmatchetCore_DX12` — changes the Unreal-embedded link surface for a diagnostic-only feature and collides with ADR-0002 plugin-shim link discipline.

Preserve one property deliberately in slice 3: the handler runs **inline on the calling thread**, unlike `debug.dock.dump`, which marshals. That is exactly what lets it diagnose a UI-thread hang — the MCP server thread is still alive to walk the stuck thread's stack. It must be commented and asserted, not left to accident.

**Slice 4 deliberately does not build an MCP↔DAP server.** A scripted, non-interactive `cdb` session — breakpoint spec in, captured output out — composes with the deterministic scenario library the loop already depends on, extends the `dump-triage.sh` family the repo owns, and adds no long-lived process. `av-capture.cdb` already emits machine-parseable `===SMATCHET_*===` sentinels, which are the ready-made parse anchors. Interactive stepping is the trade; what it buys is answering "what was this value at this call" in one run instead of one instrument-build-run cycle.

## Files to modify

**Slice 0 — doc-truth reconciliation (bounded to four claims this plan relies on)**

1. `docs/plans/active/testing-surface-roadmap.md:275-282` — Slice B's stated blocker is stale in all three clauses: the Mesa exe boots, job-level `continue-on-error` is gone, and the poller allow-list is `"."` so both lanes already block. Rewrite to the real residual work (the ~3/74 render-dependent bucket-E tests behind the step mask at `build-and-test.yml:1279`, and CI-native goldens for the bucket-C mask at `:1041`).
2. `docs/guides/testing-surface.md`, `docs/self-improvement/categories/infra.md` — same stale "fully advisory" claim; correct to step-level-masks-only.
3. Any doc citing `infra.md` `bucket-mesa-exe-boot` — the entry moved to `docs/self-improvement/categories/applied.md:1565` and its premise was falsified 2026-06-18. Re-point the references.
4. `docs/plans/active/tsan-imgui-linked-target.md:5` — header says slices 2–3 are "designed-not-started"; its own log at `:96-97` records 2a and 2b as done. Reconcile the header.

**Slice 1 — an agent-debug build preset**

5. `CMakePresets.json` — add a configure preset (e.g. `ninja-iter-agentdebug-msvc`) inheriting `ninja-iter-msvc` with `SMATCHET_AGENT_DEBUG: "ON"` and its **own `binaryDir`**, plus the matching build preset alongside `:445-452`. Additive: every existing preset stays byte-for-byte unchanged, matching the precedent recorded at `CMakeLists.txt:388`. Name must be lowercase-alnum-hyphen to be citable in agent prompts (`agents/scripts/core/test-agent-build-facts.sh:37-66` scrapes `ninja-[a-z0-9-]+` from `agents/**/*.md` and requires each to resolve).
6. `BUILD.md` + `docs/agent-rules/build.md` — preset list entries.
7. `agents/_shared/skills/debug-instrument/SKILL.md` — name the preset in § Build so the loop stops reaching for a rebuild it can skip.

**Slice 2 — `debug.log_tail`**

8. `Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp` — new read-only command over `Logger::GetEntriesSnapshot()`. Params via `PInt`/`PString` (`BuiltinCommands_Helpers.cpp:127,136`): `lines` (clamped, adopting the `kMaxLogTailLines = 300` precedent at `Source/Core/src/Diagnostics/EngineContextFormat.cpp:17`), optional `minLevel` (enum), optional `contains`. Returns `{lines[], total, truncated}`. Register the static in the call list at `:405-416`. Named `log_tail` for vocabulary consistency with the existing `LogTail` / `logTail` surfaces.
9. `tests/monkey/monkey_command_registry.cpp:88-94` — add the new name to the hand-curated read-only allow-list (`debug.thread_dump` is already there at `:93`); the list is explicitly never auto-expanded.
10. `docs/guides/cli.md:388-395` — catalogue entry. The § debug section currently documents 4 of 10 registered `debug.*` commands; do not widen that drift. [`docs/plans/cli-guide-registry-parity-gate.md`](cli-guide-registry-parity-gate.md) is the plan that will gate this (not yet implemented — none of its four planned files are on disk), and its planned scanner keys on the literal `MakeCommand("<name>"` spelling, so use that form.

**Slice 3 — real `debug.thread_dump`**

11. New `Source/Core/include/Diagnostics/ThreadIntrospection.h` (+ TU) — the provider seam: a `ThreadSnapshot` POD (`tid`, `name`, `frames[]`), a `SetProvider(fn)` installer, and a default provider returning today's note. Grep `rg -l 'ThreadIntrospection' Source/Core/` first per the template's naming rule.
12. `Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp:103-117` — replace the stub body with a call through the seam; keep `hardwareConcurrency` in the payload. Comment the **no-marshalling invariant** at the handler.
13. `Source/Standalone/` (near `SmatchetCrashHandler.cpp`, which already carries the dbghelp pragma) — the Win32 provider: enumerate this process's threads via `CreateToolhelp32Snapshot`/`Thread32First`, resolve names via `GetThreadDescription`, walk each stack with `StackWalk64`. Installed at startup from `Source/Standalone/main.cpp`.
14. `Source/Standalone/CMakeLists.txt` — add the explicit `dbghelp` link rather than relying on the pragma drectve now that a second TU depends on it (`:228` currently links `opengl32 ws2_32 crypt32 wintrust`).
15. Thread spawn sites — add `SetThreadDescription` so dumps carry names, not bare TIDs. The known set is ~14 ad-hoc `std::thread` members (`Logger.h:112`, `AppControllerImpl.h:198`, `AiAssistantController.h:334`, `Sync/TicketSyncService.h:93`, `ConfigSaveWorker.cpp:26`, `SmatchetChatPersistWorker.cpp:29`, `Persistence/BackendAuditTrail.cpp:192`, `Ui/AnnotateAnalysisUi_Internal.h:70`, `BuiltinCommands_Ai.cpp:323`, three AI scenario workers, plus `Source/Plugins/Mcp/McpPlugin.cpp:99` and two Whisper threads) — re-enumerate with `rg -n 'std::thread' Source/Core/ Source/Plugins/` before writing the diff. `AppController::backgroundWorkers_` (`AppController.h:1553-1560`) is the only registry and carries no identity metadata; naming at the spawn site is cheaper than growing it.
16. `docs/guides/cli.md` — update the entry (`:395` documents the stub's `{hardwareConcurrency}` shape).

**Slice 4 — scripted-debugger evidence tier**

17. New `agents/scripts/core/live-debug.sh` — drive a non-interactive `cdb` session against a `--spawn`ed scenario: breakpoint spec in, captured output out. Follow house conventions confirmed in `dump-triage.sh`: `#!/usr/bin/env bash`, `set -uo pipefail`, a header block with an explicit `# Exit:` line, exit codes `0` clean / `1` the real failure / `2` infra (missing `cdb`), a `--selftest` flag with the grep-able `# selftest: asserts-failure` marker required by `agents/scripts/core/test-gate-selftests.sh`, and `. agents/scripts/core/lib/resolve-py.sh` if a python leg is needed. Parse on the `===SMATCHET_*===` sentinel convention from `scripts/dev/av-capture.cdb`.
18. New `agents/_shared/skills/live-debug/SKILL.md` — the mechanics (cdb invocation, breakpoint syntax, scenario pairing, output parsing, cleanup). A **new** skill, not more of `debug-instrument` — see § Extraction sizing.
19. `tests/bats/live_debug.bats` — argument handling, absent-`cdb` degradation, selftest. Must be referenced by a `test-*.sh` wrapper or `agents/scripts/core/test-orphan-bats.sh` fails it; `scripts/dev/test-all.sh` discovers by the `test-*.sh` glob and never runs a `.bats` directly.
20. `agents/core/debug-detective.md` — add the tier to § 3's evidence catalogue and § 8's crash workflow as **pointer lines only** (the file is at 217 of a hard 250 cap).
21. `agents/_shared/skills/debug-instrument/SKILL.md` — add `debug.log_tail` and the real `thread_dump` to the § Run command table; one line in § Evidence-source catalogue pointing at the new skill.
22. `docs/agent-rules/debug-techniques.md` — a hang-capture entry beside the existing crash-capture one; hangs are the class slice 3 newly makes diagnosable.

## Existing utilities reused

- `Logger::GetEntriesSnapshot()` — `Source/Core/src/Logger.cpp:189-192`. The ring-buffer read-back seam slice 2 is built on; already has non-UI callers (`Diagnostics/BugReportService.cpp:467`, `Scenarios/MobileTextureGuardScenario.cpp:55`), so calling it from a command handler is precedented, not novel.
- `privacy::RedactLogLine` — applied at ingest in `Logger::Log` (`Logger.cpp:100`), so every ring entry is already redacted. Slice 2 adds no redaction call; it adds a **regression test** that the property still holds.
- `FillLogViewLinesFromEntries` — `Source/Core/src/Ui/SmatchetLog_detail.cpp:29`. Reused for formatting so the command and the Runtime Log panel cannot drift.
- `builtin_detail::MakeCommand` + `CommandRegistry::Register` — `BuiltinCommands_Helpers.cpp:118-125`, `CommandRegistry.h:34`. Registration alone auto-publishes to CLI, Palette, MCP (`Source/Plugins/Mcp/McpPlugin.cpp:526-529`, `:963-972`) and Lua; no per-frontend wiring.
- `ParamSpec` validation (`Command.h:36-49`) — declarative bounds/enums enforced centrally by `ValidateAndResolveArgs`, so neither new command hand-rolls arg checking.
- `agents/scripts/core/dump-triage.sh` + `minidump-triage.py` — the conventions slice 4 follows, and the fallback tier it hands off to when `cdb` is absent.
- The `--spawn` ephemeral-instance path (`docs/guides/perf-workflow.md`) — slice 4 debugs a spawned scenario instead of inventing a launch harness.

## Extraction sizing

One extraction, forced by a cap this repo enforces (`agents/scripts/core/agent_size_audit.py`).

**Slice 4 mechanics → a NEW `live-debug` skill, not `debug-instrument`.** `agents/core/debug-detective.md` is at **217 lines against a hard 250** (blocking), and `agents/_shared/skills/debug-instrument/SKILL.md` is at **381 against a 400 soft-warn**. Folding cdb mechanics into either crosses a cap. Classification: EXTRACT = the whole cdb procedure (invocation, breakpoint syntax, output parsing, cleanup) → new `agents/_shared/skills/live-debug/SKILL.md`; STAYS = the judgment of when a breakpoint beats another instrumentation round (2–4 pointer lines in `debug-detective.md` § 3 / § 8, one line in `debug-instrument` § Evidence-source catalogue). Projected: `debug-detective.md` ≈ 221 lines (clears the 250 hard cap; still over the 150 soft-warn it already exceeds), `debug-instrument/SKILL.md` ≈ 386 (under the 400 soft-warn), new skill ≈ 120 lines.

Slices 1–3 extract nothing — slice 2 is a registry entry over an existing accessor, slice 3 adds a new seam.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no steady-state impact — every path added is request-only and absent from the render loop. Slice 1 is additive-by-preset precisely so the shipping/iter builds keep the zero-overhead contract asserted at `CMakeLists.txt:391-393` and `SmatchetAgentDebug.h:20-21`.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: slice 2 is a mutex-guarded in-memory copy bounded to ≤ 300 entries — no file I/O, no meaningful stall even when invoked from the Command Palette (an ImGui-thread frontend for the same registry). Slice 3 is the real case: `SuspendThread` + `StackWalk64` across ~14 threads is not free, and the palette can reach it. Mitigated by bounding frame depth and thread count, and by documenting the command as diagnostic-only; if the static scanner flags it, the walk moves behind the dispatcher with a `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` annotation — **except** that the no-marshalling invariant is what makes hang diagnosis work, so the annotation route must not silently convert it into a marshalled call.
- **Pillar 3 (never crash)**: slice 3 carries the risk. Suspending a thread that holds the heap or loader lock and then allocating deadlocks the process — mitigated by allocating nothing while any thread is suspended (capture raw frames → resume all → symbolize afterwards), bounding the suspend window, never suspending the calling thread, and failing the command (empty result plus reason) rather than asserting on any Win32 error. Slice 4 runs out-of-process and cannot destabilise the app beyond what a debugger attach already implies.
- **Pillar 4 (accessibility)**: no impact — no user-facing UI surface is added.

## Perf-review-system gates

Mandatory: the diff touches `Source/Core/`. Per [`docs/plans/pillar-1-2-perf-review-system.md`](../shipped/pillar-1-2-perf-review-system.md).

1. **PR-fast CI** — **fires.** `command-contract-sweep` is the registry-wide envelope sweep and **auto-enrols** new commands (`Source/Core/src/Commands/Scenarios/CommandContractSweepScenario.cpp`, driven by `scripts/dev/test-command-contract.sh`), so both new registrations must satisfy the error-envelope contract. Pair with `idle` as the baseline floor. Subset declared in `scripts/dev/perf-pr-fast-set.json`.
2. **Pillar 2 static scanner** — **fires** on slice 3 (thread suspension reachable from the palette frontend); resolution per the Pillar-2 callout above. Slice 2 should not trip it (no sync I/O added — the read is in-memory).
3. **Dispatcher drain** — **N/A**; nothing here touches `MainThreadDispatcher::Drain()`. Slice 3 deliberately avoids the dispatcher, which is the point: a marshalled dump cannot diagnose a hung UI thread.
4. **Visible-cue bucket-E harness** — **N/A**; no new sync-stall path > 100 ms on a user-visible interaction. The bounds above are what keep this N/A true; if slice 3's dump exceeds budget in practice, this gate re-opens.
5. **Marker inventory** — **N/A**; no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run [`docs/guides/perf-workflow.md`](../../guides/perf-workflow.md) § Gate-check vs baseline against `command-contract-sweep` + `idle` before opening each code slice's PR.

## Risks / non-goals

- **Risk: slice 3 deadlocks the app.** Suspend-then-allocate is the classic footgun; mitigation in the Pillar-3 callout, plus exercising the command under `ninja-msvc-asan` before merge.
- **Risk: slice 3's dbghelp linkage leaks into the Unreal/DX12 target.** Mitigated by the provider seam — `Source/Core` declares an interface and ships a no-op default; only `Source/Standalone` links dbghelp. If the seam proves awkward, the fallback is `#ifdef _WIN32` in place plus explicit dual-target linking, accepted only with an ADR-0002 link-discipline check.
- **Risk: slice 2 exposes log content over MCP.** Lower than it first appears (redaction happens at ingest), but the property is now load-bearing for a remote reader — so a bucket-A test seeds a known token through `Logger::Log` and asserts it is absent from the command's output. That test is the slice's acceptance criterion, not an extra.
- **Risk: a new preset re-opens the exe-staleness footgun** that `docs/agent-rules/debug-techniques.md` exists to prevent (more build outputs, more chances to test the wrong binary). Mitigated by the distinct `binaryDir` plus the mandatory absolute-path-and-mtime report the debug loop already performs after every build.
- **Risk: `cdb` is absent on the dev box.** Slice 4 degrades to the existing `dump-triage.sh` tier with an explicit reason — never a silent skip.
- **Risk: `debug-detective.md` crosses its 250-line hard cap.** Mitigated by the § Extraction sizing budget; `agent_size_audit.py` runs as part of slice 4's verification.
- **Non-goal: flipping `SMATCHET_AGENT_DEBUG` ON by default in `ninja-iter-msvc`.** That preset feeds bucket-C, launch-smoke, CodeQL and the light build, and the plan-doc contract in three places says OFF is deliberate. It also cannot be justified today: the `autonomous-debugging-no-creds` V7.6 perf probe was deferred and never run, so no ON-build overhead number exists. Running that probe is the precondition for ever revisiting this, and belongs to that plan.
- **Non-goal: an MCP↔DAP bridge with interactive stepping.** See § Out of scope.
- **Non-goal: record/replay or time-travel debugging.** The scenario library remains the determinism mechanism.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**: slice 2's clamp/filter logic and the **redaction regression test** (seeded token absent from output); slice 3's frame-formatting and the seam's no-provider default (returns the documented note rather than an error). The Win32 walk itself is not unit-testable and is covered below.
- **Bucket E (ImGui Test Engine)**: `N/A` — no UI surface is added. Palette *dispatch* of the new commands rides the command-contract path, not a UI interaction test.
- **Bash-driver scenario / screenshot / sanitizer**: a driver that `--spawn`s an instance, emits a known `debug.log` breadcrumb and asserts it round-trips through `debug.log_tail`; a `debug.thread_dump` call asserting the UI thread appears **by name** with a non-empty stack; slice 3 additionally exercised under `ninja-msvc-asan`. Slice 4's script is covered by `tests/bats/live_debug.bats` (including absent-`cdb` degradation) behind a `test-*.sh` wrapper.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — slices 2 and 3 touch `Source/Core/`, so the DX12 core must build too), plus one configure+build of the new slice-1 preset to prove it resolves.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green — it enumerates the doc-validation steps (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint); defer to the script. A red doc-validation job blocks merge even though non-required. Slice 1 additionally must satisfy `test-agent-build-facts.sh` (any preset named in an agent prompt must resolve in `CMakePresets.json`); slice 4 additionally runs `agent_size_audit.py` and `test-gate-selftests.sh` / `test-orphan-bats.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test before slice 1 starts. Sharpen specifically: (a) whether the provider seam or in-place `#ifdef` + dual-target linking is the better trade for slice 3, (b) whether in-process `StackWalk64` is right at all versus an out-of-process helper that cannot deadlock the app, and (c) whether the scripted-cdb tier earns its keep against simply adding one more instrumentation round. Record the outcome here.
- **Manual residue**: none expected. If slice 4's cdb path cannot be exercised on a CI runner, name it here as a deferred-automation action plus a `docs/self-improvement/categories/tooling/` entry — no silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here — notably any doc describing `debug.thread_dump` as a stub once slice 3 lands, and `docs/CONTEXT.md` § Autonomous debugging, whose terminology this plan extends — and revise them. Slice 0 is the pre-emptive half of this sweep.

- **A true MCP↔DAP bridge** (agent-driven breakpoints, stepping, variable watch as first-class tools). Neither this harness nor Cursor nor Claude Code ships it natively; slice 4's scripted tier is the cheap 80%. Follow-up plan only if slice 4's usage shows the scripting boundary is the binding constraint.
- **Finishing TSan coverage.** A nightly Linux TSan job already exists (`.github/workflows/tsan-linux-nightly.yml`, check `TSan Linux subset (Clang)`, `ubuntu-latest`, with a paths-scoped PR trigger); the remaining work is slices 2c/3 of [`docs/plans/tsan-imgui-linked-target.md`](tsan-imgui-linked-target.md). No action here.
- **Resolving the TSan lane's advisory-label drift.** Four docs call that lane advisory, but its check name carries no `advisory` token, so under `MERGE_GATES_BLOCK_ALLOWLIST_RE="."` the poller blocks on it. Real, but a merge-gate question, not a debugging one — file to `docs/self-improvement/categories/infra/` rather than widening this plan.
- **Removing the bucket-C / bucket-E step masks.** Slice 0 corrects the stale claim that these lanes are advisory, but the residual work (fixing or skipping the ~3/74 render-dependent bucket-E tests; CI-native goldens for bucket-C) belongs to [`docs/plans/testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice B.
- **DX12 / Unreal parity for the new commands.** `debug.window.screenshot` is already standalone-only and gains a sibling gap here; the DX12 capture path is [`docs/plans/dx12-backbuffer-readback-screenshot.md`](dx12-backbuffer-readback-screenshot.md)'s territory.
- **A CPU/allocation profiler or flamegraph integration.** A real gap from the same assessment, but a different subsystem (perf) and owner (`perf-detective`).

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
