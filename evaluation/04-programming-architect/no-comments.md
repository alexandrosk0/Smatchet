# Smatchet — Architecture Review (comment-stripped mirror)

**Reviewer role:** Principal software architect
**Special constraint:** All C++ read from a **comment-stripped mirror** (line numbers preserved; blank lines mark where comments were). No ADRs, no `docs/high-integrity/**`, no agentic meta-layer. The question under test: *how much of the architecture is legible from names, types, and structure alone?*
**Date:** 2026-07-01

---

## 1. Executive Summary & Verdict

Read blind — signatures, types, file/dir structure, identifier naming, and control flow only — Smatchet still reads as **an unusually disciplined, engine-agnostic C++14 codebase.** The load-bearing abstractions survive comment stripping almost fully intact because they are carried by *types*, not prose: `ITrackerBackend` is a six-interface Interface-Segregation facade whose mandatory-vs-optional contract is encoded in `reference` vs `pointer` return types; the Unified Command registry's four/five-frontend fan-out is spelled out by the `CommandSource` enum and the self-describing `MakeCommand("name","description",...)` registrations; the dual render-target seam is legible from the `void* NativeDevice` / `SmatchetRendererBackend` enum handshake plus the physical separation of `Core/` from `UnrealPlugins/` and `Standalone/`.

The single biggest legibility casualty is **`AppController`**. Its header is **1,465 lines and — in the real repo — 825 of those lines (56%) were comment lines.** Stripped, it collapses into a wall of ~150 bare method declarations separated by runs of blank lines, each blank run marking a deleted per-method contract note. The *shape* (pImpl, `SMATCHET_WITH_*` gating, service composition) is still visible, but the *why* — ordering invariants, threading contracts, the live-cfg race rationale, which methods are UI-thread-only — is gone. That is where the code stopped self-documenting and started depending on prose.

**Verdict blind: STRONG (B+), essentially unchanged from the code+comments pass.** The core abstractions are so type-driven that stripping comments barely dents them. The god-object was *already* the identified debt; without comments it is simply harder to navigate safely, which sharpens rather than changes the finding. A new architect could safely refactor the Tracker, Commands, Sync, and Persistence layers blind; they should **not** refactor `AppController` internals blind without restoring the comments or reading the (withheld) ADRs.

---

## 2. Method & Constraint

I read only `Source-nocomments/**` for C++, covering: the six `ITracker*` role interfaces + `ITrackerBackend` + `ITrackerBackendFactory`; `JiraClient.h`, `PlaneClient.h`, `GitHubClient.h`, `DefaultTrackerBackendFactory.cpp`; `SmatchetImGuiHost.h` and the Unreal `SmatchetImGuiRenderBackend.h`; `AppController.h` + `AppControllerImpl.h`; the `Commands/` registry (`Command.h`, `CommandRegistry.cpp`, `Builtin/BuiltinCommands_Sync.cpp` as a representative fan-out sample); `Persistence/LocalCacheManager.h/.cpp` (schema); `Sync/OfflineQueueService.cpp`, `Sync/TicketSyncService.cpp`. I read `README.md` and `BUILD.md` for orientation and `CMakeLists.txt` for the `SMATCHET_WITH_*` gating (CMake comments are **not** stripped, so I lean on the C++ for the self-documentation test). I deliberately did not open any ADR, high-integrity baseline, or agentic doc. Comment-density figures come from `grep` over the *real* `Source/` tree to quantify what was removed. Citations are `file:line` against `Source/` (mirror preserves line numbers).

---

## 3. Architectural Legibility WITHOUT Comments

**What the structure and naming carry on their own — successfully:**

**3.1 `ITrackerBackend` — the intent is in the type system.** `ITrackerBackend.h` (23 lines) forward-declares six interfaces and exposes them via accessors. Mandatory capabilities return **references** (`ITrackerIssueReader& Reader()`, `ITrackerConnectivity& Connectivity()`, `:13–14`); optional capabilities return **pointers** (`ITrackerFieldCatalog* FieldCatalog()`, ... `:15–18`). The reference/pointer split *is* the "mandatory vs. nullable-when-unsupported" contract — no comment needed to infer it, and any C++ reader will read `T*` on an accessor as "may be null." The default-method fallback pattern in the role interfaces is even more self-documenting: every optional method's default body returns `TrackerErrorInvalidRequest("BuildCreatePayload is not supported by this backend.")` (`ITrackerIssueMutations.h:38–65`, `ITrackerFieldCatalog.h:23–39`). The error *string literal* names the capability contract. This is documentation that cannot be stripped because it lives in a runtime value.

