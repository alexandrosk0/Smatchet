# Smatchet — Architecture Review (without agents.md)

**Reviewer role:** Principal software architect
**Scope:** Structural health of the Smatchet codebase — separation of concerns, abstraction quality, dependency direction, modularity, layering, extensibility, portability boundaries.
**Date:** 2026-06-30

---

## 1. Executive Summary & Architectural-Health Verdict

Smatchet is an issue-tracker desktop client (Jira / Plane / GitHub / Linear backends) that ships as both a standalone GLFW/GL3 executable and an Unreal-embeddable DX12 static library, plus an Android `.so`, from **one engine-agnostic source tree**. After reading the real code, the headline verdict is: **this is an unusually disciplined C++ codebase with a small number of well-understood, actively-managed structural debts.**

The load-bearing abstractions are genuinely good. `ITrackerBackend` is a textbook Interface-Segregation decomposition (six capability sub-interfaces, nullable optionals for unsupported capabilities). The Unified Command registry is a clean single-definition → five-frontend fan-out (CLI, Palette, MCP, Lua, Unreal) with a single dispatch chokepoint. The dual render-target design keeps the graphics API out of Core by *physical file exclusion* plus `#ifdef` platform seams, not by leaking GL/DX12 types into Core headers. Plugin gating via `SMATCHET_WITH_*` is matched with a configure-time guard (ADR-0002 shim-link discipline) that turns a class of ODR/macro-skew crashes into a CMake `FATAL_ERROR`.

The principal liability is **`AppController` — a 1,465-line header with ~153 public methods and 117 includers.** It is a textbook god-object and fan-in hub. The team clearly knows this: there is a multi-phase `appcontroller-fan-in` plan, the header is aggressively de-coupled (json_fwd not json.hpp, pImpl for sol2, service extraction behind `IOfflineQueueDeps`/`ITicketSyncDeps`), and the include-cycle and function-size baselines are both **zero**. The debt is being paid down methodically rather than ignored. That distinction is what separates a structurally healthy codebase from a decaying one.

**Architectural-health verdict: STRONG (B+/A−).** The abstractions are load-bearing and correct; the debt is identified, fenced, and ratcheted. The risk is concentrated in one god-object whose remediation is in-flight, and in a 2,179-line CMakeLists.txt whose complexity is essential rather than accidental but is approaching the limit of maintainability.

---

## 2. Scope & Method

I read the real code and structural artifacts: `Source/**` (Core/Standalone/Plugins/Mobile/UnrealPlugins), `CMakeLists.txt`, `CMakePresets.json`, the `include/` vs `src/` layout, `docs/STRUCTURE.md`, `docs/PORTABILITY.md`, `CONTEXT-MAP.md`, `docs/adr/**`, and the `docs/high-integrity/**` baselines (include-cycle, dup, function-size, strict-zone). Specific files cited with `file:line` throughout.

**Deliberately ignored (per pass constraint):** the entire agentic-governance meta-layer — every `AGENTS.md`, the `agents/` directory, `AI_POLICY.md`, `.coderabbit.yaml`, `.cursor/`, `docs/agent-rules/**`, `docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`. Where `docs/STRUCTURE.md` and `docs/PORTABILITY.md` describe the portable/project boundary, I read them only for the *code* structure they document, not the agent process. This review judges the software architecture on its own terms.

---

## 3. System Decomposition & Layering Map

The source tree is a single root (`Source/`, ADR-0011 "single source root") with a clean `include/` vs `src/` split inside `Source/Core`. The decomposition:

```
Source/
├── Core/                          engine-agnostic library (CORE_SOURCES glob)
│   ├── include/<Subsystem>/       public headers
│   ├── src/<Subsystem>/           impl
│   │   ├── Tracker/    backends + field catalog + HTTP plumbing
│   │   ├── Sync/       offline queue, ticket sync, network usage
│   │   ├── Persistence/ SQLite LocalCacheManager, image/texture cache
│   │   ├── Config/     ConfigManager, ViewsStore
│   │   ├── Commands/   unified registry + Builtin/ + Scenarios/
│   │   ├── Ui/         ImGui panes (mostly EXCLUDED from Core for the host TU)
│   │   ├── Vcs/        Perforce / git blame
│   │   ├── Diagnostics/ Imaging/ Privacy/
│   │   └── AppController*.cpp  (the fan-in hub, split across ~11 TUs)
├── Standalone/                    GLFW/GL3 exe (main.cpp, Dx12Bootstrap, CLI)
├── Plugins/{Mcp,LuaConsole,Whisper}  CMake-gated
├── Mobile/Android*                EGL/GLES host over the same Core
└── UnrealPlugins/SmatchetImGuiPlugin  DX12 embedding
```

