# On-demand self-minidump, not in-process stack walking, for live-process thread evidence

# Status

**Accepted (2026-08-16).** Emerged from the `grill-with-docs` pass on [`docs/plans/autonomous-debug-live-evidence.md`](../plans/active/autonomous-debug-live-evidence.md), which originally specified the rejected option.

# Context

`debug.thread_dump` has always been a stub returning `hardwareConcurrency` and a note (`Source/Core/src/Commands/Builtin/BuiltinCommands_Debug.cpp:103-117`), on the stated reasoning that OS-specific code does not belong in `Source/Core`. The consequence is that a process which wedges *without crashing* produces no evidence at all — the minidump pipeline only fires on a crash. That is not hypothetical: `docs/self-improvement/categories/infra/2026-07-05-texture-guard-llvmpipe-spawn-hang.md` is an open P2 deterministic deadlock whose concrete next action is "capture the hung child's stack", and [`ui-freeze-pillar2-blocking.md`](../plans/shipped/ui-freeze-pillar2-blocking.md) tracks seven more hang-class issues.

The obvious implementation — `SuspendThread` + `StackWalk64` in the command handler, returning inline JSON frames — turns out to be the expensive one, for a reason that is invisible from the command TU itself.

# Decision

Thread evidence from a live process is captured by **writing a minidump of the calling process on demand** (`debug.dump_self`) and reading it with the triage tooling that already exists, **not** by walking stacks in-process.

The Win32 work stays in `Source/Standalone`, beside the existing crash handler, and is published into `Source/Core` as a `HostCallbacks` entry (`Source/Core/include/Types/HostCallbacks.h`) installed at startup like `SetRequestAppQuitHandler`. `Source/Core` gains no `#ifdef _WIN32`; a host that installs no provider gets today's not-available note.

Three facts drove this:

- **`Source/Core/CMakeLists.txt:30` globs `CORE_SOURCES`**, so `BuiltinCommands_Debug.cpp` compiles into four targets: `SmatchetStandalone`, `SmatchetCore_DX12`, Android `SmatchetMobile`, and the `SmatchetCore_PosixCheck` portability gate. A `StackWalk64` call there needs `dbghelp` on three link surfaces — including `SmatchetImGuiPlugin.Build.cs`, where a missing `dbghelp.lib` surfaces as an **Unreal-editor link failure that the CMake CI lane never sees**. The cost is real and partly invisible to CI.
- **The capability already exists and is already trusted.** `Source/Standalone/SmatchetCrashHandler.cpp:105` calls `MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), …)` — the app already dumps *itself*, in-process, from a signal-handler context. A minidump carries every thread's stack, so the richer capability is the cheaper one here.
- **The read path is already built.** `agents/scripts/core/dump-triage.sh` (dependency-free) and `cdb -z <dump> -c "!analyze -v; q"` already triage `.dmp` files, so inline JSON frames would have been a second, weaker representation of evidence the toolchain can already consume.

`MiniDumpNormal` is inherited, not re-decided: the comment at `SmatchetCrashHandler.cpp:98-104` records it as a deliberate secret-hygiene choice, because richer dump scopes sweep heap-resident API tokens into a file the bug reporter auto-attaches. The on-demand path carries the same constraint and a test that asserts the flag.

# Consequences

**Positive**: no new link dependency on any target; no `SuspendThread`/allocate-while-suspended deadlock risk in an application with a documented history of hangs; one implementation of dump-writing shared with the crash path, so the `MiniDumpNormal` choice cannot drift; evidence lands in a format the existing triage tooling reads.

**Negative — accepted**: the command returns a file path, not inline frames, so reading it is a two-step (dump, then triage) rather than one call. Writing a dump is a multi-hundred-millisecond stall, acceptable only because this is a diagnostic-only command. And the mechanism requires the wedged process to still serve MCP — verified to hold for the evidenced cases (`CommandRegistry::Dispatch` releases its mutex before invoking the handler, `Source/Core/src/Commands/CommandRegistry.cpp:353-356`; `--spawn` proves reachability before dispatching, `Source/Standalone/CliDispatch.cpp:182`), with attach-from-outside via the existing procdump recipes as the fallback for a process wedged harder than that.

# Considered Options

- **In-process `SuspendThread` + `StackWalk64` returning inline frames** (rejected). Greenfield — the repo contains no thread-suspension or stack-walking code anywhere — in an application with a shipped seven-issue hang-class plan, where suspend-then-allocate is a classic deadlock. Plus the three-link-surface cost above, one surface of which CI would not catch.
- **A dedicated `ThreadIntrospection` provider seam modelled on `AiClientFactory::SetTestOverride` / `AnnotateAnalysisConfig::P4RunOverride`** (rejected). Those headers mark themselves test-only — `AiClientFactory.h:19` says "Production code MUST NOT call this" — so modelling a production host capability on them invites the objection that a test mechanism is being widened. `HostCallbacks` is the documented host channel and already has three real installers.
- **Adding a method to `IAppDebug`** (rejected). It does not do what it appears to: `AppController` implements `IAppDebug` and lives in `Source/Core`, so the platform code would not move. The facet is a fan-in narrowing device, not a platform boundary.
- **Out-of-process debugger attach for every capture** (rejected as the primary path, kept as fallback). Safest in principle, but requires a debugger present and process access, and is unnecessary when the wedged process still answers MCP.