**3.2 The concrete-client realization is obvious.** `JiraClient : public ITrackerBackend, public ITrackerIssueReader, ... (all six) ` (`JiraClient.h:30–36`) plus the facade accessors returning the client itself makes the "one class implements all roles, exposes them through the facade" pattern legible from the class declaration alone. `GetTrackerType() const override { return "Jira"; }` (`:44`) is a self-labeling identity. `DefaultTrackerBackendFactory::Create` (`DefaultTrackerBackendFactory.cpp:11–35`) is a lowercased-string switch with Jira as the fallback — trivially readable, and it reveals a **fourth production backend (`LinearClient`) the README's "three backends" prose omits.** The code out-documents the docs here.

**3.3 The Command registry fan-out is self-describing.** `CommandSource { Cli, Palette, Mcp, Lua, Unreal, Internal }` (`Command.h`) names every frontend as an enum value — the "one registry feeds N frontends" claim is recoverable from that enum without any comment. The `Command` struct's boolean metadata (`Destructive`, `Idempotent`, `AsyncSafe`, `DryRunSupported`) is self-labeling, and each registration embeds its own documentation as a required argument: `MakeCommand("sync.full", "Full sync: wipe local cache and re-fetch all tickets from tracker.", ...)` (`BuiltinCommands_Sync.cpp:48`). The `Dispatch` pipeline in `CommandRegistry.cpp:283–360` reads as a clean chokepoint: snapshot-under-lock → `ValidateAndResolveArgs` → destructive-confirm gate (`RequiresExplicitConfirm`) → dry-run gate → `try { handler } catch` → `RecordRecent`. The copy-command-under-mutex-then-invoke reentrancy pattern (`:286–295`) is visible in the code shape even though its *rationale* (Lua handlers recursing into `commands.invoke`) is a comment that's now gone.