**Build-target layering** (from `CMakeLists.txt:1499–1577`): the same `CORE_SOURCES` glob (`CMakeLists.txt:1114`) feeds *four* targets — `SmatchetStandalone` (exe, `:1831`), `SmatchetCore_DX12` (STATIC, EXCLUDE_FROM_ALL, `:1499`), `SmatchetMobile` (SHARED, `:1520`), and `SmatchetCore_PosixCheck` (compile-only portability gate, `:1562`). This four-target convergence on one source set is the structural spine of the whole portability story.

**Dependency direction (intended):** `Standalone/Mobile/Unreal host → Core → subsystems → leaf "Pure" headers`. The `*Pure.cpp/.h` naming convention (e.g. `TrackerFieldCatalogPure`, `JiraIssueMappingPure`, `MembershipDiffPure`) marks a genuine **functional core / imperative shell** split: pure value-transform code that is STL-only and unit-testable without HTTP, SQLite, or ImGui. This is one of the best things in the codebase and appears consistently across Tracker, Sync, and Commands.

`SmatchetCoreInterface` (`CMakeLists.txt:1272`) is the INTERFACE target carrying include dirs + the third-party link set (nlohmann_json, SQLiteCpp, cpr, httplib, md4c). Both render targets consume it, which is how a single dependency declaration stays consistent across the GL and DX12 worlds.

---

## 4. Load-Bearing Abstractions

### 4.1 `ITrackerBackend` — the core abstraction (excellent)

`Source/Core/include/ITrackerBackend.h` is 23 lines and does exactly one thing: it composes six capability interfaces.

```cpp
class ITrackerBackend {
    virtual ITrackerIssueReader& Reader() = 0;          // mandatory
    virtual ITrackerConnectivity& Connectivity() = 0;   // mandatory
    virtual ITrackerFieldCatalog* FieldCatalog() = 0;   // nullptr if unsupported
    virtual ITrackerIssueMutations* Mutations() = 0;    // nullptr if unsupported
    virtual ITrackerCollaboration* Collaboration() = 0; // nullptr if unsupported
    virtual ITrackerActivity* Activity() = 0;           // nullptr if unsupported
};
```

This is **Interface Segregation done right.** Reading (mandatory, by-reference) is separated from mutation, field catalogs, collaboration, and activity feeds (optional, by-pointer, `nullptr` == unsupported). A read-only fixture backend implements two methods and returns null for the rest. The "sixth role" `ITrackerActivity` was added cleanly (ADR-0021) without disturbing the other five — direct evidence the segregation is genuinely extensible, not just aspirational.

`ITrackerIssueReader.h` further demonstrates the **default-method / capability-fallback** pattern (`:40–130`): `FetchIssuesChangedSince`, `FetchIssueKeysForView`, and `ProbeIssueExists` ship correct-but-heavy default implementations, so a new backend works immediately and *opts in* to native server-side filtering by overriding. The default for `ProbeIssueExists` is the safe-conservative `true` (`:120`) — a deliberate choice that prevents a backend that can't probe from destructively deleting cache rows. This is mature interface design that reasons about failure modes, not just happy paths.

**Adding a 4th backend** is genuinely cheap. `DefaultTrackerBackendFactory.cpp:11–35` is a single case-insensitive switch; the factory itself is behind `ITrackerBackendFactory` (`Source/Core/include/ITrackerBackendFactory.h`) so embedding hosts and tests inject mock backends without touching `AppController.cpp`. The factory doc-comment (`:24–30`) even encodes the #979 live-cfg contract (build from caller's in-memory `cfg`, never a disk re-read that races the debounced prefs save). Reality check: the repo already has **four** production backends plus Linear, GitHub, and Plane fixture backends — the "add a 4th" question is answered by the fact they already added a 4th *and* a 5th.

