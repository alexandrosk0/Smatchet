<!-- index-summary: registry of per-subsystem agent docs (rules / glossary / orientation) below Source/, plus where rules live for code without a leaf -->
# CONTEXT-MAP — per-subsystem agent-doc registry

Heavy subsystems below `Source/` (a `Source/Core/src/<ctx>/` context, or a plugin directory such as `Source/Plugins/Mcp/`) carry up to **three** co-located agent docs, each with its own format contract:

| Artifact | Holds | Format contract | Loaded how |
|---|---|---|---|
| `AGENTS.md` | imperative scoped rules / invariants | [agents.md spec](https://agents.md/) | harness auto-loads (nearest-wins where supported; see below) |
| `CONTEXT.md` | domain glossary (one-sentence term defs, relationships, zero impl detail) | [`grill-with-docs/CONTEXT-FORMAT.md`](agent-layer/agents/_shared/skills/grill-with-docs/CONTEXT-FORMAT.md) | read on demand |
| `README.md` | orientation (request flow, role-of-each-file; durable-by-construction — no `file:line`, no counts) | this registry's orientation shape | read on demand |

This file is the **registry** (what exists where) and the **harness-discovery index** (for any harness that doesn't honour nearest-wins). The gate `agent-layer/agents/scripts/project/test-subsystem-docs.sh` keys off the table below: a listed leaf missing on disk **FAILs**; a tracked leaf `AGENTS.md` below `Source/` (outside `ThirdParty/` and `build/`) absent from this table **FAILs**; a rule string duplicated central↔leaf **FAILs**; a context whose code changed without a `README.md` touch **WARNs**.

## Registry

| Subsystem | Rules | Glossary | Orientation |
|---|---|---|---|
| Tracker | `Source/Core/src/Tracker/AGENTS.md` | `Source/Core/src/Tracker/CONTEXT.md` | `Source/Core/src/Tracker/README.md` |
| Commands | `Source/Core/src/Commands/AGENTS.md` | — | — |
| Persistence | `Source/Core/src/Persistence/AGENTS.md` | `Source/Core/src/Persistence/CONTEXT.md` | — |
| Sync | `Source/Core/src/Sync/AGENTS.md` | — | — |
| Ui | `Source/Core/src/Ui/AGENTS.md` | — | — |
| Mcp (plugin) | `Source/Plugins/Mcp/AGENTS.md` | — | — |

Path → area routing (which area and doc own a file; the `area:*` Issue labels): [`docs/area-map.json`](docs/area-map.json). Adding or moving a leaf? Update its route's `docs` there too — `coderabbit-triage.py selftest` fails on a missing doc.

**System-wide glossary** (cross-cutting, not subsystem-scoped): [`docs/CONTEXT.md`](docs/CONTEXT.md). Term overlap with `Tracker/CONTEXT.md` (e.g. `TrackerIssueKey`, `UpdateField` set-replace) is intentional — `docs/CONTEXT.md` is the living index hub; the leaf is the subsystem-local view.

A `—` cell means **intentionally absent** (no scoped rule / no grill yet), not missing. `Tracker/` is the full exemplar; `Persistence/` gained its glossary in the `ilocalcache-seam` grill; the rest carry rules only (glossary + orientation deferred to per-subsystem follow-up grills).

## Code without a leaf

Some code sits outside every leaf directory, so no leaf auto-loads when it is edited: the subsystem TUs at the `Source/Core/src/` root and the headers under `Source/Core/include/`. Their rules live in the doc named here; the hottest governing files also carry a one-line pointer comment at the top (`ITrackerBackend.h`, the offline and field-edit delegators, the grid edit pipeline and notifications, each Lua binding source):

| Code | Rules |
|---|---|
| `Source/Core/include/ITrackerBackend.h`, `Source/Core/include/ITracker*.h` | `Source/Core/src/Tracker/AGENTS.md` (Before you edit) |
| `Source/Core/include/<ctx>/` for `Commands`, `Persistence`, `Sync`, `Tracker`, `Ui` | the leaf of the same context, `Source/Core/src/<ctx>/AGENTS.md` |
| `Source/Core/src/AppController_IssueCreateOffline.cpp` | `Source/Core/src/Sync/AGENTS.md` (Before you edit) |
| `AppController_CatalogAndFieldEdit.cpp` (`SubmitFieldEditOrQueue`), `FieldEditPipelineService.cpp`, `PendingActionPolicyPure.cpp`, `ConnectivityMonitorService.cpp`, `AppController_Connectivity.cpp` | `Source/Core/src/Sync/AGENTS.md` § Invariants; the pane a write is bound to: `Source/Core/src/Ui/AGENTS.md` § Offline-first reads (Pillar 6) |
| `AppController_LocalCacheDb.cpp`, `EditMetaCacheService.cpp`, `IssueTransitionsCacheService.cpp`, `ProjectComponentsCacheService.cpp`, `LookupPayloadsPure.cpp`, `AttachmentDiskCache.cpp`, `AttachmentCachePure.cpp` | `Source/Core/src/Persistence/AGENTS.md` § Invariants; how their cached reads render: `Source/Core/src/Ui/AGENTS.md` § Offline-first reads (Pillar 6) |
| `TicketGridModel.cpp`, `TicketGridFilterPure.cpp`, `TicketGridSortPure.cpp`, `TicketGridDurationSortPure.cpp`, `GridLiveContext.cpp`, `GridContextDepsAdapter.cpp`, `GridContextFieldCatalogOps.cpp`, `SmatchetGridFieldEditPipeline.cpp`, `SmatchetGridNotifications.cpp`, `TicketFieldEditor*.cpp`, `DataFreshnessCue.cpp` | `Source/Core/src/Ui/AGENTS.md` § Grid and its Grid file map (Before you edit) |
| `P4Annotate*.cpp`, `CallstackParser.cpp` | the header comments of `P4Annotate.h` and `CallstackParser.h`; the annotate views: `Source/Core/src/Ui/AGENTS.md` § Annotate views and syntax highlight |
| `Source/Core/src/AppController_LuaBindings*.cpp`, `AppController_LuaStubs.cpp`, `LuaAutomationHost.cpp` | [`docs/guides/lua.md`](docs/guides/lua.md) § 5 Binding authors |
| `Source/UnrealPlugins/**` (the Unreal plugin, its `SmatchetImGuiPlugin.Build.cs` and the `SmatchetPackageUnrealLibs_DX12` packaging target) | [`Source/UnrealPlugins/SmatchetImGuiPlugin/README.md`](Source/UnrealPlugins/SmatchetImGuiPlugin/README.md) § Packaging the Native Libraries (staged libs ↔ `Build.cs`); [`BUILD.md`](BUILD.md) § Unreal Packaging |

## Harness discovery

Leaf `AGENTS.md` loading is **per-harness** (see [`agent-layer/docs/harness/SETUP.md`](agent-layer/docs/harness/SETUP.md) § Per-subsystem leaf discovery):

- **Claude Code** reads nested `CLAUDE.md`, not nested `AGENTS.md`. `setup-harness.sh claude-code` generates a gitignored one-line `CLAUDE.md` (`@AGENTS.md`) beside each leaf so Claude Code lazy-loads the leaf when a file in that dir is touched. The committed tree stays `AGENTS.md`-only.
- **Codex / agents.md harnesses** read the nearest `AGENTS.md` natively — no shim needed.
- **Cursor / Aider / generic** — no nearest-wins; use this registry + explicit reads.

## Growth path

Add a leaf when **content exists**, not to fill the grid:

1. A subsystem gains a real scoped invariant → add `<dir>/AGENTS.md` below `Source/` + a row here + the path in its `docs/area-map.json` route (the `Source/**/CLAUDE.md` line in `.gitignore` already covers its Claude Code shim). Leaf creation is driven by content, **not** by the lint strict-zone list (a strict-zone dir need not have a leaf; a leaf need not be strict-zone).
2. A subsystem earns a domain grill → add `CONTEXT.md` (+ `README.md`) and fill its cells.
3. The gate enforces parity automatically once the row is here.
