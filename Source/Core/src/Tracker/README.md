# Tracker — orientation

> **Freshness**: durable-by-construction — describes the *shape* of the subsystem, not exact files/lines, so it survives refactors. If the backend-abstraction shape or the create/update flow below stops matching the code, that's a real drift to fix (the README-staleness gate WARNs when Tracker code changes without a README touch). Rules: [`AGENTS.md`](AGENTS.md). Glossary: [`CONTEXT.md`](CONTEXT.md).

## What this subsystem does

Talks to three issue trackers — Jira, Plane, GitHub — behind one backend-agnostic interface, so the rest of the app (grid, editors, sync) never branches on which tracker is active.

## Backend-abstraction shape

`ITrackerBackend` is the facade. It doesn't do work itself — it hands out five **role interfaces**, each a capability slice:

- `Reader()` → `ITrackerIssueReader` — fetch issues (sync + streamed).
- `Connectivity()` → `ITrackerConnectivity` — reachability probe, tracker-type, project listing.
- `FieldCatalog()` → `ITrackerFieldCatalog*` — field/component/edit-meta fetch (**nullable**).
- `Mutations()` → `ITrackerIssueMutations*` — create/update/attach (**nullable**).
- `Collaboration()` → `ITrackerCollaboration*` — comments/watchers/votes/worklogs (**nullable**).

The three nullable accessors return `nullptr` when a backend doesn't support that capability, so call sites null-check before use. A concrete backend (`JiraClient`, `PlaneClient`, `GitHubClient`) implements all five roles plus `ITrackerBackend` itself. `ITrackerBackendFactory` (default impl: `DefaultTrackerBackendFactory`) picks the concrete class from a case-insensitive type string, falling back to Jira.

## Per-backend divergence points

Everything else is shared; these are the seams where backends genuinely differ:

| Concern | Jira | Plane | GitHub |
|---|---|---|---|
| Issue key | `PROJ-123` | workspace/project UUID | `owner/repo#N` |
| Auth | API token + domain | API key + workspace | PAT + base URL |
| Query language | JQL (native) | Plane filters | GitHub search qualifiers (translated from JQL via `GitHubQueryFromJql`) |
| Field catalog | `createmeta` endpoint | per-project schema endpoint | static native catalog |
| Multi-value field write | set-replace (native) | set-replace (native) | additive natively → reconciled to set-replace via a label diff |

The per-backend JSON↔`CachedTicket` mapping lives in the `*IssueMappingPure` / `*IssueSearchMapping` pure helpers, one per backend, so the mapping is unit-testable without HTTP.

## Create / update request flow

A create or update is one path, whichever tracker is active:

1. The UI assembles an `IssueDraft` (project, issue type, field values, staged attachments).
2. `IssueCreatePipeline::Run` validates required fields, then asks the active backend's `ITrackerIssueMutations` to **build the payload** (`BuildCreatePayload` / `BuildUpdatePayload`) — each backend shapes the JSON its own way.
3. The pipeline POSTs (create) or PUTs (update) through the backend's HTTP calls, then attaches staged files.
4. On success it **seeds** the new/updated `CachedTicket` straight into the local cache so the grid shows the row immediately, and appends a `BackendAuditTrail` begin/result pair.

A single-field edit takes the short path: `ITrackerIssueMutations::UpdateField(key, field, values)` with **set-replace** semantics — `values` is the full intended set, not a delta.

## Field flow

Reads and writes both move through three stages, never skipping one: **catalog → parser → payload**.

- **Catalog**: `ITrackerFieldCatalog` fetches a project's fields; results cache in `FieldCatalogCache` keyed by backend+project, with pure shaping in `TrackerFieldCatalogPure`.
- **Parser**: `TrackerFieldValueParser` turns backend JSON values into display/edit form (comments, changelog, users, option labels).
- **Payload**: `TrackerFieldPayload` / `TrackerFieldPayloadPure` build the backend-shaped write JSON from raw string values, keyed off the field's `TrackerFieldFamily`.

## HTTP layer

Backends never call `cpr::` directly — they go through `TrackerHttpClient` / `TrackerHttpUtils`, which add retry-on-transient, error classification (`TrackerError`), and `NetworkUsageTracker` logging. New backend HTTP must use this layer.

## Fixture vs live

`GitHubFixtureBackend` and `PlaneFixtureBackend` load canned JSON from disk and route it through the *same* mapping helpers as the live clients, so a fixture exercises the real parse/map path without a network. Writes on a fixture are logged no-ops; reachability always reports authenticated. Tests and offline demos use the fixture factory variant; production uses the live factory.

## Cross-subsystem seams

- **Sync** owns `OfflineQueueService` — every Tracker write enqueues there (`PendingCreate` / `PendingFieldEditRecord`) so an offline edit replays through the same pipelines.
- **Persistence** owns `CachedTicket` (the cache row Tracker reads/produces) and `BackendAuditTrail` / `FieldEditAuditSource` (the audit log Tracker writes append to).
- **Ui** is strictly above Tracker in the layer DAG: Tracker code never includes `Ui/` headers (lint rule `no-ui-include-in-domain`). The Tracker-owned cell editors (Labels, DateTime) draw with ImGui directly; shared editor helpers they need (e.g. the touch/click open-gesture gate `TouchCellEditGesture.h`) live as layer-neutral leaf headers at the `Source/Core/include/` root, not under `Ui/`.