**Shared HTTP plumbing** lives in `TrackerHttpClient.h` / `TrackerHttpUtils` / `TrackerHttpPure` — a central `TrackerHttpRequestWithRetry` with typed `TrackerError` classification, exponential backoff, and a POST-safe (transport-only) retry predicate so non-idempotent mutations are never double-fired (`TrackerHttpClient.h:51–54`). Field-catalog logic is shared via `FieldCatalogCache` + `TrackerFieldCatalogPure`. So the per-backend `.cpp` files (`JiraIssueMutation.cpp` at 47KB is the largest) carry backend-specific mapping, while retry, classification, and field semantics are factored out.

**Leakiness:** minor. The `const` accessors on `ITrackerBackend` use `const_cast` (`:21–22`) — a pragmatic but slightly smelly way to provide const overloads. And the interface exposes `cpr::Response` through `TrackerHttpResult` (`TrackerHttpClient.h:29`), so cpr leaks into any TU that touches the HTTP helper directly — though notably *not* into `ITrackerBackend` itself, which is cpr-free (a deliberate win documented at `AppController.h:48–53`).

### 4.2 Unified Command registry — single-def fan-out (excellent)

`Source/Core/include/Commands/Command.h` is the crown jewel of the design. One `Command` struct (`:159`) carries name, params (`ParamSpec` with bounds/enum/maxlen), `Destructive`/`Idempotent`/`AsyncSafe`/`DryRunSupported` flags, and a `std::function` handler. The header comment (`:6–10`) names all five frontends, and the code backs it up:

- **MCP:** `McpPlugin.cpp:498–500` calls `app->Commands().All()` and emits `{name, description, inputSchema: c.BuildJsonSchema()}` for `tools/list`; `tools/call` routes to `Commands().Dispatch()` (`:529, 644, 760`).
- **CLI:** `CliCommandRunner.cpp:1351` dispatches with `CommandSource::Cli`.
- **Lua:** `AppController_LuaBindings.cpp` exposes `commands.invoke`.
- **Palette / Unreal:** same `Dispatch` entry.

`CommandRegistry::Dispatch` (`CommandRegistry.h:54–57`) is the **single chokepoint**. The reentrancy contract (`:19–23`) — copy the `Command` (including its handler) under the mutex, release, then invoke — lets a Lua handler recurse into `commands.invoke` without deadlock. The destructive-confirm gate is a single pure predicate `RequiresExplicitConfirm` (`Command.h:139`) that deliberately treats *all* sources uniformly (no per-source bypass), with the security rationale inlined (`:120–138`). The `ErrorCode` and `CommandSource` enums are explicitly documented as stable wire contracts (`Command.h:50, 96`).

Architecturally this is as clean a fan-out as you can build in C++14: the cost of adding a command is one registration in a `Builtin/BuiltinCommands_*.cpp` file (there are 20 of them, cohesively split by category — Tickets, Fields, Sync, Offline, Ai, Attach, etc.), and it appears in *all five* frontends automatically. ADR-0010 (feature-gated registry) closes the loop: OFF builds simply omit the names and `Dispatch` returns `unknown-command` — no stub "disabled" handlers.

### 4.3 Dual render-target — graphics API kept out of Core (very good)

The mechanism is **physical file exclusion plus `#ifdef` platform seams**, not header-level abstraction. `CMakeLists.txt:1114–1116` globs `CORE_SOURCES` then `REMOVE_ITEM`s `SmatchetImGuiHost.cpp` (and its diagnostics sibling). That host TU is the *only* place the GL3 vs DX12 backend choice materializes (`imgui_impl_dx12.h` at `SmatchetImGuiHost.cpp:30`), and it's compiled into per-world OBJECT libraries (`ImGuiLib` / `ImGuiLib_DX12` / `ImGuiLib_Mobile`, `CMakeLists.txt:1048–1095`) instead of being baked into Core. The rest of Core is `#ifdef _WIN32 ... #else` (the Android NDK side compiles the `#else` branch, `:1509`). The result: **no `<GL/...>`, `<d3d12.h>`, or `glfw` include appears in any `Source/Core/include/**` header.** ImGui itself is engine-agnostic (it's a UI immediate-mode lib, not a graphics API), so Core *can* and does use ImGui types freely; the actual GPU backend is what's quarantined, and it is.