**3.4 The dual render-target seam is legible from the handshake types.** `SmatchetImGuiHost.h:17–35` defines `enum class SmatchetRendererBackend { Unknown, Dx12, Ps5, Xbox }` and `SmatchetRendererInitInfo` carrying `void* NativeDevice`, `void* NativeCommandQueue`, `void* RendererResource0..2`. `RenderDrawData(SmatchetRendererBackend backend, void* nativeCommandList)` (`:58`) passes the graphics command list as an opaque `void*`. A reader immediately sees "the concrete graphics API is passed as opaque handles, so Core never names d3d12/vulkan types." Confirmed by grep: **no `glfw`/`opengl`/`d3d12`/`vulkan` include appears in any `Core/include/**` header** (ImGui *does* appear, correctly — it's an engine-agnostic immediate-mode UI lib, not a GPU API). The Unreal side mirrors this exactly: `FSmatchetRendererInitParams` has the same `void*` fields, and `ISmatchetImGuiRenderBackend` + `CreateSmatchetImGuiRenderBackend()` (`SmatchetImGuiRenderBackend.h`) is where `RHIResources.h` / DX12 finally enters — physically outside `Core/`. The directory boundary (`Core/` vs `UnrealPlugins/` vs `Standalone/` vs `Mobile/Android/`) does most of the layering documentation by itself.

**3.5 Functional-core / imperative-shell is signalled by a naming convention.** 61 files carry a `*Pure*` suffix (`TrackerFieldCatalogPure`, `JiraIssueMappingPure`, `TicketChangeDiffPure`, `MembershipDiffPure`, ...). The suffix is a self-documenting convention: "STL-only value transforms, no HTTP/SQLite/ImGui." A reader learns the split from the filenames without a single comment.

**3.6 Persistence & offline-sync design reads from the schema and the seams.** `LocalCacheManager : public ISyncCache` (`LocalCacheManager.h:31`) is dependency inversion visible in the class head. The `.cpp` shows explicit schema evolution (`tickets` → `tickets_v2`, `ticket_field_values_v2`, additive `SqliteTableHasColumn` probing), `PRAGMA journal_mode=WAL` + `synchronous=NORMAL`, and dead-letter tables (`pending_creates_dead`, `pending_field_edits_dead`) that name the offline-replay failure path. `TicketSyncService(ITicketSyncDeps& deps)` and `OfflineQueueService` fronted by `IOfflineQueueDeps`/`ISyncCache` show constructor DI through interfaces — the seam is in the constructor signature, not a comment.

**3.7 Plugin gating is legible from the `#if defined(SMATCHET_WITH_*)` blocks.** `AppControllerImpl.h` cleanly brackets Lua (`sol::state lua`), AI (`std::unique_ptr<AiAssistantController>`), and MCP (`mcpActivityLog_`) members behind their flags; `AppController.h` gates `LoadAiChatMessages` behind `SMATCHET_WITH_AI`. The four flags (AI 58 refs, LUA_AUTOMATION 56, MCP 52, WHISPER 33) partition the optional subsystems consistently. No comment is needed to see what's optional.

---

## 4. Where Comments Were Load-Bearing (intent/invariants lost)

Comment stripping did real damage in a few concentrated places:

**4.1 `AppController.h` — the biggest loss by far.** Real header: **1,465 lines, 825 comment lines (56%).** Stripped, it is ~150 bare declarations interleaved with blank runs. What vanished:
- **Threading contracts.** `IsOnUiThread()` / `PostToMainThread()` survive as signatures, but *which* of the ~150 methods are UI-thread-only vs. worker-safe was carried in per-method notes. Blind, you cannot tell whether `SyncWithBackend()` or a field-edit submit is safe to call off the UI thread.
- **Ordering invariants.** The private init sequence (`InitConfig` → `WireCoreServices` → `InitBackends` → `InitFieldCatalog` → `InitPlugins` → `InitCommands`, `:185–231`) is *named* well enough to infer a phase order, but the constraints ("must run before X", "legacy sweep only for tracker Y") were prose.
- **The live-cfg / debounced-prefs-save race** that motivates `ITrackerBackendFactory::Create` taking an in-memory `cfg` (never a disk re-read) — the interface comment that encoded that contract (`ITrackerBackendFactory.h:9–29`, now blank) is exactly the kind of *why* that a signature cannot carry. A refactorer could "helpfully" reload config from disk and reintroduce the race.

**4.2 The `void*` render handshake — the *why*, not the *what*.** The *what* (opaque handle) is self-evident; the *why* (avoid pulling `d3d12.h`/RHI into Core to keep the engine-agnostic invariant) is a design rationale that a maintainer could violate by "cleaning up" `void* NativeDevice` into a typed pointer. `SmatchetRendererInitInfo:26–34`'s field-purpose notes (what `RendererResource0..2` and `NumSrvDescriptors` map to per backend) are gone; those fields are now unlabeled `void*`s whose meaning is backend-specific and undiscoverable without the comment or the plugin-side code.

**4.3 The plugin shim-link discipline is invisible blind.** In the code+comments pass this was a headline finding (ADR-0002: an INTERFACE shim per flag, a configure-time `FATAL_ERROR` if a plugin includes Core headers without linking the matching shim, defusing an ODR/layout-skew SIGSEGV). Blind, and forbidden the ADRs, I can see the `SMATCHET_WITH_*` `#ifdef` blocks but **the ODR-hazard rationale and the crash signature it prevents are entirely lost** — that knowledge lived in comments + ADR prose, not in any C++ signature.

**4.4 Small semantic nuances.** `ProbeIssueExists` defaulting to `true` (`ITrackerIssueReader.h:120–123`) reads blind as a trivial stub; the *safety rationale* (a backend that can't probe must not let the sync layer destructively delete cache rows) was a comment. `RequiresExplicitConfirm` ignoring `source` (`Command.h`, `(void)source;`) looks like dead-parameter smell blind; the intent (deliberately *uniform* confirmation across all frontends, no automation bypass — a security decision) was prose.

---

## 5. Self-Documentation Assessment (naming / types / decomposition as a comment substitute)

**Grade: high, and unusually type-driven.** The codebase's naming and decomposition do most of the documentation work:

- **Interface Segregation via types** (reference=required, pointer=optional) is the strongest single example: the contract is *unstrippable* because it's in the return types.
- **Error-string-as-contract** (`"X is not supported by this backend."`) turns the capability matrix into runtime-visible data.
- **Suffix conventions** (`*Pure`, `*Fixture`, `I*` interfaces, `*Deps` DI seams, `*_Internal.h`, `*_detail.h/.cpp`) form a consistent vocabulary that substitutes for structural comments. A reader learns "`Pure` = testable value logic," "`I…Deps` = injected dependency seam," "`…Fixture` = test backend" without prose.
- **Category-partitioned files** (`BuiltinCommands_{Sync,Tickets,Fields,Offline,Ai,Attach,...}.cpp`) make cohesion visible in the directory listing.
- **Embedded description strings** in command registrations are documentation that ships as data.

Where self-documentation *fails* is precisely where the domain has invariants that types can't express: threading affinity, temporal ordering, cross-subsystem race conditions, and ODR/build-layout hazards. `AppController` concentrates all four, which is why it is both the architectural debt *and* the legibility cliff. The near-comment-free `ITrackerBackend.h` (1 comment line in the real repo) and `JiraClient.cpp` (3 comment lines / 176) prove the team *can* write code that needs no comments; `AppController.h` at 56% comments proves they reach for prose exactly when the design outgrows what C++14 types can state.

**Could a new architect safely refactor blind?** Layer by layer: **Yes** for Tracker, Commands, Sync, Persistence, and the render seam — the types and conventions are enough. **No** for `AppController` internals and the plugin-shim/CMake gating rationale — those need the comments (or the ADRs) restored first.

---

## 6. DELTA vs the Code+Comments Pass

**Findings reproduced blind (survived comment stripping):**
- `ITrackerBackend` = textbook ISP, mandatory-by-ref / optional-by-pointer, default-fallback methods. **Fully reproduced** from types alone — including the "sixth role `Activity()`" being a clean addition (visible as a parallel pointer accessor).
- Unified Command registry, single `Dispatch` chokepoint, five `CommandSource` frontends, one-registration-fans-out. **Fully reproduced** (enum + `MakeCommand` strings + `Dispatch` body).
- Dual render-target, graphics API kept out of Core by physical file/dir separation + `void*` handshake. **Reproduced** (the prior pass credited `REMOVE_ITEM` file-exclusion in CMake; blind I reached the same conclusion from the `Core/` vs plugin directory split and the grep-clean Core headers).
- Functional-core `*Pure` split. **Fully reproduced** (naming convention).
- `AppController` god-object: **1,465-line header, ~150 methods, pImpl, service extraction behind `I*Deps`, `json_fwd`/no-`sol2`/no-`cpr` in the header.** **Reproduced** structurally — the decoupling *techniques* are visible in the includes.
- Persistence: `ISyncCache` seam, dead-letter queues, schema versioning, WAL. **Reproduced** from the header + schema SQL.

**Findings lost or weakened without comments/ADRs:**
- **Shim-link / ADR-0002 discipline** — the prior pass's headline "unusually rigorous" plugin-gating finding is **lost**; blind I see the flags but not the ODR-crash defense.
- **The `include-cycle = 0` and `function-size = 0` baselines** — the prior pass leaned on `docs/high-integrity/**` to certify "no cycles, no oversized functions." Blind and forbidden those docs, I **cannot verify** this; it downgrades my confidence in the layering claim (I can see clean *header* seams but can't prove the absence of `.cpp` upward-include cycles without a full graph build).
- **The live-cfg race rationale**, **threading affinity per method**, and **`ProbeIssueExists=true` safety intent** — all **lost**.
- **The Tracker→Ui editor-coupling nuance** (that `TrackerLabelsEditor`/`TrackerDateTimeFieldEditor` are the entire `define-imgui` strict-zone) — partially lost; I can *see* the ImGui includes in those Tracker files blind, but the "this is the whole tracked exception set" framing came from a baseline doc.

**Score change:** My **overall score is unchanged at 8/10.** The architecture's quality is carried by types and structure, so stripping comments does not lower what the code *is* — it lowers what a reader can *safely conclude and change*. The one dimension I'd nudge is **layering discipline: 7 → 6.5 blind**, purely because I lost the baseline evidence that certified zero include cycles; the code still *looks* clean but the certification is gone. Everything else holds within rounding.

---

## 7. Scorecard

| Dimension | Score | Rationale (blind) |
|---|---|---|
| **Abstraction quality** | 9/10 | ISP encoded in return types, capability-default methods, error-string contracts, `ISyncCache`/`ITrackerBackendFactory`. Survives stripping fully. Minor `const_cast` const-accessor smell (`ITrackerBackend.h:21–22`) visible. |
| **Layering discipline** | 6.5/10 | Directory boundaries + clean headers (`json_fwd`, no cpr/sol2 in `AppController.h`) read well; docked more than the comments-pass (was 7) because the zero-cycle baseline that certified it is now unavailable, and the `AppController` fan-in is starker without per-method scoping notes. |
| **Modularity / cohesion** | 8/10 | Category-split command files, `*Pure` functional core, interface-fronted services all legible by name. `AppController` remains the low-cohesion outlier; Tracker carries some editor UI (visible via ImGui includes). |
| **Extensibility** | 9/10 | Add-a-backend = one factory case (a 4th, `LinearClient`, already present); add-a-command = one self-describing `MakeCommand` registration fanning to all frontends. Obvious from structure. |
| **Portability design** | 9/10 | `void*`+enum render handshake, Core headers grep-clean of GPU APIs, engine code physically in `UnrealPlugins/`/`Standalone/`/`Mobile/`. Best-in-class and fully legible blind. |
| **Persistence / sync design** | 8/10 | `ISyncCache` seam, dead-letter queues, schema `_v2` migration, WAL — all readable from header + SQL. `LocalCacheManager` large but cohesive. |
| **Code self-documentation / legibility-without-comments** | 7.5/10 | Type-driven abstractions and suffix conventions make most of the tree self-explanatory; sharply pulled down by `AppController.h` (56% comments in the real repo) where threading/ordering/race invariants are prose-only and vanish under stripping. |
| **Overall** | **8/10** | Strong, principled, type-driven architecture. Comment stripping barely dents *what it is*; it dents *what a reader can safely change* — concentrated almost entirely in the one god-object. |

---

## 8. Prioritized Recommendations (incl. add/keep-comments callouts)

1. **Finish the `AppController` decomposition (highest leverage) — and until then, treat its comments as load-bearing.** Blind, this header is the one place the code stops self-documenting. Extract owned services behind `I*Deps` until it is a thin wiring coordinator (<~500 lines); that both shrinks the recompile blast radius *and* removes the largest comment-dependence in the tree.

2. **KEEP/ADD comments where types can't speak — these are the ones to never strip:** (a) per-method **UI-thread affinity** on `AppController` (or better: encode it in the type system / an assertion the name reflects, e.g. `…OnUiThread` suffixes); (b) the **live-cfg race** contract on `ITrackerBackendFactory::Create`; (c) the **`void* NativeDevice/RendererResource*` field-meaning** on `SmatchetRendererInitInfo` (a `struct`-per-field naming or typed opaque wrappers would make it self-documenting); (d) the **`ProbeIssueExists` safe-default** and **uniform-confirm** security rationales. These four are where a blind refactorer is most likely to introduce a regression.

3. **Encode invariants in names/types where feasible, to reduce comment-dependence.** Rename UI-thread-only methods with an `OnUiThread`/`FromUiThread` suffix; give the render init-info fields typed opaque handles instead of bare `void*`. Every invariant moved from prose into a signature is one that survives stripping.

4. **Surface the layering/shim guarantees in code, not only docs.** The zero-include-cycle and shim-link guarantees were invisible blind. A CI-run, code-adjacent check (a generated `layering.txt` or a compile-only shim-link gate visible in `cmake/`) keeps the guarantee legible even to a reader without the ADRs.

5. **Preserve the strengths as invariants:** the `*Pure` functional-core convention, the error-string-as-capability-contract idiom, and the reference-vs-pointer ISP encoding are the three reasons this codebase reads well blind. Make them stated defaults for every new subsystem.

**Closing note:** The comment-stripping test is largely *passed*. Smatchet's architecture is carried by types, interfaces, and naming conventions to a degree most C++ codebases are not — the Tracker, Commands, Sync, Persistence, and render layers are safely legible with zero comments. The exception, `AppController`, is exactly the pre-existing architectural debt, and its 56%-comment header is the clearest possible signal that it has outgrown what C++14 types can express. Fixing the god-object and fixing the comment-dependence are, satisfyingly, the same task.