The `SmatchetCore_PosixCheck` target (`:1562`) is a compile-only portability *gate*: it builds the entire `#else` (non-`_WIN32`) path under host Linux clang so portability regressions fail at CI rather than at the Unreal/Android integration point. This is excellent — the portability boundary is *enforced*, not just documented in `docs/PORTABILITY.md`.

### 4.4 Plugin gating + shim-link discipline (excellent, and unusually rigorous)

Five feature flags: `SMATCHET_WITH_{AI,LUA_AUTOMATION,MCP,MCP_UNREAL,WHISPER}`. The naive version of this design has a well-known failure: a plugin TU compiled with `SMATCHET_WITH_AI` undefined gets a *different* struct layout for a shared type than the Core it links against, and you get a SIGSEGV inside `std::string::clear()` from a plugin TU. Smatchet solves this with **ADR-0002 shim-link discipline**: each flag also defines an INTERFACE shim library (`SmatchetCoreMcpShim`, `SmatchetCoreAiShim`, `CMakeLists.txt:1346–1356`), and a configure-time guard function `smatchet_assert_plugin_shim_links` (`:1363–1381`) emits `FATAL_ERROR` if a plugin includes Core headers without linking the matching shim. The crash signature is even documented inline. This is a structural defense turning a runtime ODR landmine into a build-time failure — a sign of architects who have been bitten and engineered the bite out.

Config-skew risk is real but bounded: the MCP define is deliberately *not* on `SmatchetCoreInterface` (`:1344`) because Unreal DX12 libs must omit MCP without an MSVC `/D` vs `/U` clash, so MCP-gated code links the shim instead. This is subtle and the kind of thing that rots; the guard is what keeps it honest.

---

## 5. Coupling / Cohesion & Dependency-Direction Findings

### 5.1 `AppController` — the god-object (the central debt)

The numbers: `AppController.h` is **1,465 lines, ~153 public methods, 117 includers** (`grep` over `Source/`). The implementation is split across ~11 TUs (`AppController.cpp` 2,835 lines, plus `_LuaBindings`, `_Attachments`, `_Connectivity`, `_CatalogAndFieldEdit`, `_IssueCreateOffline`, etc.). This is unambiguously a god-object and a fan-in hub: it owns the backend, the cache, the command registry, the offline queue, sync, Lua bindings, AI assistant, connectivity, attachments, and multi-grid pane contexts.

**What makes this tolerable rather than fatal** is the active, sophisticated remediation visible in the header itself:

- **sol2/Lua fully lifted off the header (hardening #19c, `:8–22`):** zero sol2 includes; binding methods live on `AppController::Impl` (pImpl) behind `ILuaBindingHost`. The ~100 Ui/Commands includers stopped transitively compiling `<sol/sol.hpp>`.
- **json_fwd not json.hpp (`:65–69`):** the header names `nlohmann::json` only by-reference in out-of-line decls, so the heavy full json.hpp no longer rides the edge to 114 includers.
- **cpr lifted (`:48–53`):** `JiraClient.h` used to drag cpr into every includer; now the five `ITracker*` role interfaces (cpr-free) are included directly.
- **Service extraction behind interfaces:** `OfflineQueueService` and `TicketSyncService` take `IOfflineQueueDeps&` / `ITicketSyncDeps&` (Dependency-Inversion), with `GridContextDepsAdapter` as the single `friend` (`:135–145`) instead of `friend class OfflineQueueService` + `friend class TicketSyncService`. Tests substitute `FakeOfflineQueueDeps` / `FakeTicketSyncDeps`.
- **Nested types relocated to rank-0 leaf headers (`:98–106`):** `FieldEditResult`, `TrackerConnectivityState`, offline-queue summary structs moved to `Types/` and re-exported via `using`-aliases so editing them no longer recompiles 115 includers.

So the *fan-in surface* is being narrowed even though the *class* is still huge. The include-cycle baseline is **0** (`docs/high-integrity/include-cycle-baseline.md`) and the function-size baseline is **0** (`docs/high-integrity/function-size-baseline.md`, cap 120 lines non-UI / 200 ImGui-draw, 30 branches) — meaning despite the god-object, there are no oversized functions and no include cycles anywhere in the tree. That is a remarkable discipline result.

**Residual smell — upward dependency from lower subsystems into the hub:** `TicketSyncService.cpp:3` and `OfflineQueueService.cpp` still `#include "AppController.h"`, as do several Tracker TUs (`JqlSuggestEngine.cpp`, `TrackerGridFieldDisplay.cpp`, `TrackerLabelsEditor.cpp`). Crucially, the *headers* are clean — `TicketSyncService.h` depends only on `ITicketSyncDeps` (forward-declared, `:24`) and rank-0 leaf types (`:20–22`). So the DIP seam is real at the header/link level; the `.cpp` include is transitional implementation coupling, not an architectural cycle (the baseline confirms zero cycles). This is the right shape mid-migration but should be finished.

### 5.2 Tracker → Ui dependency (minor smell)

`TrackerLabelsEditor.cpp` and `TrackerDateTimeFieldEditor.cpp` include Ui/ImGui headers — they're field *editors* that render. These three files are also the entire `define-imgui` strict-zone baseline (`baseline.md:15–18`). It's a defensible pragmatic coupling (an inline editor is intrinsically UI), but it means "Tracker" is not purely a data/transport subsystem — it carries some presentation. A cleaner split would move the editor widgets into Ui and keep Tracker as pure backend logic.

### 5.3 Cohesion of subsystems (generally high)

- **Tracker** (largest, ~80 files): high cohesion around "talk to a tracker," with the `*Pure` split isolating mapping logic. The one blemish is the embedded editor UI (5.2).
- **Sync**: tight, interface-fronted (`OfflineQueueService`, `TicketSyncService`, `NetworkUsageTracker`, the `*Pure` diff helpers). Clean.
- **Persistence**: `LocalCacheManager` (1,334-line .cpp) fronted by `ISyncCache` (71-line interface, ADR-0020). High cohesion; one large class but appropriately so.
- **Commands**: registry + 20 cohesive `Builtin/` category files. Excellent.
- **Privacy / Diagnostics / Imaging**: small, focused, low coupling.

---

## 6. Architectural Debt & Risks

1. **`AppController` god-object (HIGH, in-flight).** 153 public methods / 117 includers. Remediation is real and ratcheted but unfinished; until the `.cpp` upward-includes are cut and the class is decomposed into owned services, it remains the single biggest change-amplifier in the codebase. Every includer recompiles when the (still-large) header changes meaningfully.

2. **126KB / 2,179-line `CMakeLists.txt` (MEDIUM).** Much of the complexity is *essential* — four render targets over one source set, plugin shims, portability gates, strict-warning application, FetchContent dependency wiring, fault-injection guards. But a single 2,179-line build file is at the edge of maintainability; the per-target configure functions and shim logic would be more navigable split into `cmake/` modules (there is a `cmake/` dir already, suggesting partial extraction). Risk: build-system changes are high-cognitive-load and concentrate in one file with no module boundaries.

3. **C++14 hard constraint cost (MEDIUM, accepted).** Unreal MSVC compat forces C++14 (`CMakeLists.txt:331`), banning `std::optional` / `std::variant` / `std::string_view` / structured bindings / `if constexpr`. The team paid this down with a hand-rolled `Optional<T>` + `Result<T,E>` (`SmatchetResult.h`) over manual aligned-storage with placement-new — correct and assert-guarded, but it's maintenance the stdlib would otherwise provide, and every new dev must learn the local types. `Command.h:11–13` documents the constraint inline. This is an accepted, well-managed cost, not a mistake, but it's permanent friction.

4. **Residual `.cpp` upward-includes into `AppController.h` (MEDIUM).** Sync/Tracker implementations include the hub. The header seams (`ITicketSyncDeps` etc.) exist; the `.cpp`s haven't been cut over. Low correctness risk (no cycles), but it keeps the fan-in compile coupling alive.

5. **`const_cast` in `ITrackerBackend` const accessors (LOW).** Cosmetic; a pair of mutable/const virtual overloads would be cleaner, but the cost is trivial.

6. **Config-skew across `SMATCHET_WITH_*` matrix (LOW, guarded).** The shim-link guard (ADR-0002) defuses the dangerous case at configure time, but the flag matrix (5 flags × DX12/Standalone/Mobile) is a combinatorial space; only the shipped combinations are guaranteed exercised.

---

## 7. Scorecard

| Dimension | Score | Rationale |
|---|---|---|
| **Abstraction quality** | 9/10 | `ITrackerBackend` ISP, command `Command` struct, `ISyncCache`, `ITrackerBackendFactory` are all clean and load-bearing. Minor `const_cast` / cpr-via-helper leaks. |
| **Layering / dependency discipline** | 7/10 | Include-cycle baseline = 0 and function-size = 0 is exceptional. Docked for the `AppController` fan-in (117 includers) and the residual `.cpp` upward-includes from Sync/Tracker. |
| **Modularity / cohesion** | 8/10 | Subsystems are cohesive; the functional-core `*Pure` split is excellent. Tracker carries some editor UI (cohesion blemish); AppController is the low-cohesion outlier. |
| **Extensibility** | 9/10 | Adding a backend = one factory case (already done 4×); adding a command = one registration that fans out to 5 frontends; capability-default methods let backends opt in incrementally. |
| **Portability design** | 9/10 | One source set → 4 targets, graphics API quarantined by file exclusion + `#ifdef`, and a compile-only `PosixCheck` gate that *enforces* the boundary at CI. Best-in-class. |
| **Build-system architecture** | 6/10 | Sophisticated and correct (shims, presets, FetchContent, portability gates) but concentrated in a 2,179-line monolithic CMakeLists with little module extraction. |
| **Persistence / sync design** | 8/10 | `ISyncCache` seam (ADR-0020), dead-letter queues, conflict detection (ADR-0016), idempotent cache-meta flags, multi-grid backend-key namespacing. Solid; `LocalCacheManager` is large but cohesive. |
| **Overall** | **8/10** | Strong, principled architecture with one concentrated, actively-remediated god-object and a build file approaching its complexity ceiling. |

---

## 8. Prioritized Architectural Recommendations

1. **Finish the `AppController` decomposition (highest leverage).** The seams already exist (`IOfflineQueueDeps`, `ITicketSyncDeps`, `ILuaBindingHost`, the `Types/` leaf headers). Next moves: (a) cut the `.cpp` upward-includes in `Sync/` and `Tracker/` so no lower subsystem includes `AppController.h`; (b) continue extracting owned services (connectivity, attachments, AI) behind `I*Deps` interfaces until `AppController` is a thin coordinator that *wires* services rather than *implements* them. Target: drop the public surface well below 153 methods and the header below ~500 lines. This directly shrinks the 117-includer recompile blast radius.

2. **Modularize `CMakeLists.txt`.** Extract the per-target configure functions (`smatchet_configure_dx12_core_impl_target`, `smatchet_apply_strict_warnings`, `smatchet_assert_plugin_shim_links`), the shim definitions, and the FetchContent dependency block into `cmake/*.cmake` modules included from a slim top-level file. Aim for a top-level file that reads as a table of contents. This is pure maintainability with no behavior change.

3. **Move Tracker's inline field-editor UI into `Ui/`.** `TrackerLabelsEditor` / `TrackerDateTimeFieldEditor` / `TrackerGridFieldDisplay` are the entire `define-imgui` strict-zone baseline and the only reason Tracker depends on ImGui. Relocating the widget layer leaves Tracker as pure backend/data logic and removes a layering exception.

4. **Document and CI-exercise the `SMATCHET_WITH_*` flag matrix.** The shim guard defends the dangerous case, but add a CI lane (even compile-only, like `PosixCheck`) that builds the off-by-default combinations so config-skew is caught structurally, not just the shipped permutations.

5. **Consider a thin abstraction over `cpr::Response` in the HTTP helper.** `TrackerHttpResult` exposes raw `cpr::Response`, leaking cpr into every direct HTTP call site. A minimal `{status, headers, body}` value type would let cpr be swapped (or mocked) without touching call sites — low priority given `ITrackerBackend` is already cpr-free, but it would complete the transport-abstraction story.

6. **Low-priority cleanups:** replace the `ITrackerBackend` `const_cast` accessors with proper const/non-const virtual pairs; and keep the `*Pure` discipline as the default for all new mapping/transform code — it is the single biggest reason this codebase is testable, and it should be a stated invariant for new subsystems.

**Closing note:** The defining characteristic of this architecture is not that it has no debt — it's that the debt is *named, fenced, and ratcheted* (zero include cycles, zero oversized functions, ADRs for every load-bearing decision, baselines that can only shrink). That is the structural signature of a codebase that will stay healthy at scale, provided the `AppController` decomposition crosses the finish line before the next major subsystem lands on it.
